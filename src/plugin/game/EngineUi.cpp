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
template<typename T> class ReorderableListItem;
// VC100 不支持此头文件中依赖模板基类的析构 override，局部屏蔽后立即恢复。
#define override
#include <kenshi/gui/ManagementScreen.h>
#undef override
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>

#include "../net/SteamInvite.h"
#include "../net/SteamP2P.h" // 房间列表延迟
#include "../net/NetLink.h"
#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)
#include "../core/Config.h"  // coop::tr (UI text localization)
#include "../core/FpsLimit.h" // 帧数上限切换按钮
#include "../core/Update.h"   // 自动更新状态行 + 检查/粘贴/清除代理三个按钮
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
DataPanelLine_Button*   g_playerBoxBtn = 0; // 玩家框显示/隐藏（备用开关）
bool                    g_playerBoxesHidden = false; // 玩家框隐藏设置，由玩家框驱动负责读写设置文件
bool                    g_lastPlayerBoxesHidden = false;
// 自动更新（交接文档第五节）：状态行 + 代理行 + 三个按钮。状态由 core/Update 的后台
// 线程维护，面板每帧只读快照；g_updateLastUi 是重建闸门（状态文字变化就重建面板）。
DataPanelLine*          g_updateLine      = 0;
DataPanelLine*          g_updateProxyLine = 0;
DataPanelLine_Button*   g_checkUpdateBtn  = 0;
DataPanelLine_Button*   g_pasteProxyBtn   = 0;
DataPanelLine_Button*   g_clearProxyBtn   = 0;
std::string             g_updateStatusText;      // 状态行文字（重建时用）
std::string             g_updateProxyText;       // 代理行文字（重建时用）
std::string             g_updateLastUi;          // 上次显示的状态+代理文字（重建闸门）
int                     g_updateNoticeCode = 0;  // 粘贴/清除代理失败的原因（0 = 无）
DWORD                   g_updateNoticeUntil = 0; // 提示显示到什么时候（5 秒）
DataPanelLine_Button*   g_authReqBtn   = 0; // 申请控制好友的小队（第 3 批）
DataPanelLine_Button*   g_authYesBtn   = 0; // 同意对方申请
DataPanelLine_Button*   g_authNoBtn    = 0; // 拒绝对方申请
DataPanelLine_Button*   g_authRevokeBtn = 0; // 收回控制权
DataPanelLine*          g_authLine     = 0; // 授权状态行
// 面板把授权动作交回插件（= coopPanelTick 的 onAuth 参数；0 表示未接线）
CoopCharacterFn g_characterFn = 0;
DataPanelLine_Button* g_characterTemplateBtn = 0;
DataPanelLine_Button* g_characterCreateBtn = 0;
std::string g_lastCharacterUi;
bool g_lastCharacterEditing = false;
void onCharacterTemplateBtn(DataPanelLine*);
void onCharacterCreateBtn(DataPanelLine*);
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
bool g_slotManual = false;
unsigned long long g_roomTarget = 0;
std::string g_lastRoomUi;
DataPanelLine* g_roomLine = 0;
const int ROOM_ROWS = 3;
DataPanelLine_Button* g_roomButtons[ROOM_ROWS] = { 0 };
unsigned long long g_roomButtonIds[ROOM_ROWS] = { 0 }, g_clickedRoomId = 0;
DataPanelLine_Button* g_roomPageButton = 0;
int g_roomPage = 0;
std::string g_lastFriendRooms;
void onRoomButton(DataPanelLine* line);
void onRoomPageButton(DataPanelLine* line);
DataPanelLine_Button* g_roomRefreshButton = 0;
bool g_roomRefreshClicked = false;
std::string g_lastOwnRoom;
void onRoomRefreshButton(DataPanelLine* line);

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    // 在线时角色与连接方式不可切换（切换只在下次上线生效，会与实际会话不符）
    if (g_panel.connectedFlag) { coop::logLine("[coop-ui] role locked while online"); return; }
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    if (g_panel.connectedFlag) { coop::logLine("[coop-ui] transport locked while online"); return; }
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
    g_slotManual = true;
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
        if (!g_panel.hostFlag) { g_pastedCount = 0; g_slotManual = false; g_roomTarget = 0; }
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
void onPlayerBoxBtn(DataPanelLine*) {
    if (!g_panel.lastConnected) return;
    g_playerBoxesHidden = !g_playerBoxesHidden;
    g_panel.needsRebuild = true;
    coop::logLine(g_playerBoxesHidden ? "[playerbox] hidden from F2 panel" : "[playerbox] shown from F2 panel");
}

// 自动更新按钮（交接文档第五节）。状态与后台线程都在 core/Update，“检查更新”在
// 正在检查/下载或已更新待重启时点了没有效果。
void onCheckUpdateBtn(DataPanelLine*) {
    coop::update::Status st;
    coop::update::getStatus(st);
    if (st.stage == coop::update::STAGE_CHECKING ||
        st.stage == coop::update::STAGE_DOWNLOADING ||
        st.stage == coop::update::STAGE_UPDATED) {
        coop::logLine("[coop-ui] update check ignored (busy or restart pending)");
        return;
    }
    coop::logLine("[coop-ui] update check pressed");
    coop::update::checkNow();
    g_panel.needsRebuild = true;
}
// “粘贴代理地址”：面板不能打字（MyGUI 输入框在游戏里拿不到键盘焦点），所以照
// “粘贴好友 Steam ID”的做法读剪贴板。校验通过就写回 coop_config.json 并立即生效；
// 不通过就在状态行显示原因（5 秒），配置不变。
void onPasteProxyBtn(DataPanelLine*) {
    std::string clip;
    int result = clipboardGetText(clip) ? coop::update::applyProxyText(clip) : 1;
    if (result == 0) {
        g_updateNoticeCode = 0;
        coop::logLine("[coop-ui] update proxy pasted ok=1");
    } else {
        g_updateNoticeCode = result;
        g_updateNoticeUntil = GetTickCount() + 5000;
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] update proxy pasted ok=0 code=%d", result);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
    g_panel.needsRebuild = true;
}
// “清除代理”：写回空值，恢复用系统代理。
void onClearProxyBtn(DataPanelLine*) {
    int result = coop::update::clearProxy();
    if (result == 0) {
        g_updateNoticeCode = 0;
        coop::logLine("[coop-ui] update proxy cleared");
    } else {
        g_updateNoticeCode = result;
        g_updateNoticeUntil = GetTickCount() + 5000;
        coop::logLine("[coop-ui] update proxy clear FAILED");
    }
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
    const std::string *roomKey, *roomVal;
    const std::string *roomsKey, *roomsVal, *roomButtonKeys, *roomButtonCaps, *roomPageKey, *roomPageCap;
    const std::string *ownRoomKey, *ownRoomVal, *roomRefreshKey, *roomRefreshCap; // ownRoomKey 为空 = 无本机房间行
    int roomButtonCount;
    const std::string *scanKey, *scanCap;   // null = no Scan button (HOST/Steam)
    const std::string *discKey, *discVal;   // null = no browser row (nothing to show)
    const std::string *dbgKey, *dbgVal;
    const std::string *peerKey, *peerVal, *pasteKey, *pasteCap;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *fpsKey, *fpsCap;
    const std::string *playerBoxKey, *playerBoxCap;
    // 自动更新：状态行 + 下载代理行 + 三个按钮（文案全部走语言文件）
    const std::string *updateKey, *updateVal, *updateProxyKey, *updateProxyVal;
    const std::string *checkUpdateKey, *checkUpdateCap;
    const std::string *pasteProxyKey, *pasteProxyCap;
    const std::string *clearProxyKey, *clearProxyCap;
    const std::string *authKey, *authVal, *authReqCap;
    const std::string *authYesCap, *authNoCap, *authRevokeCap;
    const std::string *authReqKey, *authYesKey, *authNoKey, *authRevokeKey;
    const std::string *characterTemplateKey, *characterTemplateCap, *characterCreateKey, *characterCreateCap;
    const std::string *characterStatusKey, *characterStatus;
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
        g_roomLine = 0;
        if (s->roomKey && s->roomVal) g_roomLine = p->setLine(*s->roomKey, *s->roomVal, *s->empty, 0, false, true);
        // JOIN + UDP: the tailnet/LAN game browser (scan + picked-host row).
        g_scanBtn = 0;
        if (s->scanKey && s->scanCap)
            g_scanBtn = p->setLineButton(*s->scanKey, *s->scanCap, 0);
        g_discLine = 0;
        if (s->discKey && s->discVal)
            g_discLine = p->setLine(*s->discKey, *s->discVal, *s->empty, 0, false, true);
        for (int i = 0; i < ROOM_ROWS; ++i) g_roomButtons[i] = 0;
        g_roomPageButton = 0;
        g_roomRefreshButton = 0;
        if (s->roomsKey) {
            p->setLine(*s->roomsKey, *s->roomsVal, *s->empty, 0, false, true);
            if (s->ownRoomKey) p->setLine(*s->ownRoomKey, *s->ownRoomVal, *s->empty, 0, false, true);
            for (int i = 0; i < s->roomButtonCount; ++i)
                g_roomButtons[i] = p->setLineButton(s->roomButtonKeys[i], s->roomButtonCaps[i], 0);
            if (s->roomPageKey) g_roomPageButton = p->setLineButton(*s->roomPageKey, *s->roomPageCap, 0);
            g_roomRefreshButton = p->setLineButton(*s->roomRefreshKey, *s->roomRefreshCap, 0);
        }
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
        g_playerBoxBtn = p->setLineButton(*s->playerBoxKey, *s->playerBoxCap, 0);
        p->addSpace(0, 0.35f);
        // 自动更新（交接文档第五节）：状态行 + 下载代理行 + 检查/粘贴/清除三个按钮
        g_updateLine = p->setLine(*s->updateKey, *s->updateVal, *s->empty, 0, false, true);
        g_updateProxyLine = p->setLine(*s->updateProxyKey, *s->updateProxyVal, *s->empty, 0, false, true);
        g_checkUpdateBtn  = p->setLineButton(*s->checkUpdateKey, *s->checkUpdateCap, 0);
        g_pasteProxyBtn   = p->setLineButton(*s->pasteProxyKey, *s->pasteProxyCap, 0);
        g_clearProxyBtn   = p->setLineButton(*s->clearProxyKey, *s->clearProxyCap, 0);
        p->addSpace(0, 0.35f);
        // 小队控制授权（第 3 批）：状态行 + 申请按钮（+ 条件出现的同意/拒绝/收回）
        g_authLine   = p->setLine(*s->authKey, *s->authVal, *s->empty, 0, false, true);
        if (s->showAuthReq)    g_authReqBtn    = p->setLineButton(*s->authReqKey, *s->authReqCap, 0);
        if (s->showAuthYes)    g_authYesBtn    = p->setLineButton(*s->authYesKey, *s->authYesCap, 0);
        if (s->showAuthNo)     g_authNoBtn     = p->setLineButton(*s->authNoKey,  *s->authNoCap, 0);
        if (s->showAuthRevoke) g_authRevokeBtn = p->setLineButton(*s->authRevokeKey, *s->authRevokeCap, 0);
        g_characterTemplateBtn = p->setLineButton(*s->characterTemplateKey, *s->characterTemplateCap, 0);
        g_characterCreateBtn = p->setLineButton(*s->characterCreateKey, *s->characterCreateCap, 0);
        p->setLine(*s->characterStatusKey, *s->characterStatus, *s->empty, 0, false, true);
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
                   CoopMenuActionFn onMenuAction, CoopAuthFn onAuth, CoopCharacterFn onCharacter) {
    if (!st) return;
    g_authFn = onAuth;
    g_characterFn = onCharacter;
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

    // 在主线程的下一帧隐藏面板，避免销毁仍在执行回调的按钮。
    if (st->editingCharacter && !g_lastCharacterEditing && g_panel.open) {
        panelDestroySeh(g, g_panel.panel); g_panel.panel = 0;
        g_panel.open = false; g_panel.built = false;
    }
    g_lastCharacterEditing = st->editingCharacter;
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
            g_fpsBtn = 0; g_playerBoxBtn = 0;
            g_updateLine = 0; g_updateProxyLine = 0;
            g_checkUpdateBtn = 0; g_pasteProxyBtn = 0; g_clearProxyBtn = 0;
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

    // 房间列表：Steam 可用时所有人可见；离线时点击房间才连接（自动切到“加入 + STEAM”）
    bool showFriendRooms = g_panel.open && !g_panel.hostFlag && g_panel.steamFlag && !st->running;
    bool showRooms = g_panel.open && st->selfSteamId != 0;
    coop::steaminvite::browseRooms(showRooms);
    if (!g_panel.open) return;
    if (g_roomRefreshClicked) { g_roomRefreshClicked = false; coop::steaminvite::refreshRooms(); g_panel.needsRebuild = true; }
    if (g_clickedRoomId && !st->running && !showFriendRooms) {
        // 离线时点击好友房间即表示加入：自动切到“加入 + STEAM”
        g_panel.hostFlag = false; g_panel.steamFlag = true; showFriendRooms = true;
        coop::logLine("[coop-ui] room click: switching to JOIN over Steam");
    }
    if (g_clickedRoomId && !showFriendRooms) {
        g_clickedRoomId = 0;
        coop::logLine("[coop-ui] room click ignored (go offline to join another room)");
    }
    if (g_clickedRoomId) {
        unsigned long long clicked = g_clickedRoomId; g_clickedRoomId = 0;
        const coop::room::Info* found = 0;
        for (int i = 0; i < coop::steaminvite::roomCount(); ++i) {
            const coop::room::Info* candidate = coop::steaminvite::friendRoom(i);
            if (candidate && candidate->host == clicked) { found = candidate; break; }
        }
        coop::steaminvite::watchRoom(clicked);
        if (!found) coop::setNetUiError("Room is no longer available - refresh and try again");
        else {
            g_pastedPeers[0] = clicked; g_pastedCount = 1; g_slotManual = false; g_roomTarget = 0;
            coop::saveLastPeer(clicked);
            if (found->protocol != coop::PROTOCOL_VERSION)
                coop::setNetUiError("Version mismatch with host - update both to the same KenshiCoop build");
            else {
                unsigned int freeSlot = coop::room::firstFreeSlot(found->slots, found->limit);
                if (freeSlot) g_slotChoice = freeSlot;
                g_panel.connectedFlag = true;
            }
        }
        g_panel.needsRebuild = true;
    }
    int friendRooms = showRooms ? coop::steaminvite::roomCount() : 0;
    int roomPages = (friendRooms + ROOM_ROWS - 1) / ROOM_ROWS;
    if (!roomPages || g_roomPage >= roomPages) g_roomPage = 0;
    std::string roomButtonKeys[ROOM_ROWS], roomButtonCaps[ROOM_ROWS];
    int roomButtonCount = 0;
    std::string friendRoomUi;
    for (int i = g_roomPage * ROOM_ROWS; i < friendRooms && roomButtonCount < ROOM_ROWS; ++i) {
        const coop::room::Info* info = coop::steaminvite::friendRoom(i);
        if (!info) continue;
        std::string name = coop::room::displayText(info->name.c_str(), 36);
        if (name.size() < info->name.size()) name += "...";
        // 从左至右：延迟、用户（名字 + 打码 Steam ID）、已加入/可加入
        int ping = coop::steamp2p::roomPing(info->host);
        char pingText[64];
        if (ping >= 0) _snprintf(pingText, sizeof(pingText)-1, "%d ms", ping);
        else _snprintf(pingText, sizeof(pingText)-1, "%s", coop::tr(ping == -1 ? "Measuring..." : "No response"));
        pingText[sizeof(pingText)-1] = 0;
        char line[512];
        _snprintf(line, sizeof(line)-1, "%s    %s(%s)    %u/%u%s%s", pingText, name.c_str(),
            coop::maskSteamId64(info->host).c_str(), info->players, info->limit,
            info->protocol == coop::PROTOCOL_VERSION ? "" : "    ",
            info->protocol == coop::PROTOCOL_VERSION ? "" : coop::tr("Version mismatch"));
        line[sizeof(line)-1] = 0;
        roomButtonKeys[roomButtonCount] = "friendroom" + std::to_string((long long)roomButtonCount);
        roomButtonCaps[roomButtonCount] = line;
        g_roomButtonIds[roomButtonCount] = info->host;
        friendRoomUi += std::to_string(info->host) + line;
        ++roomButtonCount;
    }
    friendRoomUi += std::to_string((long long)roomPages);
    if (friendRoomUi != g_lastFriendRooms) { g_lastFriendRooms = friendRoomUi; g_panel.needsRebuild = true; }
    // 本机房间：Steam 主持时读回自己已发布的大厅数据，UDP 主持时说明好友看不到
    std::string ownRoomUi;
    if (showRooms && st->running && st->isHost) {
        if (st->transportSel != 0) ownRoomUi = coop::tr("Hosting over UDP - Steam friends cannot see this room");
        else {
            const coop::room::Info& own = coop::steaminvite::ownRoom();
            if (own.state == coop::room::AVAILABLE) {
                std::string name = coop::room::displayText(own.name.c_str(), 36);
                char line[512];
                _snprintf(line, sizeof(line)-1, "%s    %s(%s)    %u/%u", coop::tr("Local"), name.c_str(),
                    coop::maskSteamId64(own.host).c_str(), own.players, own.limit);
                line[sizeof(line)-1] = 0;
                ownRoomUi = line;
            } else if (own.state == coop::room::QUERYING) ownRoomUi = coop::tr("Creating your room...");
            else ownRoomUi = coop::tr("Room data invalid - switch offline and online to recreate");
        }
    }
    if (ownRoomUi != g_lastOwnRoom) { g_lastOwnRoom = ownRoomUi; g_panel.needsRebuild = true; }
    std::string characterUi = st->characterTemplate ? st->characterTemplate : "";
    characterUi += st->characterStatus ? st->characterStatus : "";
    characterUi += st->editingCharacter ? "E" : st->canCreateCharacter ? "Y" : "N";
    if (characterUi != g_lastCharacterUi) { g_panel.needsRebuild = true; g_lastCharacterUi = characterUi; }

    std::string roomDetail;
    if (!g_panel.hostFlag && g_panel.steamFlag) {
        unsigned long long target = st->running ? st->peerSteamId : g_pastedCount ? g_pastedPeers[0] : st->peerSteamId;
        if (g_roomTarget != target) { g_roomTarget = target; g_slotManual = false; }
        coop::steaminvite::watchRoom(target);
        const coop::room::Info& info = coop::steaminvite::watchedRoom();
        // 已连接时房间里占的槽位含自己，不自动改选也不提示占用
        if (!st->running && !g_slotManual && info.state == coop::room::AVAILABLE) {
            unsigned int freeSlot = coop::room::firstFreeSlot(info.slots, info.limit);
            if (freeSlot && g_slotChoice != freeSlot) { g_slotChoice = freeSlot; g_panel.needsRebuild = true; }
        }
        if (!info.host) roomDetail = coop::tr("Paste a host Steam ID to look up their room");
        else if (info.state == coop::room::QUERYING) roomDetail = coop::tr("Looking up the host's room...");
        else if (info.state == coop::room::NOT_PLAYING) roomDetail = coop::tr("The host is not playing Kenshi");
        else if (info.state == coop::room::NO_ROOM) roomDetail = coop::tr("The host has not opened a room");
        else if (info.state != coop::room::AVAILABLE) roomDetail = coop::tr("Room not found - you can still try connecting");
        else {
            char text[1024];
            _snprintf(text, sizeof(text)-1, coop::tr("%s's room: %s, %u/%u players, %s"), info.name.c_str(),
                info.save.empty() ? coop::tr("No save yet") : info.save.c_str(), info.players, info.limit,
                coop::tr(info.protocol == coop::PROTOCOL_VERSION ? "Version matches" : "Version mismatch - update both to the same co-op build"));
            text[sizeof(text)-1] = 0;
            roomDetail = text;
            roomDetail += coop::tr(info.world ? " - in world" : " - at menu / loading");
        }
        if (!st->running && info.state == coop::room::AVAILABLE && (info.slots & (1u << g_slotChoice)))
            roomDetail += coop::tr(" - selected squad slot is occupied");
    }
    if (roomDetail != g_lastRoomUi) { g_lastRoomUi = roomDetail; g_panel.needsRebuild = true; }

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

    if (!st->running && g_panel.connectedFlag && g_panel.lastChkVal) {
        g_panel.connectedFlag = false; g_panel.lastChkVal = false; g_panel.needsRebuild = true;
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
    if (g_playerBoxesHidden != g_lastPlayerBoxesHidden) { g_lastPlayerBoxesHidden = g_playerBoxesHidden; g_panel.needsRebuild = true; }

    // 自动更新（交接文档第五节）：把 core/Update 的后台线程状态拼成两行面板文字；
    // 文字（含 5 秒内的粘贴/清除代理失败提示）一变就重建面板。
    {
        coop::update::Status us;
        coop::update::getStatus(us);
        const bool notice = g_updateNoticeCode != 0 && GetTickCount() < g_updateNoticeUntil;
        std::string status;
        if (notice) {
            status = coop::tr(g_updateNoticeCode == 1 ? "Clipboard is empty" :
                              g_updateNoticeCode == 2 ? "Only HTTP proxies are supported (mixed ports work too)" :
                              g_updateNoticeCode == 4 ? "Could not write coop_config.json" :
                                                        "Not a proxy address - use host:port");
        } else if (us.stage == coop::update::STAGE_CHECKING) {
            status = coop::tr("Checking for updates...");
        } else if (us.stage == coop::update::STAGE_UP_TO_DATE) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, coop::tr("Up to date (%s)"), us.latestBuild);
            b[sizeof(b) - 1] = '\0';
            status = b;
        } else if (us.stage == coop::update::STAGE_DOWNLOADING) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, coop::tr("Downloading %u/%u"),
                      (unsigned)us.fileIndex, (unsigned)us.fileCount);
            b[sizeof(b) - 1] = '\0';
            status = b;
        } else if (us.stage == coop::update::STAGE_UPDATED) {
            char b[192];
            _snprintf(b, sizeof(b) - 1, coop::tr("Updated to %s (%s) - restart the game to apply"),
                      us.latestBuild, us.latestCommit);
            b[sizeof(b) - 1] = '\0';
            status = b;
            // 远端协议号与本机不同：多提示一句所有玩家都要更新（交接文档 4.3.7）
            if (us.protocolChanged)
                status += coop::tr(" - the co-op protocol changed, all players must update");
        } else if (us.stage == coop::update::STAGE_DEV_BUILD) {
            status = coop::tr("Development build - auto-update disabled");
        } else if (us.stage == coop::update::STAGE_FAILED) {
            // 面板按失败环节给中文说法（交接文档 4.3.1 的“校验失败”等），后面带错误码
            std::string failedText;
            if (us.failedStage == 2) failedText = coop::tr("Checksum failed");
            else if (us.failedStage == 1) failedText = coop::tr("Download failed");
            else if (us.failedStage == 3) failedText = coop::tr("Replace failed");
            else failedText = coop::tr("Check failed");
            char b[192];
            _snprintf(b, sizeof(b) - 1, coop::tr("Update failed: %s (%s)"),
                      failedText.c_str(), us.reason);
            b[sizeof(b) - 1] = '\0';
            status = b;
        } else {
            status = coop::tr("Not checked");
        }
        std::string proxy = coop::update::proxyText();
        std::string proxyUi;
        if (proxy.empty()) proxyUi = coop::tr("system proxy");
        else proxyUi = proxy;
        // 闸门里额外带上“提示是否还在显示”，5 秒到期时也会重建一次。
        const std::string uiKey = status + "\n" + proxyUi + (notice ? "\nnotice" : "\n-");
        if (uiKey != g_updateLastUi) { g_updateLastUi = uiKey; g_panel.needsRebuild = true; }
        g_updateStatusText = status;
        g_updateProxyText  = proxyUi;
    }

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
        g_panel.panel = g->createDatapanel(0.06f, 0.25f, 0.45f, 0.88f, false, layer, true);
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
        _snprintf(fb, sizeof(fb) - 1, coop::tr(g_panel.connectedFlag ? "Role: %s    (go offline to switch)" : "Role: %s    (switch)"), coop::tr(g_panel.hostFlag ? "HOST" : "JOIN"));
        std::string roleCap  = fb;
        std::string transKey = "trans";
        _snprintf(fb, sizeof(fb) - 1, coop::tr(g_panel.connectedFlag ? "Transport: %s    (go offline to switch)" : "Transport: %s    (switch)"), g_panel.steamFlag ? "STEAM" : "UDP");
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
        if (st->detail && *st->detail) dbgVal = st->detail;
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
            peerVal = g_panel.hostFlag ? coop::tr("Friends can join directly; paste IDs for extra access") :
                coop::tr("(click Paste friend's Steam ID)");
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
        std::string playerBoxKey = "playerboxes";
        std::string playerBoxCap = coop::tr(!st->running ? "Player boxes: available when online"
                                            : g_playerBoxesHidden ? "Player boxes: hidden    (click to show)"
                                                                  : "Player boxes: shown    (click to hide)");
        // 自动更新（交接文档第五节）：状态行 + 代理行 + 三个按钮
        std::string updateKey       = coop::tr("Update");
        std::string updateProxyKey  = coop::tr("Download proxy");
        std::string checkUpdateKey  = "updatecheck",  checkUpdateCap  = coop::tr("Check for updates");
        std::string pasteProxyKey   = "updateproxy",  pasteProxyCap   = coop::tr("Paste proxy address");
        std::string clearProxyKey   = "updateclearproxy", clearProxyCap = coop::tr("Clear proxy");
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

        std::string characterTemplateKey = "chartmpl", characterCreateKey = "charcreate";
        std::string characterTemplateCap = coop::tr("Game start (click to cycle): ");
        if (st->characterTemplate) characterTemplateCap += st->characterTemplate;
        std::string characterCreateCap = st->editingCharacter ? coop::tr("Cancel character creation") :
            st->canCreateCharacter ? coop::tr("Start the selected game start") : coop::tr("Start the selected game start (unavailable)");
        std::string characterStatusKey = coop::tr("Character creation");
        std::string characterStatus = st->characterStatus ? st->characterStatus : "";
        std::string roomKey = coop::tr("Room info");
        std::string roomsKey = coop::tr(!st->running ? "Rooms - click to connect" : "Rooms");
        std::string roomsVal = (friendRooms || !ownRoomUi.empty()) ? coop::tr("Latency    Player    Joined/Max") :
            showFriendRooms ? coop::tr("No friend rooms found - you can paste a host ID") : coop::tr("No rooms found");
        std::string ownRoomKey = coop::tr("My room");
        std::string roomRefreshKey = "roomrefresh", roomRefreshCap = coop::tr("Refresh rooms");
        std::string roomPageKey = "roompage";
        char pageCaption[128];
        _snprintf(pageCaption, sizeof(pageCaption)-1, coop::tr("More rooms (%u/%u)"), (unsigned)g_roomPage+1, (unsigned)roomPages);
        pageCaption[sizeof(pageCaption)-1] = 0;
        std::string roomPageCap = pageCaption;
        PanelStrings ps;
        ps.roomsKey = showRooms ? &roomsKey : 0; ps.roomsVal = &roomsVal;
        ps.ownRoomKey = ownRoomUi.empty() ? 0 : &ownRoomKey; ps.ownRoomVal = &ownRoomUi;
        ps.roomRefreshKey = &roomRefreshKey; ps.roomRefreshCap = &roomRefreshCap;
        ps.roomButtonKeys = roomButtonKeys; ps.roomButtonCaps = roomButtonCaps;
        ps.roomButtonCount = roomButtonCount;
        ps.roomPageKey = showRooms && roomPages > 1 ? &roomPageKey : 0; ps.roomPageCap = &roomPageCap;
        ps.roomKey = roomDetail.empty() ? 0 : &roomKey; ps.roomVal = roomDetail.empty() ? 0 : &roomDetail;
        ps.characterTemplateKey = &characterTemplateKey; ps.characterTemplateCap = &characterTemplateCap;
        ps.characterCreateKey = &characterCreateKey; ps.characterCreateCap = &characterCreateCap;
        ps.characterStatusKey = &characterStatusKey; ps.characterStatus = &characterStatus;
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
        ps.playerBoxKey = &playerBoxKey; ps.playerBoxCap = &playerBoxCap;
        ps.updateKey = &updateKey; ps.updateVal = &g_updateStatusText;
        ps.updateProxyKey = &updateProxyKey; ps.updateProxyVal = &g_updateProxyText;
        ps.checkUpdateKey = &checkUpdateKey; ps.checkUpdateCap = &checkUpdateCap;
        ps.pasteProxyKey = &pasteProxyKey; ps.pasteProxyCap = &pasteProxyCap;
        ps.clearProxyKey = &clearProxyKey; ps.clearProxyCap = &clearProxyCap;
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
        for (int i = 0; i < ROOM_ROWS; ++i) if (g_roomButtons[i]) g_roomButtons[i]->callback = MyGUI::newDelegate(&onRoomButton);
        if (g_roomPageButton) g_roomPageButton->callback = MyGUI::newDelegate(&onRoomPageButton);
        if (g_roomRefreshButton) g_roomRefreshButton->callback = MyGUI::newDelegate(&onRoomRefreshButton);
        if (g_slotBtn)    g_slotBtn->callback    = MyGUI::newDelegate(&onSlotBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_pasteIdBtn) g_pasteIdBtn->callback = MyGUI::newDelegate(&onPasteIdBtn);
        if (g_fpsBtn)     g_fpsBtn->callback     = MyGUI::newDelegate(&onFpsBtn);
        if (g_playerBoxBtn) g_playerBoxBtn->callback = MyGUI::newDelegate(&onPlayerBoxBtn);
        if (g_checkUpdateBtn) g_checkUpdateBtn->callback = MyGUI::newDelegate(&onCheckUpdateBtn);
        if (g_pasteProxyBtn)  g_pasteProxyBtn->callback  = MyGUI::newDelegate(&onPasteProxyBtn);
        if (g_clearProxyBtn)  g_clearProxyBtn->callback  = MyGUI::newDelegate(&onClearProxyBtn);
        if (g_characterTemplateBtn) g_characterTemplateBtn->callback = MyGUI::newDelegate(&onCharacterTemplateBtn);
        if (g_characterCreateBtn) g_characterCreateBtn->callback = MyGUI::newDelegate(&onCharacterCreateBtn);
        if (g_scanBtn)    g_scanBtn->callback    = MyGUI::newDelegate(&onScanBtn);
        if (g_authReqBtn)    g_authReqBtn->callback    = MyGUI::newDelegate(&onAuthReqBtn);
        if (g_authYesBtn)    g_authYesBtn->callback    = MyGUI::newDelegate(&onAuthYesBtn);
        if (g_authNoBtn)     g_authNoBtn->callback     = MyGUI::newDelegate(&onAuthNoBtn);
        if (g_authRevokeBtn) g_authRevokeBtn->callback = MyGUI::newDelegate(&onAuthRevokeBtn);
        dbgColourSeh(g_debugLine, !transfer.empty()); // amber while streaming
        // Browser row: amber while a scan is in flight, white once it settled.
        dbgColourSeh(g_discLine, st->discBusy != 0);
        dbgColourSeh(g_roomLine, false);
        dbgColourSeh(g_peerLine, false);
        dbgColourSeh(g_selfLine, false);
        dbgColourSeh(g_authLine, false);
        dbgColourSeh(g_updateLine, false);
        dbgColourSeh(g_updateProxyLine, false);

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
            const coop::room::Info& room = coop::steaminvite::watchedRoom();
            if (!g_panel.hostFlag && g_panel.steamFlag && room.state == coop::room::AVAILABLE && room.protocol != coop::PROTOCOL_VERSION) {
                coop::setNetUiError("Version mismatch with host - update both to the same KenshiCoop build");
                g_panel.connectedFlag = false; g_panel.lastChkVal = false; g_panel.needsRebuild = true;
            } else if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag,
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

namespace coop { namespace engine { namespace {
void onCharacterTemplateBtn(DataPanelLine*) { if (g_characterFn) g_characterFn(0); }
void onCharacterCreateBtn(DataPanelLine*) { if (g_characterFn) g_characterFn(1); }
} } }

namespace coop { namespace engine {

namespace {
void onRoomButton(DataPanelLine* line) {
    for (int i = 0; i < ROOM_ROWS; ++i) if (g_roomButtons[i] == line) {
        g_clickedRoomId = g_roomButtonIds[i]; break;
    }
}

void onRoomPageButton(DataPanelLine*) {
    ++g_roomPage; g_panel.needsRebuild = true;
}

void onRoomRefreshButton(DataPanelLine*) {
    g_roomRefreshClicked = true;
    coop::logLine("[coop-ui] refresh rooms pressed");
}
} // namespace
} } // namespace coop::engine


namespace coop { namespace engine { namespace {

struct PlayerBoxTabRead {
    unsigned int c, cs;
    bool loaded;
    char name[256];
};

unsigned int playerBoxTabsSeh(GameWorld* gw, PlayerBoxTabRead* out, unsigned int cap, unsigned int* members) {
    unsigned int count = 0;
    __try {
        if (!gw || !gw->player) return 0;
        *members = gw->player->playerCharacters.size();
        if (*members > 8192) *members = 0;
        Faction* f = gw->player->getFaction();
        if (!f) return 0;
        for (int list = 0; list < 2; ++list) {
            const lektor<Platoon*>* tabs = list ? &f->unloadedPlatoons : &f->activePlatoons;
            if (tabs->size() > 8192) continue;
            for (unsigned int i = 0; i < tabs->size() && count < cap; ++i) {
                Platoon* p = (*tabs)[i];
                if (!p) continue;
                const hand& h = p->getHandle();
                if (!(h.container | h.containerSerial)) continue;
                out[count].c = h.container; out[count].cs = h.containerSerial;
                out[count].loaded = p->activePlatoon != 0;
                out[count].name[0] = 0;
                if (p->activePlatoon) {
                    strncpy(out[count].name, p->activePlatoon->getName().c_str(), 255);
                    out[count].name[255] = 0;
                }
                ++count;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

DatapanelGUI* g_playerBoxes[4] = {0, 0, 0, 0};
DatapanelGUI* g_playerBoxToggle = 0;
std::string g_playerBoxTitles[4];
std::vector<std::pair<std::string, std::string> > g_playerBoxRows[4];
std::string g_playerBoxToggleTitle;
unsigned long long g_playerBoxWorld = 0;
unsigned int g_playerBoxPage = 0;
std::map<DataPanelLine*, CoopPlayerBoxAction> g_playerBoxActions[4];
CoopPlayerBoxAction g_playerBoxClickedAction;
bool g_playerBoxClicked = false;

bool playerBoxAliveSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        if (g->guiDatapanels.size() > 8192) return false;
        for (unsigned int i = 0; i < g->guiDatapanels.size(); ++i)
            if (g->guiDatapanels[i] == p) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool playerBoxAnchorSeh(ForgottenGUI* g, int* w, int* h, MyGUI::IntCoord* bar, bool* fallback) {
    __try {
        if (!g || !g->mainbar || !g->mainbar->mMainWidget || !g->isVisible() ||
            !g->mainbar->mMainWidget->getVisible() || g->isLoading() || g->hasModalMessage() ||
            g->isCharacterEditorMode() || g->getNumOpenInventoryWindows() > 0 ||
            g->isStatsWindowOpen() || g->getInDialogue()) return false;
        ManagementScreen* management = ManagementScreen::getSingleton();
        if (management && management->getVisible()) return false;
        *w = g->mainbar->mMainWidget->getWidth();
        *h = g->mainbar->mMainWidget->getHeight();
        if (*w < 640 || *h < 480) return false;
        if (g->mainbar->portraitsTabPanel) {
            if (!g->mainbar->portraitsTabPanel->getVisible()) return false;
            *bar = g->mainbar->portraitsTabPanel->getAbsoluteCoord();
            if (bar->width > 0 && bar->height > 0 && bar->top > 0 && bar->top < *h) return true;
        }
        *fallback = true;
        *bar = MyGUI::IntCoord((int)(*w * 0.308854f), (int)(*h * 0.787963f),
                               (int)(*w * 0.396875f), (int)(*h * 0.214815f));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void playerBoxPlaceSeh(DatapanelGUI* p, int x, int y, int w, int h, bool show) {
    if (!p) return;
    __try {
        if (show) {
            if (p->win && (p->win->getWidth() != w || p->win->getHeight() != h)) p->_NV_resize(w, h);
            p->_NV_setPosition(x, y);
        }
        p->_NV_show(show);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void onPlayerBoxPage(DataPanelLine*);
void onPlayerBoxHide(DataPanelLine*);
void onPlayerBoxAction(DataPanelLine*);

bool playerBoxBuildSeh(DatapanelGUI* p, const std::string* title,
                      const std::vector<std::pair<std::string, std::string> >* rows,
                      bool toggle, const std::string* hide, const std::string* page,
                      const std::vector<CoopPlayerBoxAction>* actions,
                      std::map<DataPanelLine*, CoopPlayerBoxAction>* bindings) {
    __try {
        if (bindings) bindings->clear();
        p->_NV_clear();
        p->setCaption(*title);
        p->_NV_setAutomaticTargeting(false);
        if (toggle) {
            DataPanelLine_Button* hideButton = p->setLineButton(*hide, *hide, 0);
            if (hideButton) hideButton->callback = MyGUI::newDelegate(&onPlayerBoxHide);
            if (!page->empty()) p->setLineButton(*page, *page, 0)->callback = MyGUI::newDelegate(&onPlayerBoxPage);
        } else {
            MyGUI::Align align = MyGUI::Align::Left;
            for (unsigned int i = 0; i < rows->size(); ++i) {
                if ((*rows)[i].first[0] == 'b') {
                    unsigned int index = (unsigned int)atoi((*rows)[i].first.c_str() + 1);
                    if (index >= actions->size()) continue;
                    DataPanelLine_Button* button = p->setLineButton((*rows)[i].first, (*rows)[i].second, 0);
                    if (button) {
                        (*bindings)[button] = (*actions)[index];
                        button->callback = MyGUI::newDelegate(&onPlayerBoxAction);
                    }
                } else p->setLineText((*rows)[i].first, (*rows)[i].second, 0, true, align);
            }
        }
        p->_NV_update();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void onPlayerBoxPage(DataPanelLine*) { ++g_playerBoxPage; }

} // namespace

void readPlayerBoxRoster(GameWorld* gw, std::vector<CoopPlayerSquad>& out) {
    out.clear();
    PlayerBoxTabRead tabs[OWNERS_MAX];
    unsigned int memberCount = 0;
    unsigned int n = playerBoxTabsSeh(gw, tabs, OWNERS_MAX, &memberCount);
    for (unsigned int i = 0; i < n; ++i) {
        CoopPlayerSquad row;
        row.container = tabs[i].c; row.containerSerial = tabs[i].cs;
        row.name = room::displayText(tabs[i].name, 192); row.loaded = tabs[i].loaded;
        out.push_back(row);
    }
    std::vector<EntityState> members(memberCount);
    n = memberCount ? captureSquad(gw, false, &members[0], memberCount) : 0;
    for (unsigned int i = 0; i < n; ++i) {
        char name[256];
        charName(resolve(members[i]), name, sizeof(name));
        for (unsigned int j = 0; j < out.size(); ++j)
            if (out[j].container == members[i].hContainer && out[j].containerSerial == members[i].hContainerSerial) {
                out[j].members.push_back(room::displayText(name, 192)); break;
            }
    }
}

void coopPlayerBoxesTick(const std::vector<CoopPlayerBox>& boxes, bool active,
                         unsigned long long world) {
    static bool loaded = false, savedHidden = false, loggedFallback = false;
    if (!loaded) { g_playerBoxesHidden = savedHidden = loadPlayerBoxesHidden(); loaded = true; }
    if (savedHidden != g_playerBoxesHidden) {
        savePlayerBoxesHidden(g_playerBoxesHidden); savedHidden = g_playerBoxesHidden;
    }
    ForgottenGUI* g = ::gui;
    for (int i = 0; i < 4; ++i) if (g_playerBoxes[i] && !playerBoxAliveSeh(g, g_playerBoxes[i])) {
        g_playerBoxes[i] = 0; g_playerBoxTitles[i].clear(); g_playerBoxRows[i].clear(); g_playerBoxActions[i].clear();
    }
    if (g_playerBoxToggle && !playerBoxAliveSeh(g, g_playerBoxToggle)) {
        g_playerBoxToggle = 0; g_playerBoxToggleTitle.clear();
    }
    if (!active || world != g_playerBoxWorld) {
        for (int i = 0; i < 4; ++i) {
            panelDestroySeh(g, g_playerBoxes[i]); g_playerBoxes[i] = 0;
            g_playerBoxTitles[i].clear(); g_playerBoxRows[i].clear(); g_playerBoxActions[i].clear();
        }
        panelDestroySeh(g, g_playerBoxToggle); g_playerBoxToggle = 0; g_playerBoxToggleTitle.clear();
        g_playerBoxWorld = world; g_playerBoxPage = 0; g_playerBoxClicked = false;
        if (!active) return;
    }
    int screenW = 0, screenH = 0;
    MyGUI::IntCoord bar;
    bool fallback = false;
    bool show = !g_panel.open && playerBoxAnchorSeh(g, &screenW, &screenH, &bar, &fallback);
    if (!show || boxes.empty()) {
        for (int i = 0; i < 4; ++i) playerBoxPlaceSeh(g_playerBoxes[i], 0, 0, 0, 0, false);
        playerBoxPlaceSeh(g_playerBoxToggle, 0, 0, 0, 0, false);
        return;
    }
    if (fallback && !loggedFallback) {
        coop::logLine("[playerbox] squad tab widget unavailable; using mainbar layout proportions"); loggedFallback = true;
    }
    unsigned int pages = ((unsigned int)boxes.size() + 3) / 4;
    g_playerBoxPage %= pages;
    unsigned int start = g_playerBoxPage * 4;
    unsigned int count = (unsigned int)boxes.size() - start;
    if (count > 4) count = 4;
    float scale = (float)screenH / 1080.0f;
    int gap = (int)(6 * scale), boxH = (int)(280 * scale), stripH = (int)((pages > 1 ? 98 : 72) * scale);
    int totalW = (int)(count * 270 * scale) + ((int)count - 1) * gap;
    if (totalW < bar.width) totalW = bar.width;
    if (totalW > screenW - 2 * gap) totalW = screenW - 2 * gap;
    int left = bar.left + bar.width / 2 - totalW / 2;
    if (left < gap) left = gap;
    if (left + totalW > screenW - gap) left = screenW - gap - totalW;
    int bottom = bar.top - gap;
    if (bottom <= stripH + gap + (int)(80 * scale)) {
        for (int i = 0; i < 4; ++i) playerBoxPlaceSeh(g_playerBoxes[i], 0, 0, 0, 0, false);
        playerBoxPlaceSeh(g_playerBoxToggle, 0, 0, 0, 0, false);
        return;
    }
    if (boxH + stripH > bottom - gap) boxH = bottom - stripH - gap;
    int boxW = (totalW - ((int)count - 1) * gap) / (int)count;
    int stripW = (int)(310 * scale);
    if (stripW > totalW) stripW = totalW;
    std::string layer = "MiddleFront", hide = coop::tr(g_playerBoxesHidden ? "Show player boxes" : "Hide player boxes"), page;
    char b[256];
    if (pages > 1) {
        _snprintf(b, sizeof(b) - 1, coop::tr("Players %u/%u - next page"), g_playerBoxPage + 1, pages);
        b[sizeof(b) - 1] = 0; page = b;
    }
    std::string toggleTitle = coop::tr("Co-op players");
    std::string toggleFingerprint = toggleTitle + page + hide;
    if (!g_playerBoxToggle) {
        g_playerBoxToggle = g->createDatapanel(0.1f, 0.2f, 0.2f, 0.1f, false, layer, true);
    }
    if (g_playerBoxToggle && toggleFingerprint != g_playerBoxToggleTitle &&
        playerBoxBuildSeh(g_playerBoxToggle, &toggleTitle, 0, true, &hide, &page, 0, 0)) g_playerBoxToggleTitle = toggleFingerprint;
    playerBoxPlaceSeh(g_playerBoxToggle, left, bottom - stripH - (g_playerBoxesHidden ? 0 : boxH + gap), stripW, stripH, true);
    for (unsigned int i = 0; i < 4; ++i) {
        if (i >= count || g_playerBoxesHidden) { playerBoxPlaceSeh(g_playerBoxes[i], 0, 0, 0, 0, false); continue; }
        const CoopPlayerBox& box = boxes[start + i];
        std::string title = box.name + " - " + (box.present ? coop::tr("Online") : coop::tr("Offline"));
        if (box.local) title += coop::tr(" (me)");
        std::vector<std::pair<std::string, std::string> > rows;
        std::vector<CoopPlayerBoxAction> actions;
        if (box.squads.empty()) rows.push_back(std::make_pair("empty", std::string(coop::tr("No assigned squads"))));
        for (unsigned int section = 0; section <= box.squads.size(); ++section) {
            const CoopPlayerSquad* squad = section ? &box.squads[section - 1] : 0;
            _snprintf(b, sizeof(b) - 1, "s%u", section); b[sizeof(b) - 1] = 0;
            std::string key = b;
            if (squad) rows.push_back(std::make_pair(key, squad->name));
            const std::vector<std::string>& status = squad ? squad->status : box.status;
            for (unsigned int j = 0; j < status.size(); ++j) {
                _snprintf(b, sizeof(b) - 1, "s%ut%u", section, j); b[sizeof(b) - 1] = 0;
                rows.push_back(std::make_pair(std::string(b), status[j]));
            }
            const std::vector<CoopPlayerBoxAction>& buttons = squad ? squad->actions : box.actions;
            for (unsigned int j = 0; j < buttons.size(); ++j) {
                const CoopPlayerBoxAction& a = buttons[j];
                _snprintf(b, sizeof(b) - 1, "b%uw%llue%uc%u:%up%llua%llut%u:%us%uk%d", (unsigned int)actions.size(),
                    a.world, a.epoch, a.selfConnection, a.targetConnection, a.playerId, a.actorPlayerId,
                    a.container, a.containerSerial, a.targetOwnerId, a.kind);
                b[sizeof(b) - 1] = 0;
                rows.push_back(std::make_pair(std::string(b), a.caption)); actions.push_back(a);
            }
            if (!squad) continue;
            if (squad->members.empty()) rows.push_back(std::make_pair(key + "e", std::string(squad->loaded ? coop::tr("No loaded members") : coop::tr("Members not loaded"))));
            for (unsigned int j = 0; j < squad->members.size(); ++j) {
                _snprintf(b, sizeof(b) - 1, "s%um%u", section, j); b[sizeof(b) - 1] = 0;
                rows.push_back(std::make_pair(std::string(b), "  " + (squad->members[j].empty() ? std::string(coop::tr("Unnamed character")) : squad->members[j])));
            }
        }
        if (!g_playerBoxes[i]) {
            g_playerBoxes[i] = g->createDatapanel(0.2f, 0.2f, 0.2f, 0.2f, true, layer, true);
        }
        playerBoxPlaceSeh(g_playerBoxes[i], left + (int)i * (boxW + gap), bottom - boxH, boxW, boxH, true);
        if (g_playerBoxes[i] && (g_playerBoxTitles[i] != title || g_playerBoxRows[i] != rows) &&
            playerBoxBuildSeh(g_playerBoxes[i], &title, &rows, false, &hide, &page, &actions, &g_playerBoxActions[i])) {
            g_playerBoxTitles[i] = title; g_playerBoxRows[i] = rows;
        }
        playerBoxPlaceSeh(g_playerBoxes[i], left + (int)i * (boxW + gap), bottom - boxH, boxW, boxH, true);
    }
}

} } // namespace coop::engine


namespace coop { namespace engine { namespace {
void onPlayerBoxHide(DataPanelLine*) {
    g_playerBoxesHidden = !g_playerBoxesHidden;
    coop::logLine(g_playerBoxesHidden ? "[playerbox] hidden from player box" : "[playerbox] shown from player box");
}

void onPlayerBoxAction(DataPanelLine* line) {
    if (g_playerBoxesHidden || g_panel.open) return;
    for (int i = 0; i < 4; ++i) {
        std::map<DataPanelLine*, CoopPlayerBoxAction>::const_iterator at = g_playerBoxActions[i].find(line);
        if (at == g_playerBoxActions[i].end()) continue;
        g_playerBoxClickedAction = at->second; g_playerBoxClicked = true; break;
    }
}
} // namespace

bool takePlayerBoxAction(CoopPlayerBoxAction& out) {
    if (!g_playerBoxClicked) return false;
    g_playerBoxClicked = false;
    if (g_playerBoxesHidden || g_panel.open) return false;
    out = g_playerBoxClickedAction;
    return true;
}
} } // namespace coop::engine
