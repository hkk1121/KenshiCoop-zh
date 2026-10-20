// SteamInvite：常驻好友大厅、房间查询与 Steam 菜单加入。
// 所有大厅数据与回调均由主线程 tick 驱动；面板直连只查询、不进入大厅。
// Steam 菜单加入时临时进入大厅取得主机身份，离开后走同一连接入口。
// 新成员绝不触发主机重新联机，实际准入仍由 SteamP2P 与 NetLink 校验。

#ifndef KENSHICOOP_STEAMINVITE_H
#define KENSHICOOP_STEAMINVITE_H

#include "SteamRoom.h"

namespace coop {
namespace steaminvite {

typedef unsigned long long SteamId;

// Fired (on the main thread, from the Steam callback pump / tick) when an invite
// resolves a peer. Matches Plugin.cpp's coopUiConnect (the protocol-49 list
// form; the invite layer always resolves exactly one peer, so it passes a
// one-element list and no squad-slot override).
typedef void (*ConnectFn)(bool isHost, bool useSteam,
                          const unsigned long long* peerIds,
                          unsigned int peerCount, unsigned int ownRank);

// Resolve ISteamMatchmaking/ISteamFriends from the game's steam_api64.dll and
// register the invite/lobby/P2P Steam callbacks. Idempotent; safe to call every
// frame. Requires steamp2p::init() to have succeeded first (shared Steam client).
// Returns false (and logs why) when the interfaces are unavailable.
bool init(ConnectFn onConnect);
bool ready();

// Host action: open the in-panel friend picker. Creates the lobby (async) and
// refreshes the friend list. (Replaces the Steam overlay invite dialog, whose
// friend list is broken by a Steam-client web-view bug on many titles.)
void beginInvite();

// Send a direct Steam lobby invite to a specific friend (from the picker). The
// friend gets a Steam notification; clicking it fires GameLobbyJoinRequested on
// their side and auto-joins. If the lobby is still being created, the invite is
// queued and sent on completion.
void inviteFriend(SteamId id);

// In-panel picker accessors (main thread). The list is cached in our own buffers
// (Steam's GetFriendPersonaName pointer is transient) and sorted in-Kenshi first.
// friendState: 0 = offline, 1 = online, 2 = currently playing Kenshi.
bool        pickerActive();
int         friendCount();
SteamId     friendId(int i);
const char* friendName(int i);
int         friendState(int i);

// Main-thread housekeeping: pumps Steam callbacks, refreshes the picker list, and
// polls host-side lobby membership for the arriving friend. Call every frame.
void tick();

// One-line human status for the panel/overlay (never null; "" when idle).
const char* status();

// Leave any lobby and clear invite state (called on disconnect / teardown).
void reset();

// 主线程：Steam 主机会话常驻大厅，成员到达不触发重连。
void hostRoom();
void updateSession(bool hosting, SteamId connectedHost, const char* saveUtf8,
                   bool world, unsigned int players, unsigned int slots);
void watchRoom(SteamId host);
const room::Info& watchedRoom();
void browseRooms(bool active);
int roomCount();
const room::Info* friendRoom(int index);
// 房间列表“刷新房间”：立即重查好友房间并重新测延迟。
void refreshRooms();
// 本机 Steam 主持的房间：从 Steam 读回已发布的大厅数据，按好友同样的规则校验。
// state：UNKNOWN / NO_ROOM = 未主持或数据异常，QUERYING = 大厅创建中，AVAILABLE = 正常。
const room::Info& ownRoom();

} // namespace steaminvite
} // namespace coop

#endif // KENSHICOOP_STEAMINVITE_H
