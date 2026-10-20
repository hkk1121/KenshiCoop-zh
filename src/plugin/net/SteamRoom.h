#ifndef KENSHICOOP_STEAM_ROOM_H
#define KENSHICOOP_STEAM_ROOM_H

namespace coop {
namespace room {

enum RejectReason {
    REJECT_NONE = 0,
    REJECT_VERSION = 1,
    REJECT_SLOT = 2,
    REJECT_FULL = 3,
    REJECT_FRIEND = 4,
    REJECT_IDENTITY = 5
};

// 旧版 Steamworks Windows x64 回调布局，不能使用游戏协议的 pack(1)。
enum CallbackId { LOBBY_ENTER = 504, LOBBY_DATA = 505, LOBBY_CREATED = 513,
                  JOIN_REQUESTED = 333, P2P_REQUEST = 1202 };
#pragma pack(push, 8)
struct LobbyCreated { int result; unsigned long long lobby; };
struct LobbyEnter {
    unsigned long long lobby;
    unsigned int permissions;
    unsigned char locked;
    unsigned int response;
};
struct LobbyDataUpdate { unsigned long long lobby, member; unsigned char success; };
struct JoinRequested { unsigned long long lobby, friendId; };
struct P2PRequest { unsigned long long remote; };
struct FriendGameInfo {
    unsigned long long gameId;
    unsigned int ip;
    unsigned short gamePort, queryPort;
    unsigned long long lobby;
};
#pragma pack(pop)

// 通道2只诊断连接拒绝，不参与游戏准入、归属或授权。
#pragma pack(push, 1)
struct Diagnostic { char tag[4]; unsigned int reason; };
#pragma pack(pop)

} // namespace room
} // namespace coop
#endif
