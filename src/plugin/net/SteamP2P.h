// SteamP2P - Steam P2P datagram transport over Kenshi's own steam_api64.dll.
//
// Kenshi ships the legacy ISteamNetworking P2P API (SendP2PPacket/ReadP2PPacket,
// confirmed in the DLL's flat exports; interface era SteamClient017/SteamUser019).
// Valve brokers connections BY STEAMID: UDP NAT hole-punch first, silent relay
// through Valve's network when punching fails. That removes IPs, port forwarding
// and router/CGNAT problems from the co-op session entirely.
//
// Design: this module does NOT replace the wire protocol - it is a datagram pipe.
// NetLink keeps running the stock ENet protocol (HELLO/WELCOME, channels,
// reliability, reconnect); the vendored ENet's socket layer is redirected here
// via enet_set_socket_hooks() (patch 0002), so every ENet datagram rides one
// unreliable Steam P2P packet on channel 0. UDP stays the default transport.
//
// 网络线程独占动态隧道表；主线程会话回调只写有锁队列。
// 启动前配置直接好友/手动放行策略，停止网络线程后才能 shutdown。
// Steam 旧接口的 IPC 调用可跨线程，游戏对象与界面仍只在主线程访问。

#ifndef KENSHICOOP_STEAMP2P_H
#define KENSHICOOP_STEAMP2P_H

struct _ENetHost;

namespace coop {
namespace steamp2p {

typedef unsigned long long SteamId; // steamid64

// Resolve the flat API from the game's already-loaded steam_api64.dll and log
// "[steam] id=<steamid64> loggedOn=<0|1> iface=<version>". Idempotent; returns
// false (and logs why) when Steam isn't available.
bool init();
bool ready();
SteamId selfId();

// 启动前设置手动放行名单与角色；主机允许空名单，加入方只登记主机。
void setPeers(const SteamId* ids, unsigned int count, bool hosting = false);
void setPeer(SteamId id);

// 主线程回调排队，由网络线程验证身份与人数，不直接修改隧道表。
void accept(SteamId id);

// Spike harness (KENSHICOOP_STEAM_PING=<steamid64>): ping/echo on P2P channel 1
// + periodic session-state logging, driven by tick() from the net thread. Works
// with either transport, so a UDP build can still prove Steam reachability.
void setPingPeer(SteamId id);

// Net-thread heartbeat: spike pings/echoes + session-state change logging.
// Cheap no-op when init() hasn't succeeded.
void tick();

// Install/remove the ENet socket hooks that tunnel channel 0 over Steam P2P.
// 'port' only fabricates the fake ENetAddress reported to ENet (the tunnel is
// addressless). Install BEFORE enet_host_create, remove after enet_host_destroy.
bool installEnetHooks(int port);
void removeEnetHooks();

// Close the P2P sessions (peer + ping peer).
void shutdown();

SteamId peerIdForHost(unsigned int host);
const char* selfName();

// 网络线程独占登记表；主线程会话回调仅经有锁队列提交身份。
void updateTunnelPeers(_ENetHost* host);
unsigned int takeJoinRejection();
unsigned int takeHostRejection();
void setJoinProbe(bool enabled);

} // namespace steamp2p
} // namespace coop

#endif // KENSHICOOP_STEAMP2P_H
