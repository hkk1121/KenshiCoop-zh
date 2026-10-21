#pragma warning(push)
#pragma warning(disable: 4099 4819 4005 4091)
#include "EngineInternal.h"
#include <kenshi/RaceData.h>
#include "EngineChargen.h"
#include <kenshi/Appearance.h>
#include <kenshi/Platoon.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/gui/CharacterEditWindow.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Town.h>
#include <kenshi/util/hand.h>
#include <algorithm>
#pragma warning(pop)

namespace coop { namespace engine {
namespace {
GameWorld* editWorld = 0;
CharacterEditWindow* editor = 0;
// 正在捏的本机新角色（按句柄记，关闭时再解析，免得中途被销毁后读到野指针）
const unsigned int EDIT_MAX = 64;
unsigned int editHands[EDIT_MAX][5];
unsigned int editCount = 0;
std::string editStart;
bool confirming = false;
int editResult = 0;
float previousSpeed = 1.0f;
bool previousPause = false;
typedef void (__fastcall* ConfirmFn)(CharacterEditWindow*, MyGUI::Widget*);
typedef void (__fastcall* CloseFn)(ForgottenGUI*);
ConfirmFn confirmOriginal = 0;
CloseFn closeOriginal = 0;

void __fastcall confirmHook(CharacterEditWindow* self, MyGUI::Widget* sender) {
    bool ours = self == editor && editCount != 0;
    if (ours) confirming = true;
    confirmOriginal(self, sender);
    if (ours) confirming = false;
}
void __fastcall closeHook(ForgottenGUI* self) {
    bool ours = editor && self->characterEditor == editor;
    closeOriginal(self);
    if (ours) { editor = 0; editResult = confirming ? 1 : -1; }
}

// 引擎调用的保护帧只保留POD，容器构造与析构留在外层。
Character* createBody(GameWorld* gw, GameData* data, const float* at) {
    __try {
        if (!gw || !gw->player || !gw->theFactory || !data || !g_createCharFn) return 0;
        Faction* faction = gw->player->getFaction();
        if (!faction) return 0;
        Ogre::Vector3 pos(at[0], at[1], at[2]);
        if (g_terrainHeightFn) pos.y = g_terrainHeightFn(pos.x, pos.z) + 0.2f;
        RootObject* body = g_createCharFn(gw->theFactory, faction, pos, 0, data, 0, 25.0f);
        return body ? static_cast<Character*>(body) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool showEditor(GameWorld* gw, const lektor<Character*>* chars, const Ogre::vector<GameDataReference>::type* races) {
    __try {
        if (!::gui || ::gui->characterEditor) return false;
        typedef void (__fastcall* ShowFn)(ForgottenGUI*, const lektor<Character*>*, CharacterEditMode, const Ogre::vector<GameDataReference>::type*);
        ShowFn show = (ShowFn)KenshiLib::GetRealAddress(&ForgottenGUI::showCharacterEditor);
        if (!show) return false;
        show(::gui, chars, EDIT_NEWGAME, races);
        editor = ::gui->characterEditor;
        gw->userPause(true);
        return editor != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
GameDataCopyStandalone* appearanceOf(Character* c) {
    __try { return c ? c->getAppearanceData() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool setDraftAppearance(Character* c, GameDataCopyStandalone* data, GameData* race, const std::string* name) {
    __try {
        c->setRace(race);
        c->setAppearanceData(data);
        c->setName(*name);
        if (c->getAppearance()) c->getAppearance()->reload();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool choiceLess(const CharacterChoice& a, const CharacterChoice& b) {
    return a.startSid < b.startSid;
}
// 与原生NEWGAME筛选相同：排除动物、不可玩或没有种族组的记录。
bool playableRace(GameData* race) {
    if (!race) return false;
    RaceData* info = RaceData::getRaceData(race);
    auto playable = race->bdata.find("playable");
    auto animal = race->bdata.find("animal");
    return info && info->raceGroup && playable != race->bdata.end() && playable->second &&
        (animal == race->bdata.end() || !animal->second);
}
GameData* raceDataOf(Character* c) {
    __try {
        RaceData* race = c ? c->getRace() : 0;
        return race ? race->data : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// KenshiLib 导入库里没有的导出：按修饰名从 KenshiLib.dll 取桩，再换成真实地址
void* klibExport(const char* name) {
    HMODULE module = GetModuleHandleA("KenshiLib.dll");
    void* stub = module ? (void*)GetProcAddress(module, name) : 0;
    return stub ? (void*)KenshiLib::GetRealAddress(stub) : 0;
}
// TownList 实例没有导出。原版 newGameWithCharEdit 里先 mov rcx,[TownList 全局] 再调用
// getAllTowsByGameData，从这条指令里取出全局地址。
void* townList() {
    static void** global = 0;
    if (!global) {
        unsigned char* f = (unsigned char*)klibExport("?newGameWithCharEdit@KingOfRenderThread@@QEAAXPEAVGameData@@@Z");
        unsigned char* want = (unsigned char*)klibExport("?getAllTowsByGameData@TownList@@QEAAXAEAV?$lektor@PEAVTown@@@@PEAVGameData@@@Z");
        __try {
            for (unsigned int i = 0; f && want && !global && i < 0x2000; ++i) {
                if (f[i] != 0x48 || f[i + 1] != 0x8B || f[i + 2] != 0x0D || f[i + 7] != 0xE8) continue;
                unsigned char* call = f + i + 12 + *(int*)(f + i + 8);
                if (call[0] == 0xE9) call += 5 + *(int*)(call + 1);
                if (call == want) global = (void**)(f + i + 7 + *(int*)(f + i + 3));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { global = 0; }
    }
    __try { return global ? *global : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool townPosition(Town* town, float out[3]) {
    __try {
        if (!town) return false;
        Ogre::Vector3 p = town->getPosition();
        out[0] = p.x; out[1] = p.y; out[2] = p.z;
        return _finite(p.x) && _finite(p.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool spawnSquadTemplate(GameWorld* gw, GameData* squad, const float pos[3], const hand* none, unsigned int tab[2], unsigned int first[5]) {
    __try {
        Platoon* p = gw->theFactory->createRandomSquad(gw->player->getFaction(), Ogre::Vector3(pos[0], pos[1], pos[2]), 0, 0, 0,
                                                       squad, 0, 0, 0, true, *none, 0, 1.0f, (SquadType)0, false);
        if (!p) return false;
        const hand& h = p->getHandle();
        tab[0] = h.container; tab[1] = h.containerSerial;
        if (!first[1] && p->activePlatoon && p->activePlatoon->squadleader)
            readObjectHand(static_cast<RootObject*>(p->activePlatoon->squadleader), first);
        return (tab[0] | tab[1]) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void completeStartResearch(GameWorld* gw, const Ogre::vector<GameDataReference>::type* list) {
    typedef void (__fastcall* CompleteFn)(Research*, GameData*);
    CompleteFn complete = (CompleteFn)klibExport("?completeResearch@Research@@QEAAXPEAVGameData@@@Z");
    __try {
        if (!complete || !list || !gw->player || !gw->player->technology) return;
        for (size_t i = 0; i < list->size(); ++i) {
            GameData* research = gw->gamedata.getData((*list)[i].sid);
            if (research) complete(gw->player->technology, research);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}

bool characterCreationHooks() {
    if (!confirmOriginal) {
        void* addr = (void*)KenshiLib::GetRealAddress(&CharacterEditWindow::confirmButton);
        if (!addr || KenshiLib::AddHook(addr, (void*)&confirmHook, (void**)&confirmOriginal) != KenshiLib::SUCCESS) return false;
    }
    if (!closeOriginal) {
        void* addr = (void*)KenshiLib::GetRealAddress(&ForgottenGUI::closeCharacterEditor);
        if (!addr || KenshiLib::AddHook(addr, (void*)&closeHook, (void**)&closeOriginal) != KenshiLib::SUCCESS) return false;
    }
    return true;
}

void characterChoices(GameWorld* gw, std::vector<CharacterChoice>& out) {
    out.clear();
    if (!gw) return;
    lektor<GameData*> starts;
    gw->gamedata.getDataOfType(starts, NEW_GAME_STARTOFF);
    for (unsigned int i = 0; i < starts.size(); ++i) {
        GameData* start = starts[i];
        // 没有开局队伍的记录生成不出角色，不列出
        const Ogre::vector<GameDataReference>::type* squads = start ? start->getReferenceListIfExists("squad") : 0;
        if (!squads || squads->empty()) continue;
        CharacterChoice c; c.startSid = start->stringID; c.label = start->name;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), choiceLess);
}

bool startPosition(GameWorld* gw, const std::string& startSid, float out[3]) {
    GameData* start = gw ? findItemTemplateImpl(gw, startSid.c_str(), NEW_GAME_STARTOFF) : 0;
    if (!start) return false;
    auto forced = start->bdata.find("force start pos");
    auto x = start->idata.find("start pos X");
    auto z = start->idata.find("start pos Z");
    if (forced != start->bdata.end() && forced->second && x != start->idata.end() && z != start->idata.end()) {
        out[0] = (float)x->second; out[1] = 0.0f; out[2] = (float)z->second;
        return true;
    }
    typedef void (__fastcall* TownsFn)(void* list, lektor<Town*>* found, GameData* town);
    static TownsFn townsOf = (TownsFn)klibExport("?getAllTowsByGameData@TownList@@QEAAXAEAV?$lektor@PEAVTown@@@@PEAVGameData@@@Z");
    void* list = townList();
    const Ogre::vector<GameDataReference>::type* towns = start->getReferenceListIfExists("town");
    lektor<Town*> found;
    for (size_t i = 0; towns && list && townsOf && i < towns->size(); ++i) {
        GameData* town = gw->gamedata.getData((*towns)[i].sid);
        if (town) townsOf(list, &found, town);
    }
    return found.size() && townPosition(found[(unsigned int)rand() % found.size()], out);
}

bool createStartSquads(GameWorld* gw, const std::string& startSid, const float at[3],
                       std::vector<std::pair<unsigned int, unsigned int> >& tabs, unsigned int first[5], int* money) {
    tabs.clear(); memset(first, 0, sizeof(unsigned int) * 5); *money = 0;
    GameData* start = gw ? findItemTemplateImpl(gw, startSid.c_str(), NEW_GAME_STARTOFF) : 0;
    const Ogre::vector<GameDataReference>::type* squads = start ? start->getReferenceListIfExists("squad") : 0;
    if (!squads || squads->empty() || !gw->player || !gw->theFactory) return false;
    float pos[3] = { at[0], at[1], at[2] };
    if (g_terrainHeightFn) pos[1] = g_terrainHeightFn(pos[0], pos[2]);
    hand none;
    unsigned int people[2] = { 0, 0 };
    for (size_t i = 0; i < squads->size(); ++i) {
        GameData* data = gw->gamedata.getData((*squads)[i].sid);
        if (!data) continue;
        unsigned int tab[2] = { 0, 0 };
        if (data->type == SQUAD_TEMPLATE) {
            if (spawnSquadTemplate(gw, data, pos, &none, tab, first)) tabs.push_back(std::make_pair(tab[0], tab[1]));
            continue;
        }
        if (data->type != CHARACTER) continue;
        // 角色条目按份数（0 当 1）放进同一支新队，各自在出生点周围 ±50 散开，与原版一致
        if (!(people[0] | people[1])) {
            if (!createPlayerSquad(gw, people)) continue;
            tabs.push_back(std::make_pair(people[0], people[1]));
        }
        unsigned int target[5] = { 0, people[0], people[1], 0, 0 };
        int copies = (*squads)[i].values.value[0] > 0 ? (*squads)[i].values.value[0] : 1;
        for (int k = 0; k < copies && k < (int)EDIT_MAX; ++k) {
            float p[3] = { pos[0] + (float)(rand() % 101 - 50), pos[1], pos[2] + (float)(rand() % 101 - 50) };
            Character* c = createBody(gw, data, p);
            if (!c) continue;
            if (!joinPlayerSquadAt(gw, c, target)) { despawnProxyNpc(gw, c); continue; }
            if (!first[1]) readObjectHand(static_cast<RootObject*>(c), first);
        }
    }
    completeStartResearch(gw, start->getReferenceListIfExists("research"));
    // 势力关系全队共用：照原版把开局的关系（v0）双向写进共用关系表，再由势力关系同步发给所有人
    const Ogre::vector<GameDataReference>::type* relations = start->getReferenceListIfExists("faction relations");
    for (size_t i = 0; relations && i < relations->size(); ++i)
        writeRelationBySid(gw, (*relations)[i].sid.c_str(), (float)(*relations)[i].values.value[0], true, 0, 0);
    auto cash = start->idata.find("money");
    if (cash != start->idata.end()) *money = cash->second;
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[chargen] game start '%s' spawned squads=%u at %.0f,%.0f money=%d relations=%u",
              startSid.c_str(), (unsigned)tabs.size(), pos[0], pos[2], *money, relations ? (unsigned)relations->size() : 0u);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return !tabs.empty() && first[1] != 0;
}

bool beginStartEditing(GameWorld* gw, const std::vector<std::pair<unsigned int, unsigned int> >& tabs, const std::string& startSid) {
    if (editCount || !gw || !gw->player || tabs.empty() || !characterCreationHooks() || !::gui || ::gui->characterEditor) return false;
    GameData* start = findItemTemplateImpl(gw, startSid.c_str(), NEW_GAME_STARTOFF);
    if (!start) return false;
    std::vector<Character*> chars;
    const lektor<Character*>& all = gw->player->playerCharacters;
    for (unsigned int i = 0; i < all.size() && chars.size() < EDIT_MAX; ++i) {
        unsigned int h[5];
        if (!all[i] || !readObjectHand(static_cast<RootObject*>(all[i]), h)) continue;
        if (std::find(tabs.begin(), tabs.end(), std::make_pair(h[1], h[2])) == tabs.end()) continue;
        if (!playableRace(raceDataOf(all[i]))) continue;
        memcpy(editHands[chars.size()], h, sizeof(h));
        chars.push_back(all[i]);
    }
    if (chars.empty()) return false;
    previousSpeed = g_speedIntentSeeded ? g_speedIntentMult : 1.0f;
    previousPause = g_speedIntentSeeded ? g_speedIntentPaused : false;
    // showCharacterEditor 按值接收 lektor，返回前会释放 stuff（MSVCR100 free），所以必须放在 CRT 堆上交给它
    lektor<Character*> list; list.stuff = (Character**)malloc(sizeof(Character*) * chars.size());
    if (!list.stuff) return false;
    for (size_t i = 0; i < chars.size(); ++i) list.stuff[i] = chars[i];
    list.count = list.maxSize = (uint32_t)chars.size();
    editCount = (unsigned int)chars.size(); editWorld = gw; editStart = startSid; editResult = 0; confirming = false;
    // 原生 NEWGAME 捏人界面构造时，会把外观 bdata["from file"] 为假的角色原地重置并随机化（原版新开局靠它给随机长相）；
    // 在已有世界里用时，取消后本机长相就和主机、其他玩家不一样。先标记跳过随机化，结束编辑时在 cancelCharacterCreation 去掉
    for (size_t i = 0; i < chars.size(); ++i) {
        GameDataCopyStandalone* data = appearanceOf(chars[i]);
        if (data) data->bdata["from file"] = true;
    }
    cameraFocusOn(gw, chars[0]);
    const Ogre::vector<GameDataReference>::type* races = start->getReferenceListIfExists("force race");
    if (!showEditor(gw, &list, races && !races->empty() ? races : 0)) { cancelCharacterCreation(true); return false; }
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[chargen] native NEWGAME editor opened for %u starting characters", editCount);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return true;
}

int pollCharacterCreation(GameWorld* gw, std::vector<StartAppearance>& out) {
    out.clear();
    if (editCount && gw != editWorld) { cancelCharacterCreation(false); return -1; }
    if (editCount && editor && (!::gui || ::gui->characterEditor != editor)) { editor = 0; editResult = -1; }
    if (editCount && editor) writeGameSpeed(editWorld, previousSpeed, true);
    if (!editCount || editor || !editResult) return 0;
    int result = editResult;
    for (unsigned int i = 0; result > 0 && i < editCount; ++i) {
        Character* c = resolveCharByHand(editHands[i][3], editHands[i][4], editHands[i][0], editHands[i][1], editHands[i][2]);
        GameDataCopyStandalone* data = appearanceOf(c);
        if (!data) continue;
        StartAppearance a;
        memcpy(a.hand, editHands[i], sizeof(a.hand));
        CharacterDraft& draft = a.draft;
        draft.startSid = editStart;
        draft.templateSid = c->data ? c->data->stringID : editStart;
        draft.name = c->getName();
        // 保留原生外观的八类值；不复制模板属性、装备或资金。
        #define CAPTURE_APPEARANCE(MAP, KIND, BODY) \
        for (size_t unused = 0; unused < 1; ++unused) { \
            for (auto it = data->MAP.begin(); it != data->MAP.end(); ++it) { \
                AppearanceValue value; value.kind = KIND; value.key = it->first; BODY; draft.values.push_back(value); \
            } \
        }
        CAPTURE_APPEARANCE(bdata, AV_BOOL, u8 b = it->second ? 1 : 0; draftAppend(value.bytes, &b, 1));
        CAPTURE_APPEARANCE(idata, AV_INT, draftAppend(value.bytes, &it->second, 4));
        CAPTURE_APPEARANCE(fdata, AV_FLOAT, draftAppend(value.bytes, &it->second, 4));
        CAPTURE_APPEARANCE(sdata, AV_STRING, draftAppend(value.bytes, it->second.data(), (unsigned int)it->second.size()));
        CAPTURE_APPEARANCE(filesdata, AV_FILE, draftAppend(value.bytes, it->second.data(), (unsigned int)it->second.size()));
        CAPTURE_APPEARANCE(vecdata, AV_VECTOR, draftAppend(value.bytes, &it->second.x, 12));
        CAPTURE_APPEARANCE(quatdata, AV_QUATERNION, draftAppend(value.bytes, &it->second.w, 16));
        #undef CAPTURE_APPEARANCE
        for (auto it = data->objectReferences.begin(); it != data->objectReferences.end(); ++it) {
            AppearanceValue v; v.kind = AV_REFS; v.key = it->first;
            u16 count = (u16)it->second.size(); draftAppend(v.bytes, &count, 2);
            for (size_t j = 0; j < it->second.size(); ++j) {
                draftString(v.bytes, it->second[j].sid); draftAppend(v.bytes, it->second[j].values.value, 12);
            }
            draft.values.push_back(v);
        }
        std::vector<u8> encoded;
        if (encodeCharacterDraft(draft, encoded)) out.push_back(a);
    }
    if (result > 0 && out.empty()) result = -2;
    cancelCharacterCreation(true);
    return result;
}

bool cancelCharacterCreation(bool worldAlive) {
    if (worldAlive && editWorld) {
        if (editor && ::gui && ::gui->characterEditor == editor) ::gui->closeCharacterEditor();
        writeGameSpeed(editWorld, previousSpeed, previousPause);
        for (unsigned int i = 0; i < editCount; ++i) {
            GameDataCopyStandalone* data = appearanceOf(resolveCharByHand(editHands[i][3], editHands[i][4], editHands[i][0], editHands[i][1], editHands[i][2]));
            if (data) data->bdata.erase("from file");
        }
    }
    editor = 0; editWorld = 0; editResult = 0; confirming = false; editCount = 0; editStart.clear();
    return true;
}

bool characterEditing() { return editCount != 0; }

unsigned int playerTabSize(GameWorld* gw, unsigned int container, unsigned int serial) {
    __try {
        if (!gw || !gw->player || !gw->player->getFaction()) return 0xFFFFFFFFu;
        Faction* f = gw->player->getFaction();
        const lektor<Platoon*>* lists[2] = { &f->activePlatoons, &f->unloadedPlatoons };
        for (int list = 0; list < 2; ++list) for (unsigned int i = 0; i < lists[list]->size(); ++i) {
            Platoon* p = (*lists[list])[i];
            if (p && p->getHandle().container == container && p->getHandle().containerSerial == serial)
                return (unsigned int)p->getCharacterCount();
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

bool applyCharacterDraft(GameWorld* gw, Character* c, const CharacterDraft& draft) {
    if (!gw || !c || !validCharacterDraft(draft) || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, draft.name.data(), (int)draft.name.size(), 0, 0)) return false;
    GameData* start = findItemTemplateImpl(gw, draft.startSid.c_str(), NEW_GAME_STARTOFF);
    if (!start) { coop::logErrLine("[chargen] draft start is not a loaded game start"); return false; }
    GameDataCopyStandalone* data = new GameDataCopyStandalone();
    data->initialise(CHARACTER_APPEARANCE, false);
    for (size_t i = 0; i < draft.values.size(); ++i) {
        const AppearanceValue& v = draft.values[i]; const u8* bytes = v.bytes.empty() ? 0 : &v.bytes[0];
        if (v.kind == AV_BOOL) data->bdata[v.key] = bytes[0] != 0;
        else if (v.kind == AV_INT) { int n; memcpy(&n, bytes, 4); data->idata[v.key] = n; }
        else if (v.kind == AV_FLOAT) { float n; memcpy(&n, bytes, 4); data->fdata[v.key] = n; }
        else if (v.kind == AV_STRING) data->sdata[v.key].assign(bytes ? (const char*)bytes : "", v.bytes.size());
        else if (v.kind == AV_FILE) data->filesdata[v.key].assign(bytes ? (const char*)bytes : "", v.bytes.size());
        else if (v.kind == AV_VECTOR) { float n[3]; memcpy(n, bytes, 12); data->vecdata[v.key] = Ogre::Vector3(n[0], n[1], n[2]); }
        else if (v.kind == AV_QUATERNION) { float n[4]; memcpy(n, bytes, 16); data->quatdata[v.key] = Ogre::Quaternion(n[0], n[1], n[2], n[3]); }
        else if (v.kind == AV_REFS) {
            const u8* p = bytes; const u8* end = p + v.bytes.size(); u16 count;
            draftRead(p, end, &count, 2);
            data->objectReferences[v.key].clear();
            for (u16 j = 0; j < count; ++j) {
                std::string sid; int vals[3]; draftReadString(p, end, sid, 127); draftRead(p, end, vals, 12);
                data->addToList(v.key, sid, vals[0], vals[1], vals[2]);
            }
        }
    }
    GameData* race = gw->gamedata.getData(data->getFromList("race", 0), RACE);
    const Ogre::vector<GameDataReference>::type* limits = start->getReferenceListIfExists("force race");
    bool validRace = race != 0;
    if (limits && !limits->empty()) {
        validRace = false;
        for (size_t i = 0; race && i < limits->size(); ++i) if ((*limits)[i].sid == race->stringID) validRace = true;
    }
    if (validRace) {
        validRace = playableRace(race);
        AppearanceManager* manager = AppearanceManager::getInstance();
        if (validRace && manager) manager->cleanValidateAppearanceData(data);
    }
    if (!validRace) { delete data; coop::logErrLine("[chargen] draft race is not playable or violates game-start filter"); return false; }
    // 原地改写角色现有的外观数据再原样交回：换成新对象会释放旧的，而已挂上的头发等附件仍指着旧对象，
    // 异步建模回调（setupHairMaterial）读它就崩。同一指针交回时引擎不释放，只按新值重算种族、发型等。
    GameDataCopyStandalone* current = appearanceOf(c);
    if (current) {
        current->bdata = data->bdata; current->idata = data->idata; current->fdata = data->fdata;
        current->sdata = data->sdata; current->filesdata = data->filesdata; current->vecdata = data->vecdata;
        current->quatdata = data->quatdata; current->objectReferences = data->objectReferences;
    }
    delete data;
    bool applied = current && setDraftAppearance(c, current, race, &draft.name);
    if (!applied) coop::logErrLine("[chargen] native appearance apply failed");
    return applied;
}
} }
