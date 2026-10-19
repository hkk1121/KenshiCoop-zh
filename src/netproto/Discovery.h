// Discovery.h - tailnet/LAN game-browser datagrams (probe -> reply) and the
// pure helpers behind them. NOT part of the ENet session protocol: discovery
// rides its OWN tiny UDP socket on a dedicated port (default 27815, distinct
// from the game port AND from the netsim proxy ports the harness parks on
// gamePort+1/+2) so a stray datagram can never confuse the game link.
//
// Motivation (Tailscale): a tailnet is a flat WireGuard mesh, so plain UDP
// joins already work by address - but it forwards NO broadcast/multicast, so
// LAN-beacon discovery cannot cross it. Discovery is therefore ACTIVE: the
// join enumerates tailnet peers from the local tailscale daemon (CLI JSON;
// extractor below), probes each peer's discovery port, and lists the hosts
// that answer. Loopback and RFC1918 sources are also allowed so the same
// probe works on a LAN and on the local 3-instance test rig.
//
// Header-only PODs + inline functions so prototest covers the wire format,
// the source-address gate, and the tailnet-IP extraction without linking
// winsock (the socket/thread runtime lives in src/plugin/net/Discovery.cpp).

#ifndef KENSHICOOP_NP_DISCOVERY_H
#define KENSHICOOP_NP_DISCOVERY_H

#include <cstring>
#include <string>
#include <vector>

#include "Wire.h" // u8/u16/u32, PROTOCOL_VERSION, MAX_PLAYERS

namespace coop {
namespace disc {

const u32 DISC_PROBE_MAGIC = 0x70444B43u; // "CKDp" on the wire (little-endian)
const u32 DISC_REPLY_MAGIC = 0x72444B43u; // "CKDr"
const unsigned int   DISC_NAME_MAX     = 32;    // hostName/saveName capacity (incl NUL)
const unsigned short DISC_DEFAULT_PORT = 27815;

#pragma pack(push, 1)
// Probe (join -> candidate): magic + the scanner's protocol version, so a
// host can log a mismatched scanner even though it still answers (the browser
// row shows the mismatch; the failed-connect mystery is avoided either way).
struct DiscProbe {
    u32 magic;    // DISC_PROBE_MAGIC
    u32 protocol; // scanner's PROTOCOL_VERSION
};
// Reply (host -> scanner): everything one browser row needs. Names are
// fixed-size and FORCED NUL-terminated by parseReply regardless of sender.
struct DiscReply {
    u32  magic;      // DISC_REPLY_MAGIC
    u32  protocol;   // host's PROTOCOL_VERSION
    u16  gamePort;   // ENet UDP port to join on
    u8   players;    // connected players INCLUDING the host
    u8   maxPlayers; // the host's MAX_PLAYERS policy cap
    char hostName[DISC_NAME_MAX]; // machine name (never a Steam ID)
    char saveName[DISC_NAME_MAX]; // save the host is running ("" = unknown)
};
#pragma pack(pop)

inline unsigned int encodeProbe(u8* buf) {
    DiscProbe p;
    p.magic = DISC_PROBE_MAGIC;
    p.protocol = PROTOCOL_VERSION;
    std::memcpy(buf, &p, sizeof(p));
    return (unsigned int)sizeof(p);
}

inline bool parseProbe(const u8* buf, unsigned int len, u32* protocolOut) {
    if (len < sizeof(DiscProbe)) return false; // trailing bytes tolerated
    DiscProbe p;
    std::memcpy(&p, buf, sizeof(p));
    if (p.magic != DISC_PROBE_MAGIC) return false;
    if (protocolOut) *protocolOut = p.protocol;
    return true;
}

// Copy a C string into a fixed field, always NUL-terminated, silently
// truncated (browser cosmetics - never a protocol concern).
inline void discCopyName(char* dst, const char* src) {
    if (!src) src = "";
    unsigned int i = 0;
    for (; i < DISC_NAME_MAX - 1 && src[i]; ++i) dst[i] = src[i];
    for (; i < DISC_NAME_MAX; ++i) dst[i] = '\0';
}

inline unsigned int encodeReply(u8* buf, u16 gamePort, u8 players, u8 maxPlayers,
                                const char* hostName, const char* saveName) {
    DiscReply r;
    r.magic = DISC_REPLY_MAGIC;
    r.protocol = PROTOCOL_VERSION;
    r.gamePort = gamePort;
    r.players = players;
    r.maxPlayers = maxPlayers;
    discCopyName(r.hostName, hostName);
    discCopyName(r.saveName, saveName);
    std::memcpy(buf, &r, sizeof(r));
    return (unsigned int)sizeof(r);
}

inline bool parseReply(const u8* buf, unsigned int len, DiscReply* out) {
    if (len < sizeof(DiscReply) || !out) return false;
    std::memcpy(out, buf, sizeof(DiscReply));
    if (out->magic != DISC_REPLY_MAGIC) return false;
    out->hostName[DISC_NAME_MAX - 1] = '\0'; // never trust the sender's NULs
    out->saveName[DISC_NAME_MAX - 1] = '\0';
    return true;
}

// Source-address gate: a host answers probes ONLY from private-scope sources,
// so it never advertises to the open internet. ip is IPv4 in HOST byte order
// (the runtime passes ntohl(sin_addr)). Allowed: loopback 127/8, RFC1918
// 10/8 + 172.16/12 + 192.168/16, link-local 169.254/16, and the Tailscale
// CGNAT range 100.64/10.
inline bool ipv4SourceAllowed(u32 ip) {
    if ((ip >> 24) == 127u) return true;                      // 127.0.0.0/8
    if ((ip >> 24) == 10u) return true;                       // 10.0.0.0/8
    if ((ip >> 20) == ((172u << 4) | 1u)) return true;        // 172.16.0.0/12
    if ((ip >> 16) == ((192u << 8) | 168u)) return true;      // 192.168.0.0/16
    if ((ip >> 16) == ((169u << 8) | 254u)) return true;      // 169.254.0.0/16
    if ((ip >> 22) == ((100u << 2) | 1u)) return true;        // 100.64.0.0/10
    return false;
}

// Extract the IPv4 tailnet addresses from `tailscale status --json` output:
// every "TailscaleIPs" array contributes its dotted-quad entries (self and
// peers alike - probing self is a harmless no-op unless we are also hosting).
// Deliberately a tolerant scan, not a JSON parser (same policy as
// coop_config.json): find each "TailscaleIPs" key, walk its [...] block,
// keep quoted strings that look like IPv4. Order preserved, duplicates
// dropped. IPv6 tailnet addresses are skipped (the game link is v4).
inline void extractTailscaleIPv4(const std::string& json,
                                 std::vector<std::string>& out) {
    const char* KEY = "\"TailscaleIPs\"";
    std::string::size_type at = 0;
    while ((at = json.find(KEY, at)) != std::string::npos) {
        at += std::strlen(KEY);
        std::string::size_type open = json.find('[', at);
        if (open == std::string::npos) break;
        std::string::size_type close = json.find(']', open);
        if (close == std::string::npos) break;
        std::string::size_type q = open;
        while (true) {
            q = json.find('"', q + 1);
            if (q == std::string::npos || q > close) break;
            std::string::size_type q2 = json.find('"', q + 1);
            if (q2 == std::string::npos || q2 > close) break;
            std::string s = json.substr(q + 1, q2 - q - 1);
            q = q2;
            // IPv4 shape check: digits and exactly three dots, 7..15 chars.
            if (s.size() < 7 || s.size() > 15) continue;
            unsigned int dots = 0; bool ok = true;
            for (std::string::size_type i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (c == '.') ++dots;
                else if (c < '0' || c > '9') { ok = false; break; }
            }
            if (!ok || dots != 3) continue;
            bool dup = false;
            for (unsigned int i = 0; i < out.size(); ++i)
                if (out[i] == s) { dup = true; break; }
            if (!dup) out.push_back(s);
        }
        at = close;
    }
}

} // namespace disc
} // namespace coop

#endif // KENSHICOOP_NP_DISCOVERY_H
