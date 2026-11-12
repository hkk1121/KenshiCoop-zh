# KenshiCoop-zh

[Kenshi](https://lofigames.com/) 联机插件 [KenshiCoop](https://github.com/nhoral/KenshiCoop) 的中文化与改进版，
以 [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) 插件的形式运行。

一名玩家当主机开自己的世界，好友通过 Steam（或局域网 / 直连 UDP）加入，各自带自己的编队在同一个世界里玩。
插件同步编队、NPC、战斗、背包与装备、玩家之间的交易、丢在地上的物品、基地建造与容器内容、游戏速度等；
任何一方存档都会变成一份共用存档，自动传给所有人。

> **状态：开发中。** 这是业余项目，可能遇到不同步、闪退等问题。
> 本版联机协议为 **1007**，与官方版 KenshiCoop（v0.51）**不互通**，所有玩家必须安装同一个版本。

## 相对官方版的主要改动

- **简体中文界面**：F2 联机面板、状态条、提示全部中文（按游戏语言自动切换），附带 RE_Kenshi 设置界面的中文翻译。
- **最多 4 人联机**：主机负责转发，加入方只连主机；第 3、4 人加入不会让前面的人重新联机。
- **每人独立编队**：自己招募或新建的角色只进自己的编队，只能操控自己的编队；归属写进主机存档，掉线后保留。
- **编队授权**：可以向好友申请控制某个编队，对方同意、拒绝、随时收回，借用方也可以主动归还。
- **玩家框**：编队栏上方显示每位玩家的名字、在线状态、编队和成员，可隐藏。
- **F2 创建角色**：加入方进世界后在 F2 面板选原版开局，用游戏自带的捏人界面建自己的角色，可反复创建。
- **所有原版开局都能联机**：开局列表里有“联机：<开局名>”系列。
- **每人独立资金**；科技和势力关系全队共用。
- **Steam 好友房间**：主机不用填好友 ID，好友在房间列表里点一下就能连；也可以粘贴主机 Steam ID 先看房间信息。
- **自动更新**：启动后从本仓库的 Release 检查并下载新版本，重启游戏生效；直连 GitHub 慢时可以在 F2 面板粘贴 HTTP 代理地址。
- **帧数上限**：F2 面板里切换 不限 / 100 / 200 / 300 / 400 / 500。
- 修复用户名或存档路径含中文时存档传输失败等问题。

## 安装

需要 **Steam 版 Kenshi 1.0.65**，所有玩家的 Steam 都要在线。

1. 退出 Kenshi。
2. 从 [最新 Release](https://github.com/hkk1121/KenshiCoop-zh/releases/latest) 下载 `KenshiCoop-zh-<版本>.zip`，
   把整个压缩包解压出来（不要在压缩软件里直接运行）。
3. 双击 `install.cmd`，按提示操作：
   - 自动找到 Steam 版 Kenshi 的游戏目录，找不到时把游戏文件夹拖进窗口；
   - 第一次安装会打开包里自带的 RE_Kenshi 安装程序，在里面选择 Kenshi 游戏目录并安装，装完关掉它，脚本会继续；
   - 自动在游戏的 Mods 列表里启用 KenshiCoop；你自己改过的 `coop_config.json` 不会被覆盖。
4. 看到“安装完成”即可。

装过 `20261112-1632` 及之后的版本后，以后会自动更新，不用再手动下载。
F2 面板的“更新”一栏显示当前状态；下载慢时点“粘贴代理地址”填入 HTTP 代理（例如 `127.0.0.1:10808`，混合端口也可以），留空则走系统代理。

## 联机

1. 所有人安装同一个版本，启动游戏，按 **F2** 打开联机面板（主菜单和游戏内都能用）。
2. **主机**：读取存档或用原版开局建好世界，把“角色”设为“主机”，连接方式选 STEAM，切换为“在线”，好友房间就开好了。
3. **好友**：在主菜单按 F2，把“角色”设为“加入”，连接方式选 STEAM，在“房间列表”里点主机的房间即可连接。
   也可以让主机“复制我的 Steam ID”发过来，粘贴后先看主机名、存档、人数和版本，再切换为“在线”。
4. 等主机的世界传输完、自动读档进入世界。没有自己的角色时，按 F2 选一个开局，点“开始所选开局”，
   在游戏自带的捏人界面里捏好角色并确认。
5. 左上角状态条显示连接状态；切回“离线”即退出联机。

默认只接受 Steam 直接好友加入，主机粘贴的 Steam ID 可以额外放行。
局域网 / Tailscale / 直连 UDP 也能用：把连接方式切到 UDP，在 `<Kenshi>\mods\KenshiCoop\coop_config.json` 里填主机的 `ip` / `port`，
或者用面板里的“扫描主机”。

完整的使用说明（玩家框、编队授权、创建角色的细节和注意事项）在发布包里的 `README.txt`。

### 出问题时

- **连不上**：确认双方 Steam 在线且互为好友、主机已切到“在线”、双方版本相同，面板和状态条会提示具体原因。
- **提示版本不一致**：所有人更新到同一个版本（`VERSION.txt` 里的“代码版本”要一致）。
- **插件没有加载**：看 `<Kenshi>\RE_Kenshi_log.txt` 里有没有 `KenshiCoop`，重装 RE_Kenshi 通常能解决。
- 反馈问题时请附上游戏目录下的 `KenshiCoop_host.log` 或 `KenshiCoop_join.log`。

## 工作原理

- `KenshiCoop.dll` 由 RE_Kenshi 载入游戏，通过 KenshiLib 挂接引擎，所有对游戏的改动都在主线程执行。
- 网络用 [ENet](https://github.com/lsalzman/enet)（UDP），可选 Steam P2P 隧道，不需要端口转发。
- 主机对世界有最终决定权，每位玩家对自己的编队有最终决定权。引擎接口与网络协议见 `docs/API_REFERENCE.md`（英文）。

```
src/plugin/       KenshiCoop 插件本体（网络、同步、引擎封装、测试场景）
src/netproto/     共用的网络协议头文件（C++03）
src/netsim/       协议模拟器
src/prototest/    网络协议单元测试
src/tunneltest/   Steam 隧道套接字挂钩测试
scripts/          编译、部署、联机会话、自动化测试脚本（PowerShell）
docs/             编译指南、引擎与接口参考、同步踩坑记录（英文，沿用官方版）
third_party/      ENet 补丁、VC10 兼容层（依赖需另行获取，不在仓库里）
```

## 编译

插件必须用 **Visual C++ 2010（v100）x64 工具集** 编译（KenshiLib 的要求）。
完整的工具链搭建和注意事项见 [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md)（英文）。准备好之后：

```bash
cmd //c scripts/build_plugin.cmd
```

依赖需另行获取：

- KenshiLib 和预编译库：把
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  克隆到 `third_party/KenshiLib_deps/`
- ENet：把 [lsalzman/enet](https://github.com/lsalzman/enet) 克隆到 `third_party/enet/enet/`，
  再打上 `third_party/enet/patches/` 里的补丁（见 `third_party/enet/README.md`）

## 致谢

- [nhoral](https://github.com/nhoral) - KenshiCoop 原作者，本仓库在其基础上修改
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) - RE_Kenshi 和 KenshiLib，有了它们才能做这类插件
- [lsalzman/enet](https://github.com/lsalzman/enet) - UDP 网络库
- [zeroit789](https://github.com/zeroit789) - 双流浪者联机开局（[#15](https://github.com/nhoral/KenshiCoop/pull/15)）
- Lo-Fi Games - Kenshi

## 许可

[AGPL-3.0](LICENSE)。KenshiLib 和 RE_Kenshi 采用 GPLv3，本插件按 GPLv3 第 13 条（GPL / AGPL 组合）链接 KenshiLib。
与 Lo-Fi Games 无关联，非商业的爱好者项目。
