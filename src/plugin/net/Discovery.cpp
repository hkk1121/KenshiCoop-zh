// Discovery.cpp - tailnet/LAN game-browser runtime (see Discovery.h for the
// design). Plain winsock + CreateThread (VC10 - no std::thread); the netproto
// header owns every byte layout and the source-address policy, so this file
// is only sockets, threads, and the tailscale CLI capture.

#include "Discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop {
void logLine(const char* s);
void logErrLine(const char* s);
}

namespace coop {
namespace disc {
namespace {

const unsigned int MAX_FOUND    = 8;
const DWORD        SCAN_WAIT_MS = 1500; // collect replies this long after probing
const DWORD        CLI_WAIT_MS  = 3000; // tailscale CLI must answer within this

struct State {
    CRITICAL_SECTION cs;
    bool             csInit;

    // Responder.
    HANDLE   respThread;
    SOCKET   respSock;
    volatile LONG respStop;
    // Advert (guarded by cs).
    unsigned short advGamePort;
    unsigned char  advPlayers, advMax;
    char           advSave[DISC_NAME_MAX];

    // Prober.
    HANDLE       scanThread;
    volatile LONG scanRunning;
    unsigned short scanPort;
    // Results (guarded by cs).
    FoundHost    found[MAX_FOUND];
    unsigned int foundN;
    unsigned int generation;
    bool         sawTailscale;

    State() : csInit(false), respThread(0), respSock(INVALID_SOCKET), respStop(0),
              advGamePort(0), advPlayers(1), advMax(3),
              scanThread(0), scanRunning(0), scanPort(0),
              foundN(0), generation(0), sawTailscale(false) {
        advSave[0] = '\0';
    }
};
State g_st;

void ensureCs() {
    if (!g_st.csInit) { InitializeCriticalSection(&g_st.cs); g_st.csInit = true; }
}

bool wsaUp() {
    // Refcounted by Windows; paired implicitly at process exit. ENet does its
    // own WSAStartup, but discovery must work with the net thread down too.
    WSADATA wd;
    return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
}

void logf(const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// ---- Responder ---------------------------------------------------------------

DWORD WINAPI responderMain(LPVOID) {
    u8 in[512], out[128];
    while (!InterlockedCompareExchange(&g_st.respStop, 0, 0)) {
        sockaddr_in from; int fromLen = (int)sizeof(from);
        int n = recvfrom(g_st.respSock, (char*)in, (int)sizeof(in), 0,
                         (sockaddr*)&from, &fromLen);
        if (n <= 0) continue; // timeout (500 ms) or transient error -> re-check stop
        u32 proto = 0;
        if (!parseProbe(in, (unsigned int)n, &proto)) continue;
        u32 srcHost = ntohl(from.sin_addr.s_addr);
        if (!ipv4SourceAllowed(srcHost)) continue; // never advertise publicly

        char hostName[DISC_NAME_MAX]; DWORD hn = (DWORD)sizeof(hostName);
        if (!GetComputerNameA(hostName, &hn)) strcpy(hostName, "kenshi-host");

        unsigned short gamePort; unsigned char players, maxP;
        char save[DISC_NAME_MAX];
        EnterCriticalSection(&g_st.cs);
        gamePort = g_st.advGamePort; players = g_st.advPlayers; maxP = g_st.advMax;
        memcpy(save, g_st.advSave, sizeof(save));
        LeaveCriticalSection(&g_st.cs);

        unsigned int rn = encodeReply(out, gamePort, players, maxP, hostName, save);
        sendto(g_st.respSock, (const char*)out, (int)rn, 0, (sockaddr*)&from, fromLen);
        char ip[64];
        _snprintf(ip, sizeof(ip) - 1, "%u.%u.%u.%u",
                  (srcHost >> 24) & 255, (srcHost >> 16) & 255,
                  (srcHost >> 8) & 255, srcHost & 255);
        ip[sizeof(ip) - 1] = '\0';
        logf("[disc] reply -> %s (probe v%u; advert %u/%u save='%s' port=%u)",
             ip, proto, players, maxP, save, gamePort);
    }
    return 0;
}

// ---- Prober -------------------------------------------------------------------

// Run `tailscale status --json` and capture stdout. Tries the standard install
// path first, then PATH. Returns false (quietly) when the CLI is unavailable -
// the scan then covers loopback only and the panel says so.
bool tailscaleStatusJson(std::string& out) {
    const char* candidates[2] = {
        "\"C:\\Program Files\\Tailscale\\tailscale.exe\" status --json",
        "tailscale status --json"
    };
    for (int c = 0; c < 2; ++c) {
        SECURITY_ATTRIBUTES sa; ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        HANDLE rd = 0, wr = 0;
        if (!CreatePipe(&rd, &wr, &sa, 0)) continue;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si; ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr; si.hStdError = wr;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

        char cmd[256];
        strncpy(cmd, candidates[c], sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        BOOL ok = CreateProcessA(0, cmd, 0, 0, TRUE,
                                 CREATE_NO_WINDOW, 0, 0, &si, &pi);
        CloseHandle(wr); // ours closed; child holds the write end
        if (!ok) { CloseHandle(rd); continue; }

        // Read until EOF or deadline. The pipe read unblocks when the child
        // exits (its write end closes); the deadline covers a hung daemon.
        std::string acc;
        DWORD start = GetTickCount();
        char buf[4096]; DWORD got = 0;
        for (;;) {
            if (!ReadFile(rd, buf, sizeof(buf), &got, 0) || got == 0) break;
            acc.append(buf, buf + got);
            if (GetTickCount() - start > CLI_WAIT_MS) break;
            if (acc.size() > 4u * 1024u * 1024u) break; // runaway guard
        }
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, 500);
        DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec);
        if (ec == STILL_ACTIVE) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (!acc.empty()) { out = acc; return true; }
    }
    return false;
}

DWORD WINAPI scanMain(LPVOID) {
    unsigned short port = g_st.scanPort;

    // Candidates: loopback (the local rig / same-machine host) + tailnet peers.
    std::vector<std::string> cand;
    cand.push_back("127.0.0.1");
    bool sawTs = false;
    {
        std::string json;
        if (tailscaleStatusJson(json)) {
            sawTs = true;
            extractTailscaleIPv4(json, cand); // dedupes internally; keeps order
        } else {
            coop::logLine("[disc] tailscale CLI not found - scanning loopback only "
                          "(LAN/tailnet hosts can still be joined by address)");
        }
    }

    FoundHost results[MAX_FOUND];
    unsigned int nres = 0;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        DWORD tmo = 200;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

        u8 probe[16];
        unsigned int pn = encodeProbe(probe);
        for (unsigned int i = 0; i < cand.size(); ++i) {
            sockaddr_in to; ZeroMemory(&to, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            to.sin_addr.s_addr = inet_addr(cand[i].c_str());
            if (to.sin_addr.s_addr == INADDR_NONE) continue;
            sendto(s, (const char*)probe, (int)pn, 0, (sockaddr*)&to, sizeof(to));
        }

        DWORD deadline = GetTickCount() + SCAN_WAIT_MS;
        u8 in[512];
        while (GetTickCount() < deadline && nres < MAX_FOUND) {
            sockaddr_in from; int fromLen = (int)sizeof(from);
            int n = recvfrom(s, (char*)in, (int)sizeof(in), 0,
                             (sockaddr*)&from, &fromLen);
            if (n <= 0) continue;
            DiscReply r;
            if (!parseReply(in, (unsigned int)n, &r)) continue;
            char ip[64];
            u32 src = ntohl(from.sin_addr.s_addr);
            _snprintf(ip, sizeof(ip) - 1, "%u.%u.%u.%u",
                      (src >> 24) & 255, (src >> 16) & 255,
                      (src >> 8) & 255, src & 255);
            ip[sizeof(ip) - 1] = '\0';
            bool dup = false;
            for (unsigned int i = 0; i < nres; ++i)
                if (strcmp(results[i].ip, ip) == 0) { dup = true; break; }
            if (dup) continue;
            strncpy(results[nres].ip, ip, sizeof(results[nres].ip) - 1);
            results[nres].ip[sizeof(results[nres].ip) - 1] = '\0';
            results[nres].info = r;
            ++nres;
        }
        closesocket(s);
    }

    EnterCriticalSection(&g_st.cs);
    for (unsigned int i = 0; i < nres; ++i) g_st.found[i] = results[i];
    g_st.foundN = nres;
    g_st.sawTailscale = sawTs;
    ++g_st.generation;
    LeaveCriticalSection(&g_st.cs);

    // One summary line per scan; the harness autoscan gate greps this.
    {
        char first[128] = "";
        if (nres > 0) {
            _snprintf(first, sizeof(first) - 1, " first='%s'@%s:%u v%u %u/%u",
                      results[0].info.hostName, results[0].ip,
                      results[0].info.gamePort, results[0].info.protocol,
                      results[0].info.players, results[0].info.maxPlayers);
            first[sizeof(first) - 1] = '\0';
        }
        logf("[disc] scan done: candidates=%u found=%u tailscale=%d%s",
             (unsigned int)cand.size(), nres, sawTs ? 1 : 0, first);
    }

    InterlockedExchange(&g_st.scanRunning, 0);
    return 0;
}

} // namespace

// ---- Public API ----------------------------------------------------------------

bool responderStart(unsigned short discPort) {
    ensureCs();
    if (g_st.respThread) return true; // idempotent
    if (!wsaUp()) { coop::logErrLine("[disc] WSAStartup failed"); return false; }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { coop::logErrLine("[disc] responder socket failed"); return false; }
    sockaddr_in addr; ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(discPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        logf("[disc] responder bind FAILED on port %u (another host on this "
             "machine?) - discovery off, joins by address still work", discPort);
        closesocket(s);
        return false;
    }
    DWORD tmo = 500; // stop-flag poll cadence
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

    g_st.respSock = s;
    InterlockedExchange(&g_st.respStop, 0);
    g_st.respThread = CreateThread(0, 0, responderMain, 0, 0, 0);
    if (!g_st.respThread) {
        closesocket(s); g_st.respSock = INVALID_SOCKET;
        coop::logErrLine("[disc] responder thread failed");
        return false;
    }
    logf("[disc] responder up on udp %u (private-scope sources only)", discPort);
    return true;
}

void responderStop() {
    if (!g_st.respThread) return;
    InterlockedExchange(&g_st.respStop, 1);
    WaitForSingleObject(g_st.respThread, 2000);
    CloseHandle(g_st.respThread);
    g_st.respThread = 0;
    if (g_st.respSock != INVALID_SOCKET) { closesocket(g_st.respSock); g_st.respSock = INVALID_SOCKET; }
    coop::logLine("[disc] responder down");
}

bool responderRunning() { return g_st.respThread != 0; }

void responderAdvert(unsigned short gamePort, unsigned int players,
                     unsigned int maxPlayers, const char* saveName) {
    if (!g_st.respThread) return;
    ensureCs();
    EnterCriticalSection(&g_st.cs);
    g_st.advGamePort = gamePort;
    g_st.advPlayers  = (unsigned char)(players > 255 ? 255 : players);
    g_st.advMax      = (unsigned char)(maxPlayers > 255 ? 255 : maxPlayers);
    discCopyName(g_st.advSave, saveName);
    LeaveCriticalSection(&g_st.cs);
}

bool scanStart(unsigned short discPort) {
    ensureCs();
    if (InterlockedCompareExchange(&g_st.scanRunning, 1, 0) != 0) return false;
    if (!wsaUp()) {
        InterlockedExchange(&g_st.scanRunning, 0);
        coop::logErrLine("[disc] WSAStartup failed");
        return false;
    }
    if (g_st.scanThread) { CloseHandle(g_st.scanThread); g_st.scanThread = 0; }
    g_st.scanPort = discPort;
    logf("[disc] scan start (discovery port %u)", discPort);
    g_st.scanThread = CreateThread(0, 0, scanMain, 0, 0, 0);
    if (!g_st.scanThread) {
        InterlockedExchange(&g_st.scanRunning, 0);
        coop::logErrLine("[disc] scan thread failed");
        return false;
    }
    return true;
}

bool scanBusy() { return InterlockedCompareExchange(&g_st.scanRunning, 0, 0) != 0; }

unsigned int scanGeneration() {
    ensureCs();
    EnterCriticalSection(&g_st.cs);
    unsigned int g = g_st.generation;
    LeaveCriticalSection(&g_st.cs);
    return g;
}

unsigned int foundCount() {
    ensureCs();
    EnterCriticalSection(&g_st.cs);
    unsigned int n = g_st.foundN;
    LeaveCriticalSection(&g_st.cs);
    return n;
}

bool foundGet(unsigned int i, FoundHost* out) {
    if (!out) return false;
    ensureCs();
    EnterCriticalSection(&g_st.cs);
    bool ok = (i < g_st.foundN);
    if (ok) *out = g_st.found[i];
    LeaveCriticalSection(&g_st.cs);
    return ok;
}

bool lastScanSawTailscale() {
    ensureCs();
    EnterCriticalSection(&g_st.cs);
    bool v = g_st.sawTailscale;
    LeaveCriticalSection(&g_st.cs);
    return v;
}

void shutdown() {
    responderStop();
    if (g_st.scanThread) {
        WaitForSingleObject(g_st.scanThread, SCAN_WAIT_MS + CLI_WAIT_MS + 1000);
        CloseHandle(g_st.scanThread);
        g_st.scanThread = 0;
    }
}

} // namespace disc
} // namespace coop
