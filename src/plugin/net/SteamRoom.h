#ifndef KENSHICOOP_STEAM_ROOM_H
#define KENSHICOOP_STEAM_ROOM_H

#include <string>

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

enum State { UNKNOWN, QUERYING, NOT_PLAYING, NO_ROOM, AVAILABLE };
struct Info {
    unsigned long long host, lobby;
    unsigned int protocol, players, limit, slots, refreshed, requested;
    State state;
    bool world;
    std::string name, save;
    Info() : host(0), lobby(0), protocol(0), players(0), limit(0), slots(0),
             refreshed(0), requested(0), state(UNKNOWN), world(false) {}
};

// 同时用于大厅数据校验和无游戏断言，拒绝空串、溢出与尾随垃圾。
inline bool parseNumber(const char* text, unsigned int maximum, unsigned int& out) {
    if (!text || !*text) return false;
    unsigned int value = 0;
    for (unsigned int i = 0; text[i]; ++i) {
        if (i >= 10 || text[i] < '0' || text[i] > '9') return false;
        unsigned int digit = (unsigned int)(text[i] - '0');
        if (digit > maximum || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value; return true;
}

inline unsigned int firstFreeSlot(unsigned int slots, unsigned int limit) {
    for (unsigned int i = 1; i < limit && i < 32; ++i) if (!(slots & (1u << i))) return i;
    return 0;
}

// 截断不拆分 UTF-8 字符；过滤控制符与界面颜色标记前缀。
inline std::string displayText(const char* text, size_t maximum) {
    std::string result;
    if (!text) return result;
    for (size_t i = 0; text[i] && i < 4096;) {
        unsigned char ch = (unsigned char)text[i];
        size_t count = ch < 0x80 ? 1 : ch >= 0xC2 && ch <= 0xDF ? 2 :
            ch >= 0xE0 && ch <= 0xEF ? 3 : ch >= 0xF0 && ch <= 0xF4 ? 4 : 0;
        if (!count || i + count > 4096) break;
        bool valid = true;
        for (size_t j = 1; j < count; ++j) if (!text[i+j] || ((unsigned char)text[i+j] & 0xC0) != 0x80) { valid = false; break; }
        if (!valid) break;
        if (count >= 3 && ((ch == 0xE0 && (unsigned char)text[i+1] < 0xA0) ||
            (ch == 0xED && (unsigned char)text[i+1] >= 0xA0) ||
            (ch == 0xF0 && (unsigned char)text[i+1] < 0x90) ||
            (ch == 0xF4 && (unsigned char)text[i+1] >= 0x90))) break;
        if (result.size() + count > maximum) break;
        if (ch >= 32 && ch != 127 && ch != '#') result.append(text + i, count);
        i += count;
    }
    return result;
}

} // namespace room
} // namespace coop
#endif
