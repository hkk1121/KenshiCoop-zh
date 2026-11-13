// ScenarioMedical.cpp - medical + stats scenarios (monolith split from
// Scenario.cpp, 2026-07-12): medic_order, limb_loss, crawl_move, stats_sync.
// Classes are TU-private (anonymous namespace); only makeMedicalScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

// 和 EngineInternal.h 一样关掉 boost 自动链接，ForgottenGUI.h 会引入 boost.thread
#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1
#include <kenshi/Globals.h>         // gui（抢尸体场景开关物品栏）
#include <kenshi/gui/ForgottenGUI.h>

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
//
// loot_race（搜刮模式的抢尸体变体，跨主人转移 R3～R5）：好友1 在第 0 个整点打死自己一个角色；
// 第 1 个整点各方认准尸体，记下尸体里每样东西开始时在所有身体上的总数；第 3 个整点主机和好友2
// 同时把尸体里的东西全拿到自己一个活人身上。源主人好友1 先到先批，后到的被拒、从拿的人身上收回。
// 最后各方核对本机尸体已空、每样东西在所有身体上的总数与开始时相同（LOOTKEY 行）。
// loot_race_ui：另外三方在第 2～7 个整点开着自己一个活人的物品栏，收回和源头去掉都要等关窗（看 DEBT）；
// 窗口中途被关掉的话 ui=0、判失败。
// 布防时各方把自己的活人都喂饱（途中饿死会换手把，背包快照对不上）。
// loot_race_full：在 _ui 基础上，两位拿的人同时往自己的目的地放 1 件测试物品作记号，
// 好友1 第 2 个整点把本机看到多了记号的别人背包塞满，让源端放不进、走“关窗后再从源头去掉”。
// death_race（阵亡编队编号不一致验证）：三方在第 0 个整点同时各打死自己一个角色，不按名次错开、
// 优先挑身上有物品的，编号因此在各自机器上错开；第 2 个整点各方用 removeItemsFromContainer 从
// 自己那具尸体上拿走 1 件（先散放、后穿戴，尸体上什么都没有就放进 1 件测试物品），模拟主人改尸体背包、
// 别人要跟上；第 4 个整点名下还有活人的一方从别人的新尸体里拿 1 件到自己活人身上（转移意图一路）；
// 第 9 个整点沿用 DEATHSEEN / LOOTINV 输出，另打一行 DEATHRACE final。布防时同样把自己的
// 活人喂饱。需要 KENSHICOOP_INV_SYNC=1。
// ===========================================================================
class OwnerDeathScenario : public TimedScenario {
public:
    explicit OwnerDeathScenario(bool loot, int race = 0, bool deathRace = false)
        : TimedScenario(race == 3 ? "loot_race_full" : race == 2 ? "loot_race_ui" :
                        race == 1 ? "loot_race" : deathRace ? "death_race" :
                        loot ? "corpse_loot" : "owner_death",
                        /*evidenceMs=*/0),
          loot_(loot), firstSlot_(0), killed_(false), looted_(false), edited_(-1), moved_(0),
          victim_(0), corpse_(0), nStart_(0), nTabs_(0), race_(race), deathRace_(deathRace),
          uiOpened_(false), uiClosed_(false), uiOk_(0), uiLast_(0), uiChar_(0), marked_(false),
          filled_(false), filledN_(0), dst_(0), nKeys_(0), nMark_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        firstSlot_ = (GetTickCount() + LEAD_MS) / SLOT_MS + 1;
        // 记下开始时活着的玩家角色（本机指针，只比较不解引用）和现有编队
        EntityState sq[MAX_BODIES];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
        for (unsigned int i = 0; i < n; ++i) {
            if (sq[i].bodyState & BODY_DEAD) continue;
            Character* c = engine::resolve(sq[i]);
            if (c) startAlive_[nStart_++] = c;
        }
        nTabs_ = engine::listPlayerTabs(ctx.gw, startTabs_, MAX_TABS);
        // 抢尸体：把自己的活人都喂饱，免得途中饿死进阵亡编队、换了手把（各方阵亡编队里的编号可能不同，
        // 背包快照对不上），也免得被当成尸体
        // 死亡赛同样喂饱：免得下手前就饿死，轮到下手时名下没有活人
        unsigned int fed = 0;
        for (unsigned int i = 0; (race_ || deathRace_) && i < n; ++i) {
            if ((sq[i].bodyState & BODY_DEAD) || !ctx.ownTab ||
                !ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) continue;
            unsigned int h[5];
            handFromEntity(sq[i], h);
            if (engine::writeHungerByHand(h, FULL_HUNGER, -1.0f)) ++fed;
        }
        char b[160];
        if (race_ || deathRace_) {
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTFEED who=%s fed=%u", ctx.isHost ? "host" : "join", fed);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO DEATHRUN start who=%s rank=%u bodies=%u alive=%u tabs=%u firstSlot=%lu",
                  ctx.isHost ? "host" : "join", ctx.ownRank, n, nStart_, nTabs_, firstSlot_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long slot = GetTickCount() / SLOT_MS;
        // 搜刮模式只有好友1 下手，在第 0 个整点
        // 死亡赛：三方都在第 0 个整点同时下手（loot_ 模式下的延迟本来就是 0，只是不再限定好友1）
        if (!killed_ && (deathRace_ || !loot_ || ctx.ownRank == 1) &&
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
                // 死亡赛：优先挑身上有物品的（尸体背包才有内容可对），都没有就退回第一个自己的活人
                if (deathRace_) {
                    if (fallback < 0) fallback = (int)i;
                    unsigned int h[5];
                    handFromEntity(sq[i], h);
                    InvItemEntry items[MAX_ITEMS];
                    if (engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, 0) > 0)
                        pick = (int)i;
                    continue;
                }
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
            // 死亡赛：受害者就是本机那具尸体，第 2 个整点改背包和第 5 个整点核对都认这一具
            if (deathRace_) corpse_ = victim_;
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
        // 搜刮模式：下手后第 1 个整点各方记下尸体（此时新死的只有受害者），之后搜刮和核对都认这一具，
        // 免得途中别的角色死了（比如倒地时被饥饿土匪打死）被当成尸体
        if (loot_ && !deathRace_ && !corpse_ && slot >= firstSlot_ + 1) {
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            for (unsigned int i = 0; i < n && !corpse_; ++i) {
                if (!(sq[i].bodyState & BODY_DEAD)) continue;
                Character* c = engine::resolve(sq[i]);
                for (unsigned int s = 0; s < nStart_ && !corpse_; ++s)
                    if (c && startAlive_[s] == c) corpse_ = c;
                if (!corpse_) continue;
                char b[160];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTCORPSE who=%s idx=%u ser=%u",
                          ctx.isHost ? "host" : "join", sq[i].hIndex, sq[i].hSerial);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // 抢尸体：记下尸体里的每样东西和它们开始时在所有身体上的总数；满包模式另记每个身体上的测试物品件数
            char tsid[48] = ""; unsigned int ttype = 0;
            if (race_ && corpse_) engine::commonTestItemSid(ctx.gw, tsid, sizeof(tsid), &ttype);
            for (unsigned int pass = 0; race_ && corpse_ && pass < 2; ++pass)
                for (unsigned int i = 0; i < n; ++i) {
                    Character* c = engine::resolve(sq[i]);
                    if (pass == 0 && c != corpse_) continue;
                    unsigned int h[5];
                    handFromEntity(sq[i], h);
                    InvItemEntry items[MAX_ITEMS];
                    unsigned int cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, 0);
                    int marks = 0;
                    for (unsigned int j = 0; j < cnt; ++j) {
                        int q = items[j].quantity < 1 ? 1 : (int)items[j].quantity;
                        unsigned int k = 0;
                        while (k < nKeys_ && (keys_[k].itemType != items[j].itemType ||
                                              strcmp(keys_[k].stringID, items[j].stringID) != 0)) ++k;
                        if (pass == 0 && k == nKeys_ && nKeys_ < MAX_ITEMS) {
                            keys_[nKeys_] = items[j];
                            keyStart_[nKeys_++] = 0;
                        }
                        if (pass == 1 && k < nKeys_) keyStart_[k] += q;
                        if (tsid[0] && items[j].itemType == ttype && strcmp(items[j].stringID, tsid) == 0)
                            marks += q;
                    }
                    if (pass == 1 && nMark_ < MAX_BODIES) {
                        markWho_[nMark_] = c;
                        markBase_[nMark_++] = marks;
                    }
                }
            for (unsigned int k = 0; k < nKeys_; ++k) {
                char b[200];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTKEY start who=%s sid='%s' type=%u total=%d",
                          ctx.isHost ? "host" : "join", keys_[k].stringID, keys_[k].itemType, keyStart_[k]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // 抢尸体：两位拿的人第 1 个整点过 1 秒定下目的地（自己第一个活人），满包模式再放 1 件测试物品作记号
        if (race_ && corpse_ && !marked_ && ctx.ownRank != 1 &&
            GetTickCount() >= (firstSlot_ + 1) * SLOT_MS + MARK_DELAY_MS) {
            marked_ = true;
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            int pick = -1, added = 0;
            for (unsigned int i = 0; i < n && pick < 0; ++i)
                if (!(sq[i].bodyState & BODY_DEAD) && ctx.ownTab &&
                    ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) pick = (int)i;
            if (pick >= 0) {
                dst_ = engine::resolve(sq[pick]);
                unsigned int h[5];
                handFromEntity(sq[pick], h);
                if (race_ == 3) added = engine::addTestItemsToContainer(ctx.gw, h, 1, 0, 0);
            }
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTMARK who=%s dst=%u,%u added=%d",
                      ctx.isHost ? "host" : "join", pick >= 0 ? sq[pick].hIndex : 0,
                      pick >= 0 ? sq[pick].hSerial : 0, added);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 满包模式：好友1 第 2 个整点把多了记号的别人背包（本机这份）塞满
        if (race_ == 3 && corpse_ && !filled_ && ctx.ownRank == 1 && slot >= firstSlot_ + 2) {
            filled_ = true;
            char tsid[48] = ""; unsigned int ttype = 0;
            engine::commonTestItemSid(ctx.gw, tsid, sizeof(tsid), &ttype);
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            for (unsigned int i = 0; i < n && tsid[0]; ++i) {
                if ((sq[i].bodyState & BODY_DEAD) || !ctx.ownTab ||
                    ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) continue;
                Character* c = engine::resolve(sq[i]);
                int base = -1;
                for (unsigned int m = 0; m < nMark_ && base < 0; ++m)
                    if (markWho_[m] == c) base = markBase_[m];
                unsigned int h[5];
                handFromEntity(sq[i], h);
                InvItemEntry items[MAX_ITEMS];
                unsigned int cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, 0);
                int marks = 0;
                for (unsigned int j = 0; j < cnt; ++j)
                    if (items[j].itemType == ttype && strcmp(items[j].stringID, tsid) == 0)
                        marks += items[j].quantity < 1 ? 1 : (int)items[j].quantity;
                if (base < 0 || marks <= base) continue;
                int added = 0;
                while (added < FILL_MAX && engine::addTestItemsToContainer(ctx.gw, h, 1, 0, 0) == 1) ++added;
                filledN_ += added;
                cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, 0);
                char b[160];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTFILL who=join idx=%u ser=%u marks=%d->%d added=%d entries=%u",
                          sq[i].hIndex, sq[i].hSerial, base, marks, added, cnt);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // 开窗模式：三方第 2 个整点打开自己一个活人的物品栏（拿的人开目的地的），第 7 个整点关掉。
        // 不开尸体的：尸体被拿空后游戏会自己关掉它的窗口
        if (race_ >= 2 && corpse_ && !uiOpened_ && slot >= firstSlot_ + 2) {
            uiOpened_ = true;
            uiChar_ = dst_;
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            for (unsigned int i = 0; i < n && !uiChar_; ++i)
                if (!(sq[i].bodyState & BODY_DEAD) && ctx.ownTab &&
                    ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) uiChar_ = engine::resolve(sq[i]);
            bool shown = false;
            __try {
                if (::gui && uiChar_) shown = ::gui->showInventory(uiChar_->getHandle(), true, 0.0f, 0.0f) != 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                shown = false;
            }
            uiOk_ = engine::inventoryUiOpen() ? 1 : 0;
            uiLast_ = uiOk_;
            char b[120];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTUI open who=%s shown=%d uiOpen=%d tick=%lu",
                      ctx.isHost ? "host" : "join", shown ? 1 : 0, uiOk_, (unsigned long)GetTickCount());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 开着的期间窗口状态一变就记一行（被游戏自己关掉时，收回会提前执行）
        if (race_ >= 2 && uiOpened_ && !uiClosed_ && (engine::inventoryUiOpen() ? 1 : 0) != uiLast_) {
            uiLast_ = engine::inventoryUiOpen() ? 1 : 0;
            if (!uiLast_) uiOk_ = 0;
            char b[120];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTUI changed who=%s uiOpen=%d tick=%lu",
                      ctx.isHost ? "host" : "join", uiLast_, (unsigned long)GetTickCount());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (race_ >= 2 && uiOpened_ && !uiClosed_ && slot >= firstSlot_ + UI_CLOSE_SLOT) {
            uiClosed_ = true;
            __try {
                if (::gui && uiChar_) ::gui->closeInventory(uiChar_->getHandle());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            char b[120];
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTUI close who=%s before=%d uiOpen=%d tick=%lu",
                      ctx.isHost ? "host" : "join", uiLast_, engine::inventoryUiOpen() ? 1 : 0,
                      (unsigned long)GetTickCount());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 死亡赛：第 2 个整点各方从自己那具尸体上拿走 1 件（先挑散放的，没有就挑穿戴的），
        // 模拟主人改了尸体背包、别人要跟上
        if (deathRace_ && edited_ < 0 && slot >= firstSlot_ + 2) {
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            int at = -1;
            for (unsigned int i = 0; i < n && at < 0; ++i)
                if (corpse_ && (sq[i].bodyState & BODY_DEAD) &&
                    engine::resolve(sq[i]) == corpse_) at = (int)i;
            unsigned int ser = 0;
            const char* sid = "";
            int removed = 0, added = 0;
            InvItemEntry items[MAX_ITEMS];
            if (at >= 0) {
                unsigned int h[5];
                handFromEntity(sq[at], h);
                ser = sq[at].hSerial;
                unsigned int cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, 0);
                int pick = -1;
                for (unsigned int k = 0; k < cnt && pick < 0; ++k)
                    if (!items[k].equipped) pick = (int)k;
                for (unsigned int k = 0; k < cnt && pick < 0; ++k)
                    if (items[k].equipped) pick = (int)k;
                if (pick >= 0) {
                    sid = items[pick].stringID;
                    removed = engine::removeItemsFromContainer(ctx.gw, h, items[pick].stringID,
                                                               items[pick].itemType, 1);
                } else {
                    // 尸体上什么都没有（比如裸身开局的人）：改为放进 1 件测试物品，同样是主人改了尸体背包
                    added = engine::addTestItemsToContainer(ctx.gw, h, 1, 0, 0);
                }
            }
            edited_ = removed + added;
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DEATHRACE edit who=%s ser=%u sid='%s' removed=%d added=%d",
                      ctx.isHost ? "host" : "join", ser, sid, removed, added);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 搜刮：主机在第 3 个整点隔一项拿一项，好友2 在第 6 个整点把剩下的全拿走，都放到自己一个活人身上；
        // 抢尸体时两人都在第 3 个整点全拿（满包模式放到做了记号的那个人身上）
        // 死亡赛：第 4 个整点，名下还有活人的一方从别人的新尸体里（挑东西最多的那具，它在本机的编号
        // 多半与主人那边不同）拿 1 件放到自己一个活人身上，走的是转移意图而不是背包快照
        if (loot_ && !looted_ && (deathRace_ || ctx.ownRank != 1) &&
            slot >= firstSlot_ + (deathRace_ ? DEATHRACE_LOOT_SLOT :
                                  KILL_GAP_SLOTS * (ctx.isHost || race_ ? 1 : 2))) {
            looted_ = true;
            EntityState sq[MAX_BODIES];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
            int corpse = -1, dst = -1;
            unsigned int most = 0;
            for (unsigned int i = 0; i < n; ++i) {
                Character* c = engine::resolve(sq[i]);
                if (!deathRace_ && corpse < 0 && corpse_ && c == corpse_) corpse = (int)i;
                if (deathRace_ && c && c != corpse_ && (sq[i].bodyState & BODY_DEAD)) {
                    bool fresh = false;
                    for (unsigned int s = 0; s < nStart_ && !fresh; ++s) fresh = startAlive_[s] == c;
                    unsigned int h[5];
                    handFromEntity(sq[i], h);
                    InvItemEntry seen[MAX_ITEMS];
                    unsigned int cnt = fresh ? engine::captureContainerContents(ctx.gw, h, seen, MAX_ITEMS, 0) : 0;
                    if (cnt > most) { most = cnt; corpse = (int)i; }
                }
                if (dst < 0 && !(sq[i].bodyState & BODY_DEAD) && ctx.ownTab &&
                    ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial) &&
                    (!dst_ || c == dst_)) dst = (int)i;
            }
            // 死亡赛：记下本机有没有活人可放，最后判定时没有活人的一方不要求搜到东西
            if (deathRace_ && dst >= 0) dst_ = engine::resolve(sq[dst]);
            unsigned int entries = 0;
            if (corpse >= 0 && dst >= 0) {
                unsigned int ch[5], dh[5], hash = 0;
                handFromEntity(sq[corpse], ch);
                handFromEntity(sq[dst], dh);
                InvItemEntry items[MAX_ITEMS];
                entries = engine::captureContainerContents(ctx.gw, ch, items, MAX_ITEMS, &hash);
                for (unsigned int k = 0; k < entries; ++k) {
                    if (ctx.isHost && !race_ && !deathRace_ && k % 2 == 1) continue;
                    int got = engine::moveItemBetweenContainers(ctx.gw, ch, dh, items[k].stringID,
                                                               items[k].itemType,
                                                               deathRace_ ? 1 :
                                                               items[k].quantity ? items[k].quantity : 1);
                    moved_ += got;
                    char m[200];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO LOOTITEM who=%s k=%u sid='%s' type=%u qty=%u eq=%u got=%d",
                              ctx.isHost ? "host" : "join", k, items[k].stringID, items[k].itemType,
                              (unsigned int)items[k].quantity, (unsigned int)items[k].equipped, got);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                    if (deathRace_ && got > 0) break;
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
        // 死亡赛：第 9 个整点核对（主机沿用 HOST_EXTRA_SLOTS 晚两个整点再核对，免得先退出把加入方断开）
        unsigned long finalSlot = firstSlot_ +
            (deathRace_ ? DEATHRACE_FINAL_SLOT : END_SLOTS + (race_ ? RACE_EXTRA_SLOTS : 0)) +
            (ctx.isHost ? HOST_EXTRA_SLOTS : 0);
        if (slot < finalSlot) return false;

        unsigned int dead[2] = { 0, 0 };
        bool haveDead = engine::readDeadSquadTab(ctx.gw, dead);
        EntityState sq[MAX_BODIES];
        Character* now[MAX_BODIES];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_BODIES);
        for (unsigned int i = 0; i < n; ++i) now[i] = engine::resolve(sq[i]);
        unsigned int vanished = 0, newlyDead = 0, misplaced = 0;
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
        int keyNow[MAX_ITEMS];
        for (unsigned int k = 0; k < nKeys_; ++k) keyNow[k] = 0;
        for (unsigned int i = 0; loot_ && i < n; ++i) {
            unsigned int h[5], hash = 0;
            handFromEntity(sq[i], h);
            InvItemEntry items[MAX_ITEMS];
            unsigned int cnt = engine::captureContainerContents(ctx.gw, h, items, MAX_ITEMS, &hash);
            bool isCorpse = corpse_ && now[i] == corpse_;
            if (isCorpse) corpseLeft = (int)cnt;
            for (unsigned int j = 0; j < cnt; ++j)
                for (unsigned int k = 0; k < nKeys_; ++k)
                    if (keys_[k].itemType == items[j].itemType && strcmp(keys_[k].stringID, items[j].stringID) == 0)
                        keyNow[k] += items[j].quantity < 1 ? 1 : (int)items[j].quantity;
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LOOTINV who=%s idx=%u ser=%u dead=%d corpse=%d count=%u hash=%08x",
                      ctx.isHost ? "host" : "join", sq[i].hIndex, sq[i].hSerial,
                      (sq[i].bodyState & BODY_DEAD) ? 1 : 0, isCorpse ? 1 : 0, cnt, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 抢尸体：尸体里的每样东西在所有身体上的总数要与开始时相同
        int keyBad = 0;
        for (unsigned int k = 0; k < nKeys_; ++k) {
            if (keyNow[k] != keyStart_[k]) ++keyBad;
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTKEY final who=%s sid='%s' type=%u start=%d now=%d",
                      ctx.isHost ? "host" : "join", keys_[k].stringID, keys_[k].itemType,
                      keyStart_[k], keyNow[k]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (race_) {
            _snprintf(b, sizeof(b) - 1, "SCENARIO LOOTRACE final who=%s rank=%u keys=%u keyBad=%d ui=%d filled=%d",
                      ctx.isHost ? "host" : "join", ctx.ownRank, nKeys_, keyBad, uiOk_, filledN_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (deathRace_)
            passed_ = victimOk && vanished == 0 && misplaced == 0 && newlyDead >= 3 && edited_ > 0 &&
                      (!dst_ || moved_ > 0);
        else if (loot_)
            passed_ = (ctx.ownRank == 1 ? victimOk : moved_ > 0) && corpseLeft == 0 &&
                      vanished == 0 && misplaced == 0 && newTabs == 0 && newlyDead >= 1 &&
                      (!race_ || (nKeys_ > 0 && keyBad == 0 && (race_ < 2 || uiOk_)));
        else
            passed_ = victimOk && vanished == 0 && misplaced == 0 && newTabs == 0 && newlyDead >= 3;
        if (deathRace_) {
            _snprintf(b, sizeof(b) - 1, "SCENARIO DEATHRACE final who=%s dead=%u edited=%d",
                      ctx.isHost ? "host" : "join", newlyDead, edited_ > 0 ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
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
    // 死亡赛在第 2 个整点改尸体背包、第 4 个整点搜别人的尸体、第 9 个整点核对
    static const unsigned long DEATHRACE_LOOT_SLOT  = 4;
    static const unsigned long DEATHRACE_FINAL_SLOT = 9;
    // 主机多等两个整点再核对，免得先退出把加入方断开
    static const unsigned long HOST_EXTRA_SLOTS = 2;
    // 抢尸体多等四个整点：关窗后的收回、对账和大背包 30 秒一次的补发
    static const unsigned long RACE_EXTRA_SLOTS = 4;
    static const unsigned long UI_CLOSE_SLOT    = 7;
    static const unsigned long MARK_DELAY_MS    = 1000;
    static const int           FILL_MAX         = 400;
    static const float         FULL_HUNGER;

    bool          loot_;
    unsigned long firstSlot_;
    bool          killed_;
    bool          looted_;
    int           edited_;
    int           moved_;
    Character*    victim_;
    Character*    startAlive_[MAX_BODIES];
    Character*    corpse_;
    unsigned int  nStart_;
    unsigned int  startTabs_[MAX_TABS][2];
    unsigned int  nTabs_;
    int           race_;
    bool          deathRace_;
    bool          uiOpened_;
    bool          uiClosed_;
    int           uiOk_;
    int           uiLast_;
    Character*    uiChar_;
    bool          marked_;
    bool          filled_;
    int           filledN_;
    Character*    dst_;
    InvItemEntry  keys_[MAX_ITEMS];
    int           keyStart_[MAX_ITEMS];
    unsigned int  nKeys_;
    Character*    markWho_[MAX_BODIES];
    int           markBase_[MAX_BODIES];
    unsigned int  nMark_;
};

// 饥饿值的引擎刻度约 0～3（ReplicatorChannels.cpp 协议 29 的注释）
const float OwnerDeathScenario::FULL_HUNGER = 3.0f;

// wound_kit（手动包扎测试的布置，不判定）：布防时各方把自己名下的活人喂饱、四肢伤到 40、
// 血量降到 70，并各发 2 个基础急救包（209-gamedata.base）。之后一直不结束，游戏保持运行，
// 由人手动换队、互相包扎，看 [med] TREAT SEND / TREAT RECV。需要 KENSHICOOP_INV_SYNC=1。
// kill_hold（同一个类）：不弄伤、不发急救包，三方在同一个 4 秒整点各把自己名下的活人头部伤到致命
// （与 death_race 同一条判死路），各机阵亡编队编号因此错开；之后同样不结束，供手动从尸体上丢装备。
class WoundKitScenario : public TimedScenario {
public:
    explicit WoundKitScenario(bool kill = false)
        : TimedScenario(kill ? "kill_hold" : "wound_kit", 0), kill_(kill), killed_(false), slot_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        slot_ = (GetTickCount() + 8000) / 4000 + 1;
        if (kill_) return;
        EntityState sq[64];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, 64);
        for (unsigned int i = 0; i < n; ++i) {
            if ((sq[i].bodyState & BODY_DEAD) || !ctx.ownTab ||
                !ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) continue;
            unsigned int h[5];
            handFromEntity(sq[i], h);
            engine::writeHungerByHand(h, 3.0f, -1.0f);
            bool ok = engine::woundSubjectLimbs(ctx.gw, h, 40.0f, 70.0f);
            int kits = engine::addItemsToContainerBySid(ctx.gw, h, "209-gamedata.base", 4, 2, 0, "", "");
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WOUNDKIT who=%s hand=%u,%u,%u,%u,%u wound=%d kits=%d",
                      ctx.isHost ? "host" : "join", h[0], h[1], h[2], h[3], h[4], ok ? 1 : 0, kits);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!kill_ || killed_ || GetTickCount() / 4000 < slot_) return false;
        killed_ = true;
        EntityState sq[64];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, 64);
        for (unsigned int i = 0; i < n; ++i) {
            if ((sq[i].bodyState & BODY_DEAD) || !ctx.ownTab ||
                !ctx.ownTab(sq[i].hContainer, sq[i].hContainerSerial)) continue;
            Character* c = engine::resolve(sq[i]);
            int heads = 0;
            __try {
                MedicalSystem* med = c ? &c->medical : 0;
                for (unsigned int k = 0; med && k < (unsigned int)med->anatomy.count; ++k) {
                    MedicalSystem::HealthPartStatus* p = med->anatomy.stuff[k];
                    if (!p || p->whatAmI != MedicalSystem::HealthPartStatus::PART_HEAD) continue;
                    p->flesh     = -(p->_maxHealth * 3.0f + 100.0f);
                    p->fleshStun = p->flesh;
                    ++heads;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                heads = -2;
            }
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SCENARIO KILLHOLD who=%s idx=%u ser=%u tab=%u,%u heads=%d",
                      ctx.isHost ? "host" : "join", sq[i].hIndex, sq[i].hSerial,
                      sq[i].hContainer, sq[i].hContainerSerial, heads);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        return false;
    }

private:
    bool          kill_;
    bool          killed_;
    unsigned long slot_;
};

} // namespace

Scenario* makeMedicalScenario(const std::string& name) {
    if (name == "medic_order")  return new MedicOrderScenario();
    if (name == "limb_loss")    return new LimbLossScenario();
    if (name == "crawl_move")   return new CrawlMoveScenario();
    if (name == "stats_sync")   return new StatsSyncScenario();
    if (name == "owner_death")  return new OwnerDeathScenario(false);
    if (name == "corpse_loot")  return new OwnerDeathScenario(true);
    if (name == "death_race")   return new OwnerDeathScenario(true, 0, true);
    if (name == "loot_race")      return new OwnerDeathScenario(true, 1);
    if (name == "loot_race_ui")   return new OwnerDeathScenario(true, 2);
    if (name == "loot_race_full") return new OwnerDeathScenario(true, 3);
    if (name == "wound_kit")      return new WoundKitScenario();
    if (name == "kill_hold")      return new WoundKitScenario(true);
    return 0;
}

} // namespace coop
