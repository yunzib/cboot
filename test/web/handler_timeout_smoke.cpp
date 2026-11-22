// 请求级超时（handler SLA）冒烟：超预算 → 504；正常 → 200
// 构建时注入 CBOOT_TEST_IO=1 切换 IOCP 后端
// 说明：同步 handler 无法安全抢占——504 为 SLA 语义（执行完成后判定）。
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

struct SlowApi {
    [[=Get{"/slow"}]]
    Response slow() {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return Response::text("slow");
    }
    [[=Get{"/fast"}]]
    Response fast() { return Response::text("fast"); }
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

int get_status(unsigned short port, std::string_view path) {
    SOCKET s = connect_to(port);
    if (s == INVALID_SOCKET) return 0;
    std::string req = "GET " + std::string(path) + " HTTP/1.1\r\nHost: t\r\n\r\n";
    ::send(s, req.data(), int(req.size()), 0);
    char buf[2048];
    const int n = ::recv(s, buf, sizeof buf, 0);
    ::closesocket(s);
    if (n <= 0) return 0;
    std::string_view resp(buf, std::size_t(n));
    const std::size_t sp = resp.find(' ');
    if (sp == std::string_view::npos) return 0;
    int code = 0;
    std::from_chars(resp.data() + sp + 1, resp.data() + resp.size(), code);
    return code;
}

int main() {
    server_config cfg{.port = 0, .workers = 2, .handler_timeout_ms = 50};
    cfg.io = CBOOT_TEST_IO ? server_io::iocp : server_io::threads;

    server srv(cfg);
    SlowApi api;
    srv.mount(api);
    std::jthread t([&] { (void)srv.run(); });

    for (int i = 0; i < 200 && srv.port() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const unsigned short port = srv.port();
    if (port == 0) { srv.stop(); return 100; }

    if (get_status(port, "/fast") != 200) { srv.stop(); return 1; }
    if (get_status(port, "/slow") != 504) { srv.stop(); return 2; }

    srv.stop();
    std::println("HANDLER TIMEOUT SMOKE: PASS");
    return 0;
}
