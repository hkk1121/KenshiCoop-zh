#include "FpsLimit.h"
#include "Config.h"
#include "../CoopLog.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

// OgreRoot.h 经 Ogre 线程头文件自动链接 boost_thread（本插件不需要，也没有该库），关掉自动链接
#define BOOST_ALL_NO_LIB
#include <ogre/OgreRoot.h>
#include <ogre/OgreFrameListener.h>

namespace coop {
namespace fpslimit {

namespace {

const int kSteps[] = { 0, 100, 200, 300, 400, 500 };

int           g_limit = 0;
bool          g_installed = false;
LARGE_INTEGER g_freq;
LARGE_INTEGER g_next; // 下一帧最早开始时刻（QPC），0 = 未对齐

class Limiter : public Ogre::FrameListener {
public:
    bool frameEnded(const Ogre::FrameEvent& evt) {
        (void)evt;
        if (g_limit <= 0) { g_next.QuadPart = 0; return true; }
        LONGLONG period = g_freq.QuadPart / g_limit;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        // 首帧或落后超过一帧（读档、切场景卡顿）时重新对齐，不补帧
        if (g_next.QuadPart == 0 || now.QuadPart - g_next.QuadPart > period) {
            g_next.QuadPart = now.QuadPart + period;
            return true;
        }
        // 剩余超过 2ms 时 Sleep(1)（已 timeBeginPeriod(1)），最后一段自旋保证 500 帧也准
        while (g_next.QuadPart - now.QuadPart > g_freq.QuadPart / 500) {
            Sleep(1);
            QueryPerformanceCounter(&now);
        }
        while (now.QuadPart < g_next.QuadPart) {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        }
        g_next.QuadPart += period;
        return true;
    }
};

Limiter g_listener;

} // namespace

void install() {
    if (g_installed) return;
    Ogre::Root* root = Ogre::Root::getSingletonPtr();
    if (!root) {
        logErrLine("[fps] Ogre root not ready; FPS limit disabled");
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    g_next.QuadPart = 0;
    g_limit = loadFpsLimit();
    timeBeginPeriod(1);
    root->addFrameListener(&g_listener);
    g_installed = true;
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[fps] limiter installed limit=%d", g_limit);
    b[sizeof(b) - 1] = '\0';
    logLine(b);
}

int current() { return g_limit; }

void cycle() {
    int n = sizeof(kSteps) / sizeof(kSteps[0]);
    int i = 0;
    while (i < n && kSteps[i] != g_limit) ++i;
    g_limit = kSteps[(i + 1) % n];
    g_next.QuadPart = 0;
    saveFpsLimit(g_limit);
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[fps] limit -> %d", g_limit);
    b[sizeof(b) - 1] = '\0';
    logLine(b);
}

} // namespace fpslimit
} // namespace coop
