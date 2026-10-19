// Discovery.h - runtime for the tailnet/LAN game browser (netproto/Discovery.h
// carries the pure wire format). Two independent halves, each on its own tiny
// thread so neither the game tick nor the ENet loop ever blocks on them:
//
//  * RESPONDER (host side): a UDP socket on the discovery port answers probes
//    from private-scope sources (loopback/RFC1918/link-local/Tailscale CGNAT -
//    never the open internet) with {protocol, game port, players, host name,
//    save}. Started when a UDP host goes online; the advert is refreshed
//    cheaply from the main tick.
//
//  * PROBER (join side): enumerates candidate addresses - always loopback,
//    plus every tailnet peer read from `tailscale status --json` (the CLI is
//    optional; without it the scan is loopback/LAN-manual only) - fires one
//    probe at each, and collects replies for ~1.5 s into a result list the
//    panel renders. Active probing is the ONLY design that works on Tailscale:
//    a tailnet forwards no broadcast/multicast, so LAN beacons cannot cross it.
//
// All entry points are safe to call from the main thread; results are handed
// over under a critical section as fixed-size PODs.

#ifndef KENSHICOOP_NET_DISCOVERY_H
#define KENSHICOOP_NET_DISCOVERY_H

#include "../../netproto/Discovery.h"

namespace coop {
namespace disc {

// One browser row: where the reply came from + what it said.
struct FoundHost {
    char      ip[64];
    DiscReply info;
};

// ---- Responder (host) -------------------------------------------------------
// Start answering probes on discPort. Idempotent; returns false if the socket
// could not be created/bound (port already taken by another local host).
bool responderStart(unsigned short discPort);
void responderStop();
bool responderRunning();
// Refresh what the responder advertises (main tick, cheap; no-op when the
// responder is down). players INCLUDES the host itself.
void responderAdvert(unsigned short gamePort, unsigned int players,
                     unsigned int maxPlayers, const char* saveName);

// ---- Prober (join) ----------------------------------------------------------
// Kick an async scan of loopback + every tailnet peer. Returns false if a scan
// is already running. Results replace the previous list when the scan ENDS
// (generation bumps); the old list stays readable meanwhile.
bool scanStart(unsigned short discPort);
bool scanBusy();
// Bumped once per COMPLETED scan (0 = never scanned). Poll from the tick and
// re-read results when it changes.
unsigned int scanGeneration();
unsigned int foundCount();
bool foundGet(unsigned int i, FoundHost* out);
// True if the last completed scan saw the tailscale CLI (else loopback-only).
bool lastScanSawTailscale();

// Full teardown (plugin shutdown): stops both halves.
void shutdown();

} // namespace disc
} // namespace coop

#endif // KENSHICOOP_NET_DISCOVERY_H
