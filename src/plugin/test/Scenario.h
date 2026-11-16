// Scenario - deterministic, log-emitting test scripts driven from the tick hook.
//
// A scenario runs on BOTH clients (it branches on ctx.isHost). The authoritative
// side emits "SCENARIO MEMBER hand=.. pos=.." lines; the observing side emits
// "SCENARIO RECV hand=.. pos=.." lines. The PowerShell runner cross-checks
// MEMBER vs RECV positions per hand within a tolerance, in addition to requiring
// "SCENARIO RESULT PASS" from each client. Keep these schema strings stable -
// run_test.ps1 parses them.

#ifndef KENSHICOOP_SCENARIO_H
#define KENSHICOOP_SCENARIO_H

#include <string>
#include "../../netproto/Wire.h"

class GameWorld;

namespace coop {

struct ScenarioProxyInfo; // proxy_far_fix 的代理快照，定义在本文件末尾

struct ScenarioContext {
    GameWorld*    gw;
    bool          isHost;
    u32           localId;
    unsigned long elapsedMs; // since onStart
    unsigned int  tick;      // scenario tick counter
    // True once this client has received an owned-entity batch from a peer. On the
    // HOST this means the JOIN is loaded + streaming, so it is the correct gate for
    // any time-sensitive host action (e.g. a live spawn) that the join must witness.
    // Always false in HostOnly runs (no peer) - gate with a timeout fallback there.
    bool          peerReady;
    // assault_mint: describes the MINTED proxy nearest the body at refHand -
    // outLocal[5] is the hand that resolves on THIS client (what an order needs),
    // outCanon[5] the peer's key it is streamed by, outDist the planar distance.
    // Only the replicator's proxy table can answer this, and a scenario must not
    // reach into the replicator, so Plugin.cpp supplies the adapter (0 when
    // unavailable - always check).
    bool        (*pickMintedProxy)(const unsigned int refHand[5],
                                   unsigned int outLocal[5],
                                   unsigned int outCanon[5], float* outDist);
    // The squad-tab rank this client OWNS (lowest of Config.ownRanks - the same
    // partition the Replicator streams by). Protocol 49: a third player claims
    // rank 2, so scenarios must NOT hardcode isHost ? 0 : 1 - a join-2 instance
    // would try to drive join-1's member. 2-player behavior is unchanged (host
    // resolves to 0, the classic join to 1).
    unsigned int  ownRank;
    // 编队标签 (container, containerSerial) 是否是本机玩家的编队（非 0 = 是；借到别人的
    // 编队期间只认借来的）。Plugin.cpp 用编队栏的判定填入；会话没开始或归属表没到时
    // 一律返回非 0。
    int         (*ownTab)(unsigned int container, unsigned int containerSerial);
    // 编队标签的永久主人身份（0 = 归属表里没有这一行）。授权申请要按对方的稳定身份指定目标。
    u64         (*tabOwner)(unsigned int container, unsigned int containerSerial);
    // 编队标签的主人在本机的 rank（0xFFFFFFFF = 未知 / 归属表没到）。主人掉线后仍能查到，
    // 因为归属表是永久的。
    unsigned int (*tabOwnerRank)(unsigned int container, unsigned int containerSerial);
    // 某 rank 的玩家此刻是否在线（1/0，含本机）。主人掉线时其 PeerStatus 的 present 变 0。
    int         (*playerOnline)(unsigned int rank);
    // 归属表是否已接上（1/0）。没接上时 ownTab 对任何编队都返回非 0，不能拿来做基线。
    int         (*ownersReady)();
    // 命令分类（Replicator::orderClassForHand）：1 = 本机可控，2 = 对方控制，0 = 中立。
    // 参数是 readObjectHand 布局的 5 元手把。
    int         (*orderClass)(const unsigned int hand[5]);
    // 小队控制授权状态查询（authState 的 which 参数）。
    enum {
        AUTH_PENDING_PEER = 0,  // 1 = 有对方申请待处理
        AUTH_GRANTED_MINE = 1,  // 已授权出去的编队数
        AUTH_BORROWED     = 2,  // 借到的编队数
        AUTH_REQUESTING   = 3   // 1 = 本端申请中 / 正在收回
    };
    // 小队控制授权动作（authAction 的 action 参数）。
    enum {
        AUTH_ACT_REQUEST = 0,
        AUTH_ACT_GRANT   = 1,
        AUTH_ACT_DENY    = 2,
        AUTH_ACT_REVOKE  = 3    // 授权出去的收回 / 借到的归还
    };
    int         (*authState)(int which);
    // targetPlayer 为对方的稳定身份（0 = 不按人筛选）；singleTab 为 {container,
    // containerSerial}，0 = 全部可申请 / 挂起的编队。
    void        (*authAction)(int action, u64 targetPlayer, const unsigned int singleTab[2]);
    // proxy_far_fix：逐条列出本机代理表里的代理（Harness 只读查询）。canon/local
    // 都是 readObjectHand 布局 [type, container, containerSerial, index, serial]；
    // canon 是主机流键，与主机 SCENARIO FARFIX SPAWN hand= 同序。outCensusR /
    // outAttnR 回填本端配置的 census / attention 半径（0 = 未知）。返回写入条数；
    // 适配函数缺失时一律 0。
    int         (*listProxies)(ScenarioProxyInfo* out, int maxOut,
                               float* outCensusR, float* outAttnR);
};

class Scenario {
public:
    virtual ~Scenario() {}
    virtual const char* name() const = 0;
    // Called EVERY tick between local gameplay start and peer-ready arming.
    // Scenarios that must capture a subject while the freshly-loaded world is
    // still in its baked pose (the craft worker at the prop, the duelists at
    // their spawn) PIN it on the first call and HOLD it on subsequent calls -
    // the arming wait (a join-load, ~10-20 s) is long enough for faction AI to
    // walk an unpinned subject away from where the save baked it. ctx.elapsedMs
    // here is time since gameplay start (NOT the armed scenario clock).
    virtual void onGameplay(const ScenarioContext&) {}
    // Called ONCE when the scenario ARMS: at peer-ready (the first owned-entity
    // batch from the peer - on the host, "the join is loaded + streaming"), or
    // at the arm-timeout fallback. ctx.elapsedMs is measured from THIS moment,
    // so every scripted action happens with the peer watching.
    virtual void onStart(const ScenarioContext& ctx) = 0;
    // Returns true when the scenario is complete (caller logs RESULT then holds).
    virtual bool onTick(const ScenarioContext& ctx) = 0;
    virtual bool passed() const = 0;
};

// Build the scenario named 'name', or 0 if unknown.
Scenario* makeScenario(const std::string& name);

// proxy_far_fix 场景的只读代理快照：只包含判定要用的字段，场景不能直接碰
// Replicator。d 是水平偏差（parkDivergedCopy 用的度量），d3 是三维偏差（[proxy]
// drift 用的度量），camd 是代理到本机兴趣锚点（各队队长 + 相机）的最近距离 -
// 也就是 wide pass 的扫描半径基准。haveCensus=0 时 d/d3 为 -1。
struct ScenarioProxyInfo {
    unsigned int canon[5];    // 主机流键（readObjectHand 布局）
    unsigned int local[5];    // 本机手把（readObjectHand 布局）
    float        localPos[3];
    float        censusPos[3];
    float        d;
    float        d3;
    float        camd;
    int          haveCensus;
    int          driven;
    int          frozen;
    int          haveDest;
};

} // namespace coop

#endif // KENSHICOOP_SCENARIO_H
