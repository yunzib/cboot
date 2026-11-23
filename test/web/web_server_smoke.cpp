// cboot.web 服务器回环冒烟测试（winsock 客户端 → 嵌入式 server）
// 验证：临时端口 / GET+POST / 响应头 / 405+Allow / 413 / 并发 / keep-alive /
//       pipelining / Connection: close / 优雅停机
// GCC16 模块边界守卫（kwik-ui 同款）：winsock 文本链与 import std 的同名
// 声明冲突——C 头直取纯 C 版本 + intrin guard
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

import cboot.json;
import cboot.web.http;
import cboot.web.server;
import std;

struct UserDto {
    std::string name;
    int age{};
};

struct UserApi {
    [[=Get{"/users/{id}"}]]
    Response get_user(int id) {
        return json_response(UserDto{.name = "u" + std::to_string(id), .age = id});
    }
    [[=Post{"/users"}]]
    Response create_user(const UserDto& dto) {
        return json_response(UserDto{.name = dto.name + "!", .age = dto.age});
    }
    [[=Get{"/hdr"}]]
    Response hdr() {
        return json_response(UserDto{.name = "h", .age = 0}).header("X-Trace", "t1");
    }

    [[=Get{"/header"}]]
    Response header(const Request& req) {
        return json_response(std::string(req.header("X-Test").value_or("none")));
    }
};

// ── 客户端：连接 + 按 Content-Length 收完整响应（支持 keep-alive）──

SOCKET connect_to(unsigned short port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD tmo = 8000;
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

std::size_t response_content_length(std::string_view head) {
    std::string lower(head);
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    std::size_t v = lower.find_first_not_of(" \t", pos + 15);
    std::size_t out = 0;
    std::from_chars(lower.data() + v, lower.data() + lower.size(), out);
    return out;
}

// 从 buf（跨调用保留）中解析一个完整响应；不足则 recv
std::string read_response(SOCKET s, std::string& buf, bool* closed = nullptr) {
    if (closed) *closed = false;
    while (true) {
        std::size_t he = buf.find("\r\n\r\n");
        if (he != std::string::npos) {
            std::size_t cl = response_content_length(std::string_view(buf).substr(0, he));
            if (buf.size() >= he + 4 + cl) {
                std::string resp = buf.substr(0, he + 4 + cl);
                buf.erase(0, he + 4 + cl);
                return resp;
            }
        }
        char tmp[8192];
        int n = ::recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) {
            if (closed) *closed = true;
            std::string rest = std::move(buf);
            buf.clear();
            return rest;
        }
        buf.append(tmp, std::size_t(n));
    }
}

std::string body_of(const std::string& resp) {
    std::size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? std::string{} : resp.substr(p + 4);
}

// 一次性请求（读完整响应后关闭）
std::string one_shot(unsigned short port, std::string_view req) {
    SOCKET s = connect_to(port);
    if (s == INVALID_SOCKET) return {};
    ::send(s, req.data(), int(req.size()), 0);
    std::string buf;
    std::string resp = read_response(s, buf);
    ::closesocket(s);
    return resp;
}

#ifndef CBOOT_TEST_IO
#define CBOOT_TEST_IO 0        // 0 = threads 后端；1 = iocp 后端（构建时注入）
#endif

int main() {
    // ── 临时端口（0 = 系统分配）+ 4 工作线程；后端由构建选项决定 ──
    server srv(0, 4, CBOOT_TEST_IO ? server_io::iocp : server_io::threads);
    UserApi api;
    srv.mount(api);
    std::jthread t([&] { (void)srv.run(); });

    for (int i = 0; i < 200 && srv.port() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const unsigned short port = srv.port();
    if (port == 0) { srv.stop(); return 100; }

    // ── GET ──
    std::string resp = one_shot(port, "GET /users/42 HTTP/1.1\r\nHost: t\r\n\r\n");
    if (resp.find("200") == std::string::npos) { srv.stop(); return 1; }
    auto v = json::parse(body_of(resp));
    if (v.find("name")->as_string() != "u42" || v.find("age")->as_int() != 42) {
        srv.stop(); return 2;
    }

    // ── POST ──
    const std::string post_body = R"({"name":"bob","age":30})";
    resp = one_shot(port,
        "POST /users HTTP/1.1\r\nHost: t\r\nContent-Length: "
            + std::to_string(post_body.size()) + "\r\n\r\n" + post_body);
    v = json::parse(body_of(resp));
    if (v.find("name")->as_string() != "bob!" || v.find("age")->as_int() != 30) {
        srv.stop(); return 3;
    }

    // ── 响应头 ──
    resp = one_shot(port, "GET /hdr HTTP/1.1\r\nHost: t\r\n\r\n");
    if (resp.find("X-Trace: t1") == std::string::npos) { srv.stop(); return 4; }

    // ── 请求头透传（Authorization 等中间件依赖的路径）──
    resp = one_shot(port,
        "GET /header HTTP/1.1\r\nHost: t\r\nX-Test: hi\r\n\r\n");
    if (body_of(resp) != R"("hi")") { srv.stop(); return 15; }

    // ── 405 + Allow ──
    resp = one_shot(port, "DELETE /users/1 HTTP/1.1\r\nHost: t\r\n\r\n");
    if (resp.find("405") == std::string::npos || resp.find("Allow:") == std::string::npos) {
        srv.stop(); return 5;
    }

    // ── 413（Content-Length 超限，无需真发 body）──
    resp = one_shot(port,
        "POST /users HTTP/1.1\r\nHost: t\r\nContent-Length: 2000000\r\n\r\n");
    if (resp.find("413") == std::string::npos) { srv.stop(); return 6; }

    // ── keep-alive：同一连接上顺序两个请求 ──
    {
        SOCKET s = connect_to(port);
        if (s == INVALID_SOCKET) { srv.stop(); return 7; }
        std::string buf;
        const std::string rq1 = "GET /users/1 HTTP/1.1\r\nHost: t\r\n\r\n";
        ::send(s, rq1.data(), int(rq1.size()), 0);
        std::string r1 = read_response(s, buf);
        if (r1.find("Connection: keep-alive") == std::string::npos) {
            ::closesocket(s); srv.stop(); return 8;
        }
        const std::string rq2 = "GET /users/2 HTTP/1.1\r\nHost: t\r\n\r\n";
        ::send(s, rq2.data(), int(rq2.size()), 0);
        std::string r2 = read_response(s, buf);
        if (json::parse(body_of(r2)).find("name")->as_string() != "u2") {
            ::closesocket(s); srv.stop(); return 9;
        }
        ::closesocket(s);
    }

    // ── pipelining：一次写入两个请求，按序读两个响应 ──
    {
        SOCKET s = connect_to(port);
        if (s == INVALID_SOCKET) { srv.stop(); return 10; }
        std::string both =
            "GET /users/11 HTTP/1.1\r\nHost: t\r\n\r\n"
            "GET /users/12 HTTP/1.1\r\nHost: t\r\n\r\n";
        ::send(s, both.data(), int(both.size()), 0);
        std::string buf;
        std::string p1 = read_response(s, buf);
        std::string p2 = read_response(s, buf);
        if (json::parse(body_of(p1)).find("name")->as_string() != "u11"
            || json::parse(body_of(p2)).find("name")->as_string() != "u12") {
            ::closesocket(s); srv.stop(); return 11;
        }
        ::closesocket(s);
    }

    // ── Connection: close：响应关闭，随后的 recv 返回 0 ──
    {
        SOCKET s = connect_to(port);
        if (s == INVALID_SOCKET) { srv.stop(); return 12; }
        std::string req = "GET /users/3 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        ::send(s, req.data(), int(req.size()), 0);
        std::string buf;
        std::string r = read_response(s, buf);
        char tmp[1];
        const int n = ::recv(s, tmp, 1, 0);    // 服务端已半关闭 → 0
        if (r.find("Connection: close") == std::string::npos || n != 0) {
            ::closesocket(s); srv.stop(); return 13;
        }
        ::closesocket(s);
    }

    // ── 并发：8 线程 × 25 请求（每线程复用一条连接）──
    std::atomic<int> failures{0};
    std::vector<std::jthread> clients;
    for (int c = 0; c < 8; ++c) {
        clients.emplace_back([&, c] {
            SOCKET s = connect_to(port);
            if (s == INVALID_SOCKET) { ++failures; return; }
            std::string buf;
            for (int i = 0; i < 25; ++i) {
                const std::string id = std::to_string(c * 100 + i);
                std::string rq = "GET /users/" + id + " HTTP/1.1\r\nHost: t\r\n\r\n";
                ::send(s, rq.data(), int(rq.size()), 0);
                std::string r = read_response(s, buf);
                if (r.find("200") == std::string::npos) { ++failures; continue; }
                auto j = json::parse(body_of(r));
                if (j.find("name")->as_string() != "u" + id) ++failures;
            }
            ::closesocket(s);
        });
    }
    for (auto& th : clients) th.join();
    if (failures.load() != 0) { srv.stop(); return 14; }

    srv.stop();       // 优雅停机（排空队列 → worker join）
    std::println("WEB SERVER SMOKE: PASS");
    return 0;
}
