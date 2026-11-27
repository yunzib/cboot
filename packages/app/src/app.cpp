// cboot.app 实现单元 — 优雅停机信号处理
//   - Windows：控制台事件（Ctrl+C / CLOSE / SHUTDOWN）
//   - POSIX：SIGTERM / SIGINT → 自管道 → 看门线程调用 stop()
//     （信号处理器内只做 async-signal-safe 的 write，不碰锁）
// GMF 不跨单元共享：系统头与守卫宏在本单元自带
module;

#ifdef _WIN32
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MMINTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

module cboot.app;

import std;
import cboot.web.server;

namespace detail {
namespace {

#ifdef _WIN32

server* g_active = nullptr;      // 单应用约定（run 只应调用一次）

BOOL WINAPI on_console_event(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_active) g_active->stop();     // 优雅停机：停 accept → 排空 → PreDestroy
        return TRUE;                        // 已处理，阻止默认强杀（CLOSE 有超时限制）
    default:
        return FALSE;
    }
}

#else  // POSIX

server* g_active = nullptr;      // 由看门线程读取；置空在 join 之后
int g_pipe[2] = {-1, -1};

/**
 * @brief 信号处理器：只 write 一个字节（async-signal-safe）
 *
 * 不在处理器里调用 server::stop()——其中含互斥量/条件变量，
 * 不是 async-signal-safe；唤醒看门线程由它做真正的停机。
 */
extern "C" void on_posix_signal(int) {
    const char byte = 1;
    if (g_pipe[1] >= 0) {
        const ssize_t ignored = ::write(g_pipe[1], &byte, 1);
        (void)ignored;
    }
}

/** @brief 看门线程：读到 1 → stop()；读到 0 → 退出 */
std::thread g_watcher;

#endif

} // namespace

#ifdef _WIN32

void install_stop_handler(server& s) {
    g_active = &s;
    ::SetConsoleCtrlHandler(on_console_event, TRUE);
}

void remove_stop_handler() {
    ::SetConsoleCtrlHandler(on_console_event, FALSE);
    g_active = nullptr;
}

#else

void install_stop_handler(server& s) {
    g_active = &s;
    if (::pipe(g_pipe) != 0) return;      // 极端环境：退化为默认信号行为

    struct sigaction sa {};
    sa.sa_handler = on_posix_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                      // 不设 SA_RESTART：让阻塞系统调用返回
    ::sigaction(SIGTERM, &sa, nullptr);   // k8s 滚动更新/缩容
    ::sigaction(SIGINT, &sa, nullptr);    // Ctrl+C

    g_watcher = std::thread([] {
        char byte = 0;
        while (::read(g_pipe[0], &byte, 1) == 1) {
            if (byte == 1 && g_active) g_active->stop();
            else if (byte == 0) return;   // remove_stop_handler 的退出哨兵
        }
    });
}

void remove_stop_handler() {
    if (g_pipe[1] >= 0) {                 // 哨兵 0：让看门线程退出
        const char stop = 0;
        const ssize_t ignored = ::write(g_pipe[1], &stop, 1);
        (void)ignored;
    }
    if (g_watcher.joinable()) g_watcher.join();
    g_active = nullptr;
    if (g_pipe[0] >= 0) ::close(g_pipe[0]);
    if (g_pipe[1] >= 0) ::close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
    ::signal(SIGTERM, SIG_DFL);           // 恢复默认，避免二次 Ctrl+C 不退出
    ::signal(SIGINT, SIG_DFL);
}

#endif

} // namespace detail
