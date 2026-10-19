// EngineUi.cpp - the in-game co-op UI plane: the debug marker HUD labels, the
// F2 co-op session panel (native DatapanelGUI), and the persistent status
// overlay. Split out of EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so
// the entity capture/resolve/apply TU stays focused on sync and the UI/render
// surface lives with its public header (EngineUi.h).
//
// Owner state: section-private statics / anon-namespace SEH shims only (marker
// colour + create/update/destroy shims, panel widget pointers, overlay label).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them), install
// hooks, or change any log string - "[coop-ui] ..." phrasing is API consumed by
// the harness. The public marker* declarations stay in Engine.h (the Replicator
// uses them for KENSHICOOP_DEBUG_MARKERS); only their definitions moved here.

#include "EngineInternal.h"

// In-game co-op session panel: the native DatapanelGUI window + its interactive
// rows and Win32 key capture. EngineInternal.h already pulls Globals.h (::gui),
// ForgottenGUI.h and MyGUI_Button.h; these add the panel row types, the
// free-function delegate factory, and GetAsyncKeyState/VK_* for the F2 toggle +
// digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>

#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)
#include "../core/Config.h"  // coop::tr (UI text localization)
#include "../core/FpsLimit.h" // 帧数上限切换按钮
#include "../../netproto/Wire.h" // MAX_PLAYERS（小队槽位切换上限）

namespace coop {
namespace engine {

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is this label one the GUI still owns? ForgottenGUI keeps the authoritative
// registry of live ScreenLabels (plus deferred add/remove queues), and that
// registry - not our cached pointer - is the only ground truth available: the
// GUI destroys labels on its own schedule and notifies nobody, so a handle held
// across ticks dangles silently. Measured 2026-08-03: the join minted three
// proxies in one burst, debugMark found stale map entries sitting at RECYCLED
// Character* addresses, and setCaption on the previous occupants' dead labels
// faulted twice inside ScreenLabel::setCaption (kenshi+0x6e451e) moments before
// the process died. Same lesson as the world-item proxy hands - never
// dereference a cached engine pointer without re-asking the engine.
//
// Deliberately unlocked. guiScreenLabelsMutex guards these lektors, but taking
// an engine shared_mutex from inside a detour is its own class of hazard, and a
// torn read here is harmless BECAUSE we test for an exact pointer match: garbage
// answers "not present", which costs one discarded marker, whereas a false
// "alive" would need the torn word to equal the very pointer we are asking
// about. The safe direction is the likely one.
bool labelListHas(const lektor<ScreenLabelInterface*>* v, const void* l) {
    __try {
        ScreenLabelInterface* const* p = v->stuff;
        unsigned int n = v->count;
        if (!p || n > 8192u) return false;   // bound a torn/garbage count
        for (unsigned int i = 0; i < n; ++i)
            if ((const void*)p[i] == l) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerAlive(void* label) {
    if (!label) return false;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    // Queued for removal counts as dead: the GUI has already decided, and we
    // would only be racing its next flush.
    if (labelListHas(&g->guiScreenLabelsToRemove, label)) return false;
    // A freshly created label sits in the add queue until the GUI flushes it, so
    // both lists are "alive" or markerCreate's own handle would look dead.
    return labelListHas(&g->guiScreenLabels, label) ||
           labelListHas(&g->guiScreenLabelsToAdd, label);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text || !markerAlive(label)) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    // Handing the GUI a label it has already destroyed is the same
    // use-after-free as updating one; prune paths reach here with handles whose
    // Character went away, which is exactly when the GUI has cleaned up too.
    if (!markerAlive(label)) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

// ---- In-game co-op session panel (config-driven, spike-50 DatapanelGUI stack) -
// A native DatapanelGUI window toggled with F2. The player picks role + transport
// (toggle BUTTONS - the only DatapanelGUI control with a callable RVA callback;
// MyGUI comboboxes/editboxes have no reachable getters and never receive keyboard
// focus during gameplay) and connects/leaves via a bound checkbox. The friend code
// (peer SteamID) + UDP endpoint come from coop_config.json and are shown READ-ONLY;
// a "Copy my Steam ID" button puts the player's own id on the clipboard to share.
// The GUI layer is session-agnostic: live status arrives via *st; the user's
// actions leave via the onConnect/onDisconnect callbacks (the plugin root owns the
// net/session/config wiring).
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The one exception is createDatapanel's BY-VALUE
// std::string 'layer' arg (an unwindable temporary => C2712), so the window is
// created in the outer, non-SEH function; ::gui is verified non-null first and the
// createScreenLabel/createFloatingLabel factory family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          hostFlag;      // true = HOST role armed
    bool          steamFlag;     // true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    std::string   lastStatus;    // last status text shown (refresh gate)
    std::string   lastTransfer;  // last save-transfer line shown (refresh gate)
    int           lastAuthReq;   // 上次的授权状态（第 3 批；任一项变化即重建面板）
    int           lastAuthReqable;
    int           lastAuthPending;
    int           lastAuthGranted;
    int           lastAuthBorrowed;
    std::string   lastDisc;      // last discovery-browser line shown (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), hostFlag(true), steamFlag(true),
          connectedFlag(false), lastConnected(false), lastChkVal(false),
          needsRebuild(false), f2Down(false), lastAuthReq(0), lastAuthReqable(0),
          lastAuthPending(0), lastAuthGranted(0), lastAuthBorrowed(0) {}
};

CoopPanelUi             g_panel;
DataPanelLine_Button*   g_roleBtn      = 0;
DataPanelLine_Button*   g_transBtn     = 0;
DataPanelLine_Button*   g_connBtn      = 0; // Online/Offline toggle (replaces the checkbox)
DataPanelLine_Button*   g_slotBtn      = 0; // JOIN squad-slot toggle (protocol 49)
DataPanelLine_Button*   g_copyIdBtn    = 0;
DataPanelLine_Button*   g_pasteIdBtn   = 0; // "Paste friend's Steam ID" from clipboard
DataPanelLine_Button*   g_fpsBtn       = 0; // 帧数上限切换
DataPanelLine_Button*   g_authReqBtn   = 0; // 申请控制好友的小队（第 3 批）
DataPanelLine_Button*   g_authYesBtn   = 0; // 同意对方申请
DataPanelLine_Button*   g_authNoBtn    = 0; // 拒绝对方申请
DataPanelLine_Button*   g_authRevokeBtn = 0; // 收回控制权
DataPanelLine*          g_authLine     = 0; // 授权状态行
// 面板把授权动作交回插件（= coopPanelTick 的 onAuth 参数；0 表示未接线）
CoopAuthFn              g_authFn       = 0;
DataPanelLine_Button*   g_scanBtn      = 0; // JOIN+UDP: scan tailnet / next found host
DataPanelLine*          g_debugLine    = 0; // white connection-status debug row
DataPanelLine*          g_discLine     = 0; // white "Found hosts" browser row
DataPanelLine*          g_peerLine     = 0; // white "Friend's Steam ID" row
DataPanelLine*          g_selfLine     = 0; // white "Your Steam ID" row
bool                    g_scanClicked  = false; // Scan pressed; dispatched in tick

// Title-screen co-op launcher: a small always-visible window on the MAIN MENU
// with native HOST GAME / JOIN GAME buttons (the same proven DatapanelGUI
// stack as the F2 panel - it renders at the title screen, spike 50). Clicking
// arms the role and opens the full panel; the launcher hides while the panel
// is open and is destroyed the moment a world exists.
DatapanelGUI*           g_menuPanel    = 0;
DataPanelLine_Button*   g_menuHostBtn  = 0;
DataPanelLine_Button*   g_menuJoinBtn  = 0;
DataPanelLine*          g_menuStatLine = 0;
bool                    g_menuBuilt    = false;
int                     g_menuOpenReq  = 0; // +1 HOST pressed, -1 JOIN pressed
std::string             g_menuLastStatus;   // rebuild gate for the status row
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Friend SteamIDs pasted in-panel this session (first stage: a HOST may hold
// up to MAX_PLAYERS - 1 friends; a JOIN uses slot 0 = the host). Per-session
// by design: they live only in memory, so relaunching Kenshi clears them and
// the ids are re-pasted (nothing is written to disk). Passed to onConnect,
// where a non-empty list overrides the config steamPeer(s). Paste APPENDS;
// a duplicate paste is a no-op; pasting when full starts the list over with
// the just-pasted id (the "fix a typo" path without a clear button).
const unsigned int      MAX_PASTED     = coop::MAX_PLAYERS - 1;
unsigned long long      g_pastedPeers[MAX_PASTED] = { 0 };
unsigned int            g_pastedCount  = 0;
bool                    g_pasteFailed  = false; // last paste wasn't a valid Steam ID
// JOIN squad-slot choice: 1 = second player, 2 = third player, 3 = fourth.
unsigned int            g_slotChoice   = 1;

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick, same as before.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// JOIN squad-slot toggle: claim the next squad tab. The host rejects a slot
// another join already holds.
void onSlotBtn(DataPanelLine*) {
    ++g_slotChoice;
    if (g_slotChoice >= coop::MAX_PLAYERS) g_slotChoice = 1;
    g_panel.needsRebuild = true;
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] squad slot -> %u", g_slotChoice);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Title-screen launcher buttons: record the choice; the ONE-CLICK action is
// dispatched in coopPanelTick (the plugin root owns the session wiring).
void onMenuHostBtn(DataPanelLine*) {
    g_menuOpenReq = 1;
    coop::logLine("[coop-ui] menu: HOST GAME pressed");
}
void onMenuJoinBtn(DataPanelLine*) {
    g_menuOpenReq = -1;
    coop::logLine("[coop-ui] menu: JOIN GAME pressed");
}
// Scan for hosts (JOIN + UDP): the actual scan/cycle logic lives in the plugin
// root (it owns the discovery module + config); the GUI only reports the press.
void onScanBtn(DataPanelLine*) {
    g_scanClicked = true;
    g_panel.needsRebuild = true;
    coop::logLine("[coop-ui] scan for hosts pressed");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Paste a friend's SteamID from the clipboard: read text, extract + validate a
// SteamID64, and APPEND it to the session peer list (used on the next
// Connect). No typing, no config edit. A duplicate paste is a no-op; pasting
// when the list is full starts it over with the just-pasted id. Rejects
// arbitrary clipboard junk (g_pasteFailed drives the peer-row hint).
void onPasteIdBtn(DataPanelLine*) {
    std::string clip;
    unsigned long long id = 0;
    if (clipboardGetText(clip) && coop::parseSteamId64(clip, id)) {
        bool dup = false;
        for (unsigned int i = 0; i < g_pastedCount; ++i)
            if (g_pastedPeers[i] == id) { dup = true; break; }
        if (!dup) {
            if (g_pastedCount >= MAX_PASTED) {
                g_pastedPeers[0] = id; // full: start over with this one
                g_pastedCount    = 1;
            } else {
                g_pastedPeers[g_pastedCount++] = id;
            }
        }
        g_pasteFailed = false;
        coop::saveLastPeer(id); // 最近粘贴的好友 ID 作为下次默认值（上游 PR #25）
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste friend id=%llu ok=1 (%u pasted)",
                  id, g_pastedCount);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    } else {
        g_pasteFailed = true;
        coop::logLine("[coop-ui] paste friend id=0 ok=0 (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}
void onFpsBtn(DataPanelLine*) {
    coop::fpslimit::cycle();
    g_panel.needsRebuild = true;
}

// 小队控制授权按钮（第 3 批）：把动作码交回插件，由插件调用 Replicator。
void onAuthReqBtn(DataPanelLine*) {
    if (g_authFn) g_authFn(0);
    g_panel.needsRebuild = true;
}
void onAuthYesBtn(DataPanelLine*) {
    if (g_authFn) g_authFn(1);
    g_panel.needsRebuild = true;
}
void onAuthNoBtn(DataPanelLine*) {
    if (g_authFn) g_authFn(2);
    g_panel.needsRebuild = true;
}
void onAuthRevokeBtn(DataPanelLine*) {
    if (g_authFn) g_authFn(3);
    g_panel.needsRebuild = true;
}

// POD-only pointer bundle so the row-build SEH frame constructs no std::string.
// slotKey/slotCap null = no squad-slot row (HOST role selected).
struct PanelStrings {
    const std::string *title, *roleKey, *roleCap, *transKey, *transCap;
    const std::string *connKey, *connCap;
    const std::string *slotKey, *slotCap;
    const std::string *scanKey, *scanCap;   // null = no Scan button (HOST/Steam)
    const std::string *discKey, *discVal;   // null = no browser row (nothing to show)
    const std::string *dbgKey, *dbgVal;
    const std::string *peerKey, *peerVal, *pasteKey, *pasteCap;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *fpsKey, *fpsCap;
    const std::string *authKey, *authVal, *authReqCap;
    const std::string *authYesCap, *authNoCap, *authRevokeCap;
    const std::string *authReqKey, *authYesKey, *authNoKey, *authRevokeKey;
    int showAuthReq, showAuthYes, showAuthNo, showAuthRevoke; // 1 = 该按钮行要出现
    const std::string *empty;
};

void panelBuildSeh(DatapanelGUI* p, const PanelStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_roleBtn  = p->setLineButton(*s->roleKey,  *s->roleCap,  0);
        g_transBtn = p->setLineButton(*s->transKey, *s->transCap, 0);
        g_connBtn  = p->setLineButton(*s->connKey,  *s->connCap,  0);
        // JOIN only (protocol 49): which squad tab this player claims.
        g_slotBtn = 0;
        if (s->slotKey && s->slotCap)
            g_slotBtn = p->setLineButton(*s->slotKey, *s->slotCap, 0);
        // JOIN + UDP: the tailnet/LAN game browser (scan + picked-host row).
        g_scanBtn = 0;
        if (s->scanKey && s->scanCap)
            g_scanBtn = p->setLineButton(*s->scanKey, *s->scanCap, 0);
        g_discLine = 0;
        if (s->discKey && s->discVal)
            g_discLine = p->setLine(*s->discKey, *s->discVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.35f);
        // Connection-status debug line (coloured white below, outside SEH).
        g_debugLine = p->setLine(*s->dbgKey, *s->dbgVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.35f);
        // Friend SteamID(s): pasted in-panel (Copy on their side -> Paste here).
        g_peerLine = p->setLine(*s->peerKey, *s->peerVal, *s->empty, 0, false, true);
        g_pasteIdBtn = p->setLineButton(*s->pasteKey, *s->pasteCap, 0);
        p->addSpace(0, 0.35f);
        g_selfLine = p->setLine(*s->selfKey, *s->selfVal, *s->empty, 0, false, true);
        g_copyIdBtn = p->setLineButton(*s->copyKey, *s->copyCap, 0);
        p->addSpace(0, 0.35f);
        g_fpsBtn = p->setLineButton(*s->fpsKey, *s->fpsCap, 0);
        p->addSpace(0, 0.35f);
        // 小队控制授权（第 3 批）：状态行 + 申请按钮（+ 条件出现的同意/拒绝/收回）
        g_authLine   = p->setLine(*s->authKey, *s->authVal, *s->empty, 0, false, true);
        if (s->showAuthReq)    g_authReqBtn    = p->setLineButton(*s->authReqKey, *s->authReqCap, 0);
        if (s->showAuthYes)    g_authYesBtn    = p->setLineButton(*s->authYesKey, *s->authYesCap, 0);
        if (s->showAuthNo)     g_authNoBtn     = p->setLineButton(*s->authNoKey,  *s->authNoCap, 0);
        if (s->showAuthRevoke) g_authRevokeBtn = p->setLineButton(*s->authRevokeKey, *s->authRevokeCap, 0);
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
// yellow=true tints the value column amber - used for the join's live
// "Streaming host world..." transfer line so it reads as in-progress activity.
void dbgColourSeh(DataPanelLine* line, bool yellow) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        MyGUI::Colour amber(1.0f, 0.82f, 0.20f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(yellow ? amber : white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Arm a freshly-minted panel: register it for ForgottenGUI's per-frame refresh
// AND make it visible. createDatapanel returns a built-but-hidden window; without
// this pair the F2 toggle logs open/close yet nothing ever draws (the render bug
// in the reconstruction). PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        g->addDatapanelToUpdateList(p);
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// POD-only string bundle for the launcher rows (same C2712 discipline as
// PanelStrings: the SEH build frame constructs no std::string).
struct MenuStrings {
    const std::string *title;
    const std::string *hostKey, *hostCap, *joinKey, *joinCap;
    const std::string *statKey, *statVal;
    const std::string *hintKey, *hintVal;
    const std::string *empty;
};

void menuBuildSeh(DatapanelGUI* p, const MenuStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_menuHostBtn = p->setLineButton(*s->hostKey, *s->hostCap, 0);
        g_menuJoinBtn = p->setLineButton(*s->joinKey, *s->joinCap, 0);
        p->addSpace(0, 0.30f);
        g_menuStatLine = p->setLine(*s->statKey, *s->statVal, *s->empty, 0, false, true);
        p->setLine(*s->hintKey, *s->hintVal, *s->empty, 0, false, true);
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Create/refresh/destroy the title-screen launcher. Exists exactly when the
// tick is at the TITLE and the full panel is closed; the status row mirrors
// the live session line so "Hosting - waiting for peer..." is visible without
// opening the panel (a join going ONLINE from the menu is the normal flow).
void coopMenuLauncherTick(ForgottenGUI* g, const CoopPanelState* st) {
    bool want = st->atTitle && !g_panel.open;
    if (!want) {
        if (g_menuPanel) {
            panelDestroySeh(g, g_menuPanel);
            g_menuPanel = 0; g_menuHostBtn = 0; g_menuJoinBtn = 0;
            g_menuStatLine = 0; g_menuBuilt = false;
        }
        return;
    }
    // Status priority: the world stream beats everything; a live session line
    // beats the browser line; the browser line (scanning / found / none)
    // narrates the one-click JOIN while still offline.
    std::string status = st->detail ? std::string(st->detail) : std::string();
    if (!st->running && st->discDetail && st->discDetail[0]) status = st->discDetail;
    if (st->transferDetail) status = st->transferDetail; // join streaming the world
    if (!g_menuPanel) {
        std::string layer = "Info"; // the render-proven layer (spike 48)
        // createDatapanel takes (TOP, LEFT, width, height) as render-area
        // fractions - NOT (x, y). Measured on the live title screen
        // (2026-08-03): args (0.70, 0.06) rendered at top=0.70/left=0.06,
        // and the F2 panel's (0.22, 0.30) renders at top 0.22, left 0.30.
        // Top-right corner, clear of the menu buttons (left column) and the
        // Kenshi logo text.
        g_menuPanel = g->createDatapanel(0.06f, 0.70f, 0.28f, 0.20f, false, layer, true);
        g_menuBuilt = false;
        if (!g_menuPanel) {
            static bool s_warned = false;
            if (!s_warned) { s_warned = true;
                coop::logErrLine("[coop-ui] menu launcher createDatapanel FAILED"); }
            return;
        }
        if (!uiPanelArmSeh(g, g_menuPanel))
            coop::logErrLine("[coop-ui] menu launcher arm FAILED");
        coop::logLine("[coop-ui] menu launcher shown");
    }
    if (!g_menuBuilt || status != g_menuLastStatus) {
        std::string title   = coop::tr("KenshiCoop  -  play together");
        std::string hostKey = "mhost";
        std::string hostCap = coop::tr("HOST GAME");
        std::string joinKey = "mjoin";
        std::string joinCap = coop::tr("JOIN GAME");
        std::string statKey = coop::tr("Status");
        std::string statVal = status.empty() ? std::string(coop::tr("Offline")) : status;
        std::string hintKey = coop::tr("Hint");
        std::string hintVal = coop::tr("Host: click, then load your save. Join: just click.");
        std::string empty   = "";
        MenuStrings ms;
        ms.title = &title;
        ms.hostKey = &hostKey; ms.hostCap = &hostCap;
        ms.joinKey = &joinKey; ms.joinCap = &joinCap;
        ms.statKey = &statKey; ms.statVal = &statVal;
        ms.hintKey = &hintKey; ms.hintVal = &hintVal;
        ms.empty = &empty;
        menuBuildSeh(g_menuPanel, &ms);
        if (g_menuHostBtn) g_menuHostBtn->callback = MyGUI::newDelegate(&onMenuHostBtn);
        if (g_menuJoinBtn) g_menuJoinBtn->callback = MyGUI::newDelegate(&onMenuJoinBtn);
        dbgColourSeh(g_menuStatLine, st->transferDetail != 0); // amber while streaming
        g_menuBuilt = true;
        g_menuLastStatus = status;
    }
}

} // namespace

void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect, CoopScanFn onScan,
                   CoopMenuActionFn onMenuAction, CoopAuthFn onAuth) {
    if (!st) return;
    g_authFn = onAuth;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // F2 rising edge toggles the panel open/closed.
    // 仅当游戏窗口在前台时响应（GetAsyncKeyState 是全局按键状态，同机双开时会同时触发两个窗口）
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    bool f2 = fgPid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !g_panel.f2Down) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_slotChoice = st->ownRank ? st->ownRank : 1;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            panelDestroySeh(g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_roleBtn = 0; g_transBtn = 0; g_connBtn = 0; g_slotBtn = 0;
            g_copyIdBtn = 0;
            g_pasteIdBtn = 0; g_scanBtn = 0;
            g_debugLine = 0; g_discLine = 0; g_peerLine = 0; g_selfLine = 0;
            g_fpsBtn = 0;
            g_authReqBtn = 0; g_authYesBtn = 0; g_authNoBtn = 0;
            g_authRevokeBtn = 0; g_authLine = 0;
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    // Title-screen launcher: native HOST/JOIN buttons on the main menu. A
    // press is the whole flow (one-click: no panel, no toggles) - the plugin
    // root hosts, or scans + auto-picks + auto-slots + connects. The launcher
    // status row narrates progress; F2 stays the advanced path.
    coopMenuLauncherTick(g, st);
    if (g_menuOpenReq != 0) {
        int req = g_menuOpenReq;
        g_menuOpenReq = 0;
        if (onMenuAction) onMenuAction(req > 0);
    }

    if (!g_panel.open) return;

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a peer-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the button caption
    // + debug line reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    std::string detail = st->detail ? std::string(st->detail) : std::string();
    if (detail != g_panel.lastStatus) g_panel.needsRebuild = true;

    // Join save-transfer line (throttled to whole-percent by the caller): rebuild
    // when it changes so the "Streaming host world... NN%" line advances live.
    std::string transfer = st->transferDetail ? std::string(st->transferDetail)
                                               : std::string();
    if (transfer != g_panel.lastTransfer) g_panel.needsRebuild = true;

    // 小队控制授权状态变化（第 3 批）：申请出现 / 消失、授权成立 / 收回，都要重建面板，
    // 让状态行与 [同意]/[拒绝]/[收回] 按钮即时出现或消失。
    if (st->authRequesting  != g_panel.lastAuthReq ||
        st->authRequestable != g_panel.lastAuthReqable ||
        st->authPendingPeer != g_panel.lastAuthPending ||
        st->authGrantedMine != g_panel.lastAuthGranted ||
        st->authBorrowed    != g_panel.lastAuthBorrowed) {
        g_panel.lastAuthReq     = st->authRequesting;
        g_panel.lastAuthReqable = st->authRequestable;
        g_panel.lastAuthPending = st->authPendingPeer;
        g_panel.lastAuthGranted = st->authGrantedMine;
        g_panel.lastAuthBorrowed = st->authBorrowed;
        g_panel.needsRebuild = true;
    }
    // Discovery-browser line: rebuild when the scan status / pick changes.
    std::string discDetail = st->discDetail ? std::string(st->discDetail)
                                            : std::string();
    if (discDetail != g_panel.lastDisc) g_panel.needsRebuild = true;

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        // Height covers the tallest variant: JOIN+UDP adds the squad-slot,
        // Scan and Found-hosts rows on top of the host layout. Args are
        // (TOP, LEFT, width, height) render-area fractions (see the menu
        // launcher note): top 0.20, left 0.30 - the classic center-left spot.
        g_panel.panel = g->createDatapanel(0.18f, 0.30f, 0.30f, 0.66f, false, layer, true);
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else if (!uiPanelArmSeh(g, g_panel.panel)) {
            coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
        }
    }

    // (Re)populate the rows when anything visible changed.
    if (g_panel.panel && (g_panel.needsRebuild || !g_panel.built)) {
        // 可见文字经 coop::tr 本地化; 整句作为格式串翻译, 以适应不同语言的语序 (fb 为格式化缓冲)
        char fb[256];
        fb[sizeof(fb) - 1] = '\0';
        std::string title    = coop::tr("Co-op Session    -    F2 to close");
        std::string roleKey  = "role";
        _snprintf(fb, sizeof(fb) - 1, coop::tr("Role: %s    (switch)"), coop::tr(g_panel.hostFlag ? "HOST" : "JOIN"));
        std::string roleCap  = fb;
        std::string transKey = "trans";
        _snprintf(fb, sizeof(fb) - 1, coop::tr("Transport: %s    (switch)"), g_panel.steamFlag ? "STEAM" : "UDP");
        std::string transCap = fb;
        std::string connKey  = "conn";
        _snprintf(fb, sizeof(fb) - 1, coop::tr("Connection: %s    (switch)"), coop::tr(g_panel.connectedFlag ? "ONLINE" : "OFFLINE"));
        std::string connCap  = fb;
        // JOIN only (protocol 49): the squad-tab slot this player claims.
        std::string slotKey  = "slot";
        std::string slotCap;
        {
            char sc[64];
            _snprintf(sc, sizeof(sc) - 1, coop::tr("Squad slot: %u    (switch)"),
                      g_slotChoice);
            sc[sizeof(sc) - 1] = '\0';
            slotCap = sc;
        }

        // JOIN + UDP: the tailnet/LAN game browser. The button doubles as
        // scan-and-cycle; the row shows the scan status / picked host.
        bool showScan = (!g_panel.hostFlag && !g_panel.steamFlag);
        std::string scanKey = "scan";
        std::string scanCap = discDetail.empty()
            ? coop::tr("Scan for hosts (Tailscale / LAN)")
            : coop::tr("Scan for hosts    (next / rescan)");
        std::string discKey = coop::tr("Found hosts");

        // White debug line: describes the live connection state + type. Reflects
        // the ACTUAL running session when online; the armed toggles when offline.
        const char* transStr = (st->transportSel == 0) ? "Steam" : "UDP";
        std::string dbgKey   = coop::tr("Connection status");
        if (st->running) {
            if (st->peerPresent)
                _snprintf(fb, sizeof(fb) - 1, coop::tr(st->isHost ? "Hosting over %s - peer connected"
                                                                   : "Joining over %s - peer connected"), transStr);
            else if (st->isHost)
                _snprintf(fb, sizeof(fb) - 1, coop::tr("Hosting over %s - waiting for peer..."), transStr);
            else
                _snprintf(fb, sizeof(fb) - 1, coop::tr("Joining over %s - connecting to host..."), transStr);
        } else {
            _snprintf(fb, sizeof(fb) - 1, coop::tr(g_panel.hostFlag ? "Offline - will host over %s on Connect"
                                                                    : "Offline - will join over %s on Connect"),
                      g_panel.steamFlag ? "Steam" : "UDP");
        }
        std::string dbgVal = fb;
        // A join streaming the host's world at the menu has no leader for the
        // screen overlay, so surface the live progress here instead (amber).
        if (!transfer.empty()) { dbgVal = transfer; dbgKey = coop::tr("World transfer"); }

        // Friend SteamID(s): prefer the values pasted in-panel this session;
        // fall back to the config (steamPeer, mainly for advanced/back-compat
        // use). A host shows every pasted id (up to MAX_PLAYERS - 1 friends).
        std::string peerKey = coop::tr(g_panel.hostFlag ? "Friend Steam IDs" : "Host's Steam ID");
        std::string peerVal;
        if (g_pastedCount > 0) {
            for (unsigned int i = 0; i < g_pastedCount; ++i) {
                if (i) peerVal += ", ";
                peerVal += coop::maskSteamId64(g_pastedPeers[i]);
            }
        } else if (st->peerSteamId != 0) {
            char pb[32];
            _snprintf(pb, sizeof(pb) - 1, "%llu", (unsigned long long)st->peerSteamId);
            pb[sizeof(pb) - 1] = '\0';
            peerVal = coop::maskSteamId64((unsigned long long)st->peerSteamId);
        } else if (g_pasteFailed) {
            peerVal = coop::tr("(clipboard was not a Steam ID - copy theirs and retry)");
        } else {
            peerVal = coop::tr("(click Paste friend's Steam ID)");
        }
        std::string pasteKey = "pasteid";
        std::string pasteCap = coop::tr("Paste friend's Steam ID");

        std::string selfKey  = coop::tr("Your Steam ID");
        std::string selfVal  = st->selfSteamId
                                   ? coop::maskSteamId64((unsigned long long)st->selfSteamId)
                                   : std::string(coop::tr("(Steam not running)"));
        std::string copyKey  = "copyid";
        std::string copyCap  = coop::tr("Copy my Steam ID");
        std::string fpsKey   = "fps";
        std::string fpsVal   = coop::fpslimit::current() > 0 ? std::to_string((long long)coop::fpslimit::current())
                                                             : std::string(coop::tr("Unlimited"));
        _snprintf(fb, sizeof(fb) - 1, coop::tr("FPS limit: %s    (switch)"), fpsVal.c_str());
        std::string fpsCap   = fb;
        // 小队控制授权（第 3 批）：状态行文字 + 按钮文案 + 条件显示标志
        std::string authKey = coop::tr("Squad control");
        std::string authVal;
        if (st->authPendingPeer)
            authVal = coop::tr("Your friend asks for control of your squad");
        else if (st->authRequesting)
            authVal = coop::tr("Waiting for your friend to answer...");
        else if (st->authGrantedMine)
            authVal = coop::tr("Your friend controls your squad");
        else if (st->authBorrowed)
            authVal = coop::tr("You control your friend's squad");
        else if (!st->authRequestable)
            authVal = coop::tr("No squad to hand over");
        else
            authVal = coop::tr("Each player controls their own squad");
        std::string authReqCap    = coop::tr("Ask to control your friend's squad");
        std::string authYesCap    = coop::tr("Grant control");
        std::string authNoCap     = coop::tr("Refuse");
        std::string authRevokeCap = st->authGrantedMine ? std::string(coop::tr("Take control back"))
                                                        : std::string(coop::tr("Give control back"));
        std::string authReqKey    = "authreq";
        std::string authYesKey    = "authyes";
        std::string authNoKey     = "authno";
        std::string authRevokeKey = "authrev";
        std::string empty    = "";

        PanelStrings ps;
        ps.title = &title; ps.roleKey = &roleKey; ps.roleCap = &roleCap;
        ps.transKey = &transKey; ps.transCap = &transCap;
        ps.connKey = &connKey; ps.connCap = &connCap;
        ps.slotKey = g_panel.hostFlag ? 0 : &slotKey;
        ps.slotCap = g_panel.hostFlag ? 0 : &slotCap;
        ps.scanKey = showScan ? &scanKey : 0;
        ps.scanCap = showScan ? &scanCap : 0;
        ps.discKey = (showScan && !discDetail.empty()) ? &discKey : 0;
        ps.discVal = (showScan && !discDetail.empty()) ? &discDetail : 0;
        ps.dbgKey = &dbgKey; ps.dbgVal = &dbgVal;
        ps.peerKey = &peerKey; ps.peerVal = &peerVal;
        ps.pasteKey = &pasteKey; ps.pasteCap = &pasteCap;
        ps.selfKey = &selfKey; ps.selfVal = &selfVal;
        ps.copyKey = &copyKey; ps.copyCap = &copyCap;
        ps.fpsKey = &fpsKey; ps.fpsCap = &fpsCap;
        ps.authKey = &authKey; ps.authVal = &authVal; ps.authReqCap = &authReqCap;
        ps.authYesCap = &authYesCap; ps.authNoCap = &authNoCap;
        ps.authRevokeCap = &authRevokeCap;
        ps.authReqKey = &authReqKey; ps.authYesKey = &authYesKey;
        ps.authNoKey = &authNoKey; ps.authRevokeKey = &authRevokeKey;
        ps.showAuthReq = (st->authRequestable && !st->authRequesting) ? 1 : 0;
        ps.showAuthYes = st->authPendingPeer ? 1 : 0;
        ps.showAuthNo  = st->authPendingPeer ? 1 : 0;
        ps.showAuthRevoke = (st->authGrantedMine || st->authBorrowed) ? 1 : 0;
        ps.empty = &empty;
        // 条件行会在重建中消失，先把上一轮的指针清掉，避免指向已释放的行
        g_authReqBtn = 0; g_authYesBtn = 0; g_authNoBtn = 0;
        g_authRevokeBtn = 0; g_authLine = 0;
        panelBuildSeh(g_panel.panel, &ps);

        // Delegate assignment + white-colouring live OUTSIDE the SEH frame (pointer
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        if (g_roleBtn)    g_roleBtn->callback    = MyGUI::newDelegate(&onRoleBtn);
        if (g_transBtn)   g_transBtn->callback   = MyGUI::newDelegate(&onTransBtn);
        if (g_connBtn)    g_connBtn->callback    = MyGUI::newDelegate(&onConnBtn);
        if (g_slotBtn)    g_slotBtn->callback    = MyGUI::newDelegate(&onSlotBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_pasteIdBtn) g_pasteIdBtn->callback = MyGUI::newDelegate(&onPasteIdBtn);
        if (g_fpsBtn)     g_fpsBtn->callback     = MyGUI::newDelegate(&onFpsBtn);
        if (g_scanBtn)    g_scanBtn->callback    = MyGUI::newDelegate(&onScanBtn);
        if (g_authReqBtn)    g_authReqBtn->callback    = MyGUI::newDelegate(&onAuthReqBtn);
        if (g_authYesBtn)    g_authYesBtn->callback    = MyGUI::newDelegate(&onAuthYesBtn);
        if (g_authNoBtn)     g_authNoBtn->callback     = MyGUI::newDelegate(&onAuthNoBtn);
        if (g_authRevokeBtn) g_authRevokeBtn->callback = MyGUI::newDelegate(&onAuthRevokeBtn);
        dbgColourSeh(g_debugLine, !transfer.empty()); // amber while streaming
        // Browser row: amber while a scan is in flight, white once it settled.
        dbgColourSeh(g_discLine, st->discBusy != 0);
        dbgColourSeh(g_peerLine, false);
        dbgColourSeh(g_selfLine, false);
        dbgColourSeh(g_authLine, false);

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastStatus = detail;
        g_panel.lastTransfer = transfer;
        g_panel.lastDisc = discDetail;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend ids (none = empty) are handed to the plugin, which lets a
    // non-empty list override the config steamPeer(s); the JOIN's squad-slot
    // choice rides along (protocol 49). UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s slot=%u",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp",
                      g_panel.hostFlag ? 0u : g_slotChoice);
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag,
                                     g_pastedPeers, g_pastedCount,
                                     g_panel.hostFlag ? 0u : g_slotChoice);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (onDisconnect) onDisconnect();
        }
    }

    // Scan button press -> plugin root (scan or step to the next found host).
    if (g_scanClicked) {
        g_scanClicked = false;
        if (onScan) onScan();
    }
}

// ---- Persistent co-op status overlay ----------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// Unlike the character-tracked ScreenLabel this replaces, it needs no player
// character and holds its place while the camera moves - which also makes it
// visible at the title screen, where a join has no leader while it streams the
// host's world. Removed when show=false.
//
// Two widgets, because neither factory alone does the job. Measured 2026-08-04:
// createFloatingLabel hands back a bare MyGUI::Window that IS visible and
// layer-attached at the coords we ask for, but its skin carries no text region at
// all - setCaption is silently dropped (getCaption().size() stays 0) and it has no
// children, so it draws nothing. It is still the only way to get a widget parented
// to a screen layer instead of to another window, so we keep it as an invisible
// container and put Kenshi's own label factory inside it (createLabelAbs ->
// MyGUI::TextBox with a text-bearing skin, the one the datapanel rows use).
// Caption + colour go to the child; the container is only geometry.

namespace {
// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox   = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay      = 0; // the label that actually draws the text
int             g_overlayState = -1;
std::string     g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// dbgColourSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(MyGUI::Window* box, const std::string* text) {
    __try {
        box->setCoord(kOverlayX, kOverlayY, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = ::gui->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
                                                  *text, MyGUI::Align::Left);
        if (l) {
            l->setTextAlign(MyGUI::Align::Left);
            l->setVisible(true);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Caption + colour in place. MyGUI::UString owns a buffer (destructor => C2712),
// so the caller builds it outside this frame and passes a pointer. false = the
// widget faulted; the caller then treats the pointer as dead.
bool overlayUpdateSeh(MyGUI::TextBox* l, const MyGUI::UString* text,
                      const MyGUI::Colour* col) {
    __try {
        l->setCaption(*text);
        l->setTextColour(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Destroying the container takes its label child with it (MyGUI owns the subtree).
void overlayDestroySeh(ForgottenGUI* g, MyGUI::Window* box) {
    __try { g->destroyWidget(box); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

void coopOverlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel above; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); g_overlayBox = 0; }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            coop::logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g_overlayBox, &t);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (!g_overlay) {
            coop::logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently (same lesson as the ScreenLabel registry
            // note above). Forget them and re-mint on the next tick.
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
    }
}

// 任意物品栏/交易窗口打开时返回 true（上游 PR #28）。调用方据此推迟物品栏修改，
// 避免释放 UI 仍持有的 Item（Inventory::removeItem 释放后使用，进城/商店时崩溃）
bool inventoryUiOpen() {
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    __try {
        return g->getNumOpenInventoryWindows() > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace engine
} // namespace coop
