// FpsLimit - 帧数上限（游戏设置里没有，只有垂直同步）。
//
// Kenshi 用 Ogre::Root::startRendering 驱动主循环，这里注册一个 Ogre FrameListener，
// 在每帧 frameEnded（画面已提交）时等到目标间隔，主菜单与游戏内都生效。
// 上限在 F2 面板点击切换：不限 -> 100 -> 200 -> 300 -> 400 -> 500 -> 不限，
// 存于 DLL 旁的 coop_fps_limit.txt，下次启动沿用。仅主线程调用。
#ifndef KENSHICOOP_FPSLIMIT_H
#define KENSHICOOP_FPSLIMIT_H

namespace coop {
namespace fpslimit {

// 读取保存的上限并注册帧监听器；启动时调用一次，重复调用忽略
void install();

// 当前上限，0 = 不限
int current();

// 切换到下一档并保存
void cycle();

} // namespace fpslimit
} // namespace coop

#endif
