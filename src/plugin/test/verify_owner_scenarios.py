# 核对三方场景日志；只有本机判定和跨机证据全部通过才返回成功。
# 用法：py -I verify_owner_scenarios.py <场景名> <日志目录>
import math
from pathlib import Path
import re
import sys

scenario, directory = sys.argv[1:]
if scenario not in ("recruit_owner", "offline_lock", "auth_isolated", "auth_single"):
    sys.exit("Unknown scenario: " + scenario)
names = ("test1_host.log", "test2_join.log", "test3_join.log")
if scenario == "offline_lock":
    names = ("test1_host.log", "test2_join.log", "test3_offline.log", "test3_back.log")
logs = {}
errors = []
for name in names:
    path = Path(directory) / name
    if not path.is_file():
        errors.append(name + ": missing log")
        logs[name] = ""
        continue
    logs[name] = path.read_text(encoding="utf-8", errors="replace")
    results = re.findall(r"SCENARIO RESULT (PASS|FAIL)\b", logs[name])
    if results != ["PASS"]:
        errors.append(name + ": expected one RESULT PASS, got " + str(results))

if scenario == "recruit_owner":
    for name, log in logs.items():
        if re.search(r"\[owners\] unknown tab=.*?-> host fallback", log):
            errors.append(name + ": ownership fell back to host")
        actions = re.findall(r"SCENARIO RECRUITACT .*?leg=(\d+) res=(-?\d+)", log)
        if len(actions) != 1 or actions[0][1] != "1":
            errors.append(name + ": missing successful recruit action")
        print(name + ": recruit legs=" + str(actions))

if scenario == "offline_lock":
    views = {}
    for name, log in logs.items():
        stages = {"base": {}, "back": {}}
        for row in re.findall(r"SCENARIO OFFLINEBODY ([^\r\n]+)", log):
            fields = dict(re.findall(r"(\w+)=([^\s]+)", row))
            key = (fields["idx"], fields["ser"])
            stages[fields["stage"]][key] = fields
        views[name] = stages
    baseline = views["test3_offline.log"]["base"]
    if not baseline:
        errors.append("missing pre-disconnect body baseline")
    for name, stages in views.items():
        for stage, bodies in stages.items():
            if name == "test3_offline.log" and stage == "back":
                continue
            if set(bodies) != set(baseline):
                errors.append(name + ": " + stage + " body identities differ from pre-disconnect baseline")
            for key in set(bodies) & set(baseline):
                if bodies[key]["tab"] != baseline[key]["tab"]:
                    errors.append(name + ": " + stage + " body tab changed: " + str(key))
    # 好友2 离线期间主机和好友1 向它申请编队：本地状态应为 0，日志里不能有 SEND kind=1 或 TIMEOUT
    for name in names[:2]:
        log = logs[name]
        requests = re.findall(r"SCENARIO OFFLINEREQ .*?target=(\d+) req=(-?\d+)", log)
        if len(requests) != 1 or requests[0][0] == "0" or requests[0][1] != "0":
            errors.append(name + ": offline authorization request was not rejected locally")
        checks = re.findall(r"SCENARIO OFFLINEREQCHK .*?stage=(\w+) req=(-?\d+) ok=(\d)", log)
        if checks != [("next", "0", "1"), ("timeout", "0", "1")]:
            errors.append(name + ": offline authorization state was not clear")
        if re.search(r"\[auth\] SEND kind=1\b", log):
            errors.append(name + ": authorization request was sent to the offline player")
        if re.search(r"\[auth\] TIMEOUT", log):
            errors.append(name + ": authorization request to the offline player timed out")
        print(name + ": offline authorization checks=" + str(checks))
    back = re.findall(r"SCENARIO OFFLINEBACKFINAL .*?pend=(-?\d+) grant=(-?\d+) borrow=(-?\d+) "
                      r"req=(-?\d+) ok=(\d)", logs["test3_back.log"])
    if back != [("0", "0", "0", "0", "1")]:
        errors.append("reconnected player did not clear authorization state")
    moves = re.findall(r"SCENARIO OFFLINEBACKMOVE ([^\r\n]+)", logs["test3_back.log"])
    if len(moves) != 1:
        errors.append("expected one reconnect movement challenge")
    else:
        move = dict(re.findall(r"(\w+)=([^\s]+)", moves[0]))
        key = (move["idx"], move["ser"])
        if move["moved"] != "1" or key not in baseline:
            errors.append("reconnect movement challenge failed or used an unknown body")
        origin = tuple(map(float, move["from"].split(",")))
        target_x = float(move["to"])
        source = views["test3_back.log"]["back"].get(key)
        if not source:
            errors.append("missing authoritative position after reconnect movement")
        else:
            position = tuple(map(float, source["pos"].split(",")))
            if (not all(map(math.isfinite, origin + position + (target_x,)))
                    or abs(target_x - origin[0]) < 3.0
                    or abs(position[0] - origin[0]) < 2.0
                    or abs(position[0] - target_x) > 1.5):
                errors.append("reconnected player did not reach the movement challenge target")
            for name in names[:2]:
                received = views[name]["back"].get(key)
                if not received:
                    errors.append(name + ": missing reconnect movement observation")
                    continue
                observed = tuple(map(float, received["pos"].split(",")))
                if not all(map(math.isfinite, observed)) or math.dist(observed, position) > 1.5:
                    errors.append(name + ": fresh reconnect movement was not synchronized")
                print(name + ": reconnect body=" + str(key) + " pos=" + received["pos"])

if scenario == "auth_isolated":
    samples = re.findall(r"SCENARIO AUTHISOLATED rank=2 slot=(\d+) ok=(\d)", logs[names[2]])
    slots = [int(slot) for slot, _ in samples]
    starts = re.findall(r"SCENARIO AUTHSTART who=join rank=2 firstSlot=(\d+)", logs[names[2]])
    if (len(slots) < 2 or any(ok != "1" for _, ok in samples)
            or any(b != a + 1 for a, b in zip(slots, slots[1:]))):
        errors.append("third-party isolation samples failed or skipped clock slots")
    if len(starts) != 1 or not set(range(int(starts[0]), int(starts[0]) + 22)).issubset(slots):
        errors.append("third-party isolation samples do not cover the complete authorization timeline")
    for name, log in logs.items():
        phases = re.findall(r"SCENARIO AUTHCHK .*?phase=(\d+) .*?ok=(\d)", log)
        if phases != [(str(phase), "1") for phase in range(1, 7)]:
            errors.append(name + ": missing successful authorization phase checks")
    controls = re.findall(r"SCENARIO AUTHCONTROL who=host phase=(\d+) .*?classBad=(\d+) ok=(\d)", logs[names[0]])
    if controls != [(str(phase), "0", "1") for phase in range(1, 7)]:
        errors.append("host command permissions were not verified for all authorization phases")
    print("third-party isolation samples=" + str(len(samples)))

if scenario == "auth_single":
    samples = re.findall(r"SCENARIO AUTHISOLATED rank=2 slot=(\d+) ok=(\d)", logs[names[2]])
    slots = [int(slot) for slot, _ in samples]
    starts = re.findall(r"SCENARIO AUTHSINGLESTART who=join rank=2 firstSlot=(\d+)", logs[names[2]])
    if (len(slots) < 2 or any(ok != "1" for _, ok in samples)
            or any(b != a + 1 for a, b in zip(slots, slots[1:]))):
        errors.append("third-party isolation samples failed or skipped clock slots")
    if len(starts) != 1 or not set(range(int(starts[0]), int(starts[0]) + 12)).issubset(slots):
        errors.append("third-party isolation samples do not cover the complete authorization timeline")
    # 三方必须挑到同一对出借方编队 X、Y
    bases = {}
    for name, log in logs.items():
        found = re.findall(r"SCENARIO AUTHSINGLEBASE .*?x=(\d+,\d+) y=(\d+,\d+) bowner=(\d+) ok=(\d)", log)
        if len(found) != 1 or found[0][3] != "1":
            errors.append(name + ": missing lender squad baseline")
        else:
            bases[name] = found[0][:3]
    if len(bases) != 3 or len(set(bases.values())) != 1:
        errors.append("lender squad selection differs between clients")
    for name in names:
        checks = re.findall(r"SCENARIO AUTHSINGLECHK .*?phase=(\d+) .*?xBad=(\d+) .*?yBad=(\d+) "
                            r"ok=(\d)", logs[name])
        if checks != [(str(phase), "0", "0", "1") for phase in (1, 2, 5, 6)]:
            errors.append(name + ": authorization control checks failed")
    print("third-party isolation samples=" + str(len(samples)))

for error in errors:
    print("SCENARIO VERIFY FAIL " + error)
print("SCENARIO VERIFY RESULT " + ("FAIL" if errors else "PASS"))
sys.exit(1 if errors else 0)
