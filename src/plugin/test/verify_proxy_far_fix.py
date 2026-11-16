# 核对 proxy_far_fix 的三份日志：主机时间线 + 两个加入方各自的代理纠正过程。
# 用法：py -I verify_proxy_far_fix.py <日志目录>
# 输出每条判据的 PASS/FAIL，最后一行是 SCENARIO VERIFY RESULT PASS/FAIL。
# 判定口径见 提示词/交接_20261117_场景测试_远铸代理位置纠正.md 3.4。
import math
from pathlib import Path
import re
import sys

HOST = "test1_host.log"
JOINS = ("test2_join.log", "test3_join.log")
NAMES = (HOST,) + JOINS

TS = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")
NUM = r"-?\d+(?:\.\d+)?"


def stamp(line, day):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    t = h * 3600 + mi * 60 + s + ms / 1000.0
    if t + day < 0:
        t += 86400.0
    return t + day


def read_log(path):
    """按时间戳把每行读成 (t, line)；跨零点自动加一天。"""
    out = []
    day = 0.0
    last = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        t = stamp(line, day)
        if t is None:
            out.append((last if last is not None else 0.0, line))
            continue
        if last is not None and t + day < last - 43200.0:
            day += 86400.0
            t += 86400.0
        last = t
        out.append((t, line))
    return out


def fields(row):
    return dict(re.findall(r"(\w+)=([^\s]+)", row))


def hand5(text):
    parts = [int(x) for x in text.split(",")]
    return tuple(parts) if len(parts) == 5 else None


def canon_of_bound(text):
    """[spawn] proxy BOUND/ADOPT 的 hand 就是 readObjectHand 布局 t,c,cs,i,s。"""
    p = [int(x) for x in text.split(",")]
    if len(p) != 5:
        return None
    return (p[0], p[1], p[2], p[3], p[4])


def fmt_hand(h):
    return ",".join(str(x) for x in h)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: verify_proxy_far_fix.py <log directory>")
    directory = Path(sys.argv[1])
    logs = {}
    errors = []
    for name in NAMES:
        path = directory / name
        if not path.is_file():
            errors.append(name + ": missing log")
            logs[name] = []
            continue
        logs[name] = read_log(path)
        results = re.findall(r"SCENARIO RESULT (PASS|FAIL)\b",
                             path.read_text(encoding="utf-8", errors="replace"))
        if results != ["PASS"]:
            errors.append(name + ": expected one RESULT PASS, got " + str(results))
        errs = sum(1 for _, line in logs[name] if "ERROR" in line)
        if errs:
            errors.append(name + ": " + str(errs) + " ERROR line(s)")
        print(name + ": RESULT " + str(results) + " ERROR=" + str(errs))

    # ---- 主机时间线 -----------------------------------------------------
    host = logs[HOST]
    cfg = None
    eff = {}
    spawn = None
    moves = {}
    for t, line in host:
        m = re.search(r"SCENARIO FARFIX CFG censusR=(\d+) attnR=(\d+)", line)
        if m and cfg is None:
            cfg = (float(m.group(1)), float(m.group(2)))
        if "effective cfg" in line and not eff:
            eff = fields(line)
        m = re.search(r"SCENARIO FARFIX SPAWN hand=([\d,]+) pos=([-\d.,]+) "
                      r"parked=(\d)", line)
        if m and spawn is None:
            spawn = (t, m.group(1), m.group(2), m.group(3))
        m = re.search(r"SCENARIO FARFIX MOVE t=\d+ leg=(\w+) pos=([-\d.,]+) "
                      r"parked=(\d)", line)
        if m:
            moves[m.group(1)] = (t, m.group(2), m.group(3))

    checks = []

    def check(name, ok, detail):
        checks.append((name, ok, detail))
        print("SCENARIO VERIFY CHECK " + name + " " +
              ("PASS" if ok else "FAIL") + " " + detail)

    if cfg is None:
        # 场景 CFG 行拿不到（onStart 时 g_lastGw 未就绪等）就用 effective cfg。
        if "censusR" in eff:
            cfg = (float(eff["censusR"]), float(eff.get("attnR", "0")))
            errors.append(HOST + ": missing SCENARIO FARFIX CFG (used effective cfg)")
        else:
            errors.append(HOST + ": missing SCENARIO FARFIX CFG")
            census_r = 2000.0
    if cfg is not None:
        census_r = cfg[0]
        print(HOST + ": censusR=%.0f attnR=%.0f" % cfg)
        if census_r <= 0.0:
            errors.append(HOST + ": censusR is 0 (census disabled)")
    if eff:
        print(HOST + ": effective park=%s walk=%s mintR=%s cellAuth=%s" %
              (eff.get("park", "?"), eff.get("walk", "?"),
               eff.get("mintR", "?"), eff.get("cellAuth", "?")))
    if spawn is None:
        errors.append(HOST + ": missing SCENARIO FARFIX SPAWN")
        host_hand = None
        walk_t = park_t = None
    else:
        host_hand = hand5(spawn[1])
        print(HOST + ": spawn hand=" + spawn[1] + " pos=" + spawn[2] +
              " parked=" + spawn[3])
        if spawn[3] != "1":
            errors.append(HOST + ": spawned NPC was not parked at P0")
    if "walk" not in moves:
        errors.append(HOST + ": missing MOVE leg=walk")
        walk_t = None
    else:
        walk_t = moves["walk"][0]
        print(HOST + ": MOVE walk pos=" + moves["walk"][1] +
              " parked=" + moves["walk"][2])
        if moves["walk"][2] != "1":
            errors.append(HOST + ": P1 move did not park")
    if "park" not in moves:
        errors.append(HOST + ": missing MOVE leg=park")
        park_t = None
    else:
        park_t = moves["park"][0]
        print(HOST + ": MOVE park pos=" + moves["park"][1] +
              " parked=" + moves["park"][2])
        if moves["park"][2] != "1":
            errors.append(HOST + ": P2 move did not park")

    # 主机 KEEP 行里应有目标点不变、偶尔 repark 的痕迹；只做存在性提示。
    keeps = sum(1 for _, line in host if "SCENARIO FARFIX KEEP " in line)
    print(HOST + ": KEEP samples=" + str(keeps))

    if host_hand is None or walk_t is None or park_t is None:
        for name, ok, detail in checks:
            pass
        for e in errors:
            print("SCENARIO VERIFY FAIL " + e)
        print("SCENARIO VERIFY RESULT FAIL")
        return 1

    print(HOST + ": target canon=" + fmt_hand(host_hand))

    # ---- 每个加入方 -----------------------------------------------------
    for name in JOINS:
        rows = logs[name]
        print("-- " + name + " --")
        # 绑定：同一主机手把只能有一次 proxy BOUND/ADOPT。
        bounds = []
        for t, line in rows:
            m = re.search(r"\[spawn\] proxy (?:BOUND|ADOPT) hand=([\d,]+).*?"
                          r"mintDist=(" + NUM + ")", line)
            if not m:
                continue
            if canon_of_bound(m.group(1)) == host_hand:
                bounds.append((t, float(m.group(2))))
        check(name + ".bind", len(bounds) == 1,
              "bound=%d mintDist=%s" % (len(bounds),
                                        [round(b[1]) for b in bounds]))
        if not bounds:
            continue
        bind_t, mint_dist = bounds[0]
        in_ring = (census_r * 1.0 <= mint_dist <= census_r * 1.25)
        check(name + ".mint_ring", in_ring,
              "mintDist=%.0f ring=[%.0f,%.0f]" %
              (mint_dist, census_r, census_r * 1.25))

        # 该手把的逐秒样本。
        samples = []
        for t, line in rows:
            if "SCENARIO FARFIX PROXY " not in line:
                continue
            f = fields(line)
            if "canon" not in f:
                continue
            if hand5(f["canon"]) != host_hand:
                continue
            try:
                samples.append({
                    "t": t,
                    "d": float(f["d"]),
                    "d3": float(f["d3"]),
                    "camd": float(f["camd"]),
                    "inband": int(f["inband"]),
                    "driven": int(f["driven"]),
                    "frozen": int(f["frozen"]),
                    "dest": int(f["dest"]),
                })
            except (KeyError, ValueError):
                continue
        print(name + ": samples=" + str(len(samples)) +
              " bind_t=%.3f" % bind_t)
        if not samples:
            check(name + ".samples", False, "no SCENARIO FARFIX PROXY row")
            continue

        # 判据 1 后半：绑定后到段 1 之前偏差 < 120。
        pre = [s for s in samples if bind_t <= s["t"] < walk_t]
        bad_pre = [s for s in pre if not (0.0 <= s["d"] < 120.0)]
        check(name + ".pre_walk_low", bool(pre) and not bad_pre,
              "samples=%d bad=%d" % (len(pre), len(bad_pre)))

        # 判据 2：段 1~4 全部 driven=0，camd > censusR（wide pass 够不着）。
        seg = [s for s in samples if s["t"] >= walk_t]
        driven_bad = [s for s in seg if s["driven"] != 0]
        cam_bad = [s for s in seg if not (s["camd"] > census_r)]
        cam_vals = [s["camd"] for s in seg] or [0.0]
        check(name + ".not_driven", not driven_bad,
              "rows=%d driven=1 rows=%d" % (len(seg), len(driven_bad)))
        check(name + ".in_ring_after", not cam_bad,
              "rows=%d camd<=%d rows=%d camd_min=%.0f camd_max=%.0f" %
              (len(seg), int(census_r), len(cam_bad),
               min(cam_vals), max(cam_vals)))

        # [drive] 行（DEBUG_DRIVE_TRAIL=1 才有）不能出现这个手把。
        drive_hits = 0
        for _, line in rows:
            m = re.search(r"\[drive\] proxy hand=([\d,]+)", line)
            if m and hand5(m.group(1)) == host_hand:
                drive_hits += 1
        check(name + ".no_drive_line", drive_hits == 0,
              "[drive] proxy rows=" + str(drive_hits))

        # 判据 3：段 1 开始后 30 秒内偏差 < 120；有走路证据、没有 park。
        # first_low 必须取“偏差先涨上去、再降下来”的第一次：段 1 开始的那一秒
        # 里 census 还没到，样本还是 d=0，直接取会让后面涨到 250 的样本算成
        # 抖动，判据 4 就误判了。
        rise = None
        for s in samples:
            if s["t"] >= walk_t and s["d"] >= 120.0:
                rise = s
                break
        base_t = rise["t"] if rise is not None else walk_t
        first_low = None
        for s in samples:
            if s["t"] >= base_t and 0.0 <= s["d"] < 120.0:
                first_low = s
                break
        low_secs = None if first_low is None else first_low["t"] - walk_t
        check(name + ".walk_low_30s", low_secs is not None and low_secs <= 30.0,
              "seconds_to_<120=" +
              ("none" if low_secs is None else "%.1f" % low_secs))

        def census_rows(tag, start, end):
            hits = []
            for t, line in rows:
                if not (start <= t < end):
                    continue
                m = re.search(r"\[census\] " + tag + r" hand=(\d+),(\d+)", line)
                if m and (int(m.group(1)), int(m.group(2))) == \
                        (host_hand[3], host_hand[4]):
                    hits.append(t)
            return hits

        walks = census_rows("walk", walk_t, park_t)
        parks = census_rows("park", walk_t, park_t)
        # [census] walk 的日志被全局 1 Hz 限流（walkLogTick 是函数级 static，
        # 全镇所有走路纠正共用），本手把那次走路很可能被别人的行挤掉。dest=1
        # 是同一个走路分支留下的 haveDest，没被限流，作为等价证据。
        dest_seen = [s for s in samples if s["t"] >= walk_t and s["dest"] == 1]
        check(name + ".walk_logged", bool(walks) or bool(dest_seen),
              "[census] walk rows=%d walk-dest samples=%d" %
              (len(walks), len(dest_seen)))
        check(name + ".no_park_in_seg1", not parks,
              "[census] park rows=" + str(len(parks)))
        # 传送签名：段 1 里相邻样本不该出现一次几百单位的落差（走路是一秒几十）。
        max_drop = 0.0
        prev = None
        for s in samples:
            if not (walk_t <= s["t"] < park_t):
                continue
            if prev is not None:
                max_drop = max(max_drop, prev - s["d"])
            prev = s["d"]
        check(name + ".no_teleport_in_seg1", max_drop <= 150.0,
              "max_one_sample_drop=%.1f" % max_drop)

        # 判据 4：首次 <120 之后到段 3 之前保持 <120（允许 1 个抖动样本），
        # 且 dest 回到 0。
        seg1 = [s for s in samples if walk_t <= s["t"] < park_t]
        if first_low is None:
            check(name + ".hold_low", False, "no sample below 120 in segment 1")
            check(name + ".dest_cleared", False, "no convergence sample")
        else:
            tail = [s for s in seg1 if s["t"] >= first_low["t"]]
            over = [s for s in tail if s["d"] >= 120.0]
            last = tail[-1] if tail else None
            ok = (last is not None and last["d"] < 120.0 and len(over) <= 1)
            check(name + ".hold_low", ok,
                  "rows=%d over_120=%d last_d=%s" %
                  (len(tail), len(over),
                   "none" if last is None else "%.1f" % last["d"]))
            check(name + ".dest_cleared",
                  last is not None and last["dest"] == 0,
                  "last_dest=" + ("none" if last is None else str(last["dest"])))

        # 判据 5：段 2 内 frozen 1 -> 0，且发生在首次 <120 之后约 20 秒。
        release = None
        saw_frozen = False
        for s in seg1:
            if s["frozen"]:
                saw_frozen = True
            elif saw_frozen and first_low is not None and \
                    s["t"] >= first_low["t"]:
                release = s
                break
        release_secs = None
        if release is not None and first_low is not None:
            release_secs = release["t"] - first_low["t"]
        check(name + ".unfreeze", release is not None and
              release_secs is not None and 12.0 <= release_secs <= 32.0,
              "frozen_seen=%d release=%s seconds_after_<120=%s" %
              (1 if saw_frozen else 0,
               "yes" if release is not None else "no",
               "none" if release_secs is None else "%.1f" % release_secs))

        # 判据 6：段 3 开始后 10 秒内有 park 且偏差 < 120。
        parks3 = census_rows("park", park_t, park_t + 10.0)
        pr = None
        for s in samples:
            if s["t"] >= park_t and s["d"] >= 120.0:
                pr = s
                break
        low3 = None
        for s in samples:
            if s["t"] >= (pr["t"] if pr is not None else park_t) and \
                    0.0 <= s["d"] < 120.0:
                low3 = s
                break
        low3_secs = None if low3 is None else low3["t"] - park_t
        check(name + ".park_low_10s",
              bool(parks3) and low3_secs is not None and low3_secs <= 10.0,
              "park_rows=%d seconds_to_<120=%s" %
              (len(parks3),
               "none" if low3_secs is None else "%.1f" % low3_secs))

        # 判据 7：收尾时偏差 < 120、frozen=0。
        last = samples[-1]
        check(name + ".final", 0.0 <= last["d"] < 120.0 and last["frozen"] == 0,
              "d=%.1f frozen=%d at t=%.1f" %
              (last["d"], last["frozen"], last["t"]))

    for name, ok, detail in checks:
        if not ok:
            errors.append(name + ": " + detail)
    for e in errors:
        print("SCENARIO VERIFY FAIL " + e)
    print("SCENARIO VERIFY RESULT " + ("FAIL" if errors else "PASS"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
