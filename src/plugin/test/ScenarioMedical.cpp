// ScenarioMedical.cpp - medical + stats scenarios (monolith split from
// Scenario.cpp, 2026-07-12): medic_order, limb_loss, crawl_move, stats_sync.
// Classes are TU-private (anonymous namespace); only makeMedicalScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// medic_order (medical replication validation): players HEAL each other, both
// directions. Save 'squad1'. Window A: the HOST wounds its OWN tab-0 leader
// (limb flesh + blood - the owner's authoritative medical truth), then the JOIN
// "bandages" its DRIVEN copy of that member (healSubjectBandage = the medical
// fields a real first-aid pass raises). Window B inverts the roles. The oracle
// compares the wounded member's VITALS series on both sides: the wound must
// reach the healer's copy, the treatment must reach the owner's body, and the
// two series must converge. (Phase-1 truth per spikes 21-23: NEITHER crosses -
// the healer's copy is pristine (nothing to bandage, n=0) and the owner never
// sees the treatment. The vitals-sync + treatment-forwarding features are what
// turn this green.)
class MedicOrderScenario : public TimedScenario {
public:
    MedicOrderScenario()
        : TimedScenario("medic_order", 0), recvCount_(0), lastLogMs_(0), lastHealMs_(0),
          haveOwn_(false), havePeer_(false), woundLogged_(false),
          healLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank  = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long woundAt  = ctx.isHost ? A_WOUND_AT_MS : B_WOUND_AT_MS;
        const unsigned long healAt   = ctx.isHost ? B_HEAL_AT_MS : A_HEAL_AT_MS;
        const char* wTag             = ctx.isHost ? "A" : "B";
        const char* hTag             = ctx.isHost ? "B" : "A";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // WOUND our own member (we are its owner - authoritative medical truth).
        if (haveOwn_ && !woundLogged_ && ctx.elapsedMs >= woundAt) {
            bool ok = engine::woundSubjectLimbs(ctx.gw, ownHand_, 40.0f, 70.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO MEDIC %s wound issued hand=%u,%u ok=%d",
                      wTag, ownHand_[3], ownHand_[4], ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            woundLogged_ = true;
        }

        // HEAL the PEER's wounded member = bandage the DRIVEN copy we render.
        // Re-applied ~1 Hz for a few seconds (a real first-aid pass is also
        // incremental, and the phase-2 treatment detector samples).
        if (havePeer_ && ctx.elapsedMs >= healAt && ctx.elapsedMs < healUntil(healAt)) {
            if (ctx.elapsedMs - lastHealMs_ >= 1000) {
                lastHealMs_ = ctx.elapsedMs;
                int nb = engine::healSubjectBandage(ctx.gw, peerHand_);
                if (!healLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO MEDIC %s heal issued hand=%u,%u n=%d",
                              hTag, peerHand_[3], peerHand_[4], nb);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    healLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (woundLogged_ && healLogged_)
                                 : (woundLogged_ && healLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared timeline: host wounds A at 10s, join heals A 20-27s; join wounds B
    // at 32s, host heals B 42-49s.
    static const unsigned long A_WOUND_AT_MS   = 10000;
    static const unsigned long A_HEAL_AT_MS    = 20000;
    static const unsigned long B_WOUND_AT_MS   = 32000;
    static const unsigned long B_HEAL_AT_MS    = 42000;
    static const unsigned long HEAL_WINDOW_MS  = 7000;
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned long JOIN_DURATION_MS = 56000;
    static const unsigned int  MAX_SQUAD        = 32;

    static unsigned long healUntil(unsigned long healAt) { return healAt + HEAL_WINDOW_MS; }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MEDIC own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MEDIC peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastHealMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          woundLogged_;
    bool          healLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// limb_loss (protocol-16 limb replication validation): each client runs the
// engine's own amputate on ITS OWN tab leader (window A = host member LEFT_ARM
// at 12s, window B = join member RIGHT_ARM at 30s) - authoritative damage with
// the severed ground item, exactly what a real severing hit does. Both sides
// log VITALS (with the ls= LimbStates) for every squad member at 2 Hz plus a
// "SCENARIO LIMBITEMS" severed-ground-item count near each subject. The
// Test-LimbLoss oracle gates: the COPY side reaches the STUMP state within a
// latency budget (event or self-heal), and both sides converge on exactly one
// severed ground item per amputation (host-authoritative world-item channel;
// the join's local duplicate must be deduped).
class LimbLossScenario : public TimedScenario {
public:
    LimbLossScenario()
        : TimedScenario("limb_loss", 0), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), cutLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long cutAt   = ctx.isHost ? A_CUT_AT_MS : B_CUT_AT_MS;
        const int           limb    = ctx.isHost ? 0 : 1; // A: LEFT_ARM, B: RIGHT_ARM
        const char*         wTag    = ctx.isHost ? "A" : "B";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // Amputate OUR OWN member (we are its owner - authoritative damage).
        if (haveOwn_ && !cutLogged_ && ctx.elapsedMs >= cutAt) {
            bool ok = engine::amputateSubjectLimb(ctx.gw, ownHand_, limb);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LIMB %s amputate issued hand=%u,%u limb=%d ok=%d",
                      wTag, ownHand_[3], ownHand_[4], limb, ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            cutLogged_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            // Severed ground items near each subject (both sides count their
            // LOCAL world - convergence on exactly 1 per cut is the gate).
            if (haveOwn_) logLimbItems(ctx, ownHand_);
            if (havePeer_) logLimbItems(ctx, peerHand_);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? cutLogged_ : (cutLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long A_CUT_AT_MS      = 12000;
    static const unsigned long B_CUT_AT_MS      = 30000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logLimbItems(const ScenarioContext& ctx, const unsigned int h[5]) {
        int n = engine::countSeveredLimbsNear(ctx.gw, h, 20.0f);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO LIMBITEMS hand=%u,%u t=%lu n=%d",
                  h[3], h[4], ctx.elapsedMs, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LIMB own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LIMB peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          cutLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// stats_sync (protocol-17 character-stats replication validation): each client
// raises stats on ITS OWN tab leader via the raise-only scaffold (window A =
// host raises Strength+Stealth at 12s, window B = join raises Dexterity+
// Athletics at 30s) - the owner-side write the stats channel must carry to the
// peer's driven copy. Both sides log "SCENARIO STATS" series for BOTH leaders
// at 2 Hz; the Test-StatsSync oracle gates: each raised value crosses to the
// peer copy within a latency budget, stays sticky, and stats neither side
// touched do not drift.
class StatsSyncScenario : public TimedScenario {
public:
    StatsSyncScenario()
        : TimedScenario("stats_sync", 0), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), raiseLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank  = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long raiseAt  = ctx.isHost ? A_RAISE_AT_MS : B_RAISE_AT_MS;
        // StatsEnumerated: host raises STRENGTH(1)+STEALTH(16); join raises
        // DEXTERITY(18)+ATHLETICS(17). Disjoint so each direction is provable.
        const int   stat1 = ctx.isHost ? 1  : 18;
        const int   stat2 = ctx.isHost ? 16 : 17;
        const char* wTag  = ctx.isHost ? "A" : "B";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // Raise target: well above any squad1 save-load stat so the crossing
        // is an unambiguous step in the series (raise-only scaffold).
        const float RAISE_TO = 60.0f;

        // Raise OUR OWN leader's stats (we are its owner - authoritative write).
        if (haveOwn_ && !raiseLogged_ && ctx.elapsedMs >= raiseAt) {
            bool ok1 = engine::raiseSubjectStat(ctx.gw, ownHand_, stat1, RAISE_TO);
            bool ok2 = engine::raiseSubjectStat(ctx.gw, ownHand_, stat2, RAISE_TO);
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO STATRAISE %s hand=%u,%u stat%d=%.0f ok=%d stat%d=%.0f ok=%d",
                      wTag, ownHand_[3], ownHand_[4], stat1, RAISE_TO, ok1 ? 1 : 0,
                      stat2, RAISE_TO, ok2 ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            raiseLogged_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logStatsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? raiseLogged_ : (raiseLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long A_RAISE_AT_MS    = 12000;
    static const unsigned long B_RAISE_AT_MS    = 30000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    // One "SCENARIO STATS" line: the watched stats (STRENGTH/STEALTH raised by
    // A, DEXTERITY/ATHLETICS by B, TOUGHNESS as the untouched-drift control)
    // plus xp, read from the LOCAL CharStats of the body at h.
    void logStatsLine(const unsigned int h[5], unsigned long t) {
        engine::StatsRead sr;
        if (!engine::readStatsByHand(h, &sr) || !sr.valid) return;
        char b[200];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO STATS hand=%u,%u t=%lu str=%.2f stealth=%.2f dex=%.2f athl=%.2f tough=%.2f xp=%.0f",
                  h[3], h[4], t, sr.stats[1], sr.stats[16], sr.stats[18],
                  sr.stats[17], sr.stats[21], sr.xp);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO STATS own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO STATS peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          raiseLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// crawl_move (protocol-53 prone posture + crippled cause): the coverage hole
// limb_loss left open. limb_loss cuts ARMS and judges a body standing still, so
// no gate had ever watched an INJURED character walk - which is exactly where a
// crippled crawler was being driven as an upright walker.
//
// Each client amputates a LEG on ITS OWN tab leader (window A = host member
// LEFT_LEG, window B = join member RIGHT_LEG) via the engine's own amputate -
// authoritative damage, the same lever limb_loss uses - and then keeps that body
// MOVING on a short alternating leg. Both sides log "SCENARIO CRAWL" for both
// subjects at 2 Hz: the prone posture, the medical crippled flag, and position.
// Test-CrawlMove gates the copy reaching the owner's prone posture AND crippled
// flag within budget, never reverting, and tracking position while crawling.
// Posture parity is the assertion that distinguishes this from a position gate:
// a copy walking upright toward the right coordinates satisfies the latter.
class CrawlMoveScenario : public TimedScenario {
public:
    CrawlMoveScenario()
        : TimedScenario("crawl_move", 0), lastLogMs_(0), moveIssued_(0),
          haveOwn_(false), havePeer_(false), cutLogged_(false),
          haveStart_(false), sx_(0.0f), sy_(0.0f), sz_(0.0f) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank  = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long cutAt    = ctx.isHost ? A_CUT_AT_MS : B_CUT_AT_MS;
        // RobotLimbs::Limb order: 0 LEFT_ARM, 1 RIGHT_ARM, 2 LEFT_LEG, 3 RIGHT_LEG.
        const int           limb     = ctx.isHost ? 2 : 3;
        const char*         wTag     = ctx.isHost ? "A" : "B";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // Take the leg off OUR OWN member (we own it - authoritative damage).
        if (haveOwn_ && !cutLogged_ && ctx.elapsedMs >= cutAt) {
            bool ok = engine::amputateSubjectLimb(ctx.gw, ownHand_, limb);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CRAWL %s amputate issued hand=%u,%u limb=%d ok=%d",
                      wTag, ownHand_[3], ownHand_[4], limb, ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            cutLogged_ = true;
        }

        // Then keep the injured body MOVING: an alternating out-and-back leg (the
        // leader_move pattern) so the copy is driven the whole window instead of
        // parking, which is what makes a crawl-vs-walk mismatch measurable.
        //
        // The leg phase is counted from the START of the move window, not from
        // ctx.elapsedMs. Phasing on absolute elapsed time truncated whichever leg
        // the window opened mid-way through (run 20260805_153425: the first leg got
        // 1 s of its 4, so the crawler covered ~4.8 u before being ordered back) -
        // and a body that never travels cannot show a tracking failure, which is
        // how a byte-frozen copy passed the gap check. A leg also has to outlast a
        // CRAWL: at the measured ~4.8 u/s, 20 u needs ~4.2 s, so 4 s could not
        // complete even an untruncated one.
        unsigned long moveFrom = cutAt + MOVE_AFTER_MS;
        if (cutLogged_ && ctx.elapsedMs >= moveFrom &&
            ctx.elapsedMs < moveFrom + MOVE_FOR_MS) {
            Character* oc = engine::resolveCharByHand(ownHand_[3], ownHand_[4],
                                                      ownHand_[0], ownHand_[1],
                                                      ownHand_[2]);
            if (oc) {
                if (!haveStart_) haveStart_ = engine::readPos(oc, &sx_, &sy_, &sz_);
                if (haveStart_) {
                    bool legOut = (((ctx.elapsedMs - moveFrom) / LEG_MS) % 2) == 0;
                    if (engine::orderMoveTo(oc, legOut ? sx_ + LEG : sx_, sy_,
                                            legOut ? sz_ + LEG : sz_))
                        ++moveIssued_;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // BOTH subjects from BOTH sides, so the oracle can pair owner and
            // copy by hand for either direction.
            if (haveOwn_)  logCrawlLine(ownHand_, ctx.elapsedMs);
            if (havePeer_) logCrawlLine(peerHand_, ctx.elapsedMs);
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = cutLogged_ && moveIssued_ > 0;
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CRAWL verdict role=%s cut=%d moves=%u",
                      ctx.isHost ? "host" : "join", cutLogged_ ? 1 : 0, moveIssued_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

private:
    static const unsigned long A_CUT_AT_MS      = 10000;
    static const unsigned long B_CUT_AT_MS      = 34000;
    static const unsigned long MOVE_AFTER_MS    = 5000;  // let the cut settle first
    static const unsigned long MOVE_FOR_MS      = 16000; // driven-crawl window
    static const unsigned long LEG_MS           = 6000;  // > 20u at a ~4.8u/s crawl
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         LEG;

    // The protocol-53 parity sample: prone posture + the crippled CAUSE flag +
    // position, for one body, as read on THIS client. Emitted for both subjects
    // on both sides so the oracle pairs owner/copy by hand and time.
    void logCrawlLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        engine::readPos(c, &x, &y, &z);
        int prone = engine::readProneState(c);
        unsigned short bs = engine::readBodyState(c);
        engine::MedicalRead mr;
        int crip = (engine::readMedicalByHand(h, &mr) && mr.valid)
                     ? (mr.crippled ? 1 : 0) : -1;
        bool moving = false; float spd = 0.0f;
        engine::readMotion(c, &moving, &spd);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CRAWL hand=%u,%u t=%lu prone=%d crip=%d "
                  "pos=%.2f,%.2f,%.2f bs=%u mv=%d spd=%.2f",
                  h[3], h[4], t, prone, crip, x, y, z,
                  (unsigned)bs, moving ? 1 : 0, spd);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Which transform does the MODEL follow? readPos (Character::getPosition,
        // and the nametag) tracked the owner while the mesh stayed at the collapse
        // spot, so log the other candidates beside it for the same body on both
        // clients: a value that is stale on the copy and live on the owner is the
        // one the render actually uses.
        engine::DriveProbe dp;
        if (engine::readDriveProbe(c, &dp)) {
            char p[256];
            _snprintf(p, sizeof(p) - 1,
                      "SCENARIO CRAWLPROBE hand=%u,%u t=%lu mv=%.2f,%.2f,%.2f "
                      "hk=%d:%.2f,%.2f,%.2f mode=%d ao=%d tav=%.2f,%.2f,%.2f "
                      "mot=%.2f,%.2f,%.2f hkvel=%.2f,%.2f,%.2f "
                      "hkmoved=%.3f,%.3f,%.3f cur=%d:%.2f",
                      h[3], h[4], t, dp.mvX, dp.mvY, dp.mvZ,
                      dp.haveHk ? 1 : 0, dp.hkX, dp.hkY, dp.hkZ,
                      dp.movementMode, dp.animOverride ? 1 : 0,
                      dp.tavX, dp.tavY, dp.tavZ, dp.motX, dp.motY, dp.motZ,
                      dp.hkVelX, dp.hkVelY, dp.hkVelZ,
                      dp.hkMovedX, dp.hkMovedY, dp.hkMovedZ,
                      dp.curMoving ? 1 : 0, dp.curSpeed);
            p[sizeof(p) - 1] = '\0'; coop::logLine(p);
        }
    }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CRAWL own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CRAWL peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned long lastLogMs_;
    unsigned int  moveIssued_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          cutLogged_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

const float CrawlMoveScenario::LEG = 20.0f;

// ===========================================================================
// owner_death（死亡处理 784650d / ed771f1 的战斗路径）：三方各自打死一个自己的角色。
// 主人把角色头部伤到致命，由原版医疗更新判死、调 declareDead，与战斗中被砍死是同一
// 条路。部位伤只同步不低于 0 的值，其他人的副本自己判不出死，只能等主人的 EVT_DEATH
// 走 [death] owner death applied —— 饿死测不到的就是这条。
//   * 主机、好友1、好友2 依次在第 0、3、6 个 4 秒整点下手（系统时钟，本机多开共用）；
//   * 好友2 优先挑编队里只剩他一个活人的（编队空了会被引擎判死），其他人挑多人编队里的；
//   * 最后一人下手约 24 秒后各方核对：自己的受害者死了且在阵亡编队；开始时活着、现在
//     死了的身体都在本机阵亡编队；开始时的身体一个不少；没有多出编队；新死的至少 3 个。
// 各方 DEATHKILL 行的编号、位置与另外两方的 DEATHSEEN 行对照，确认三人的死亡都同步到。
//
// corpse_loot（同一个类的搜刮模式）：只有好友1 在第 0 个整点打死自己一个角色；第 3 个整点
// 主机把尸体身上一半的东西（隔一项拿一项）移到自己一个活人身上，第 6 个整点好友2 把剩下的
// 全拿走（尸体被拿空，也测“零行”）。尸体由好友1 发布，别人拿东西走的是跨主人转移。最后各方
// 核对本机的尸体已空（好友1 没动手，空了说明别人的转移到了主人那里），并给每个身体打一行
// LOOTINV（件数 + 内容指纹），三方对照指纹一致。需要 KENSHICOOP_INV_SYNC=1（场景模式下
// 背包同步默认关）。
// ===========================================================================
class OwnerDeathScenario : public TimedScenario {
public:
    explicit OwnerDeathScenario(bool loot)
        : TimedScenario(loot ? "corpse_loot" : "owner_death", /*evidenceMs=*/0),
          loot_(loot), firstSlot_(0), killed_(false), looted_(false), moved_(0),
          victim_(0), nStart_(0), nTabs_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        firstSlot_ = (GetTickCount() + LEAD_MS) / SLOT_MS + 1;
        // 记下开始时活着的玩家角色（本机指针，只比较不解引用）和现有编队
        EntityState sq[MAX_BODIES];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
        for (unsigned int i = 0; i < n; ++i) {
            if (sq[i].bodyState & BODY_DEAD) continue;
            Character* c = engine::resolve(sq[i]);
            if (!c) continue;
            startOwn_[nStart_] = ctx.ownTab && ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial);
            startAlive_[nStart_++] = c;
        }
        nTabs_ = engine::listPlayerTabs(ctx.gw, startTabs_, MAX_TABS);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO DEATHRUN start who=%s rank=%u bodies=%u alive=%u tabs=%u firstSlot=%lu",
                  ctx.isHost ? "host" : "join", ctx.ownRank, n, nStart_, nTabs_, firstSlot_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long slot = GetTickCount() / SLOT_MS;
        // 搜刮模式只有好友1 下手，在第 0 个整点
        if (!killed_ && (!loot_ || ctx.ownRank == 1) &&
            slot >= firstSlot_ + (loot_ ? 0 : KILL_GAP_SLOTS * ctx.ownRank)) {
            killed_ = true;
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            // 挑受害者：好友2 要编队里只剩他一个活人的，其他人要多人编队里的；都没有就挑第一个自己的活人
            int pick = -1, fallback = -1;
            unsigned int mates = 0;
            for (unsigned int i = 0; i < n && pick < 0; ++i) {
                if ((sq[i].bodyState & BODY_DEAD) || !ctx.ownTab ||
                    !ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) continue;
                mates = 0;
                for (unsigned int j = 0; j < n; ++j)
                    if (!(sq[j].bodyState & BODY_DEAD) && sq[j].hContainer == sq[i].hContainer &&
                        sq[j].hContainerSerial == sq[i].hContainerSerial) ++mates;
                if (fallback < 0) fallback = (int)i;
                if ((ctx.ownRank == 2) == (mates == 1)) pick = (int)i;
            }
            // pref：挑到的是否合乎上面的偏好（没有合适的才退回第一个自己的活人）
            bool pref = pick >= 0;
            if (!pref) pick = fallback;
            int heads = -1;
            if (pick >= 0) victim_ = engine::resolve(sq[pick]);
            if (victim_) {
                // 头部伤到远低于负的上限，下一帧原版医疗更新判死
                heads = 0;
                __try {
                    MedicalSystem* med = &victim_->medical;
                    for (unsigned int k = 0; k < (unsigned int)med->anatomy.count; ++k) {
                        MedicalSystem::HealthPartStatus* p = med->anatomy.stuff[k];
                        if (!p || p->whatAmI != MedicalSystem::HealthPartStatus::PART_HEAD) continue;
                        p->flesh     = -(p->_maxHealth * 3.0f + 100.0f);
                        p->fleshStun = p->flesh;
                        ++heads;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    heads = -2;
                }
            }
            char b[240];
            if (pick >= 0)
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO DEATHKILL who=%s rank=%u idx=%u ser=%u tab=%u,%u pos=%.1f,%.1f heads=%d pref=%d tick=%lu",
                          ctx.isHost ? "host" : "join", ctx.ownRank, sq[pick].hIndex, sq[pick].hSerial,
                          sq[pick].hContainer, sq[pick].hContainerSerial, sq[pick].x, sq[pick].z,
                          heads, pref ? 1 : 0, (unsigned long)GetTickCount());
            else
                _snprintf(b, sizeof(b) - 1, "SCENARIO DEATHKILL who=%s rank=%u none (no own living character)",
                          ctx.isHost ? "host" : "join", ctx.ownRank);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return false;
        }
        // 搜刮：主机在第 3 个整点隔一项拿一项，好友2 在第 6 个整点把剩下的全拿走，都放到自己一个活人身上
        if (loot_ && !looted_ && ctx.ownRank != 1 &&
            slot >= firstSlot_ + KILL_GAP_SLOTS * (ctx.isHost ? 1 : 2)) {
            looted_ = true;
            EntityState sq[MAX_BODIES];
            Character* now[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            int corpse = -1, dst = -1;
            for (unsigned int i = 0; i < n; ++i) {
                now[i] = engine::resolve(sq[i]);
                if (dst < 0 && !(sq[i].bodyState & BODY_DEAD) && ctx.ownTab &&
                    ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) dst = (int)i;
            }
            // 尸体：开始时活着、不是自己的、现在死了
            for (unsigned int s = 0; s < nStart_ && corpse < 0; ++s) {
                if (startOwn_[s]) continue;
                for (unsigned int i = 0; i < n && corpse < 0; ++i)
                    if (now[i] == startAlive_[s] && (sq[i].bodyState & BODY_DEAD)) corpse = (int)i;
            }
            unsigned int entries = 0;
            if (corpse >= 0 && dst >= 0) {
                unsigned int ch[5], dh[5], hash = 0;
                handFromEntity(sq[corpse], ch);
                handFromEntity(sq[dst], dh);
                InvItemEntry items[MAX_ITEMS];
                entries = engine::captureContainerContents(ctx.gw, ch, items, MAX_ITEMS, &hash);
                for (unsigned int k = 0; k < entries; ++k) {
                    if (ctx.isHost && k % 2 == 1) continue;
                    int got = engine::moveItemBetweenContainers(ctx.gw, ch, dh, items[k].stringID,
                                                               items[k].itemType,
                                                               items[k].quantity ? items[k].quantity : 1);
                    moved_ += got;
                    char m[200];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO LOOTITEM who=%s k=%u sid='%s' type=%u qty=%u eq=%u got=%d",
                              ctx.isHost ? "host" : "join", k, items[k].stringID, items[k].itemType,
                              (unsigned int)items[k].quantity, (unsigned int)items[k].equipped, got);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
            char b[240];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOOT who=%s rank=%u corpse=%u,%u dst=%u,%u entries=%u moved=%d tick=%lu",
                      ctx.isHost ? "host" : "join", ctx.ownRank,
                      corpse >= 0 ? sq[corpse].hIndex : 0, corpse >= 0 ? sq[corpse].hSerial : 0,
                      dst >= 0 ? sq[dst].hIndex : 0, dst >= 0 ? sq[dst].hSerial : 0,
                      entries, moved_, (unsigned long)GetTickCount());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return false;
        }
        if (slot < firstSlot_ + END_SLOTS + (ctx.isHost ? HOST_EXTRA_SLOTS : 0)) return false;

        unsigned int dead[2] = { 0, 0 };
        bool haveDead = engine::readDeadSquadTab(ctx.gw, dead);
        EntityState sq[MAX_BODIES];
        Character* now[MAX_BODIES];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
        for (unsigned int i = 0; i < n; ++i) now[i] = engine::resolve(sq[i]);
        unsigned int vanished = 0, newlyDead = 0, misplaced = 0;
        int corpseAt = -1;
        bool victimOk = false;
        char b[240];
        for (unsigned int s = 0; s < nStart_; ++s) {
            int at = -1;
            for (unsigned int i = 0; i < n && at < 0; ++i)
                if (now[i] == startAlive_[s]) at = (int)i;
            if (at < 0) { ++vanished; continue; }
            const EntityState& e = sq[at];
            if (!(e.bodyState & BODY_DEAD)) continue;
            bool inDead = haveDead && e.hContainer == dead[0] && e.hContainerSerial == dead[1];
            ++newlyDead;
            if (corpseAt < 0) corpseAt = at;
            if (!inDead) ++misplaced;
            if (startAlive_[s] == victim_ && inDead) victimOk = true;
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO DEATHSEEN who=%s idx=%u ser=%u tab=%u,%u deadSquad=%d mine=%d pos=%.1f,%.1f",
                      ctx.isHost ? "host" : "join", e.hIndex, e.hSerial, e.hContainer,
                      e.hContainerSerial, inDead ? 1 : 0, startAlive_[s] == victim_ ? 1 : 0, e.x, e.z);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        unsigned int tabs[MAX_TABS][2];
        unsigned int nt = engine::listPlayerTabs(ctx.gw, tabs, MAX_TABS), newTabs = 0;
        for (unsigned int t = 0; t < nt; ++t) {
            bool known = false;
            for (unsigned int k = 0; k < nTabs_ && !known; ++k)
                known = startTabs_[k][0] == tabs[t][0] && startTabs_[k][1] == tabs[t][1];
            if (known) continue;
            ++newTabs;
            _snprintf(b, sizeof(b) - 1, "SCENARIO DEATHTAB who=%s new tab=%u,%u",
                      ctx.isHost ? "host" : "join", tabs[t][0], tabs[t][1]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 搜刮模式：每个身体打一行件数和内容指纹供三方对照；本机的尸体（只有一个新死的）应已拿空
        int corpseLeft = -1;
        for (unsigned int i = 0; loot_ && i < n; ++i) {
            unsigned int h[5], hash = 0;
            handFromEntity(sq[i], h);
            InvItemEntry items[MAX_ITEMS];
            unsigned int cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, &hash);
            if ((int)i == corpseAt) corpseLeft = (int)cnt;
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOOTINV who=%s idx=%u ser=%u dead=%d corpse=%d count=%u hash=%08x",
                      ctx.isHost ? "host" : "join", sq[i].hIndex, sq[i].hSerial,
                      (sq[i].bodyState & BODY_DEAD) ? 1 : 0, (int)i == corpseAt ? 1 : 0, cnt, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (loot_)
            passed_ = (ctx.ownRank == 1 ? victimOk : moved_ > 0) && corpseLeft == 0 &&
                      vanished == 0 && misplaced == 0 && newTabs == 0 && newlyDead >= 1;
        else
            passed_ = victimOk && vanished == 0 && misplaced == 0 && newTabs == 0 && newlyDead >= 3;
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO DEATHRUN final who=%s rank=%u victim=%d newlyDead=%u misplaced=%u vanished=%u tabs=%u->%u newTabs=%u deadTab=%u,%u moved=%d corpseLeft=%d",
                  ctx.isHost ? "host" : "join", ctx.ownRank, victimOk ? 1 : 0, newlyDead, misplaced,
                  vanished, nTabs_, nt, newTabs, dead[0], dead[1], moved_, corpseLeft);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

private:
    static const unsigned int  MAX_BODIES       = 64;
    static const unsigned int  MAX_TABS         = 64;
    static const unsigned int  MAX_ITEMS        = 64;
    static const unsigned long SLOT_MS          = 4000;
    static const unsigned long LEAD_MS          = 8000;
    static const unsigned long KILL_GAP_SLOTS   = 3;
    static const unsigned long END_SLOTS        = 12;
    // 主机多等两个整点再核对，免得先退出把加入方断开
    static const unsigned long HOST_EXTRA_SLOTS = 2;

    bool          loot_;
    unsigned long firstSlot_;
    bool          killed_;
    bool          looted_;
    int           moved_;
    Character*    victim_;
    Character*    startAlive_[MAX_BODIES];
    bool          startOwn_[MAX_BODIES];
    unsigned int  nStart_;
    unsigned int  startTabs_[MAX_TABS][2];
    unsigned int  nTabs_;
};

} // namespace

Scenario* makeMedicalScenario(const std::string& name) {
    if (name == "medic_order")  return new MedicOrderScenario();
    if (name == "limb_loss")    return new LimbLossScenario();
    if (name == "crawl_move")   return new CrawlMoveScenario();
    if (name == "stats_sync")   return new StatsSyncScenario();
    if (name == "owner_death")  return new OwnerDeathScenario(false);
    if (name == "corpse_loot")  return new OwnerDeathScenario(true);
    return 0;
}

} // namespace coop
