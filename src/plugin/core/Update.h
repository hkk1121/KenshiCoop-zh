// Update - 从 GitHub 发布自动更新（查版本、下载、核对 SHA256、替换文件）。
//
// 用户的决定（提示词/交接_20261111_自动更新与下载代理.md）：插件从
// hkk1121/KenshiCoop-zh 的 GitHub 发布读更新，发现新版本就自动下载替换，重启游戏
// 生效，F2 面板显示状态。发布格式见交接文档第三节：Release 附件带 update.json
// （扁平键值，键见下）+ 4 个文件；本机 mods/KenshiCoop/update.json 就是“已安装版本”，
// 没有它（开发版、本机直接编译安装的 DLL）只检查、只显示，不自动替换。
//
// 约束：VS2010（v100）+ Windows 自带库（WinHTTP 下载、BCrypt 算 SHA256、CreateThread
// 跑后台线程）。后台线程只做网络与文件操作，不碰引擎对象、MyGUI；和主线程只通过下面的
// Status 快照交换（临界区保护），由面板每帧读取。
//
// 日志（英文，标签 [update]，格式固定，判定脚本依赖这些措辞）：
//   [update] check start proxy=<none|system|host:port>
//   [update] latest build=... commit=... local=<build|dev> -> <up-to-date|update|dev-skip>
//   [update] downloaded name=... bytes=... sha=ok|BAD
//   [update] replaced files=N restart required
//   [update] failed stage=<check|download|verify|replace> err=<WinHTTP 或 Win32 错误码> http=<状态码>

#ifndef KENSHICOOP_UPDATE_H
#define KENSHICOOP_UPDATE_H

#include <string>

namespace coop {
namespace update {

// 面板显示阶段
enum Stage {
    STAGE_IDLE = 0,     // 未检查
    STAGE_CHECKING,     // 正在检查
    STAGE_UP_TO_DATE,   // 已是最新
    STAGE_DOWNLOADING,  // 正在下载 n/N
    STAGE_UPDATED,      // 已更新，重启游戏生效（本局不再检查）
    STAGE_DEV_BUILD,    // 开发版，不自动更新
    STAGE_FAILED        // 失败（原因在 Status::reason）
};

// 主线程可见的状态快照。定长字符数组：后台线程写、主线程读，整体在临界区里拷贝。
struct Status {
    int  stage;            // Stage
    char latestBuild[24];  // 远端 buildTime（还不知道时为空串）
    char latestCommit[24]; // 远端 commit（同上）
    char localBuild[24];   // 本机 buildTime（开发版为空串）
    char reason[80];       // 失败原因（面板显示）："err=<错误码> http=<状态码>"，日志里另有完整 stage=
    int  failedStage;      // 失败发生在哪一步：0 检查 / 1 下载 / 2 校验 / 3 替换（stage 不为 FAILED 时无意义）
    int  fileIndex;        // 正在下载第几个文件（1 起；不在下载阶段时是 0 或上一次的值）
    int  fileCount;        // 本次要下载的文件总数
    int  protocolChanged;  // 1 = 远端 protocol 与本机协议号不同（面板提示所有玩家都要更新）
};

// 启动（主线程，startPlugin 调用一次）：
//   * 清理上次替换残留的 *.old 与 update_tmp/（删不掉就跳过）；
//   * 记下下载代理（空 = 系统代理；非空先按规则校验，非法则记日志并改用系统代理）；
//   * 起后台线程，autoCheck 为真时约 10 秒后自动检查一次（整局只自动一次）；为假时
//     线程只等 F2 的手动检查。
void startup(const std::string& proxy, bool autoCheck);

// 退出（主线程，SteamAPI_Shutdown 钩子调用）：请求后台线程停止，等它收尾（上限 16 秒，
// 盖住一次卡住的 WinHTTP 阻塞调用）。等到就关句柄；等不到（线程正卡在网络调用里）
// 就保留句柄让进程退出时一起结束，避免关掉的句柄被复用后线程再引用。
// 没启动过时是空操作。
void shutdown();

// 手动检查（F2“检查更新”按钮）：正在检查/下载中，或已更新待重启时忽略。后台线程进行。
void checkNow();

// 主线程读当前状态快照（线程安全）。
void getStatus(Status& out);

// 当前下载代理串：空 = 用系统代理，否则 "host:port"（面板状态行用）。
std::string proxyText();

// 面板“粘贴代理地址”：raw 是剪贴板原文（首尾空白在这里去掉）。合法则写回
// coop_config.json 并立即对后续检查生效。
// 返回：0 成功；1 剪贴板为空；2 不支持 SOCKS；3 不是合法的 host:port；4 写配置失败。
int applyProxyText(const std::string& raw);

// 面板“清除代理”：写回空值，恢复用系统代理。
// 返回：0 成功；4 写配置失败。
int clearProxy();

} // namespace update
} // namespace coop

#endif // KENSHICOOP_UPDATE_H
