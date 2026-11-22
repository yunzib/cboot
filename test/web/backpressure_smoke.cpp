// 背压冒烟：max_connections 超限 → 503；释放后恢复 200
// 构建时注入 CBOOT_TEST_IO=1 切换 IOCP 后端（与 web_server_smoke 同款）
// 说明：接受/回收是异步的（IOCP 完成包 + sweeper），断言采用**轮询收敛**
// 而非固定 sleep，避免时序抖动导致偶发失败。
// GCC16 模块边界守卫
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MMINTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#include <winsock2.h>
#include <ws2tcpip.h>

import cboot.web.http;
import cboot.web.server;
import std;

#ifndef CBOOT_TEST_IO
#define CBOOT_TEST_IO 0
#endif

struct Api {
    [[=Get{"/ping"}]]
    Response ping() { return Response::text("pong"); }
};

SOCKET connect_to(unsigned short port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD tmo = 3000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tmo), sizeof tmo);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string roundtrip(SOCKET s, std::string_view req) {
    ::send(s, req.data(), int(req.size()), 0);
    std::string out;
    char buf[4096];
    int n;
    while ((n = ::recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, std::size_t(n));
    return out;
}

/** @brief 一次探测（新连接 → GET /ping → 返回状态码；失败 0） */
int probe_status(unsigned short port) {
    SOCKET s = connect_to(port);
    if (s == INVALID_SOCKET) return 0;
    const std::string resp = roundtrip(s, "GET /ping HTTP/1.1\r\nHost: b\r\n\r\n");
    ::closesocket(s);
    const std::size_t sp = resp.find(' ');
    if (sp == std::string::npos) return 0;
    int code = 0;
    std::from_chars(resp.data() + sp + 1, resp.data() + resp.size(), code);
    return code;
}

/** @brief 轮询直到探测状态 == expected（超时返回 false） */
bool wait_status(unsigned short port, int expected, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe_status(port) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return false;
}

int main() {
    server_config cfg{.port = 0, .workers = 2, .max_connections = 2};
    cfg.io = CBOOT_TEST_IO ? server_io::iocp : server_io::threads;

    server srv(cfg);
    Api api;
    srv.mount(api);
    std::jthread t([&] { (void)srv.run(); });

    for (int i = 0; i < 200 && srv.port() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const unsigned short port = srv.port();
    if (port == 0) { srv.stop(); return 100; }

    // 两条空闲连接占满上限（不发请求）
    SOCKET idle1 = connect_to(port);
    SOCKET idle2 = connect_to(port);
    if (idle1 == INVALID_SOCKET || idle2 == INVALID_SOCKET) {
        srv.stop();
        return 1;
    }

    // 稳定后应持续 503
    if (!wait_status(port, 503)) {
        std::println("expected 503 under saturation");
        srv.stop();
        return 2;
    }

    // 释放一条 → 轮询等待恢复 200（sweeper/reap 异步完成）
    ::closesocket(idle1);
    if (!wait_status(port, 200)) {
        std::println("expected recovery to 200");
        srv.stop();
        return 3;
    }

    ::closesocket(idle2);
    srv.stop();
    std::println("BACKPRESSURE SMOKE: PASS");
    return 0;
}
