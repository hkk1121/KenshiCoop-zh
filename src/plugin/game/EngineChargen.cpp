#pragma warning(push)
#pragma warning(disable: 4099 4819 4005 4091)
#include "EngineInternal.h"
#include <kenshi/RaceData.h>
#include "EngineChargen.h"
#include <kenshi/Appearance.h>
#include <kenshi/Platoon.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/gui/CharacterEditWindow.h>
#include <algorithm>
#pragma warning(pop)

namespace coop { namespace engine {
namespace {
Character* preview = 0;
GameWorld* previewWorld = 0;
CharacterEditWindow* editor = 0;
CharacterChoice previewChoice;
bool confirming = false;
int editResult = 0;
float previousSpeed = 1.0f;
bool previousPause = false;
float previousCamera[3] = { 0, 0, 0 };
unsigned int previousFollow[5] = { 0, 0, 0, 0, 0 };
std::string deferredSave;
bool deferredAuto = true;
typedef void (__fastcall* ConfirmFn)(CharacterEditWindow*, MyGUI::Widget*);
typedef void (__fastcall* CloseFn)(ForgottenGUI*);
ConfirmFn confirmOriginal = 0;
CloseFn closeOriginal = 0;

void __fastcall confirmHook(CharacterEditWindow* self, MyGUI::Widget* sender) {
    bool ours = self == editor && preview != 0;
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
bool showEditor(GameWorld* gw, const lektor<Character*>* chars, GameData* start) {
        if (!::gui || ::gui->characterEditor) return false;
        const Ogre::vector<GameDataReference>::type* races = start->getReferenceListIfExists("force race");
        if (races && races->empty()) races = 0;
        ::gui->showCharacterEditor(*chars, EDIT_NEWGAME, races);
        editor = ::gui->characterEditor;
        gw->userPause(true);
        return editor != 0;
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
void restoreEditorWorld() {
    __try {
        if (!previewWorld || !previewWorld->player) return;
        previewWorld->player->stopTrackCharacter();
        if (preview) previewWorld->player->objectSelected(preview, false);
        if (previewWorld->player->camera) {
            previewWorld->player->camera->teleport(Ogre::Vector3(previousCamera[0], previousCamera[1], previousCamera[2]));
            Character* followed = resolveCharByHand(previousFollow[3], previousFollow[4], previousFollow[0], previousFollow[1], previousFollow[2]);
            if (followed) previewWorld->player->startTrackCharacter(followed);
        }
        writeGameSpeed(previewWorld, previousSpeed, previousPause);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool choiceLess(const CharacterChoice& a, const CharacterChoice& b) {
    return a.startSid < b.startSid || (a.startSid == b.startSid && a.templateSid < b.templateSid);
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
    std::set<std::pair<std::string, std::string> > seen;
    for (unsigned int i = 0; i < starts.size(); ++i) {
        GameData* start = starts[i];
        if (!start) continue;
        const Ogre::vector<GameDataReference>::type* squads = start->getReferenceListIfExists("squad");
        if (!squads) continue;
        for (size_t j = 0; j < squads->size(); ++j) {
            GameData* target = gw->gamedata.getData((*squads)[j].sid);
            if (!target) continue;
            std::vector<GameData*> candidates;
            if (target->type == CHARACTER) candidates.push_back(target);
            else if (target->type == SQUAD_TEMPLATE) {
                const char* lists[] = { "leader", "squad" };
                for (int k = 0; k < 2; ++k) {
                    const Ogre::vector<GameDataReference>::type* members = target->getReferenceListIfExists(lists[k]);
                    if (!members) continue;
                    for (size_t m = 0; m < members->size(); ++m) {
                        GameData* member = gw->gamedata.getData((*members)[m].sid);
                        if (member && member->type == CHARACTER) candidates.push_back(member);
                    }
                }
            }
            for (size_t k = 0; k < candidates.size(); ++k) {
                if (!seen.insert(std::make_pair(start->stringID, candidates[k]->stringID)).second) continue;
                CharacterChoice c; c.startSid = start->stringID; c.templateSid = candidates[k]->stringID;
                c.label = start->name + " / " + candidates[k]->name; out.push_back(c);
            }
        }
    }
    std::sort(out.begin(), out.end(), choiceLess);
}

bool beginCharacterCreation(GameWorld* gw, const CharacterChoice& choice) {
    if (preview || !gw || !characterCreationHooks() || !::gui || ::gui->characterEditor) return false;
    GameData* tmpl = findItemTemplateImpl(gw, choice.templateSid.c_str(), CHARACTER);
    GameData* start = findItemTemplateImpl(gw, choice.startSid.c_str(), NEW_GAME_STARTOFF);
    if (!tmpl || !start || !cameraCenter(gw, previousCamera)) return false;
    memset(previousFollow, 0, sizeof(previousFollow));
    if (gw->player && gw->player->camera) {
        const hand& h = gw->player->camera->getFollowObject();
        previousFollow[0] = h.type; previousFollow[1] = h.container; previousFollow[2] = h.containerSerial;
        previousFollow[3] = h.index; previousFollow[4] = h.serial;
    }
    previousSpeed = g_speedIntentSeeded ? g_speedIntentMult : 1.0f;
    previousPause = g_speedIntentSeeded ? g_speedIntentPaused : false;
    previewWorld = gw; previewChoice = choice; editResult = 0;
    preview = createBody(gw, tmpl, previousCamera);
    if (!preview) { previewWorld = 0; return false; }
    detachFromTownAI(preview); clearGoals(preview);
    lektor<Character*> chars; chars.stuff = &preview; chars.count = chars.maxSize = 1;
    if (!showEditor(gw, &chars, start)) { cancelCharacterCreation(true); return false; }
    coop::logLine("[chargen] native NEWGAME editor opened; preview excluded from replication");
    return true;
}

int pollCharacterCreation(GameWorld* gw, CharacterDraft& out) {
    if (preview && gw != previewWorld) { cancelCharacterCreation(false); return -1; }
    if (preview && editor && (!::gui || ::gui->characterEditor != editor)) { editor = 0; editResult = -1; }
    if (!preview || editor || !editResult) return 0;
    if (gw != previewWorld) { cancelCharacterCreation(false); return -1; }
    int result = editResult;
    if (result > 0) {
        GameDataCopyStandalone* data = appearanceOf(preview);
        if (!data) result = -2;
        else {
            CharacterDraft draft;
            draft.startSid = previewChoice.startSid; draft.templateSid = previewChoice.templateSid;
            draft.name = preview->getName();
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
                for (size_t i = 0; i < it->second.size(); ++i) {
                    draftString(v.bytes, it->second[i].sid); draftAppend(v.bytes, it->second[i].values.value, 12);
                }
                draft.values.push_back(v);
            }
            std::vector<u8> encoded;
            if (encodeCharacterDraft(draft, encoded)) out = draft;
            else result = -2;
        }
    }
    cancelCharacterCreation(true);
    return result;
}

void cancelCharacterCreation(bool worldAlive, bool resumeSave) {
    if (worldAlive && previewWorld) {
        if (editor && ::gui && ::gui->characterEditor == editor) ::gui->closeCharacterEditor();
        restoreEditorWorld();
        if (preview && !despawnProxyNpc(previewWorld, preview)) coop::logErrLine("[chargen] preview destruction failed");
    }
    preview = 0; editor = 0; previewWorld = 0; editResult = 0; confirming = false;
    if (worldAlive && resumeSave && !deferredSave.empty()) {
        std::string name = deferredSave; deferredSave.clear();
        coop::logLine("[chargen] resuming deferred save after preview cleanup");
        saveGameAs(name);
    } else deferredSave.clear();
}

bool characterPreview(const Character* c) { return preview && (!c || c == preview); }

bool deferCharacterSave(const std::string* name, bool autosave) {
    if (!preview || !name || name->empty()) return false;
    if (deferredSave.empty() || deferredAuto || !autosave) { deferredSave = *name; deferredAuto = autosave; }
    coop::logLine("[chargen] save deferred while temporary editor character exists");
    return true;
}

unsigned int playerTabSize(GameWorld* gw, unsigned int container, unsigned int serial) {
    __try {
        if (!gw || !gw->player || !gw->player->getFaction()) return 0xFFFFFFFFu;
        Faction* f = gw->player->getFaction();
        for (unsigned int i = 0; i < f->activePlatoons.size(); ++i) {
            Platoon* p = f->activePlatoons[i];
            if (p && p->getHandle().container == container && p->getHandle().containerSerial == serial)
                return (unsigned int)p->getCharacterCount();
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

bool applyCharacterDraft(GameWorld* gw, Character* c, const CharacterDraft& draft) {
    if (!gw || !c || !validCharacterDraft(draft) || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, draft.name.data(), (int)draft.name.size(), 0, 0)) return false;
    std::vector<CharacterChoice> choices; characterChoices(gw, choices);
    bool allowed = false;
    for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i].startSid == draft.startSid && choices[i].templateSid == draft.templateSid) { allowed = true; break; }
    if (!allowed) return false;
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
            for (u16 j = 0; j < count; ++j) {
                std::string sid; int vals[3]; draftReadString(p, end, sid, 127); draftRead(p, end, vals, 12);
                data->addToList(v.key, sid, vals[0], vals[1], vals[2]);
            }
        }
    }
    GameData* race = gw->gamedata.getData(data->getFromList("race", 0), RACE);
    GameData* start = gw->gamedata.getData(draft.startSid, NEW_GAME_STARTOFF);
    const Ogre::vector<GameDataReference>::type* limits = start ? start->getReferenceListIfExists("force race") : 0;
    bool validRace = race != 0;
    if (limits && !limits->empty()) {
        validRace = false;
        for (size_t i = 0; race && i < limits->size(); ++i) if ((*limits)[i].sid == race->stringID) validRace = true;
    }
    if (validRace) {
        // 与原生NEWGAME筛选相同：排除动物、不可玩或没有种族组的记录。
        RaceData* raceInfo = RaceData::getRaceData(race);
        auto playable = race->bdata.find("playable");
        auto animal = race->bdata.find("animal");
        validRace = raceInfo && raceInfo->raceGroup && playable != race->bdata.end() && playable->second &&
            (animal == race->bdata.end() || !animal->second);
        AppearanceManager* manager = AppearanceManager::getInstance();
        if (validRace && manager) manager->cleanValidateAppearanceData(data);
    }
    if (!validRace) { delete data; return false; }
    // setAppearanceData接管指针；即使后续reload失败，也不能再次释放。
    return setDraftAppearance(c, data, race, &draft.name);
}

bool createDraftCharacter(GameWorld* gw, const CharacterDraft& draft, const float* position, unsigned int out[5]) {
    if (!gw || !out || !validCharacterDraft(draft)) return false;
    GameData* tmpl = findItemTemplateImpl(gw, draft.templateSid.c_str(), CHARACTER);
    float at[3] = { 0, 0, 0 };
    Character* ld = leader(gw);
    if ((!ld || !readPos(ld, &at[0], &at[1], &at[2])) && !cameraCenter(gw, at)) return false;
    if (position && _finite(position[0]) && _finite(position[2])) {
        float dx = position[0] - at[0], dz = position[2] - at[2];
        if (dx * dx + dz * dz < 40000.0f && isZoneLoadedAt(gw, position[0], at[1], position[2])) { at[0] = position[0]; at[2] = position[2]; }
    }
    Character* c = createBody(gw, tmpl, at);
    if (!c) return false;
    if (!applyCharacterDraft(gw, c, draft)) { despawnProxyNpc(gw, c); return false; }
    unsigned int tab[2] = { 0, 0 };
    if (!createPlayerSquad(gw, tab)) { despawnProxyNpc(gw, c); return false; }
    unsigned int target[5] = { 0, tab[0], tab[1], 0, 0 };
    if (!joinPlayerSquadAt(gw, c, target) || !readObjectHand(static_cast<RootObject*>(c), out)) {
        despawnProxyNpc(gw, c); return false;
    }
    coop::logLine("[chargen] authoritative character created in new player squad");
    return true;
}
} }
