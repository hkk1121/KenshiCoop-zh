// ReplicatorCore.cpp - Replicator construction + session lifecycle (monolith
// split from Replicator.cpp, 2026-07-12): the ctor defaults, the Phase 3
// unified entity-lifecycle audit (lifeName/lifeSet/lifeSweep + life_),
// resetSession (the ONE place every cross-tick hub resets on session swap),
// inbound ingest (ingest/ingestInv), tab latching/ranking, and the
// end-of-session smoothness summary.
//
// Shared hubs written here: ALL of them (resetSession clears every map);
// life_ is owned here (lifeSet is the only writer, called from every TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"
#include "../game/EngineChargen.h"
#include "SaveXfer.h"

namespace coop {

Replicator::Replicator()
    : catchupK_(CATCHUP_K), snapDist_(SNAP_DIST), snapSeconds_(SNAP_SECONDS),
      combatSoftDist_(COMBAT_SOFT_DIST), combatSnapDist_(COMBAT_SNAP_DIST),
      combatBigSnapDist_(COMBAT_BIG_SNAP_DIST), combatSlideMax_(COMBAT_SLIDE_MAX),
      combatConvergeMs_(COMBAT_CONVERGE_MS),
      sendStamp_(true),
      starveHoldMs_(10000), starveHeldNow_(0),
      leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f), slewSkipFrames_(0),
      zpDown_(0), zpCarried_(0), zpFurn_(0), zpChain_(0), zpCrawl_(0),
      zpSneak_(0), zpSquadIdle_(0), zpAlias_(0), zpStall_(0),
      onsetReentries_(0),
      onActiveEarly_(0), onActiveMid_(0), onActiveSteady_(0),
      onZeroEarly_(0), onZeroMid_(0), onZeroSteady_(0),
      interpLerp_(0), interpSingle_(0), interpClampOld_(0),
      interpExtrap_(0), interpSegSnap_(0),
      hardSnapSquad_(0), hardSnapNpc_(0), hardSnapMid_(0),
      walkReissueSquad_(0), walkReissueNpc_(0), restFlipNpc_(0), restFlipMid_(0),
      combatSnapTotal_(0), combatSoftWalk_(0), combatSlide_(0), combatOrder_(0),
      combatWrongTgt_(0), combatLogTick_(0),
      interpLogTick_(0),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      marchHold_(0), marchSettle_(0), marchRelapse_(0),
      marchHoldDip_(0), marchHoldStop_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      aiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), worldSeeded_(false),
      nextDropId_(1), nextPickupId_(1), nextXferId_(1),
      xferScanMs_(0), nextTreatId_(1),
      quietRelapse_(0), crawlPhysRestore_(0),
      sitOrders_(0), detachUses_(0), noDetach_(false),
      dmgGuard_(false), reportCombat_(false), nextHitId_(1),
      carrySync_(true), furnSync_(true), chainSync_(true),
      stealthSync_(true), proneSync_(true),
      gateAuthority_(false), trustLogTick_(0),
      trustGrants_(0), trustRevokes_(0),
      authSuppresses_(0), authRestores_(0), authReassertMs_(0), authPruned_(0),
      censusRadius_(0.0f), censusSendMs_(0), censusRecvMs_(0),
      censusCulls_(0),
      censusOffCell_(0), cellYields_(0), localId_(0xFFFFFFFFu), hostDriveRefusals_(0),
      censusPubTrunc_(false), censusFreshPrev_(false), censusFreshChkMs_(0),
      censusStaleMs_(0), censusStaleEdges_(0), proxyDriftLogMs_(0),
      camHintSendMs_(0),
      midCursor_(0), midSliceMs_(0), midFastPromoted_(0),
      censusParkDist_(0.0f), censusParks_(0),
      censusPrevMs_(0), censusWalkDist_(0.0f), censusWalks_(0),
      censusFreezeAi_(true),
      attentionRadius_(0.0f),
      attnFlips_(0), attnWinMs_(0), attnBaseSupp_(0), attnBaseCull_(0),
      attnBaseProxy_(0), attnVetoMs_(0), attnVetoRawN_(0), attnVetoMask_(0),
      auditRows_(false), jailProbe_(false), jailObserve_(false),
      speedLastApplied_(-1.0f), speedMyReq_(-1.0f),
      speedCombatCap_(true),
      speedMyCombat_(false), speedLastSet_(-1.0f),
      speedSeqOut_(1),
      speedLastSendMs_(0), speedCombatSampleMs_(0), speedCombatHoldMs_(0),
      spawnSync_(false), spawnPosLogMs_(0),
      spawnMintRadius_(0.0f), adoptRadius_(0.0f), censusAdopts_(0),
      censusScanMs_(0),
      poolSeen_(-1),
      poolSeq_(0), poolAcked_(0),
      moneySync_(true), recruitSync_(true),
      squadSync_(true), tabsSeeded_(0),
      sessionLive_(false), peerBodyTabs_(0),
      cellAuth_(false), cellCollapse_(false), collapsed_(false),
      claimSendMs_(0), claimAssertMs_(0), claimMapMs_(0),
      facSeqOut_(1), facSampleMs_(0), factionSync_(true),
      doorSeqOut_(1), doorSampleMs_(0), doorSync_(true),
      buildKeySerial_(0), buildSeqOut_(1), buildSampleMs_(0), buildSync_(true),
      bdoorSeqOut_(1), bdoorSampleMs_(0), bdoorSync_(true),
      hungerSync_(true),
      prodSeqOut_(1), prodSampleMs_(0), prodSync_(true),
      researchSeqOut_(1), researchSampleMs_(0), researchSync_(true),
      deedSeqOut_(1), deedSampleMs_(0), deedAuditMs_(0), deedSync_(true),
      fixtureSeqOut_(1), fixtureSampleMs_(0), fixtureSync_(true),
      storeSync_(false), contCensusMs_(0),
      timeSync_(true), timeBrake_(true),
      timeSlew_(1.0f), timeSeqOut_(1), timeSeqSeen_(0),
      timeLastSendMs_(0), timeLastLogMs_(0), timeSlewApplied_(-1.0f),
      platoonT0_(0),
      lifeSweepMs_(0),
      tabNotice_(0), playerIdentity_(0), ownersWorld_(0), ownersFileWorld_(0),
      ownersRevision_(0), ownersAuthEpoch_(1), ownersLoaded_(false), ownersAttached_(false),
      ownersDirty_(false), ownersHaveSnapshot_(false), ownersInvalid_(false), ownersResetPending_(false),
      ownersKeepOnIdentity_(false) {
    characterWorldEpoch_ = ownersAuthEpoch_;
    walletsLoaded_ = false;
}

// ---- Phase 3: unified entity lifecycle ---------------------------------------
// The AUDIT layer over the authority/mint/drive machinery: every decision
// point reports the state it just put a hand into, lifeSet logs the edge, and
// the lifecycle oracle judges the journeys. Mechanics live where they were
// validated; this is the one place their OUTCOMES meet.

const char* Replicator::lifeName(int s) {
    switch (s) {
    case LIFE_DISCOVERED: return "DISCOVERED";
    case LIFE_RESOLVED:   return "RESOLVED";
    case LIFE_HI:         return "HI";
    case LIFE_MID:        return "MID";
    case LIFE_PARKED:     return "PARKED";
    case LIFE_CULLED:     return "CULLED";
    default:              return "UNKNOWN";
    }
}

int Replicator::lifeSet(const Key& k, int to, const char* reason) {
    unsigned long now = nowMs();
    Lifecycle& lc = life_[k];
    int from = lc.state;
    lc.touchMs = now;
    if (from == to) return from;
    lc.state = (u8)to;
    lc.sinceMs = now;
    lc.stuckLogMs = 0;
    // Silent seed: every census-band wilderness NPC is born PARKED, and
    // logging hundreds of those at session start buries the real journeys.
    if (from == LIFE_UNKNOWN && to == LIFE_PARKED) return from;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[life] hand=%u,%u,%u,%u,%u from=%s to=%s reason=%s",
              k.t, k.c, k.cs, k.i, k.s,
              lifeName(from), lifeName(to), reason ? reason : "-");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return from;
}

void Replicator::lifeSweep(GameWorld* gw, unsigned long now) {
    if ((now - lifeSweepMs_) < 5000) return;
    lifeSweepMs_ = now;
    // A hand not confirmed for a while left interest (or its body despawned):
    // drop the record so the map tracks the live world, and a re-appearance
    // starts a fresh journey from UNKNOWN.
    const unsigned long PRUNE_MS = 30000;
    // Self-audit: DISCOVERED means "the host swears this exists and we have
    // no body" - the mint pipeline should resolve that within its dwell +
    // round-trip budget WHEN the hand's census position sits in a locally
    // loaded zone (an unloaded far zone defers legitimately, forever if need
    // be). Older than STUCK_MS is the invisible-raid failure (the join
    // fights nothing); one line per hand per STUCK_LOG_MS so the lifecycle
    // oracle sees it without the log flooding.
    const unsigned long STUCK_MS     = 30000;
    const unsigned long STUCK_LOG_MS = 15000;
    for (std::map<Key, Lifecycle>::iterator it = life_.begin();
         it != life_.end(); ) {
        Lifecycle& lc = it->second;
        if ((now - lc.touchMs) > PRUNE_MS) { life_.erase(it++); continue; }
        if (lc.state == LIFE_DISCOVERED && (now - lc.sinceMs) > STUCK_MS &&
            (lc.stuckLogMs == 0 || (now - lc.stuckLogMs) > STUCK_LOG_MS)) {
            std::map<Key, CensusPos>::iterator cp = censusPos_.find(it->first);
            bool mintable = cp != censusPos_.end() &&
                            engine::isZoneLoadedAt(gw, cp->second.x,
                                                   cp->second.y, cp->second.z);
            if (mintable) {
                lc.stuckLogMs = now;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "[life] STUCK hand=%u,%u,%u,%u,%u state=DISCOVERED age=%lus (mintable)",
                          it->first.t, it->first.c, it->first.cs, it->first.i,
                          it->first.s, (now - lc.sinceMs) / 1000);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                lc.stuckLogMs = now; // re-check the zone on the next cadence
            }
        }
        ++it;
    }
}

void Replicator::resetSession(bool keepWalletSequence, bool keepWallets) {
    // Pointer caches (dangling after the swap) + everything keyed off them.
    targets_.clear();          // interp buffers + per-body drive state
    drivenChars_.clear();
    drivenSeen_.clear();       // recently-driven grace (pointers dangle after swap)
    canonicalOf_.clear();      // capture-translation reverse map (same pointers)
    rekeyLogged_.clear();      // and the change-detector for its [rekey] line
    jailObs_.clear();          // jail-observe spike per-captive last sample
    proxyByKey_.clear(); createdAppearanceApplied_.clear();
    mintedBodies_.clear();     // 与 proxyByKey_ 同为旧世界指针，读档后失效
    suppressed_.clear();
    midBand_.clear();          // host mid-band round-robin (rebuilt by next census)
    midCursor_ = 0; midSliceMs_ = 0;
    life_.clear();             // Phase 3 lifecycle: the OLD world's journeys
    lifeSweepMs_ = 0;
    // Debug markers hold raw Character* from the OLD world plus GUI label
    // objects we own - destroy the labels and drop the map before either
    // dangles into the new session.
    for (std::map<Character*, DebugMarker>::iterator mi = debugMarkers_.begin();
         mi != debugMarkers_.end(); ++mi)
        engine::markerDestroy(mi->second.label);
    debugMarkers_.clear();
    hostBody_.clear();
    attackerOf_.clear();
    combatCapMs_.clear();
    authCount_.clear();
    ownHands_.clear(); allSquad_.clear();
    // Protocol 36: the existence census describes the OLD world's hands; the
    // host re-publishes within a second of the new world going live.
    censusHands_.clear();
    censusByOwner_.clear();
    censusPos_.clear();
    censusPrev_.clear();
    censusPrevMs_ = 0;
    censusFix_.clear();
    parkMs_.clear();
    censusRecvMs_ = 0;
    censusSendMs_ = 0;
    // Protocol 43: the camera hints describe the OLD world's coordinates.
    camHintSendMs_ = 0;
    peerCams_.clear();
    // Attention latches are per-hand state about the OLD world's geometry.
    attnObs_.clear();
    attnObsPeer_.clear();
    // Claims describe where bodies stand in the OLD world.
    claimSlots_.clear();
    claimedCells_.clear();
    cellLastOwner_.clear();
    claimDwell_.clear();
    cellYield_.clear();
    claimSendMs_   = 0;
    claimAssertMs_ = 0;
    claimMapMs_    = 0;
    attnFlips_ = 0;
    attnWinMs_ = 0;
    // The zone verdict describes the OLD world's streaming state.
    attnVetoMs_ = 0;
    attnVetoRawN_ = 0;
    attnVetoMask_ = 0;
    furnPeerPend_.clear();
    ownFurnExit_.clear();
    // Session maps + change-gate baselines (they describe the OLD world; the
    // reloaded save re-seeds them on first sample).
    ownBuilds_.clear();
    peerBuilds_.clear();
    mintByLocal_.clear();
    bdoorRows_.clear();
    doorRows_.clear();
    prodRows_.clear();
    researchRows_.clear(); // protocol 38: re-baselined from the new world's store
    deedRows_.clear();     // protocol 54: re-baselined from the new world's owned set
    facRows_.clear();
    invPub_.clear();
    invRecv_.clear();
    ownedContainers_.clear();
    censusContainers_.clear(); // protocol 34: re-censused in the new world
    worldTrack_.clear();
    worldProxies_.clear();
    worldSeeded_ = false; // re-baseline the reloaded world's save-native items
    weaponCensus_.clear();
    appliedDrops_.clear();
    appliedPickups_.clear();
    groundedWeapons_.clear();
    pendingPickups_.clear(); // the objects those intents named belong to the old world
    // Protocol 37: every container hand and Item* baseline is stale in the new world.
    xferBase_.clear();
    xferSeeded_.clear();
    xferPend_.clear();
    xferLatch_.clear();
    xferDefer_.clear();
    xferOut_.clear();
    dstApplied_.clear();
    xferDebt_.clear();
    xferShort_.clear();
    appliedXfers_.clear();
    wdSuppress_.clear();
    xferScanMs_ = 0;
    medPub_.clear();
    medRecv_.clear();
    medNpc_.clear();
    statsPub_.clear();
    // 读档后重建余额基线并丢弃旧世界的待确认变化量；同一连接的发送序号保持递增，
    // 避免主机把新交易当成重复消息，或旧确认清掉新交易。真正断线时才清零序号。
    poolSeen_ = -1;
    if (!keepWalletSequence) poolSeq_ = 0;
    poolAcked_ = 0;
    poolPending_.clear();
    // 同一主机只重启网络时保留余额和首次开局记录，连接相关状态仍清零；读档则重新加载账本。
    ownersKeepOnIdentity_ = keepWallets && isHostRole();
    if (keepWallets) {
        for (std::map<u64, Wallet>::iterator it = wallets_.begin(); it != wallets_.end(); ++it) {
            it->second.acked = 0; it->second.connection = 0;
            it->second.sent = -1; it->second.sentMs = 0;
        }
        char b[96]; _snprintf(b, sizeof(b) - 1,
            "[wallet] ledger kept players=%u", (unsigned)wallets_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } else {
        wallets_.clear(); walletsLoaded_ = false;
    }
    stealthPub_.clear();
    pinOwned_.clear();
    pinPeer_.clear();
    moveEcho_.clear();
    exitedOwn_.clear();
    // Protocol 35: the rank latch + the engine's pointer->hand baseline both
    // describe the OLD world (containers and Character* dangle after a swap);
    // the reloaded save re-seeds them at first census/poll.
    tabRank_.clear();
    tabOwned_.clear();
    tabControl_.clear(); pendingPeerMembers_.clear();
    tabsSeeded_ = 0;
    // 协议 1002：小队控制授权不写存档，断线 / 读档 / 会话重置一律作废（第 2 批）
    tabAuth_.clear();
    tabReq_.clear();
    tabOutbox_.clear();
    tabNotice_ = 0;
    peerBodyOwnerCounts_.clear();
    peerBodyOwnerQueue_.clear();
    peerBodyTabs_ = 0;
    rekeyedOld_.clear();
    engine::clearSquadRoster();
    probed_.clear();
    spawnReq_.clear();
    unresolvedHands_.clear();
    forceReqHands_.clear();
    spawnLogged_.clear();
    spawnReplyMs_.clear();
    censusScanMs_ = 0;
    // Speed/time consensus: re-seed from the fresh world's live state (the
    // save's speed becomes the new baseline; the join's slew re-measures).
    speedLastApplied_ = -1.0f;
    speedMyReq_       = -1.0f;
    speedVotes_.clear();
    speedMyCombat_    = false;
    speedLastSet_     = -1.0f;
    speedSeqSeen_.clear();
    speedLastSendMs_  = 0;
    speedCombatSampleMs_ = 0;
    speedCombatHoldMs_ = 0;
    timeSlew_         = 1.0f;
    timeSeqSeen_      = 0;
    timeLastSendMs_   = 0;
    timeSlewApplied_  = -1.0f;
    // Sample-cadence clocks restart.
    facSampleMs_ = doorSampleMs_ = buildSampleMs_ = bdoorSampleMs_ = 0;
    prodSampleMs_ = 0;
    researchSampleMs_ = 0;
    deedSampleMs_ = 0;
    deedAuditMs_ = 0;
    // A coordinated load re-instantiates every runtime fixture, so both halves
    // of the protocol-55 pairing are stale: re-announce and re-match from
    // scratch rather than translate a hand into a destroyed object.
    fixtureSampleMs_ = 0;
    fixtureOut_.clear();
    fixtureMap_.clear();
    contCensusMs_ = 0;
    authReassertMs_ = 0;
    // Config gates, ownRanks_ and every OUTBOUND seq counter are deliberately
    // preserved (see the header comment).
    coop::logLine("[load] session reset: pointer caches, session maps, change gates cleared");
}

void Replicator::clearPeerReplicationState(GameWorld* gw, bool keepWallets) {
    // Destroy every MINTED proxy body before resetSession() drops the map that
    // owns the pointers. The engine owns the bodies; a proxy left standing after
    // the peer that authored it is gone is a permanent ghost, and any map still
    // pointing at it (targets_, drivenChars_) would drive a body with no fresh
    // authority - or a freed pointer once the engine reaps it. SEH-guarded via
    // despawnProxyNpc so a single bad pointer can't take down the leave path.
    //
    // ADOPTED entries are released, not destroyed (destroyIfMinted): a body we
    // adopted is one this client generated itself, so the peer leaving makes it
    // ours again rather than making it garbage. Destroying them here would empty a
    // whole town on disconnect, and bake that emptiness into the next save.
    unsigned int cleared = 0, released = 0;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ++it) {
        if (!gw || !it->second) continue;
        if (destroyIfMinted(gw, it->second)) ++cleared; else ++released;
    }
    mintedBodies_.clear();
    char b[128];
    _snprintf(b, sizeof(b) - 1, "[leave] cleared proxies=%u released=%u",
              cleared, released);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // World-item proxies (Phase 3): the world stays LIVE across a peer leave /
    // reconnect (no engine world swap), so the proxy RootObjects we minted for the
    // departed peer's ground items must be destroyed here - otherwise they linger
    // as duplicate items and, worse, get baked into the save on the next write
    // (becoming natives that re-stream on reload). resetSession() below only clears
    // the map, not the bodies, so despawn first. The world surviving does NOT mean
    // the proxies did: the peer may have left after a long trek, and anything whose
    // block unloaded on the way is already destroyed, so each one is re-resolved.
    unsigned int wcleared = 0, wstale = 0;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator wi = worldProxies_.begin();
         wi != worldProxies_.end(); ++wi) {
        RootObject* live = gw ? liveWorldProxy(wi->second) : 0;
        if (!live) { ++wstale; continue; }
        if (engine::removeWorldItemProxy(gw, live)) ++wcleared;
    }
    _snprintf(b, sizeof(b) - 1, "[leave] cleared worldProxies=%u stale=%u",
              wcleared, wstale);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // Now drop every map (proxyByKey_ included) back to freshly-launched state.
    for (std::map<u64, PeerStatusPacket>::iterator p = players_.begin(); p != players_.end(); ++p)
        if (p->first != playerIdentity_) p->second.present = 0;
    resetSession(false, keepWallets);
}

void Replicator::clearOwnerReplicationState(GameWorld* gw, u32 ownerId) {
    // The whole remote session ended (a join lost its host link, or the host
    // went offline): the legacy full wipe is both correct and required.
    if (ownerId == OWNER_ID_ALL) { clearPeerReplicationState(gw); return; }

    // 该 owner 参与的授权全部作废。借给它的标签收回，向它借的标签停发。
    std::vector<std::pair<std::pair<u32, u32>, bool> > revert;
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator ai = tabAuth_.begin();
         ai != tabAuth_.end(); ++ai) {
        if (ai->second.ownerId != ownerId) continue;
        revert.push_back(std::make_pair(ai->first, ai->second.mode == 1));
    }
    for (unsigned int ri = 0; ri < revert.size(); ++ri)
        reassignTab(gw, revert[ri].first, revert[ri].second);
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator ri = tabReq_.begin();
         ri != tabReq_.end(); ) {
        if (ri->second.ownerId == ownerId) tabReq_.erase(ri++);
        else ++ri;
    }
    peerBodyOwnerCounts_.erase(ownerId);
    // 连接编号可能复用，新进程的低序号不能被上一位玩家留下的门槛拒绝。
    for (std::map<std::string, FacRow>::iterator r = facRows_.begin(); r != facRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<Key, DoorRow>::iterator r = doorRows_.begin(); r != doorRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<Key, PeerBuild>::iterator r = peerBuilds_.begin(); r != peerBuilds_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<std::pair<Key, int>, BdoorRow>::iterator r = bdoorRows_.begin(); r != bdoorRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<std::pair<int, Key>, ProdRow>::iterator r = prodRows_.begin(); r != prodRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<std::string, ResearchRow>::iterator r = researchRows_.begin(); r != researchRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<Key, DeedRow>::iterator r = deedRows_.begin(); r != deedRows_.end(); ++r) r->second.seqSeen.erase(ownerId);
    for (std::map<Key, FixtureRow>::iterator r = fixtureMap_.begin(); r != fixtureMap_.end(); ++r) r->second.seqSeen.erase(ownerId);

    // ONE owner left a session that continues for everyone else (protocol 49).
    // Surgical teardown of exactly its authored state; everything shared and
    // everything authored by the remaining owners stays live.
    //
    // 1. Proxies it authored: a minted body whose hand is pinned to this owner
    //    has no authority left behind it - despawn it (SEH-guarded) before the
    //    maps that point at it are erased. Save-native bodies it drove are NOT
    //    despawned - they idle at their last pose until the owner rejoins.
    unsigned int cleared = 0;
    for (std::map<Key, u32>::iterator pi = pinPeer_.begin();
         pi != pinPeer_.end(); ++pi) {
        if (pi->second != ownerId) continue;
        std::map<Key, Character*>::iterator px = proxyByKey_.find(pi->first);
        if (px == proxyByKey_.end()) continue;
        if (gw && px->second && destroyIfMinted(gw, px->second)) ++cleared;
        proxyByKey_.erase(px);
    }
    // 2. Stop driving its bodies: erase the interp/drive records its stream
    //    fed. drivenChars_/drivenSeen_/canonicalOf_ are pointer-keyed per-tick
    //    caches - the drive grace prune ages them out naturally once nothing
    //    re-feeds them.
    unsigned int undriven = 0;
    for (std::map<Key, Driven>::iterator ti = targets_.begin();
         ti != targets_.end(); ) {
        if (ti->second.owner != ownerId) { ++ti; continue; }
        std::map<Key, Character*>::iterator px = proxyByKey_.find(ti->first);
        if (px != proxyByKey_.end()) {
            if (gw && px->second && destroyIfMinted(gw, px->second)) ++cleared;
            proxyByKey_.erase(px);
        }
        targets_.erase(ti++);
        ++undriven;
    }
    // 3. Its world-item proxies: keyed by (author, netId), so the scope is
    //    exact. The REAL ground items it dropped stay grounded (conservation:
    //    the drop happened; the item exists) - only the proxy layer goes.
    unsigned int wcleared = 0;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator wi = worldProxies_.begin();
         wi != worldProxies_.end(); ) {
        if (wi->first.first == ownerId) {
            if (gw && wi->second.obj && engine::removeWorldItemProxy(gw, wi->second.obj))
                ++wcleared;
            worldProxies_.erase(wi++);
        } else ++wi;
    }
    // 4. Un-pin its hands (a rejoin re-pins under its new id), and drop its
    //    received-container baselines so a rejoin's snapshots re-reconcile
    //    from scratch.
    for (std::map<Key, u32>::iterator pi = pinPeer_.begin();
         pi != pinPeer_.end(); ) {
        if (pi->second == ownerId) pinPeer_.erase(pi++);
        else ++pi;
    }
    for (std::map<Key, InvRecv>::iterator ii = invRecv_.begin();
         ii != invRecv_.end(); ) {
        if (ii->second.ownerId == ownerId) invRecv_.erase(ii++);
        else ++ii;
    }
    // 5. Its votes, hints, clock, and per-owner dedup rows. Erasing the speed
    //    vote matters most: a departed join's pause (req 0) would otherwise
    //    pin the arbitrated speed at 0 forever.
    speedVotes_.erase(ownerId);
    speedSeqSeen_.erase(ownerId);
    peerCams_.erase(ownerId);
    peerClock_.erase(ownerId);
    censusByOwner_.erase(ownerId);
    refreshCensusUnion();
    bool claimsChanged = false;
    for (std::map<std::pair<u32, u32>, CellClaim>::iterator ci = claimSlots_.begin();
         ci != claimSlots_.end(); ) {
        if (ci->first.first == ownerId) {
            claimSlots_.erase(ci++);
            claimsChanged = true;
        } else ++ci;
    }
    for (std::map<std::pair<int, int>, u32>::iterator li = cellLastOwner_.begin();
         li != cellLastOwner_.end(); ) {
        if (li->second == ownerId) {
            cellLastOwner_.erase(li++);
            claimsChanged = true;
        } else ++li;
    }
    if (claimsChanged) rebuildClaimedCells();
    for (std::set<std::pair<u32, u32> >::iterator di = appliedDrops_.begin();
         di != appliedDrops_.end(); ) {
        if (di->first == ownerId) appliedDrops_.erase(di++); else ++di;
    }
    for (std::set<std::pair<u32, u32> >::iterator pi2 = appliedPickups_.begin();
         pi2 != appliedPickups_.end(); ) {
        if (pi2->first == ownerId) appliedPickups_.erase(pi2++); else ++pi2;
    }
    for (std::set<std::pair<u32, u32> >::iterator xi = appliedXfers_.begin();
         xi != appliedXfers_.end(); ) {
        if (xi->first == ownerId) appliedXfers_.erase(xi++); else ++xi;
    }
    for (std::map<std::pair<u32, u32>, DstApplied>::iterator di = dstApplied_.begin();
         di != dstApplied_.end(); ) {
        if (di->first.first == ownerId) dstApplied_.erase(di++); else ++di;
    }
    char b[128];
    _snprintf(b, sizeof(b) - 1,
              "[leave] owner=%u cleared: driven=%u proxies=%u worldProxies=%u",
              (unsigned)ownerId, undriven, cleared, wcleared);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        // Wire v35: index the interp ring on the SENDER's capture time mapped
        // into the local clock, not the arrival time - path jitter (Steam
        // relay) otherwise smears straight into the snapshot spacing and the
        // buffer starves into extrapolation/snap cycles (the jumpy remote-
        // player movement). Mapping = sendMs + min-tracked offset (see
        // PeerClock); clamped to 'now' so a stamp can never land in the future.
        unsigned long t = now;
        if (sendStamp_) {
            PeerClock& pc = peerClock_[it->ownerId];
            long off = (long)(now - (unsigned long)it->sendMs);
            if (!pc.have) {
                pc.offsetMs = off; pc.have = true; pc.lastCreepMs = now;
            } else {
                unsigned long dt = now - pc.lastCreepMs;
                if (dt >= 500) { // creep ~2 ms/s toward slower routes
                    pc.offsetMs += (long)(dt / 500);
                    pc.lastCreepMs = now;
                }
                if (off < pc.offsetMs) pc.offsetMs = off;
            }
            t = (unsigned long)((long)it->sendMs + pc.offsetMs);
            if ((long)(t - now) > 0) t = now;
        }
        Driven& d = targets_[keyOf(it->e)];
        d.owner = it->ownerId; // protocol 49: scope a leave teardown to this owner
        d.interp.push(it->e, t, now);
        d.lastSeenMs = now;
    }
}

void Replicator::setOwnedContainerHand(const unsigned int hand[5]) {
    ownedContainers_.clear();
    Key k; k.t = hand[0]; k.c = hand[1]; k.cs = hand[2]; k.i = hand[3]; k.s = hand[4];
    ownedContainers_.insert(k);
}

void Replicator::ingestInv(Inbound& in) {
    std::deque<InboundInv> got;
    in.drainInv(got);
    for (std::deque<InboundInv>::iterator it = got.begin(); it != got.end(); ++it) {
        Key k; k.t = it->cKey[0]; k.c = it->cKey[1]; k.cs = it->cKey[2];
        k.i = it->cKey[3]; k.s = it->cKey[4];
        // Protocol 34: a placer-key row resolves through OUR build maps to
        // the LOCAL building hand (own placement = own hand; the host's
        // placement = our minted proxy). An unresolvable key (mint not
        // landed yet / refused / tombstoned) is dropped - the sender's 5 s
        // safety resend re-delivers once the mint exists.
        if (it->keyKind == 1) {
            unsigned int hand[5];
            if (!localHandForBuildKey(k, hand)) continue;
            k.t = hand[0]; k.c = hand[1]; k.cs = hand[2]; k.i = hand[3]; k.s = hand[4];
        }
        InvRecv& r = invRecv_[k];
        r.ownerId   = it->ownerId;
        r.items     = it->items; // latest snapshot supersedes
        r.dirty     = true;
        r.truncated = (it->flags & INV_FLAG_TRUNCATED) != 0;
    }
}

void Replicator::latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return; // legacy per-tick ranking needs no state
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabRank_.find(ctnrs[i]) != tabRank_.end()) continue;
        unsigned int next = (unsigned int)tabRank_.size();
        tabRank_[ctnrs[i]] = next;
        if (next >= 2) { // session-start seeding of the standard 2-tab save is silent
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[squad] LATCH cont=%u,%u rank=%u",
                      ctnrs[i].first, ctnrs[i].second, next);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::decideTabs(const EntityState* raw, unsigned int nSquad,
                            const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return;   // legacy per-tick ranking owns nothing durably
    bool seeding = (tabsSeeded_ == 0);
    if (ownersAttached_) {
        for (unsigned int m = 0; m < nSquad; ++m) {
            Character* c = engine::resolveCharByHand(raw[m].hIndex, raw[m].hSerial, raw[m].hType,
                                                    raw[m].hContainer, raw[m].hContainerSerial);
            if (!c || !mintedBodies_.count(c)) continue;
            Tab tab(raw[m].hContainer, raw[m].hContainerSerial);
            u32 owner = ownerForIdentity(tabOwnerIdentity(tab));
            if (owner == OWNER_ID_ALL || owner == localId_) continue;
            mintedBodies_.erase(c); ++peerBodyTabs_;
            if (isHostRole() && peerBodyOwnerCounts_[owner]++ == 0) peerBodyOwnerQueue_.push_back(owner);
        }
        if (seeding && !ctnrs.empty()) tabsSeeded_ = (unsigned int)tabRank_.size();
        return;
    }
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabOwned_.find(ctnrs[i]) != tabOwned_.end()) continue;
        unsigned int rank = tabRankFor(ctnrs[i], ctnrs);
        bool owned;
        const char* why;
        if (seeding) {
            // The save's own tabs, ranked identically on both clients: the
            // historical rule, so nothing about a normal session changes.
            owned = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
            if (!isHostRole() && !owned && !ctnrs.empty() && !ownRanks_.empty() &&
                *ownRanks_.begin() >= ctnrs.size() && i + 1 == ctnrs.size()) owned = true;
            why = "seed";
        } else {
            // A tab that did not exist at session start. Its rank is a local
            // number the peer does not share, so the rank rule cannot answer -
            // ask who authored the bodies standing in it. The pins are still
            // present at this moment (the move/recruit that created the tab is
            // what put them there); capturing the answer HERE is what makes it
            // survive the later EXIT that clears them.
            int verdict = 0;   // +1 ours, -1 peer's, 0 nobody claims it
            bool peerBody = false;
            u32 peerOwner = OWNER_ID_ALL;
            std::vector<Character*> promote;
            for (unsigned int m = 0; m < nSquad; ++m) {
                if (raw[m].hContainer != ctnrs[i].first ||
                    raw[m].hContainerSerial != ctnrs[i].second) continue;
                Key k = keyOf(raw[m]);
                if (pinOwned_.count(k)) { verdict = 1; break; }
                std::map<Key, u32>::const_iterator pk = pinPeer_.find(k);
                if (pk != pinPeer_.end()) {
                    verdict = -1;
                    peerOwner = pk->second;
                }
                // 好友带自己的角色加入：对方流里的队员在本端铸造成代理后，引擎为它新建了玩家
                // 小队标签。铸造的身体只可能来自对方，标签必须判给对方（原先落到
                // host-fallback，主机把好友的角色当成自己的）。
                Character* bc = engine::resolveCharByHand(raw[m].hIndex, raw[m].hSerial,
                                                          raw[m].hType, raw[m].hContainer,
                                                          raw[m].hContainerSerial);
                if (bc && mintedBodies_.count(bc)) {
                    verdict = -1;
                    peerBody = true;
                    std::map<Key, Driven>::const_iterator di = targets_.find(k);
                    if (di != targets_.end()) peerOwner = di->second.owner;
                    promote.push_back(bc);
                }
            }
            if (verdict == -1 && peerBody) {
                // 已成为本端玩家小队成员（会写进存档）：不再当作可丢弃的代理，
                // 对方掉线时 clearPeerReplicationState 不得销毁它
                for (size_t p = 0; p < promote.size(); ++p) mintedBodies_.erase(promote[p]);
                ++peerBodyTabs_;
                if (peerOwner != OWNER_ID_ALL && isHostRole()) {
                    unsigned int& count = peerBodyOwnerCounts_[peerOwner];
                    if (count++ == 0) peerBodyOwnerQueue_.push_back(peerOwner);
                }
            }
            owned = (verdict == 1) ? true
                                   : ((verdict == -1) ? false : isHostRole());
            why = (verdict == 1) ? "pin-own"
                                 : ((verdict == -1) ? (peerBody ? "peer-body" : "pin-peer")
                                                    : "host-fallback");
        }
        tabOwned_[ctnrs[i]] = owned;
        if (!isHostRole() && owned) pendingOwnerTabs_[ctnrs[i]] = 0;
        if (!seeding) {
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[squad] TABOWN cont=%u,%u rank=%u own=%d via=%s",
                      ctnrs[i].first, ctnrs[i].second, rank, owned ? 1 : 0, why);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Freeze the shared-rank prefix at the end of the seeding pass.
    if (seeding && !ctnrs.empty()) tabsSeeded_ = (unsigned int)tabRank_.size();
}

bool Replicator::ownsTab(const std::pair<u32, u32>& key, unsigned int rank) const {
    if (ownersAttached_) {
        Tab tab = canonicalTab(key);
        std::map<Tab, OwnerRow>::const_iterator row = owners_.find(tab);
        if (row != owners_.end()) return row->second.playerId == playerIdentity_;
        return pendingOwnerTabs_.count(key) != 0;
    }
    std::map<std::pair<u32, u32>, bool>::const_iterator it = tabOwned_.find(key);
    if (it != tabOwned_.end()) return it->second;
    // Not yet decided (squadSync_ off, or the first tick of a fresh container):
    // the historical rule, so this is never MORE permissive than before.
    return ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
}

unsigned int Replicator::tabRankFor(const std::pair<u32, u32>& key,
                                    const std::vector<std::pair<u32, u32> >& ctnrs) const {
    if (squadSync_) {
        std::map<std::pair<u32, u32>, unsigned int>::const_iterator it =
            tabRank_.find(key);
        return it == tabRank_.end() ? 0xFFFFFFFFu : it->second;
    }
    return (unsigned int)(std::lower_bound(ctnrs.begin(), ctnrs.end(), key)
                          - ctnrs.begin());
}

void Replicator::logSmoothSummary() {
    float zeroFrac = (activeFrames_ > 0)
                         ? (float)zeroWhileActive_ / (float)activeFrames_
                         : 0.0f;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO SMOOTH active=%lu zeroWhileActive=%lu zeroFrac=%.3f maxStep=%.3f slewSkip=%lu",
              activeFrames_, zeroWhileActive_, zeroFrac, maxStep_, slewSkipFrames_);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // zeroFrac population audit (see the zp* declarations): what the frozen
    // frames actually WERE. Everything but `free` is a body that could not have
    // walked that frame, so it bounds how much of zeroFrac is even addressable.
    char zp[208];
    _snprintf(zp, sizeof(zp) - 1,
              "SCENARIO ZEROPOP zero=%lu down=%lu carried=%lu furn=%lu chain=%lu "
              "crawl=%lu sneak=%lu squadIdle=%lu alias=%lu stall=%lu",
              zeroWhileActive_, zpDown_, zpCarried_, zpFurn_, zpChain_,
              zpCrawl_, zpSneak_, zpSquadIdle_, zpAlias_, zpStall_);
    zp[sizeof(zp) - 1] = '\0';
    coop::logLine(zp);

    // Motion-onset audit (see the onset* declarations): is zeroFrac reporting
    // stutter, or is it reporting how often the body re-entered scoring? Each
    // bucket's fraction is what to read - if early runs far above steady, the
    // metric is charging render catch-up at motion onset, and the low-sample
    // runs fail because bursty motion pays that toll repeatedly.
    float eF = (onActiveEarly_  > 0) ? (float)onZeroEarly_  / (float)onActiveEarly_  : 0.0f;
    float mF = (onActiveMid_    > 0) ? (float)onZeroMid_    / (float)onActiveMid_    : 0.0f;
    float sF = (onActiveSteady_ > 0) ? (float)onZeroSteady_ / (float)onActiveSteady_ : 0.0f;
    char on[240];
    _snprintf(on, sizeof(on) - 1,
              "SCENARIO ONSET reentries=%lu earlyN=%lu earlyZero=%lu earlyFrac=%.3f "
              "midN=%lu midZero=%lu midFrac=%.3f steadyN=%lu steadyZero=%lu steadyFrac=%.3f",
              onsetReentries_, onActiveEarly_, onZeroEarly_, eF,
              onActiveMid_, onZeroMid_, mF,
              onActiveSteady_, onZeroSteady_, sF);
    on[sizeof(on) - 1] = '\0';
    coop::logLine(on);

    // Anim-truth oracle: fraction of translating frames that did NOT report a
    // real walk state. Low == engine is walking the body (Stage 3 goal); high ==
    // the body slides a static pose (the float bug).
    unsigned long floatFrames = (translateFrames_ > walkTruthFrames_)
                                    ? (translateFrames_ - walkTruthFrames_) : 0;
    float floatFrac = (translateFrames_ > 0)
                          ? (float)floatFrames / (float)translateFrames_
                          : 0.0f;
    char a[160];
    _snprintf(a, sizeof(a) - 1,
              "SCENARIO ANIM translate=%lu walkTruth=%lu floatFrac=%.3f",
              translateFrames_, walkTruthFrames_, floatFrac);
    a[sizeof(a) - 1] = '\0';
    coop::logLine(a);

    // March-in-place oracle: of the at-rest frames, fraction where the body played
    // a walk clip while NOT moving. High == "walking on the spot" (the failure the
    // float oracle cannot see, e.g. a host-seated NPC stuck walking on the join).
    float marchFrac = (restSampleFrames_ > 0)
                          ? (float)marchFrames_ / (float)restSampleFrames_
                          : 0.0f;
    // hold/settle/rlps attribute those march frames (see the counter decls). They
    // are appended AFTER marchFrac so Test-MarchInPlace's regex still matches.
    char m[224];
    _snprintf(m, sizeof(m) - 1,
              "SCENARIO MARCH restSamples=%lu march=%lu marchFrac=%.3f "
              "hold=%lu settle=%lu rlps=%lu holdDip=%lu holdStop=%lu",
              restSampleFrames_, marchFrames_, marchFrac,
              marchHold_, marchSettle_, marchRelapse_,
              marchHoldDip_, marchHoldStop_);
    m[sizeof(m) - 1] = '\0';
    coop::logLine(m);

    // Step-2 pruning evidence: how often the legacy quieting patchwork actually
    // fired this run. Sustained relapse=0 across regressions = the I11 re-quiet is
    // dead code under default AI-suspend and can be deleted; detach counts feed the
    // KENSHICOOP_NO_DETACH A/B decision.
    char q[160];
    _snprintf(q, sizeof(q) - 1,
              "SCENARIO QUIET relapse=%lu sitOrders=%lu detach=%lu noDetach=%d "
              "crawlPhys=%lu",
              quietRelapse_, sitOrders_, detachUses_, noDetach_ ? 1 : 0,
              crawlPhysRestore_);
    q[sizeof(q) - 1] = '\0';
    coop::logLine(q);

    // Step-4 evidence: how much of the driven set the divergence gate handed back
    // to local AI. grants>0 = the mechanism engages; the npc_track oracle proves
    // trusted bodies still track.
    if (gateAuthority_) {
        char t[128];
        _snprintf(t, sizeof(t) - 1,
                  "SCENARIO TRUST grants=%lu revokes=%lu", trustGrants_, trustRevokes_);
        t[sizeof(t) - 1] = '\0';
        coop::logLine(t);
    }

    // Step-5 evidence: suppression churn under the hysteresis band (split_interest
    // metric; boundary flip-flops show up as high counts).
    char au[112];
    _snprintf(au, sizeof(au) - 1,
              "SCENARIO AUTH suppresses=%lu restores=%lu", authSuppresses_, authRestores_);
    au[sizeof(au) - 1] = '\0';
    coop::logLine(au);

    // Protocol 36 jumpiness evidence: what regime the interp buffer ran in
    // (extrapFrac = starvation share of all samples) and how often the drive
    // layer had to hard-snap / re-path. Under WAN jitter these are the numbers
    // the interp fixes must move.
    {
        unsigned long total = interpLerp_ + interpSingle_ + interpClampOld_ +
                              interpExtrap_ + interpSegSnap_;
        float extrapFrac = (total > 0)
                               ? (float)(interpExtrap_ + interpClampOld_) / (float)total
                               : 0.0f;
        char ip[240];
        _snprintf(ip, sizeof(ip) - 1,
                  "SCENARIO INTERP samples=%lu lerp=%lu extrap=%lu clamp=%lu seg=%lu "
                  "extrapFrac=%.3f snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu "
                  "restFlip=%lu pruned=%lu",
                  total, interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
                  extrapFrac, hardSnapSquad_, hardSnapNpc_,
                  walkReissueSquad_, walkReissueNpc_, restFlipNpc_, authPruned_);
        ip[sizeof(ip) - 1] = '\0';
        coop::logLine(ip);
    }

    // Step-3 evidence: how many locally-simulated melee hits the damage guard
    // intercepted vs passed through. guarded>0 in a combat scenario proves the
    // hook engaged (complements the blood-flat vitals check).
    if (dmgGuard_) {
        unsigned long guarded = 0, passed = 0;
        engine::damageGuardStats(&guarded, &passed);
        char dg[112];
        _snprintf(dg, sizeof(dg) - 1,
                  "SCENARIO DMGGUARD guarded=%lu passed=%lu", guarded, passed);
        dg[sizeof(dg) - 1] = '\0';
        coop::logLine(dg);
    }
}

// ---- 小队控制授权（协议 1002，第 2 批） ---------------------------------------
// 持久主人与临时控制权分离，读档或断线不会改写主人。
bool Replicator::ownsTabEffective(const std::pair<u32, u32>& tab, unsigned int rank) const {
    Tab canonical = canonicalTab(tab);
    std::map<Tab, AuthPeer>::const_iterator auth = tabAuth_.find(canonical);
    if (auth != tabAuth_.end()) return auth->second.mode == 2;
    std::map<Tab, bool>::const_iterator control = tabControl_.find(canonical);
    if (control != tabControl_.end()) return control->second;
    return ownsTab(tab, rank);
}

// 切换某标签的控制权：改判定 + 清挂起状态 + 清掉会压过它的 per-hand pin。
// 接管（owned）时还要清掉驱动残留，与 rekeyPeerBody 的 CONTROL-FLIP claim 一致：
// canonicalOf_ 按 Character* 记录“正被驱动”，不清掉的话 publishOwned 的驱动排除会让
// 这些角色永远不被本端发布；targets_ / spawnReq_ 是对端流的状态。
// AI 挂起、伤害保护由 applyTargets 每帧重建，不需要手工清理。
void Replicator::reassignTab(GameWorld* gw, const std::pair<u32, u32>& tab, bool owned) {
    Tab key = canonicalTab(tab);
    tabControl_[key] = owned;
    tabAuth_.erase(key);
    tabReq_.erase(key);
    // 只扫本帧的小队名单（captureSquad 自带 SEH；本 TU 不接触 GameWorld 内部结构）。
    static EntityState buf[160]; // 与 publishOwned 的 MAX_PUBLISH 同量级，仅主线程
    unsigned int members = 0;
    if (gw) {
        unsigned int n = engine::captureSquad(gw, false, buf, 160);
        for (unsigned int i = 0; i < n; ++i) {
            if (canonicalTab(Tab(buf[i].hContainer, buf[i].hContainerSerial)) != key) continue;
            Key k = keyOf(buf[i]);
            if (owned) {
                pinPeer_.erase(k);
                Character* c = engine::resolveCharByHand(
                    buf[i].hIndex, buf[i].hSerial, buf[i].hType,
                    buf[i].hContainer, buf[i].hContainerSerial);
                std::map<Character*, Key>::iterator ci =
                    c ? canonicalOf_.find(c) : canonicalOf_.end();
                if (ci != canonicalOf_.end()) {
                    targets_.erase(ci->second);
                    spawnReq_.erase(ci->second);
                    canonicalOf_.erase(ci);
                }
                targets_.erase(k);
                spawnReq_.erase(k);
                // 对端在交接前已发出的批次不再 REQ / 铸代理（同 CONTROL-FLIP claim）
                rekeyedOld_[k] = nowMs();
            } else {
                pinOwned_.erase(k);
            }
            ++members;
        }
    }
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[auth] REASSIGN tab=%u,%u own=%d members=%u",
              tab.first, tab.second, owned ? 1 : 0, members);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void Replicator::sendSquadAuth(NetLink& net, u32 ownerId, u32 targetId,
                               unsigned char kind,
                               const std::pair<u32, u32>& tab) {
    EventPacket ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = (u8)PKT_EVENT;
    ev.event   = EVT_SQUAD_AUTH;
    ev.ownerId = ownerId;
    ev.targetOwnerId = targetId;
    ev.eventId = nextEventId_++;
    Tab canonical = canonicalTab(tab);
    ev.sContainer = canonical.first;
    ev.sContainerSerial = canonical.second;
    ev.arg = (f32)kind;
    net.queueEvent(ev);
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[auth] SEND kind=%u tab=%u,%u target=%u",
              (unsigned)kind, tab.first, tab.second, (unsigned)targetId);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

// 收到 EVT_SQUAD_AUTH。接收半只改状态（拿不到 NetLink），需要回的消息进 tabOutbox_。
void Replicator::applySquadAuth(GameWorld* gw, const EventPacket& ev) {
    if (!squadSync_) return;
    if ((unsigned char)ev.arg == 6) {     // DELETE：发送方删掉了自己的一支空编队（发给所有人）
        Tab gone = canonicalTab(Tab((u32)ev.sContainer, (u32)ev.sContainerSerial), ev.ownerId);
        std::map<Tab, OwnerRow>::iterator row = owners_.find(gone);
        // 主机删自己的编队时当场去掉归属表行，新表可能比这条事件先到（2026-11-13 实测），
        // 所以好友一侧查不到行也照做；主机一侧必须查到行且主人是发送方
        if (ev.ownerId == localId_ ||
            (row != owners_.end() ? row->second.playerId != identityForOwner(ev.ownerId) : isHostRole())) return;
        int result = engine::destroyEmptyPlayerSquad(gw, gone.first, gone.second);
        if (isHostRole() && result != 2) { owners_.erase(row); ++ownersRevision_; ownersDirty_ = true; }
        char d[144];
        _snprintf(d, sizeof(d) - 1, "[owners] squad deleted by owner=%u tab=%u,%u result=%d",
                  (unsigned)ev.ownerId, gone.first, gone.second, result);
        d[sizeof(d) - 1] = '\0'; coop::logLine(d);
        return;
    }
    if (ev.targetOwnerId != localId_) return;
    unsigned char kind = (unsigned char)ev.arg;
    std::pair<u32, u32> tab((u32)ev.sContainer, (u32)ev.sContainerSerial);
    if (tab.first == 0 && tab.second == 0) return;
    unsigned int rank = 0xFFFFFFFFu;
    std::map<std::pair<u32, u32>, unsigned int>::const_iterator rt = tabRank_.find(tab);
    if (rt != tabRank_.end()) rank = rt->second;
    bool mine = ownsTabEffective(tab, rank);
    char b[192];
    _snprintf(b, sizeof(b) - 1,
              "[auth] RECV kind=%u from=%u target=%u tab=%u,%u mine=%d rank=%u",
              (unsigned)kind, (unsigned)ev.ownerId, (unsigned)ev.targetOwnerId,
              tab.first, tab.second, mine ? 1 : 0, rank);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    if (kind == 1) {                      // REQUEST：对方申请控制这个标签
        std::map<Tab, AuthPeer>::const_iterator pending = tabReq_.find(tab);
        bool available = pending == tabReq_.end() ||
            (pending->second.mode == 2 && pending->second.ownerId == ev.ownerId);
        if (mine && available && (!ownersAttached_ || tabOwnerIdentity(tab) == playerIdentity_)) {
            tabReq_[tab].ownerId = ev.ownerId;
            tabReq_[tab].mode = 2;
            tabReq_[tab].ms = nowMs();
            tabNotice_ = 5;               // 状态栏提示“好友申请控制你的小队”
        } else {
            // 该标签不归本端（刚交接完 / 未知标签）：直接拒绝，保持两端状态明确
            SquadAuthMsg m; m.kind = 3; m.targetId = ev.ownerId;
            m.c = tab.first; m.cs = tab.second;
            tabOutbox_.push_back(m);
        }
        return;
    }
    if (kind == 2) {                      // GRANT：对方同意，控制权转到本端
        std::map<std::pair<u32, u32>, AuthPeer>::iterator rq = tabReq_.find(tab);
        if (mine) return;                 // 已经拥有（重复投递）：忽略
        if (rq == tabReq_.end() || rq->second.mode != 1 ||
            rq->second.ownerId != ev.ownerId) {
            // 申请已超时（或没向它申请过）：同意方已先停发，回 RETURN 让它收回，
            // 否则该小队两边都不驱动，且同意方之后的 [收回] 也得不到确认
            SquadAuthMsg m; m.kind = 5; m.targetId = ev.ownerId;
            m.c = tab.first; m.cs = tab.second;
            tabOutbox_.push_back(m);
            coop::logLine("[auth] late GRANT without pending request -> RETURN");
            return;
        }
        tabReq_.erase(rq);
        reassignTab(gw, tab, true);
        tabAuth_[tab].ownerId = ev.ownerId;
        tabAuth_[tab].mode = 2;           // 记：ownerId 授权给本端（可 [归还]）
        tabNotice_ = 3;
        return;
    }
    if (kind == 3) {                      // DENY：申请被拒绝
        std::map<std::pair<u32, u32>, AuthPeer>::iterator rq = tabReq_.find(tab);
        if (rq == tabReq_.end() || rq->second.mode != 1 ||
            rq->second.ownerId != ev.ownerId) return;
        tabReq_.erase(rq);
        tabNotice_ = 2;
        return;
    }
    if (kind == 4) {                      // REVOKE：对方收回，要求本端停发并确认
        std::map<std::pair<u32, u32>, AuthPeer>::iterator at = tabAuth_.find(tab);
        if (!mine || at == tabAuth_.end() || at->second.mode != 2 ||
            at->second.ownerId != ev.ownerId) return;
        reassignTab(gw, tab, false);      // 停发
        SquadAuthMsg m; m.kind = 5; m.targetId = ev.ownerId;
        m.c = tab.first; m.cs = tab.second;
        tabOutbox_.push_back(m);          // 回 RETURN 确认
        tabNotice_ = 4;
        return;
    }
    if (kind == 5) {                      // RETURN：对方归还 / 收回确认
        std::map<std::pair<u32, u32>, AuthPeer>::iterator at = tabAuth_.find(tab);
        if (at == tabAuth_.end() || at->second.mode != 1 ||
            at->second.ownerId != ev.ownerId) return;
        reassignTab(gw, tab, true);       // 收回生效：本端重新拥有
        tabNotice_ = 4;
        return;
    }
}

// 每帧调用：发出发件箱，然后处理挂起申请的超时（两端都清，避免状态悬空）。
void Replicator::squadAuthTick(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (!squadSync_) return;
    if (!tabOutbox_.empty()) {
        std::vector<SquadAuthMsg> out;
        out.swap(tabOutbox_);
        for (unsigned int i = 0; i < out.size(); ++i) {
            std::pair<u32, u32> tab(out[i].c, out[i].cs);
            sendSquadAuth(net, ownerId, out[i].targetId, out[i].kind, tab);
        }
    }
    unsigned long now = nowMs();
    unsigned int expired = 0;
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator it = tabReq_.begin();
         it != tabReq_.end(); ) {
        if (it->second.ms != 0 &&
            (now - it->second.ms) > (unsigned long)TAB_AUTH_TIMEOUT_MS) {
            it = tabReq_.erase(it);
            ++expired;
        } else {
            ++it;
        }
    }
    if (expired > 0) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[auth] TIMEOUT pending=%u",
                  (unsigned)tabReq_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        tabNotice_ = 1;
    }
}

// 面板动作：按永久主人及可选标签申请，默认仍处理全部可申请编队。
void Replicator::requestSquadAuth(NetLink& net, u32 ownerId, u64 targetPlayer,
                                  const std::pair<u32, u32>* singleTab) {
    (void)net; (void)ownerId;
    if (!squadSync_ || !sessionLive_) return;
    std::set<Tab> candidates;
    if (ownersAttached_) {
        for (std::map<Tab, OwnerRow>::const_iterator it = owners_.begin(); it != owners_.end(); ++it)
            candidates.insert(it->first);
    } else {
        for (std::map<Tab, unsigned int>::const_iterator it = tabRank_.begin(); it != tabRank_.end(); ++it)
            candidates.insert(canonicalTab(it->first));
    }
    unsigned long now = nowMs();
    for (std::set<Tab>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
        Tab tab = *it;
        if (singleTab && tab != canonicalTab(*singleTab)) continue;
        SquadAuthView view;
        squadAuthView(tab, view);
        if (!view.requestable) continue;
        u32 target = driverOwnerForTab(tab);
        u64 identity = ownersAttached_ ? tabOwnerIdentity(tab) : identityForOwner(target);
        if (targetPlayer && identity != targetPlayer) continue;
        tabReq_[tab].ownerId = target;
        tabReq_[tab].mode = 1;
        tabReq_[tab].ms = now;
        SquadAuthMsg m; m.kind = 1; m.targetId = target;
        m.c = tab.first; m.cs = tab.second;
        tabOutbox_.push_back(m);
    }
}

// 面板 [同意]：先把本侧停发（归属翻成对方，命令拦截同帧生效），再回 GRANT。
void Replicator::grantSquadAuth(GameWorld* gw, NetLink& net, u32 ownerId, u64 targetPlayer,
                                  const std::pair<u32, u32>* singleTab) {
    (void)net; (void)ownerId;
    if (!squadSync_ || !sessionLive_) return;
    unsigned long now = nowMs();
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator it = tabReq_.begin();
         it != tabReq_.end(); ) {
        if (it->second.mode != 2 || (targetPlayer && identityForOwner(it->second.ownerId) != targetPlayer) ||
            (singleTab && it->first != canonicalTab(*singleTab)) ||
            (it->second.ms && now - it->second.ms > (unsigned long)TAB_AUTH_TIMEOUT_MS) ||
            (ownersAttached_ && (ownersInvalid_ || tabOwnerIdentity(it->first) != playerIdentity_)) ||
            !ownsTabEffective(it->first, 0xFFFFFFFFu)) { ++it; continue; }
        std::pair<u32, u32> tab = it->first;
        u32 target = it->second.ownerId;
        tabReq_.erase(it++);
        reassignTab(gw, tab, false);      // 先停发
        tabAuth_[tab].ownerId = target;
        tabAuth_[tab].mode = 1;           // 记：控制权已交给对方
        SquadAuthMsg m; m.kind = 2; m.targetId = target;
        m.c = tab.first; m.cs = tab.second;
        tabOutbox_.push_back(m);
        tabNotice_ = 3;
    }
}

// 面板 [拒绝]：清挂起并回 DENY。
void Replicator::denySquadAuth(NetLink& net, u32 ownerId, u64 targetPlayer,
                                  const std::pair<u32, u32>* singleTab) {
    (void)net; (void)ownerId;
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator it = tabReq_.begin();
         it != tabReq_.end(); ) {
        if (it->second.mode != 2 || (targetPlayer && identityForOwner(it->second.ownerId) != targetPlayer) ||
            (singleTab && it->first != canonicalTab(*singleTab))) { ++it; continue; }
        std::pair<u32, u32> tab = it->first;
        u32 target = it->second.ownerId;
        tabReq_.erase(it++);
        SquadAuthMsg m; m.kind = 3; m.targetId = target;
        m.c = tab.first; m.cs = tab.second;
        tabOutbox_.push_back(m);
    }
}

// 面板 [收回控制权] / [归还控制权]。授权出去的标签：发 REVOKE 并记“收回中”，等对方
// RETURN 后才真正接管（先停发再接管，避免两端同时发布的窗口，见设计说明第 5 节）。
// 借到的标签：本端先停发，再发 RETURN，对方收到后接管。
void Replicator::revokeSquadAuth(GameWorld* gw, NetLink& net, u32 ownerId, u64 targetPlayer,
                                  const std::pair<u32, u32>* singleTab) {
    (void)net; (void)ownerId;
    std::vector<std::pair<u32, u32> > borrowed;
    unsigned long now = nowMs();
    for (std::map<std::pair<u32, u32>, AuthPeer>::iterator it = tabAuth_.begin();
         it != tabAuth_.end(); ++it) {
        if ((targetPlayer && identityForOwner(it->second.ownerId) != targetPlayer) ||
            (singleTab && it->first != canonicalTab(*singleTab))) continue;
        if (it->second.mode == 2) { borrowed.push_back(it->first); continue; }
        tabReq_[it->first].ownerId = it->second.ownerId;
        tabReq_[it->first].mode = 3;
        tabReq_[it->first].ms = now;
        SquadAuthMsg m; m.kind = 4; m.targetId = it->second.ownerId;
        m.c = it->first.first; m.cs = it->first.second;
        tabOutbox_.push_back(m);
    }
    for (unsigned int i = 0; i < borrowed.size(); ++i) {
        std::map<std::pair<u32, u32>, AuthPeer>::iterator at = tabAuth_.find(borrowed[i]);
        if (at == tabAuth_.end() || at->second.mode != 2) continue;
        u32 target = at->second.ownerId;
        reassignTab(gw, borrowed[i], false); // 先停发（同时清掉 tabAuth_ 记录）
        SquadAuthMsg m; m.kind = 5; m.targetId = target;
        m.c = borrowed[i].first; m.cs = borrowed[i].second;
        tabOutbox_.push_back(m);
        tabNotice_ = 4;
    }
}

int Replicator::squadAuthPendingPeer() const {
    for (std::map<std::pair<u32, u32>, AuthPeer>::const_iterator it = tabReq_.begin();
         it != tabReq_.end(); ++it)
        if (it->second.mode == 2) return 1;
    return 0;
}

int Replicator::squadAuthGrantedMine() const {
    int n = 0;
    for (std::map<std::pair<u32, u32>, AuthPeer>::const_iterator it = tabAuth_.begin();
         it != tabAuth_.end(); ++it)
        if (it->second.mode == 1) ++n;
    return n;
}

int Replicator::squadAuthBorrowed() const {
    int n = 0;
    for (std::map<std::pair<u32, u32>, AuthPeer>::const_iterator it = tabAuth_.begin();
         it != tabAuth_.end(); ++it)
        if (it->second.mode == 2) ++n;
    return n;
}

int Replicator::squadAuthRequesting() const {
    for (std::map<std::pair<u32, u32>, AuthPeer>::const_iterator it = tabReq_.begin();
         it != tabReq_.end(); ++it)
        if (it->second.mode == 1 || it->second.mode == 3) return 1;
    return 0;
}

bool Replicator::squadAuthHasRequestable() const {
    if (ownersAttached_) {
        for (std::map<Tab, OwnerRow>::const_iterator it = owners_.begin(); it != owners_.end(); ++it) {
            SquadAuthView view;
            squadAuthView(it->first, view);
            if (view.requestable) return true;
        }
    } else {
        for (std::map<Tab, unsigned int>::const_iterator it = tabRank_.begin(); it != tabRank_.end(); ++it) {
            SquadAuthView view;
            squadAuthView(it->first, view);
            if (view.requestable) return true;
        }
    }
    return false;
}

unsigned int Replicator::takeSquadAuthNotice() {
    unsigned int n = tabNotice_;
    tabNotice_ = 0;
    return n;
}

u32 Replicator::driverOwnerForTab(const std::pair<u32, u32>& tab) const {
    Tab canonical = canonicalTab(tab);
    std::map<Tab, AuthPeer>::const_iterator auth = tabAuth_.find(canonical);
    if (auth != tabAuth_.end() && auth->second.mode == 1) return auth->second.ownerId;
    if (ownersAttached_) return ownerForIdentity(tabOwnerIdentity(tab));
    unsigned long now = nowMs();
    for (std::map<Key, Driven>::const_iterator it = targets_.begin(); it != targets_.end(); ++it)
        if (it->first.c == tab.first && it->first.cs == tab.second && peerStreamFresh(it->first, now))
            return it->second.owner;
    return OWNER_ID_ALL;
}

int Replicator::orderClassForHand(const unsigned int h[5]) const {
    if (!sessionLive_) return 0;
    Tab tab(h[1], h[2]);
    Key local; local.t = h[0]; local.c = h[1]; local.cs = h[2]; local.i = h[3]; local.s = h[4];
    if (ownersAttached_ && (owners_.count(canonicalTab(tab)) || allSquad_.count(local)))
        return ownsTabEffective(tab, 0xFFFFFFFFu) && squadBarVisible(tab) ? 1 : 2; // 借用期间自己的编队也锁
    int cls = ownerClassForHand(h);
    if (cls != 2) return cls;
    Key k; k.t = h[0]; k.c = h[1]; k.cs = h[2]; k.i = h[3]; k.s = h[4];
    return peerStreamFresh(k, nowMs()) || peerTabLocked(tab) ? 2 : 0;
}

bool Replicator::peerTabLocked(const std::pair<u32, u32>& tab) const {
    if (!sessionLive_) return false;
    std::map<Tab, unsigned int>::const_iterator rank = tabRank_.find(tab);
    return !ownsTabEffective(tab, rank == tabRank_.end() ? 0xFFFFFFFFu : rank->second);
}

bool Replicator::takePeerBodyTabOwner(u32* ownerId) {
    if (!ownerId || peerBodyOwnerQueue_.empty()) return false;
    *ownerId = peerBodyOwnerQueue_.front();
    peerBodyOwnerQueue_.erase(peerBodyOwnerQueue_.begin());
    return true;
}

void Replicator::setPlayerIdentity(u64 id) {
    bool keepOwners = ownersKeepOnIdentity_ && isHostRole() && playerIdentity_ == id;
    ownersKeepOnIdentity_ = false;
    if (playerIdentity_ != id) {
        players_.clear();
        wallets_.clear(); walletsLoaded_ = false;
    }
    ownersDirty_ = true;
    if (!keepOwners) {
        ownersLoaded_ = false; ownersAttached_ = false; ownersInvalid_ = false;
        ownersHaveSnapshot_ = false; ownersFileWorld_ = 0;
        owners_.clear();
    }
    tabAliases_.clear(); pendingOwnerTabs_.clear();
    playerIdentity_ = id;
    for (std::map<u64, PeerStatusPacket>::iterator it = players_.begin(); it != players_.end(); ++it)
        it->second.present = 0;
}

void Replicator::ingestPlayers(Inbound& in) {
    std::deque<PeerStatusPacket> got;
    in.drainPeerStatus(got);
    for (std::deque<PeerStatusPacket>::const_iterator it = got.begin(); it != got.end(); ++it) {
        for (std::map<u64, PeerStatusPacket>::iterator old = players_.begin(); old != players_.end(); ++old)
            if (old->second.ownerId == it->ownerId) old->second.present = 0;
        players_[it->playerId] = *it;
        if (isHostRole()) ownersDirty_ = true;
        char b[224];
        _snprintf(b, sizeof(b) - 1, "[owners] player=%llu owner=%u rank=%u online=%u name='%s'",
                  it->playerId, it->ownerId, it->ownRank, it->present, it->name);
        b[sizeof(b) - 1] = 0; coop::logLine(b);
    }
}

u64 Replicator::identityForOwner(u32 owner) const {
    if (owner == localId_) return playerIdentity_;
    for (std::map<u64, PeerStatusPacket>::const_iterator it = players_.begin(); it != players_.end(); ++it)
        if (it->second.present && it->second.ownerId == owner) return it->first;
    return 0;
}

u32 Replicator::ownerForIdentity(u64 identity) const {
    if (!identity) return OWNER_ID_ALL;
    if (identity == playerIdentity_) return localId_;
    std::map<u64, PeerStatusPacket>::const_iterator it = players_.find(identity);
    return it != players_.end() && it->second.present ? it->second.ownerId : OWNER_ID_ALL;
}

Replicator::Tab Replicator::canonicalTab(const Tab& tab, u32 author) const {
    std::map<Tab, Tab>::const_iterator alias = tabAliases_.find(tab);
    if (author == OWNER_ID_ALL && alias != tabAliases_.end()) return alias->second;
    u64 identity = author == OWNER_ID_ALL ? playerIdentity_ : identityForOwner(author);
    std::map<Tab, OwnerRow>::const_iterator exact = owners_.find(tab);
    if (exact != owners_.end() && (exact->second.playerId == identity || (author == OWNER_ID_ALL && ownersAttached_))) return tab;
    for (std::map<Tab, OwnerRow>::const_iterator it = owners_.begin(); it != owners_.end(); ++it)
        if (it->second.sourceActive && it->second.playerId == identity && it->second.sourceContainer == tab.first &&
            it->second.sourceSerial == tab.second) return it->first;
    return tab;
}

u64 Replicator::tabOwnerIdentity(const Tab& tab) const {
    std::map<Tab, OwnerRow>::const_iterator row = owners_.find(canonicalTab(tab));
    return row == owners_.end() ? 0 : row->second.playerId;
}

void Replicator::claimLocalTab(const Tab& tab) {
    if (!sessionLive_ || !(tab.first | tab.second)) return;
    Tab canonical = canonicalTab(tab);
    std::map<Tab, OwnerRow>::const_iterator row = owners_.find(canonical);
    if (ownersAttached_ && row != owners_.end()) return;
    pendingOwnerTabs_[tab] = 0;
    if (isHostRole() && ownersAttached_ && owners_.size() < OWNERS_MAX) {
        OwnerRow value;
        value.container = tab.first; value.containerSerial = tab.second;
        value.playerId = playerIdentity_;
        value.sourceContainer = tab.first; value.sourceSerial = tab.second; value.sourceActive = 0;
        owners_[tab] = value; ++ownersRevision_; ownersDirty_ = true;
        pendingOwnerTabs_.erase(tab);
    }
}

Replicator::Tab Replicator::registerPeerTab(GameWorld* gw, u32 author, const Tab& source, bool foreign) {
    u64 identity = identityForOwner(author);
    if (!identity || !isHostRole() || owners_.size() >= OWNERS_MAX) return Tab(0, 0);
    Tab canonical = canonicalTab(source, author);
    if (foreign) {
        for (std::map<Tab, OwnerRow>::iterator row = owners_.begin(); row != owners_.end(); ++row)
            if (row->second.playerId == identity && row->second.sourceContainer == source.first &&
                row->second.sourceSerial == source.second) {
                if (!row->second.sourceActive) { row->second.sourceActive = 1; ++ownersRevision_; ownersDirty_ = true; }
                return row->first;
            }
    }
    std::map<Tab, OwnerRow>::const_iterator known = owners_.find(canonical);
    if (known != owners_.end() && (!foreign || known->second.playerId == identity)) return canonical;
    unsigned int created[2] = { 0, 0 };
    if (!engine::createPlayerSquad(gw, created)) return Tab(0, 0);
    canonical = Tab(created[0], created[1]);
    if (owners_.count(canonical)) return Tab(0, 0);
    OwnerRow row;
    row.container = canonical.first; row.containerSerial = canonical.second;
    row.playerId = identity; row.sourceContainer = source.first; row.sourceSerial = source.second; row.sourceActive = 1;
    owners_[canonical] = row; ++ownersRevision_; ownersDirty_ = true;
    char b[192];
    _snprintf(b, sizeof(b) - 1, "[owners] registered player=%llu source=%u,%u tab=%u,%u",
              identity, source.first, source.second, canonical.first, canonical.second);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
    return canonical;
}

bool Replicator::readOwners(const std::string& name) {
    std::string path = savexfer::saveFolderFor(name) + "/coop_owners.txt";
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, 0, 0);
    if (n <= 0) return false;
    std::vector<wchar_t> wide(n);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n);
    FILE* f = _wfopen(&wide[0], L"rb");
    if (!f) return false;
    char line[1024]; unsigned int version = 0, revision = 0;
    u64 world = 0;
    bool valid = fgets(line, sizeof(line), f) &&
                 sscanf(line, "KCOOP_OWNERS %u %I64u %u", &version, &world, &revision) == 3 && version == 1 && world != 0;
    std::map<Tab, OwnerRow> rows;
    while (valid && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        if (line[0] == 'P') {
            PeerStatusPacket p; memset(&p, 0, sizeof(p)); p.ownerId = OWNER_ID_ALL;
            char hex[129] = { 0 };
            if (sscanf(line, "P %I64u %u %128s", &p.playerId, &p.ownRank, hex) >= 2 && p.playerId) {
                for (unsigned int i = 0; i < 63 && hex[i * 2] && hex[i * 2 + 1]; ++i) {
                    unsigned int byte = 0;
                    if (sscanf(hex + i * 2, "%2x", &byte) != 1) break;
                    p.name[i] = (char)byte;
                }
                if (!players_.count(p.playerId)) players_[p.playerId] = p;
            }
            continue;
        }
        OwnerRow row = { 0 };
        valid = sscanf(line, "T %u %u %I64u %u %u", &row.container, &row.containerSerial,
                       &row.playerId, &row.sourceContainer, &row.sourceSerial) == 5 && row.playerId != 0 &&
                (row.container | row.containerSerial) != 0 && rows.size() < OWNERS_MAX;
        Tab tab(row.container, row.containerSerial);
        if (valid && !rows.count(tab)) rows[tab] = row;
        else valid = false;
    }
    if (ferror(f)) valid = false;
    fclose(f);
    if (!valid) {
        owners_.clear(); ownersInvalid_ = true; ownersAttached_ = true;
        coop::logErrLine("[owners] invalid ownership file; ownership migration blocked");
        return true;
    }
    ownersFileWorld_ = world;
    if (isHostRole() || !ownersHaveSnapshot_) {
        ownersWorld_ = world; ownersRevision_ = revision; owners_.swap(rows);
    }
    ownersAttached_ = isHostRole() || (ownersHaveSnapshot_ && ownersWorld_ == ownersFileWorld_);
    if (isHostRole()) ownersDirty_ = true;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[owners] restored world=%llu rows=%u save='%s' attached=%d",
              world, (unsigned)owners_.size(), name.c_str(), ownersAttached_ ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
    return true;
}

bool Replicator::writeOwners(const std::string& name) const {
    if (!isHostRole() || !ownersAttached_ || !ownersWorld_ || ownersInvalid_) return false;
    std::string path = savexfer::saveFolderFor(name) + "/coop_owners.txt";
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, 0, 0);
    if (n <= 0) return false;
    std::vector<wchar_t> wide(n);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n);
    std::wstring target(&wide[0]); std::wstring temp = target + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"wb");
    if (!f) { coop::logErrLine("[owners] ownership file could not be opened for writing"); return false; }
    fprintf(f, "KCOOP_OWNERS 1 %I64u %u\n", ownersWorld_, ownersRevision_);
    for (std::map<u64, PeerStatusPacket>::const_iterator it = players_.begin(); it != players_.end(); ++it) {
        fprintf(f, "P %I64u %u ", it->first, it->second.ownRank);
        for (unsigned int i = 0; i < 63 && it->second.name[i]; ++i) fprintf(f, "%02x", (unsigned char)it->second.name[i]);
        fprintf(f, "\n");
    }
    for (std::map<Tab, OwnerRow>::const_iterator it = owners_.begin(); it != owners_.end(); ++it)
        fprintf(f, "T %u %u %I64u %u %u\n", it->first.first, it->first.second,
                it->second.playerId, it->second.sourceContainer, it->second.sourceSerial);
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[owners] saved rows=%u save='%s' ok=%d", (unsigned)owners_.size(), name.c_str(), ok ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
    return ok;
}

void Replicator::resetOwnersForWorld() {
    tabAliases_.clear(); pendingOwnerTabs_.clear();
    ownersResetPending_ = true;
    if (!ownersKeepOnIdentity_) {
        ownersLoaded_ = false; ownersAttached_ = false; ownersInvalid_ = false;
        ownersFileWorld_ = 0;
    }
    if (isHostRole()) {
        if (ownersKeepOnIdentity_) {
            for (std::map<Tab, OwnerRow>::iterator row = owners_.begin(); row != owners_.end(); ++row)
                row->second.sourceActive = 0;
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[owners] kept across reconnect rows=%u world=%llu",
                (unsigned)owners_.size(), ownersWorld_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else {
            owners_.clear(); ownersWorld_ = 0; ownersHaveSnapshot_ = false;
        }
        ++ownersAuthEpoch_; characterWorldEpoch_ = ownersAuthEpoch_; ownersDirty_ = true;
    } else {
        // 加入方没有自己的纪元：新会话可能连到纪元更低的主机（同一存档重开主机），
        // 旧纪元/版本号会让新快照一直被当成过期丢掉，这里回到初始值让首份快照必定被接受
        ownersAuthEpoch_ = 1; ownersRevision_ = 0; characterWorldEpoch_ = 1;
    }
}

void Replicator::syncOwners(GameWorld* gw, NetLink& net, Inbound& in, bool host) {
    std::deque<InboundOwners> snapshots;
    in.drainOwners(snapshots);
    for (std::deque<InboundOwners>::const_iterator it = snapshots.begin(); !host && it != snapshots.end(); ++it) {
        if (ownersHaveSnapshot_ && it->header.worldId == ownersWorld_) {
            if (it->header.authEpoch < ownersAuthEpoch_) continue;
            if (it->header.authEpoch == ownersAuthEpoch_ && it->header.revision < ownersRevision_) continue;
        }
        std::map<Tab, OwnerRow> rows;
        bool valid = it->rows.size() <= OWNERS_MAX;
        for (size_t i = 0; valid && i < it->rows.size(); ++i) {
            const OwnerRow& row = it->rows[i]; Tab key(row.container, row.containerSerial);
            valid = row.playerId != 0 && row.sourceActive <= 1 && (key.first | key.second) != 0 && !rows.count(key);
            if (valid) rows[key] = row;
        }
        if (!valid) { coop::logErrLine("[owners] rejected invalid ownership snapshot"); continue; }
        if (ownersWorld_ != it->header.worldId || ownersAuthEpoch_ != it->header.authEpoch) {
            std::vector<Tab> restore;
            for (std::map<Tab, bool>::const_iterator c = tabControl_.begin(); c != tabControl_.end(); ++c)
                restore.push_back(c->first);
            for (size_t c = 0; c < restore.size(); ++c)
                reassignTab(gw, restore[c], tabOwnerIdentity(restore[c]) == playerIdentity_);
            tabControl_.clear(); tabAuth_.clear(); tabReq_.clear(); tabOutbox_.clear();
        }
        ownersWorld_ = it->header.worldId; ownersRevision_ = it->header.revision;
        ownersAuthEpoch_ = it->header.authEpoch;
        // 加入方的开局请求下限跟随主机纪元；本进程之前当过主机时留下的旧值会比主机纪元大，请求会被自己拦下
        characterWorldEpoch_ = ownersAuthEpoch_;
        owners_.swap(rows); ownersHaveSnapshot_ = true;
        bool attached = ownersFileWorld_ != 0 && ownersFileWorld_ == ownersWorld_;
        if (attached && !ownersAttached_) tabOwned_.clear();
        ownersAttached_ = attached;
        for (std::map<Tab, unsigned long>::iterator p = pendingOwnerTabs_.begin(); p != pendingOwnerTabs_.end(); ) {
            Tab key = canonicalTab(p->first);
            if (owners_.count(key) && owners_[key].playerId == playerIdentity_) {
                tabAliases_[p->first] = key; pendingOwnerTabs_.erase(p++);
            } else ++p;
        }
        char b[144];
        _snprintf(b, sizeof(b) - 1, "[owners] snapshot world=%llu revision=%u epoch=%u rows=%u attached=%d",
                  ownersWorld_, ownersRevision_, ownersAuthEpoch_, (unsigned)owners_.size(), ownersAttached_ ? 1 : 0);
        b[sizeof(b) - 1] = 0; coop::logLine(b);
    }
    if (!ownersLoaded_) {
        char name[64] = { 0 }; engine::saveInfo(name, sizeof(name), 0, 0);
        bool found = name[0] && readOwners(name);
        ownersLoaded_ = true;
        if (host && !walletsLoaded_) {
            if (found) readPlayers(name);
            walletsLoaded_ = true;
        }
        if (host && !found) {
            FILETIME time; GetSystemTimeAsFileTime(&time);
            ownersWorld_ = ((u64)time.dwHighDateTime << 32 | time.dwLowDateTime) ^ playerIdentity_;
            if (!ownersWorld_) ownersWorld_ = 1;
            owners_.clear(); ownersRevision_ = 1; ownersAttached_ = true; ownersDirty_ = true;
            static EntityState legacy[160];
            unsigned int n = engine::captureSquad(gw, false, legacy, 160);
            unsigned int dead[2] = { 0, 0 };
            engine::readDeadSquadTab(gw, dead);
            std::vector<Tab> sorted;
            for (unsigned int i = 0; i < n; ++i)
                if (legacy[i].hContainer != dead[0] || legacy[i].hContainerSerial != dead[1])
                    sorted.push_back(Tab(legacy[i].hContainer, legacy[i].hContainerSerial));
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            for (unsigned int i = 0; i < sorted.size(); ++i) {
                OwnerRow row; row.container = sorted[i].first; row.containerSerial = sorted[i].second;
                row.playerId = playerIdentity_; row.sourceContainer = row.container; row.sourceSerial = row.containerSerial; row.sourceActive = 0;
                for (std::map<u64, PeerStatusPacket>::const_iterator p = players_.begin(); p != players_.end(); ++p)
                    if (p->second.present && p->second.ownRank == i && i != 0) { row.playerId = p->first; break; }
                owners_[sorted[i]] = row;
                char b[160];
                _snprintf(b, sizeof(b) - 1, "[owners] legacy seed tab=%u,%u rank=%u player=%llu",
                          row.container, row.containerSerial, i, row.playerId);
                b[sizeof(b) - 1] = 0; coop::logLine(b);
            }
        }
    }
    if (ownersInvalid_) return;
    for (std::map<Key, u32>::iterator p = pendingPeerMembers_.begin(); p != pendingPeerMembers_.end(); ) {
        Tab tab = canonicalTab(Tab(p->first.c, p->first.cs), p->second);
        if (!owners_.count(tab)) { ++p; continue; }
        Key key = p->first; u32 author = p->second;
        pendingPeerMembers_.erase(p++);
        std::map<Key, Character*>::iterator proxy = proxyByKey_.find(key);
        Character* c = proxy != proxyByKey_.end() ? proxy->second :
            engine::resolveCharByHand(key.i, key.s, key.t, key.c, key.cs);
        if (c) insertPeerMember(gw, author, c, key, "recruit", ownsTabEffective(tab, 0xFFFFFFFFu));
    }
    std::deque<OwnerRequestPacket> requests;
    in.drainOwnerRequests(requests);
    for (std::deque<OwnerRequestPacket>::const_iterator it = requests.begin(); host && it != requests.end(); ++it) {
        if (!identityForOwner(it->ownerId)) continue;
        if (!(it->container | it->containerSerial)) {
            std::vector<Tab> restore;
            for (std::map<Tab, bool>::const_iterator c = tabControl_.begin(); c != tabControl_.end(); ++c)
                restore.push_back(c->first);
            for (size_t c = 0; c < restore.size(); ++c)
                reassignTab(gw, restore[c], tabOwnerIdentity(restore[c]) == playerIdentity_);
            tabAuth_.clear(); tabReq_.clear(); tabControl_.clear(); tabOutbox_.clear();
            for (std::map<Tab, OwnerRow>::iterator row = owners_.begin(); row != owners_.end(); ++row)
                if (it->worldId == ownersWorld_ && row->second.playerId == identityForOwner(it->ownerId)) row->second.sourceActive = 0;
            ++ownersRevision_; ++ownersAuthEpoch_; ownersDirty_ = true; continue;
        }
        if (it->worldId && it->worldId != ownersWorld_) { ownersDirty_ = true; continue; }
        registerPeerTab(gw, it->ownerId, Tab(it->container, it->containerSerial), it->worldId == 0);
        ownersDirty_ = true;
    }
    // 身体可能先于整表到达；表落地后补登记，避免它在断线清理中仍被当作临时代理。
    for (std::map<Key, Character*>::const_iterator p = proxyByKey_.begin(); p != proxyByKey_.end(); ++p) {
        if (!p->second || !mintedBodies_.count(p->second)) continue;
        std::map<Key, Driven>::const_iterator stream = targets_.find(p->first);
        if (stream == targets_.end()) continue;
        u32 author = stream->second.owner;
        Tab tab = canonicalTab(Tab(p->first.c, p->first.cs), author);
        std::map<Tab, OwnerRow>::const_iterator owner = owners_.find(tab);
        if (owner != owners_.end() && owner->second.playerId == identityForOwner(author))
            insertPeerMember(gw, author, p->second, p->first, "recruit", false);
    }
    if (host) {
        // 旧版本把阵亡编队当成未知编队登记给了主机，这里去掉（listPlayerTabs 已不再列出它）
        unsigned int dead[2] = { 0, 0 };
        if (engine::readDeadSquadTab(gw, dead) && owners_.erase(Tab(dead[0], dead[1]))) {
            ++ownersRevision_; ownersDirty_ = true;
            char b[144];
            _snprintf(b, sizeof(b) - 1, "[owners] dead squad tab=%u,%u removed", dead[0], dead[1]);
            b[sizeof(b) - 1] = 0; coop::logLine(b);
        }
        unsigned int tabs[OWNERS_MAX][2]; unsigned int n = engine::listPlayerTabs(gw, tabs, OWNERS_MAX);
        for (unsigned int i = 0; i < n; ++i) {
            Tab tab(tabs[i][0], tabs[i][1]);
            if (owners_.count(tab) || owners_.size() >= OWNERS_MAX) continue;
            claimLocalTab(tab);
            char b[144];
            _snprintf(b, sizeof(b) - 1, "[owners] unknown tab=%u,%u -> host fallback", tab.first, tab.second);
            b[sizeof(b) - 1] = 0; coop::logLine(b);
        }
        if (ownersDirty_) {
            std::vector<OwnerRow> rows;
            for (std::map<Tab, OwnerRow>::const_iterator it = owners_.begin(); it != owners_.end(); ++it) rows.push_back(it->second);
            net.queueOwners(ownersWorld_, ownersRevision_, ownersAuthEpoch_, rows); ownersDirty_ = false;
        }
    } else {
        if (ownersResetPending_ && ownersHaveSnapshot_) {
            OwnerRequestPacket p; memset(&p, 0, sizeof(p));
            p.type = (u8)PKT_OWNER_REQUEST; p.ownerId = localId_; p.worldId = ownersAttached_ ? ownersWorld_ : 0;
            net.queueOwnerRequest(p); ownersResetPending_ = false;
        }
        unsigned long now = nowMs();
        for (std::map<Tab, unsigned long>::iterator it = pendingOwnerTabs_.begin(); it != pendingOwnerTabs_.end(); ++it) {
            if (it->second && now - it->second < 2000) continue;
            OwnerRequestPacket p; memset(&p, 0, sizeof(p));
            p.type = (u8)PKT_OWNER_REQUEST; p.ownerId = localId_;
            p.worldId = ownersAttached_ ? ownersWorld_ : 0;
            p.container = it->first.first; p.containerSerial = it->first.second;
            net.queueOwnerRequest(p); it->second = now;
        }
    }
}

void Replicator::placeRecruit(GameWorld* gw, Character* c) {
    if (!sessionLive_ || !squadSync_ || !gw || !c || ownersInvalid_) return;
    unsigned int current[5] = { 0, 0, 0, 0, 0 };
    if (!engine::readObjectHand(reinterpret_cast<RootObject*>(c), current)) return;
    Tab old(current[1], current[2]);
    unsigned int tabs[OWNERS_MAX][2]; unsigned int n = engine::listPlayerTabs(gw, tabs, OWNERS_MAX);
    Tab home(0, 0);
    for (unsigned int i = 0; i < n; ++i) {
        Tab candidate(tabs[i][0], tabs[i][1]);
        std::map<Tab, unsigned int>::const_iterator rank = tabRank_.find(candidate);
        bool own = ownersAttached_ ? (tabOwnerIdentity(candidate) == playerIdentity_ || pendingOwnerTabs_.count(candidate)) :
            ownsTab(candidate, rank == tabRank_.end() ? 0xFFFFFFFFu : rank->second);
        if (!own || !ownsTabEffective(candidate, rank == tabRank_.end() ? 0xFFFFFFFFu : rank->second)) continue;
        home = candidate;
        if (candidate == old) break;
    }
    if (home == old) return;
    if (!(home.first | home.second)) {
        unsigned int created[2] = { 0, 0 };
        if (!engine::createPlayerSquad(gw, created)) {
            coop::logErrLine("[owners] recruit home squad creation failed"); return;
        }
        home = Tab(created[0], created[1]); claimLocalTab(home);
    }
    unsigned int target[5] = { current[0], home.first, home.second, current[3], current[4] };
    bool moved = engine::joinPlayerSquadAt(gw, c, target);
    if (moved) {
        moveEcho_[current[4]] = nowMs();
        unsigned int after[5] = { 0, 0, 0, 0, 0 };
        if (engine::readObjectHand(reinterpret_cast<RootObject*>(c), after)) moveEcho_[after[4]] = nowMs();
    }
    char b[160]; _snprintf(b, sizeof(b) - 1, "[owners] recruit home old=%u,%u target=%u,%u moved=%d",
                           old.first, old.second, home.first, home.second, moved ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
}

bool Replicator::canCreateCharacter(GameWorld* gw, u32 author) const {
    if (!gw || !sessionLive_ || !ownersAttached_ || ownersInvalid_ || owners_.size() >= OWNERS_MAX) return false;
    return identityForOwner(author) != 0;
}

bool Replicator::registerCreatedCharacter(u32 author, unsigned int container, unsigned int serial) {
    u64 identity = identityForOwner(author);
    Tab tab(container, serial);
    if (!isHostRole() || !identity || !ownersAttached_ || ownersInvalid_ || owners_.size() >= OWNERS_MAX ||
        !(tab.first | tab.second) || owners_.count(tab)) return false;
    OwnerRow row = { 0 };
    row.container = tab.first; row.containerSerial = tab.second; row.playerId = identity;
    row.sourceContainer = tab.first; row.sourceSerial = tab.second;
    owners_[tab] = row; ++ownersRevision_; ownersDirty_ = true;
    return true;
}

u32 Replicator::characterConnection(u32 owner) const {
    u64 identity = identityForOwner(owner);
    std::map<u64, PeerStatusPacket>::const_iterator p = players_.find(identity);
    return p != players_.end() && p->second.present ? p->second.connectionId : 0;
}

bool Replicator::characterRequestCurrent(const CharacterRequestHeader& h) const {
    return h.worldId && h.worldId == ownershipWorld() && h.authEpoch >= characterWorldEpoch_ && h.authEpoch <= ownersAuthEpoch_ &&
        h.playerId && h.playerId == identityForOwner(h.ownerId) &&
        h.connectionId && h.connectionId == characterConnection(h.ownerId);
}

bool Replicator::applyCreatedAppearance(GameWorld* gw, const unsigned int h[5], const CharacterDraft& draft) {
    Key key; key.t = h[0]; key.c = h[1]; key.cs = h[2]; key.i = h[3]; key.s = h[4];
    std::map<Key, Character*>::const_iterator proxy = proxyByKey_.find(key);
    Character* c = proxy != proxyByKey_.end() ? proxy->second : engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
    unsigned int local[5] = { 0, 0, 0, 0, 0 };
    if (!c || !engine::readObjectHand(reinterpret_cast<RootObject*>(c), local)) { createdAppearanceApplied_.erase(key); return false; }
    std::map<Key, Character*>::const_iterator applied = createdAppearanceApplied_.find(key);
    if (applied != createdAppearanceApplied_.end() && applied->second == c) return true;
    // 失败也按指针记一次，避免每帧重试并刷错误日志；代理重建后指针变化会再试一次。
    bool ok = engine::applyCharacterDraft(gw, c, draft);
    createdAppearanceApplied_[key] = c;
    if (!ok) return false;
    coop::logLine("[chargen] host appearance applied to resolved native or proxy character");
    return true;
}

bool Replicator::createdCharacterArrived(u32 author, const unsigned int h[5]) const {
    Key key; key.t = h[0]; key.c = h[1]; key.cs = h[2]; key.i = h[3]; key.s = h[4];
    std::map<Key, Driven>::const_iterator stream = targets_.find(key);
    return stream != targets_.end() && stream->second.owner == author && stream->second.lastSeenMs != 0;
}

void Replicator::squadAuthView(const std::pair<u32, u32>& source, SquadAuthView& out) const {
    out.authMode = out.requestMode = 0;
    out.authPlayerId = out.requestPlayerId = 0;
    out.requestable = false;
    if (!squadSync_ || !sessionLive_ || (ownersAttached_ && ownersInvalid_)) return;
    Tab tab = canonicalTab(source);
    std::map<Tab, AuthPeer>::const_iterator auth = tabAuth_.find(tab);
    if (auth != tabAuth_.end()) {
        out.authMode = auth->second.mode;
        out.authPlayerId = identityForOwner(auth->second.ownerId);
    }
    std::map<Tab, AuthPeer>::const_iterator request = tabReq_.find(tab);
    if (request != tabReq_.end() && (!request->second.ms ||
        nowMs() - request->second.ms <= (unsigned long)TAB_AUTH_TIMEOUT_MS)) {
        out.requestMode = request->second.mode;
        out.requestPlayerId = identityForOwner(request->second.ownerId);
    }
    if (auth != tabAuth_.end() || request != tabReq_.end()) return;
    u32 target = driverOwnerForTab(tab);
    if (target == OWNER_ID_ALL || target == localId_) return;
    if (ownersAttached_) {
        u64 owner = tabOwnerIdentity(tab);
        if (!owner || owner == playerIdentity_ || ownerForIdentity(owner) != target) return;
    }
    unsigned int rank = 0xFFFFFFFFu;
    for (std::map<Tab, unsigned int>::const_iterator it = tabRank_.begin(); it != tabRank_.end(); ++it)
        if (canonicalTab(it->first) == tab) { rank = it->second; break; }
    out.requestable = !ownsTabEffective(tab, rank);
}

// 联机且归属表已接上后才筛选；未联机、归属未定或表无效时全部显示，保持原版编队栏。
bool Replicator::squadBarVisible(const std::pair<u32, u32>& source) const {
    if (!squadSync_ || !sessionLive_ || !ownersAttached_ || ownersInvalid_) return true;
    Tab tab = canonicalTab(source);
    bool borrowing = false;
    for (std::map<Tab, AuthPeer>::const_iterator it = tabAuth_.begin(); it != tabAuth_.end(); ++it) {
        if (it->second.mode != 2) continue;
        if (it->first == tab) return true;
        borrowing = true;
    }
    return !borrowing && ownsTab(source, 0xFFFFFFFFu);
}

// 个人账本：首行 KCOOP_PLAYERS 1 <世界编号>，之后每人一行 W <玩家标识> <余额> <开局SID，没有为 ->。
// 世界编号与归属表不一致的文件属于别的世界，不读。
bool Replicator::readPlayers(const std::string& name) {
    FILE* f = _wfopen(savexfer::widePath(savexfer::saveFolderFor(name) + "/coop_players.txt").c_str(), L"rb");
    if (!f) return false;
    char line[512]; unsigned int version = 0; u64 world = 0;
    bool valid = fgets(line, sizeof(line), f) && sscanf(line, "KCOOP_PLAYERS %u %I64u", &version, &world) == 2 &&
                 version == 1 && world != 0 && world == ownersWorld_;
    std::map<u64, Wallet> rows;
    while (valid && fgets(line, sizeof(line), f)) {
        u64 id = 0; int money = 0; char start[200] = { 0 };
        if (sscanf(line, "W %I64u %d %199[^\r\n]", &id, &money, start) != 3 || !id) continue;
        Wallet& w = rows[id];
        w.money = money < 0 ? 0 : money;
        if (strcmp(start, "-") != 0) w.start = start;
    }
    fclose(f);
    if (valid) wallets_.swap(rows);
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[wallet] ledger restored players=%u save='%s' ok=%d",
              (unsigned)wallets_.size(), name.c_str(), valid ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
    return valid;
}

bool Replicator::writePlayers(const std::string& name) const {
    if (!isHostRole() || !moneySync_ || !walletsLoaded_ || !ownersWorld_ || ownersInvalid_) return true;
    std::wstring target = savexfer::widePath(savexfer::saveFolderFor(name) + "/coop_players.txt");
    std::wstring temp = target + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"wb");
    if (!f) { coop::logErrLine("[wallet] player ledger could not be opened for writing"); return false; }
    fprintf(f, "KCOOP_PLAYERS 1 %I64u\n", ownersWorld_);
    for (std::map<u64, Wallet>::const_iterator it = wallets_.begin(); it != wallets_.end(); ++it)
        fprintf(f, "W %I64u %d %s\n", it->first, it->second.money,
                it->second.start.empty() ? "-" : it->second.start.c_str());
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[wallet] ledger saved players=%u save='%s' ok=%d",
              (unsigned)wallets_.size(), name.c_str(), ok ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
    return ok;
}

// 主机自己的钱直接写进本机游戏；加入者的记到账上，下一次发布时发给本人。
// 可以反复创建：只有第一次（账上还没记开局）把开局资金加到现有余额上，之后再开局余额不变，防止刷钱或覆盖已有的钱
void Replicator::setPlayerStart(GameWorld* gw, u32 author, const std::string& start, int money) {
    u64 identity = identityForOwner(author);
    if (!isHostRole() || !identity) return;
    Wallet& w = wallets_[identity];
    bool first = w.start.empty();
    if (first) w.start = start;
    if (money < 0 || !first) money = 0;
    int current = 0;
    if (identity == playerIdentity_) { if (money && engine::readPlayerWallet(gw, &current) && current >= 0) engine::writePlayerWallet(gw, current + money); }
    else if (moneySync_) w.money += money;
    char b[200];
    _snprintf(b, sizeof(b) - 1, "[wallet] START player=%llu start='%s' money=%d first=%d", identity, start.c_str(), money, first ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
}

void Replicator::releaseLocalTab(const Tab& tab) {
    if (!sessionLive_ || !squadSync_ || !(tab.first | tab.second)) return;
    Tab canonical = canonicalTab(tab);
    pendingOwnerTabs_.erase(tab);
    u64 owner = tabOwnerIdentity(canonical);
    bool mine = owner == 0 || owner == playerIdentity_;
    if (mine) {
        SquadAuthMsg m; m.kind = 6; m.targetId = OWNER_ID_ALL;
        m.c = canonical.first; m.cs = canonical.second;
        tabOutbox_.push_back(m);
        if (isHostRole() && owners_.erase(canonical)) { ++ownersRevision_; ownersDirty_ = true; }
    }
    char b[160];
    _snprintf(b, sizeof(b) - 1, "[owners] local squad deleted tab=%u,%u canonical=%u,%u sent=%d",
              tab.first, tab.second, canonical.first, canonical.second, mine ? 1 : 0);
    b[sizeof(b) - 1] = 0; coop::logLine(b);
}

} // namespace coop
