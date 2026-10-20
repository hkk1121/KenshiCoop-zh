// Plugin - RE_Kenshi / KenshiLib plugin entry point (clean rebuild).
//
// Responsibilities (kept deliberately thin - everything substantive lives in the
// core/, net/, game/, sync/ modules):
//   * startPlugin(): read config, open the log, install the main-loop + title
//     hooks, start networking.
//   * mainLoop_hook(): the single main-thread safe point. Detects "gameplay
//     live", drains net events, drives per-stage sync, and (in test mode)
//     self-exits after the configured duration.
//   * titleUpdate_hook(): auto-loads the configured save once the menu settles.
//
// Stage 0 scope: modular skeleton + ENet handshake (version-checked) + lifecycle.
// Sync of entities is added from Stage 1 onward.

#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include <Debug.h>                  // DebugLog / ErrorLog
#include <core/Functions.h>         // KenshiLib::AddHook / GetRealAddress / SUCCESS
#include <kenshi/GameWorld.h>       // GameWorld (main-loop hook signature)
#include <kenshi/gui/TitleScreen.h> // TitleScreen::_NV_update (auto-load trigger)

#include <windows.h>
#include <cstdio>
#include <string>
#include <deque>

#include "CoopLog.h"
#include "core/Config.h"
#include "core/CrashDump.h"
#include "core/FpsLimit.h"
#include "core/OwnRanks.h"
#include "core/Inbound.h"
#include "net/NetLink.h"
#include "net/Discovery.h"
#include "net/SteamP2P.h"
#include "net/SteamInvite.h"
#include "game/Engine.h"
#include "game/EngineChargen.h"
#include "game/EngineUi.h"       // Phase 5a: F2 co-op panel + status overlay
#include "game/EngineScenario.h" // Phase 5a: auto-bake scene builders
#include "sync/Replicator.h"
#include "sync/SaveXfer.h"
#ifdef KENSHICOOP_HARNESS
#include "test/Scenario.h" // scenario runner: Harness/Debug builds only (Phase 1)
#endif

namespace {

// PluginHost (Phase 3): composition root owning the four long-lived subsystem
// singletons as one cohesive object instead of four loose globals. The g_cfg /
// g_net / g_inbound / g_repl names remain as references INTO the host so every
// existing call site is unchanged; later phases thread the host explicitly and
// fold the session-state globals in behind a SessionController.
struct PluginHost {
    coop::Config     cfg;
    coop::NetLink    net;
    coop::Inbound    inbound;
    coop::Replicator repl;
};
PluginHost g_host;

coop::Config&     g_cfg     = g_host.cfg;
coop::NetLink&    g_net     = g_host.net;
coop::Inbound&    g_inbound = g_host.inbound;
coop::Replicator& g_repl    = g_host.repl;
coop::u32        g_tick = 0;

// Last GameWorld seen by the main-loop hook. The F2-panel UI callbacks
// (coopUiConnect/coopUiDisconnect) run without a GameWorld argument, but the
// world is live when the user hits Connect/Disconnect, so we despawn our
// minted proxies (NPC + world-item, Phase 3) through this cached pointer to
// avoid leaking duplicate bodies into the save. Only touched on the main thread.
GameWorld*       g_lastGw = 0;

// Cross-owner trade veto owner classifier (engine InvOwnerClassFn): forwards a
// save-stable owner hand to the Replicator's squad-ownership sets. Free function
// so the engine layer (which must not know about the Replicator) can call it.
static int coopInvOwnerClass(const unsigned int h[5]) {
    return g_repl.ownerClassForHand(h);
}

// assault_mint's victim query (ScenarioContext::pickMintedProxy). Same shape and
// same reason as the classifier above: the scenario layer must not know about the
// Replicator, and only the Replicator knows which local bodies are mints.
static bool coopScenarioPickMintedProxy(const unsigned int refHand[5],
                                        unsigned int outLocal[5],
                                        unsigned int outCanon[5],
                                        float* outDist) {
    if (!g_lastGw) return false;
    return g_repl.pickMintedProxyNear(g_lastGw, refHand, outLocal, outCanon, outDist);
}

// SessionController (Phase 3): the mutable per-session lifecycle state that used
// to live as ~18 loose file globals - peer presence, the gameplay-start edge,
// the title auto-load gate, and the coordinated save/load (protocol 31/32)
// coordination counters + latches. Grouping them behind one object makes the
// session boundary explicit and gives the reset helpers (sessionResetForUi /
// sessionResetForWorldReload) a single, named place to clear. As with
// PluginHost, the historical g_* names remain as references INTO the controller
// so every call site is unchanged (zero churn). C++03: defaults are set in the
// constructor (no in-class member initializers).
struct SessionController {
    bool         gameStarted;      // gameplay has been live at least once
    DWORD        gameStartTick;    // GetTickCount at the gameplay-start edge
    bool         autoLoadDone;     // title auto-load fired (settle gate)
    DWORD        titleFirstTick;   // first title tick (settle gate base)
    // Remote owners connected right now (protocol 49). On the host: the
    // admitted join ids. On a join: the host (0) plus the roster the host
    // announces via PKT_PEER_STATUS. Empty = solo.
    std::set<coop::u32> peersPresent;
    // Coordinated save (protocol 31).
    std::string  savePending;      // host: save name awaiting quiescence
    coop::u32    saveReqId;        // join: monotonic PKT_SAVE_REQ counter
    // Push-save-on-connect bootstrap (host): when a peer connects the host bakes
    // a fresh save of its live world and, once the folder quiesces, sends the
    // join a LOAD_GO for it (not a blind stream) - the join loads it if it has an
    // identical copy, else NACKs and driveLoadSync's fallback transfer streams
    // the folder first. Lets a joiner enter the host's world with no pre-shared
    // save. bootstrapTargets (protocol 49): the ids the pending bake will be
    // announced to - ONLY those joins get the LOAD_GO, so a late joiner's
    // bootstrap can never yank an already-playing join through a reload.
    bool         bootstrapArmed;   // host: a connect-triggered save is baking
    std::string  bootstrapName;    // host: that save's name (== savePending)
    std::set<coop::u32> bootstrapTargets; // host: joins awaiting that announce
    // World-swap edge detection (protocol 32): once gameplay has started,
    // gameplayLive dropping means the engine is swapping worlds (a load); live
    // again = the reload edge (session-reset point). Sub-second dips are FLICKER
    // and do NOT count - a real load screen lasts seconds and a spurious reset
    // would wipe live session state mid-game.
    DWORD        swapStartTick;    // != 0: gameplay non-live, swap running
    coop::u32    swapHookTicks;    // mainLoop ticks observed during the swap
    // Coordinated load (protocol 32).
    bool         loadSuppressOn;   // join: current suppression lever state
    coop::u32    loadIdOut;        // host: monotonic LOAD_GO id
    coop::u32    loadIdSeen;       // join: newest GO loadId handled
    coop::u32    loadReqId;        // join: monotonic PKT_LOAD_REQ counter
    // Host: saves awaiting a post-reload transfer, one entry per NACKing join
    // (protocol 49: each transfer streams to ITS join only; they run
    // sequentially through the single SaveXfer sender).
    std::vector<std::pair<std::string, coop::u32> > loadXferPending;
    // 好友带入自己角色后，只给该 owner 安排一次延迟存档重推。
    std::map<coop::u32, DWORD> peerBodyRepushDue;
    std::string  loadAfterCommit;  // join: save to load once its transfer commits
    coop::u32    loadCommitBase;   // join: savexfer::commitSeq() at NACK time
    // Deferred-signal backstop: SaveManager::load only SETS the LOADGAME signal;
    // it can sit unconsumed mid-session. Armed on every coordinated load issue;
    // if the swap hasn't started after the grace window, pump execute() once.
    DWORD        loadPumpArmTick;  // != 0: backstop armed at this tick

    SessionController()
      : gameStarted(false), gameStartTick(0), autoLoadDone(false),
        titleFirstTick(0),
        saveReqId(0), bootstrapArmed(false),
        swapStartTick(0), swapHookTicks(0),
        loadSuppressOn(false), loadIdOut(0), loadIdSeen(0), loadReqId(0),
        loadCommitBase(0), loadPumpArmTick(0) {}
};
SessionController g_session;

bool&        g_gameStarted    = g_session.gameStarted;
DWORD&       g_gameStartTick   = g_session.gameStartTick;
bool&        g_autoLoadDone    = g_session.autoLoadDone;
DWORD&       g_titleFirstTick  = g_session.titleFirstTick;
std::set<coop::u32>& g_peersPresent = g_session.peersPresent;
std::string& g_savePending     = g_session.savePending;
coop::u32&   g_saveReqId       = g_session.saveReqId;
bool&        g_bootstrapArmed  = g_session.bootstrapArmed;
std::string& g_bootstrapName   = g_session.bootstrapName;
std::set<coop::u32>& g_bootstrapTargets = g_session.bootstrapTargets;
DWORD&       g_swapStartTick   = g_session.swapStartTick;
coop::u32&   g_swapHookTicks   = g_session.swapHookTicks;
bool&        g_loadSuppressOn  = g_session.loadSuppressOn;
coop::u32&   g_loadIdOut       = g_session.loadIdOut;
coop::u32&   g_loadIdSeen      = g_session.loadIdSeen;
coop::u32&   g_loadReqId       = g_session.loadReqId;
std::vector<std::pair<std::string, coop::u32> >& g_loadXferPending =
    g_session.loadXferPending;
std::map<coop::u32, DWORD>& g_peerBodyRepushDue = g_session.peerBodyRepushDue;
std::string& g_loadAfterCommit = g_session.loadAfterCommit;
coop::u32&   g_loadCommitBase  = g_session.loadCommitBase;
DWORD&       g_loadPumpArmTick  = g_session.loadPumpArmTick;
// 兜底存档传输被加入方判失败（ACK ok=0）时自动重发。多人会连续排入多份传输，
// 因此按 xferId 记录存档目标，并按 owner 单独计算重发次数。
std::string  g_loadXferLastName;
coop::u32    g_loadXferLastOwner = 0;
std::map<coop::u32, std::pair<std::string, coop::u32> > g_loadXferSent;
std::map<coop::u32, unsigned int> g_loadXferRetries;
const unsigned int LOAD_XFER_MAX_RETRIES = 2;
// Scenario harness state. Harness/Debug builds only - the shipped Release DLL
// excludes test/Scenario*.cpp and does not define KENSHICOOP_HARNESS (Phase 1).
#ifdef KENSHICOOP_HARNESS
coop::Scenario* g_scenario        = 0;
bool            g_scenarioStarted = false;
DWORD           g_scenarioStartTick = 0;
unsigned int    g_scenarioTick    = 0;
DWORD           g_scenarioDoneTick = 0; // !=0 once RESULT logged; begins capture hold
const DWORD     SCENARIO_HOLD_MS  = 4000; // hold synced state on screen for capture
#endif

// "Multiplayer+ (Wanderer x2)" start (dist/mods/KenshiCoop/KenshiCoop.mod). The FCS record
// sets the 500k shared wallet, but a Kenshi Character record only carries the
// GROUPED stat fields ("combat stats", "ranged stats", "stealth stats", "strength",
// "unarmed stats") and randomises them - there is no way to say "50 in everything"
// in data. So the start marks itself with the template below and the plugin writes
// the stat line at gameplay start. Keep the id in sync with Sid(4) in
// tools/MultiplayerStartGen/Program.cs.
const char* const WPX2_MARKER_SID = "4-KenshiCoop-MultiplayerStart.mod";
const float       WPX2_STAT_LEVEL = 50.0f;

// Test-scene setup (host-only): one-shot world spawn the user then saves.
bool            g_setupDone     = false;
const DWORD     SETUP_DELAY_MS  = 4000; // let the world settle before spawning
DWORD           g_lastCraftRearmTick = 0; // throttle host craft re-arm
DWORD           g_bakeSaveTick  = 0;     // != 0: auto-bake save armed at this tick
const DWORD     CRAFT_REARM_MS  = 3000; // re-issue the work goal at most this often

// Session-state timing constants (the mutable coordination state itself now
// lives in SessionController / g_session above).
const DWORD  SWAP_MIN_MS        = 400;   // shorter world-swap dips are flicker, not a reload
const DWORD  LOAD_PUMP_GRACE_MS = 2000;  // deferred-LOADGAME backstop grace window

// 好友连接/断开瞬间在状态栏短暂提示（上游 PR #24，改为复用状态栏而非另建控件）
const DWORD  TOAST_SHOW_MS      = 4000;
const char*  g_toastText        = 0;     // 已翻译的提示文字，0 = 无
int          g_toastState       = 0;     // 状态栏颜色（2 = 绿，0 = 红）
DWORD        g_toastArmMs       = 0;

// 小队控制授权：命令拦截的归属分类（engine OrderOwnerClassFn）。只有对方此刻正在驱动的
// 角色才算对方控制，单人游戏和双方都不拥有的小队不拦（ownerClassForHand 在单人时也会把
// 第 2 个以后的小队判为“非本端”）。
static int coopOrderOwnerClass(const unsigned int h[5]) {
    return g_repl.orderClassForHand(h);
}

// 小队控制授权（第 1 批）：本地命令被拦下时的状态栏提示（engine OrderBlockedFn）。
// 与 coopInvOwnerClass 同一模式：引擎层不认识面板与 Replicator，只回调。
static void coopOrderBlocked() {
    g_toastText  = coop::tr("This is your friend's squad - ask them for control");
    g_toastState = 0;
    g_toastArmMs = GetTickCount();
}

// Original function pointers, filled by KenshiLib::AddHook.
void (*g_mainLoop_orig)(GameWorld*, float) = 0;
void (*g_titleUpdate_orig)(TitleScreen*)   = 0;

// In-game co-op panel (F2) connect/disconnect handlers. Defined after
// startNetworking() (which coopUiConnect reuses); forward-declared here so
// mainLoop_hook can hand their addresses to coopPanelTick.
void startNetworking();
void coopRecruitOwnSquad(Character* c);
void coopCreatedSquad(unsigned int container, unsigned int serial);
void coopUiConnect(bool isHost, bool useSteam, const unsigned long long* peerIds,
                   unsigned int peerCount, unsigned int ownRank);
void coopUiDisconnect();
// 小队控制授权（第 3 批）：F2 面板按钮 → Replicator 动作（0 申请 / 1 同意 / 2 拒绝 / 3 收回或归还）
void coopUiAuth(int action);
void coopUiCharacter(int action);
void driveCharacterCreation(GameWorld* gw, bool worldLive);
void completeCharacterSave(bool saved);
std::vector<coop::engine::CharacterChoice> g_characterChoices;
unsigned int g_characterChoice = 0;
int g_characterAction = -1;
bool g_characterAvailable = false;
bool g_characterPending = false;
coop::u64 g_characterWorld = 0;
coop::u32 g_characterRequestId = 0;
coop::CharacterRequestHeader g_characterWait = { 0 };
DWORD g_characterWaitTick = 0, g_characterAppearanceTick = 0;
float g_characterPosition[3] = { 0, 0, 0 };
std::deque<coop::InboundCharacterRequest> g_characterRequests;
std::map<coop::u64, coop::CharacterReplyPacket> g_characterResults;
std::vector<coop::InboundCharacterAppearance> g_characterAppearances;
std::string g_characterStatus, g_characterNotified;
bool g_characterSaveInFlight = false;
coop::CharacterReplyPacket g_characterSaveReply = { 0 };
coop::CharacterReplyPacket g_characterPush = { 0 };
coop::u64 g_characterEditWorld = 0;
coop::u32 g_characterEditConnection = 0;
GameWorld* g_characterChoiceWorld = 0;

// Log to BOTH our dedicated per-line-flushed file (what the test runner reads)
// and the engine's kenshi.log (handy when attached live).
void coopLog(const char* msg) { coop::logLine(msg);    DebugLog(msg); }
void coopErr(const char* msg) { coop::logErrLine(msg); ErrorLog(msg); }

// Blank-portraits diagnostic (protocol 36): loading a save folder without its
// portrait atlas blanks the squad-tab avatars until the next portrait rebuild.
// Warn at every coordinated-load issue point so a session log pinpoints WHICH
// load introduced the blank avatars (host reload, join GO, or a transferred
// copy that shipped without the atlas).
void warnIfNoPortraits(const std::string& name) {
    std::string p = coop::savexfer::saveFolderFor(name) + "\\portraits_texture.png";
    if (GetFileAttributesW(coop::savexfer::widePath(p).c_str()) == INVALID_FILE_ATTRIBUTES) {
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[load] WARN save '%s' has no portraits_texture.png "
                  "(squad avatars will render blank)", name.c_str());
        b[sizeof(b) - 1] = '\0'; coopErr(b);
    }
}

// SessionController reset primitives (Phase 3). The old world / peer is gone, so
// both drop the Replicator's session maps and the Inbound world-state queues in
// a fixed order; centralizing them keeps that order from drifting between the
// several edges that each need one.

// UI connect/disconnect teardown: the world is live (reconnect/disconnect from
// within a running game), so despawn our minted NPC + world-item proxies before
// clearing the maps - else a reconnect leaves orphaned duplicates or bakes them
// into the next save. Falls back to a plain map reset if no world has ticked yet.
void sessionResetForUi() {
    coop::engine::cancelCharacterCreation(g_lastGw && coop::engine::gameplayLive(g_lastGw));
    g_characterPending = false; g_characterChoiceWorld = 0; g_characterAction = -1;
    g_characterRequests.clear(); g_characterAppearances.clear(); g_characterResults.clear(); g_characterWorld = 0;
    g_characterSaveInFlight = false; g_characterPush.result = 0;
    std::deque<coop::CharacterReplyPacket> characterReplies; g_inbound.drainCharacterReplies(characterReplies);
    g_peersPresent.clear();
    g_peerBodyRepushDue.clear();
    g_loadXferLastName.clear();
    g_loadXferLastOwner = 0;
    g_loadXferSent.clear();
    g_loadXferRetries.clear();
    if (g_lastGw) g_repl.clearPeerReplicationState(g_lastGw);
    else          g_repl.resetSession();
    g_repl.resetOwnersForWorld();
    g_inbound.flushWorldState();
    std::deque<coop::InboundOwners> owners; g_inbound.drainOwners(owners);
    std::deque<coop::OwnerRequestPacket> requests; g_inbound.drainOwnerRequests(requests);
    std::deque<coop::PeerStatusPacket> players; g_inbound.drainPeerStatus(players);
    g_net.bumpSessionEpoch(); // v44: fence off any in-flight prior-session batch
}

// World-reload session reset (protocol 32): the old world is gone - every
// pointer cache and session map describes it. Peer presence and the suppression
// levers survive (the connection never dropped). Both roles run this on their
// OWN reload edge, and the synchronous-swap backstop reuses it, so the reset
// order (repl maps, then inbound queues) is defined in exactly one place.
void sessionResetForWorldReload() {
    coop::engine::cancelCharacterCreation(false);
    g_characterPending = false; g_characterChoiceWorld = 0; g_characterAction = -1;
    g_characterRequests.clear(); g_characterAppearances.clear(); g_characterResults.clear(); g_characterWorld = 0;
    g_characterSaveInFlight = false; g_characterPush.result = 0;
    g_peerBodyRepushDue.clear();
    g_repl.resetSession();
    g_repl.resetOwnersForWorld();
    g_inbound.flushWorldState();
    g_net.bumpSessionEpoch(); // v44: post-reload batches supersede the old session
    coopLog("[load] inbound world-state queues flushed");
}

// Push-save-on-connect (host): bake a fresh save of the live world and arm the
// bootstrap so driveSaveSync announces it to 'targetId' with a LOAD_GO once
// the folder quiesces (protocol 49: targeted, so an already-playing join is
// never yanked through the newcomer's bootstrap load). Called for either
// connect ordering: a peer connecting while the host is already in-game
// (processNetEvents), or the host's gameplay starting with peers already
// connected (mainLoop_hook gameplay-start edge - once per present peer).
// A second target arriving while a bake is still pending just joins the
// pending announce (one bake serves both).
// Host + saveSync + in-game are the caller's responsibility.
bool armConnectPush(coop::u32 targetId) {
    g_bootstrapTargets.insert(targetId);
    if (g_bootstrapArmed) {
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "[boot] bake pending; added target id=%u", (unsigned)targetId);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        return true;
    }
    char cur[64];
    cur[0] = '\0';
    coop::engine::saveInfo(cur, sizeof(cur), 0, 0);
    std::string name = cur[0] ? cur : "coopresume";
    g_bootstrapArmed = true;
    g_bootstrapName  = name;
    char b[144];
    _snprintf(b, sizeof(b) - 1,
              "[boot] baking save '%s' to push to join id=%u on connect",
              name.c_str(), (unsigned)targetId);
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    if (!coop::engine::saveGameAs(name)) {
        coopErr("[boot] connect-push save FAILED to issue");
        g_bootstrapArmed = false; g_bootstrapTargets.clear(); return false;
    }
    return true;
}

// Drain peer connect/leave events and surface a single game-thread confirmation
// per event. The net thread already logs the handshake; this proves the event
// reached the game thread cleanly (and is where later stages spawn/sweep).
void processNetEvents(GameWorld* gw) {
    std::deque<coop::u32> conns, leaves;
    g_repl.setLocalId(g_net.localId());
    g_repl.ingestPlayers(g_inbound);
    g_inbound.drainConnects(conns);
    g_inbound.drainLeaves(leaves);
    // 先处理离开再处理连接：同一玩家重连顶替旧连接时，离开与连接的编号相同（见 Inbound::pushLeave）
    for (std::deque<coop::u32>::iterator it = leaves.begin(); it != leaves.end(); ++it) {
        char b[64];
        _snprintf(b, sizeof(b) - 1, "handshake: peer left id=%u", (unsigned)*it);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        // Carried-body sync (protocol 18) + furniture occupancy (protocol 19):
        // the departed peer's stream will never author its drop/exit edges -
        // release any carry or occupancy its driven copies still hold.
        if (gw && (g_cfg.carrySync || g_cfg.furnSync)) g_repl.sweepCarries(gw);
        // 仅在确实连着时提示断开（加入方重试连接失败也会产生离开事件，避免每次重试都弹）
        if (!g_peersPresent.empty()) {
            g_toastText = coop::tr("Peer disconnected");
            g_toastState = 0;
            g_toastArmMs = GetTickCount();
        }
        // Phase 2 crash hardening, per owner since protocol 49: a peer drop
        // leaves this side's minted proxies standing AND its drive maps
        // pointing at bodies with no fresh authority (the engine will
        // eventually reap them, and the next drive touches a freed pointer -
        // the "join crash -> host follow-on crash" chain). OWNER_ID_ALL (a
        // join losing its host link) keeps the full wipe + world-state flush;
        // a single owner's leave tears down exactly its state, and the
        // session continues for everyone else.
        if (*it == coop::OWNER_ID_ALL) {
            g_peersPresent.clear();
            g_peerBodyRepushDue.clear();
            g_loadXferLastName.clear();
            g_loadXferLastOwner = 0;
            g_loadXferSent.clear();
            g_loadXferRetries.clear();
            g_bootstrapTargets.clear();
            g_loadXferPending.clear();
            g_repl.clearPeerReplicationState(gw);
            g_inbound.flushWorldState();
            // Coordinated save: disconnected = solo again; local saves work.
            if (!g_cfg.isHost && g_cfg.saveSync) {
                coop::engine::setSaveSuppress(false);
                coopLog("[save] JOIN save suppression OFF (host link lost)");
            }
        } else {
            g_peersPresent.erase(*it);
            g_peerBodyRepushDue.erase(*it);
            g_bootstrapTargets.erase(*it);
            if (*it == g_loadXferLastOwner) {
                g_loadXferLastName.clear();
                g_loadXferLastOwner = 0;
            }
            for (std::map<coop::u32, std::pair<std::string, coop::u32> >::iterator
                     xi = g_loadXferSent.begin();
                 xi != g_loadXferSent.end(); ) {
                if (xi->second.second == *it) xi = g_loadXferSent.erase(xi);
                else ++xi;
            }
            g_loadXferRetries.erase(*it);
            for (std::vector<std::pair<std::string, coop::u32> >::iterator
                     xi = g_loadXferPending.begin();
                 xi != g_loadXferPending.end(); ) {
                if (xi->second == *it) xi = g_loadXferPending.erase(xi);
                else ++xi;
            }
            g_repl.clearOwnerReplicationState(gw, *it);
        }
    }
    for (std::deque<coop::u32>::iterator it = conns.begin(); it != conns.end(); ++it) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "handshake: peer present id=%u (local id=%u)",
                  (unsigned)*it, (unsigned)g_net.localId());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        // Connect-edge resync (protocol 30): re-announce placed buildings and
        // force an immediate resend pass across all change-gated channels, so
        // a late joiner / reconnector converges now instead of waiting out
        // per-channel safety resends (or never minting a pre-connect build).
        // With protocol 49 this also fires on a JOIN when the host announces
        // a new fellow join (roster edge) - our re-burst reaches it through
        // the host relay.
        if (g_cfg.latejoinSync) g_repl.onPeerConnected(g_net, g_net.localId());
        else coopLog("[latejoin] connect edge seen, resync OFF (gate)");
        coop::clearNetUiError(); // 好友已连上，此前的拒绝原因作废
        g_toastText = coop::tr("Peer connected"); g_toastState = 2; g_toastArmMs = GetTickCount();
        g_peersPresent.insert(*it);
        g_peerBodyRepushDue.erase(*it);
        // Coordinated save (protocol 31): while connected TO THE HOST under
        // save-sync, the JOIN never writes a save locally - the host's save
        // is authoritative and a local save press forwards as PKT_SAVE_REQ.
        // Keyed to the host link (id 0), not a fellow join's roster edge.
        if (!g_cfg.isHost && g_cfg.saveSync && *it == 0) {
            coop::engine::setSaveSuppress(true);
            coopLog("[save] JOIN save suppression ON (host save is authoritative)");
        }
        // Push-save-on-connect (host): if already in a game, bake+announce the
        // live world so the join can enter it with no pre-shared save. If the
        // host is NOT yet in-game (title/loading), the gameplay-start edge in
        // mainLoop_hook arms this instead - covers either connect ordering.
        // Targeted (protocol 49): only the connecting join is announced to.
        if (g_cfg.isHost && g_cfg.saveSync && g_gameStarted)
            armConnectPush(*it);
    }
}

// SaveLoadCoordinator receive primitive (protocol 31): the join-side save-
// transfer receive pump. Drain staged BEGIN/FILE/DONE packets, verify + commit
// the streamed folder, and ACK the outcome. This identical work is needed on two
// paths - the normal saveSync receiver half (driveSaveSync) and the saveSync-
// gated-off fallback transfer (driveLoadSync) - so it lives here once instead of
// being copy-pasted into both. Emits no log line, so the oracle log contract is
// unaffected by the extraction.
void pumpSaveReceive() {
    // 按与网络线程入队相反的顺序取队列（DONE → FILE → BEGIN）：网络线程按 BEGIN、FILE…、DONE
    // 的顺序入队，若先取 FILE 后取 DONE，最后一块和 DONE 恰好在两次取之间到达时，就会先校验
    // 后写入，最后一块被判丢失（badCrc=1）。反序取保证取到的 DONE / FILE 之前的包都已在手。
    std::deque<coop::InboundSaveDone> dones;
    g_inbound.drainSaveDones(dones);
    std::deque<coop::InboundSaveFile> chunks;
    g_inbound.drainSaveFiles(chunks);
    std::deque<coop::InboundSaveBegin> begins;
    g_inbound.drainSaveBegins(begins);

    for (std::deque<coop::InboundSaveBegin>::iterator it = begins.begin();
         it != begins.end(); ++it)
        coop::savexfer::onSaveBegin(it->pkt);

    for (std::deque<coop::InboundSaveFile>::iterator it = chunks.begin();
         it != chunks.end(); ++it)
        coop::savexfer::onSaveFile(it->hdr, it->path.c_str(),
                                   it->data.empty() ? 0 : &it->data[0]);

    for (std::deque<coop::InboundSaveDone>::iterator it = dones.begin();
         it != dones.end(); ++it) {
        coop::u16 files = 0;
        unsigned __int64 bytes = 0;
        int ok = coop::savexfer::onSaveDone(it->hdr,
                                            it->crcs.empty() ? 0 : &it->crcs[0],
                                            &files, &bytes);
        coop::SaveAckPacket ack;
        memset(&ack, 0, sizeof(ack));
        ack.type    = (coop::u8)coop::PKT_SAVE_ACK;
        ack.ownerId = g_net.localId();
        ack.xferId  = it->hdr.xferId;
        ack.ok      = ok ? 1 : 0;
        ack.files   = files;
        ack.bytes   = bytes;
        g_net.queueSaveAck(ack);
    }
}

// Coordinated save + session resume (protocol 31), both roles, once per tick.
//
// HOST: any local save edge (menu/quicksave/autosave/programmatic - the
// SaveManager::save detour catches them all) arms the folder-quiescence
// watch; when the folder settles, the whole save folder streams to the join
// (SaveXfer sender, paced chunks); the join's ACK closes the loop. A join
// PKT_SAVE_REQ funnels into the same path via engine::saveGameAs.
//
// JOIN: local saves are suppressed while connected (the detour skips the
// engine write); a suppressed MANUAL edge forwards to the host as
// PKT_SAVE_REQ (autosave edges are dropped - the host's own autosave already
// drives a coordinated save). Received BEGIN/FILE/DONE stage + verify +
// commit the host's folder; the ACK reports the outcome.
void driveSaveSync() {
    // Local save edges from the detour (max 8 queued per tick).
    coop::engine::SaveEdge edges[8];
    unsigned int nEdges = coop::engine::drainSaveEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        std::string name = edges[i].name[0] ? edges[i].name : "coopresume";
        if (g_cfg.isHost) {
            g_savePending = name;
            coop::savexfer::armWatch(name);
        } else if (edges[i].suppressed && !edges[i].autosave) {
            coop::SaveReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_SAVE_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_saveReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueSaveReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[save] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    if (g_cfg.isHost) {
        // Join save requests -> the ONE authoritative save (the saveGameAs
        // below re-enters the detour, whose edge arms the watch above).
        std::deque<coop::InboundSaveReq> reqs;
        g_inbound.drainSaveReqs(reqs);
        for (std::deque<coop::InboundSaveReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[save] REQ from join id=%u name='%s' -> saving",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::saveGameAs(name[0] ? name : "coopresume"))
                coopErr("[save] join-requested save FAILED to issue");
        }

        // Quiescence watch -> start the transfer once the save is on disk.
        if (coop::savexfer::watching()) {
            unsigned int files = 0;
            unsigned __int64 bytes = 0;
            unsigned long waited = 0;
            int rc = coop::savexfer::tickWatch(&files, &bytes, &waited);
            if (rc == 1 || rc == 2) {
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "[save] QUIESCED kind=%s name='%s' files=%u bytes=%I64u waitMs=%lu",
                          rc == 1 ? "settled" : "timeout", g_savePending.c_str(),
                          files, bytes, waited);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                if (g_characterSaveInFlight && rc != 1) {
                    coopErr("[chargen] save did not settle; targeted push cancelled");
                    completeCharacterSave(false);
                    g_bootstrapArmed = false; g_bootstrapTargets.clear(); g_savePending.clear(); return;
                }
                if (!g_repl.writeOwners(g_savePending) && g_net.isRunning()) {
                    completeCharacterSave(false);
                    coopErr("[owners] save transfer cancelled: ownership file write failed");
                    g_bootstrapArmed = false; g_bootstrapTargets.clear(); g_savePending.clear();
                    return;
                }
                completeCharacterSave(true);
                if (!g_cfg.saveSync) { g_savePending.clear(); return; }
                if (g_bootstrapArmed && g_savePending == g_bootstrapName) {
                    // Connect-push: announce the freshly-baked save with a
                    // LOAD_GO instead of a blind stream. The join loads it
                    // directly if its on-disk copy matches the fingerprint;
                    // otherwise it NACKs and driveLoadSync's fallback transfer
                    // streams the folder before the join loads. Reuses the
                    // whole existing LOAD_GO/NACK/transfer/commit machinery.
                    // TARGETED (protocol 49): only the joins that triggered
                    // this bake are announced to - a join already in the
                    // world must not be reloaded by a newcomer's bootstrap.
                    coop::LoadGoPacket go;
                    memset(&go, 0, sizeof(go));
                    go.type        = (coop::u8)coop::PKT_LOAD_GO;
                    go.ownerId     = g_net.localId();
                    go.loadId      = ++g_loadIdOut;
                    go.fingerprint = coop::savexfer::folderFingerprint(g_bootstrapName);
                    strncpy(go.name, g_bootstrapName.c_str(), sizeof(go.name) - 1);
                    for (std::set<coop::u32>::iterator ti = g_bootstrapTargets.begin();
                         ti != g_bootstrapTargets.end(); ++ti) {
                        g_net.queueLoadGo(go, *ti);
                        char b2[192];
                        _snprintf(b2, sizeof(b2) - 1,
                                  "[boot] GO->join target=%u id=%u name='%s' fp=%08x (push on connect)",
                                  (unsigned)*ti, go.loadId,
                                  g_bootstrapName.c_str(), go.fingerprint);
                        b2[sizeof(b2) - 1] = '\0'; coopLog(b2);
                    }
                    g_loadPumpArmTick = GetTickCount();
                    warnIfNoPortraits(g_bootstrapName);
                    g_bootstrapArmed = false;
                    g_bootstrapName.clear();
                    g_bootstrapTargets.clear();
                    g_savePending.clear();
                } else if (!g_peersPresent.empty())
                    coop::savexfer::beginSend(g_net, g_net.localId(), g_savePending,
                                              coop::OWNER_ID_ALL);
                else
                    coopLog("[save] no peer connected; transfer skipped");
            }
        }
        // Paced chunk pump for an in-flight transfer.
        if (coop::savexfer::sending() &&
            coop::savexfer::tickSend(g_net, g_net.localId())) {
            if (g_loadXferLastOwner != 0) {
                g_loadXferSent[coop::savexfer::lastSentXferId()] =
                    std::make_pair(g_loadXferLastName, g_loadXferLastOwner);
                g_loadXferLastName.clear();
                g_loadXferLastOwner = 0;
            }
        }

        // Join commit acknowledgements.
        std::deque<coop::InboundSaveAck> acks;
        g_inbound.drainSaveAcks(acks);
        for (std::deque<coop::InboundSaveAck>::iterator it = acks.begin();
             it != acks.end(); ++it) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[save] XFER-ACK id=%u owner=%u ok=%u files=%u bytes=%I64u",
                      it->pkt.xferId, (unsigned)it->pkt.ownerId,
                      (unsigned)it->pkt.ok,
                      (unsigned)it->pkt.files, it->pkt.bytes);
            b[sizeof(b) - 1] = '\0';
            if (it->pkt.ok) coopLog(b); else coopErr(b);
            coop::savexfer::noteAck(it->pkt.xferId, it->pkt.ownerId,
                                    it->pkt.ok ? 1 : 0);
            // 按 xferId 找回对应的兜底传输；连续给多人发送时，ACK 可能晚于
            // 下一份传输开始，不能再用“最近一次”推断失败者。
            std::map<coop::u32, std::pair<std::string, coop::u32> >::iterator sent =
                g_loadXferSent.find(it->pkt.xferId);
            if (sent != g_loadXferSent.end()) {
                std::string sentName = sent->second.first;
                coop::u32 sentOwner = sent->second.second;
                g_loadXferSent.erase(sent);
                if (it->pkt.ok) {
                    g_loadXferRetries.erase(sentOwner);
                } else {
                    unsigned int& retries = g_loadXferRetries[sentOwner];
                    if (retries < LOAD_XFER_MAX_RETRIES) {
                        ++retries;
                        bool queued = false;
                        for (std::vector<std::pair<std::string, coop::u32> >::iterator
                                 xi = g_loadXferPending.begin();
                             xi != g_loadXferPending.end(); ++xi) {
                            if (xi->second != sentOwner) continue;
                            xi->first = sentName;
                            queued = true;
                            break;
                        }
                        if (!queued)
                            g_loadXferPending.push_back(
                                std::make_pair(sentName, sentOwner));
                        _snprintf(b, sizeof(b) - 1,
                                  "[load] transfer failed on join -> retry %u/%u name='%s' target=%u",
                                  retries, LOAD_XFER_MAX_RETRIES, sentName.c_str(),
                                  (unsigned)sentOwner);
                        b[sizeof(b) - 1] = '\0'; coopLog(b);
                    }
                }
            }
        }
    } else {
        // Receiver half: stage, verify, commit, acknowledge (shared pump).
        pumpSaveReceive();
    }
}

// Coordinated load (protocol 32), both roles, once per tick.
//
// HOST (load-authoritative, mirroring the save arbitration): any local load
// edge (menu or programmatic - the SaveManager::load detour catches both)
// broadcasts PKT_LOAD_GO with the save's folder fingerprint, then the host
// loads natively (never delayed). A join PKT_LOAD_REQ funnels into the same
// path via engine::loadSave. A join PKT_LOAD_NACK (copy missing/diverged)
// queues a protocol-31 SaveXfer of the folder, fired once the host's own
// reload has completed; the join loads after the verified commit.
//
// JOIN: local loads are suppressed while connected under the gate (the
// detour swallows the engine call); a suppressed manual edge forwards as
// PKT_LOAD_REQ. On LOAD_GO the join fingerprints its local copy: match =
// load now (bypass-once through the suppressed detour); mismatch/missing =
// NACK + a pending latch that loads after the matching transfer commits.
void driveLoadSync(GameWorld* gw) {
    // Local load edges from the detour (max 8 queued per tick).
    coop::engine::LoadEdge edges[8];
    unsigned int nEdges = coop::engine::drainLoadEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        if (!edges[i].name[0]) continue;
        std::string name = edges[i].name;
        if (g_cfg.isHost) {
            // The title-screen auto-load precedes gameplay (and any peer) -
            // only a MID-SESSION load edge coordinates.
            if (!g_gameStarted) continue;
            // A load supersedes any in-flight save coordination.
            coop::savexfer::abortAll();
            g_savePending.clear();
            coop::LoadGoPacket go;
            memset(&go, 0, sizeof(go));
            go.type        = (coop::u8)coop::PKT_LOAD_GO;
            go.ownerId     = g_net.localId();
            go.loadId      = ++g_loadIdOut;
            go.fingerprint = coop::savexfer::folderFingerprint(name);
            strncpy(go.name, name.c_str(), sizeof(go.name) - 1);
            g_net.queueLoadGo(go);
            g_loadPumpArmTick = GetTickCount();
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[load] GO->join id=%u name='%s' fp=%08x",
                      go.loadId, name.c_str(), go.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            warnIfNoPortraits(name);
        } else if (edges[i].suppressed) {
            // Forward the swallowed manual load for host arbitration.
            coop::LoadReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_LOAD_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_loadReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueLoadReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[load] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        } else if (g_gameStarted) {
            // The join's own coordinated (bypass-once) load went through the
            // engine - arm the stalled-signal backstop for it too.
            g_loadPumpArmTick = GetTickCount();
        }
    }

    if (g_cfg.isHost) {
        // Join load requests -> the ONE authoritative load (the loadSave
        // below re-enters the detour, whose edge broadcasts the GO above).
        std::deque<coop::InboundLoadReq> reqs;
        g_inbound.drainLoadReqs(reqs);
        for (std::deque<coop::InboundLoadReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            if (coop::savexfer::folderFingerprint(name) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] REQ from join id=%u name='%s' REFUSED (no such save here)",
                          it->pkt.reqId, name);
                b[sizeof(b) - 1] = '\0'; coopErr(b);
                continue;
            }
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[load] REQ from join id=%u name='%s' -> loading",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::loadSave(name))
                coopErr("[load] join-requested load FAILED to issue");
        }

        // NACKs: the join can't load our save - stream it the folder once
        // OUR OWN reload has completed (never mid-swap).
        std::deque<coop::InboundLoadNack> nacks;
        g_inbound.drainLoadNacks(nacks);
        for (std::deque<coop::InboundLoadNack>::iterator it = nacks.begin();
             it != nacks.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[176];
            if (it->pkt.loadId != g_loadIdOut) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] stale NACK id=%u (current %u) ignored",
                          it->pkt.loadId, g_loadIdOut);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                continue;
            }
            _snprintf(b, sizeof(b) - 1,
                      "[load] NACK id=%u owner=%u name='%s' joinFp=%08x -> transfer after reload",
                      it->pkt.loadId, (unsigned)it->pkt.ownerId, name,
                      it->pkt.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            // One pending transfer per NACKing join (protocol 49); duplicates
            // for the same join collapse to the newest name.
            bool updated = false;
            for (std::vector<std::pair<std::string, coop::u32> >::iterator
                     xi = g_loadXferPending.begin();
                 xi != g_loadXferPending.end(); ++xi) {
                if (xi->second == it->pkt.ownerId) {
                    xi->first = name;
                    updated = true;
                    break;
                }
            }
            if (!updated)
                g_loadXferPending.push_back(
                    std::make_pair(std::string(name), it->pkt.ownerId));
            g_loadXferRetries[it->pkt.ownerId] = 0; // 新的 NACK：该玩家计数从头算
            for (std::map<coop::u32, std::pair<std::string, coop::u32> >::iterator
                     xi = g_loadXferSent.begin();
                 xi != g_loadXferSent.end(); ) {
                if (xi->second.second == it->pkt.ownerId)
                    xi = g_loadXferSent.erase(xi);
                else ++xi;
            }
        }
        if (!g_loadXferPending.empty() && coop::engine::gameplayLive(gw) &&
            !coop::savexfer::sending()) {
            // Sequential: one SaveXfer stream at a time; the next pending
            // join's transfer starts when this one's DONE goes out.
            std::pair<std::string, coop::u32> next = g_loadXferPending.front();
            g_loadXferPending.erase(g_loadXferPending.begin());
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[load] starting fallback transfer name='%s' target=%u",
                      next.first.c_str(), (unsigned)next.second);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (coop::savexfer::beginSend(g_net, g_net.localId(), next.first,
                                          next.second)) {
                g_loadXferLastName = next.first;
                g_loadXferLastOwner = next.second;
            } else {
                g_loadXferLastName.clear();
                g_loadXferLastOwner = 0;
            }
        }
        // The chunk pump normally lives in driveSaveSync; keep the fallback
        // transfer moving even when saveSync is gated off.
        if (!g_cfg.saveSync && coop::savexfer::sending() &&
            coop::savexfer::tickSend(g_net, g_net.localId())) {
            if (g_loadXferLastOwner != 0) {
                g_loadXferSent[coop::savexfer::lastSentXferId()] =
                    std::make_pair(g_loadXferLastName, g_loadXferLastOwner);
                g_loadXferLastName.clear();
                g_loadXferLastOwner = 0;
            }
        }
    } else {
        // Test-only (KENSHICOOP_FORCE_STREAM=1, join): force the missing/diverged
        // NACK branch even when our on-disk fingerprint MATCHES the host's, so a
        // single-machine run (where both installs share %LOCALAPPDATA%\kenshi\save
        // and would otherwise MATCH + load from disk) still exercises the REAL
        // folder-transfer + post-transfer load path. Default OFF; read once.
        static int s_forceStream = -1;
        if (s_forceStream < 0) {
            const char* e = getenv("KENSHICOOP_FORCE_STREAM");
            s_forceStream = (e && e[0] == '1') ? 1 : 0;
            if (s_forceStream)
                coopLog("[load] FORCE-STREAM armed (test): join NACKs matching "
                        "saves to exercise the transfer");
        }

        // LOAD_GOs: verify our on-disk copy and follow the host.
        // 刚进入自己新开的游戏时，先把主机的推送压住一会：让自己的角色先传到主机、进到主机
        // 的存档里（主机会在角色进来后重新推送）。压住期间到的多个推送只处理最新的一个。
        const DWORD JOIN_GO_HOLD_MS = 10000;
        bool holdGo = gw && g_gameStarted && (GetTickCount() - g_gameStartTick) < JOIN_GO_HOLD_MS;
        std::deque<coop::InboundLoadGo> gos;
        if (!holdGo) g_inbound.drainLoadGos(gos);
        while (gos.size() > 1) gos.pop_front();
        for (std::deque<coop::InboundLoadGo>::iterator it = gos.begin();
             it != gos.end(); ++it) {
            if (it->pkt.loadId <= g_loadIdSeen) continue; // stale/duplicate
            g_loadIdSeen = it->pkt.loadId;
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            coop::u32 fp = coop::savexfer::folderFingerprint(name);
            char b[192];
            if (!s_forceStream && fp != 0 && fp == it->pkt.fingerprint) {
                // Our copy is loadable as-is, so load it - ALWAYS, including
                // when this save is the one already live here. A host
                // mid-session load and a connect-push arrive as the SAME
                // packet (nothing on the wire says which), so the old "already
                // in this save, skip the hitch" arm swallowed the real reload:
                // the host swapped worlds while this side kept running the
                // pre-load one, holding its characters' damage, position and
                // inventory, and never reached sessionResetForWorldReload().
                // The cost of reloading unconditionally is one load screen when
                // two clients connect already sitting on the same save.
                _snprintf(b, sizeof(b) - 1,
                          "[load] GO id=%u name='%s' fp=%08x MATCH -> loading",
                          it->pkt.loadId, name, fp);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                warnIfNoPortraits(name);
                g_loadAfterCommit.clear();
                coop::engine::setLoadBypassOnce();
                if (!coop::engine::loadSave(name))
                    coopErr("[load] coordinated load FAILED to issue");
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] GO id=%u name='%s' hostFp=%08x localFp=%08x %s -> NACK (transfer)",
                          it->pkt.loadId, name, it->pkt.fingerprint, fp,
                          fp == 0 ? "MISSING" : "DIVERGED");
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                coop::LoadNackPacket nk;
                memset(&nk, 0, sizeof(nk));
                nk.type        = (coop::u8)coop::PKT_LOAD_NACK;
                nk.ownerId     = g_net.localId();
                nk.loadId      = it->pkt.loadId;
                nk.fingerprint = fp;
                strncpy(nk.name, name, sizeof(nk.name) - 1);
                g_net.queueLoadNack(nk);
                g_loadAfterCommit = name;
                g_loadCommitBase  = coop::savexfer::commitSeq();
            }
        }

        // The transfer's receive half (BEGIN/FILE/DONE -> stage/verify/
        // commit/ACK) normally lives in driveSaveSync; run it here when
        // saveSync is gated off so the fallback transfer still lands.
        if (!g_cfg.saveSync)
            pumpSaveReceive();

        // Pending latch: load once the transfer for OUR save committed.
        if (!g_loadAfterCommit.empty() &&
            coop::savexfer::commitSeq() > g_loadCommitBase) {
            if (coop::savexfer::lastCommitResult() == 1 &&
                _stricmp(coop::savexfer::lastCommitName().c_str(),
                         g_loadAfterCommit.c_str()) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] transfer committed -> loading '%s'",
                          g_loadAfterCommit.c_str());
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                warnIfNoPortraits(g_loadAfterCommit);
                coop::engine::setLoadBypassOnce();
                if (!coop::engine::loadSave(g_loadAfterCommit))
                    coopErr("[load] post-transfer load FAILED to issue");
                g_loadAfterCommit.clear();
            } else {
                // Some other/failed commit landed; re-base and keep waiting.
                g_loadCommitBase = coop::savexfer::commitSeq();
            }
        }
    }
}

// ---- Tailnet/LAN discovery browser (panel-side state) -------------------------
// The Scan button's session state: which scanned host is currently picked. The
// pick survives coopUiConnect's config-file reload (session-scoped, exactly like
// panel-pasted Steam IDs) so ONLINE connects to the scanned host.
struct DiscSel {
    bool           valid;
    char           ip[64];
    unsigned short port;
    DiscSel() : valid(false), port(0) { ip[0] = '\0'; }
};
DiscSel      g_discSel;
int          g_discPick    = -1;    // index into the scan results (-1 = none)
unsigned int g_discSeenGen = 0;     // last scan generation consumed by the tick
bool         g_discApply   = false; // auto-pick result 0 when the UI scan lands
bool         g_discAutoJoin = false;// ONE-CLICK JOIN: connect when the scan lands
std::string  g_discDetail;          // browser status line shown in the panel
bool         g_discAutoScanFired = false; // harness: one-shot self-scan latch
bool         g_uiAutoFired       = false; // KENSHICOOP_UI_AUTO one-shot latch

// Adopt scan result g_discPick: remember it for coopUiConnect and surface it in
// the config so the choice is immediate and visible.
void applyDiscPick() {
    coop::disc::FoundHost fh;
    if (g_discPick < 0 || !coop::disc::foundGet((unsigned int)g_discPick, &fh)) return;
    strncpy(g_discSel.ip, fh.ip, sizeof(g_discSel.ip) - 1);
    g_discSel.ip[sizeof(g_discSel.ip) - 1] = '\0';
    g_discSel.port  = fh.info.gamePort;
    g_discSel.valid = true;
    g_cfg.ip   = g_discSel.ip;
    g_cfg.port = g_discSel.port;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[disc] selected host '%s' %s:%u (v%u %u/%u save='%s')",
              fh.info.hostName, fh.ip, (unsigned)fh.info.gamePort,
              fh.info.protocol, fh.info.players, fh.info.maxPlayers,
              fh.info.saveName);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
}

// Panel Scan button (JOIN + UDP): first click scans; further clicks step
// through the found hosts; wrapping back to the first also refreshes the list
// in the background (so a host that went up/down since the scan shows up).
void coopUiScan() {
    if (coop::disc::scanBusy()) { coopLog("[disc] scan already running"); return; }
    unsigned int n = coop::disc::foundCount();
    if (n > 0 && g_discSeenGen == coop::disc::scanGeneration() && g_discPick >= 0) {
        g_discPick = (g_discPick + 1) % (int)n;
        applyDiscPick();
        if (g_discPick == 0)
            coop::disc::scanStart((unsigned short)g_cfg.discPort); // silent refresh
    } else {
        g_discApply = true;
        coop::disc::scanStart((unsigned short)g_cfg.discPort);
    }
}

// ONE-CLICK launcher actions (the title-screen HOST GAME / JOIN GAME buttons;
// also driven by the KENSHICOOP_UI_AUTO rehearsal hook - synthetic mouse input
// never reaches Kenshi, so the flow test presses the same entry point).
//   HOST: go ONLINE as a UDP host right away; the player then loads a save.
//   JOIN: scan, and when the scan lands (tickDiscovery below) auto-pick a
//         protocol-matching host, auto-claim the NEXT FREE squad slot from
//         the advertised player count (1 player in = slot 1; 2 in = slot 2),
//         and connect. Steam transport / manual slots stay on the F2 panel.
void coopUiMenuAction(bool host) {
    if (host) {
        if (g_net.isRunning() && g_cfg.isHost) {
            coopLog("[coop-ui] menu: already hosting");
            return;
        }
        coopLog("[coop-ui] menu: one-click HOST (udp)");
        coopUiConnect(true, false, 0, 0, 0);
    } else {
        if (g_net.isRunning() && !g_cfg.isHost) {
            coopLog("[coop-ui] menu: already joining/joined (F2 to manage)");
            return;
        }
        if (coop::disc::scanBusy()) {
            g_discAutoJoin = true; // adopt the scan already in flight
            return;
        }
        coopLog("[coop-ui] menu: one-click JOIN - scanning for hosts");
        g_discAutoJoin = true;
        g_discApply    = false;
        coop::disc::scanStart((unsigned short)g_cfg.discPort);
    }
}

// Discovery tick: runs in BOTH interactive and harness modes (the panel drive
// below is interactive-only). Refreshes the host responder's advert and
// consumes finished scans; the harness autoscan (KENSHICOOP_DISC_AUTOSCAN)
// fires one join-side scan ~10 s after startup so the loopback rig proves the
// probe->reply round trip end to end from the logs alone.
void tickDiscovery() {
    if (coop::disc::responderRunning())
        coop::disc::responderAdvert((unsigned short)g_cfg.port,
                                    1 + (unsigned int)g_peersPresent.size(),
                                    coop::MAX_PLAYERS, g_cfg.save.c_str());

    if (g_cfg.discAutoScan && !g_cfg.isHost && !g_discAutoScanFired) {
        static DWORD s_first = 0;
        if (s_first == 0) s_first = GetTickCount();
        if (GetTickCount() - s_first >= 10000) {
            g_discAutoScanFired = true;
            coop::disc::scanStart((unsigned short)g_cfg.discPort); // log-only
        }
    }

    // Consume a finished scan: adopt result 0 for a UI-initiated scan, clamp a
    // stale pick, and rebuild the panel status line.
    unsigned int gen = coop::disc::scanGeneration();
    if (gen != g_discSeenGen) {
        g_discSeenGen = gen;
        unsigned int n = coop::disc::foundCount();
        if (g_discAutoJoin) {
            // ONE-CLICK JOIN: pick the first protocol-matching host, claim the
            // next free squad slot from the advertised player count, connect.
            // The advert can be a beat stale (two friends clicking JOIN at
            // once): the host rejects a doubly-claimed slot LOUDLY, and the
            // next click rescans with the fresh count.
            g_discAutoJoin = false;
            int pick = -1;
            coop::disc::FoundHost fh;
            for (unsigned int i = 0; i < n; ++i) {
                if (coop::disc::foundGet(i, &fh) &&
                    fh.info.protocol == coop::PROTOCOL_VERSION) { pick = (int)i; break; }
            }
            if (pick < 0 && n > 0 && coop::disc::foundGet(0, &fh))
                pick = 0; // version mismatch: connect anyway - it fails loudly
            if (pick >= 0 && fh.info.players >= fh.info.maxPlayers) {
                // The status row shows the n/n count; just don't connect.
                g_discPick = pick;
                coopLog("[disc] auto-join refused: session full");
            } else if (pick >= 0) {
                g_discPick = pick;
                applyDiscPick();
                unsigned int rank = fh.info.players; // host + joins in = next free tab
                if (rank < 1) rank = 1;
                if (fh.info.maxPlayers > 1 && rank > (unsigned int)(fh.info.maxPlayers - 1))
                    rank = fh.info.maxPlayers - 1;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "[disc] auto-join '%s' %s:%u slot=%u",
                          fh.info.hostName, fh.ip, (unsigned)fh.info.gamePort, rank);
                b[sizeof(b) - 1] = '\0';
                coopLog(b);
                coopUiConnect(false, false, 0, 0, rank);
            }
            // No hosts found: the status text below says so; the next JOIN
            // click rescans.
        } else if (g_discApply) {
            g_discApply = false;
            g_discPick = (n > 0) ? 0 : -1;
            if (g_discPick >= 0) applyDiscPick();
        } else if (g_discPick >= (int)n) {
            g_discPick = (n > 0) ? 0 : -1;
        }
    }
    if (coop::disc::scanBusy()) {
        g_discDetail = coop::tr("Scanning Tailscale + loopback...");
    } else if (g_discSeenGen == 0) {
        g_discDetail.clear(); // never scanned - the button caption is the hint
    } else {
        unsigned int n = coop::disc::foundCount();
        if (n == 0) {
            g_discDetail = coop::tr("No hosts found - the host must be ONLINE over UDP");
            if (!coop::disc::lastScanSawTailscale()) {
                g_discDetail += " ";
                g_discDetail += coop::tr("(no tailscale CLI: loopback/LAN only)");
            }
        } else {
            coop::disc::FoundHost fh;
            if (g_discPick >= 0 && coop::disc::foundGet((unsigned int)g_discPick, &fh)) {
                char b[192];
                _snprintf(b, sizeof(b) - 1,
                          coop::tr("[%d/%u] %s @ %s:%u  save: %s  %u/%u%s"),
                          g_discPick + 1, n, fh.info.hostName, fh.ip,
                          (unsigned)fh.info.gamePort,
                          fh.info.saveName[0] ? fh.info.saveName : "?",
                          fh.info.players, fh.info.maxPlayers,
                          fh.info.protocol == coop::PROTOCOL_VERSION
                              ? "" : coop::tr("  [PROTOCOL MISMATCH]"));
                b[sizeof(b) - 1] = '\0';
                g_discDetail = b;
            }
        }
    }
}

// Co-op session panel (F2) + status overlay. Interactive sessions only - the
// unattended harness (scenario / self-exit timer) never touches the panel, and
// keeping the GUI stack out of those runs avoids perturbing the scenario
// oracles. Both calls are SEH-guarded internally and touch only GUI + input +
// (guarded) leader read, so they are safe wherever the GUI stack is up. gw may
// be null (title screen): coopOverlayTick then finds no leader and hides the
// banner, while the panel itself needs no world. Driven from BOTH the in-game
// mainLoop_hook and the title-screen titleUpdate_hook so a join can go ONLINE
// (and copy/paste Steam IDs) straight from the main menu.
void coopPlayerBoxesDrive(GameWorld* gw);

void coopPanelDrive(GameWorld* gw) {
    // Discovery ticks in EVERY mode (the harness autoscan + responder advert
    // must run without the panel); the panel below stays interactive-only.
    tickDiscovery();
    if (!(g_cfg.scenario.empty() && g_cfg.testSeconds == 0)) return;

    // Menu-flow rehearsal (KENSHICOOP_UI_AUTO=host|join): press HOST GAME /
    // JOIN GAME for us ~15 s after the first interactive tick. Synthetic mouse
    // input never reaches Kenshi, so the automated flow test drives the SAME
    // entry point the launcher buttons call. One-shot; interactive mode only
    // (this sits below the harness gate).
    if (!g_cfg.uiAuto.empty() && !g_uiAutoFired) {
        static DWORD s_uiAutoFirst = 0;
        if (s_uiAutoFirst == 0) s_uiAutoFirst = GetTickCount();
        if (GetTickCount() - s_uiAutoFirst >= 15000) {
            g_uiAutoFired = true;
            coopLog(g_cfg.uiAuto == "host" ? "[coop-ui] UI_AUTO pressing HOST GAME"
                                           : "[coop-ui] UI_AUTO pressing JOIN GAME");
            coopUiMenuAction(g_cfg.uiAuto == "host");
        }
    }
    bool steamHost = g_net.isRunning() && g_cfg.isHost && g_cfg.transport == "steam";
    unsigned int occupied = 1;
    const std::map<coop::u64, coop::PeerStatusPacket>& roomPlayers = g_repl.players();
    for (std::map<coop::u64, coop::PeerStatusPacket>::const_iterator it = roomPlayers.begin(); it != roomPlayers.end(); ++it)
        if (it->second.present && it->second.ownRank < coop::MAX_PLAYERS &&
            (it->second.ownerId == 0 || g_peersPresent.count(it->second.ownerId))) occupied |= 1u << it->second.ownRank;
    char currentSave[512] = { 0 };
    if (steamHost && gw) coop::engine::saveInfo(currentSave, sizeof(currentSave), 0, 0);
    std::wstring wideSave = coop::savexfer::widePath(currentSave, CP_ACP);
    int saveBytes = WideCharToMultiByte(CP_UTF8, 0, wideSave.c_str(), (int)wideSave.size(), 0, 0, 0, 0);
    std::string roomSave(saveBytes > 0 ? saveBytes : 0, '\0');
    if (saveBytes > 0) WideCharToMultiByte(CP_UTF8, 0, wideSave.c_str(), (int)wideSave.size(), &roomSave[0], saveBytes, 0, 0);
    coop::steaminvite::updateSession(steamHost,
        g_net.isRunning() && !g_cfg.isHost && g_cfg.transport == "steam" ? g_cfg.steamPeer : 0,
        roomSave.c_str(), gw && coop::engine::gameplayLive(gw), 1 + (unsigned int)g_peersPresent.size(), occupied);
    if (g_net.isRunning() && !g_cfg.isHost && g_cfg.transport == "steam") coop::steaminvite::watchRoom(g_cfg.steamPeer);
    coop::steaminvite::tick();
    coop::engine::CoopPanelState ps;
    ps.selfSteamId  = (unsigned long long)coop::steamp2p::selfId();
    ps.peerSteamId  = g_cfg.steamPeer;
    ps.running      = g_net.isRunning();
    ps.peerPresent  = !g_peersPresent.empty();
    ps.isHost       = g_cfg.isHost;
    ps.transportSel = (g_cfg.transport == "steam") ? 0 : 1;
    ps.ownRank      = g_cfg.ownRank;
    if (ps.ownRank == 0) {
        ps.ownRank = 1;
        for (std::set<unsigned int>::const_iterator ri = g_cfg.ownRanks.begin();
             ri != g_cfg.ownRanks.end(); ++ri) {
            if (*ri != 0) { ps.ownRank = *ri; break; }
        }
    }
    // 小队控制授权（第 3 批）：面板要显示的授权状态 + 按钮可用性
    ps.authRequestable = g_repl.squadAuthHasRequestable() ? 1 : 0;
    ps.authRequesting  = g_repl.squadAuthRequesting();
    ps.authPendingPeer = g_repl.squadAuthPendingPeer();
    ps.authGrantedMine = g_repl.squadAuthGrantedMine();
    ps.authBorrowed    = g_repl.squadAuthBorrowed();
    ps.canCreateCharacter = gw && g_characterAvailable;
    ps.editingCharacter = coop::engine::characterPreview();
    ps.characterTemplate = g_characterChoices.empty() ? "" : g_characterChoices[g_characterChoice].label.c_str();
    ps.characterStatus = g_characterStatus.c_str();
    // 授权事件的一次性提示（收到申请 / 同意 / 拒绝 / 收回 / 超时）→ 状态栏 4 秒。
    // 分支里直接写 tr("字面量")，让 check_lang.py 能扫到（与连接/断开提示同一写法）。
    {
        unsigned int notice = g_repl.takeSquadAuthNotice();
        if (notice == 5) {
            g_toastText = coop::tr("Your friend asks for control of your squad");
            g_toastState = 1; g_toastArmMs = GetTickCount();
        } else if (notice == 3) {
            g_toastText = coop::tr("Squad control granted");
            g_toastState = 2; g_toastArmMs = GetTickCount();
        } else if (notice == 2) {
            g_toastText = coop::tr("Your friend refused the request");
            g_toastState = 0; g_toastArmMs = GetTickCount();
        } else if (notice == 4) {
            g_toastText = coop::tr("Squad control returned");
            g_toastState = 2; g_toastArmMs = GetTickCount();
        } else if (notice == 1) {
            g_toastText = coop::tr("No answer - the request timed out");
            g_toastState = 0; g_toastArmMs = GetTickCount();
        }
    }
    unsigned int roomRejected = coop::steamp2p::takeHostRejection();
    if (roomRejected) {
        g_toastText = roomRejected == 3 ? coop::tr("Rejected a joiner: room full") :
            coop::tr("Rejected a joiner: not a Steam friend or on the allowlist");
        g_toastState = 0; g_toastArmMs = GetTickCount();
    }
    std::string detail;
    int ostate;
    if (!g_peersPresent.empty()) {
        if (g_cfg.isHost) {
            char db[96];
            _snprintf(db, sizeof(db) - 1,
                      coop::tr("Connected - %u player(s) joined"),
                      (unsigned)g_peersPresent.size());
            db[sizeof(db) - 1] = '\0';
            detail = db;
        } else {
            detail = coop::tr("Connected to host");
        }
        ostate = 2;
    } else if (g_net.isRunning()) {
        detail = coop::tr(g_cfg.isHost ? "Hosting - waiting for peer..." : "Connecting...");
        ostate = 1;
    } else {
        detail = coop::tr("Offline - press F2, then set Connection to ONLINE");
        ostate = 0;
    }
    // 连接/断开后 4 秒内状态栏显示提示；未连上时优先显示网络层的拒绝原因（如版本不一致）
    const char* netErrMsg = coop::netUiError();
    static std::string lastHostRoomError;
    if (g_cfg.isHost && netErrMsg[0] && lastHostRoomError != netErrMsg) {
        g_toastText = coop::tr(netErrMsg); g_toastState = 0; g_toastArmMs = GetTickCount();
    }
    lastHostRoomError = g_cfg.isHost ? netErrMsg : "";
    if (g_toastText && GetTickCount() - g_toastArmMs < TOAST_SHOW_MS) {
        detail = g_toastText;
        ostate = g_toastState;
    } else if (g_peersPresent.empty() && netErrMsg[0]) {
        detail = coop::tr(netErrMsg);
        ostate = 0;
    }
    if (!g_cfg.isHost && g_net.isRunning() && g_peersPresent.empty() &&
        !std::strcmp(netErrMsg, "Host is not responding: check they are online, you are Steam friends, and both use the same co-op build")) {
        const coop::room::Info& room = coop::steaminvite::watchedRoom();
        if (room.state == coop::room::NOT_PLAYING) detail = coop::tr("The host is not playing Kenshi");
        else if (room.state == coop::room::NO_ROOM) detail = coop::tr("The host has not opened a room");
        else if (room.state == coop::room::AVAILABLE && room.protocol != coop::PROTOCOL_VERSION)
            detail = coop::tr("Version mismatch with host - update both to the same KenshiCoop build");
        else if (room.state == coop::room::AVAILABLE && room.players >= room.limit)
            detail = coop::tr("Room is full - wait for a free player slot");
    }
    ps.detail = detail.c_str();

    // Join save-transfer status for the panel: while a join streams the host's
    // world at the menu (no leader -> no screen overlay), show live progress on
    // the F2 panel. The percent is whole-number so the panel only rebuilds ~100x
    // over a transfer, not every chunk. Null when not streaming.
    std::string transfer;
    if (!g_cfg.isHost && g_net.isRunning() && !g_gameStarted) {
        if (coop::savexfer::receiving()) {
            unsigned __int64 got = coop::savexfer::recvBytes();
            unsigned __int64 tot = coop::savexfer::recvTotalBytes();
            int pct = (tot > 0) ? (int)((got * 100) / tot) : 0;
            if (pct > 100) pct = 100;
            char tb[160];
            _snprintf(tb, sizeof(tb) - 1,
                      coop::tr("Streaming host world... %d%% (%.1f/%.1f MB)"), pct,
                      (double)got / (1024.0 * 1024.0),
                      (double)tot / (1024.0 * 1024.0));
            tb[sizeof(tb) - 1] = '\0';
            transfer = tb;
        } else if (!g_loadAfterCommit.empty()) {
            // NACK sent (host baking/streaming) or committed + about to load.
            transfer = coop::tr("Preparing host world...");
        }
    }
    ps.transferDetail = transfer.empty() ? (const char*)0 : transfer.c_str();

    // Tailnet/LAN browser line (join+UDP): scan progress / the picked host.
    ps.discDetail = g_discDetail.empty() ? (const char*)0 : g_discDetail.c_str();
    ps.discBusy = coop::disc::scanBusy() ? 1 : 0;

    // Title screen == no world (the title hook drives this with gw=0; the
    // in-game mainLoop hook always has a world). Shows the menu launcher.
    ps.atTitle = (gw == 0);

    // Still pump Steam callbacks so an inbound "Join Game" (a friend inviting
    // US) can fire coopUiConnect; the outbound invite/picker UI is gone.

    coop::engine::coopPanelTick(&ps, &coopUiConnect, &coopUiDisconnect, &coopUiScan,
                                &coopUiMenuAction, &coopUiAuth, &coopUiCharacter);
    coop::engine::coopOverlayTick(detail.c_str(), ostate, g_net.isRunning());
    coopPlayerBoxesDrive(gw);
}

// Main-thread tick hook: the one safe point where we touch game state.
// ---------------------------------------------------------------------------
// TickPipeline stages (Phase 3). mainLoop_hook drives these in a fixed order
// every frame; each stage was lifted VERBATIM out of the old monolithic hook,
// so per-tick behavior is unchanged - the hook now reads as an explicit
// sequence. All stages share the file-static plugin globals (g_cfg, g_repl,
// g_net, g_inbound, ...) and are defined here, just before the hook that uses
// them, so no forward declarations are needed.
// ---------------------------------------------------------------------------

// Protocol 32 world-swap edge detection. Runs FIRST so the reload edge (and,
// under load-sync, the session reset) lands before any sync code touches
// pointers from the torn-down world this tick.
void tickWorldSwapEdge(GameWorld* gw) {
    if (g_gameStarted) {
        bool live = coop::engine::gameplayLive(gw);
        if (!live && g_swapStartTick == 0) {
            g_swapStartTick = GetTickCount();
            g_swapHookTicks = 0;
            coopLog("[load] WORLD-SWAP begin (gameplay went non-live)");
        } else if (!live) {
            ++g_swapHookTicks; // probe evidence: does the hook tick during the load screen?
        } else if (g_swapStartTick != 0) {
            DWORD swapMs = GetTickCount() - g_swapStartTick;
            g_swapStartTick = 0;
            char b[160];
            if (swapMs < SWAP_MIN_MS) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-FLICKER ms=%lu (ignored, below reload threshold)",
                          (unsigned long)swapMs);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-RELOAD swapMs=%lu hookTicksDuringSwap=%u",
                          (unsigned long)swapMs, (unsigned)g_swapHookTicks);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // Session reset (protocol 32): centralized order (repl maps then
                // inbound queues). Peer presence + suppression levers survive.
                if (g_cfg.loadSync)
                    sessionResetForWorldReload();
            }
        }
    }
}

// Test-scene setup: host spawns the controlled scene ONCE, a few seconds after
// gameplay starts, then leaves the game running so the user can arrange the pose
// (e.g. seat the character) and SAVE. Baking it into a save gives the chair/NPC
// save-stable hands that resolve on both clients.
void tickSetupScene(GameWorld* gw) {
    if (!g_setupDone && !g_cfg.setupScene.empty() && g_cfg.isHost && gw &&
        g_gameStarted && (GetTickCount() - g_gameStartTick) >= SETUP_DELAY_MS) {
        g_setupDone = true;
        if (g_cfg.setupScene == "craft") {
            // Crafting/gathering (Stage 3a): spawn a work fixture + an NPC and force
            // the NPC to work it, so the host streams the work task. Both clients
            // load the baked save, so the fixture has save-stable hands on both.
            coop::engine::setupCraftScene(gw);
        } else if (g_cfg.setupScene == "down") {
            // Body-state (Stage 2) BAKE: spawn a non-squad world NPC and ragdoll it,
            // so the user can SAVE a 'down1' that both clients then load.
            coop::engine::setupDownScene(gw);
        } else if (g_cfg.setupScene == "downhold") {
            // Body-state VALIDATION: the subject is already baked into the save - do
            // NOT spawn a duplicate. The periodic re-arm below keeps it down.
            coopLog("SETUP(downhold): no spawn - re-arm keeps baked down bodies down");
        } else if (g_cfg.setupScene == "duel") {
            // Combat (Stage 3c) BAKE: spawn two PEACEFUL non-squad NPCs (same faction)
            // so the user can SAVE a neutral 'duel1' that both clients load. The fight
            // is triggered live later (combat_order) so the join sees the transition.
            bool ok = coop::engine::setupDuelScene(gw);
            coopLog(ok ? "SETUP(duel): peaceful duelists spawned - SAVE 'duel1' now"
                       : "SETUP(duel): duelist spawn FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let the duelists settle/ground
        } else if (g_cfg.setupScene == "squad") {
            // Bidirectional presence (Phase 3.5) BAKE: build a SECOND player squad tab
            // so host (tab 0) and join (tab 1) each own a tab. User SAVEs e.g. 'squad1'.
            bool ok = coop::engine::setupSquadScene(gw);
            coopLog(ok ? "SETUP(squad): second squad tab built - SAVE your two-tab save now"
                       : "SETUP(squad): squad-tab build FAILED");
        } else if (g_cfg.setupScene == "squad3") {
            // 3-player fixture (protocol 49) BAKE: split a THIRD player squad tab
            // out of the loaded 2-tab save so ranks 0/1/2 (host / join 1 / join 2)
            // each own a member. With KENSHICOOP_BAKESAVE set (bake_scene.ps1
            // -Setup squad3 -BaseSave squad1 -BakeSave squad3) the save writes
            // automatically once the split settles.
            bool ok = coop::engine::setupSquad3Scene(gw);
            coopLog(ok ? "SETUP(squad3): third squad tab built - SAVE 'squad3' now"
                       : "SETUP(squad3): third-tab build FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let the split/AI settle
        } else if (g_cfg.setupScene == "splitfar") {
            // Far-apart desync fixture BAKE ('splitfar1'): park the non-leader
            // squad tab at a MEASURED remote point and save, so split_far opens
            // with the two tabs already in two different regions instead of
            // spiralling to whatever it happens to find.
            //
            // The target must be somewhere BOTH clients see population, which
            // means BAKED population. The spot where the bug was first caught
            // (-42161,1555,-2655: join enumerated 8, host 0) is exactly the
            // wrong choice - those 8 bodies existed only in the join's process,
            // so a bake, which writes the HOST's world, records an empty field
            // and the scenario has nothing to judge. Verified: baking there
            // yielded sep=9839 with popMover=0 on both sides.
            // This target held 17 NPCs on BOTH clients for a full window
            // (run 20260802_105738, hop 2), so the crowd is in the save.
            // Override with KENSHICOOP_SPLITFAR_AT="x,y,z".
            float sx = -50446.0f, sy = 841.0f, sz = -2652.0f;
            const char* at = getenv("KENSHICOOP_SPLITFAR_AT");
            if (at && *at) sscanf(at, "%f,%f,%f", &sx, &sy, &sz);
            unsigned int nm = coop::engine::setupSplitFarScene(gw, sx, sy, sz);
            char sb[176];
            _snprintf(sb, sizeof(sb) - 1,
                      "SETUP(splitfar): relocated %u member(s) to %.0f,%.0f,%.0f - SAVE 'splitfar1' now",
                      nm, sx, sy, sz);
            sb[sizeof(sb) - 1] = '\0'; coopLog(sb);
            // Longer than the other bakes: the destination zone has to stream in
            // and the relocated bodies have to ground before the save is written.
            if (nm > 0 && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 20000;
        } else if (g_cfg.setupScene == "bedcage") {
            // Bed+cage occupancy (protocol 19) BAKE: spawn a bed and a prison
            // cage near the leader so both clients load save-stable furniture
            // hands. With KENSHICOOP_BAKESAVE set the save is written
            // automatically a few seconds later (no manual menu round-trip).
            bool ok = coop::engine::setupBedCageScene(gw);
            coopLog(ok ? "SETUP(bedcage): bed + cage spawned - SAVE 'bedcage1' now"
                       : "SETUP(bedcage): spawn FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "pole") {
            // Prisoner-pole occupancy (protocol 19 kind=4) BAKE: spawn one
            // standing prisoner POLE in front of the leader so both clients load
            // a save-stable pole hand. The pole_put controlled test then KOs a PC
            // and setPrisonMode's it onto the pole (visibly a body ON A POLE, not
            // in a cage). With KENSHICOOP_BAKESAVE the save writes automatically.
            bool ok = coop::engine::setupPoleScene(gw);
            coopLog(ok ? "SETUP(pole): prisoner pole spawned - SAVE 'pole1' now"
                       : "SETUP(pole): spawn FAILED (no pole template? see candidates)");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "buffpc") {
            // Stat buff BAKE (single client): raise EVERY player-squad PC to 120 in
            // all stats, then leave the game running so the user can SAVE manually
            // (or auto-bake if KENSHICOOP_BAKESAVE is set). No coop peer required.
            unsigned int nb = coop::engine::buffAllPlayerStats(gw, 120.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SETUP(buffpc): buffed %u PC(s) to 120 in every stat - SAVE now", nb);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (nb > 0 && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 4000; // let the recalc settle
        } else if (g_cfg.setupScene == "inventory") {
            // Inventory (Phase 4a) BAKE: spawn a save-stable storage container in front
            // of the leader + seed it with items so both clients load an identical
            // container with a resolvable hand. User SAVEs e.g. 'inv1'.
            unsigned int ch[5] = { 0, 0, 0, 0, 0 };
            bool ok = coop::engine::setupInventoryScene(gw, ch);
            if (ok) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "SETUP(inventory): container hand=%u,%u,%u,%u,%u - SAVE 'inv1' now",
                ch[0], ch[1], ch[2], ch[3], ch[4]); b[sizeof(b) - 1] = '\0'; coopLog(b); }
            else coopLog("SETUP(inventory): container prep FAILED");
        } else {
            RootObject* seat = 0;
            bool ok = coop::engine::spawnSeatInFront(gw, 7.0f, 0.0f, &seat);
            if (ok && seat) {
                unsigned int h[5];
                if (coop::engine::readObjectHand(seat, h))
                    { char b[160]; _snprintf(b, sizeof(b)-1,
                        "SETUP: spawned seat hand=%u,%u,%u,%u,%u",
                        h[3], h[4], h[0], h[1], h[2]); b[sizeof(b)-1]='\0'; coopLog(b); }
                else coopLog("SETUP: spawned seat (hand unread)");
            } else {
                coopLog("SETUP: seat spawn FAILED (no seat template or createBuilding faulted)");
            }
            if (g_cfg.setupScene == "npc") {
                Character* npc = coop::engine::spawnNpcInFront(gw, 2.5f, 1.0f);
                coopLog(npc ? "SETUP: spawned world NPC" : "SETUP: NPC spawn FAILED");
            }
        }
        coopLog("SETUP: scene ready - arrange the pose and SAVE the game now");
    }
}

// Deferred auto-bake: write the fixture save once the armed settle window
// elapses. One-shot; the self-exit timer (KENSHICOOP_TEST_SECONDS) then ends
// the bake run.
void tickAutoBake() {
    if (g_bakeSaveTick != 0 && GetTickCount() >= g_bakeSaveTick) {
        g_bakeSaveTick = 0;
        bool ok = coop::engine::saveGameAs(g_cfg.bakeSave);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SETUP: auto-bake save '%s' %s",
                  g_cfg.bakeSave.c_str(), ok ? "ISSUED" : "FAILED");
        b[sizeof(b) - 1] = '\0'; coopLog(b);
    }
}

// Host re-arm + inventory-owner registration. Baked work goals / ragdoll state
// do not survive save/load and drift over time, so re-issue them on an interval;
// and (re)resolve the owned container so publishInventories streams its contents.
void tickHostRearm(GameWorld* gw) {
    // Craft re-arm (host): a baked work goal does not persist across save/load, and a
    // world worker's own AI can drift off-station. Re-issue the work goal on an
    // interval so the host keeps streaming the work task throughout the scene. The
    // call is throttled and no-ops when the worker is already on task, so it never
    // thrashes the worker's pathing. Only the 'craft' scene arms this.
    if (g_cfg.setupScene == "craft" && g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmCraftScene(gw);
    }

    // Down re-arm (host): a healthy ragdolled body recovers and stands back up, and
    // ragdoll state does not survive save/load. Re-knock the nearby non-squad bodies
    // on an interval so they stay on the ground throughout the scene. Both the bake
    // ('down') and the spawn-free validation ('downhold') arm this.
    if ((g_cfg.setupScene == "down" || g_cfg.setupScene == "downhold") &&
        g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmDownScene(gw);
    }

    // Inventory owner registration (host, Phase 4a): when inventory sync is active,
    // (re)resolve the baked storage container nearest the leader (else the leader's own
    // inventory) and register it as the owned container so publishInventories streams
    // its contents. Re-resolved on an interval because the container hand only resolves
    // once the world block has loaded. The join never registers (it reconciles).
    if (g_cfg.invSync && g_cfg.isHost && gw && g_gameStarted) {
        static DWORD lastInvReg = 0;
        if (GetTickCount() - lastInvReg >= (DWORD)CRAFT_REARM_MS) {
            lastInvReg = GetTickCount();
            unsigned int ch[5];
            if (coop::engine::pickInventoryContainer(gw, ch))
                g_repl.setOwnedContainerHand(ch);
        }
    }
}

// Scenario completion hold (harness): once a verdict is logged, keep driving the
// synced bodies on screen for the capture window, then self-exit cleanly.
void tickScenarioHold() {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_scenarioDoneTick != 0) {
        if (GetTickCount() - g_scenarioDoneTick >= SCENARIO_HOLD_MS) {
            coop::logClose();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
#endif
}

// Phase 6: build the per-tick channel call environment (SyncContext) from the
// long-lived globals. A cheap POD rebuilt per call, so localId() is read at the
// same point it always was (byte-identical to the old loose-arg calls).
static coop::SyncContext replCtx(GameWorld* gw) {
    coop::SyncContext c;
    c.gw = gw; c.net = &g_net; c.in = &g_inbound;
    c.localId = g_net.localId(); c.isHost = g_cfg.isHost;
    return c;
}

// Replication publish (pre-engine). BIDIRECTIONAL presence: both clients stream
// their OWNED squad subset (partitioned by hand-rank) and drive the peer's; the
// host additionally streams world NPCs. Ingest received targets BEFORE the engine
// tick so apply targets are current. worldLive is sampled ONCE pre-engine by the
// caller (see the comment there); every channel is gated by its own Config knob.
void tickReplicatePublish(GameWorld* gw, bool worldLive) {
    if (worldLive) {
        g_repl.ingest(g_inbound);
        // Phase 4a: drain received container-contents snapshots into the per-container
        // cache (reconciled after the engine tick by applyInventories).
        g_repl.ingestInv(g_inbound);
        // Both clients latch reliable transition events (KO/death/revive) for the bodies
        // they drive, before apply (a side can emit an event for its own owned body and
        // the peer that drives that body must honour it).
        g_repl.applyEvents(gw, g_inbound);
    }
    if (worldLive) {
        g_repl.publishOwned(gw, g_net, g_net.localId());
        // Phase W2: BOTH clients watch their OWNED characters for a WEAPON drop and author a
        // reliable conservation intent so the peer relocates its own copy of that weapon (a
        // weapon can't be rebuilt via the W1 proxy path). Bidirectional; gated on worldSync.
        // Ordered BEFORE publishInventories: the gear census is what tells the inventory
        // publisher a drop is still being adjudicated (wdPendingDrop), and running it first
        // means that signal is current rather than one tick stale. Otherwise a bag snapshot
        // can be authored ahead of the intent that explains it, and the peer's reconcile
        // destroys the copy the intent was going to relocate.
        if (g_cfg.worldSync)
            g_repl.detectAndPublishWeaponDrops(gw, g_net, g_net.localId());
        // Phase W3 convergence: prune ground-gear tracks whose object is gone (a stale
        // Item* is how a re-home silently no-ops, leaving the author's copy on the ground
        // as a duplicate) and finish any re-home the peer's pickup could not complete yet.
        // Ordered before publishInventories so a completed re-home ships in THIS tick's
        // snapshot; it only ever touches PEER-owned containers, so it cannot feed the
        // owned-character census above and loop.
        if (g_cfg.worldSync)
            g_repl.reconcileGroundGear(gw);
        // Both clients stream the contents of every squad member they OWN (host tab 0,
        // join tab 1) on content-change - bidirectional, disjoint by the same tab
        // partition as positional sync. Gated on invSync so ordinary co-op sessions add
        // no inventory traffic; the peer reconciles via applyInventories (skips own).
        if (g_cfg.invSync)
            g_repl.publishInventories(gw, g_net, g_net.localId());
        // Protocol 37: BOTH clients diff every tracked container (own + received)
        // against its baseline to catch a completed cross-owner UI drag - the one
        // inventory write the single-writer snapshots cannot represent - and author
        // a reliable PKT_INV_XFER so the peer relocates its own copy (conservation).
        // RETIRED by the trade veto (blockXfer): a refused drag can never complete,
        // so there is nothing to detect/replicate - Config forces xferSync off when
        // blockXfer is on, making this (and applyTransfers below) a no-op. The
        // xferLatch_/xferDefer_ reconcile-race machinery then stays dormant (never
        // populated). KENSHICOOP_BLOCK_XFER=0 restores this replicate-the-trade path.
        if (g_cfg.xferSync)
            g_repl.detectAndPublishTransfers(gw, g_net, g_net.localId());
        // Phase W1 (bidirectional): BOTH clients stream the free ground items they
        // author in their interest sphere - owner-scoped netId spaces, peer items
        // filtered by the proxy echo guard - so a join-side drop of materials/food
        // finally appears on the host. Proxies reconcile after the engine tick
        // (applyWorldItems, also both sides now).
        if (g_cfg.worldSync)
            g_repl.publishWorldItems(gw, g_net, g_net.localId());
        // Phase 2 (player combat + medical): owner-authoritative vitals sync for
        // player-squad members, both directions. publishMedical streams OUR
        // members' medical model (change-gated, reliable); applyMedical writes
        // received snapshots onto the peer copies we drive AND forwards any
        // local first aid administered on those copies back to their owner;
        // applyTreatments lands forwarded first aid on the bodies we own.
        // Ordered after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.medSync) {
            g_repl.publishMedical(gw, g_net, g_net.localId());
            g_repl.applyMedical(gw, g_inbound, g_net, g_net.localId());
            g_repl.applyTreatments(gw, g_inbound);
            // Join-dealt authoritative damage (protocol 45): the JOIN forwards the
            // damage its guarded melee would have dealt to driven world-NPC copies;
            // the HOST applies it to the real body (blood + a frontal flesh wound),
            // which the vitals stream then mirrors back. Decouples the join PC's
            // damage from its disrupted (position-driven) copy animation.
            if (g_cfg.isHost)
                g_repl.applyCombatHits(gw, g_inbound);
            else
                g_repl.publishCombatHits(gw, g_net, g_net.localId());
        }
        // Character stats sync (protocol 17): owner-authoritative CharStats
        // stream for player-squad members, both directions. publishStats
        // streams OUR members' stats (change-gated, reliable); applyStats
        // writes received snapshots onto the peer copies we drive. Ordered
        // after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.statsSync) {
            g_repl.publishStats(gw, g_net, g_net.localId());
            g_repl.applyStats(gw, g_inbound);
        }
        // Shared money pool (protocol 52): Kenshi keeps ONE player wallet, so
        // both players spend from one pool with the host as authority. Publish
        // samples the wallet and reports any local movement (join: a signed
        // delta, host: the authoritative total); apply folds deltas / adopts the
        // total. Ordered after publishOwned so isHostRole() reflects this tick's
        // ownership partition.
        if (g_cfg.moneySync) {
            g_repl.publishMoneyPool(replCtx(gw));
            g_repl.applyMoneyPool(replCtx(gw));
        }
        // Recruitment sync (protocol 23): drain the recruit detour's edge queue
        // into reliable EVT_RECRUIT events (subject = old hand, actor = new
        // hand) and pin recruited hands to their recruiter's ownership. The
        // receive half (re-key) lives in applyEvents above. Ordered after
        // publishOwned so this tick's recruits pin BEFORE next tick's census.
    if (g_cfg.recruitSync)
        g_repl.publishRecruits(gw, g_net, g_net.localId());

    // Squad management sync (protocol 35): poll the roster's pointer->hand
    // baseline (~2 Hz; a squad-tab move re-containers the body but the
    // Character* survives) and author reliable EVT_SQUAD_MOVE re-key edges,
    // pinning moved hands to the mover's ownership. The receive half (shared
    // EVT_RECRUIT re-key path) lives in applyEvents above.
    if (g_cfg.squadSync)
        g_repl.publishSquadMoves(gw, g_net, g_net.localId());

    // 小队控制授权（协议 1002，第 2 批）：发出发件箱（申请 / 同意 / 拒绝 / 收回 / 归还）
    // 并清理超时的挂起申请。squadSync 关时整体 no-op。
    g_repl.squadAuthTick(gw, g_net, g_net.localId());

    // Change-gated sampled channels (Phase 6c): faction relations (protocol 24),
    // baked doors (26), placed buildings (27), placed-building doors (28),
    // production machines (33), and research (38). Each is a self-contained
    // publish/apply pair sharing the ChangeGate policy + SyncContext; the
    // channel-descriptor registry inside driveSampledChannels owns their
    // per-channel enable, direction (symmetric detector vs host-authoritative),
    // and cadence order, so adding a sampled channel is one table row there -
    // not an edit to this tick. Ordered after recruit/squad (ownership pins)
    // and before stealth/speed/time, exactly as the explicit blocks were.
    g_repl.driveSampledChannels(replCtx(gw));
        // Stealth sync (protocol 20): the HOST is the world-detection authority
        // - it streams each DRIVEN sneaker's whoSeesMeSneaking back to the
        // sneaker's owner; every client replays received snapshots onto the
        // bodies it OWNS (the indicators render on the owner's screen). The
        // posture half lives inside applyTargets (continuous BODY_SNEAK apply).
        if (g_cfg.stealthSync) {
            if (g_cfg.isHost)
                g_repl.publishStealth(gw, g_net, g_net.localId());
            g_repl.applyStealthFeedback(gw, g_inbound);
        }
        // Consensus game-speed sync: detect local speed clicks as REQUESTS,
        // host arbitrates effective = min(requests) (capped at 1x while either
        // player squad fights) and broadcasts; the join applies the SET. Runs
        // after publishOwned (the combat flag samples the ownHands_ set).
        if (g_cfg.speedSync)
            g_repl.syncSpeed(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
        // Phase 6 (6a evidence spike): env-gated ([shackledbg]) per-character
        // shackle/lock trace. No-op unless KENSHICOOP_DEBUG_SHACKLE=1, so it is
        // free to leave in the tick for manual-session characterization.
        coop::engine::shackleDbgTick(gw, g_cfg.isHost);
        // Game-clock sync (protocol 25): the host broadcasts its absolute
        // in-game clock ~1 Hz; the join measures the offset and SLEWS - a
        // multiplier the speed layer's quiet writes fold in on top of the
        // arbitrated consensus effective. AFTER syncSpeed so a slew change
        // applies against this tick's consensus state.
        if (g_cfg.timeSync)
            g_repl.syncTime(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
        // Runtime-spawn proxy replication (protocol 21): the join asks about
        // streamed hands it couldn't resolve last tick (host RUNTIME spawns)
        // and mints local proxy bodies from the host's replies; the host
        // answers requests. BEFORE the engine tick so a proxy bound this
        // frame is driven by this frame's applyTargets.
        if (g_cfg.spawnSync)
            g_repl.syncSpawns(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
    }
}

// Coordinated save + load (protocols 31/32) run under g_gameStarted, NOT
// worldLive: they must keep pumping DURING a world swap (the join drains the
// host's LOAD_GO and issues its own load; the host's fallback transfer and the
// deferred-signal state advance) and they only touch net packets, file IO and
// the load/save ENTRY points - never a cached world pointer the swap invalidates.
void tickCoordinatedSaveLoad(GameWorld* gw) {
    if (g_gameStarted) {
        // Coordinated save + session resume (protocol 31): host-arbitrated
        // save edges, folder-quiescence completion, paced in-band folder
        // transfer, staged+verified commit on the join.
        if (g_cfg.saveSync || g_cfg.isHost)
            driveSaveSync();
        // Coordinated load (protocol 32): host-arbitrated load edges,
        // fingerprint-verified join follow, SaveXfer fallback on divergence.
        if (g_cfg.loadSync)
            driveLoadSync(gw);
    }
}

#ifdef KENSHICOOP_HARNESS
// The squad-tab rank this client owns, for ScenarioContext.ownRank: lowest of
// the resolved ownership set (the Replicator's partition), falling back to the
// classic role default when the set is empty. Protocol 49: join 2 resolves 2.
static unsigned int scenarioOwnRank() {
    if (!g_cfg.ownRanks.empty()) return *g_cfg.ownRanks.begin();
    return g_cfg.isHost ? 0u : 1u;
}
#endif // KENSHICOOP_HARNESS

// Scenario onStart (harness) fires once, BEFORE the engine tick, so a host-issued
// move order takes effect this frame.
//
// PEER-READY ARMING: the scenario clock does NOT start at gameplay start - it
// starts when this client first receives a peer's owned-entity batch
// (Inbound::sawRemoteEntity). On the HOST that flips true exactly when the JOIN
// is loaded + streaming, so every scripted host action (orders, kills, item adds
// - all timed off ctx.elapsedMs) happens with the join watching, instead of
// racing its load screen. On the JOIN the host's stream is already arriving by
// the time it reaches gameplay, so it arms ~immediately - which also aligns both
// clients' scenario clocks to roughly the same wall moment. Fallback: arm anyway
// after scenarioArmTimeoutMs of gameplay (peer never connected / host-only
// diagnostics); 0 = arm immediately (spike runs).
void tickScenarioStart(GameWorld* gw) {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_gameStarted && gw && !g_scenarioStarted) {
        // Protocol 49: with KENSHICOOP_ARM_MIN_PEERS > 1 (3-player runs) the arm
        // edge waits for owned-entity batches from that many DISTINCT peers, so
        // every participant's scenario clock starts only once the LAST one is
        // live. Default 1 == the classic sawRemoteEntity edge.
        bool  peerReady = (g_cfg.scenarioArmMinPeers <= 1)
                              ? g_inbound.sawRemoteEntity()
                              : (g_inbound.remoteEntityOwners() >= g_cfg.scenarioArmMinPeers);
        DWORD waitedMs  = GetTickCount() - g_gameStartTick;
        bool  fallback  = (g_cfg.scenarioArmTimeoutMs == 0) ||
                          (waitedMs >= (DWORD)g_cfg.scenarioArmTimeoutMs);
        // Pre-arm phase: every tick until arming, let the scenario pin/hold its
        // subjects while the freshly-loaded world is still in its baked pose
        // (waiting a join-load before pinning loses wandering NPCs).
        {
            coop::ScenarioContext pctx;
            pctx.gw = gw; pctx.isHost = g_cfg.isHost; pctx.localId = g_net.localId();
            pctx.elapsedMs = waitedMs; pctx.tick = g_scenarioTick;
            pctx.peerReady = peerReady;
            pctx.pickMintedProxy = &coopScenarioPickMintedProxy;
            pctx.ownRank = scenarioOwnRank();
            g_scenario->onGameplay(pctx);
        }
        if (peerReady || fallback) {
            g_scenarioStarted   = true;
            g_scenarioStartTick = GetTickCount();
            coop::ScenarioContext ctx;
            ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
            ctx.elapsedMs = 0; ctx.tick = g_scenarioTick;
            ctx.peerReady = peerReady;
            ctx.pickMintedProxy = &coopScenarioPickMintedProxy;
            ctx.ownRank = scenarioOwnRank();
            char m[200];
            _snprintf(m, sizeof(m) - 1, "SCENARIO arm trigger=%s waitedMs=%lu",
                      peerReady ? "peer-ready" : "timeout", (unsigned long)waitedMs);
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            _snprintf(m, sizeof(m) - 1, "SCENARIO %s start", g_scenario->name());
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenario->onStart(ctx);
        }
    }
#endif // KENSHICOOP_HARNESS
}

// Replication apply (post-engine) so our transform is the last word the renderer
// samples (the local AI re-decides at the start of the next tick). Both clients
// drive the PEER's owned bodies; applyTargets skips our OWN owned hands. Re-sample
// live: the engine tick may have STARTED a swap (gameplay just went non-live), in
// which case the caches are now stale - skip apply this tick too (the reset runs
// next tick's top on the reload edge).
void trackMove(GameWorld* gw);   // KENSHICOOP_TRACK_MOVE route recorder (below)

void tickReplicateApply(GameWorld* gw, bool worldLive) {
    if (worldLive && coop::engine::gameplayLive(gw)) {
        g_repl.applyTargets(gw);
        // Phase W2: relocate our own copy of any DROPPED weapon to the ground BEFORE the
        // inventory reconcile runs, so the conservation move beats the (debounced) removal
        // reconcile that would otherwise DESTROY the weapon we cannot refabricate.
        if (g_cfg.worldSync) {
            g_repl.applyWeaponDrops(gw, g_inbound);
            // Phase W3: re-home tracked ground copies into the picking character's bag, also
            // before the inventory reconcile (which can't refabricate a weapon into the proxy).
            g_repl.applyWeaponPickups(gw, g_inbound);
        }
        // Protocol 37: relocate our copy of any cross-owner TRADED item between the
        // two containers BEFORE the inventory reconcile, so the conservation move
        // beats the stale-snapshot dupe/wipe (and traded gear survives - no
        // fabrication on this path).
        if (g_cfg.xferSync) {
            g_repl.applyTransfers(gw, g_inbound, g_net, g_net.localId());
            // Protocol 50: settle our own outstanding intents on the receiver's
            // verdict. Before the reconcile, because a rejected transfer works
            // by DROPPING our latch and letting applyInventories restore the
            // owner's version - one tick later and the reconcile would run once
            // more while still defending a move that was refused.
            g_repl.applyXferAcks(gw, g_inbound, g_net.localId());
        }
        // Phase 4a: reconcile any peer-owned container we received a fresh snapshot
        // for (the join applies the host's container; the host skips its own).
        g_repl.applyInventories(gw);
        // Phase W1 (bidirectional): BOTH clients spawn/update/cull local proxies for
        // the peer's streamed ground items, keyed (ownerId, netId) so the per-sender
        // netId spaces never collide.
        if (g_cfg.worldSync) {
            g_repl.applyWorldItems(gw, g_inbound);
            // Protocol 47: a peer consumed a proxy it held for one of OUR ground items,
            // so destroy our real copy. Runs AFTER applyWorldItems so a snapshot and the
            // claim that retires it in the same batch resolve in authored order.
            g_repl.applyWorldClaims(gw, g_inbound, g_net.localId());
        }
        // Host-authoritative world: only the JOIN hides/freezes any local NPC the
        // host isn't streaming (so the join can't run a divergent copy). The host IS
        // the world authority, so it never suppresses.
        // NPC existence census (protocol 36): the host broadcasts its 1 Hz
        // wide-radius hand list; the join drains it and lets the wide-radius
        // pass in enforceHostAuthority cull local-only ghosts at render range.
        // Under presence authority both halves run on BOTH sides: each client
        // authors the cells it stands in and judges its local copies of the
        // other's. With cellAuth off the two conditions collapse to today's
        // exclusive split, which is what makes the fail-open A/B meaningful.
        if (!g_cfg.isHost || g_cfg.cellAuth) {
            g_repl.applyNpcCensus(gw, g_inbound);
            g_repl.enforceHostAuthority(gw, g_net.localId());
        }
        if (g_cfg.isHost || g_cfg.cellAuth) {
            g_repl.publishNpcCensus(gw, g_net, g_net.localId());
        }
        // Camera-anchored interest (protocol 43): both sides publish their
        // LOCAL camera to the engine's anchor store, ship its center to the
        // peer at ~1 Hz, and fold a fresh peer hint into interestCenters.
        // Runs even with CAM_INTEREST off (the engine-side knob makes
        // interestCenters ignore the anchors) so an A/B toggle needs no
        // session restart logic.
        g_repl.syncCamHint(gw, g_inbound, g_net, g_net.localId());
        g_repl.syncCellClaims(gw, g_inbound, g_net, g_net.localId());
        trackMove(gw);
    }
}

// KENSHICOOP_TRACK_MOVE=1: log one line per player squad TAB per second - where
// its first member is and whether it is moving. Log-only; gates nothing.
//
// It exists because the cell claims are far too sparse to reconstruct a walked
// route. A claim fires when a tab crosses a 4608 u cell boundary, so a 99,600 u
// crossing left 29 points, and run_apart's attempt to walk between consecutive
// ones wedged the pair at -48980,-5670: a straight line between two cell-crossing
// points is not a path, because the human who produced them walked AROUND
// whatever sits between. A 1 Hz track is ~500 points over the same crossing,
// which is a route, and it also shows plainly whether a single distant click
// routes across the map at all - the question run_apart is currently guessing at.
void trackMove(GameWorld* gw) {
    if (!g_cfg.trackMove || !gw) return;
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (lastTick != 0 && (now - lastTick) < 1000) return;
    lastTick = now;

    const unsigned int MAXB = 32;
    coop::EntityState sq[MAXB];
    unsigned int n = coop::engine::captureSquad(gw, false, sq, MAXB);
    // One line per tab, keyed on the container hand: every squad member would be
    // six lines a second of the same information.
    unsigned int seenC[8], seenS[8];
    unsigned int nSeen = 0;
    for (unsigned int i = 0; i < n && nSeen < 8; ++i) {
        bool dup = false;
        for (unsigned int k = 0; k < nSeen; ++k)
            if (seenC[k] == (unsigned int)sq[i].hContainer &&
                seenS[k] == (unsigned int)sq[i].hContainerSerial)
                dup = true;
        if (dup) continue;
        seenC[nSeen] = (unsigned int)sq[i].hContainer;
        seenS[nSeen] = (unsigned int)sq[i].hContainerSerial;
        ++nSeen;
        int cx = 0, cz = 0;
        int haveCell = coop::engine::cellAt(gw, sq[i].x, sq[i].z, &cx, &cz) ? 1 : 0;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[track] tab=%u,%u pos=%.0f,%.0f,%.0f moving=%u speed=%.1f "
                  "cell=%d(%d,%d)",
                  (unsigned)sq[i].hContainer, (unsigned)sq[i].hContainerSerial,
                  sq[i].x, sq[i].y, sq[i].z, (unsigned)sq[i].cMoving, sq[i].cSpeed,
                  haveCell, cx, cz);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// Coordinated load (protocol 32): deferred-signal backstop. The engine usually
// consumes the LOADGAME signal on its own within ~0.5 s (load_probe run 2), but
// run 1 saw it sit forever - if the swap hasn't begun after the grace window,
// pump SaveManager::execute() once from this end-of-tick context (the engine tick
// above is done with the world).
void tickLoadPumpBackstop(GameWorld* gw) {
    if (g_loadPumpArmTick != 0 && g_cfg.loadSync) {
        if (g_swapStartTick != 0) {
            g_loadPumpArmTick = 0; // the swap started on its own
        } else if (GetTickCount() - g_loadPumpArmTick >= LOAD_PUMP_GRACE_MS) {
            g_loadPumpArmTick = 0;
            int delay = -1;
            int sig = coop::engine::saveMgrSignal(&delay);
            if (sig == 2 /*LOADGAME*/) {
                bool ok = coop::engine::saveMgrExecute();
                int sigAfter = coop::engine::saveMgrSignal(0);
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] EXEC pumped (deferred LOADGAME stalled) ok=%d sigAfter=%d",
                          ok ? 1 : 0, sigAfter);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // If execute() swapped the world SYNCHRONOUSLY (signal consumed
                // and we're still live), the reload-edge detector never saw a
                // non-live frame, so it won't run the session reset - force it
                // here. The caches from the OLD world are dangling right now.
                if (ok && sigAfter != 2 && g_swapStartTick == 0 &&
                    coop::engine::gameplayLive(gw)) {
                    coopLog("[load] synchronous execute swap - forcing session reset");
                    sessionResetForWorldReload();
                }
            }
        }
    } else if (g_loadPumpArmTick != 0) {
        g_loadPumpArmTick = 0;
    }
}

// Scenario onTick (harness) AFTER apply, so a join's RECV line reflects the
// applied pos; on completion it emits the smoothness summary + verdict and begins
// the capture hold.
void tickScenarioTick(GameWorld* gw) {
#ifdef KENSHICOOP_HARNESS
    if (g_scenario && g_gameStarted && gw && g_scenarioStarted && g_scenarioDoneTick == 0) {
        coop::ScenarioContext ctx;
        ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
        ctx.elapsedMs = GetTickCount() - g_scenarioStartTick;
        ctx.tick = ++g_scenarioTick;
        ctx.peerReady = g_inbound.sawRemoteEntity();
        ctx.pickMintedProxy = &coopScenarioPickMintedProxy;
        ctx.ownRank = scenarioOwnRank();
        if (g_scenario->onTick(ctx)) {
            // Stage 2: the receiver emits its interpolation smoothness summary
            // alongside the verdict so the runner can assert per-frame gliding.
            if (!g_cfg.isHost) g_repl.logSmoothSummary();
            bool ok = g_scenario->passed();
            char m[48];
            _snprintf(m, sizeof(m) - 1, "SCENARIO RESULT %s", ok ? "PASS" : "FAIL");
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenarioDoneTick = GetTickCount(); // begin the capture hold
        }
    }
#endif // KENSHICOOP_HARNESS
}

void mainLoop_hook(GameWorld* gw, float dt) {
    ++g_tick;
    g_lastGw = gw; // cache for the argument-less F2 UI callbacks

#ifdef KENSHICOOP_HARNESS
    // TEST-ONLY (KENSHICOOP_TEST_CRASH=<seconds>): fault on purpose, to prove the
    // crash-dump filter actually produces a dump with our frames in it. A crash
    // handler nobody has ever seen fire is a coin flip, and the run it would waste
    // is a human reproducing a rare bug. Deliberately NOT wrapped in SEH.
    {
        static int crashAfter = -1;
        if (crashAfter < 0) {
            const char* e = getenv("KENSHICOOP_TEST_CRASH");
            crashAfter = e ? atoi(e) : 0;
        }
        if (crashAfter > 0 && g_gameStarted &&
            (GetTickCount() - g_gameStartTick) >= (DWORD)(crashAfter * 1000)) {
            coopLog("[crash] TEST-CRASH: faulting on purpose now");
            volatile int* p = (int*)0x80; // same shape as the 2026-08-03 fault
            *p = 1;
        }
    }
#endif

    coopPanelDrive(gw);

    // Protocol 32 world-swap edge detection + session reset. Runs FIRST so the
    // reload edge lands before any sync code touches pointers from the torn-down
    // world this tick.
    tickWorldSwapEdge(gw);

    if (!g_gameStarted && coop::engine::gameplayLive(gw)) {
        g_gameStarted   = true;
        g_gameStartTick = GetTickCount();
        coopLog("KenshiCoop: gameplay started");
        // Coordinated load (protocol 32): the title-screen auto-load fired the
        // load detour BEFORE gameplay - discard its queued edge here, or the
        // first driveLoadSync tick (g_gameStarted now true) would mistake it
        // for a mid-session host load and broadcast a spurious PKT_LOAD_GO.
        {
            coop::engine::LoadEdge stale[8];
            unsigned int n = coop::engine::drainLoadEdges(stale, 8);
            if (n > 0) {
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "[load] %u pre-gameplay load edge(s) discarded (title-screen auto-load)",
                          n);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            }
        }
        // Speed-intent capture (vote/effective decoupling): detour the engine's
        // speed setters so every USER action (button, keyboard pause, simulated
        // click) registers as a vote, while our own quiet applies stay invisible.
        // Installed at gameplay start so the seed reads the save's live speed.
        if (coop::engine::installSpeedIntentHooks(gw))
            coopLog("[speed] intent hooks installed (setGameSpeed/userPause/togglePause)");
        else
            coopLog("[speed] FAILED to install intent hooks (vote capture degraded)");
        // "Multiplayer+ (Wanderer x2)" start: give both wanderers the stat line the start
        // promises. Raise-only, so it is a FLOOR at 50 - it re-runs on every load
        // of such a world and is a no-op once the characters have trained past it.
        //
        // Gated on having no peer: buffAllPlayerStats walks EVERY squad tab, which
        // on a connected join includes the host's bodies, and writing those fights
        // the owner-authoritative stats channel (protocol 17). Solo at gameplay
        // start the write is purely local, and the join receives it as part of the
        // host's world through the connect-push below.
        if (g_peersPresent.empty() &&
            coop::engine::playerSquadHasTemplate(gw, WPX2_MARKER_SID)) {
            unsigned int nb = coop::engine::buffAllPlayerStats(gw, WPX2_STAT_LEVEL);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "[start] Multiplayer+ (Wanderer x2): raised %u PC(s) to %d in every stat",
                      nb, (int)WPX2_STAT_LEVEL);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
        // Push-save-on-connect ordering: if a peer connected while we were still
        // Push-save-on-connect ordering: if peers connected while we were still
        // at the menu / loading, their connect edges could not bake a save (no
        // live world yet). Now that gameplay is live, arm the connect-push for
        // every waiting join so each gets pulled into this world (one bake,
        // one targeted GO per join - protocol 49).
        if (g_cfg.isHost && g_cfg.saveSync && !g_peersPresent.empty())
            for (std::set<coop::u32>::iterator pi = g_peersPresent.begin();
                 pi != g_peersPresent.end(); ++pi)
                armConnectPush(*pi);
    }

    // Manual-validation helper (host only): KENSHICOOP_AUTORECRUIT=N seconds -
    // ONCE, N s after gameplay settles, programmatically recruit the nearest
    // non-player world NPC. probeRecruit -> recruitNpc -> g_recruitFn is the
    // SAME PlayerInterface::recruit the dialog "join me" outcome hits, so
    // recruit_hook authors the edge and the whole recruit-sync path runs. Lets
    // us exercise recruit on any populated save (camp/squad1) with no dialog-
    // hireable NPC. OFF by default (0 = no-op, zero cost).
    if (g_cfg.isHost && g_gameStarted) {
        static int  autoRecruitS    = -1;
        static bool autoRecruitDone = false;
        if (autoRecruitS < 0) {
            const char* e = std::getenv("KENSHICOOP_AUTORECRUIT");
            autoRecruitS = e ? std::atoi(e) : 0;
        }
        if (autoRecruitS > 0 && !autoRecruitDone &&
            (GetTickCount() - g_gameStartTick) >= (DWORD)autoRecruitS * 1000u) {
            autoRecruitDone = true;
            unsigned int hb[5], ha[5];
            // Wide reach (600u): a caged/imprisoned player squad (the camp save)
            // sits well beyond the scenario's 60u nearest-NPC probe from the
            // guards/prisoners it should recruit.
            int res = coop::engine::probeRecruit(gw, false, hb, ha, 600.0f);
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                "[recruit] AUTORECRUIT res=%d before=%u,%u,%u,%u,%u "
                "after=%u,%u,%u,%u,%u", res, hb[0], hb[1], hb[2], hb[3], hb[4],
                ha[0], ha[1], ha[2], ha[3], ha[4]);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Test-runner self-exit: quit cleanly after the configured duration so
    // unattended runs terminate on their own and flush their logs. Also a hard
    // backstop for a scenario that never reports completion.
    if (g_cfg.testSeconds > 0 && g_gameStarted &&
        (GetTickCount() - g_gameStartTick) >= (DWORD)g_cfg.testSeconds * 1000u) {
        coopLog("KenshiCoop: test duration elapsed; exiting");
        coop::logClose();
        // TerminateProcess (not ExitProcess): we're inside the live game loop
        // with GPU/audio/net threads running; orderly teardown deadlocks on the
        // loader lock. Our log is already flushed/closed above.
        TerminateProcess(GetCurrentProcess(), 0);
    }

    processNetEvents(gw);

    // Coordinated load (protocol 32): the JOIN never loads locally while a
    // peer is connected under the gate - the host arbitrates; a manual load
    // press forwards as PKT_LOAD_REQ. Evaluated per tick (not just on the
    // connect edge) so the title-screen auto-load - which precedes gameplay -
    // is never swallowed.
    {
        bool want = !g_cfg.isHost && g_cfg.loadSync &&
                    g_peersPresent.count(0u) != 0 && g_gameStarted;
        if (want != g_loadSuppressOn) {
            g_loadSuppressOn = want;
            coop::engine::setLoadSuppress(want);
            coopLog(want ? "[load] JOIN load suppression ON (host arbitrates loads)"
                         : "[load] JOIN load suppression OFF");
        }
    }

    // Test-scene setup: host spawns the controlled scene ONCE, a few seconds
    // after gameplay starts, then leaves the game running so the user can arrange
    // the pose and SAVE (baking save-stable hands that resolve on both clients).
    tickSetupScene(gw);

    // Deferred auto-bake: write the fixture save once the armed settle window
    // elapses (one-shot); the self-exit timer then ends the bake run.
    tickAutoBake();

    // Host re-arm + inventory-owner registration: keep craft/down scenes on task
    // across save/load drift and (re)resolve the owned container for invSync.
    tickHostRearm(gw);

    // Scenario completion hold: once a verdict is logged, keep driving the synced
    // bodies on screen for the capture window, then self-exit cleanly. (harness)
    tickScenarioHold();

    // Coordinated load (protocol 32): while a world swap is in progress the
    // OLD world is being torn down under us - every cached Character*/
    // RootObject* is dangling, and mainLoop_hook keeps ticking throughout
    // (~500 ticks across a ~4 s load). Touching those pointers mid-swap is a
    // pure-virtual-call crash (a virtual on a half-destroyed object). Skip
    // ALL replication until the reload edge (gameplay live again) runs the
    // session reset at the top of the hook, which clears the stale caches -
    // the next live tick then resumes replication with a fresh world.
    // Sampled ONCE (pre-engine): if the engine tick below completes the
    // reload, the reset still runs first NEXT tick, so we stay hands-off this
    // tick either way. Before first gameplay this is simply false (no caches
    // yet). driveLoadSync itself is NOT gated on this - it only touches net
    // packets + file IO + loadSave, never cached world pointers.
    const bool worldLive = g_gameStarted && coop::engine::gameplayLive(gw);

    // Build capture runs inside an engine factory call, not on our tick, so it
    // needs worldLive handed to it: a world load creates every building in the
    // save through that same factory and none of them are player placements.
    coop::engine::setBuildCaptureArmed(worldLive);

    // Replication publish (pre-engine, worldLive-gated): ingest received targets,
    // then stream every owned channel so applied state is current this tick.
    // 联机会话中（网络在运行）对方小队即使对方掉线也保持锁定，纯单人不锁
    g_repl.setSessionLive(g_net.isRunning());
    if (worldLive && g_net.isRunning()) g_repl.syncOwners(gw, g_net, g_inbound, g_cfg.isHost);
    driveCharacterCreation(gw, worldLive);
    if (!g_characterStatus.empty() && g_characterStatus != g_characterNotified) {
        g_characterNotified = g_characterStatus; g_toastText = g_characterNotified.c_str();
        g_toastState = 1; g_toastArmMs = GetTickCount();
    }
    tickReplicatePublish(gw, worldLive);

    // Coordinated save + load (protocols 31/32): run under g_gameStarted (NOT
    // worldLive) so they keep pumping DURING a world swap - they only touch net
    // packets, file IO and the load/save ENTRY points, never a cached world ptr.
    tickCoordinatedSaveLoad(gw);

    // Scenario onStart (harness): pre-arm hold + peer-ready/timeout arm, fired
    // once BEFORE the engine tick so a host-issued order takes effect this frame.
    tickScenarioStart(gw);

    g_mainLoop_orig(gw, dt); // run the engine (incl. local AI)

    // 选中限制兜底：收回授权的瞬间 / 不走 objectSelected 的路径，也要保证“选中集合 >1 时
    // 不含对方角色”。worldLive 门控，避免世界切换时碰旧指针。
    if (worldLive) coop::engine::enforcePeerSelection(gw);

    // 好友带自己的角色加入：只给带入角色的 owner 延迟重推一次存档。不同玩家的
    // 角色到达时间可以不同，不能因为其中一个的新标签把所有人一起拉回读档。
    if (worldLive && g_cfg.isHost && g_cfg.saveSync) {
        coop::u32 owner = 0;
        DWORD now = GetTickCount();
        while (g_repl.takePeerBodyTabOwner(&owner)) {
            if (g_peersPresent.count(owner) == 0) continue;
            if (g_peerBodyRepushDue.find(owner) == g_peerBodyRepushDue.end())
                g_peerBodyRepushDue[owner] = now;
        }
        for (std::map<coop::u32, DWORD>::iterator pi = g_peerBodyRepushDue.begin();
             pi != g_peerBodyRepushDue.end(); ) {
            if (g_peersPresent.count(pi->first) == 0) {
                pi = g_peerBodyRepushDue.erase(pi);
                continue;
            }
            if ((now - pi->second) < 2000) { ++pi; continue; }
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "[boot] peer body arrived -> re-push save target=%u",
                      (unsigned)pi->first);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            armConnectPush(pi->first);
            pi = g_peerBodyRepushDue.erase(pi);
        }
    }

    // Replication apply (post-engine): our transform is the last word the renderer
    // samples. Re-checks gameplayLive so a swap STARTED by this engine tick skips
    // apply (its caches are now stale; the reset runs next tick's reload edge).
    tickReplicateApply(gw, worldLive);

    // Coordinated load deferred-signal backstop: if the LOADGAME signal stalls
    // past the grace window, pump SaveManager::execute() once from end-of-tick.
    tickLoadPumpBackstop(gw);

    // Scenario onTick (harness) AFTER apply, so a join's RECV line reflects the
    // applied pos; latches the verdict + begins the capture hold on completion.
    tickScenarioTick(gw);
}

// Title-screen update hook: a safe main-thread point every frame the menu is up.
// Wait g_cfg.autoLoadDelayMs after the first title frame AND until the save
// subsystem reports ready, then issue the deferred load once.
// SEH-guarded so a fault in the (title-screen-untested) GUI stack can never
// abort titleUpdate_hook before the bootstrap pump runs. No C++ unwind objects
// live in titleUpdate_hook, but coopPanelDrive uses std::string internally, so
// the guarded call lives in its own function (C2712).
void coopPanelDriveSeh() {
    __try { coopPanelDrive(0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            coopErr("[coop-ui] panel tick FAULTED at title screen (guarded)"); }
    }
}

void titleUpdate_hook(TitleScreen* self) {
    g_titleUpdate_orig(self);

    // Bring-up trace: log once on the first title tick, then only when the
    // online state flips (the moment the user toggles ONLINE via F2), so the log
    // shows the menu hook is live and pinpoints the connect edge without spam.
    {
        static int s_lastRunning = -1;
        int running = g_net.isRunning() ? 1 : 0;
        if (running != s_lastRunning) {
            s_lastRunning = running;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "[boot] title-tick running=%d host=%d started=%d saveEmpty=%d",
                      running, g_cfg.isHost ? 1 : 0, g_gameStarted ? 1 : 0,
                      g_cfg.save.empty() ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Push-save-on-connect (join, main menu). A join that has gone ONLINE must
    // drain the connect edge, receive the host's pushed save, and load it while
    // still at the title screen - work that normally only runs in-game from
    // mainLoop_hook (gated on g_gameStarted). Pump the join half FIRST (before
    // the panel) so a GUI fault can never block it. gw is null: processNetEvents
    // only touches it under a gw&& guard, and the JOIN branches of driveSaveSync/
    // driveLoadSync never deref it. The load path is gated on savesReady():
    // before then the host's LOAD_GO simply waits in the inbound queue (no NACK
    // -> no stream yet), and the save-receiver half still commits chunks to disk.
    if (g_net.isRunning() && !g_cfg.isHost && !g_gameStarted) {
        processNetEvents(0);
        if (g_cfg.saveSync) driveSaveSync();
        if (g_cfg.loadSync && coop::engine::savesReady()) driveLoadSync(0);
        // F2 panel + status banner while the join waits at the menu (guarded; the
        // host's world is the destination, so we skip the config auto-load for a
        // join session).
        coopPanelDriveSeh();
        return;
    }

    // Co-op panel (F2) at the main menu for the HOST / offline case: lets a user
    // toggle ONLINE and paste a Steam ID before any save is loaded. Guarded
    // because the title-screen GUI stack is otherwise unexercised.
    coopPanelDriveSeh();

    if (g_autoLoadDone || g_cfg.save.empty()) return;

    DWORD now = GetTickCount();
    if (g_titleFirstTick == 0) { g_titleFirstTick = now; return; }
    if ((now - g_titleFirstTick) < g_cfg.autoLoadDelayMs) return;
    if (!coop::engine::savesReady()) return;

    if (coop::engine::loadSave(g_cfg.save)) {
        g_autoLoadDone = true;
        char m[128];
        _snprintf(m, sizeof(m) - 1,
                  "KenshiCoop: auto-load issued for save '%s' (after %lu ms settle)",
                  g_cfg.save.c_str(), (unsigned long)(now - g_titleFirstTick));
        m[sizeof(m) - 1] = '\0';
        coopLog(m);
    }
}

void startNetworking() {
    // Debug WAN simulation: when configured, hold/drop inbound entity batches so the
    // loopback harness exercises the real-latency path (interp + local enforcement)
    // instead of the ~0 ms same-frame delivery we'd otherwise validate against.
    if (g_cfg.netSimDelayMs || g_cfg.netSimJitterMs || g_cfg.netSimLossPct) {
        g_net.setNetSim(g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: NET SIM on - delay=%ums jitter=+/-%ums loss=%u%%",
                  g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Steam reachability spike (channel 1): independent of the transport, so a
    // UDP session can still prove Steam P2P punch-vs-relay against a peer.
    if (g_cfg.steamPing != 0) {
        if (coop::steamp2p::init()) coop::steamp2p::setPingPeer(g_cfg.steamPing);
        else coopErr("[steam] KENSHICOOP_STEAM_PING set but Steam init failed");
    }

    g_net.setSteamTransport(0);
    if (g_cfg.transport == "steam") {
        std::vector<unsigned long long> ids = g_cfg.steamPeers;
        if (!g_cfg.isHost) {
            if (ids.empty() && g_cfg.steamPeer) ids.push_back(g_cfg.steamPeer);
            if (!ids.empty()) { ids.resize(1); g_cfg.steamPeer = ids[0]; }
        }
        else if (ids.empty() && g_cfg.steamPeer) ids.push_back(g_cfg.steamPeer);
        if (!g_cfg.isHost && ids.empty()) {
            coop::setNetUiError("No host Steam ID - paste it in the F2 panel"); return;
        }
        if (!coop::steamp2p::init()) {
            coop::setNetUiError("Steam is unavailable - start Steam and reconnect"); return;
        }
        coop::steaminvite::init(&coopUiConnect);
        coop::steamp2p::setPeers(ids.empty() ? 0 : &ids[0], (unsigned int)ids.size(), g_cfg.isHost);
        g_net.setSteamTransport(g_cfg.isHost ? coop::steamp2p::selfId() : ids[0]);
        coopLog("[room] Steam transport armed; host accepts direct friends and manual allowlist");
    }

    bool useSteamId = g_cfg.transport == "steam";
    coop::u64 identity = useSteamId ? coop::steamp2p::selfId() : g_cfg.localPlayerId;
    std::string name = useSteamId && coop::steamp2p::selfName()[0]
                         ? coop::steamp2p::selfName() : g_cfg.playerName;
    g_net.setIdentity(identity, name);
    g_repl.setPlayerIdentity(identity);
    bool ok;
    if (g_cfg.isHost) {
        coopLog("KenshiCoop: starting as HOST");
        ok = g_net.startHost(g_cfg.port, &g_inbound);
        // Tailnet/LAN game browser: a UDP host answers discovery probes on the
        // dedicated port so joins can Scan instead of typing an address. Steam
        // transport skips it (the advertised UDP endpoint would be meaningless);
        // a bind failure (second host on this machine) only disables discovery.
        if (ok && g_cfg.discovery && g_cfg.transport != "steam") {
            if (coop::disc::responderStart((unsigned short)g_cfg.discPort))
                coop::disc::responderAdvert((unsigned short)g_cfg.port,
                                            1 + (unsigned int)g_peersPresent.size(),
                                            coop::MAX_PLAYERS, g_cfg.save.c_str());
        }
    } else {
        // Protocol 49: carry our squad-slot claim in HELLO so the host can
        // enforce slot uniqueness (and evict our own crash ghost on rejoin).
        coop::u32 claim = g_cfg.ownRanks.empty() ? 1u : *g_cfg.ownRanks.begin();
        g_net.setClaimRank(claim);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: starting as CLIENT (squad slot %u)",
                  (unsigned)claim);
        b[sizeof(b) - 1] = '\0'; coopLog(b);
        ok = g_net.startClient(g_cfg.ip, g_cfg.port, &g_inbound);
    }
    if (ok && g_cfg.isHost && g_cfg.transport == "steam") coop::steaminvite::hostRoom();
    if (!ok) coopErr("KenshiCoop: networking failed to start");
}

// In-game panel handlers. coopUiConnect tears down any live session, re-arms the
// config from the panel's choices, and restarts via the shared startNetworking()
// path (NetLink cleanly supports stop() then start again; Steam is re-armed and
// the Replicator/Inbound session state is reset for a clean handshake).
void coopUiConnect(bool isHost, bool useSteam, const unsigned long long* peerIds,
                   unsigned int peerCount, unsigned int ownRank) {
    if (!isHost && useSteam && peerIds && peerCount && g_net.isRunning() &&
        !g_cfg.isHost && g_cfg.transport == "steam" && g_cfg.steamPeer == peerIds[0]) return;
    if (!isHost && useSteam) {
        const coop::room::Info& room = coop::steaminvite::watchedRoom();
        coop::u64 target = peerIds && peerCount ? peerIds[0] : g_cfg.steamPeer;
        if (room.host == target && room.state == coop::room::AVAILABLE) {
            if (room.protocol != coop::PROTOCOL_VERSION) {
                coop::setNetUiError("Version mismatch with host - update both to the same KenshiCoop build"); return;
            }
            if (!ownRank) ownRank = coop::room::firstFreeSlot(room.slots, room.limit);
        }
    }
    coop::disc::responderStop(); // startNetworking re-arms it for a UDP host
    g_net.stop();
    coop::steaminvite::reset();
    coop::steamp2p::shutdown();
    // World is live here (reconnect from within a running game): despawn minted
    // proxies before clearing maps so a re-connect leaves no orphaned duplicates.
    sessionResetForUi();

    // 角色变化时同步日志标签；使用默认日志名时一并改名（KENSHICOOP_LOG 自定义路径不动）
    if (isHost != g_cfg.isHost) {
        if (g_cfg.logPath == (g_cfg.isHost ? "KenshiCoop_host.log" : "KenshiCoop_join.log"))
            g_cfg.logPath = isHost ? "KenshiCoop_host.log" : "KenshiCoop_join.log";
        coop::logInit(g_cfg.logPath.c_str(), isHost ? "HOST" : "JOIN");
    }
    g_cfg.isHost    = isHost;
    g_cfg.transport = useSteam ? "steam" : "udp";
    // Re-read the UDP endpoint (ip/port) from coop_config.json so editing it then
    // hitting Connect works without restarting the game. (It also picks up
    // steamPeer(s)/ownRank if set for advanced/back-compat use.)
    coop::reloadPeerFromFile(g_cfg);
    // A host picked from the discovery Scan this session WINS over the config
    // file (same precedent as panel-pasted Steam IDs): re-apply it after the
    // reload so hitting ONLINE connects to the scanned host, not the stale
    // coop_config.json endpoint. Session-scoped; never written to disk.
    if (!isHost && !useSteam && g_discSel.valid) {
        g_cfg.ip   = g_discSel.ip;
        g_cfg.port = g_discSel.port;
        char db[128];
        _snprintf(db, sizeof(db) - 1, "[disc] connecting to scanned host %s:%u",
                  g_discSel.ip, (unsigned)g_discSel.port);
        db[sizeof(db) - 1] = '\0';
        coopLog(db);
    }
    // Steam IDs pasted in the F2 panel this session win over the config: the
    // normal flow is Copy my Steam ID -> friend Pastes it -> Connect, with no
    // file editing. An empty paste list leaves the config values standing.
    // A HOST's pasted list becomes the friend list (protocol 49); a JOIN's
    // first pasted id is the host.
    if (peerIds && peerCount > 0) {
        if (isHost) {
            g_cfg.steamPeers.clear();
            for (unsigned int i = 0; i < peerCount; ++i)
                if (peerIds[i] != 0) g_cfg.steamPeers.push_back(peerIds[i]);
            if (!g_cfg.steamPeers.empty()) g_cfg.steamPeer = g_cfg.steamPeers[0];
        } else {
            g_cfg.steamPeers.clear();
            g_cfg.steamPeer = peerIds[0];
        }
    }
    // The JOIN's squad-slot choice from the panel (protocol 49): 0 = keep the
    // config/default; the resolved ownRanks below pick it up.
    if (!isHost && ownRank != 0) g_cfg.ownRank = ownRank;
    // Host streams world NPCs; under presence authority both do, each for the cells it claims.
    g_repl.setStreamNpcs(isHost || g_cfg.cellAuth);
    // Ownership ranks must follow the role chosen in the panel. Only an explicit
    // KENSHICOOP_OWN_SQUAD override is preserved; otherwise recompute the default
    // (host owns {0}, join owns {1}). Without this, a session launched as HOST
    // that switches to JOIN keeps rank {0} and wrongly claims the host's player
    // squad, so that unit never moves on the client (unowned NPCs still sync).
    // A join's protocol-49 single-slot claim (config "ownRank", re-read above)
    // overrides the role default the same way loadConfig applies it.
    coop::resolveOwnRanks(g_cfg.ownRanks, isHost, g_cfg.ownRanksFromEnv);
    if (!g_cfg.ownRanksFromEnv && !isHost && g_cfg.ownRank != 0) {
        g_cfg.ownRanks.clear();
        g_cfg.ownRanks.insert(g_cfg.ownRank);
    }
    g_repl.setOwnRanks(g_cfg.ownRanks);

    std::string ranks;
    for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
         it != g_cfg.ownRanks.end(); ++it) {
        char n[16]; _snprintf(n, sizeof(n) - 1, "%u", *it); n[sizeof(n) - 1] = '\0';
        if (!ranks.empty()) ranks += ",";
        ranks += n;
    }
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[coop-ui] connect: role=%s transport=%s peer=%llu ownRanks={%s} src=%s",
              isHost ? "HOST" : "JOIN", g_cfg.transport.c_str(),
              (unsigned long long)g_cfg.steamPeer, ranks.c_str(),
              g_cfg.ownRanksFromEnv ? "env" : "role");
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
    startNetworking();
}

void coopUiDisconnect() {
    coopLog("[coop-ui] disconnect");
    coop::disc::responderStop();
    g_net.stop();
    coop::steaminvite::reset(); // leave any Steam lobby
    coop::steamp2p::shutdown();
    // World stays live on a manual disconnect: despawn minted proxies before
    // clearing maps so nothing lingers as a duplicate or gets baked into a save.
    sessionResetForUi();
}

// 退出游戏时游戏会调用 SteamAPI_Shutdown，随后 steamclient 被卸载；网络线程若还在经 Steam P2P
// 收包（enet_host_service → hookReceive），就会读到已卸载的地址而崩溃（双方退出时都出现过）。
// 先停网络线程、离开大厅、关闭 P2P 会话，再放行原函数。
void (*g_steamShutdown_orig)() = 0;
void steamShutdown_hook() {
    coopLog("[steam] SteamAPI_Shutdown: stopping net thread before Steam unloads");
    g_net.stop();
    coop::steaminvite::reset();
    coop::steamp2p::shutdown();
    g_steamShutdown_orig();
}

// 小队控制授权（第 3 批）：面板按钮的动作。归属切换需要 GameWorld 做 pin 清扫，所以传
// g_lastGw（面板在主菜单按下的那一刻没有世界，此时清扫是 no-op，本来就没人发布）。
void coopUiAuth(int action) {
    if (g_peersPresent.empty()) return; // 好友不在线：没有可交接的对象
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] squad auth action=%d", action);
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
    if (action == 0)      g_repl.requestSquadAuth(g_net, g_net.localId());
    else if (action == 1) g_repl.grantSquadAuth(g_lastGw, g_net, g_net.localId());
    else if (action == 2) g_repl.denySquadAuth(g_net, g_net.localId());
    else if (action == 3) g_repl.revokeSquadAuth(g_lastGw, g_net, g_net.localId());
}

} // namespace

// RE_Kenshi resolves the entry by its C++-mangled name (?startPlugin@@YAXXZ), so
// this must NOT be extern "C".
// ---------------------------------------------------------------------------
// PluginHooks (Phase 3): startPlugin's install wall, split into named phases.
// Each was lifted VERBATIM from startPlugin, so load-time behavior + ordering
// are unchanged; startPlugin now reads as loadConfig -> banner -> hook -> wire.
// ---------------------------------------------------------------------------

// Startup log roster: load banner, fake-skew notice, build stamp, role line,
// and the effective (resolved) config summary. Answerable from the log alone.
void logStartupBanner() {
    coopLog("KenshiCoop loaded! (clean rebuild)");
    if (g_cfg.fakeClockSkewMs != 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: FAKE clock skew injected: %+ld ms",
                  g_cfg.fakeClockSkewMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    // Build stamp: changes every compile, so the test runner can confirm a fresh
    // DLL is actually deployed (anti-stale guard) rather than an old cached copy.
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: build %s %s", __DATE__, __TIME__);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    {
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: role=%s proto=v%u port=%d save='%s'",
                  g_cfg.isHost ? "HOST" : "JOIN", (unsigned)coop::PROTOCOL_VERSION,
                  g_cfg.port, g_cfg.save.c_str());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    // Effective (resolved) config roster: which sync channels are actually on/off
    // for THIS run + the key tuning knobs. Answerable from the log alone.
    coopLog(coop::describeConfig(g_cfg).c_str());
}

// Main-thread tick hook. GameWorld exposes both the virtual and the non-virtual
// _NV_mainLoop_GPUSensitiveStuff (same RVA); GetRealAddress only works on the
// _NV_ variant. Returns false (caller aborts startup) if the hook can't install.
bool installMainLoopHook() {
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &g_mainLoop_orig)) {
        coopErr("KenshiCoop: could not install main-loop hook!");
        return false;
    }
    return true;
}

// Resolve engine symbols and apply every replicator configuration knob (stream
// scope, ownership ranks, interp/drive/combat tuning, audit rows, and the
// channel enables that are pure config - the detour-backed channels are wired in
// installEngineDetours).
void configureReplicator() {
    coop::engine::resolve();

    // Stage 4: the host streams nearby world NPCs (host-authoritative) in addition
    // to its squad; the join resolves each by hand and drives it like a squad body.
    if (g_cfg.isHost || g_cfg.cellAuth) g_repl.setStreamNpcs(true);

    // Bidirectional presence: this client owns (controls + streams) a disjoint set of
    // the shared squad chosen by save-stable hand-rank; it drives the peer's owned
    // members from their stream. Default leader-first: host {0}, join {1}.
    g_repl.setOwnRanks(g_cfg.ownRanks);
    {
        std::string ranks;
        for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
             it != g_cfg.ownRanks.end(); ++it) {
            char num[16]; _snprintf(num, sizeof(num) - 1, "%u", *it); num[sizeof(num)-1] = '\0';
            if (!ranks.empty()) ranks += ",";
            ranks += num;
        }
        std::string m = "KenshiCoop: ownership ranks = {" + ranks + "} (bidirectional presence)";
        coopLog(m.c_str());
    }

    // Protocol 36 movement-smoothness knobs (KENSHICOOP_INTERP_* / _CATCHUP_K /
    // _SNAP_DIST): live-tune the interp window and the walk-drive gains for WAN
    // A/B runs. Defaults reproduce the historical constants exactly.
    {
        coop::InterpConfig ic;
        ic.minDelayMs  = g_cfg.interpMinDelayMs;
        ic.maxDelayMs  = g_cfg.interpMaxDelayMs;
        ic.maxExtrapMs = g_cfg.interpMaxExtrapMs;
        ic.staleMs     = g_cfg.interpStaleMs;
        ic.snapDistSq  = g_cfg.interpSnapDist * g_cfg.interpSnapDist;
        ic.cadenceDelayK     = g_cfg.interpCadenceK;
        ic.maxCadenceDelayMs = g_cfg.interpMaxCadenceDelayMs;
        g_repl.setInterpConfig(ic);
        g_repl.setDriveTuning(g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds);
        g_repl.setCombatTuning(g_cfg.combatSoftDist, g_cfg.combatSnapDist,
                               g_cfg.combatBigSnapDist, g_cfg.combatSlideMax,
                               g_cfg.combatConvergeMs);
        g_repl.setSendStamp(g_cfg.sendStamp);
        g_repl.setCensusRadius(g_cfg.censusRadius);
        g_repl.setSpawnMintRadius(g_cfg.spawnMintRadius);
        g_repl.setAdoptRadius(g_cfg.adoptRadius);
        g_repl.setCensusParkDist(g_cfg.censusParkDist);
        g_repl.setCensusWalkDist(g_cfg.censusWalkDist);
        g_repl.setCensusFreezeAi(g_cfg.censusFreezeAi);
        g_repl.setAttentionRadius(g_cfg.attentionRadius);
        g_repl.setCellAuth(g_cfg.cellAuth);
        g_repl.setCellCollapse(g_cfg.cellCollapse);
        g_repl.setSpeedCombatCap(g_cfg.speedCombatCap);
        g_repl.setStarveHold(g_cfg.starveHoldMs);
        // Camera-anchored interest (protocol 43): engine-side master enable
        // for the camera anchors in interestCenters.
        coop::engine::setCamInterest(g_cfg.camInterest);
        // travel_parity needs the 5 s SCENARIO WORLD/WNPC worldstate rows on
        // both sides; npc_sync feeds the same rows to the anti-zombie oracle
        // (Phase 2 mid-band tier: a populated town run has moving census-band
        // NPCs, which the hop corridor mostly lacks). Every other scenario
        // stays quiet (log volume).
        // run_apart joins them because the population question it exists to ask
        // - who holds a body while the two squads are 146,000 u apart and one
        // camera is on the other's town - can only be answered by cross-matching
        // the two sides' enumerated sets, hand by hand, with the authority class
        // each side assigned. The aggregate counters cannot: a creature visible
        // on one client and absent on the other leaves every bucket balanced.
        g_repl.setAuditRows(g_cfg.scenario == "travel_parity" ||
                            g_cfg.scenario == "split_far" ||
                            g_cfg.scenario == "run_apart" ||
                            g_cfg.scenario == "npc_sync" ||
                            g_cfg.scenario == "world_parity" ||
                            g_cfg.scenario == "escape_cohesion" ||
                            g_cfg.scenario == "jail_probe" ||
                            g_cfg.scenario == "jail_soak" ||
                            g_cfg.jailProbe);  // manual -JailProbe: no scenario name
        char b[260];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: interp delay=%u-%ums extrap=%ums stale=%ums snap=%.0fu "
                  "drive catchupK=%.2f snapDist=%.1fu snapSec=%.2f sendStamp=%d "
                  "census=%.0fu mint=%.0fu park=%.0fu walk=%.0fu starveHold=%ums",
                  g_cfg.interpMinDelayMs, g_cfg.interpMaxDelayMs,
                  g_cfg.interpMaxExtrapMs, g_cfg.interpStaleMs, g_cfg.interpSnapDist,
                  g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds,
                  g_cfg.sendStamp ? 1 : 0,
                  g_cfg.censusRadius, g_cfg.spawnMintRadius, g_cfg.censusParkDist,
                  g_cfg.censusWalkDist, g_cfg.starveHoldMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state. KENSHICOOP_CARRY_SYNC=0 is the A/B escape hatch.
    g_repl.setCarrySync(g_cfg.carrySync);

    // Furniture occupancy sync (protocol 19, default ON): reliable bed/cage
    // enter/exit edges + self-healing occupancy state. KENSHICOOP_FURN_SYNC=0
    // is the A/B escape hatch.
    g_repl.setFurnSync(g_cfg.furnSync);
    g_repl.setChainSync(g_cfg.chainSync);
    g_repl.setStealthSync(g_cfg.stealthSync);

    // Prone posture sync (protocol 53, default ON): driven copies are posed with
    // the owner's exact ProneState so an injured crawler is not walk-driven
    // upright. KENSHICOOP_PRONE_SYNC=0 is the A/B escape hatch.
    g_repl.setProneSync(g_cfg.proneSync);

    // Shared money pool (protocol 52, default ON): one host-authoritative purse
    // both players spend from. KENSHICOOP_MONEY_SYNC=0 is the A/B escape hatch
    // (and the shop_probe baseline).
    g_repl.setMoneySync(g_cfg.moneySync);

    // Runtime-spawn proxy replication (protocol 21, default ON): the join mints
    // local proxy bodies for host runtime spawns it cannot resolve by hand.
    // KENSHICOOP_SPAWN_SYNC=0 is the A/B escape hatch (spawn_probe forces off).
    g_repl.setSpawnSync(g_cfg.spawnSync);

    // AI-gating probe (join side): recruit diverged NPCs to test the inhabit lever.
    if (!g_cfg.isHost && g_cfg.probeRecruit) g_repl.setProbeRecruit(true);
}

// Install every engine detour + set the detour-backed channel enables (AI
// suspend, task-select spike, jail probes, damage guard, shop, trade veto, item
// drop, recruit/squad/faction/build/dismantle, coordinated save/load).
void installEngineDetours() {
    // AI-suspend (BOTH roles, DEFAULT ON): detour Character::periodicUpdate so
    // any body a client DRIVES from the peer's stream stops self-tasking (decision
    // layer off) while still animating. Faction is untouched - we hold the body's
    // current action instead of letting the AI re-decide and wander/thrash. This
    // is the universal QUIETING layer (doctrine 15 amendment); per-class APPLY
    // levers sit on top. KENSHICOOP_AI_SUSPEND=0 disables (A/B escape hatch).
    //
    // Phase 1b: enabled on the HOST too (was join-only). The host drives nothing
    // in the classic host-authoritative single-direction case (it owns every
    // NPC), so this is a no-op there. But once a recruit is TRANSFERRED into the
    // join's tab, control flips to the join and the HOST must now drive that
    // peer-owned squad member - without suspend it self-follows the host's leader
    // and shows the same slow-walk/snap artifact the join had. applyTargets only
    // ever suspends bodies we drive (peer-owned), so a host-owned NPC/leader is
    // never quieted by this.
    if (g_cfg.aiSuspend) {
        if (coop::engine::installAiSuspendHook()) {
            g_repl.setAiSuspend(true);
            coopLog("[ai] periodicUpdate detour installed; AI-suspend ON (default)");
        } else {
            coopLog("[ai] FAILED to install periodicUpdate detour; AI-suspend disabled");
        }
    }
    // Task-selection observation spike (KENSHICOOP_TASK_SPIKE, off by default):
    // passive detour on CharBody::setCurrentAction to prove the selection seam is
    // hookable in isolation and log the chosen task tuple per body. No behavior
    // change - a diagnostic for the "stream selection, not motion" direction.
    if (g_cfg.taskSelectSpike) {
        if (coop::engine::installTaskSelectSpikeHook()) {
            coop::engine::setTaskSelectSpike(true);
            coopLog("[spike] setCurrentAction detour installed; task-select observation ON (KENSHICOOP_TASK_SPIKE)");
        } else {
            coopLog("[spike] FAILED to install setCurrentAction detour (seam unresolved)");
        }
    }
    // Jail put-to-work desync spike: correlated [jail] STATE traces (read-only).
    g_repl.setJailProbe(g_cfg.jailProbe);
    if (g_cfg.jailProbe)
        coopLog("[jail] KENSHICOOP_JAIL_PROBE=1: captive-state [jail] STATE tracing ON");
    // Jail put-to-work observation spike (Phase A): host runs the captive
    // unopposed and logs its trajectory ([jail] OBSERVE) to classify intent.
    g_repl.setJailObserve(g_cfg.jailObserve);
    if (g_cfg.jailObserve)
        coopLog("[jail] KENSHICOOP_JAIL_OBSERVE=1: captive drive/suspend/self-heal OFF; [jail] OBSERVE tracing ON");
    // Step-2 experiment: sitter no-detach A/B (manual runs only; off by default).
    if (!g_cfg.isHost && g_cfg.noDetach) {
        g_repl.setNoDetach(true);
        coopLog("[quiet] KENSHICOOP_NO_DETACH=1: sitter detachFromTownAI SKIPPED (experiment)");
    }

    // Divergence-gated authority (join side, DEFAULT ON since the step-4 A/B):
    // trust world NPCs whose local AI sustainedly agrees with the host; drive
    // only divergence. KENSHICOOP_GATE_AUTHORITY=0 disables (A/B escape hatch).
    if (!g_cfg.isHost && g_cfg.gateAuthority) {
        g_repl.setGateAuthority(true);
        coopLog("[trust] divergence-gated authority ON (default; KENSHICOOP_GATE_AUTHORITY=0 disables)");
    }

    // Damage guard (BOTH sides, DEFAULT ON): locally-simulated melee hits on
    // driven bodies are suppressed (HIT_MISSED) so cosmetic fights cannot diverge
    // the local-only medical model. The guard set is "every body this client
    // drives": the peer's world-NPC copies on the join, the peer's SQUAD-member
    // copies on the host (host-side extension 2026-07-06 - phase-1 player_combat
    // measured the host's copy of a join victim bleeding 40+ blood while the
    // join's guarded copies stayed at 0). KENSHICOOP_DAMAGE_GUARD=0 disables.
    if (g_cfg.damageGuard) {
        if (coop::engine::installDamageGuardHook()) {
            g_repl.setDamageGuard(true);
            // Join-dealt authoritative damage report (protocol 45): only the JOIN
            // reports (the HOST owns + simulates world NPCs, so its own swings land
            // natively). Enabling report mode on the join makes the guard accumulate
            // the damage its player-squad melee WOULD have dealt to driven world-NPC
            // copies; publishCombatHits forwards it and the host wounds the real body.
            g_repl.setReportCombat(!g_cfg.isHost);
            coopLog(g_cfg.isHost
                ? "[dmg] hitByMeleeAttack detour installed; damage guard ON (host, driven peer-squad bodies)"
                : "[dmg] hitByMeleeAttack detour installed; damage guard ON + combat-hit report ON (join)");
        } else {
            coopLog("[dmg] FAILED to install hitByMeleeAttack detour; damage guard disabled");
        }
    }

    // Purchase observability (protocol 52 groundwork): log every real trade-UI
    // purchase ("[shop] BUY-LOCAL"), the field evidence for the vendor-stock
    // mirror design. Rides the money-sync gate.
    if (g_cfg.moneySync) {
        if (coop::engine::installShopHook())
            coopLog("[shop] buyItem detour installed; purchase logging ON");
        else
            coopLog("[shop] FAILED to install buyItem detour; purchase logging off");
    }

    // Cross-owner trade veto (KENSHICOOP_BLOCK_XFER, default ON in real sessions):
    // refuse a UI inventory drag whose source + destination squad characters are
    // owned by different clients (item stays in the source bag) so ground drops
    // are the only cross-client transfer path. Retires Protocol 37 (Config forces
    // xferSync off when this is on). The classifier is always registered (cheap);
    // the detours install only when the veto is on or the xfer_block test runs.
    coop::engine::setInvOwnerClassifier(&coopInvOwnerClass);
    coop::engine::setBlockXfer(g_cfg.blockXfer);
    if (g_cfg.blockXfer || g_cfg.scenario == "xfer_block") {
        if (coop::engine::installXferBlockHook())
            coopLog(g_cfg.blockXfer
                ? "[xfer] drag detours installed; cross-owner trade veto ON (drop to transfer)"
                : "[xfer] drag detours installed; drag logging ON");
        else
            coopLog("[xfer] FAILED to install drag detours; cross-owner trade veto degraded");
    }

    // 小队控制授权（第 1 批，默认 ON）：本地玩家对“对方控制的角色”下达的命令被拦下。
    // 分类器只认对方正在驱动的角色（Replicator::orderClassForHand）。
    coop::engine::setOrderGuard(&coopOrderOwnerClass, &coopOrderBlocked);
    if (coop::engine::installPlayerOrderGuardHook())
        coopLog("[order] player-command guard installed; commands on the peer's squad are blocked");
    else
        coopLog("[order] FAILED to install player-command guard; commands on the peer's squad are NOT blocked");

    // 选中限制（默认 ON）：未授权时对方角色只能单独选中查看——框选 / Ctrl+A / Shift 加选
    // 都会跳过它们，多选里也不会混进对方角色（命令拦截因此只剩兜底作用）。
    if (coop::engine::installSelectionGuardHook())
        coopLog("[select] selection guard installed; peer squad can only be selected alone");
    else
        coopLog("[select] FAILED to install selection guard; multi-select may include the peer's squad");

    // Phase W1b: query-free ground-drop capture (town reliability). Detour
    // Inventory::dropItem so every drop is discovered without the spatial sphere
    // query that fails in towns; publishWorldItems seeds its tracker from these
    // edges and culls by handle liveness. Rides the world-sync gate (the channel
    // it feeds), so real sessions and the world_item_* / drop scenarios get it.
    if (g_cfg.worldSync) {
        if (coop::engine::installItemDropHook())
            coopLog("[wi] dropItem detour installed; query-free drop capture ON");
        else
            coopLog("[wi] FAILED to install dropItem detour; drop capture falls back to the spatial scan");
    }

    // Recruitment sync (protocol 23, default ON): detour PlayerInterface::
    // recruit so every successful local recruit (dialog or programmatic)
    // authors a reliable EVT_RECRUIT; the peer re-keys its local copy of the
    // recruited body to the new stream key. KENSHICOOP_RECRUIT_SYNC=0 is the
    // A/B escape hatch (recruit_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setRecruitSync(g_cfg.recruitSync);
    // Squad management sync (protocol 35, default ON): pointer-diff move
    // edges -> EVT_SQUAD_MOVE re-keys + the container-rank latch (mid-session
    // tabs append instead of reshuffling ownership). KENSHICOOP_SQUAD_SYNC=0
    // is the A/B escape hatch (squad_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setSquadSync(g_cfg.squadSync);
    coop::engine::setSquadOwnershipCallbacks(&coopRecruitOwnSquad, &coopCreatedSquad);
    if (g_cfg.squadSync && !coop::engine::installSquadCreateHook())
        coopErr("[owners] squad creation hook installation failed");
    g_repl.setFactionSync(g_cfg.factionSync);
    g_repl.setTimeSync(g_cfg.timeSync);
    g_repl.setTimeBrake(g_cfg.timeBrake);
    g_repl.setDoorSync(g_cfg.doorSync);
    g_repl.setBuildSync(g_cfg.buildSync);
    g_repl.setBdoorSync(g_cfg.bdoorSync);
    g_repl.setHungerSync(g_cfg.hungerSync);
    g_repl.setProdSync(g_cfg.prodSync);
    g_repl.setResearchSync(g_cfg.researchSync);
    g_repl.setDeedSync(g_cfg.deedSync);
    g_repl.setFixtureSync(g_cfg.fixtureSync);
    // Protocol 34: the HOST authors every storage/machine container near the
    // interest centers (the ~1 Hz census inside publishInventories); the join
    // reconciles via the translated key. Host-only flag - the join must never
    // census-author (host-authoritative world containers). Layered on invSync
    // (publishInventories is the carrier and is gated on it below).
    g_repl.setStoreSync(g_cfg.storeSync && g_cfg.isHost);
    if (g_cfg.recruitSync) {
        if (coop::engine::installRecruitHook())
            coopLog("[recruit] recruit detour installed; recruitment sync ON");
        else
            coopLog("[recruit] FAILED to install recruit detour; recruitment sync degraded (no local-edge detection)");
    }

    // Build-placement observability (protocol 27): detour RootObjectFactory::
    // createBuilding so every REAL build-mode commit logs its template +
    // transform + runtime hand ("[build] LOCAL-PLACE") and queues an edge for
    // the sync layer. Installed for the sync AND for build_probe (which
    // measures the unsynced baseline but wants the placement evidence).
    if (g_cfg.buildSync || g_cfg.scenario == "build_probe") {
        if (coop::engine::installBuildHook())
            coopLog("[build] createBuilding capture installed; placement logging ON");
        else
            coopLog("[build] FAILED to install placement capture; placement sync OFF");
    }

    // Dismantle observability (protocol 28): detour Building::
    // notifyConstructionDismantling so every UI dismantle logs its hand
    // ("[build] LOCAL-DISMANTLE") and queues a removal edge for the sync
    // layer. Installed for the sync AND for bdoor_probe (unsynced baseline,
    // but the edge evidence is the finding).
    if (g_cfg.bdoorSync || g_cfg.scenario == "bdoor_probe") {
        if (coop::engine::installDismantleHook())
            coopLog("[build] dismantle detour installed; removal logging ON");
        else
            coopLog("[build] FAILED to install dismantle detour; removal logging degraded");
    }

    // Faction-relation observability (protocol 24): detour both affectRelations
    // overloads so every REAL relation mutation logs its cause ("[fac] AFFECT")
    // and records a delta for the sync layer. Installed for the sync AND for
    // faction_probe (which measures the unsynced baseline but needs the
    // cause-attribution evidence).
    if (g_cfg.factionSync || g_cfg.scenario == "faction_probe") {
        if (coop::engine::installFactionHooks())
            coopLog("[fac] affectRelations detours installed; relation logging ON");
        else
            coopLog("[fac] FAILED to install affectRelations detours; relation logging degraded");
    }

    // Coordinated save (protocol 31): detour SaveManager::save so every local
    // save (menu, quicksave, autosave timer, programmatic bake) logs its
    // "[save] LOCAL-SAVE" edge and queues a SaveEdge - the trigger for the
    // host-authoritative save transfer (and the join's REQ forwarding).
    // save_probe installs it too but measures the raw behaviour alone
    // (saveSync forced off - no coordination on top).
    if (g_cfg.saveSync || g_cfg.squadSync || g_cfg.scenario == "save_probe") {
        if (coop::engine::installSaveHook())
            coopLog(g_cfg.saveSync
                ? "[save] save detour installed; coordinated save ON"
                : "[save] save detour installed; save-edge logging ON");
        else
            coopLog("[save] FAILED to install save detour; coordinated save degraded");
    }

    // Coordinated load (protocol 32): detour SaveManager::load so every local
    // load (menu, title screen, programmatic) logs its "[load] LOCAL-LOAD"
    // edge and queues a LoadEdge - the trigger for the host-authoritative
    // load broadcast (and the join's REQ forwarding / suppression).
    // load_probe installs it too but measures the raw world swap alone
    // (loadSync forced off - no coordination on top).
    if (g_cfg.loadSync || g_cfg.scenario == "load_probe") {
        if (coop::engine::installLoadHook())
            coopLog(g_cfg.loadSync
                ? "[load] load detour installed; coordinated load ON"
                : "[load] load detour installed; load-edge logging ON");
        else
            coopLog("[load] FAILED to install load detour; coordinated load degraded");
    }
}

// Title-screen hook. It drives THREE things at the main menu: the config
// auto-load (only when a save is set), the F2 co-op panel (go ONLINE / paste
// a Steam ID before loading), and the push-save-on-connect bootstrap (a join
// with NO save receives + loads the host's world). So install it whenever a
// save is configured OR this is an interactive session - NOT only when a save
// is present, which was the old behavior that left a join-from-menu with no
// title hook (the panel and bootstrap never ran). The scenario/test harness
// always sets a save, so its behavior is unchanged. titleUpdate_hook itself
// self-gates each of the three concerns.
void installTitleHook() {
    bool interactive = g_cfg.scenario.empty() && g_cfg.testSeconds == 0;
    if (!g_cfg.save.empty() || interactive) {
        if (KenshiLib::SUCCESS !=
            KenshiLib::AddHook(
                KenshiLib::GetRealAddress(&TitleScreen::_NV_update),
                &titleUpdate_hook, &g_titleUpdate_orig)) {
            coopErr("KenshiCoop: could not install title-screen hook (F2 panel + bootstrap + auto-load disabled)");
        } else if (!g_cfg.save.empty()) {
            std::string m = "KenshiCoop: title hook armed (auto-load save '" + g_cfg.save + "')";
            coopLog(m.c_str());
        } else {
            coopLog("KenshiCoop: title hook armed (F2 panel + push-on-connect at menu; no auto-load save)");
        }
    }
}

__declspec(dllexport) void startPlugin() {
    coop::loadConfig(g_cfg);
    // The fake clock skew must be armed BEFORE the first log line so every
    // timestamp in this run (and every time-sync packet) shares the skewed clock.
    coop::logSetFakeSkewMs(g_cfg.fakeClockSkewMs);
    coop::logInit(g_cfg.logPath.c_str(), g_cfg.isHost ? "HOST" : "JOIN");

    // Fault-time crash dumps, installed before anything else can fault. RE_Kenshi's
    // own dump is written from a late filter and carries no usable frames, which is
    // where the 2026-08-03 crash investigation dead-ended; ours chains to RE_Kenshi's
    // filter afterwards, so its emergency save is unaffected. See core/CrashDump.h.
    {
        std::string dir = g_cfg.logPath;
        size_t cut = dir.find_last_of("\\/");
        dir = (cut == std::string::npos) ? std::string(".") : dir.substr(0, cut);
        coop::crashdump::install(dir.c_str(), g_cfg.isHost ? "host" : "join");
    }

    logStartupBanner();

    // Hook the main-thread tick FIRST (early-return on failure), then wire the
    // replicator, engine detours and UI hooks as named install phases.
    if (!installMainLoopHook())
        return;

    configureReplicator();

    installEngineDetours();

    installTitleHook();

    // 退出时先停网络线程，再让 Steam 卸载（见 steamShutdown_hook）
    {
        HMODULE steamApi = GetModuleHandleA("steam_api64.dll");
        FARPROC shutdownFn = steamApi ? GetProcAddress(steamApi, "SteamAPI_Shutdown") : 0;
        if (shutdownFn && KenshiLib::SUCCESS ==
                KenshiLib::AddHook((intptr_t)shutdownFn, &steamShutdown_hook, &g_steamShutdown_orig))
            coopLog("[steam] SteamAPI_Shutdown hook installed (net thread stops before Steam unloads)");
        else
            coopLog("[steam] FAILED to hook SteamAPI_Shutdown; quitting may crash in the net thread");
    }
    // 帧数上限（F2 面板切换），未设置时不限
    coop::fpslimit::install();

    if (g_cfg.testSeconds > 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: test mode - self-exit %d s after gameplay starts",
                  g_cfg.testSeconds);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Scenario harness: build the selected scenario (both host & join run it; it
    // branches on host/join internally). Unknown names are logged and ignored.
    // Harness/Debug only - the shipped Release DLL has no scenario runner.
#ifdef KENSHICOOP_HARNESS
    if (!g_cfg.scenario.empty()) {
        g_scenario = coop::makeScenario(g_cfg.scenario);
        if (g_scenario) {
            std::string m = "KenshiCoop: scenario armed '" + g_cfg.scenario + "'";
            coopLog(m.c_str());
        } else {
            std::string m = "KenshiCoop: unknown scenario '" + g_cfg.scenario + "'";
            coopErr(m.c_str());
        }
    }
#endif

    // Session start policy (in-game panel, 2026-07-14): the unattended harness
    // (a scenario or a self-exit timer) ALWAYS auto-starts - it never touches the
    // F2 panel. An interactive install DEFERS the session to the panel's Connect
    // unless KENSHICOOP_AUTOCONNECT=1 restores load-time auto-start from the
    // env/config role+transport+peer (the legacy launcher behaviour).
    bool autoStart = g_cfg.autoConnect || !g_cfg.scenario.empty() || g_cfg.testSeconds > 0;

    // Bring Steam up now (when the panel may be used, or Steam is configured) so
    // the panel can show THIS player's SteamID for the two-code exchange before
    // any connection is made. init() is idempotent; startNetworking re-uses it.
    // When Steam is up, also arm the invite layer so this player can RECEIVE a
    // Steam "Join Game"/invite (GameLobbyJoinRequested) even before touching the
    // panel - the joiner never has to open it or type an ID.
    if (!autoStart || g_cfg.transport == "steam" || g_cfg.steamPing != 0) {
        if (coop::steamp2p::init())
            coop::steaminvite::init(&coopUiConnect);
    }

    if (autoStart) {
        startNetworking();
    } else {
        coopLog("KenshiCoop: session DEFERRED - press F2 in-game to pick role/transport, "
                "enter the friend's Steam ID, and check CONNECTED");
    }
}


namespace {
void coopRecruitOwnSquad(Character* c) {
    g_repl.placeRecruit(g_lastGw, c);
}

void coopCreatedSquad(unsigned int container, unsigned int serial) {
    g_repl.claimLocalTab(std::make_pair((coop::u32)container, (coop::u32)serial));
}

void coopUiCharacter(int action) { g_characterAction = action; }

void replyCharacterCreation(const coop::CharacterReplyPacket& reply) {
    if (reply.targetOwnerId == g_net.localId()) g_inbound.pushCharacterReply(reply);
    else {
        std::vector<coop::u8> bytes;
        coop::draftAppend(bytes, &reply, sizeof(reply));
        g_net.queueCharacterPacket(reply.targetOwnerId, bytes);
    }
}

void completeCharacterSave(bool saved) {
    if (!g_characterSaveInFlight) return;
    g_characterSaveInFlight = false;
    if (!saved) g_characterPush.result = 0;
    g_characterSaveReply.result = saved ? coop::CHARACTER_SAVED : coop::CHARACTER_SAVE_FAILED;
    g_characterResults[g_characterSaveReply.playerId] = g_characterSaveReply;
    replyCharacterCreation(g_characterSaveReply);
    coopLog(saved ? "[chargen] character save settled with ownership sidecar" : "[chargen] character remains in world; coordinated save failed");
}


void publishCharacterAppearance(const coop::InboundCharacterAppearance& appearance) {
    std::vector<coop::u8> bytes;
    coop::draftAppend(bytes, &appearance.header, sizeof(appearance.header));
    coop::draftAppend(bytes, &appearance.data[0], (unsigned int)appearance.data.size());
    g_net.queueCharacterPacket(coop::OWNER_ID_ALL, bytes);
}

void driveCharacterCreation(GameWorld* gw, bool worldLive) {
    bool online = g_net.isRunning() && (g_cfg.isHost || !g_peersPresent.empty());
    if (!worldLive || !online) {
        if (coop::engine::characterPreview()) {
            coop::engine::cancelCharacterCreation(worldLive);
            g_characterStatus = coop::tr("Character creation cancelled");
        }
        g_characterAvailable = false; g_characterChoiceWorld = 0; g_characterChoices.clear();
        g_characterPending = false; g_characterAction = -1; g_characterRequests.clear();
        return;
    }
    coop::u64 world = g_repl.ownershipWorld();
    if (world && world != g_characterWorld) {
        g_characterWorld = world; g_characterResults.clear(); g_characterAppearances.clear();
        g_characterPending = false; g_characterRequests.clear();
    }
    if (g_characterChoiceWorld != gw) {
        coop::engine::characterChoices(gw, g_characterChoices); g_characterChoice = 0; g_characterChoiceWorld = gw;
    }
    g_characterAvailable = g_cfg.saveSync && g_cfg.loadSync && g_cfg.squadSync && !g_characterPending && !coop::engine::characterPreview() &&
        !g_bootstrapArmed && !coop::savexfer::watching() && !coop::savexfer::sending() && coop::engine::saveMgrSignal(0) == 0 &&
        g_repl.characterConnection(g_net.localId()) && g_repl.canCreateCharacter(gw, g_net.localId());
    if (coop::engine::characterPreview() && (world != g_characterEditWorld ||
        g_repl.characterConnection(g_net.localId()) != g_characterEditConnection)) {
        coop::engine::cancelCharacterCreation(true);
        g_characterStatus = coop::tr("Session changed; character creation cancelled");
    }
    int action = g_characterAction; g_characterAction = -1;
    if (coop::engine::characterPreview() && action == 1) {
        g_characterStatus = coop::engine::cancelCharacterCreation(true) ? coop::tr("Character creation cancelled") :
            coop::tr("Preview cleanup failed; saving is deferred until cleanup succeeds");
        action = -1;
    } else if (g_characterAvailable && !g_characterChoices.empty()) {
        if (action == 0) g_characterChoice = (g_characterChoice + 1) % (unsigned int)g_characterChoices.size();
        else if (action == 1) {
            g_characterEditWorld = world; g_characterEditConnection = g_repl.characterConnection(g_net.localId());
            coop::engine::cameraCenter(gw, g_characterPosition);
            if (coop::engine::beginCharacterCreation(gw, g_characterChoices[g_characterChoice]))
                g_characterStatus = coop::tr("Edit your character, confirm in the editor; F2 to cancel");
            else g_characterStatus = coop::tr("Cannot open character editor");
        }
    }
    if (action == 1 && !g_characterAvailable && !coop::engine::characterPreview()) {
        if (g_characterPending) g_characterStatus = coop::tr("A character creation request is already pending");
        else if (!g_cfg.saveSync || !g_cfg.loadSync || !g_cfg.squadSync) g_characterStatus = coop::tr("Enable save, load and squad sync before creating a character");
        else if (g_bootstrapArmed || coop::savexfer::watching() || coop::savexfer::sending() || coop::engine::saveMgrSignal(0) != 0)
            g_characterStatus = coop::tr("Wait for the current save or transfer to finish");
        else if (!world) g_characterStatus = coop::tr("Load the host save and wait for ownership synchronization");
        else g_characterStatus = coop::tr("Creating a character requires that you own no characters");
    }
    if (action == 1 && g_characterAvailable && g_characterChoices.empty()) g_characterStatus = coop::tr("No character templates found in loaded game starts");
    coop::CharacterDraft draft;
    int result = coop::engine::pollCharacterCreation(gw, draft);
    if (result < 0) g_characterStatus = result == -3 ? coop::tr("Preview cleanup failed; saving is deferred until cleanup succeeds") :
        result == -1 ? coop::tr("Character creation cancelled") : coop::tr("Invalid character appearance");
    if (result == 1) {
        coop::InboundCharacterRequest request; memset(&request.header, 0, sizeof(request.header));
        coop::CharacterRequestHeader& h = request.header;
        h.type = (coop::u8)coop::PKT_CHARACTER_REQUEST; h.ownerId = g_net.localId();
        h.playerId = g_repl.playerIdentity(); h.worldId = world; h.authEpoch = g_repl.ownershipEpoch();
        h.connectionId = g_repl.characterConnection(h.ownerId); h.requestId = ++g_characterRequestId;
        memcpy(h.position, g_characterPosition, sizeof(h.position));
        if (coop::encodeCharacterDraft(draft, request.data) && g_repl.characterRequestCurrent(h)) {
            h.bytes = (coop::u32)request.data.size();
            g_characterWait = h; g_characterPending = true; g_characterWaitTick = GetTickCount();
            g_characterStatus = coop::tr("Waiting for host to create and save character");
            if (g_cfg.isHost) g_characterRequests.push_back(request);
            else {
                std::vector<coop::u8> bytes; coop::draftAppend(bytes, &h, sizeof(h));
                coop::draftAppend(bytes, &request.data[0], h.bytes);
                g_net.queueCharacterPacket(0, bytes);
            }
            coopLog("[chargen] confirmed draft submitted to authoritative creation queue");
        } else g_characterStatus = coop::tr("Invalid character appearance");
    }
    std::deque<coop::CharacterReplyPacket> replies; g_inbound.drainCharacterReplies(replies);
    for (size_t i = 0; i < replies.size(); ++i) {
        const coop::CharacterReplyPacket& p = replies[i];
        if (p.requestId != g_characterWait.requestId || p.playerId != g_repl.playerIdentity() ||
            p.connectionId != g_repl.characterConnection(g_net.localId()) || p.worldId != g_characterWait.worldId) continue;
        g_characterPending = false;
        if (p.result == coop::CHARACTER_SAVED) g_characterStatus = coop::tr("Character saved with permanent ownership");
        else if (p.result == coop::CHARACTER_CREATED) g_characterStatus = coop::tr("Character created and save requested");
        else if (p.result == coop::CHARACTER_SAVE_FAILED) g_characterStatus = coop::tr("Character exists, but saving failed; save on host, then reconnect");
        else if (p.result == coop::CHARACTER_DENIED) g_characterStatus = coop::tr("Host refused: character already owned or session changed");
        else if (p.result == coop::CHARACTER_INVALID) g_characterStatus = coop::tr("Invalid character appearance");
        else g_characterStatus = coop::tr("Character creation failed");
        coopLog("[chargen] authoritative creation reply received");
    }
    if (g_characterPending && GetTickCount() - g_characterWaitTick > 120000) {
        g_characterStatus = coop::tr("Host response timed out; wait or reconnect to check character ownership");
    }
    std::deque<coop::InboundCharacterRequest> requests; g_inbound.drainCharacterRequests(requests);
    if (g_cfg.isHost) for (size_t i = 0; i < requests.size(); ++i) {
        const coop::CharacterRequestHeader& h = requests[i].header;
        std::map<coop::u64, coop::CharacterReplyPacket>::const_iterator done = g_characterResults.find(h.playerId);
        if (done != g_characterResults.end() && done->second.connectionId == h.connectionId && done->second.requestId >= h.requestId) {
            if (done->second.requestId == h.requestId) replyCharacterCreation(done->second);
            continue;
        }
        bool pending = false;
        for (size_t j = 0; j < g_characterRequests.size(); ++j)
            if (g_characterRequests[j].header.playerId == h.playerId) pending = true;
        if (!pending && g_characterRequests.size() < coop::MAX_PLAYERS) g_characterRequests.push_back(requests[i]);
    }
    if (g_characterPush.result && (g_repl.characterConnection(g_characterPush.targetOwnerId) != g_characterPush.connectionId ||
        g_repl.createdCharacterArrived(g_characterPush.targetOwnerId, g_characterPush.hand))) {
        g_characterPush.result = 0;
        coopLog("[chargen] targeted load completed or requester disconnected; creation queue resumed");
    }
    if (g_cfg.isHost && !g_characterPush.result && !g_characterRequests.empty() && !g_bootstrapArmed && !coop::savexfer::watching() &&
        !coop::savexfer::sending() && g_loadXferPending.empty() && g_loadXferSent.empty() &&
        coop::engine::saveMgrSignal(0) == 0 && !coop::engine::characterPreview()) {
        coop::InboundCharacterRequest request = g_characterRequests.front(); g_characterRequests.pop_front();
        const coop::CharacterRequestHeader& h = request.header;
        coop::CharacterReplyPacket reply; memset(&reply, 0, sizeof(reply));
        reply.type = (coop::u8)coop::PKT_CHARACTER_REPLY; reply.targetOwnerId = h.ownerId;
        reply.playerId = h.playerId; reply.worldId = h.worldId; reply.connectionId = h.connectionId; reply.requestId = h.requestId;
        reply.result = coop::CHARACTER_DENIED;
        if (g_cfg.saveSync && g_cfg.loadSync && g_cfg.squadSync && g_characterAppearances.size() < 32 && g_repl.characterRequestCurrent(h) && g_repl.canCreateCharacter(gw, h.ownerId)) {
            coop::CharacterDraft decoded;
            reply.result = coop::CHARACTER_INVALID;
            if (!request.data.empty() && coop::decodeCharacterDraft(&request.data[0], (unsigned int)request.data.size(), decoded)) {
                reply.result = coop::CHARACTER_FAILED;
                if (coop::engine::createDraftCharacter(gw, decoded, h.position, reply.hand)) {
                    if (!g_repl.registerCreatedCharacter(h.ownerId, reply.hand)) {
                        coop::engine::despawnProxyNpc(gw, coop::engine::resolveCharByHand(reply.hand[3], reply.hand[4], reply.hand[0], reply.hand[1], reply.hand[2]));
                    } else {
                        coop::InboundCharacterAppearance appearance; memset(&appearance.header, 0, sizeof(appearance.header));
                        appearance.header.type = (coop::u8)coop::PKT_CHARACTER_APPEARANCE; appearance.header.worldId = world;
                        memcpy(appearance.header.hand, reply.hand, sizeof(reply.hand));
                        appearance.draft = decoded;
                        appearance.data = request.data; appearance.header.bytes = (coop::u32)appearance.data.size();
                        if (g_characterAppearances.size() < 32) g_characterAppearances.push_back(appearance);
                        publishCharacterAppearance(appearance);
                        bool saved;
                        if (h.ownerId != g_net.localId()) saved = armConnectPush(h.ownerId);
                        else {
                            char cur[64] = { 0 }; coop::engine::saveInfo(cur, sizeof(cur), 0, 0);
                            saved = coop::engine::saveGameAs(cur[0] ? cur : "coopresume");
                        }
                        reply.result = saved ? coop::CHARACTER_CREATED : coop::CHARACTER_SAVE_FAILED;
                        if (saved) {
                            g_characterSaveInFlight = true; g_characterSaveReply = reply;
                            if (h.ownerId != g_net.localId()) g_characterPush = reply;
                        }
                        coopLog("[chargen] character registered to permanent owner; fresh coordinated save requested");
                    }
                }
            }
        }
        g_characterResults[h.playerId] = reply;
        replyCharacterCreation(reply);
    }
    std::deque<coop::InboundCharacterAppearance> appearances; g_inbound.drainCharacterAppearances(appearances);
    for (size_t i = 0; !g_cfg.isHost && i < appearances.size(); ++i) {
        const coop::InboundCharacterAppearance& a = appearances[i];
        if (a.header.worldId != world) continue;
        bool known = false;
        for (size_t j = 0; j < g_characterAppearances.size(); ++j)
            if (memcmp(g_characterAppearances[j].header.hand, a.header.hand, sizeof(a.header.hand)) == 0) known = true;
        coop::CharacterDraft decoded;
        if (!known && g_characterAppearances.size() < 32 && !a.data.empty() &&
            coop::decodeCharacterDraft(&a.data[0], (unsigned int)a.data.size(), decoded)) {
            coop::InboundCharacterAppearance cached = a; cached.draft = decoded; g_characterAppearances.push_back(cached);
        }
    }
    for (size_t i = 0; i < g_characterAppearances.size(); ++i) {
        const coop::InboundCharacterAppearance& a = g_characterAppearances[i];
        if (a.header.worldId != world) continue;
        if (g_cfg.isHost) {
            if (GetTickCount() - g_characterAppearanceTick >= 5000) publishCharacterAppearance(a);
        } else {
            g_repl.applyCreatedAppearance(gw, a.header.hand, a.draft);
        }
    }
    if (g_cfg.isHost && GetTickCount() - g_characterAppearanceTick >= 5000) g_characterAppearanceTick = GetTickCount();
}

} // namespace


namespace {
void coopPlayerBoxesDrive(GameWorld* gw) {
    static std::vector<coop::engine::CoopPlayerBox> boxes;
    static unsigned long sampled = 0;
    static coop::u64 lastWorld = 0;
    coop::u64 world = g_repl.ownershipWorld();
    bool active = g_net.isRunning() && g_gameStarted && gw && world && coop::engine::gameplayLive(gw);
    unsigned long now = GetTickCount();
    coop::engine::CoopPlayerBoxAction clicked;
    if (coop::engine::takePlayerBoxAction(clicked)) {
        std::map<coop::u64, coop::PeerStatusPacket>::const_iterator self = g_repl.players().find(g_repl.playerIdentity());
        std::map<coop::u64, coop::PeerStatusPacket>::const_iterator peer = g_repl.players().find(clicked.playerId);
        bool current = active && clicked.world == world && clicked.epoch == g_repl.ownershipEpoch() &&
            clicked.actorPlayerId == g_repl.playerIdentity() && self != g_repl.players().end() && self->second.present &&
            clicked.selfConnection && clicked.selfConnection == self->second.connectionId && self->second.ownerId == g_net.localId() &&
            peer != g_repl.players().end() && peer->second.present && clicked.targetOwnerId == peer->second.ownerId &&
            clicked.targetConnection && clicked.targetConnection == peer->second.connectionId;
        unsigned int matched = 0;
        if (current) for (std::map<std::pair<coop::u32, coop::u32>, coop::OwnerRow>::const_iterator o = g_repl.owners().begin(); o != g_repl.owners().end(); ++o) {
            const std::pair<coop::u32, coop::u32>& tab = o->first;
            if (clicked.single && tab != std::make_pair(clicked.container, clicked.containerSerial)) continue;
            coop::Replicator::SquadAuthView view;
            g_repl.squadAuthView(tab, view);
            if (clicked.kind == 0 && o->second.playerId == clicked.playerId && view.requestable)
                g_repl.requestSquadAuth(g_net, g_net.localId(), clicked.playerId, &tab);
            else if (clicked.kind == 1 && o->second.playerId == g_repl.playerIdentity() && view.requestMode == 2 && view.requestPlayerId == clicked.playerId)
                g_repl.grantSquadAuth(gw, g_net, g_net.localId(), clicked.playerId, &tab);
            else if (clicked.kind == 2 && o->second.playerId == g_repl.playerIdentity() && view.requestMode == 2 && view.requestPlayerId == clicked.playerId)
                g_repl.denySquadAuth(g_net, g_net.localId(), clicked.playerId, &tab);
            else if (clicked.kind == 3 && o->second.playerId == g_repl.playerIdentity() && view.authMode == 1 && view.authPlayerId == clicked.playerId && view.requestMode != 3)
                g_repl.revokeSquadAuth(gw, g_net, g_net.localId(), clicked.playerId, &tab);
            else if (clicked.kind == 4 && o->second.playerId == clicked.playerId && view.authMode == 2 && view.authPlayerId == clicked.playerId)
                g_repl.revokeSquadAuth(gw, g_net, g_net.localId(), clicked.playerId, &tab);
            else continue;
            ++matched;
        }
        char log[160];
        _snprintf(log, sizeof(log) - 1, "[playerbox] action=%d single=%d target=%04X current=%d matched=%u", clicked.kind,
            clicked.single ? 1 : 0, (unsigned int)(clicked.playerId & 0xFFFFu), current ? 1 : 0, matched);
        log[sizeof(log) - 1] = 0; coopLog(log);
        g_toastText = matched ? coop::tr("Control action submitted") : coop::tr("Player or squad changed - try again");
        g_toastState = matched ? 2 : 0; g_toastArmMs = now;
        sampled = 0;
    }
    if (!active) { boxes.clear(); sampled = 0; lastWorld = 0; }
    else if (!sampled || lastWorld != world || now - sampled >= 250) {
        sampled = now; lastWorld = world; boxes.clear();
        std::vector<coop::engine::CoopPlayerSquad> roster;
        coop::engine::readPlayerBoxRoster(gw, roster);
        for (std::map<coop::u64, coop::PeerStatusPacket>::const_iterator p = g_repl.players().begin(); p != g_repl.players().end(); ++p) {
            coop::engine::CoopPlayerBox box;
            box.playerId = p->first; box.rank = p->second.ownRank;
            box.present = p->second.present != 0; box.local = p->first == g_repl.playerIdentity();
            box.name = coop::room::displayText(p->second.name, 63);
            char text[128];
            if (box.name.empty()) {
                _snprintf(text, sizeof(text) - 1, coop::tr("Player %04X"), (unsigned int)(p->first & 0xFFFFu));
                text[sizeof(text) - 1] = 0; box.name = text;
            }
            for (std::map<std::pair<coop::u32, coop::u32>, coop::OwnerRow>::const_iterator o = g_repl.owners().begin(); o != g_repl.owners().end(); ++o) {
                if (o->second.playerId != p->first) continue;
                coop::engine::CoopPlayerSquad squad;
                squad.container = o->first.first; squad.containerSerial = o->first.second; squad.loaded = false;
                for (unsigned int i = 0; i < roster.size(); ++i) {
                    std::pair<coop::u32, coop::u32> tab(roster[i].container, roster[i].containerSerial);
                    if (g_repl.canonicalTab(tab) != o->first) continue;
                    squad.name = roster[i].name; squad.loaded = squad.loaded || roster[i].loaded;
                    squad.members.insert(squad.members.end(), roster[i].members.begin(), roster[i].members.end());
                }
                if (squad.name.empty()) {
                    _snprintf(text, sizeof(text) - 1, coop::tr("Squad %u:%u"), squad.container, squad.containerSerial);
                    text[sizeof(text) - 1] = 0; squad.name = text;
                }
                box.squads.push_back(squad);
            }
            unsigned int pos = 0;
            while (pos < boxes.size() && (boxes[pos].rank < box.rank ||
                (boxes[pos].rank == box.rank && (boxes[pos].present > box.present ||
                (boxes[pos].present == box.present && boxes[pos].playerId < box.playerId))))) ++pos;
            boxes.insert(boxes.begin() + pos, box);
        }
        for (unsigned int i = 0; i < boxes.size(); ++i) {
            coop::engine::CoopPlayerBox& box = boxes[i];
            unsigned int incoming = 0, lent = 0, borrowed = 0, requestable = 0;
            char text[512];
            if (!box.present) box.status.push_back(coop::tr("Offline - control requests unavailable"));
            for (unsigned int j = 0; j < box.squads.size(); ++j) {
                coop::engine::CoopPlayerSquad& squad = box.squads[j];
                std::pair<coop::u32, coop::u32> tab(squad.container, squad.containerSerial);
                coop::Replicator::SquadAuthView view;
                g_repl.squadAuthView(tab, view);
                std::string authName = coop::tr("Unknown player"), requestName = authName;
                for (unsigned int k = 0; k < boxes.size(); ++k) {
                    if (boxes[k].playerId == view.authPlayerId) authName = boxes[k].name;
                    if (boxes[k].playerId == view.requestPlayerId) requestName = boxes[k].name;
                }
                const char* authFormat = view.requestMode == 3 ? coop::tr("Reclaiming from %s") :
                    view.authMode == 1 ? coop::tr("Lent to %s") : view.authMode == 2 ? coop::tr("Borrowed from %s") : 0;
                if (authFormat) {
                    _snprintf(text, sizeof(text) - 1, authFormat, authName.c_str()); text[sizeof(text) - 1] = 0;
                    squad.status.push_back(text);
                }
                const char* requestFormat = view.requestMode == 1 ? coop::tr("Request sent to %s") :
                    view.requestMode == 2 ? coop::tr("Request from %s") : 0;
                if (requestFormat) {
                    _snprintf(text, sizeof(text) - 1, requestFormat, requestName.c_str()); text[sizeof(text) - 1] = 0;
                    squad.status.push_back(text);
                }
                coop::engine::CoopPlayerBoxAction a = {};
                if (!box.local && view.requestable) {
                    ++requestable; a.kind = 0; a.playerId = box.playerId; a.caption = coop::tr("Request this squad"); squad.actions.push_back(a);
                }
                if (view.authMode == 2 && view.authPlayerId == box.playerId) {
                    ++borrowed; a.kind = 4; a.playerId = box.playerId; a.caption = coop::tr("Return this squad"); squad.actions.push_back(a);
                }
                if (box.local && view.requestMode == 2 && view.requestPlayerId) {
                    a.playerId = view.requestPlayerId;
                    a.kind = 1; a.caption = coop::tr("Approve this squad"); squad.actions.push_back(a);
                    a.kind = 2; a.caption = coop::tr("Deny this squad"); squad.actions.push_back(a);
                }
                if (box.local && view.authMode == 1 && view.authPlayerId && view.requestMode != 3) {
                    a.playerId = view.authPlayerId; a.kind = 3; a.caption = coop::tr("Reclaim this squad"); squad.actions.push_back(a);
                }
            }
            if (!box.local) {
                for (std::map<std::pair<coop::u32, coop::u32>, coop::OwnerRow>::const_iterator o = g_repl.owners().begin(); o != g_repl.owners().end(); ++o) {
                    if (o->second.playerId != g_repl.playerIdentity()) continue;
                    coop::Replicator::SquadAuthView view;
                    g_repl.squadAuthView(o->first, view);
                    if (view.requestMode == 2 && view.requestPlayerId == box.playerId) ++incoming;
                    if (view.authMode == 1 && view.authPlayerId == box.playerId && view.requestMode != 3) ++lent;
                }
                if (incoming) {
                    _snprintf(text, sizeof(text) - 1, coop::tr("Requests your squads: %u"), incoming); text[sizeof(text) - 1] = 0;
                    box.status.push_back(text);
                }
                coop::engine::CoopPlayerBoxAction a = {}; a.playerId = box.playerId;
                if (requestable) { a.kind = 0; a.caption = coop::tr("Request all available squads"); box.actions.push_back(a); }
                if (borrowed) { a.kind = 4; a.caption = coop::tr("Return all borrowed squads"); box.actions.push_back(a); }
                if (lent) { a.kind = 3; a.caption = coop::tr("Reclaim your squads"); box.actions.push_back(a); }
            } else {
                for (unsigned int k = 0; k < boxes.size(); ++k) {
                    if (boxes[k].local || !boxes[k].present) continue;
                    bool pending = false;
                    for (unsigned int j = 0; j < box.squads.size(); ++j) {
                        coop::Replicator::SquadAuthView view;
                        g_repl.squadAuthView(std::make_pair(box.squads[j].container, box.squads[j].containerSerial), view);
                        if (view.requestMode == 2 && view.requestPlayerId == boxes[k].playerId) { pending = true; break; }
                    }
                    if (!pending) continue;
                    coop::engine::CoopPlayerBoxAction a = {}; a.playerId = boxes[k].playerId;
                    _snprintf(text, sizeof(text) - 1, coop::tr("Approve all requests from %s"), boxes[k].name.c_str()); text[sizeof(text) - 1] = 0;
                    a.kind = 1; a.caption = text; box.actions.push_back(a);
                    _snprintf(text, sizeof(text) - 1, coop::tr("Deny all requests from %s"), boxes[k].name.c_str()); text[sizeof(text) - 1] = 0;
                    a.kind = 2; a.caption = text; box.actions.push_back(a);
                }
            }
            for (unsigned int section = 0; section <= box.squads.size(); ++section) {
                coop::engine::CoopPlayerSquad* squad = section ? &box.squads[section - 1] : 0;
                std::vector<coop::engine::CoopPlayerBoxAction>& actions = squad ? squad->actions : box.actions;
                for (unsigned int j = 0; j < actions.size();) {
                    coop::engine::CoopPlayerBoxAction& a = actions[j];
                    std::map<coop::u64, coop::PeerStatusPacket>::const_iterator peer = g_repl.players().find(a.playerId);
                    if (peer == g_repl.players().end() || !peer->second.present || !peer->second.connectionId) { actions.erase(actions.begin() + j); continue; }
                    a.single = squad != 0; a.container = squad ? squad->container : 0; a.containerSerial = squad ? squad->containerSerial : 0;
                    a.actorPlayerId = g_repl.playerIdentity(); a.world = world; a.epoch = g_repl.ownershipEpoch();
                    a.selfConnection = g_repl.characterConnection(g_net.localId());
                    a.targetOwnerId = peer->second.ownerId; a.targetConnection = peer->second.connectionId;
                    if (!a.selfConnection) { actions.erase(actions.begin() + j); continue; }
                    ++j;
                }
            }
        }
    }
    coop::engine::coopPlayerBoxesTick(boxes, active, world);
}
} // namespace
