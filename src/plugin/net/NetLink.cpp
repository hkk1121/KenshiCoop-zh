#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetLink.h"
#include "../../netproto/CharacterCreation.h"
#include "SteamP2P.h"
#include "SteamRoom.h"
#include "../CoopLog.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace coop {

namespace {
const int        TICK_MS       = 50; // 20 Hz service/transmit cadence
// Traffic-class channels (protocol 44). ENet guarantees ordering + reliable
// retransmit PER channel, so head-of-line blocking is per channel too. The bulk
// coordinated save/load transfer (multi-MB, dozens of ~4 KB reliable fragments)
// used to share CH_RELIABLE with the small latency-sensitive game events (doors,
// money, faction, ...); during a transfer those events stalled behind megabytes
// of save data. Moving the whole save/load handshake onto its own CH_BULK keeps
// its internal ordering intact while letting events flow on CH_RELIABLE. The
// receive ladder dispatches purely by packet TYPE, so the channel a packet
// arrives on is irrelevant there - only the sender and the host/connect channel
// count change.
const enet_uint8 CH_RELIABLE   = 0;  // handshake + small reliable game events
const enet_uint8 CH_UNRELIABLE = 1;  // entity batches / stealth / cam (newest supersedes)
const enet_uint8 CH_BULK       = 2;  // coordinated save/load transfer (bulk reliable)
const int        CH_COUNT      = 3;  // channels negotiated at host-create / connect

// 断开原因码，经 enet_peer_disconnect(peer, data) 传给对端（对端 DISCONNECT 事件的 ev.data）。
// 主机在 HELLO 阶段发现版本不一致时用它告诉被拒的加入方原因（上游 PR #24）；0 = 正常断开
const enet_uint32 DISCONNECT_VERSION_MISMATCH = 1;

// 网络线程 -> 界面的连接错误通道存储。全局对象在 DLL 加载时构造（早于任何网络线程），
// 临界区必然先初始化（VC10 的函数内静态变量不是线程安全的）
struct NetUiErrChannel {
    CRITICAL_SECTION cs;
    char             buf[256];
    NetUiErrChannel() { InitializeCriticalSection(&cs); buf[0] = '\0'; }
    ~NetUiErrChannel() { DeleteCriticalSection(&cs); }
};
NetUiErrChannel g_netUiErr;

// Net-thread diagnostics. OutputDebugStringA is thread-safe, and CoopLog guards
// its FILE* with a lock, so both are safe to call off the main thread.
void netLog(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}
void netErr(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ERROR: ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logErrLine(buf);
}

// Monotonic ms clock for the batch send stamp (v35). QPC, not GetTickCount:
// the receiver reconstructs snapshot SPACING from consecutive stamps, and
// GetTickCount's ~15 ms granularity would re-introduce the very quantization
// the stamp exists to remove (the Replicator's nowMs rationale).
u32 monoMs() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return (u32)GetTickCount();
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u32)(((unsigned __int64)c.QuadPart * 1000ULL) /
                 (unsigned __int64)freq.QuadPart);
}

// The fixed-POD queueX methods are all the same MAIN-thread enqueue: take the
// publish lock, append one copied packet, release. This collapses that shared
// body so each queueX is a one-liner and the lock discipline lives in one place.
// (The variable-length queues - inv/world-item/census/save-file - keep custom
// bodies because they flatten a payload before the locked append.)
template<class T>
void pushLocked(CRITICAL_SECTION& cs, std::vector<T>& q, const T& v) {
    EnterCriticalSection(&cs);
    q.push_back(v);
    LeaveCriticalSection(&cs);
}
} // namespace

NetLink::NetLink()
    : isHost_(false), port_(0),
      enetHost_(0), serverPeer_(0), inbound_(0),
      claimRank_(1), playerId_(0),
      outOwner_(0), outStampMs_(0), haveOut_(false),
      thread_(0), running_(0), stopFlag_(0), myId_(0),
      sendEpoch_(0),
      steamPeer_(0),
      simDelayMs_(0), simJitterMs_(0), simLossPct_(0) {
    connectionSequence_ = 0;
    playerName_[0] = 0;
    InitializeCriticalSection(&outCs_);
}

NetLink::~NetLink() {
    stop();
    DeleteCriticalSection(&outCs_);
}

bool NetLink::startHost(int port, Inbound* inbound) {
    clearNetUiError(); // 新一次连接尝试，清掉上次的拒绝原因
    isHost_ = true; port_ = port; inbound_ = inbound; myId_ = 0;
    if (!playerId_) return false;
    return launchThread();
}

bool NetLink::startClient(const std::string& ip, int port, Inbound* inbound) {
    clearNetUiError(); // 新一次连接尝试，清掉上次的拒绝原因
    isHost_ = false; ip_ = ip; port_ = port; inbound_ = inbound; myId_ = 0;
    if (!playerId_) return false;
    return launchThread();
}

void NetLink::setClaimRank(u32 rank) {
    claimRank_ = rank;
}

bool NetLink::launchThread() {
    if (enet_initialize() != 0) { netErr("enet_initialize failed"); return false; }
    stopFlag_ = 0;
    InterlockedExchange(&running_, 1);
    thread_ = CreateThread(0, 0, &NetLink::threadEntry, this, 0, 0);
    if (thread_ == 0) { InterlockedExchange(&running_, 0); netErr("CreateThread failed"); enet_deinitialize(); return false; }
    return true;
}

void NetLink::stop() {
    if (thread_) {
        InterlockedExchange(&stopFlag_, 1);
        // The worker owns transport teardown (enet_host_destroy + the Steam hook
        // removal at the end of threadLoop). Deinitializing ENet or closing the
        // thread handle while the worker is still inside enet_host_service is a
        // use-after-free / double-free, so we MUST wait for it to fully exit
        // before tearing anything down. The loop services at TICK_MS (50 ms) and
        // checks stopFlag_ each pass, so a clean exit is prompt; if it somehow
        // stalls we keep waiting rather than pulling the rug out from under it.
        DWORD wr = WaitForSingleObject(thread_, 5000);
        if (wr != WAIT_OBJECT_0) {
            netErr("net worker still running after 5s; waiting for clean exit "
                   "before ENet teardown");
            WaitForSingleObject(thread_, INFINITE);
        }
        CloseHandle(thread_);
        thread_ = 0;
        enet_deinitialize();
    }
    EnterCriticalSection(&outCs_);
    outOwners_.clear(); outOwnerRequests_.clear(); outCharacter_.clear();
    LeaveCriticalSection(&outCs_);
}

void NetLink::setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count) {
    EnterCriticalSection(&outCs_);
    outOwner_ = ownerId;
    if (arr && count > 0) out_.assign(arr, arr + count);
    else                  out_.clear();
    // Capture-time stamp (v35). The net thread may re-send this same snapshot
    // next tick; the unchanged stamp lets the receiver dedupe the re-send
    // instead of recording a phantom zero-velocity segment.
    outStampMs_ = monoMs();
    haveOut_ = true;
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueEvent(const EventPacket& ev) { pushLocked(outCs_, outEvents_, ev); }

void NetLink::queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                               const InvItemEntry* items, unsigned int count, u8 flags) {
    OutInv oi;
    oi.ownerId = ownerId;
    oi.keyKind = keyKind;
    oi.flags   = flags;
    for (int k = 0; k < 5; ++k) oi.cKey[k] = cKey[k];
    // Clamping here is itself a truncation: mark it so the receiver stays additive-only
    // (the caller normally sets the flag, but never let a silent clamp delete items).
    if (count > INV_ITEMS_MAX) { count = INV_ITEMS_MAX; oi.flags |= INV_FLAG_TRUNCATED; }
    if (items && count > 0) oi.items.assign(items, items + count);
    pushLocked(outCs_, outInv_, oi);
}

void NetLink::queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count) {
    OutWorldItems ow;
    ow.ownerId = ownerId;
    if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
    if (items && count > 0) ow.items.assign(items, items + count);
    pushLocked(outCs_, outWorldItems_, ow);
}

void NetLink::queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count) {
    OutWorldRemove ow;
    ow.ownerId = ownerId;
    if (count > 255) count = 255; // u8 count on the wire
    if (netIds && count > 0) ow.netIds.assign(netIds, netIds + count);
    pushLocked(outCs_, outWorldRemove_, ow);
}

void NetLink::queueWorldClaim(u32 ownerId, u32 authorId, const u32* netIds,
                              unsigned int count) {
    OutWorldClaim ow;
    ow.ownerId = ownerId; ow.authorId = authorId;
    if (count > 255) count = 255; // u8 count on the wire
    if (netIds && count > 0) ow.netIds.assign(netIds, netIds + count);
    pushLocked(outCs_, outWorldClaim_, ow);
}

void NetLink::queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                             unsigned int count) {
    OutNpcCensus oc;
    oc.ownerId = ownerId;
    if (count > NPC_CENSUS_MAX) count = NPC_CENSUS_MAX;
    if (hands && count > 0) oc.hands.assign(hands, hands + count * 5);
    if (pos && count > 0) oc.pos.assign(pos, pos + count * 3);
    pushLocked(outCs_, outNpcCensus_, oc);
}

void NetLink::queueWorldDrop(const WorldDropPacket& pkt) { pushLocked(outCs_, outWorldDrops_, pkt); }

void NetLink::queueMedical(const MedicalPacket& pkt) { pushLocked(outCs_, outMedical_, pkt); }

void NetLink::queueTreatment(const TreatmentPacket& pkt) { pushLocked(outCs_, outTreatments_, pkt); }

void NetLink::queueCombatHit(const CombatHitPacket& pkt) { pushLocked(outCs_, outCombatHits_, pkt); }

void NetLink::queueSpeed(const SpeedPacket& pkt) { pushLocked(outCs_, outSpeed_, pkt); }

void NetLink::queueStats(const StatsPacket& pkt) { pushLocked(outCs_, outStats_, pkt); }

void NetLink::queueMoney(const MoneyPacket& pkt) { pushLocked(outCs_, outMoney_, pkt); }
void NetLink::queueMoneyDelta(const MoneyDeltaPacket& pkt) { pushLocked(outCs_, outMoneyDelta_, pkt); }

void NetLink::queueFaction(const FactionPacket& pkt) { pushLocked(outCs_, outFaction_, pkt); }

void NetLink::queueTime(const TimePacket& pkt) { pushLocked(outCs_, outTime_, pkt); }

void NetLink::queueDoor(const DoorPacket& pkt) { pushLocked(outCs_, outDoor_, pkt); }

void NetLink::queueProd(const ProdPacket& pkt) { pushLocked(outCs_, outProd_, pkt); }

void NetLink::queueResearch(const ResearchPacket& pkt) { pushLocked(outCs_, outResearch_, pkt); }

void NetLink::queueDeed(const DeedPacket& pkt) { pushLocked(outCs_, outDeed_, pkt); }

void NetLink::queueFixture(const FixturePacket& pkt) { pushLocked(outCs_, outFixture_, pkt); }

void NetLink::queueBuildPlace(const BuildPlacePacket& pkt) { pushLocked(outCs_, outBuildPlace_, pkt); }

void NetLink::queueBuildState(const BuildStatePacket& pkt) { pushLocked(outCs_, outBuildState_, pkt); }

void NetLink::queueBuildDoor(const BuildDoorPacket& pkt) { pushLocked(outCs_, outBuildDoor_, pkt); }

void NetLink::queueBuildRemove(const BuildRemovePacket& pkt) { pushLocked(outCs_, outBuildRemove_, pkt); }

void NetLink::queueStealth(const StealthPacket& pkt) { pushLocked(outCs_, outStealth_, pkt); }

void NetLink::queueCamHint(const CamHintPacket& pkt) { pushLocked(outCs_, outCamHint_, pkt); }

void NetLink::queueCellClaim(const CellClaimPacket& pkt) { pushLocked(outCs_, outCellClaim_, pkt); }

void NetLink::queueSpawnReq(const SpawnReqPacket& pkt) { pushLocked(outCs_, outSpawnReq_, pkt); }

void NetLink::queueSpawnInfo(const SpawnInfoPacket& pkt) { pushLocked(outCs_, outSpawnInfo_, pkt); }

void NetLink::queueWorldPickup(const WorldPickupPacket& pkt) { pushLocked(outCs_, outWorldPickups_, pkt); }

void NetLink::queueInvXfer(const InvXferPacket& pkt) { pushLocked(outCs_, outInvXfers_, pkt); }

void NetLink::queueInvXferAck(const InvXferAckPacket& pkt) { pushLocked(outCs_, outInvXferAcks_, pkt); }

void NetLink::queueSaveReq(const SaveReqPacket& pkt) { pushLocked(outCs_, outSaveReq_, pkt); }

void NetLink::queueSaveBegin(const SaveBeginPacket& pkt, u32 targetId) {
    OutSaveBegin ob;
    ob.targetId = targetId;
    ob.pkt = pkt;
    pushLocked(outCs_, outSaveBegin_, ob);
}

void NetLink::queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                            const unsigned char* data, unsigned int dataLen,
                            u32 targetId) {
    OutSaveFile of;
    of.targetId = targetId;
    of.hdr = hdr;
    of.tail.reserve(hdr.pathLen + dataLen);
    of.tail.assign(relPath, relPath + hdr.pathLen);
    if (data && dataLen > 0) of.tail.insert(of.tail.end(), data, data + dataLen);
    pushLocked(outCs_, outSaveFile_, of);
}

void NetLink::queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs,
                            unsigned int count, u32 targetId) {
    OutSaveDone od;
    od.targetId = targetId;
    od.hdr = hdr;
    if (crcs && count > 0) od.crcs.assign(crcs, crcs + count);
    pushLocked(outCs_, outSaveDone_, od);
}

void NetLink::queueSaveAck(const SaveAckPacket& pkt) { pushLocked(outCs_, outSaveAck_, pkt); }

void NetLink::queueLoadGo(const LoadGoPacket& pkt, u32 targetId) {
    OutLoadGo og;
    og.targetId = targetId;
    og.pkt = pkt;
    pushLocked(outCs_, outLoadGo_, og);
}

void NetLink::queueLoadReq(const LoadReqPacket& pkt) { pushLocked(outCs_, outLoadReq_, pkt); }

void NetLink::queueLoadNack(const LoadNackPacket& pkt) { pushLocked(outCs_, outLoadNack_, pkt); }

void NetLink::setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct) {
    simDelayMs_  = delayMs;
    simJitterMs_ = jitterMs;
    simLossPct_  = (lossPct > 100) ? 100 : lossPct;
}

void NetLink::setSteamTransport(unsigned long long peerSteamId) {
    steamPeer_ = peerSteamId;
}

// Deliver one received entity to the game thread, applying the WAN sim if enabled.
// Loopback has ~0 latency, so without this a received "down"/move lands the same
// frame it was sent - exactly the regime we want to stop relying on. With it, the
// entity is parked until base +/- jitter has elapsed (and dropped lossPct% of the
// time), so the join must coast on interpolation/local enforcement between arrivals.
// MAIN thread: advance the session epoch so post-reset batches supersede any
// still-in-flight batch from the prior session, and drop the pending owned-
// entity snapshot so the net thread does not re-broadcast a stale (old-world)
// snapshot stamped with the NEW epoch (which the peer would then accept as
// fresh). The main thread republishes a real snapshot within a tick or two.
void NetLink::bumpSessionEpoch() {
    InterlockedIncrement(&sendEpoch_);
    EnterCriticalSection(&outCs_);
    out_.clear();
    haveOut_ = false;
    LeaveCriticalSection(&outCs_);
}

// NET thread: monotonic per-owner epoch gate. Latest-wins on the unreliable
// motion stream, so "accept if >= newest seen" both admits a fresh session
// (higher epoch) and rejects a delayed prior-session batch (lower epoch).
bool NetLink::acceptEpoch(u32 ownerId, u32 epoch) {
    std::map<u32, u32>::iterator it = epochSeen_.find(ownerId);
    if (it == epochSeen_.end()) { epochSeen_[ownerId] = epoch; return true; }
    if (epoch < it->second) return false;
    it->second = epoch;
    return true;
}

// NET thread (protocol 49): the join-authored, owner-tagged packet classes the
// host re-sends to the OTHER joins. Everything here already carries the
// sender's ownerId and is deduped/gated per owner on the receiver, so a
// relayed copy is indistinguishable from a direct one. Host-directed intents
// (speed/save/load/spawn requests and time probes) and the handshake never
// relay. Camera hints are the exception: attention-gated reconciliation needs
// every client to evaluate the same camera-anchor set, so the host forwards a
// join's hint to the other join. Census is also symmetric: every player's
// rows only claim the cells that player owns, so all joins need the merged
// existence set. Host-directed channels (prod/research/time/stealth/spawn-info)
// never arrive at the host through this list. PKT_ENTITY_BATCH relays too, but
// inside its epoch-accepted branch in the receive ladder. The symmetric
// 阵营、门、建筑、房产和设施显式转发，应用基线会压制再次发布。
// 生产和研究在主机应用意图后由主机流回显，不转发加入方原包。
bool NetLink::isRelayedType(u8 type) {
    switch (type) {
        case PKT_EVENT:
        case PKT_INV_SNAPSHOT:
        case PKT_WORLD_ITEM:
        case PKT_WORLD_ITEM_REMOVE:
        case PKT_WORLD_ITEM_CLAIM:
        case PKT_WORLD_DROP:
        case PKT_WORLD_PICKUP:
        case PKT_INV_XFER:
        case PKT_INV_XFER_ACK:
        case PKT_MEDICAL:
        case PKT_TREATMENT:
        case PKT_STATS:
        case PKT_MONEY:
        case PKT_NPC_CENSUS:
        case PKT_CAM_HINT:
        case PKT_CELL_CLAIM:
        case PKT_FACTION:
        case PKT_DOOR:
        case PKT_BUILD_PLACE:
        case PKT_BUILD_STATE:
        case PKT_BUILD_DOOR:
        case PKT_BUILD_REMOVE:
        case PKT_DEED:
        case PKT_FIXTURE:
            return true;
        default:
            return false;
    }
}

// NET thread (protocol 49): re-send a received packet's bytes to every
// connected join except the sender, preserving the channel it arrived on and
// its delivery class. Mirrors enet_host_broadcast's refcount idiom: one
// packet, N sends, destroy only if nobody queued it.
void NetLink::relayToOthers(ENetPeer* from, enet_uint8 channelId, const ENetPacket* pkt) {
    if (peersById_.size() < 2) return; // no third edge to serve
    enet_uint32 flags =
        pkt->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED);
    ENetPacket* out = enet_packet_create(pkt->data, pkt->dataLength, flags);
    if (!out) return;
    for (std::map<u32, ENetPeer*>::iterator it = peersById_.begin();
         it != peersById_.end(); ++it) {
        if (it->second == from) continue;
        if (it->second->state != ENET_PEER_STATE_CONNECTED) continue;
        enet_peer_send(it->second, channelId, out);
    }
    if (out->referenceCount == 0) enet_packet_destroy(out);
}

// NET thread (protocol 49): route one outbound packet by target. On the host,
// OWNER_ID_ALL broadcasts and a specific id goes to that join alone (the
// targeted connect-push save); a join always has exactly one counterparty -
// the host link. Takes ownership of 'pkt' either way.
void NetLink::routeOut(u32 targetId, enet_uint8 channel, ENetPacket* pkt) {
    if (isHost_) {
        if (targetId == OWNER_ID_ALL) {
            enet_host_broadcast(enetHost_, channel, pkt);
            return;
        }
        std::map<u32, ENetPeer*>::iterator it = peersById_.find(targetId);
        if (it != peersById_.end() &&
            it->second->state == ENET_PEER_STATE_CONNECTED &&
            enet_peer_send(it->second, channel, pkt) == 0) {
            return;
        }
        if (pkt->referenceCount == 0) enet_packet_destroy(pkt); // target gone
    } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
        enet_peer_send(serverPeer_, channel, pkt);
    } else {
        enet_packet_destroy(pkt);
    }
}

// NET thread (protocol 49): one PKT_PEER_STATUS roster row to one join.
void NetLink::sendStatusTo(ENetPeer* to, u32 ownerId, u32 ownRank, bool present) {
    if (!to || to->state != ENET_PEER_STATE_CONNECTED) return;
    std::map<u32, PeerStatusPacket>::const_iterator row = statusById_.find(ownerId);
    if (row == statusById_.end()) return;
    PeerStatusPacket ps = row->second;
    ps.type    = (u8)PKT_PEER_STATUS;
    ps.present = present ? 1 : 0;
    ps.ownerId = ownerId;
    ps.ownRank = ownRank;
    ENetPacket* out = enet_packet_create(&ps, sizeof(ps), ENET_PACKET_FLAG_RELIABLE);
    if (out) enet_peer_send(to, CH_RELIABLE, out);
}

// NET thread (protocol 49): announce a roster edge to every join except
// 'exceptId' (the subject itself - its own presence is implicit in its link).
void NetLink::broadcastStatusExcept(u32 exceptId, u32 ownerId, u32 ownRank, bool present) {
    for (std::map<u32, ENetPeer*>::iterator it = peersById_.begin();
         it != peersById_.end(); ++it) {
        if (it->first == exceptId) continue;
        sendStatusTo(it->second, ownerId, ownRank, present);
    }
}

// NET thread (protocol 49): all host bookkeeping for a join id that is gone -
// normal disconnect or a crash-ghost eviction when its squad slot is
// reclaimed. Erases the registry rows and the owner's epoch entries, tells
// the game thread (which tears down that owner's driven state), and, when
// 'announce' is set, tells the remaining joins so they do the same.
void NetLink::dropPeer(u32 id, bool announce) {
    u32 rank = 0;
    std::map<u32, u32>::iterator rit = rankById_.find(id);
    if (rit != rankById_.end()) { rank = rit->second; rankById_.erase(rit); }
    peersById_.erase(id);
    epochSeen_.erase(id);
    if (announce) broadcastStatusExcept(id, id, rank, false);
    std::map<u32, PeerStatusPacket>::iterator row = statusById_.find(id);
    if (row != statusById_.end()) {
        row->second.present = 0;
        if (inbound_) inbound_->pushPeerStatus(row->second);
        statusById_.erase(row);
    }
    if (inbound_) inbound_->pushLeave(id);
    char b[64];
    _snprintf(b, sizeof(b) - 1, "peer disconnected id=%u rank=%u", (unsigned)id,
              (unsigned)rank);
    b[sizeof(b) - 1] = '\0';
    netLog(b);
}

void NetLink::deliverEntity(u32 ownerId, u32 sendMs, const EntityState& e) {
    if (simDelayMs_ == 0 && simJitterMs_ == 0 && simLossPct_ == 0) {
        if (inbound_) inbound_->pushEntity(ownerId, sendMs, e);
        return;
    }
    if (simLossPct_ > 0 && (unsigned)(rand() % 100) < simLossPct_) return; // dropped
    int jitter = 0;
    if (simJitterMs_ > 0) jitter = (rand() % (int)(2 * simJitterMs_ + 1)) - (int)simJitterMs_;
    int delay = (int)simDelayMs_ + jitter;
    if (delay < 0) delay = 0;
    Delayed d;
    d.releaseTick = GetTickCount() + (DWORD)delay;
    d.ownerId     = ownerId;
    d.sendMs      = sendMs;
    d.e           = e;
    delayed_.push_back(d);
}

// Release every held entity whose simulated arrival time has passed. Jitter can put
// release times out of order; we scan the whole queue (small N) and keep the rest.
// "Newest supersedes" on the receiver makes any reordering harmless (and realistic).
void NetLink::flushDelayed() {
    if (delayed_.empty()) return;
    DWORD now = GetTickCount();
    std::deque<Delayed> keep;
    for (std::deque<Delayed>::iterator it = delayed_.begin(); it != delayed_.end(); ++it) {
        if ((long)(now - it->releaseTick) >= 0) { if (inbound_) inbound_->pushEntity(it->ownerId, it->sendMs, it->e); }
        else                                     { keep.push_back(*it); }
    }
    delayed_.swap(keep);
}

DWORD WINAPI NetLink::threadEntry(LPVOID self) {
    reinterpret_cast<NetLink*>(self)->threadLoop();
    return 0;
}

void NetLink::threadLoop() {
    InterlockedExchange(&running_, 1);

    // Steam P2P transport: redirect ENet's socket layer through the Steam tunnel
    // BEFORE the host is created (the fake socket is handed out at create time).
    // The tunnel is addressless; ENet still needs an ENetAddress for its peer
    // routing, so both sides use the fabricated "1.0.0.1:port".
    const bool steam = (steamPeer_ != 0);
    if (steam) {
        if (steamp2p::installEnetHooks(port_)) {
            netLog("transport=steam (ENet tunnelled over Steam P2P)");
        } else {
            coop::logErrLine("[room] Steam tunnel unavailable; refusing UDP fallback");
            setNetUiError("Steam is unavailable - start Steam and reconnect");
            InterlockedExchange(&running_, 0); return;
        }
    }

    if (isHost_) {
        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = (enet_uint16)port_;
        enetHost_ = enet_host_create(&addr, 8 /*peers*/, CH_COUNT /*channels*/, 0, 0);
        if (!enetHost_) { netErr("host create failed"); InterlockedExchange(&running_, 0); return; }
        netLog("hosting");
    } else {
        enetHost_ = enet_host_create(0, 1, CH_COUNT, 0, 0);
        if (!enetHost_) { netErr("client create failed"); InterlockedExchange(&running_, 0); return; }
        ENetAddress addr;
        if (steam) enet_address_set_host_ip(&addr, "1.0.0.1");
        else       enet_address_set_host(&addr, ip_.c_str());
        addr.port = (enet_uint16)port_;
        serverPeer_ = enet_host_connect(enetHost_, &addr, CH_COUNT, 0);
        if (!serverPeer_) netErr("connect failed");
        else              netLog("connecting");
    }

    // Steam's unreliable P2P packets cap at 1200 bytes; clamp the MTU so every
    // ENet datagram (including fragments of large reliable packets) fits one
    // P2P packet. Set before any peer negotiates its own MTU from ours.
    if (steam && enetHost_ && enetHost_->mtu > 1200) {
        enetHost_->mtu = 1200;
        if (serverPeer_) serverPeer_->mtu = 1200;
    }

    DWORD lastConnectAttempt = GetTickCount();
    DWORD handshakeSince = GetTickCount();
    bool welcomed = false, rejected = false;
    std::map<ENetPeer*, DWORD> pendingHello;

    // Fresh session: no admitted joins yet (protocol 49). The registries are
    // members only so the net-thread helpers can reach them; they never
    // survive a thread restart.
    peersById_.clear();
    rankById_.clear();
    statusById_.clear();
    if (isHost_) {
        PeerStatusPacket self;
        memset(&self, 0, sizeof(self));
        self.type = (u8)PKT_PEER_STATUS; self.present = 1;
        self.connectionId = ++connectionSequence_;
        self.playerId = playerId_; memcpy(self.name, playerName_, sizeof(self.name));
        statusById_[0] = self;
        if (inbound_) inbound_->pushPeerStatus(self);
    }

    // Wall-clock time-sync state (client only). The join pings every ~2 s; each
    // pong yields an (rtt, offset) sample; the minimum-RTT sample wins (NTP
    // filter). CLOCKSYNC is logged every ~5 s so the oracles can align this
    // log's timestamps into the host clock frame.
    DWORD         lastTimePing  = 0;
    DWORD         lastClockLog  = 0;
    u32           pingNonce     = 1;
    unsigned long bestRttMs     = 0xFFFFFFFFul;
    long          bestOffsetMs  = 0;
    unsigned int  syncSamples   = 0;
    const long    HALF_DAY_MS   = 12l * 3600l * 1000l;

    while (!stopFlag_) {
        // Steam heartbeat: spike ping/echo + session-state change logging.
        // Cheap no-op when Steam isn't initialised (pure-UDP sessions).
        if (steam) steamp2p::updateTunnelPeers(enetHost_);
        steamp2p::tick();
        unsigned int roomReason = steam && !isHost_ ? steamp2p::takeJoinRejection() : 0;
        if (steam && !isHost_ && !welcomed && !rejected) {
            unsigned int reason = roomReason;
            if (reason) {
                steamp2p::setJoinProbe(false);
                rejected = true;
                if (reason == room::REJECT_FULL) setNetUiError("Room is full - wait for a free player slot");
                else setNetUiError("The host rejected you: not a Steam friend or on the allowlist");
                coop::logLine("[room] host rejected the Steam connection");
                if (serverPeer_) { enet_peer_reset(serverPeer_); serverPeer_ = 0; }
            }
        }
        if (!isHost_ && !welcomed && !rejected && GetTickCount() - handshakeSince >= 10000) {
            setNetUiError("Host is not responding: check they are online, you are Steam friends, and both use the same co-op build");
        }
        for (std::map<ENetPeer*, DWORD>::iterator pi = pendingHello.begin(); pi != pendingHello.end();) {
            if (GetTickCount() - pi->second >= 10000) {
                enet_peer_disconnect(pi->first, 0); pendingHello.erase(pi++);
            } else ++pi;
        }

        // Client reconnect: if we have no live connection, retry every 2 s so
        // dropping/relaunching the host re-establishes.
        if (!isHost_ && !rejected) {
            bool disconnected =
                (serverPeer_ == 0) ||
                (serverPeer_->state == ENET_PEER_STATE_DISCONNECTED) ||
                (serverPeer_->state == ENET_PEER_STATE_ZOMBIE);
            DWORD now = GetTickCount();
            if (disconnected && (now - lastConnectAttempt) >= 2000) {
                lastConnectAttempt = now;
                if (serverPeer_) { enet_peer_reset(serverPeer_); serverPeer_ = 0; }
                ENetAddress addr;
                if (steam) enet_address_set_host_ip(&addr, "1.0.0.1");
                else       enet_address_set_host(&addr, ip_.c_str());
                addr.port = (enet_uint16)port_;
                serverPeer_ = enet_host_connect(enetHost_, &addr, CH_COUNT, 0);
                if (steam && serverPeer_) serverPeer_->mtu = 1200;
                netLog(serverPeer_ ? "reconnecting" : "reconnect failed");
            }
        }

        ENetEvent ev;
        while (enet_host_service(enetHost_, &ev, TICK_MS) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    if (isHost_) {
                        // Wait for the client's HELLO before assigning an id, so
                        // a version mismatch is rejected before we admit it.
                        // Per-owner epoch state is NOT cleared here (protocol
                        // 49): other joins' sessions continue across this
                        // arrival, and ids are never reused, so the newcomer
                        // starts with no entry anyway.
                        pendingHello[ev.peer] = GetTickCount();
                        netLog("peer connecting (awaiting HELLO)");
                    } else {
                        // A fresh host link restarts OUR whole session: every
                        // owner behind that link (host + relayed joins) begins
                        // a new epoch sequence, so forget all prior epochs -
                        // the new session's first batch is never mistaken for
                        // stale (v44).
                        epochSeen_.clear();
                        // Introduce ourselves with our protocol version + the
                        // squad slot we claim (protocol 49).
                        HelloPacket h;
                        h.type = (u8)PKT_HELLO; h.version = PROTOCOL_VERSION;
                        h.ownRank = claimRank_; h.playerId = playerId_;
                        h.nameLen = (u8)strlen(playerName_);
                        unsigned char hello[sizeof(h) + 63];
                        memcpy(hello, &h, sizeof(h));
                        memcpy(hello + sizeof(h), playerName_, h.nameLen);
                        ENetPacket* out = enet_packet_create(hello, sizeof(h) + h.nameLen, ENET_PACKET_FLAG_RELIABLE);
                        enet_peer_send(ev.peer, CH_RELIABLE, out);
                        netLog("connected to host; sent HELLO");
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const u8 type = packetType(ev.packet->data, (unsigned)ev.packet->dataLength);
                    if (isHost_ && type != PKT_HELLO && ev.peer->data == 0) {
                        enet_packet_destroy(ev.packet); break;
                    }
                    if (isHost_ && type == PKT_HELLO) {
                        if (ev.packet->dataLength >= 3 && ev.packet->dataLength < sizeof(HelloPacket)) {
                            enet_peer_disconnect(ev.peer, DISCONNECT_VERSION_MISMATCH);
                        }
                        HelloPacket h;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h)) {
                            if (h.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: peer v%u, ours v%u; rejecting",
                                          (unsigned)h.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                // 主机界面也显示拒绝原因，并经断开数据告知被拒的加入方（它收不到 WELCOME）
                                setNetUiError("Rejected a joiner: version mismatch - update both to the same KenshiCoop build");
                                enet_peer_disconnect(ev.peer, DISCONNECT_VERSION_MISMATCH);
                            } else if (!h.playerId || h.playerId == playerId_ || h.nameLen > 63 ||
                                       ev.packet->dataLength != sizeof(h) + h.nameLen ||
                                       (steam && steamp2p::peerIdForHost(ev.peer->address.host) != h.playerId)) {
                                netErr("[owners] rejected invalid player identity");
                                enet_peer_disconnect(ev.peer, room::REJECT_IDENTITY);
                            } else if (ev.peer->data != 0) {
                                netErr("[owners] repeated HELLO ignored");
                            } else {
                                // Admit policy (protocol 49). The sync model is a
                                // host-relayed star: join-authored state reaches the
                                // other joins through the host relay, so more than one
                                // join is supported - up to MAX_PLAYERS, each on its
                                // own squad-tab rank.
                                //
                                // 槽位冲突直接拒绝；只有相同稳定身份允许替换旧连接。
                                u32 rank = h.ownRank;
                                u32 ghostId = 0;
                                bool taken = (rank == 0 || rank >= MAX_PLAYERS);
                                for (std::map<u32, u32>::iterator it = rankById_.begin();
                                     it != rankById_.end(); ++it) {
                                    std::map<u32, PeerStatusPacket>::const_iterator identity = statusById_.find(it->first);
                                    bool same = identity != statusById_.end() && identity->second.playerId == h.playerId;
                                    if (same) ghostId = it->first;
                                    else if (it->second == rank) { taken = true; break; }
                                }
                                if (!taken && ghostId != 0) {
                                    // 同一个玩家重连，先撤销旧连接的授权与会话状态。
                                    std::map<u32, ENetPeer*>::iterator pit =
                                        peersById_.find(ghostId);
                                    ENetPeer* ghost = (pit != peersById_.end()) ? pit->second : 0;
                                    char eb[96];
                                    _snprintf(eb, sizeof(eb) - 1,
                                              "squad slot %u reclaimed; evicting old id=%u",
                                              (unsigned)rank, (unsigned)ghostId);
                                    eb[sizeof(eb) - 1] = '\0';
                                    netLog(eb);
                                    dropPeer(ghostId, true);
                                    if (ghost) { ghost->data = 0; enet_peer_reset(ghost); }
                                    taken = false;
                                }
                                if (taken) {
                                    char rb[96];
                                    _snprintf(rb, sizeof(rb) - 1,
                                              "squad slot %u is not joinable; rejecting",
                                              (unsigned)rank);
                                    rb[sizeof(rb) - 1] = '\0';
                                    netErr(rb);
                                    setNetUiError("Rejected a joiner: squad slot is occupied");
                                    enet_peer_disconnect(ev.peer, room::REJECT_SLOT);
                                } else if (peersById_.size() + 1 >= MAX_PLAYERS) {
                                    char fb[96];
                                    _snprintf(fb, sizeof(fb) - 1,
                                              "session full (%u players); rejecting",
                                              (unsigned)MAX_PLAYERS);
                                    fb[sizeof(fb) - 1] = '\0';
                                    netErr(fb);
                                    setNetUiError("Rejected a joiner: room full");
                                    enet_peer_disconnect(ev.peer, room::REJECT_FULL);
                                } else {
                                    // 在已登记对端中取最小空闲编号：多对端仍不会重复，
                                    // 断线重连也不会把 id 一路推高。
                                    u32 id = 1;
                                    while (peersById_.find(id) != peersById_.end()) ++id;
                                    pendingHello.erase(ev.peer);
                                    ev.peer->data = (void*)(size_t)id;
                                    peersById_[id] = ev.peer;
                                    rankById_[id]  = rank;
                                    PeerStatusPacket row;
                                    memset(&row, 0, sizeof(row));
                                    row.type = (u8)PKT_PEER_STATUS; row.present = 1;
                                    row.ownerId = id; row.ownRank = rank; row.playerId = h.playerId;
                                    row.connectionId = ++connectionSequence_;
                                    memcpy(row.name, ev.packet->data + sizeof(h), h.nameLen);
                                    statusById_[id] = row;
                                    if (inbound_) inbound_->pushPeerStatus(row);
                                    WelcomePacket w;
                                    w.type = (u8)PKT_WELCOME; w.version = PROTOCOL_VERSION; w.playerId = id;
                                    ENetPacket* out =
                                        enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                                    enet_peer_send(ev.peer, CH_RELIABLE, out);
                                    // Roster exchange (protocol 49): the newcomer
                                    // learns every join already here; they learn the
                                    // newcomer. The receiving game threads reset that
                                    // owner's state and re-burst their own reliable
                                    // channels, which the relay carries across.
                                    for (std::map<u32, PeerStatusPacket>::iterator it = statusById_.begin();
                                         it != statusById_.end(); ++it) {
                                        sendStatusTo(ev.peer, it->first, it->second.ownRank, true);
                                    }
                                    broadcastStatusExcept(id, id, rank, true);
                                    char b[96];
                                    _snprintf(b, sizeof(b) - 1,
                                              "peer connected id=%u rank=%u (proto v%u)",
                                              (unsigned)id, (unsigned)rank,
                                              (unsigned)PROTOCOL_VERSION);
                                    b[sizeof(b) - 1] = '\0';
                                    netLog(b);
                                    if (inbound_) inbound_->pushConnect(id);
                                }
                            }
                        }
                    } else if (!isHost_ && type == PKT_WELCOME) {
                        WelcomePacket w;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &w)) {
                            if (w.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: host v%u, ours v%u",
                                          (unsigned)w.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                setNetUiError("Version mismatch with host - update both to the same KenshiCoop build");
                            } else {
                                welcomed = true; if (steam) steamp2p::setJoinProbe(false); clearNetUiError();
                                InterlockedExchange(&myId_, (LONG)w.playerId);
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "peer connected id=%u (proto v%u) - received WELCOME",
                                          (unsigned)w.playerId, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                                if (inbound_) inbound_->pushConnect(0); // host id = 0
                            }
                        }
                    } else if (type == PKT_CHARACTER_REQUEST) {
                        CharacterRequestHeader h;
                        u32 owner = (u32)(size_t)ev.peer->data;
                        std::map<u32, ENetPeer*>::const_iterator admitted = peersById_.find(owner);
                        std::map<u32, PeerStatusPacket>::const_iterator identity = statusById_.find(owner);
                        if (isHost_ && inbound_ && admitted != peersById_.end() && admitted->second == ev.peer &&
                            identity != statusById_.end() && ev.channelID == CH_RELIABLE &&
                            readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h) &&
                            h.ownerId == owner && h.playerId == identity->second.playerId && h.connectionId == identity->second.connectionId &&
                            h.worldId && h.requestId && h.bytes && h.bytes <= CHARACTER_DRAFT_MAX &&
                            ev.packet->dataLength == sizeof(h) + h.bytes)
                            inbound_->pushCharacterRequest(h, ev.packet->data + sizeof(h));
                        else netErr("[chargen] rejected request direction, connection or framing");
                    } else if (type == PKT_CHARACTER_REPLY) {
                        CharacterReplyPacket p;
                        if (!isHost_ && inbound_ && ev.peer == serverPeer_ && ev.channelID == CH_RELIABLE &&
                            ev.packet->dataLength == sizeof(p) && readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &p) &&
                            p.targetOwnerId == localId() && p.playerId == playerId_ &&
                            p.result >= CHARACTER_CREATED && p.result <= CHARACTER_SAVED)
                            inbound_->pushCharacterReply(p);
                    } else if (type == PKT_CHARACTER_APPEARANCE) {
                        CharacterAppearanceHeader h;
                        if (!isHost_ && inbound_ && ev.peer == serverPeer_ && ev.channelID == CH_RELIABLE &&
                            readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h) &&
                            h.worldId && h.bytes && h.bytes <= CHARACTER_DRAFT_MAX &&
                            ev.packet->dataLength == sizeof(h) + h.bytes)
                            inbound_->pushCharacterAppearance(h, ev.packet->data + sizeof(h));
                    } else if (type == PKT_OWNERS) {
                        OwnersHeader h;
                        if (!isHost_ && inbound_ &&
                            readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h) &&
                            h.worldId && h.count <= OWNERS_MAX &&
                            ev.packet->dataLength == sizeof(h) + h.count * sizeof(OwnerRow)) {
                            inbound_->pushOwners(h, (const OwnerRow*)(ev.packet->data + sizeof(h)));
                        }
                    } else if (type == PKT_OWNER_REQUEST) {
                        OwnerRequestPacket p;
                        if (isHost_ && inbound_ &&
                            readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &p) &&
                            ev.packet->dataLength == sizeof(p) && p.ownerId == (u32)(size_t)ev.peer->data)
                            inbound_->pushOwnerRequest(p);
                    } else if (type == PKT_PEER_STATUS) {
                        // Peer roster edge (protocol 49, host -> joins): another
                        // join arrived or left. Forget that owner's epoch entries
                        // (its session, if any, ended or is brand new) and hand
                        // the edge to the game thread, which clears or seeds that
                        // owner's replication state and - on an arrival - re-
                        // bursts our own reliable channels so the newcomer
                        // receives them through the host relay.
                        PeerStatusPacket ps;
                        if (!isHost_ &&
                            readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ps)
                            && inbound_) {
                            if (!ps.playerId || ps.ownerId >= MAX_PLAYERS) {
                                enet_packet_destroy(ev.packet); break;
                            }
                            ps.name[63] = 0;
                            inbound_->pushPeerStatus(ps);
                            epochSeen_.erase(ps.ownerId);
                            char b[96];
                            _snprintf(b, sizeof(b) - 1,
                                      "peer roster: id=%u rank=%u %s",
                                      (unsigned)ps.ownerId, (unsigned)ps.ownRank,
                                      ps.present ? "joined" : "left");
                            b[sizeof(b) - 1] = '\0';
                            netLog(b);
                            if (ps.ownerId != 0 && ps.ownerId != (u32)myId_) {
                                if (ps.present) inbound_->pushConnect(ps.ownerId);
                                else            inbound_->pushLeave(ps.ownerId);
                            }
                        }
                    } else if (type == PKT_ENTITY_BATCH) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(EntityBatchHeader) && inbound_) {
                            EntityBatchHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need =
                                sizeof(EntityBatchHeader) + (unsigned)hdr.count * sizeof(EntityState);
                            // Drop batches from a superseded session (protocol 44)
                            // before they reach the WAN-sim queue / interp buffers.
                            if (len >= need && acceptEpoch(hdr.ownerId, hdr.epoch)) {
                                const enet_uint8* p = ev.packet->data + sizeof(EntityBatchHeader);
                                for (unsigned i = 0; i < hdr.count; ++i) {
                                    EntityState e;
                                    std::memcpy(&e, p + i * sizeof(EntityState), sizeof(e));
                                    deliverEntity(hdr.ownerId, hdr.sendMs, e);
                                }
                                // Host relay (protocol 49): a join's motion stream
                                // must also reach the OTHER joins. Gated on our
                                // epoch accept so a superseded session's batch is
                                // never propagated.
                                if (isHost_)
                                    relayToOthers(ev.peer, ev.channelID, ev.packet);
                            }
                        }
                    } else if (type == PKT_EVENT) {
                        // Reliable transition. Delivered immediately (NOT through the
                        // WAN-sim delay buffer): the sim models unreliable-batch loss,
                        // while the whole point of the reliable channel is that these
                        // survive that loss - so we honour their guaranteed delivery.
                        EventPacket evp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &evp)
                            && inbound_) {
                            inbound_->pushEvent(evp.ownerId, evp);
                        }
                    } else if (type == PKT_INV_SNAPSHOT) {
                        // Reliable container-contents snapshot (Phase 4a). Like
                        // PKT_EVENT, delivered immediately (not through the WAN-sim
                        // delay buffer): it rides the reliable channel precisely so a
                        // content change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(InvSnapshotHeader) && inbound_) {
                            InvSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(InvSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(InvItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(InvSnapshotHeader);
                                u32 cKey[5] = { hdr.cType, hdr.cContainer,
                                                hdr.cContainerSerial, hdr.cIndex, hdr.cSerial };
                                const InvItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const InvItemEntry*>(p) : 0;
                                inbound_->pushInv(hdr.ownerId, hdr.keyKind, cKey,
                                                  items, hdr.count, hdr.flags);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM) {
                        // Reliable world-item snapshot (Phase W1). Delivered immediately
                        // (not through the WAN-sim delay buffer), like the inventory
                        // snapshot - it rides the reliable channel so a ground-item
                        // change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemSnapshotHeader) && inbound_) {
                            WorldItemSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(WorldItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemSnapshotHeader);
                                const WorldItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const WorldItemEntry*>(p) : 0;
                                inbound_->pushWorldItems(hdr.ownerId, items, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_REMOVE) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemRemoveHeader) && inbound_) {
                            WorldItemRemoveHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemRemoveHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemRemoveHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                inbound_->pushWorldRemove(hdr.ownerId, netIds, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_CLAIM) {
                        // A peer consumed the proxies it held for these netIds
                        // (protocol 47), so the AUTHOR must destroy its real
                        // ground objects - the pickup mirror W1 lacked.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemClaimHeader) && inbound_) {
                            WorldItemClaimHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemClaimHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemClaimHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                inbound_->pushWorldClaim(hdr.ownerId, hdr.authorId,
                                                         netIds, hdr.count);
                            }
                        }
                    } else if (type == PKT_NPC_CENSUS) {
                        // Reliable wide-radius NPC existence census (protocol
                        // 36; v38 rows carry positions too). Latest-wins on
                        // the game thread; delivered whole.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(NpcCensusHeader) && inbound_) {
                            NpcCensusHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(NpcCensusHeader)
                                          + (unsigned)hdr.count * 5 * sizeof(u32)
                                          + (unsigned)hdr.count * 3 * sizeof(float);
                            if (len >= need && hdr.count <= NPC_CENSUS_MAX) {
                                const enet_uint8* p = ev.packet->data + sizeof(NpcCensusHeader);
                                const u32* hands =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                const float* pos = (hdr.count > 0)
                                    ? reinterpret_cast<const float*>(
                                          p + (unsigned)hdr.count * 5 * sizeof(u32))
                                    : 0;
                                inbound_->pushNpcCensus(hdr.ownerId, hands, pos, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_DROP) {
                        // Reliable conservation drop intent (Phase W2). Delivered immediately
                        // (not via the WAN-sim buffer) like the other reliable packets.
                        WorldDropPacket wdp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wdp)
                            && inbound_) {
                            inbound_->pushWorldDrop(wdp.ownerId, wdp);
                        }
                    } else if (type == PKT_WORLD_PICKUP) {
                        // Reliable conservation pickup intent (Phase W3), mirror of the drop.
                        WorldPickupPacket wpp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wpp)
                            && inbound_) {
                            inbound_->pushWorldPickup(wpp.ownerId, wpp);
                        }
                    } else if (type == PKT_INV_XFER) {
                        // Reliable cross-owner transfer intent (protocol 37).
                        InvXferPacket ixp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ixp)
                            && inbound_) {
                            inbound_->pushInvXfer(ixp.ownerId, ixp);
                        }
                    } else if (type == PKT_INV_XFER_ACK) {
                        // Reliable transfer verdict (protocol 50). Must be as
                        // reliable as the intent it answers: a lost ack leaves
                        // the author back on the wall-clock guess it replaces.
                        InvXferAckPacket iap;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &iap)
                            && inbound_) {
                            inbound_->pushInvXferAck(iap.ownerId, iap);
                        }
                    } else if (type == PKT_MEDICAL) {
                        // Reliable owner-authoritative vitals snapshot (phase 2).
                        // Delivered immediately like the other reliable packets.
                        MedicalPacket mp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &mp)
                            && inbound_) {
                            inbound_->pushMedical(mp.ownerId, mp);
                        }
                    } else if (type == PKT_TREATMENT) {
                        // Reliable treatment delta (first aid on a driven copy,
                        // forwarded to the owner).
                        TreatmentPacket tp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &tp)
                            && inbound_) {
                            inbound_->pushTreatment(tp.ownerId, tp);
                        }
                    } else if (type == PKT_COMBAT_HIT) {
                        // Reliable join-dealt damage report (join -> host): the
                        // host wounds the authoritative world NPC.
                        CombatHitPacket chp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &chp)
                            && inbound_) {
                            inbound_->pushCombatHit(chp.ownerId, chp);
                        }
                    } else if (type == PKT_SPEED_REQ || type == PKT_SPEED_SET) {
                        // Reliable game-speed request/set (consensus speed sync).
                        // Delivered immediately like the other reliable packets.
                        SpeedPacket sp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sp)
                            && inbound_) {
                            inbound_->pushSpeed(sp.ownerId, sp);
                        }
                    } else if (type == PKT_STATS) {
                        // Reliable owner-authoritative character-stats snapshot
                        // (protocol 17). Delivered immediately like the others.
                        StatsPacket stp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &stp)
                            && inbound_) {
                            inbound_->pushStats(stp.ownerId, stp);
                        }
                    } else if (type == PKT_MONEY) {
                        // Reliable host-authoritative money-pool total
                        // (protocol 52). Delivered immediately like the others.
                        MoneyPacket mo;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &mo)
                            && inbound_) {
                            inbound_->pushMoney(mo.ownerId, mo);
                        }
                    } else if (type == PKT_MONEY_DELTA) {
                        // Reliable money-pool delta (protocol 52, join -> host).
                        // Ordered delivery is what makes the fold exactly-once.
                        MoneyDeltaPacket md;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &md)
                            && inbound_) {
                            inbound_->pushMoneyDelta(md.ownerId, md);
                        }
                    } else if (type == PKT_FACTION) {
                        // Reliable player-faction relation row (protocol 24):
                        // host stream or join intent, disambiguated on apply.
                        FactionPacket fa;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &fa)
                            && inbound_) {
                            inbound_->pushFaction(fa.ownerId, fa);
                        }
                    } else if (type == PKT_TIME) {
                        // Reliable host-authoritative game-clock sample
                        // (protocol 25). Delivered immediately like the others.
                        TimePacket ti;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ti)
                            && inbound_) {
                            inbound_->pushTime(ti.ownerId, ti);
                        }
                    } else if (type == PKT_DOOR) {
                        // Reliable baked-door state row (protocol 26):
                        // symmetric change-gated, disambiguated on apply.
                        DoorPacket dp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &dp)
                            && inbound_) {
                            inbound_->pushDoor(dp.ownerId, dp);
                        }
                    } else if (type == PKT_PROD) {
                        // Reliable host-authoritative machine state row
                        // (protocol 33), applied via the engine levers.
                        ProdPacket pp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &pp)
                            && inbound_ && pp.ownerId == (isHost_ ? (u32)(size_t)ev.peer->data : 0u)) {
                            inbound_->pushProd(pp.ownerId, pp);
                        }
                    } else if (type == PKT_RESEARCH) {
                        // Reliable host-authoritative known-research row
                        // (protocol 38), applied via Research::startResearch.
                        ResearchPacket rp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &rp)
                            && inbound_ && rp.ownerId == (isHost_ ? (u32)(size_t)ev.peer->data : 0u)) {
                            inbound_->pushResearch(rp.ownerId, rp);
                        }
                    } else if (type == PKT_DEED) {
                        // Reliable property-deed ownership row (protocol 54):
                        // symmetric, applied as a pure state write.
                        DeedPacket dep;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &dep)
                            && inbound_) {
                            inbound_->pushDeed(dep.ownerId, dep);
                        }
                    } else if (type == PKT_FIXTURE) {
                        // Reliable runtime-fixture identity row (protocol 55):
                        // symmetric, pairs the sender's hand with our own copy.
                        FixturePacket fxp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &fxp)
                            && inbound_) {
                            inbound_->pushFixture(fxp.ownerId, fxp);
                        }
                    } else if (type == PKT_BUILD_PLACE) {
                        // Reliable placed-building announcement (protocol 27):
                        // describe/mint edge, keyed by the placer's hand.
                        BuildPlacePacket bp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bp)
                            && inbound_) {
                            inbound_->pushBuildPlace(bp.ownerId, bp);
                        }
                    } else if (type == PKT_BUILD_STATE) {
                        // Reliable construction-progress row (protocol 27),
                        // placer-authoritative, translation-map applied.
                        BuildStatePacket bs;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bs)
                            && inbound_) {
                            inbound_->pushBuildState(bs.ownerId, bs);
                        }
                    } else if (type == PKT_BUILD_DOOR) {
                        // Reliable placed-building door row (protocol 28),
                        // symmetric on the translated (bkey, index) identity.
                        BuildDoorPacket bd;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &bd)
                            && inbound_) {
                            inbound_->pushBuildDoor(bd.ownerId, bd);
                        }
                    } else if (type == PKT_BUILD_REMOVE) {
                        // Reliable placer-authoritative building removal
                        // (protocol 28).
                        BuildRemovePacket br;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &br)
                            && inbound_) {
                            inbound_->pushBuildRemove(br.ownerId, br);
                        }
                    } else if (type == PKT_STEALTH) {
                        // Unreliable stealth detection-map snapshot (protocol 20).
                        // Delivered immediately; the game thread keeps latest-wins
                        // per subject.
                        StealthPacket slp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &slp)
                            && inbound_) {
                            inbound_->pushStealth(slp.ownerId, slp);
                        }
                    } else if (type == PKT_SPAWN_REQ) {
                        // Reliable runtime-spawn query (protocol 21, join -> host).
                        SpawnReqPacket srp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &srp)
                            && inbound_) {
                            inbound_->pushSpawnReq(srp.ownerId, srp);
                        }
                    } else if (type == PKT_SPAWN_INFO) {
                        // Reliable runtime-spawn description (protocol 21, host -> join).
                        SpawnInfoPacket sip;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sip)
                            && inbound_) {
                            inbound_->pushSpawnInfo(sip.ownerId, sip);
                        }
                    } else if (type == PKT_SAVE_REQ) {
                        // Reliable join save request (protocol 31, join -> host).
                        SaveReqPacket sq;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sq)
                            && inbound_) {
                            inbound_->pushSaveReq(sq.ownerId, sq);
                        }
                    } else if (type == PKT_SAVE_BEGIN) {
                        // Reliable save-transfer announce (protocol 31, host -> join).
                        SaveBeginPacket sb;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sb)
                            && inbound_) {
                            inbound_->pushSaveBegin(sb.ownerId, sb);
                        }
                    } else if (type == PKT_SAVE_FILE) {
                        // Reliable save-file chunk (protocol 31, host -> join):
                        // [SaveFileHeader][path[pathLen]][payload[dataLen]].
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(SaveFileHeader) && inbound_) {
                            SaveFileHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(SaveFileHeader)
                                          + (unsigned)hdr.pathLen + (unsigned)hdr.dataLen;
                            if (len >= need && hdr.pathLen > 0 &&
                                hdr.pathLen <= SAVE_PATH_MAX &&
                                hdr.dataLen <= SAVE_CHUNK_MAX) {
                                const enet_uint8* p = ev.packet->data + sizeof(SaveFileHeader);
                                inbound_->pushSaveFile(hdr.ownerId, hdr,
                                                       (const char*)p,
                                                       (const u8*)p + hdr.pathLen);
                            }
                        }
                    } else if (type == PKT_SAVE_DONE) {
                        // Reliable save-transfer CRC table (protocol 31, host -> join):
                        // [SaveDoneHeader][u32 crc * fileCount].
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(SaveDoneHeader) && inbound_) {
                            SaveDoneHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(SaveDoneHeader)
                                          + (unsigned)hdr.fileCount * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(SaveDoneHeader);
                                inbound_->pushSaveDone(hdr.ownerId, hdr,
                                                       (hdr.fileCount > 0)
                                                           ? (const u32*)p : 0);
                            }
                        }
                    } else if (type == PKT_SAVE_ACK) {
                        // Reliable commit acknowledgement (protocol 31, join -> host).
                        SaveAckPacket sa;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &sa)
                            && inbound_) {
                            inbound_->pushSaveAck(sa.ownerId, sa);
                        }
                    } else if (type == PKT_LOAD_GO) {
                        // Reliable coordinated-load order (protocol 32, host -> join).
                        LoadGoPacket lg;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &lg)
                            && inbound_) {
                            inbound_->pushLoadGo(lg.ownerId, lg);
                        }
                    } else if (type == PKT_LOAD_REQ) {
                        // Reliable join load request (protocol 32, join -> host).
                        LoadReqPacket lr;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &lr)
                            && inbound_) {
                            inbound_->pushLoadReq(lr.ownerId, lr);
                        }
                    } else if (type == PKT_LOAD_NACK) {
                        // Reliable copy-missing/diverged answer (protocol 32, join -> host).
                        LoadNackPacket ln;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ln)
                            && inbound_) {
                            inbound_->pushLoadNack(ln.ownerId, ln);
                        }
                    } else if (type == PKT_CAM_HINT) {
                        // Camera hint (protocol 43): latest-wins interest
                        // anchor. Accepted in BOTH directions - the attention
                        // gate needs each side to know where the peer is
                        // looking, not just the host.
                        CamHintPacket chp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &chp)
                            && inbound_) {
                            inbound_->pushCamHint(chp.ownerId, chp);
                        }
                    } else if (type == PKT_CELL_CLAIM) {
                        // Cell claim (protocol 49): presence-based authorship.
                        // Both directions - each side claims the cells its own
                        // tabs are standing in.
                        CellClaimPacket ccp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &ccp)
                            && inbound_) {
                            inbound_->pushCellClaim(ccp.ownerId, ccp);
                        }
                    } else if (type == PKT_TIME_PING) {
                        // Wall-clock sync probe: echo immediately (host side). Answered
                        // inside the service loop so the response delay stays minimal
                        // (the join's rtt/2 assumption depends on it).
                        TimePingPacket tp;
                        if (isHost_ && readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &tp)) {
                            TimePongPacket po;
                            po.type = (u8)PKT_TIME_PONG;
                            po.nonce = tp.nonce;
                            po.echoWallMs = tp.senderWallMs;
                            po.responderWallMs = (u32)coop::wallClockMs();
                            ENetPacket* out = enet_packet_create(&po, sizeof(po), 0 /*unreliable*/);
                            enet_peer_send(ev.peer, CH_UNRELIABLE, out);
                        }
                    } else if (type == PKT_TIME_PONG) {
                        // Wall-clock sync echo (join side): derive one (rtt, offset)
                        // sample; keep the minimum-RTT one (least queueing noise).
                        TimePongPacket po;
                        if (!isHost_ && readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &po)) {
                            unsigned long t1 = coop::wallClockMs();
                            unsigned long t0 = (unsigned long)po.echoWallMs;
                            if (t1 >= t0) { // skip the (rare) sample spanning local midnight
                                unsigned long rtt = t1 - t0;
                                long offset = (long)po.responderWallMs + (long)(rtt / 2ul) - (long)t1;
                                // Normalize a cross-midnight host/join wrap into [-12h, +12h).
                                while (offset >= HALF_DAY_MS)  offset -= 2l * HALF_DAY_MS;
                                while (offset < -HALF_DAY_MS)  offset += 2l * HALF_DAY_MS;
                                ++syncSamples;
                                if (rtt <= bestRttMs) { bestRttMs = rtt; bestOffsetMs = offset; }
                            }
                        }
                    }
                    // Host relay (protocol 49): forward join-authored,
                    // owner-tagged state to the other joins - the star's third
                    // edge. Most rows are reliable; camera hints intentionally
                    // retain their unreliable/latest-wins delivery. Runs
                    // after local delivery so the relayed copy and our own
                    // apply see the same order. (Entity batches relay in their
                    // epoch-gated branch above, not here.)
                    if (isHost_ && isRelayedType(type))
                        relayToOthers(ev.peer, ev.channelID, ev.packet);
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    pendingHello.erase(ev.peer);
                    if (isHost_) {
                        // One join left; the others' sessions continue. dropPeer
                        // erases its registry rows + epoch entries (per owner,
                        // v44 generalized), announces the departure to the
                        // remaining joins, and queues the game-thread leave.
                        u32 id = (u32)(size_t)ev.peer->data;
                        ev.peer->data = 0;
                        if (id != 0) dropPeer(id, true);
                        else netLog("unadmitted peer disconnected");
                    } else {
                        // Our host link died: the whole remote session ends for
                        // us - every owner's epoch sequence (host + relayed
                        // joins) restarts on reconnect (v44).
                        epochSeen_.clear();
                        if (welcomed) handshakeSince = GetTickCount();
                        welcomed = false;
                        InterlockedExchange(&myId_, 0);
                        serverPeer_ = 0;
                        if (inbound_) inbound_->pushLeave(OWNER_ID_ALL);
                        // 主机因版本不一致拒绝时会在断开数据里带上原因
                        if (ev.data == DISCONNECT_VERSION_MISMATCH) {
                            netErr("host rejected us: protocol/version mismatch");
                            setNetUiError("Version mismatch with host - update both to the same KenshiCoop build");
                        }
                        if (ev.data >= room::REJECT_VERSION && ev.data <= room::REJECT_IDENTITY) rejected = true;
                        if (ev.data == room::REJECT_SLOT) setNetUiError("Squad slot is occupied - choose another slot and reconnect");
                        else if (ev.data == room::REJECT_FULL) setNetUiError("Room is full - wait for a free player slot");
                        else if (ev.data == room::REJECT_FRIEND) setNetUiError("The host rejected you: not a Steam friend or on the allowlist");
                        else if (ev.data == room::REJECT_IDENTITY) setNetUiError("The host rejected an invalid player identity");
                        if (steam) steamp2p::setJoinProbe(!rejected);
                        netLog("disconnected from host");
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Release any WAN-sim-delayed inbound entities whose arrival time has come.
        // No-op (and cheap) when the sim is disabled / nothing is pending.
        flushDelayed();

        // Wall-clock time sync (join side): ping every ~2 s on the UNRELIABLE
        // channel (a retransmitted probe would carry a stale t0), and log the
        // best CLOCKSYNC estimate every ~5 s for the oracles' clock alignment.
        if (!isHost_) {
            DWORD nowTick = GetTickCount();
            if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED
                && (lastTimePing == 0 || nowTick - lastTimePing >= 2000)) {
                lastTimePing = nowTick;
                TimePingPacket tp;
                tp.type = (u8)PKT_TIME_PING;
                tp.nonce = pingNonce++;
                tp.senderWallMs = (u32)coop::wallClockMs();
                ENetPacket* out = enet_packet_create(&tp, sizeof(tp), 0 /*unreliable*/);
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            }
            if (syncSamples > 0 && (lastClockLog == 0 || nowTick - lastClockLog >= 5000)) {
                lastClockLog = nowTick;
                char b[96];
                _snprintf(b, sizeof(b) - 1, "CLOCKSYNC offset=%ld rtt=%lu n=%u",
                          bestOffsetMs, bestRttMs, syncSamples);
                b[sizeof(b) - 1] = '\0';
                netLog(b);
            }
        }

        // Drain + send any queued reliable events on CH_RELIABLE. ENet guarantees
        // delivery + ordering on that channel, so these survive the unreliable-batch
        // loss the WAN sim injects (the reliability proof the death oracle checks).
        std::vector<OutCharacter> characterPackets;
        EnterCriticalSection(&outCs_); characterPackets.swap(outCharacter_); LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < characterPackets.size(); ++i) {
            const OutCharacter& p = characterPackets[i];
            if (p.epoch != (u32)sendEpoch_ || p.data.empty()) continue;
            ENetPacket* out = enet_packet_create(&p.data[0], p.data.size(), ENET_PACKET_FLAG_RELIABLE);
            if (out) routeOut(p.targetId, CH_RELIABLE, out);
        }
        std::vector<InboundOwners> owners;
        std::vector<OwnerRequestPacket> requests;
        EnterCriticalSection(&outCs_);
        owners.swap(outOwners_); requests.swap(outOwnerRequests_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < owners.size(); ++i) {
            const InboundOwners& row = owners[i];
            ENetPacket* out = enet_packet_create(0, sizeof(OwnersHeader) + row.rows.size() * sizeof(OwnerRow), ENET_PACKET_FLAG_RELIABLE);
            memcpy(out->data, &row.header, sizeof(OwnersHeader));
            if (!row.rows.empty()) memcpy(out->data + sizeof(OwnersHeader), &row.rows[0], row.rows.size() * sizeof(OwnerRow));
            routeOut(OWNER_ID_ALL, CH_RELIABLE, out);
        }
        for (size_t i = 0; i < requests.size(); ++i) {
            ENetPacket* out = enet_packet_create(&requests[i], sizeof(OwnerRequestPacket), ENET_PACKET_FLAG_RELIABLE);
            routeOut(OWNER_ID_ALL, CH_RELIABLE, out);
        }
        std::vector<EventPacket> events;
        EnterCriticalSection(&outCs_);
        events.swap(outEvents_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < events.size(); ++i) {
            ENetPacket* out = enet_packet_create(&events[i], sizeof(EventPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued conservation DROP intents on CH_RELIABLE (Phase W2). A
        // fixed-size POD per drop, like an event; reliable so a drop is never lost.
        //
        // Ordered BEFORE the inventory snapshots below: both ride CH_RELIABLE, which ENet
        // delivers in SEND order, so draining drops first means a drop intent can never be
        // overtaken on the wire by the bag snapshot that reflects it. The peer's apply phase
        // already runs drops before the reconcile within a tick, so this is belt-and-braces
        // - but it removes an inversion that made the same-tick case confusing to reason
        // about (and to read in a packet capture).
        std::vector<WorldDropPacket> drops;
        EnterCriticalSection(&outCs_);
        drops.swap(outWorldDrops_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < drops.size(); ++i) {
            ENetPacket* out = enet_packet_create(&drops[i], sizeof(WorldDropPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued conservation PICKUP intents on CH_RELIABLE (Phase W3).
        std::vector<WorldPickupPacket> pickups;
        EnterCriticalSection(&outCs_);
        pickups.swap(outWorldPickups_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < pickups.size(); ++i) {
            ENetPacket* out = enet_packet_create(&pickups[i], sizeof(WorldPickupPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued container-contents snapshots on CH_RELIABLE. Each
        // is [InvSnapshotHeader][InvItemEntry*count]; only enqueued on content-change,
        // so this channel stays quiet. Reliable so the change survives WAN loss.
        std::vector<OutInv> invs;
        EnterCriticalSection(&outCs_);
        invs.swap(outInv_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < invs.size(); ++i) {
            unsigned count = (unsigned)invs[i].items.size();
            u8 iflags = invs[i].flags;
            if (count > INV_ITEMS_MAX) { count = INV_ITEMS_MAX; iflags |= INV_FLAG_TRUNCATED; }
            unsigned bytes = sizeof(InvSnapshotHeader) + count * sizeof(InvItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            InvSnapshotHeader hdr;
            hdr.type             = (u8)PKT_INV_SNAPSHOT;
            hdr.ownerId          = invs[i].ownerId;
            hdr.keyKind          = invs[i].keyKind;
            hdr.flags            = iflags;
            hdr.cType            = invs[i].cKey[0];
            hdr.cContainer       = invs[i].cKey[1];
            hdr.cContainerSerial = invs[i].cKey[2];
            hdr.cIndex           = invs[i].cKey[3];
            hdr.cSerial          = invs[i].cKey[4];
            hdr.count            = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &invs[i].items[0],
                            count * sizeof(InvItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued world-item snapshots on CH_RELIABLE (Phase W1). Each
        // is [WorldItemSnapshotHeader][WorldItemEntry*count]; only enqueued for new/
        // changed ground items, so a settled world produces no traffic. Host-authored.
        std::vector<OutWorldItems> wis;
        std::vector<OutWorldRemove> wrs;
        std::vector<OutWorldClaim> wcs;
        EnterCriticalSection(&outCs_);
        wis.swap(outWorldItems_);
        wrs.swap(outWorldRemove_);
        wcs.swap(outWorldClaim_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < wis.size(); ++i) {
            unsigned count = (unsigned)wis[i].items.size();
            if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
            unsigned bytes = sizeof(WorldItemSnapshotHeader) + count * sizeof(WorldItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemSnapshotHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM;
            hdr.ownerId = wis[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wis[i].items[0],
                            count * sizeof(WorldItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Drain + send any queued world-item culls on CH_RELIABLE (Phase W1).
        for (size_t i = 0; i < wrs.size(); ++i) {
            unsigned count = (unsigned)wrs[i].netIds.size();
            if (count > 255) count = 255;
            unsigned bytes = sizeof(WorldItemRemoveHeader) + count * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemRemoveHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM_REMOVE;
            hdr.ownerId = wrs[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wrs[i].netIds[0], count * sizeof(u32));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Drain + send any queued world-item CLAIMS on CH_RELIABLE (protocol 47). A claim
        // travels the opposite way to a cull: the peer that consumed a proxy tells the
        // AUTHOR to destroy its real ground copy.
        for (size_t i = 0; i < wcs.size(); ++i) {
            unsigned count = (unsigned)wcs[i].netIds.size();
            if (count > 255) count = 255;
            unsigned bytes = sizeof(WorldItemClaimHeader) + count * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemClaimHeader hdr;
            hdr.type     = (u8)PKT_WORLD_ITEM_CLAIM;
            hdr.ownerId  = wcs[i].ownerId;
            hdr.authorId = wcs[i].authorId;
            hdr.count    = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wcs[i].netIds[0], count * sizeof(u32));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued NPC existence censuses on CH_RELIABLE
        // (protocol 36, host -> join, 1 Hz). ENet fragments the large list.
        std::vector<OutNpcCensus> censuses;
        EnterCriticalSection(&outCs_);
        censuses.swap(outNpcCensus_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < censuses.size(); ++i) {
            unsigned count = (unsigned)(censuses[i].hands.size() / 5);
            if (count > NPC_CENSUS_MAX) count = NPC_CENSUS_MAX;
            // v38 layout: hands block then positions block. A queue call that
            // somehow lacked positions still sends a well-formed packet
            // (zeroed positions), never a short one.
            unsigned bytes = sizeof(NpcCensusHeader) + count * 5 * sizeof(u32)
                           + count * 3 * sizeof(float);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            NpcCensusHeader hdr;
            hdr.type    = (u8)PKT_NPC_CENSUS;
            hdr.ownerId = censuses[i].ownerId;
            hdr.count   = (u16)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0) {
                std::memcpy(out->data + sizeof(hdr), &censuses[i].hands[0],
                            count * 5 * sizeof(u32));
                enet_uint8* pp = out->data + sizeof(hdr) + count * 5 * sizeof(u32);
                std::memset(pp, 0, count * 3 * sizeof(float));
                if (censuses[i].pos.size() >= count * 3)
                    std::memcpy(pp, &censuses[i].pos[0], count * 3 * sizeof(float));
            }
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued cross-owner TRANSFER intents on CH_RELIABLE
        // (protocol 37). Fixed-size PODs like the drop/pickup intents.
        std::vector<InvXferPacket> xfers;
        EnterCriticalSection(&outCs_);
        xfers.swap(outInvXfers_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xfers.size(); ++i) {
            ENetPacket* out = enet_packet_create(&xfers[i], sizeof(InvXferPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Transfer VERDICTS (protocol 50), same channel as the intents.
        std::vector<InvXferAckPacket> xferAcks;
        EnterCriticalSection(&outCs_);
        xferAcks.swap(outInvXferAcks_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < xferAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&xferAcks[i], sizeof(InvXferAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued medical snapshots + treatment deltas on
        // CH_RELIABLE (phase 2). Fixed-size PODs like events; change-gated by the
        // Replicator so steady state is silent.
        std::vector<MedicalPacket>   meds;
        std::vector<TreatmentPacket> treats;
        std::vector<CombatHitPacket> combatHits;
        EnterCriticalSection(&outCs_);
        meds.swap(outMedical_);
        treats.swap(outTreatments_);
        combatHits.swap(outCombatHits_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < meds.size(); ++i) {
            ENetPacket* out = enet_packet_create(&meds[i], sizeof(MedicalPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < treats.size(); ++i) {
            ENetPacket* out = enet_packet_create(&treats[i], sizeof(TreatmentPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < combatHits.size(); ++i) {
            ENetPacket* out = enet_packet_create(&combatHits[i], sizeof(CombatHitPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued game-speed packets on CH_RELIABLE (consensus
        // speed sync). Change-gated by the Replicator; a lost SET would leave
        // the engines running at different rates, so reliable is mandatory.
        std::vector<SpeedPacket> speeds;
        EnterCriticalSection(&outCs_);
        speeds.swap(outSpeed_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < speeds.size(); ++i) {
            ENetPacket* out = enet_packet_create(&speeds[i], sizeof(SpeedPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued character-stats snapshots on CH_RELIABLE
        // (protocol 17). Fixed-size PODs; change-gated by the Replicator so
        // steady state is silent (stats creep slowly).
        std::vector<StatsPacket> statPkts;
        EnterCriticalSection(&outCs_);
        statPkts.swap(outStats_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < statPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&statPkts[i], sizeof(StatsPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued money-pool totals on CH_RELIABLE (protocol
        // 52). Fixed-size PODs; change-gated by the Replicator so a settled
        // economy is silent. A lost total would diverge cats until the safety
        // resend, so reliable is the right channel.
        std::vector<MoneyPacket> moneyPkts;
        EnterCriticalSection(&outCs_);
        moneyPkts.swap(outMoney_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < moneyPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&moneyPkts[i], sizeof(MoneyPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued money-pool deltas on CH_RELIABLE (protocol
        // 52, join -> host). These are CONSERVATION intents, not snapshots: a
        // dropped or reordered delta would silently mint or burn cats, which is
        // exactly what the reliable ordered channel prevents.
        std::vector<MoneyDeltaPacket> moneyDeltaPkts;
        EnterCriticalSection(&outCs_);
        moneyDeltaPkts.swap(outMoneyDelta_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < moneyDeltaPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&moneyDeltaPkts[i], sizeof(MoneyDeltaPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued faction-relation rows on CH_RELIABLE
        // (protocol 24). Change-gated by the Replicator; a settled diplomacy
        // is silent. A lost row would diverge hostility until the safety
        // resend, so reliable is the right channel.
        std::vector<FactionPacket> facPkts;
        EnterCriticalSection(&outCs_);
        facPkts.swap(outFaction_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < facPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&facPkts[i], sizeof(FactionPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued game-clock samples on CH_RELIABLE (protocol
        // 25). ~1 Hz host broadcast; ordered-reliable keeps the join's offset
        // estimator from ever seeing samples out of order.
        std::vector<TimePacket> timePkts;
        EnterCriticalSection(&outCs_);
        timePkts.swap(outTime_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < timePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&timePkts[i], sizeof(TimePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued baked-door state rows on CH_RELIABLE
        // (protocol 26). Change-gated by the Replicator; a settled town is
        // silent. A lost row would leave a door diverged until the safety
        // resend, so reliable is the right channel.
        std::vector<DoorPacket> doorPkts;
        EnterCriticalSection(&outCs_);
        doorPkts.swap(outDoor_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < doorPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&doorPkts[i], sizeof(DoorPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued machine state rows on CH_RELIABLE
        // (protocol 33). Host -> joins only in practice (the Replicator only
        // publishes on the host); change-gated + safety-resent by the caller,
        // so a settled base is near-silent.
        std::vector<ProdPacket> prodPkts;
        EnterCriticalSection(&outCs_);
        prodPkts.swap(outProd_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < prodPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&prodPkts[i], sizeof(ProdPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued known-research rows on CH_RELIABLE
        // (protocol 38). Host -> joins only (the Replicator only publishes on
        // the host); first-sight + safety-resent by the caller, so a settled
        // tech tree is near-silent.
        std::vector<ResearchPacket> researchPkts;
        EnterCriticalSection(&outCs_);
        researchPkts.swap(outResearch_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < researchPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&researchPkts[i], sizeof(ResearchPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued property-deed rows on CH_RELIABLE (protocol
        // 54). SYMMETRIC - either client may buy - so this drains on both
        // sides. First-sight + safety-resent by the caller; a party that owns a
        // stable set of buildings is silent between resends.
        std::vector<DeedPacket> deedPkts;
        EnterCriticalSection(&outCs_);
        deedPkts.swap(outDeed_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < deedPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&deedPkts[i], sizeof(DeedPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued runtime-fixture identity rows on CH_RELIABLE
        // (protocol 55). SYMMETRIC: the pose path is bidirectional, so each side
        // must be able to translate the other's fixture hands.
        std::vector<FixturePacket> fixPkts;
        EnterCriticalSection(&outCs_);
        fixPkts.swap(outFixture_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < fixPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&fixPkts[i], sizeof(FixturePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued placed-building announcements + progress
        // rows on CH_RELIABLE (protocol 27). PLACE is a one-shot describe/mint
        // edge (a lost one strands an invisible building on the peer - the
        // protocol-21 lesson); STATE rows are change-gated by the Replicator
        // (~1 Hz sample, 10 s safety resend while incomplete), so the channel
        // is silent once every site completes. Same-channel ordered-reliable
        // guarantees a STATE row never arrives before its PLACE.
        std::vector<BuildPlacePacket> buildPlacePkts;
        std::vector<BuildStatePacket> buildStatePkts;
        EnterCriticalSection(&outCs_);
        buildPlacePkts.swap(outBuildPlace_);
        buildStatePkts.swap(outBuildState_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < buildPlacePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildPlacePkts[i], sizeof(BuildPlacePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < buildStatePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildStatePkts[i], sizeof(BuildStatePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued placed-building door rows + removals on
        // CH_RELIABLE (protocol 28). Door rows are change-gated by the
        // Replicator (the protocol-26 door cadence on the translated key);
        // a REMOVE is a one-shot edge - losing it would strand a ghost proxy
        // on the peer, so reliable is mandatory.
        std::vector<BuildDoorPacket>   buildDoorPkts;
        std::vector<BuildRemovePacket> buildRemovePkts;
        EnterCriticalSection(&outCs_);
        buildDoorPkts.swap(outBuildDoor_);
        buildRemovePkts.swap(outBuildRemove_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < buildDoorPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildDoorPkts[i], sizeof(BuildDoorPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < buildRemovePkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&buildRemovePkts[i], sizeof(BuildRemovePacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued stealth detection-map snapshots on
        // CH_UNRELIABLE (protocol 20). Latest-wins continuous state: loss just
        // delays an arrow refresh until the next throttled snapshot.
        std::vector<StealthPacket> stealthPkts;
        EnterCriticalSection(&outCs_);
        stealthPkts.swap(outStealth_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < stealthPkts.size(); ++i) {
            ENetPacket* out = enet_packet_create(&stealthPkts[i], sizeof(StealthPacket),
                                                 0 /*unreliable*/);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued camera hints on CH_UNRELIABLE (protocol
        // 43, both directions, ~1 Hz). Latest wins; a lost hint is replaced
        // by the next one a second later.
        std::vector<CamHintPacket> camHints;
        EnterCriticalSection(&outCs_);
        camHints.swap(outCamHint_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < camHints.size(); ++i) {
            ENetPacket* out = enet_packet_create(&camHints[i], sizeof(CamHintPacket),
                                                 0 /*unreliable*/);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Cell claims ride CH_RELIABLE (protocol 49, ~1 Hz): losing one leaves
        // the cell reading as host-authored until the next re-assert, and both
        // sides authoring the same bodies is the failure this channel exists to
        // prevent.
        std::vector<CellClaimPacket> cellClaims;
        EnterCriticalSection(&outCs_);
        cellClaims.swap(outCellClaim_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < cellClaims.size(); ++i) {
            ENetPacket* out = enet_packet_create(&cellClaims[i], sizeof(CellClaimPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued runtime-spawn packets on CH_RELIABLE
        // (protocol 21). Requests are debounced per hand and replies are
        // cache-throttled by the Replicator, so the reliable channel stays
        // quiet; a lost request would strand an invisible enemy on the join,
        // so reliable is mandatory.
        std::vector<SpawnReqPacket>  spawnReqs;
        std::vector<SpawnInfoPacket> spawnInfos;
        EnterCriticalSection(&outCs_);
        spawnReqs.swap(outSpawnReq_);
        spawnInfos.swap(outSpawnInfo_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < spawnReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&spawnReqs[i], sizeof(SpawnReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        for (size_t i = 0; i < spawnInfos.size(); ++i) {
            ENetPacket* out = enet_packet_create(&spawnInfos[i], sizeof(SpawnInfoPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued coordinated-save packets on CH_BULK (protocol
        // 31; moved off CH_RELIABLE in v44). REQ/ACK are fixed PODs; FILE/DONE
        // carry their variable tails (ENet fragments + reassembles the ~4 KB
        // chunks transparently on the reliable channel). Pacing lives in the
        // SaveXfer sender (~32 chunks queued per 50 ms), so one drain never
        // floods the channel - and CH_BULK keeps the megabytes off CH_RELIABLE,
        // so a live transfer no longer stalls door/money/faction events.
        std::vector<SaveReqPacket>   saveReqs;
        std::vector<OutSaveBegin>    saveBegins;
        std::vector<OutSaveFile>     saveFiles;
        std::vector<OutSaveDone>     saveDones;
        std::vector<SaveAckPacket>   saveAcks;
        EnterCriticalSection(&outCs_);
        saveReqs.swap(outSaveReq_);
        saveBegins.swap(outSaveBegin_);
        saveFiles.swap(outSaveFile_);
        saveDones.swap(outSaveDone_);
        saveAcks.swap(outSaveAck_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < saveReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveReqs[i], sizeof(SaveReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(OWNER_ID_ALL, CH_BULK, out);
        }
        for (size_t i = 0; i < saveBegins.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveBegins[i].pkt, sizeof(SaveBeginPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(saveBegins[i].targetId, CH_BULK, out);
        }
        for (size_t i = 0; i < saveFiles.size(); ++i) {
            unsigned bytes = sizeof(SaveFileHeader) + (unsigned)saveFiles[i].tail.size();
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            std::memcpy(out->data, &saveFiles[i].hdr, sizeof(SaveFileHeader));
            if (!saveFiles[i].tail.empty())
                std::memcpy(out->data + sizeof(SaveFileHeader), &saveFiles[i].tail[0],
                            saveFiles[i].tail.size());
            routeOut(saveFiles[i].targetId, CH_BULK, out);
        }
        for (size_t i = 0; i < saveDones.size(); ++i) {
            unsigned bytes = sizeof(SaveDoneHeader)
                           + (unsigned)saveDones[i].crcs.size() * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            std::memcpy(out->data, &saveDones[i].hdr, sizeof(SaveDoneHeader));
            if (!saveDones[i].crcs.empty())
                std::memcpy(out->data + sizeof(SaveDoneHeader), &saveDones[i].crcs[0],
                            saveDones[i].crcs.size() * sizeof(u32));
            routeOut(saveDones[i].targetId, CH_BULK, out);
        }
        for (size_t i = 0; i < saveAcks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&saveAcks[i], sizeof(SaveAckPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(OWNER_ID_ALL, CH_BULK, out);
        }

        // Drain + send queued coordinated-load packets (protocol 32) on CH_BULK
        // (v44). All fixed PODs: GO host -> join, REQ/NACK join -> host. They
        // share CH_BULK with the save transfer they gate (a NACK's fallback
        // stream must stay ordered behind its GO), and off CH_RELIABLE so they
        // do not queue behind - or ahead of - live game events.
        std::vector<OutLoadGo>      loadGos;
        std::vector<LoadReqPacket>  loadReqs;
        std::vector<LoadNackPacket> loadNacks;
        EnterCriticalSection(&outCs_);
        loadGos.swap(outLoadGo_);
        loadReqs.swap(outLoadReq_);
        loadNacks.swap(outLoadNack_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < loadGos.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadGos[i].pkt, sizeof(LoadGoPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(loadGos[i].targetId, CH_BULK, out);
        }
        for (size_t i = 0; i < loadReqs.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadReqs[i], sizeof(LoadReqPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(OWNER_ID_ALL, CH_BULK, out);
        }
        for (size_t i = 0; i < loadNacks.size(); ++i) {
            ENetPacket* out = enet_packet_create(&loadNacks[i], sizeof(LoadNackPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            routeOut(OWNER_ID_ALL, CH_BULK, out);
        }

        // Transmit this peer's owned entities (latest snapshot), chunked so each
        // batch fits one datagram - which on Steam means the 1200 B clamped MTU
        // (ENet would otherwise send the oversized unreliable packet as RELIABLE
        // fragments: retransmits + ordering stalls on the motion stream). Raw UDP
        // keeps the full 17-entity chunk. Unreliable: the newest batch supersedes
        // loss.
        const unsigned batchCap = steam ? ENTITY_BATCH_MAX_STEAM : ENTITY_BATCH_MAX;
        std::vector<EntityState> ents;
        u32  owner = 0;
        u32  stamp = 0;
        bool have  = false;
        EnterCriticalSection(&outCs_);
        ents  = out_;
        owner = outOwner_;
        stamp = outStampMs_;
        have  = haveOut_;
        LeaveCriticalSection(&outCs_);

        if (have && !ents.empty()) {
            for (size_t off = 0; off < ents.size(); off += batchCap) {
                unsigned count = (unsigned)(ents.size() - off);
                if (count > batchCap) count = batchCap;

                unsigned bytes = sizeof(EntityBatchHeader) + count * sizeof(EntityState);
                ENetPacket* out = enet_packet_create(0, bytes, 0 /*unreliable*/);
                EntityBatchHeader hdr;
                hdr.type = (u8)PKT_ENTITY_BATCH; hdr.ownerId = owner; hdr.count = (u8)count;
                hdr.sendMs = stamp;
                hdr.epoch  = (u32)sendEpoch_; // v44: current session epoch
                std::memcpy(out->data, &hdr, sizeof(hdr));
                std::memcpy(out->data + sizeof(hdr), &ents[off], count * sizeof(EntityState));
                if (isHost_) {
                    enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
                } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                    enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
                } else {
                    enet_packet_destroy(out); // no one to send to yet
                }
            }
        }
    }

    if (enetHost_) { enet_host_destroy(enetHost_); enetHost_ = 0; }
    peersById_.clear(); // the ENetPeers died with the host (protocol 49)
    rankById_.clear();
    if (steam) steamp2p::removeEnetHooks();
    InterlockedExchange(&running_, 0);
}

// 连接错误通道（上游 PR #24）：setNetUiError 在网络线程调用，netUiError/clearNetUiError 在主线程调用，
// 三者共用同一把锁
void setNetUiError(const char* msg) {
    EnterCriticalSection(&g_netUiErr.cs);
    _snprintf(g_netUiErr.buf, sizeof(g_netUiErr.buf) - 1, "%s", msg ? msg : "");
    g_netUiErr.buf[sizeof(g_netUiErr.buf) - 1] = '\0';
    LeaveCriticalSection(&g_netUiErr.cs);
}

const char* netUiError() {
    // 调用方只在主线程，锁内拷贝到主线程私有缓冲区，返回的字符串不会被网络线程写到一半
    static char mainCopy[256];
    EnterCriticalSection(&g_netUiErr.cs);
    memcpy(mainCopy, g_netUiErr.buf, sizeof(mainCopy));
    LeaveCriticalSection(&g_netUiErr.cs);
    return mainCopy;
}

void clearNetUiError() {
    EnterCriticalSection(&g_netUiErr.cs);
    g_netUiErr.buf[0] = '\0';
    LeaveCriticalSection(&g_netUiErr.cs);
}

void NetLink::setIdentity(u64 id, const std::string& name) {
    playerId_ = id;
    memset(playerName_, 0, sizeof(playerName_));
    strncpy(playerName_, name.c_str(), sizeof(playerName_) - 1);
}

void NetLink::queueOwners(u64 worldId, u32 revision, u32 authEpoch, const std::vector<OwnerRow>& rows) {
    if (rows.size() > OWNERS_MAX) return;
    InboundOwners p;
    p.header.type = (u8)PKT_OWNERS; p.header.worldId = worldId;
    p.header.revision = revision; p.header.authEpoch = authEpoch;
    p.header.count = (u16)rows.size(); p.rows = rows;
    pushLocked(outCs_, outOwners_, p);
}

void NetLink::queueOwnerRequest(const OwnerRequestPacket& request) {
    pushLocked(outCs_, outOwnerRequests_, request);
}

void NetLink::queueCharacterPacket(u32 target, const std::vector<u8>& data) {
    if (data.empty() || data.size() > CHARACTER_DRAFT_MAX + sizeof(CharacterRequestHeader)) return;
    OutCharacter p; p.targetId = target; p.epoch = (u32)sendEpoch_; p.data = data;
    EnterCriticalSection(&outCs_);
    if (outCharacter_.size() < 64) outCharacter_.push_back(p);
    LeaveCriticalSection(&outCs_);
}

} // namespace coop
