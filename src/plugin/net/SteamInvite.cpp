#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "SteamInvite.h"
#include "SteamRoom.h"
#include "NetLink.h"
#include "SteamP2P.h"        // selfId() + accept() (shared Steam client)
#include "../CoopLog.h"
#include "../../netproto/Wire.h" // coop::PROTOCOL_VERSION (lobby-data version gate)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdlib> // atoi (lobby protocol-version gate)

namespace coop {
namespace steaminvite {
namespace {

// ---- Steamworks callback ABI (hand-rolled; no SDK headers) -------------------
// CCallbackBase (steam_api_common.h) layout the client's callback manager expects:
//   [0] vptr
//   [8] uint8  m_nCallbackFlags
//   [12] int   m_iCallback
// vtable order: Run(void*) / Run(void*,bool,SteamAPICall_t) / GetCallbackSizeBytes().
// We match it with a plain class whose 3 virtuals are declared in that exact order;
// MSVC lays vptr at 0 and packs the two members at 8/12. Our own fields live past
// offset 16 where the manager never looks. Dispatch routes on m_iCallback.

typedef unsigned long long SteamAPICall_t;

void dispatch(int iCallback, void* param, bool ioFailure); // fwd

class SteamCallback {
public:
    virtual void Run(void* param) { dispatch(m_iCallback, param, false); }
    virtual void Run(void* param, bool ioFailure, SteamAPICall_t) {
        dispatch(m_iCallback, param, ioFailure);
    }
    virtual int GetCallbackSizeBytes() { return m_size; }

    unsigned char m_nCallbackFlags; // offset 8  (written by RegisterCallback)
    int           m_iCallback;      // offset 12 (callback id)
    int           m_size;           // ours: sizeof the callback param struct

    void arm(int id, int sz) { m_nCallbackFlags = 0; m_iCallback = id; m_size = sz; }
};

// ---- Callback ids ------------------------------------------------------------
const int cb_LobbyCreated = room::LOBBY_CREATED;
const int cb_LobbyEnter = room::LOBBY_ENTER;
const int cb_GameLobbyJoinRequested = room::JOIN_REQUESTED;
const int cb_P2PSessionRequest = room::P2P_REQUEST;
typedef room::LobbyCreated LobbyCreated_t;
typedef room::LobbyEnter LobbyEnter_t;
typedef room::JoinRequested GameLobbyJoinRequested_t;
typedef room::P2PRequest P2PSessionRequest_t;

const int k_EResultOK           = 1;
const int k_ELobbyTypeFriendsOnly = 1;

// ---- Flat API surface --------------------------------------------------------
typedef int   (*GetHSteamFn)();
typedef void* (*SteamClientFn)();
typedef void* (*ClientGetIfaceFn)(void* client, int hUser, int hPipe, const char* version);

typedef SteamAPICall_t (*MmCreateLobbyFn)(void* mm, int eLobbyType, int cMaxMembers);
typedef SteamAPICall_t (*MmJoinLobbyFn)(void* mm, unsigned long long lobby);
typedef void  (*MmLeaveLobbyFn)(void* mm, unsigned long long lobby);
typedef bool  (*MmInviteToLobbyFn)(void* mm, unsigned long long lobby, unsigned long long invitee);
typedef bool  (*MmSetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k, const char* v);
typedef const char* (*MmGetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k);
typedef unsigned long long (*MmGetLobbyOwnerFn)(void* mm, unsigned long long lobby);
typedef int   (*MmGetNumMembersFn)(void* mm, unsigned long long lobby);
typedef unsigned long long (*MmGetMemberByIndexFn)(void* mm, unsigned long long lobby, int i);
typedef void  (*FriendsInviteDialogFn)(void* friends, unsigned long long lobby);
typedef int   (*FrGetCountFn)(void* friends, int eFriendFlags);
typedef unsigned long long (*FrGetByIndexFn)(void* friends, int i, int eFriendFlags);
typedef const char* (*FrGetNameFn)(void* friends, unsigned long long id);
typedef int   (*FrGetStateFn)(void* friends, unsigned long long id);

typedef room::FriendGameInfo FriendGameInfo_t;
typedef bool  (*FrGetGamePlayedFn)(void* friends, unsigned long long id, FriendGameInfo_t* out);

const unsigned KENSHI_APPID = 233860;
typedef void  (*RegisterCbFn)(void* cb, int iCallback);
typedef void  (*RegisterCrFn)(void* cb, SteamAPICall_t call);
typedef void  (*UnregisterCbFn)(void* cb);
typedef void  (*RunCallbacksFn)();

void*                 g_mm       = 0; // ISteamMatchmaking*
void*                 g_friends  = 0; // ISteamFriends*
MmCreateLobbyFn       g_createLobby = 0;
MmJoinLobbyFn         g_joinLobby   = 0;
MmLeaveLobbyFn        g_leaveLobby  = 0;
MmInviteToLobbyFn     g_inviteToLobby = 0;
MmSetLobbyDataFn      g_setData     = 0;
MmGetLobbyDataFn      g_getData     = 0;
MmGetLobbyOwnerFn     g_getOwner    = 0;
MmGetNumMembersFn     g_getNum      = 0;
MmGetMemberByIndexFn  g_getMember   = 0;
FriendsInviteDialogFn g_inviteDlg   = 0;
FrGetCountFn          g_frCount     = 0;
FrGetByIndexFn        g_frByIndex   = 0;
FrGetNameFn           g_frName      = 0;
FrGetStateFn          g_frState     = 0;
FrGetGamePlayedFn     g_frGame      = 0;
RegisterCbFn          g_regCb       = 0;
RegisterCrFn          g_regCr       = 0;
UnregisterCbFn        g_unregCb     = 0;
RunCallbacksFn        g_runCb       = 0;

bool      g_ready     = false;
ConnectFn g_onConnect = 0;

// Invite/lobby state (main thread only).
unsigned long long g_lobby        = 0;    // our active lobby (0 = none)
bool               g_weAreHost    = false;// created the lobby (vs joined a friend's)
bool               g_hostFired    = false;// host already connected to the arrived friend
bool               g_creating     = false;// CreateLobby call in flight
bool               g_picker       = false;// in-panel friend picker is active
unsigned long long g_pendingInvitee = 0;  // friend clicked before the lobby existed
char               g_status[128]  = {0};
bool g_wantHost = false, g_createAbandoned = false;
DWORD g_lastLobbyAttempt = 0;
SteamAPICall_t g_createCall = 0;
typedef void (*UnregisterCrFn)(void*, SteamAPICall_t);
UnregisterCrFn g_unregCr = 0;
unsigned long long g_joiningLobby = 0, g_connectedHost = 0;
typedef bool (*RequestDataFn)(void*, SteamId);
typedef bool (*RichPresenceFn)(void*, const char*, const char*);
typedef bool (*HasFriendFn)(void*, SteamId, int);
RequestDataFn g_requestData = 0;
RichPresenceFn g_richPresence = 0;
HasFriendFn g_hasFriend = 0;
SteamCallback g_cbLobbyData;
room::Info g_watched, g_advert;
std::map<std::string, std::string> g_published;
std::vector<room::Info> g_rooms;
bool g_browser = false;
void queryRoom(room::Info& info);
void readRoom(room::Info& info, room::LobbyDataUpdate* data);
void publishRoom();
void createLobbyIfNeeded();

// Cached friend list for the in-panel picker (our own copy - GetFriendPersonaName
// returns a Steam-owned pointer only valid until the next call). Sorted in-Kenshi
// first, then online, then offline. state: 0 offline, 1 online, 2 in Kenshi.
struct FriendRow { unsigned long long id; char name[64]; int state; };
const int          MAX_FRIENDS = 32;
FriendRow          g_friendRows[MAX_FRIENDS];
int                g_friendN     = 0;
unsigned int       g_lastFriendRefresh = 0;

SteamCallback g_cbLobbyCreated;
SteamCallback g_cbLobbyEnter;
SteamCallback g_cbJoinRequested;
SteamCallback g_cbP2PRequest;

void steamLog(const char* msg) {
    char b[256];
    _snprintf(b, sizeof(b) - 1, "[invite] %s", msg ? msg : "");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
void setStatus(const char* s) {
    _snprintf(g_status, sizeof(g_status) - 1, "%s", s ? s : "");
    g_status[sizeof(g_status) - 1] = '\0';
}

// True if a friend is currently playing Kenshi (checked via GetFriendGamePlayed).
bool friendInKenshi(unsigned long long fid) {
    if (!g_frGame) return false;
    FriendGameInfo_t gi;
    std::memset(&gi, 0, sizeof(gi));
    if (!g_frGame(g_friends, fid, &gi)) return false;
    return (unsigned)(gi.gameId & 0xFFFFFFu) == KENSHI_APPID;
}

// (Re)build the friend cache for the in-panel picker: enumerate immediate friends,
// copy names into our own buffers, tag state (2 = in Kenshi, 1 = online, 0 = off),
// and sort in-Kenshi > online > offline so the useful ones surface first.
void refreshFriends() {
    g_friendN = 0;
    if (!g_ready || !g_frCount || !g_frByIndex || !g_frName) return;
    std::vector<room::Info> next;
    int n = g_frCount(g_friends, 0x04);
    for (int i = 0; i < n; ++i) {
        SteamId id = g_frByIndex(g_friends, i, 0x04);
        FriendGameInfo_t game; std::memset(&game, 0, sizeof(game));
        if (!id || !g_frGame(g_friends, id, &game) || (game.gameId & 0xFFFFFFu) != KENSHI_APPID || !game.lobby) continue;
        // 先筛选所有好友的游戏/大厅状态，再限制候选缓存数量。
        if (next.size() >= MAX_FRIENDS) break;
        room::Info info; info.host = id;
        for (size_t j = 0; j < g_rooms.size(); ++j) if (g_rooms[j].host == id && g_rooms[j].lobby == game.lobby) { info = g_rooms[j]; break; }
        queryRoom(info);
        if (info.state == room::AVAILABLE && GetTickCount() - info.refreshed > 8000) info.state = room::UNKNOWN;
        next.push_back(info);
        FriendRow& row = g_friendRows[g_friendN++]; row.id = id; row.state = 2;
        std::string name = room::displayText(g_frName(g_friends, id), sizeof(row.name)-1);
        std::memcpy(row.name, name.c_str(), name.size()+1);
    }
    g_rooms.swap(next);
}

// ---- Callback handlers (main thread, via Kenshi's SteamAPI_RunCallbacks) -----

void onLobbyCreated(LobbyCreated_t* r, bool ioFailure) {
    g_creating = false; g_createCall = 0;
    if (ioFailure || !r || r->result != k_EResultOK) {
        steamLog("lobby creation FAILED");
        setStatus("Lobby creation failed - try again or type the ID");
        return;
    }
    if (!g_wantHost || g_createAbandoned) {
        if (g_leaveLobby) g_leaveLobby(g_mm, r->lobby);
        g_createAbandoned = false;
        if (g_wantHost) createLobbyIfNeeded();
        return;
    }
    g_published.clear();
    g_lobby     = r->lobby;
    g_weAreHost = true;
    g_hostFired = false;
    char pv[16];
    _snprintf(pv, sizeof(pv) - 1, "%u", (unsigned)coop::PROTOCOL_VERSION);
    pv[sizeof(pv) - 1] = '\0';
    if (g_setData) {
        g_setData(g_mm, g_lobby, "kc_protocol", pv);
        g_setData(g_mm, g_lobby, "kc_game", "KenshiCoop");
    }
    publishRoom();
    char b[96];
    _snprintf(b, sizeof(b) - 1, "lobby created %llu (protocol %s)", g_lobby, pv);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    // If a friend was picked before the lobby existed, invite them now.
    if (g_pendingInvitee != 0) {
        unsigned long long who = g_pendingInvitee;
        g_pendingInvitee = 0;
        if (g_inviteToLobby) g_inviteToLobby(g_mm, g_lobby, who);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "sent lobby invite to %llu", who);
        c[sizeof(c) - 1] = '\0';
        steamLog(c);
        setStatus("Invite sent - waiting for friend to accept...");
    } else {
        setStatus("Pick a friend to invite");
    }
}

void onGameLobbyJoinRequested(GameLobbyJoinRequested_t* r) {
    if (!r || !g_joinLobby || r->lobby == g_lobby || r->lobby == g_joiningLobby) return;
    if (g_connectedHost && g_getOwner && g_getOwner(g_mm, r->lobby) == g_connectedHost) return;
    g_joiningLobby = r->lobby;
    // The friend clicked our invite / "Join Game". Enter their lobby; the actual
    // connect happens in onLobbyEnter once membership + owner are known.
    char b[96];
    _snprintf(b, sizeof(b) - 1, "join requested for lobby %llu - entering...",
              r->lobby);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    setStatus("Joining friend's game...");
    g_joinLobby(g_mm, r->lobby);
}

void onLobbyEnter(LobbyEnter_t* r) {
    if (!r || r->response != 1) { g_joiningLobby = 0; return; }
    unsigned long long lobby = r->lobby;
    unsigned long long owner = g_getOwner ? g_getOwner(g_mm, lobby) : 0;
    SteamId self = (SteamId)coop::steamp2p::selfId();
    if (owner == self) {
        if (g_wantHost && !g_createAbandoned) {
            if (g_lobby != lobby) g_published.clear();
            g_lobby = lobby; g_weAreHost = true; publishRoom();
        } else if (g_leaveLobby) g_leaveLobby(g_mm, lobby);
        return;
    }
    if (g_joiningLobby != lobby) {
        if (g_leaveLobby && lobby != g_lobby) g_leaveLobby(g_mm, lobby);
        return;
    }
    if (!owner || !g_getData || std::strcmp(g_getData(g_mm, lobby, "kc_game"), "KenshiCoop")) {
        if (g_leaveLobby) g_leaveLobby(g_mm, lobby);
        g_joiningLobby = 0; return;
    }
    // Joiner path: the owner is the host. Optionally gate on protocol version.
    if (g_getData) {
        const char* hp = g_getData(g_mm, lobby, "kc_protocol");
        if (hp && hp[0]) {
            unsigned hv = (unsigned)atoi(hp);
            if (hv != (unsigned)coop::PROTOCOL_VERSION) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "protocol mismatch: host v%u, you v%u - update the mod on both sides",
                          hv, (unsigned)coop::PROTOCOL_VERSION);
                b[sizeof(b) - 1] = '\0';
                steamLog(b);
                setStatus("Version mismatch with host - update both sides");
                setNetUiError("Version mismatch with host - update both to the same KenshiCoop build");
                if (g_leaveLobby) g_leaveLobby(g_mm, lobby);
                g_joiningLobby = 0;
                return;
            }
        }
    }

    room::Info joined; joined.host = owner; joined.lobby = lobby;
    room::LobbyDataUpdate update; update.lobby = lobby; update.member = lobby; update.success = 1;
    readRoom(joined, &update);
    unsigned int rank = joined.state == room::AVAILABLE ? room::firstFreeSlot(joined.slots, joined.limit) : 0;
    if (g_leaveLobby) g_leaveLobby(g_mm, lobby);
    g_joiningLobby = 0;
    if (g_connectedHost == owner) return;
    coop::steamp2p::accept(owner); // pre-accept the host's P2P session
    char b[96];
    _snprintf(b, sizeof(b) - 1, "joined host %llu via lobby - connecting as CLIENT", owner);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    setStatus("Connecting to host...");
    if (g_onConnect) { SteamId one = owner; g_onConnect(false, true, &one, 1, rank); }
}

void onP2PSessionRequest(P2PSessionRequest_t* r) {
    if (!r) return;
    // Accept any inbound session from within an invite flow so the tunnel opens
    // regardless of which side sends first.
    coop::steamp2p::accept(r->remote);
}

void dispatch(int iCallback, void* param, bool ioFailure) {
    switch (iCallback) {
        case cb_LobbyCreated:           onLobbyCreated((LobbyCreated_t*)param, ioFailure); break;
        case cb_LobbyEnter:             onLobbyEnter((LobbyEnter_t*)param); break;
        case cb_GameLobbyJoinRequested: onGameLobbyJoinRequested((GameLobbyJoinRequested_t*)param); break;
        case room::LOBBY_DATA:
            readRoom(g_watched, (room::LobbyDataUpdate*)param);
            for (size_t i = 0; i < g_rooms.size(); ++i) readRoom(g_rooms[i], (room::LobbyDataUpdate*)param);
            break;
        case cb_P2PSessionRequest:      onP2PSessionRequest((P2PSessionRequest_t*)param); break;
        default: break;
    }
}

void* getIface(void* client, GetHSteamFn getUser, GetHSteamFn getPipe,
               ClientGetIfaceFn get, const char* const* versions, int n, const char** used) {
    void* p = 0;
    for (int i = 0; i < n && p == 0; ++i) {
        p = get(client, getUser(), getPipe(), versions[i]);
        if (p != 0 && used) *used = versions[i];
    }
    return p;
}

} // namespace

// ---- Public API --------------------------------------------------------------

bool init(ConnectFn onConnect) {
    if (g_ready) { if (onConnect) g_onConnect = onConnect; return true; }
    g_onConnect = onConnect;

    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (mod == 0) { steamLog("init deferred: steam_api64.dll not loaded"); return false; }

    GetHSteamFn      getUser   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamUser");
    GetHSteamFn      getPipe   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamPipe");
    SteamClientFn    getClient = (SteamClientFn)   GetProcAddress(mod, "SteamClient");
    ClientGetIfaceFn getMmIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    ClientGetIfaceFn getFrIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamFriends");

    g_createLobby = (MmCreateLobbyFn)      GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_CreateLobby");
    g_joinLobby   = (MmJoinLobbyFn)        GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_JoinLobby");
    g_leaveLobby  = (MmLeaveLobbyFn)       GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    g_inviteToLobby = (MmInviteToLobbyFn)  GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    g_setData     = (MmSetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    g_getData     = (MmGetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    g_getOwner    = (MmGetLobbyOwnerFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    g_getNum      = (MmGetNumMembersFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    g_getMember   = (MmGetMemberByIndexFn) GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    g_inviteDlg   = (FriendsInviteDialogFn)GetProcAddress(mod, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    g_frCount     = (FrGetCountFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendCount");
    g_frByIndex   = (FrGetByIndexFn)       GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendByIndex");
    g_frName      = (FrGetNameFn)          GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_frState     = (FrGetStateFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    g_frGame      = (FrGetGamePlayedFn)    GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    g_regCb       = (RegisterCbFn)         GetProcAddress(mod, "SteamAPI_RegisterCallback");
    g_regCr       = (RegisterCrFn)         GetProcAddress(mod, "SteamAPI_RegisterCallResult");
    g_unregCb     = (UnregisterCbFn)       GetProcAddress(mod, "SteamAPI_UnregisterCallback");
    g_requestData = (RequestDataFn)GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    g_richPresence = (RichPresenceFn)GetProcAddress(mod, "SteamAPI_ISteamFriends_SetRichPresence");
    g_hasFriend = (HasFriendFn)GetProcAddress(mod, "SteamAPI_ISteamFriends_HasFriend");
    g_unregCr = (UnregisterCrFn)GetProcAddress(mod, "SteamAPI_UnregisterCallResult");
    g_runCb       = (RunCallbacksFn)       GetProcAddress(mod, "SteamAPI_RunCallbacks");

    if (!getUser || !getPipe || !getClient || !getMmIf || !getFrIf || !g_createLobby ||
        !g_joinLobby || !g_setData || !g_getData || !g_getOwner || !g_getNum ||
        !g_getMember || !g_inviteToLobby || !g_frCount || !g_frByIndex || !g_frName ||
        !g_frGame || !g_hasFriend || !g_requestData || !g_regCb || !g_regCr || !g_unregCr || !g_leaveLobby || !g_runCb) {
        steamLog("init FAILED: invite/lobby flat exports missing");
        return false;
    }

    void* client = getClient();
    if (client == 0) { steamLog("init FAILED: SteamClient() null (Steam not running?)"); return false; }
    if (getUser() == 0 || getPipe() == 0) { steamLog("init FAILED: no Steam user/pipe"); return false; }

    static const char* const mmVers[] = { "SteamMatchMaking009", "SteamMatchMaking008", "SteamMatchMaking010" };
    static const char* const frVers[] = { "SteamFriends015", "SteamFriends014", "SteamFriends017", "SteamFriends016" };
    const char* usedMm = 0; const char* usedFr = 0;
    g_mm      = getIface(client, getUser, getPipe, getMmIf, mmVers, 3, &usedMm);
    g_friends = getIface(client, getUser, getPipe, getFrIf, frVers, 4, &usedFr);
    if (g_mm == 0)      { steamLog("init FAILED: no ISteamMatchmaking interface"); return false; }
    if (g_friends == 0) { steamLog("init FAILED: no ISteamFriends interface"); return false; }

    // Register the persistent (non-call-result) callbacks. LobbyCreated is a call
    // result, registered per-CreateLobby in hostInvite().
    g_cbLobbyEnter.arm(cb_LobbyEnter, sizeof(LobbyEnter_t));
    g_cbJoinRequested.arm(cb_GameLobbyJoinRequested, sizeof(GameLobbyJoinRequested_t));
    g_cbP2PRequest.arm(cb_P2PSessionRequest, sizeof(P2PSessionRequest_t));
    g_regCb(&g_cbLobbyEnter, cb_LobbyEnter);
    g_regCb(&g_cbJoinRequested, cb_GameLobbyJoinRequested);
    g_regCb(&g_cbP2PRequest, cb_P2PSessionRequest);
    g_cbLobbyData.arm(room::LOBBY_DATA, sizeof(room::LobbyDataUpdate));
    g_regCb(&g_cbLobbyData, room::LOBBY_DATA);

    g_ready = true;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "ready: mm=%s friends=%s (in-panel friend picker armed)",
              usedMm ? usedMm : "?", usedFr ? usedFr : "?");
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    return true;
}

bool ready() { return g_ready; }

namespace {
// 常驻好友大厅只负责发现；游戏在线人数来自 ENet 准入，不随大厅成员变动重连。
void createLobbyIfNeeded() {
    if (g_lobby != 0 || g_creating) return;
    g_creating = true; g_createAbandoned = false; g_lastLobbyAttempt = GetTickCount();
    steamLog("creating friends-only 4-player lobby...");
    SteamAPICall_t call = g_createLobby(g_mm, k_ELobbyTypeFriendsOnly,
                                        (int)coop::MAX_PLAYERS);
    if (call == 0) {
        g_creating = false;
        steamLog("CreateLobby returned no call handle");
        setStatus("Lobby creation failed - type the ID instead");
        return;
    }
    // Re-arm the LobbyCreated call-result object for this specific async call.
    if (g_createCall && g_unregCr) g_unregCr(&g_cbLobbyCreated, g_createCall);
    g_createCall = call;
    g_cbLobbyCreated.arm(cb_LobbyCreated, sizeof(LobbyCreated_t));
    g_regCr(&g_cbLobbyCreated, call);
}
} // namespace

void beginInvite() {
    if (!g_ready) { steamLog("beginInvite: Steam invite layer not ready"); setStatus("Steam unavailable - type the ID instead"); return; }
    g_picker = true;
    refreshFriends();
    createLobbyIfNeeded();
    if (g_status[0] == '\0') setStatus("Pick a friend to invite");
    steamLog("friend picker opened");
}

void inviteFriend(SteamId id) {
    if (!g_ready || id == 0) return;
    createLobbyIfNeeded(); // no-op if already have one / creating
    // Look up the name for a friendlier status line.
    const char* nm = 0;
    for (int i = 0; i < g_friendN; ++i) if (g_friendRows[i].id == id) { nm = g_friendRows[i].name; break; }
    if (g_lobby != 0 && g_inviteToLobby) {
        g_inviteToLobby(g_mm, g_lobby, id);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "sent lobby invite to %llu (%s)", id, nm ? nm : "?");
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
    } else {
        // Lobby still being created; send the invite the moment it exists.
        g_pendingInvitee = id;
        steamLog("lobby not ready yet - invite queued");
    }
    char s[96];
    _snprintf(s, sizeof(s) - 1, "Invited %s - waiting for them to accept...", nm ? nm : "friend");
    s[sizeof(s) - 1] = '\0';
    setStatus(s);
}

bool pickerActive()            { return g_picker; }
int  friendCount()             { return g_friendN; }
SteamId friendId(int i)        { return (i >= 0 && i < g_friendN) ? g_friendRows[i].id : 0; }
const char* friendName(int i)  { return (i >= 0 && i < g_friendN) ? g_friendRows[i].name : ""; }
int  friendState(int i)        { return (i >= 0 && i < g_friendN) ? g_friendRows[i].state : 0; }

void tick() {
    if (!g_ready) return;
    // Drive the Steam callback pump ourselves: Kenshi does not call
    // SteamAPI_RunCallbacks (its async CreateLobby result never dispatched until
    // we added this), so our invite/lobby/join/P2P callbacks + call-results only
    // fire from here. Safe even if the game did pump - RunCallbacks just drains
    // the message queue, so sequential main-thread calls can't double-deliver a
    // single posted callback. Main thread only (this runs inside mainLoop_hook).
    if (g_runCb) g_runCb();
    if (g_watched.host && (!g_watched.requested || GetTickCount() - g_watched.requested >= 4000)) queryRoom(g_watched);
    if (g_watched.state == room::QUERYING && GetTickCount() - g_watched.requested >= 3500) g_watched.state = room::UNKNOWN;
    if (g_watched.state == room::AVAILABLE && GetTickCount() - g_watched.refreshed > 8000) g_watched.state = room::UNKNOWN;
    if (g_wantHost && !g_lobby && !g_creating && GetTickCount() - g_lastLobbyAttempt >= 5000) createLobbyIfNeeded();
    if (g_wantHost && g_lobby) publishRoom();

    // Refresh the picker's friend list every ~3s while it's open (online / in-game
    // status changes over time). Skip once the host has already connected.
    if (g_browser || (g_picker && !g_hostFired)) {
        unsigned int now = GetTickCount();
        if (g_lastFriendRefresh == 0 || now - g_lastFriendRefresh >= 4000) {
            g_lastFriendRefresh = now;
            refreshFriends();
        }
    }

    // 大厅常驻；成员加入由 P2P 回调与 HELLO 准入处理，不重启主机会话。

}

const char* status() { return g_status; }

void reset() {
    if (g_ready && g_lobby && g_weAreHost && g_setData) g_setData(g_mm, g_lobby, "kc_open", "0");
    if (g_ready && g_lobby != 0 && g_leaveLobby) g_leaveLobby(g_mm, g_lobby);
    if (g_ready && g_joiningLobby && g_leaveLobby) g_leaveLobby(g_mm, g_joiningLobby);
    if (g_ready && g_richPresence) g_richPresence(g_friends, "status", "");
    g_published.clear();
    g_lobby     = 0;
    g_weAreHost = false;
    g_hostFired = false;
    // 创建仍在途时保留回调；旧请求结果只离开，新主机会话另建大厅。
    if (g_creating) g_createAbandoned = true;
    g_picker    = false;
    g_pendingInvitee = 0;
    g_friendN   = 0;
    g_rooms.clear(); g_browser = false; g_lastFriendRefresh = 0;
    g_status[0] = '\0';
    g_wantHost = false; g_connectedHost = 0; g_joiningLobby = 0;

}

void hostRoom() {
    if (!g_ready) return;
    g_wantHost = true;
    createLobbyIfNeeded();
}

void updateSession(bool hosting, SteamId connectedHost, const char* saveUtf8,
                   bool world, unsigned int players, unsigned int slots) {
    g_connectedHost = connectedHost;
    g_advert.name = room::displayText(g_ready ? g_frName(g_friends, steamp2p::selfId()) : steamp2p::selfName(), 240);
    g_advert.save = room::displayText(saveUtf8, 240);
    g_advert.world = world; g_advert.players = players; g_advert.slots = slots;
    if (hosting && !g_wantHost) hostRoom();
    else if (!hosting && g_wantHost) reset();
}

void watchRoom(SteamId host) {
    if (g_watched.host == host) return;
    g_watched = room::Info(); g_watched.host = host;
    if (g_ready && host) queryRoom(g_watched);
}

const room::Info& watchedRoom() { return g_watched; }

namespace {
void queryRoom(room::Info& info) {
    if (!info.host || !g_ready) return;
    info.requested = GetTickCount();
    if (!g_hasFriend(g_friends, info.host, 0x04)) { info.state = room::UNKNOWN; info.lobby = 0; return; }
    FriendGameInfo_t game; std::memset(&game, 0, sizeof(game));
    if (!g_frGame(g_friends, info.host, &game) || (game.gameId & 0xFFFFFFu) != KENSHI_APPID) {
        info.state = room::NOT_PLAYING; info.lobby = 0; return;
    }
    if (!game.lobby) { info.state = room::NO_ROOM; info.lobby = 0; return; }
    if (info.lobby != game.lobby) { info.state = room::QUERYING; info.refreshed = 0; }
    info.lobby = game.lobby;
    if (!info.refreshed) info.state = room::QUERYING;
    info.name = room::displayText(g_frName(g_friends, info.host), 240);
    if (!g_requestData(g_mm, info.lobby)) info.state = room::UNKNOWN;
}

void readRoom(room::Info& info, room::LobbyDataUpdate* data) {
    if (!data || !info.host || info.lobby != data->lobby || data->member != data->lobby) return;
    info.state = room::UNKNOWN;
    if (!data->success || g_getOwner(g_mm, info.lobby) != info.host ||
        std::strcmp(g_getData(g_mm, info.lobby, "kc_game"), "KenshiCoop")) return;
    unsigned int open = 0, world = 0;
    if (!room::parseNumber(g_getData(g_mm, info.lobby, "kc_open"), 1, open) || !open) { info.state = room::NO_ROOM; return; }
    if (!room::parseNumber(g_getData(g_mm, info.lobby, "kc_protocol"), 65535, info.protocol) || !info.protocol ||
        !room::parseNumber(g_getData(g_mm, info.lobby, "kc_players"), coop::MAX_PLAYERS, info.players) || !info.players ||
        !room::parseNumber(g_getData(g_mm, info.lobby, "kc_limit"), coop::MAX_PLAYERS, info.limit) || info.limit != coop::MAX_PLAYERS || info.players > info.limit ||
        !room::parseNumber(g_getData(g_mm, info.lobby, "kc_slots"), (1u << coop::MAX_PLAYERS) - 1, info.slots) || !(info.slots & 1) ||
        !room::parseNumber(g_getData(g_mm, info.lobby, "kc_world"), 1, world)) return;
    unsigned int count = 0;
    for (unsigned int i = 0; i < coop::MAX_PLAYERS; ++i) if (info.slots & (1u << i)) ++count;
    if (count != info.players) return;
    info.world = world != 0;
    info.name = room::displayText(g_getData(g_mm, info.lobby, "kc_host"), 240);
    info.save = room::displayText(g_getData(g_mm, info.lobby, "kc_save"), 240);
    info.state = room::AVAILABLE; info.refreshed = GetTickCount();
}

void publishRoom() {
    if (!g_wantHost || !g_lobby || !g_setData) return;
    const char* keys[] = { "kc_game", "kc_protocol", "kc_host", "kc_save", "kc_players", "kc_limit", "kc_slots", "kc_world", "kc_open" };
    std::string values[] = { "KenshiCoop", std::to_string((unsigned long long)coop::PROTOCOL_VERSION),
        g_advert.name, g_advert.save, std::to_string((unsigned long long)g_advert.players),
        std::to_string((unsigned long long)coop::MAX_PLAYERS), std::to_string((unsigned long long)g_advert.slots),
        g_advert.world ? "1" : "0", "1" };
    for (unsigned int i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        if (g_published.find(keys[i]) != g_published.end() && g_published[keys[i]] == values[i]) continue;
        if (g_setData(g_mm, g_lobby, keys[i], values[i].c_str())) g_published[keys[i]] = values[i];
    }
    if (g_richPresence && g_published["presence"] != "1" && g_richPresence(g_friends, "status", "Hosting KenshiCoop"))
        g_published["presence"] = "1";
}
} // namespace

void browseRooms(bool active) {
    if (active && !g_browser) g_lastFriendRefresh = 0;
    g_browser = active;
}

int roomCount() {
    int count = 0;
    for (size_t i = 0; i < g_rooms.size(); ++i)
        if (g_rooms[i].state == room::AVAILABLE && GetTickCount() - g_rooms[i].refreshed <= 8000) ++count;
    return count;
}

const room::Info* friendRoom(int index) {
    if (index < 0) return 0;
    for (size_t i = 0; i < g_rooms.size(); ++i) {
        if (g_rooms[i].state != room::AVAILABLE || GetTickCount() - g_rooms[i].refreshed > 8000) continue;
        if (index-- == 0) return &g_rooms[i];
    }
    return 0;
}

} // namespace steaminvite
} // namespace coop
