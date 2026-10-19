// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 that lets the player pick role + transport
// (buttons/checkboxes - the only reliably interactive DatapanelGUI controls;
// MyGUI comboboxes/editboxes have no usable RVAs and don't receive keyboard focus
// during gameplay) and Connect/Disconnect. The friend's Steam ID is entered by
// clipboard: "Copy my Steam ID" puts the player's own id on the clipboard to
// share, and "Paste friend's Steam ID" reads the friend's id back in
// (有效粘贴会记入 coop_last_peer.txt，下次启动预填). The UDP endpoint (ip/port) still comes from
// coop_config.json. The GUI layer stays session-agnostic: live status is passed IN
// via *st and the user's actions are handed BACK through the callbacks (the plugin
// root owns the session/config wiring). Main-thread only; SEH-guarded.
struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        detail;      // one-line status string for the panel/overlay
    // Join-side save-transfer status (null when not streaming): byte-level
    // progress the one-line detail above has no room for, shown on the F2 panel
    // while a join receives the host's world (e.g. "Streaming host world... 42%
    // (3.1/7.4 MB)"). Set by coopPanelDrive, rendered in dbgVal.
    const char*        transferDetail;
    // 小队控制授权（协议 1002，第 3 批）：面板要显示的授权状态与按钮可用性。
    int                authRequestable; // 1 = 有“不归自己”的标签可以申请
    int                authRequesting;  // 1 = 本端已申请 / 正在收回，等对方回应
    int                authPendingPeer; // 1 = 对方申请中，需要 [同意]/[拒绝]
    int                authGrantedMine; // >0 = 本端已授权出去的标签数（可 [收回]）
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect; the UDP endpoint is re-read from the config there.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId);
typedef void (*CoopDisconnectFn)();
// 小队控制授权动作（第 3 批）：0 = 申请控制，1 = 同意，2 = 拒绝，3 = 收回控制权。
typedef void (*CoopAuthFn)(int action);
void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect, CoopAuthFn onAuth);

// Persistent co-op connection-status banner: a single screen-space label fixed 10
// px in from the top-left corner (a createFloatingLabel MyGUI::Window on the
// spike-48 screenshot-proven "Info" layer) whose caption shows the live session
// status, colored by state (0 = offline/red, 1 = waiting/yellow, 2 =
// connected/green). Needs no player character, so it also shows at the title
// screen; updated in place when the text/state changes and re-minted if the GUI
// destroyed the widget (world load). Pass show=false to remove it. Main-thread
// only; SEH-guarded.
void coopOverlayTick(const char* text, int state, bool show);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
