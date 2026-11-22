// cboot.app 实现单元 — 优雅停机信号处理（Windows 控制台事件）
// GMF 不跨单元共享：windows.h 与守卫宏在本单元自带
module;

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

module cboot.app;

import std;
import cboot.web.server;

namespace detail {
namespace {

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

} // namespace

void install_stop_handler(server& s) {
    g_active = &s;
    ::SetConsoleCtrlHandler(on_console_event, TRUE);
}

void remove_stop_handler() {
    ::SetConsoleCtrlHandler(on_console_event, FALSE);
    g_active = nullptr;
}

} // namespace detail
