// MultiplayerStartGen - generates dist/mods/KenshiCoop/KenshiCoop.mod, the FCS data
// half of the KenshiCoop mod. It carries TWO co-op game starts:
//
//   "Multiplayer (Wanderer x2)"   vanilla Wanderer start, two wanderers, one squad each.
//   "Multiplayer+ (Wanderer x2)"  the same shape with money = 500000. The KenshiCoop
//                                 plugin recognises this start by its squad-2 leader
//                                 template and floors every stat on the player squad
//                                 at 50 (FCS cannot express per-skill values; a
//                                 Character record only carries the grouped
//                                 "combat stats"/"ranged stats"/... fields).
//
// The two names are a matched pair on purpose: same start, with and without the
// boost. The "+" variant shipped as "Wanderer+ x2" in v0.50 and was renamed for
// v0.51 - a DISPLAY name only, so no save is affected (see STRING IDs below).
//
// DESIGN RULE (inherited from the original start by zeroit789, PR #15): stay
// vanilla-equivalent. Squad 1 always reuses the untouched vanilla Wanderer
// SquadTemplate; each squad 2 gets a NEW template whose leader is an exact clone of
// the vanilla Wanderer Character. No faction, AI or combat data is altered.
//
// STRING IDs ARE FROZEN. Kenshi saves store the StringId of the template a character
// came from, so renaming a record orphans it in every existing save. Records 1-3 were
// first published under the old "KenshiCoop-MultiplayerStart" mod name and keep that
// suffix forever, even though the file now ships as KenshiCoop.mod. New records join
// the same namespace rather than starting a new one.
//
// REGENERATE:
//   Requires a .NET 9 SDK (OpenConstructionSet 4.1.0 targets net9.0). If `dotnet` is
//   not on PATH, install per-user:
//     powershell -NoProfile -ExecutionPolicy Bypass -Command "& $env:TEMP\dotnet-install.ps1 -Channel 9.0 -InstallDir \"$env:LOCALAPPDATA\Microsoft\dotnet\""
//
//   dotnet run                       # read the game data, write the .mod, verify it
//   dotnet run -- dump               # dump the .mod already on disk (no write)
//   dotnet run -- "D:\...\Kenshi"    # same as the first, with a non-default install
//
// Then deploy it into the game: scripts\deploy.cmd
//
// 本分支（zh-cn-improvements）新增：其余原版开局各生成一个“联机：<中文名>”版本（记录 7 起），
// 资金、出生地、阵营关系、科技、限定种族、角色模板全部沿用原版，另加一个相同配置的第 2 小队给加入方；
// 开局界面文字取自游戏 locale\zh_CN\gamedata.po 直接写成中文，原有两个流浪者开局也改为中文显示名。
// 2026-11-13 起开局记录（3、6 和 7 起每组的 +0）不再写进 .mod，只保留角色和小队模板，编号仍冻结。
// 本机 Kenshi 不在默认路径时：dotnet run -- "G:\SteamLibrary\steamapps\common\Kenshi"
using OpenConstructionSet.Data;
using OpenConstructionSet.Mods;

// ---- Identity constants -----------------------------------------------------
// The file we ship. Deliberately NOT the record namespace below.
const string OutModName = "KenshiCoop";
// The record namespace, frozen at the name the records were first published under.
const string RecordNamespace = "KenshiCoop-MultiplayerStart";

// Vanilla StringIds (verified against gamedata.base / rebirth.mod).
const string VanillaStartId = "1980-gamedata.base";    // NewGameStartoff "Wanderer"
const string VanillaSquadId = "45550-gamedata.base";   // SquadTemplate "startoff- Wanderer squad"
const string VanillaCharId = "1533662-rebirth.mod";    // Character "Wanderer"
const string HubTownId = "18919-Newwworld.mod";        // Town "The Hub" (vanilla Wanderer spawn)

// Money for the "Multiplayer+ (Wanderer x2)" start. Kenshi has ONE player faction
// wallet and KenshiCoop replicates it as a single shared pool, so this is 500k for
// the pair.
const int PlusStartMoney = 500000;

string Sid(int id) => $"{id}-{RecordNamespace}.mod";

// ---- Paths ------------------------------------------------------------------
string repoRoot = FindRepoRoot();
string outDir = Path.Combine(repoRoot, "dist", "mods", OutModName);
string outPath = Path.Combine(outDir, OutModName + ".mod");

string[] argList = args;
bool dumpMode = argList.Any(a => a.Equals("dump", StringComparison.OrdinalIgnoreCase));
string? kenshiDirArg = argList.FirstOrDefault(a => !a.Equals("dump", StringComparison.OrdinalIgnoreCase));

// ---- Dump mode: inspect the .mod already on disk ----------------------------
if (dumpMode)
{
    if (!File.Exists(outPath)) { Console.WriteLine($"Not found: {outPath} - generate it first."); return 1; }
    DumpAll(await new ModFile(outPath).ReadDataAsync(), outPath);
    return 0;
}

string kenshiDir = ResolveKenshiDir(kenshiDirArg);
string dataDir = Path.Combine(kenshiDir, "data");
Console.WriteLine($"Kenshi install: {kenshiDir}");

// ---- 1) Read the base game data ---------------------------------------------
Console.WriteLine("Reading Kenshi base data...");
var baseData = await new ModFile(Path.Combine(dataDir, "gamedata.base")).ReadDataAsync();
var rebirth = await new ModFile(Path.Combine(dataDir, "rebirth.mod")).ReadDataAsync();
Console.WriteLine($"  gamedata.base: {baseData.Items.Count} items | rebirth.mod: {rebirth.Items.Count} items");

// 原版各开局的联机版需要四个基础文件按加载顺序叠加后的有效数据
var baseLayers = new List<ModFileData> { baseData };
foreach (var f in new[] { "Newwworld.mod", "Dialogue.mod" })
    baseLayers.Add(await new ModFile(Path.Combine(dataDir, f)).ReadDataAsync());
baseLayers.Add(rebirth);

// 开局界面文字的简体中文：新记录不在游戏翻译表里（翻译按 StringId 匹配），生成时直接写入中文。
// 取自游戏自带的 locale\zh_CN\gamedata.po（NEW_GAME_STARTOFF 条目，英文原文 -> 译文）
var zhStart = new Dictionary<string, string>();
string poPath = Path.Combine(kenshiDir, "locale", "zh_CN", "gamedata.po");
if (File.Exists(poPath))
{
    static string PoText(string block, string key)
    {
        var m = System.Text.RegularExpressions.Regex.Match(block, "\\n" + key + " ((?:\".*\"\\n?)+)");
        if (!m.Success) return "";
        var parts = System.Text.RegularExpressions.Regex.Matches(m.Groups[1].Value, "\"(.*)\"").Select(x => x.Groups[1].Value);
        return System.Text.RegularExpressions.Regex.Unescape(string.Concat(parts));
    }
    foreach (var block in File.ReadAllText(poPath).Replace("\r\n", "\n").Split("\n\n"))
    {
        if (!block.Contains("msgctxt \"NEW_GAME_STARTOFF,")) continue;
        string en = PoText(block, "msgid"), zh = PoText(block, "msgstr");
        if (en.Length > 0 && zh.Length > 0) zhStart[en] = zh;
    }
}
Console.WriteLine($"  zh_CN start texts: {zhStart.Count}");
string Zh(string en) => zhStart.TryGetValue(en, out var zh) ? zh : en;

// 联机开局说明的统一结尾
const string CoopNote = "\n\n【联机开局】主机控制第 1 个小队，加入方控制第 2 个小队，两队角色配置相同；资金为双方共用。";

// 某记录在四个基础文件叠加后的有效数据：值逐层覆盖；引用按类别、按目标逐层覆盖，新目标追加在后。
// 返回最后一层的记录（类型、显示名）和合并后的引用；基础文件里没有删除引用的标记，不处理删除
(Item? Item, List<ReferenceCategory> Refs) Effective(string sid)
{
    Item? last = null;
    var refs = new List<ReferenceCategory>();
    foreach (var layer in baseLayers)
        foreach (var it in layer.Items.Where(i => i.StringId.Equals(sid, StringComparison.OrdinalIgnoreCase)))
        {
            last = it;
            foreach (var cat in it.ReferenceCategories)
            {
                var merged = refs.FirstOrDefault(c => c.Name == cat.Name);
                if (merged is null) refs.Add(merged = new ReferenceCategory(cat.Name));
                foreach (var r in cat.References)
                {
                    int at = merged.References.FindIndex(x => x.TargetId == r.TargetId);
                    if (at >= 0) merged.References[at] = new Reference(r);
                    else merged.References.Add(new Reference(r));
                }
            }
        }
    return (last, refs);
}

Dictionary<string, object> MergedValues(string sid)
{
    var values = new Dictionary<string, object>();
    foreach (var layer in baseLayers)
        foreach (var it in layer.Items.Where(i => i.StringId.Equals(sid, StringComparison.OrdinalIgnoreCase)))
            foreach (var kv in it.Values) values[kv.Key] = kv.Value;
    return values;
}

// Lookup by StringId within one file. Duplicates: last wins, as the game does.
static Item? Find(IEnumerable<Item> items, string stringId) =>
    items.LastOrDefault(i => i.StringId.Equals(stringId, StringComparison.OrdinalIgnoreCase));

var startBase = Find(baseData.Items, VanillaStartId);
var startOver = Find(rebirth.Items, VanillaStartId);
var squadBase = Find(baseData.Items, VanillaSquadId);
var squadOver = Find(rebirth.Items, VanillaSquadId);
var wandChar = Find(rebirth.Items, VanillaCharId);

if (startBase is null || startOver is null || squadBase is null || squadOver is null || wandChar is null)
{
    Console.WriteLine("ERROR: the expected vanilla records were not found:");
    Console.WriteLine($"  startBase={startBase is not null} startOver={startOver is not null} " +
                      $"squadBase={squadBase is not null} squadOver={squadOver is not null} char={wandChar is not null}");
    return 1;
}
Console.WriteLine($"  Vanilla OK: start \"{startOver.Name}\", squad \"{squadBase.Name}\", char \"{wandChar.Name}\"");

// ---- 2) Effective vanilla values = base record + rebirth override ------------
// Kenshi applies .mod files as deltas, so the values a player actually gets are the
// base record overlaid with the override. Reproduce that merge before cloning.
static Dictionary<string, object> MergeValues(Item baseItem, Item overrideItem)
{
    var merged = new Dictionary<string, object>(baseItem.Values);
    foreach (var kv in overrideItem.Values) merged[kv.Key] = kv.Value;
    return merged;
}
var startValues = MergeValues(startBase, startOver);   // includes rebirth's description
var squadValues = MergeValues(squadBase, squadOver);   // includes "dont multiply" = True

var newSave = new ItemSaveData(1, ItemChangeType.New);

// Clone the vanilla Wanderer Character verbatim (clothing, weapon, dialogue,
// personality, stat fields - everything). Only the name and StringId differ.
Item CloneWanderer(int id, string name) => new(
    ItemType.Character, id, name, Sid(id), newSave,
    new Dictionary<string, object>(wandChar.Values),
    wandChar.ReferenceCategories.Select(c => new ReferenceCategory(c)),
    []);

// Clone the effective vanilla squad template, pointing its leader at `leaderId`.
// v0 = 1 on the leader reference means one unit, matching vanilla.
Item CloneSquad(int id, string name, int leaderId) => new(
    ItemType.SquadTemplate, id, name, Sid(id), newSave,
    new Dictionary<string, object>(squadValues),
    [new ReferenceCategory("leader", [new Reference(Sid(leaderId), 1, 0, 0)])],
    []);

// Build a co-op start: vanilla Wanderer values, our description, and TWO squads.
// Reference ORDER matters - the first squad is the host's (squad tab 1).
Item MakeStart(int id, string name, string description, int squad2Id, int money)
{
    var values = new Dictionary<string, object>(startValues) { ["description"] = description };
    // 难度/风格文字同样不在翻译表里，写入中文
    foreach (var key in new[] { "difficulty", "style" })
        if (values.TryGetValue(key, out var text) && text is string s) values[key] = Zh(s);
    // Preserve the field's stored numeric type rather than forcing int.
    values["money"] = values.TryGetValue("money", out var existing) && existing is not null
        ? Convert.ChangeType(money, existing.GetType())!
        : money;
    return new Item(
        ItemType.NewGameStartoff, id, name, Sid(id), newSave, values,
        [
            new ReferenceCategory("squad",
            [
                new Reference(VanillaSquadId, 0, 0, 0), // squad 1: untouched vanilla template (host)
                new Reference(Sid(squad2Id), 0, 0, 0),  // squad 2: our clone (joining player)
            ]),
            new ReferenceCategory("town", [new Reference(HubTownId, 0, 0, 0)]),
        ],
        []);
}

// ---- 3) The records ----------------------------------------------------------
// 1-3: the original "Multiplayer (Wanderer x2)" start. Reproduced byte-for-byte in
// intent - these ids are referenced by saves already in the wild.
var wanderer2 = CloneWanderer(1, "流浪者 2");
var wandererSquad2 = CloneSquad(2, "startoff- Wanderer squad 2 (co-op)", 1);
// 本分支：显示名与说明改为简体中文（只改显示文字，StringId 不变，不影响已有存档）
var wandererStart = MakeStart(3, "联机：流浪者",
    "两个只有几枚硬币、一条裤子和一把生锈旧剑的流浪者，准备一起闯荡世界。" + CoopNote,
    squad2Id: 2, money: Convert.ToInt32(startValues["money"]));

// 4-6: the "Multiplayer+ (Wanderer x2)" start. Record 4 is a SEPARATE Character from
// record 1 even though both spawn a PC called "Wanderer 2" - the StringId is what tells
// the two starts apart, and record 4's is the MARKER the plugin matches on to apply the
// stat floor. Keep it in sync with WPX2_MARKER_SID in src/plugin/Plugin.cpp.
var plusWanderer = CloneWanderer(4, "流浪者 2");
var plusSquad2 = CloneSquad(5, "startoff- Multiplayer+ squad 2 (co-op)", 4);
var plusStart = MakeStart(6, "联机+：流浪者（50 万资金、全属性 50）",
    "两个身经百战的流浪者，带着一笔小财富一起出发。与“联机：流浪者”相同，但省去前期练级：" +
    "50 万开局资金为双方共用，KenshiCoop 插件会在新游戏开始时把两人的所有属性提升到 50。" + CoopNote,
    squad2Id: 5, money: PlusStartMoney);

// 7 起：其余原版开局的联机版（本分支新增）。每个原版开局占三个编号：+0 开局、+1 主机小队模板、
// +2 加入方小队模板。开局只引用一个小队模板时主机直接用原版模板，+1 空着不用。
// 编号一经发布永久冻结（存档按 StringId 引用），以后只能追加，不能改动已有编号。
// 流浪者已有记录 1-6；The Hungover Ninja（5425）不在官方翻译里、开局列表也不显示，不做联机版
(string VanillaSid, int BaseId)[] coopVariants =
[
    ("1981-gamedata.base", 7),     // The Freedom Seekers
    ("2287-gamedata.base", 10),    // Rock Bottom
    ("1985-gamedata.base", 13),    // Son of a Captain（直接引用角色）
    ("2918-gamedata.base", 16),    // The Cannibal Hunters（直接引用角色）
    ("2060-gamedata.base", 19),    // The Holy Sword（直接引用角色）
    ("1994-gamedata.base", 22),    // The Wandering Trader
    ("3062-gamedata.base", 25),    // Nobodies
    ("47924-rebirth.mod", 28),     // Guy with a dog
    ("1532934-rebirth.mod", 31),   // The Slaves
    ("1533582-rebirth.mod", 34),   // Holy Nation Citizen
    ("1533657-rebirth.mod", 37),   // The Hive Exile
    ("1533659-rebirth.mod", 40),   // Empire Citizen
];
var variantItems = new List<Item>();
foreach (var (vanillaSid, baseId) in coopVariants)
{
    var (start, startRefs) = Effective(vanillaSid);
    var squadRefs = startRefs.FirstOrDefault(c => c.Name == "squad")?.References ?? [];
    if (start is null || start.Type != ItemType.NewGameStartoff || squadRefs.Count == 0)
    {
        Console.WriteLine($"ERROR: vanilla start {vanillaSid} not found or has no squad");
        return 1;
    }
    string hostSquadSid;
    List<ReferenceCategory> joinRefs;
    Dictionary<string, object> joinValues;
    var (firstTarget, firstRefs) = Effective(squadRefs[0].TargetId);
    if (squadRefs.Count == 1 && firstTarget?.Type == ItemType.SquadTemplate)
    {
        // 只引用一个小队模板：主机用原版模板，加入方用其有效数据的副本（角色模板、数量、动物都相同）
        hostSquadSid = squadRefs[0].TargetId;
        joinValues = MergedValues(hostSquadSid);
        joinRefs = firstRefs;
    }
    else
    {
        // 直接引用角色：合成小队模板，首个角色为队长，其余入队。原版单角色开局（如船长之子）的数量写 0
        // 也只生成一个角色，所以数量按至少 1 计。小队参数沿用原版流浪者小队（含 dont multiply）
        var members = new List<Reference>();
        foreach (var r in squadRefs)
        {
            if (Effective(r.TargetId).Item?.Type != ItemType.Character)
            {
                Console.WriteLine($"ERROR: {vanillaSid} mixes squad templates and characters - not supported");
                return 1;
            }
            members.Add(new Reference(r.TargetId, Math.Max(r.Value0, 1), 0, 0));
        }
        joinValues = new Dictionary<string, object>(squadValues);
        joinRefs = [new ReferenceCategory("leader", [new Reference(members[0].TargetId, 1, 0, 0)])];
        if (members.Count > 1) joinRefs.Add(new ReferenceCategory("squad", members.Skip(1)));
        hostSquadSid = Sid(baseId + 1);
        variantItems.Add(new Item(ItemType.SquadTemplate, baseId + 1, $"start- {start.Name} (co-op host)",
            hostSquadSid, newSave, new Dictionary<string, object>(joinValues),
            joinRefs.Select(c => new ReferenceCategory(c)), []));
    }
    variantItems.Add(new Item(ItemType.SquadTemplate, baseId + 2, $"start- {start.Name} (co-op join)",
        Sid(baseId + 2), newSave, joinValues, joinRefs.Select(c => new ReferenceCategory(c)), []));

    // 开局本身：资金、出生地、阵营关系、科技、限定种族等全部沿用原版，只把 squad 换成主机 + 加入方两个小队
    var values = MergedValues(vanillaSid);
    foreach (var key in new[] { "description", "difficulty", "style" })
        if (values.TryGetValue(key, out var text) && text is string s) values[key] = Zh(s);
    values["description"] = (values.TryGetValue("description", out var d) ? d as string : "") + CoopNote;
    var refs = startRefs.Select(c => c.Name != "squad"
        ? new ReferenceCategory(c)
        : new ReferenceCategory("squad",
            [new Reference(hostSquadSid, 0, 0, 0), new Reference(Sid(baseId + 2), 0, 0, 0)]));
    variantItems.Add(new Item(ItemType.NewGameStartoff, baseId, "联机：" + Zh(start.Name), Sid(baseId),
        newSave, values, refs, []));
}

// 2026-11-13：F2 创建角色已能直接用原版开局，“联机：”开局不再写进 .mod；
// 角色和小队模板记录照旧保留（已有存档按 StringId 引用）
Item[] items = [.. new[] { wanderer2, wandererSquad2, wandererStart, plusWanderer, plusSquad2, plusStart }.Concat(variantItems)
    .Where(i => i.Type != ItemType.NewGameStartoff)];

// ---- 4) Assemble and write ---------------------------------------------------
var header = new Header(1, "",
    "KenshiCoop 的数据部分：旧版“联机：”开局用过的角色与小队模板，留给已有存档使用；开局本身已移除，" +
    "联机请直接用原版开局，好友进世界后按 F2 创建角色。纯数据 mod，联机需要 KenshiCoop 插件。")
{
    Dependencies = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"],
};
var info = new ModInfoData
{
    ModName = OutModName + ".mod",
    Title = "KenshiCoop",
    Tags = ["Gameplay"],
};
var modData = new ModFileData(DataFileType.Mod, header, items.Length, items, info);

Directory.CreateDirectory(outDir);
await new ModFile(outPath).WriteDataAsync(modData);
Console.WriteLine($"\nWrote: {outPath} ({new FileInfo(outPath).Length} bytes)");

// The shipped mod folder is just the DLL + RE_Kenshi.json + the .mod (see
// scripts\deploy.cmd). OCS also emits a launcher .info alongside the data file;
// drop it so the folder keeps the layout deploy and the mod kit expect.
foreach (var stray in Directory.GetFiles(outDir, "_*.info"))
{
    File.Delete(stray);
    Console.WriteLine($"Removed generated launcher file: {Path.GetFileName(stray)}");
}

// ---- 5) Verify by re-reading what we just wrote ------------------------------
var verify = await new ModFile(outPath).ReadDataAsync();
DumpAll(verify, outPath);

var errors = new List<string>();
if (verify.Items.Count != items.Length)
    errors.Add($"expected {items.Length} items, found {verify.Items.Count}");

// Every StringId we have ever published must still be present and of the right type.
(string Sid, ItemType Type)[] expected =
[
    (Sid(1), ItemType.Character), (Sid(2), ItemType.SquadTemplate),
    (Sid(4), ItemType.Character), (Sid(5), ItemType.SquadTemplate),
];
foreach (var (sid, type) in expected)
{
    var found = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (found is null) errors.Add($"missing record {sid} (existing saves reference it)");
    else if (found.Type != type) errors.Add($"record {sid} is {found.Type}, expected {type}");
}

// Character clones must carry the full vanilla Wanderer payload.
int wandRefs = wandChar.ReferenceCategories.Sum(c => c.References.Count);
foreach (var sid in new[] { Sid(1), Sid(4) })
{
    var c = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (c is null) continue;
    if (c.Values.Count != wandChar.Values.Count)
        errors.Add($"{sid}: {c.Values.Count} values (vanilla Wanderer has {wandChar.Values.Count})");
    if (c.ReferenceCategories.Sum(x => x.References.Count) != wandRefs)
        errors.Add($"{sid}: {c.ReferenceCategories.Sum(x => x.References.Count)} refs (vanilla has {wandRefs})");
}

// Squad templates must keep the vanilla values and lead with our clone.
foreach (var (sid, leaderSid) in new[] { (Sid(2), Sid(1)), (Sid(5), Sid(4)) })
{
    var s = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (s is null) continue;
    if (s.Values.Count != squadValues.Count)
        errors.Add($"{sid}: {s.Values.Count} values (expected {squadValues.Count})");
    var leader = s.ReferenceCategories.FirstOrDefault(c => c.Name == "leader")?.References.SingleOrDefault();
    if (leader is null || leader.TargetId != leaderSid || leader.Value0 != 1)
        errors.Add($"{sid}: 'leader' does not point at {leaderSid} with v0=1");
    if (!Equals(s.Values.TryGetValue("dont multiply", out var dm) ? dm : null, true))
        errors.Add($"{sid}: missing 'dont multiply'=True (vanilla effective value)");
}

// Both starts: two squads in the right order, the vanilla town, vanilla game values.
foreach (var (sid, squad2Sid, money) in new[]
         {
             (Sid(3), Sid(2), Convert.ToInt32(startValues["money"])),
             (Sid(6), Sid(5), PlusStartMoney),
         })
{
    var st = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (st is null) continue;
    var squads = st.ReferenceCategories.FirstOrDefault(c => c.Name == "squad")?.References ?? [];
    if (squads.Count != 2) errors.Add($"{sid}: {squads.Count} 'squad' refs (expected 2)");
    else
    {
        if (squads[0].TargetId != VanillaSquadId) errors.Add($"{sid}: squad 1 is not the vanilla host template");
        if (squads[1].TargetId != squad2Sid) errors.Add($"{sid}: squad 2 is not {squad2Sid}");
    }
    var town = st.ReferenceCategories.FirstOrDefault(c => c.Name == "town")?.References.SingleOrDefault();
    if (town is null || town.TargetId != HubTownId) errors.Add($"{sid}: 'town' is not vanilla The Hub");
    if (Convert.ToInt32(st.Values.TryGetValue("money", out var m) ? m : 0) != money)
        errors.Add($"{sid}: money is {(m ?? "null")}, expected {money}");
    // Everything except money and description stays exactly as the vanilla Wanderer start.
    foreach (var key in new[] { "difficulty", "start pos X", "start pos Z", "style", "force start pos" })
        if (!Equals(st.Values.TryGetValue(key, out var v) ? v : null,
                    key is "difficulty" or "style" ? Zh((string)startBase.Values[key]) : startBase.Values[key]))
            errors.Add($"{sid}: value '{key}' differs from the vanilla Wanderer start");
}

// “联机：”开局已不再发布，文件里不应再有开局记录
if (verify.Items.Any(i => i.Type == ItemType.NewGameStartoff))
    errors.Add("the .mod still carries a game start");

Console.WriteLine();
if (errors.Count > 0)
{
    Console.WriteLine("VERIFICATION FAILED:");
    foreach (var e in errors) Console.WriteLine($"  - {e}");
    return 1;
}
Console.WriteLine($"VERIFICATION OK: {items.Length} records, no game starts, legacy StringIds intact.");
return 0;

// ---- Helpers -----------------------------------------------------------------

// The repo root is the nearest ancestor of the build output that holds dist/mods/<mod>.
static string FindRepoRoot()
{
    for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        if (Directory.Exists(Path.Combine(dir.FullName, "dist", "mods", "KenshiCoop")))
            return dir.FullName;
    throw new DirectoryNotFoundException(
        $"could not find the repo root (no dist/mods/KenshiCoop above {AppContext.BaseDirectory})");
}

// The install holding gamedata.base. Same default the scripts/ drivers use.
static string ResolveKenshiDir(string? explicitDir)
{
    string[] candidates = explicitDir is not null
        ? [explicitDir]
        :
        [
            @"C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
            @"C:\Program Files\Steam\steamapps\common\Kenshi",
        ];
    foreach (var c in candidates)
        if (File.Exists(Path.Combine(c, "data", "gamedata.base")))
            return c;
    throw new DirectoryNotFoundException(
        "could not find a Kenshi install with data\\gamedata.base. Tried: " + string.Join("; ", candidates) +
        ". Pass the install path as an argument: dotnet run -- \"D:\\path\\to\\Kenshi\"");
}

// Full dump of a .mod, for eyeballing a generated file.
static void DumpAll(ModFileData d, string path)
{
    Console.WriteLine($"\n=== DUMP {Path.GetFileName(path)}: {d.Items.Count} items, lastId={d.LastId} ===");
    Console.WriteLine($"HEADER deps=[{string.Join("; ", d.Header.Dependencies)}]");
    foreach (var it in d.Items)
    {
        Console.WriteLine($"\n[{it.Type}] \"{it.Name}\" ({it.StringId}) save={it.SaveData}");
        foreach (var kv in it.Values.OrderBy(k => k.Key))
        {
            var s = kv.Value?.ToString() ?? "";
            Console.WriteLine($"    val {kv.Key} = {(s.Length > 90 ? s[..90] + "..." : s)}");
        }
        foreach (var cat in it.ReferenceCategories)
            foreach (var r in cat.References)
                Console.WriteLine($"    ref {cat.Name} -> {r.TargetId} [v0={r.Value0} v1={r.Value1} v2={r.Value2}]");
    }
}
