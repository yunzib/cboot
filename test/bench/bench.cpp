// cboot 本地基准：keep-alive vs 每请求建连（loopback）
// 用法：cboot_bench [threads] [requests_per_thread]
// 服务端 8 工作线程；客户端每线程一条连接顺序发请求（keep-alive 模式）
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

#include <cstdio>
import cboot.json;
import cboot.web.http;
import cboot.web.routing;
import cboot.web.session;
import cboot.web.server;
import std;

struct BenchJson {
    std::string message;
    int id{};
};

struct BenchApi {
    [[=Get{"/ping"}]]                      // plaintext：4 字节响应
    Response ping() { return Response::text("pong"); }

    [[=Get{"/json"}]]                      // JSON 序列化：反射绑定往返
    Response json() { return json_response(BenchJson{"hello, world!", 42}); }
};

SOCKET connect_to(unsigned short port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD tmo = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tmo), sizeof tmo);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::fprintf(stderr, "[bench] connect %u failed: %d\n", unsigned(port),
                     int(::WSAGetLastError()));
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// 读一个完整响应（Content-Length 定界），返回 false = 连接异常
// 响应头 content-length 解析（大小写不敏感、零分配）
std::size_t content_length_from(std::string_view buf, std::size_t from, std::size_t he) {
    constexpr std::string_view name = "content-length:";
    for (std::size_t i = from; i + name.size() <= he; ++i) {
        std::size_t k = 0;
        while (k < name.size()) {
            char c = buf[i + k];
            if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
            if (c != name[k]) break;
            ++k;
        }
        if (k == name.size()) {
            std::size_t v = i + name.size();
            while (v < he && (buf[v] == ' ' || buf[v] == '\t')) ++v;
            std::size_t out = 0;
            std::from_chars(buf.data() + v, buf.data() + he, out);
            return out;
        }
    }
    return 0;
}

// 读一个完整响应；off 为消费偏移（pipelining 下避免每响应 erase 搬移）
bool read_response(SOCKET s, std::string& buf, std::size_t& off, std::string& out) {
    for (;;) {
        const std::size_t he = buf.find("\r\n\r\n", off);
        if (he != std::string::npos) {
            const std::size_t cl = content_length_from(buf, off, he);
            if (buf.size() >= he + 4 + cl) {
                out.assign(buf, off, he + 4 + cl - off);
                off = he + 4 + cl;
                if (off == buf.size()) {
                    buf.clear();
                    off = 0;
                } else if (off > 8192 && off > buf.size() / 2) {
                    buf.erase(0, off);
                    off = 0;
                }
                return true;
            }
        }
        char tmp[8192];
        const int n = ::recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, std::size_t(n));
    }
}

struct Result {
    double rps = 0;
    std::size_t requests = 0;
    std::size_t failures = 0;
};

// 基准专用关闭：SO_LINGER=0 → RST，跳过 TIME_WAIT（避免耗尽临时端口）
void bench_close(SOCKET s);

// 单连接一次请求-响应往返（keep-alive）
bool exchange(SOCKET s, const std::string& req, std::string& buf, std::size_t& off,
              std::string& resp) {
    if (::send(s, req.data(), int(req.size()), 0) <= 0) return false;
    return read_response(s, buf, off, resp);
}

Result run_keepalive(unsigned short port, int threads, int per_thread,
                     std::string_view path = "/ping") {
    Result r;
    std::atomic<std::size_t> done{0}, fails{0}, reconnects{0};
    const std::string req = "GET " + std::string(path) + " HTTP/1.1\r\nHost: b\r\n\r\n";
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::jthread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&] {
            SOCKET s = INVALID_SOCKET;
            std::string buf, resp;
            std::size_t off = 0;
            int on_conn = 0;
            for (int k = 0; k < per_thread; ++k) {
                // 主动重连：低于服务端每连接上限（1024）
                if (s == INVALID_SOCKET || on_conn >= 1000) {
                    if (s != INVALID_SOCKET) bench_close(s);
                    s = connect_to(port);
                    buf.clear();
                    off = 0;
                    on_conn = 0;
                    if (s == INVALID_SOCKET) { ++fails; break; }
                    ++reconnects;
                }
                if (!exchange(s, req, buf, off, resp)) {
                    bench_close(s);
                    s = INVALID_SOCKET;
                    ++fails;
                    continue;    // 重连后重试本请求
                }
                ++done;
                ++on_conn;
            }
            if (s != INVALID_SOCKET) bench_close(s);
        });
    }
    for (auto& t : pool) t.join();
    const auto t1 = std::chrono::steady_clock::now();
    r.requests = done.load();
    r.failures = fails.load();
    r.rps = double(r.requests) / std::chrono::duration<double>(t1 - t0).count();
    std::println("  (reconnects={} failures={})", reconnects.load(), fails.load());
    return r;
}

Result run_close(unsigned short port, int threads, int per_thread) {
    Result r;
    std::atomic<std::size_t> done{0}, fails{0};
    const std::string req = "GET /ping HTTP/1.1\r\nHost: b\r\nConnection: close\r\n\r\n";
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::jthread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&] {
            for (int k = 0; k < per_thread; ++k) {
                SOCKET s = connect_to(port);
                if (s == INVALID_SOCKET) { ++fails; break; }
                std::string buf, resp;
                std::size_t off = 0;
                if (exchange(s, req, buf, off, resp)) ++done; else ++fails;
                bench_close(s);       // RST 关闭：避免 TIME_WAIT 耗尽临时端口
            }
        });
    }
    for (auto& t : pool) t.join();
    const auto t1 = std::chrono::steady_clock::now();
    r.requests = done.load();          // 只统计成功
    r.failures = fails.load();
    r.rps = double(r.requests) / std::chrono::duration<double>(t1 - t0).count();
    return r;
}

// 基准专用关闭：SO_LINGER=0 → RST，跳过 TIME_WAIT（避免耗尽临时端口）
void bench_close(SOCKET s) {
    linger lg{1, 0};
    ::setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof lg);
    ::closesocket(s);
}

// ── 流水线（pipelining）批量：每次写入 depth 个请求，再读 depth 个响应 ──
// 目的：让一次读完成携带多个完整请求 → 服务端 on_bytes 循环一次处理多个，
// 响应合并成一次写。depth=1 即顺序模式。
Result run_pipeline(unsigned short port, int threads, int per_thread,
                    std::string_view path, int depth) {
    Result r;
    std::atomic<std::size_t> done{0}, fails{0}, reconnects{0};
    const std::string one = "GET " + std::string(path) + " HTTP/1.1\r\nHost: b\r\n\r\n";
    std::string batch;
    batch.reserve(one.size() * std::size_t(depth));
    for (int i = 0; i < depth; ++i) batch += one;

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::jthread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&] {
            SOCKET s = INVALID_SOCKET;
            std::string buf, resp;
            std::size_t off = 0;
            int on_conn = 0;
            int remaining = per_thread;
            while (remaining >= depth) {
                // 主动重连（低于服务端每连接上限 1024）
                if (s == INVALID_SOCKET || on_conn + depth > 1000) {
                    if (s != INVALID_SOCKET) bench_close(s);
                    s = connect_to(port);
                    buf.clear();
                    off = 0;
                    on_conn = 0;
                    if (s == INVALID_SOCKET) { ++fails; return; }
                    ++reconnects;
                }
                if (::send(s, batch.data(), int(batch.size()), 0) <= 0) {
                    bench_close(s);
                    s = INVALID_SOCKET;
                    ++fails;
                    continue;
                }
                int ok = 0;
                for (int k = 0; k < depth; ++k) {
                    if (!read_response(s, buf, off, resp)) break;
                    ++ok;
                }
                if (ok != depth) {
                    bench_close(s);
                    s = INVALID_SOCKET;
                    ++fails;
                    continue;
                }
                on_conn += depth;
                done += std::size_t(depth);
                remaining -= depth;
            }
            if (s != INVALID_SOCKET) bench_close(s);
        });
    }
    for (auto& t : pool) t.join();
    const auto t1 = std::chrono::steady_clock::now();
    r.requests = done.load();
    r.failures = fails.load();
    r.rps = double(r.requests) / std::chrono::duration<double>(t1 - t0).count();
    std::println("  (reconnects={} failures={})", reconnects.load(), fails.load());
    return r;
}

// 连接规模验证：建立 N 条 keep-alive 连接 → 各发一个请求 → 各读一个响应
int run_conn_scale(unsigned short port, int count) {
    std::vector<SOCKET> socks;
    socks.reserve(count);
    const std::string req = "GET /ping HTTP/1.1\r\nHost: b\r\n\r\n";
    for (int i = 0; i < count; ++i) {
        SOCKET s = connect_to(port);
        if (s == INVALID_SOCKET) break;
        if (::send(s, req.data(), int(req.size()), 0) <= 0) { bench_close(s); break; }
        socks.push_back(s);
    }
    int served = 0;
    for (SOCKET s : socks) {
        std::string buf, resp;
        std::size_t off = 0;
        if (read_response(s, buf, off, resp)) ++served;
        bench_close(s);
    }
    return served;
}

int main(int argc, char** argv) {
    // 用法：cboot_bench <threads> <per_thread> [iocp] [conn_scale]
    //        [--workers N] [--client PORT]     ← --client：只做客户端，压外部服务
    const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    const int per_thread = argc > 2 ? std::atoi(argv[2]) : 20000;
    const bool use_iocp = argc > 3 && std::string_view(argv[3]) == "iocp";
    const int conn_scale = argc > 4 ? std::atoi(argv[4]) : 0;
    std::size_t workers = 8;
    int pipeline = 1;
    bool dispatch_bench = false;
    unsigned short client_port = 0;
    unsigned short serve_port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        const bool has_val = i + 1 < argc;
        if (a == "--workers" && has_val)
            workers = static_cast<std::size_t>(std::atoi(argv[i + 1]));
        if (a == "--client" && has_val)
            client_port = static_cast<unsigned short>(std::atoi(argv[i + 1]));
        if (a == "--serve" && has_val)
            serve_port = static_cast<unsigned short>(std::atoi(argv[i + 1]));
        if (a == "--pipeline" && has_val)
            pipeline = std::max(1, std::atoi(argv[i + 1]));
        if (a == "--dispatch")
            dispatch_bench = true;
    }

    // ── 纯派发微基准：无 socket，测协议层 ns/请求 ──
    if (dispatch_bench) {
        router r;
        BenchApi api;
        r.mount(api);
        constexpr int kN = 2'000'000;
        for (std::string_view path : {"/ping", "/json"}) {
            // 预热
            for (int i = 0; i < 100000; ++i) (void)r.dispatch("GET", path, "");
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kN; ++i) (void)r.dispatch("GET", path, "");
            const auto t1 = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
            std::println("dispatch {}: {:.0f} ns/req", path, ns);
        }
        // 细分：Response::text 构造 / 序列化
        {
            constexpr int kN = 2'000'000;
            std::size_t acc = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kN; ++i) { Response r = Response::text("pong"); acc += r.body.size(); }
            const auto t1 = std::chrono::steady_clock::now();
            std::println("Response::text   : {:.0f} ns  (acc={})",
                         std::chrono::duration<double, std::nano>(t1 - t0).count() / kN, acc);

            Response sample = Response::text("pong");
            std::string out;
            out.reserve(4096);
            const auto t2 = std::chrono::steady_clock::now();
            for (int i = 0; i < kN; ++i) { out.clear(); http_append_response(out, sample, true); acc += out.size(); }
            const auto t3 = std::chrono::steady_clock::now();
            std::println("append_response  : {:.0f} ns  (acc={})",
                         std::chrono::duration<double, std::nano>(t3 - t2).count() / kN, acc);
        }
        // 服务端协议栈：64 请求/批 + 会话（解析 + 派发 + 序列化 + keep-alive 决策）
        {
            router r2;
            BenchApi api2;
            r2.mount(api2);
            std::string batch;
            for (int i = 0; i < 64; ++i)
                batch += "GET /ping HTTP/1.1\r\nHost: b\r\n\r\n";
            std::string out;
            out.reserve(16384);
            constexpr int kRounds = 2000;       // 2000 × 16 批 × 64 = 204.8 万请求
            std::size_t total = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < kRounds; ++r) {
                http_session sess(r2);
                for (int k = 0; k < 16; ++k) {
                    out.clear();
                    sess.on_bytes(batch, out, false);
                    total += 64;
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            std::println("session(64/batch): {:.0f} ns/req",
                         std::chrono::duration<double, std::nano>(t1 - t0).count() / total);
        }
        // JSON 响应构造成本（流式序列化 + body 字符串）
        {
            BenchJson dto{"hello, world!", 42};
            constexpr int kN2 = 2'000'000;
            std::size_t acc = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kN2; ++i) {
                Response r = json_response(dto);
                acc += r.body.size();
            }
            const auto t1 = std::chrono::steady_clock::now();
            std::println("json_response    : {:.0f} ns  (acc={})",
                         std::chrono::duration<double, std::nano>(t1 - t0).count() / kN2,
                         acc);
        }
        return 0;
    }

    // ── 纯服务模式：独立进程起服务（与 Java 对等：客户端在另一进程）──
    if (serve_port != 0) {
        server srv(serve_port, workers, use_iocp ? server_io::iocp : server_io::threads);
        BenchApi api;
        srv.mount(api);
        std::println("serve mode: port={} backend={} workers={}", serve_port,
                     use_iocp ? "iocp" : "threads", workers);
        std::fflush(nullptr);
        return srv.run();
    }

    // ── 客户端模式：对已存在的服务（Java/其他）跑同一负载 ──
    if (client_port != 0) {
        // 客户端模式不构造 server → 需自行 WSAStartup（否则 WSAENOTINITIALISED）
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 101;
        std::println("client mode: port={} connections={} per_conn={} pipeline={}",
                     client_port, threads, per_thread, pipeline);

        if (pipeline > 1) {
            // 流水线批量：一次写入 depth 个请求，读 depth 个响应
            for (int round = 1; round <= 3; ++round) {
                const Result p = run_pipeline(client_port, threads, per_thread, "/ping", pipeline);
                std::println("pipe-plain #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                             round, p.rps, p.requests, p.failures);
            }
            for (int round = 1; round <= 3; ++round) {
                const Result j = run_pipeline(client_port, threads, per_thread, "/json", pipeline);
                std::println("pipe-json  #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                             round, j.rps, j.requests, j.failures);
            }
            return 0;
        }

        for (int round = 1; round <= 3; ++round) {
            const Result p = run_keepalive(client_port, threads, per_thread, "/ping");
            std::println("plaintext #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                         round, p.rps, p.requests, p.failures);
        }
        for (int round = 1; round <= 3; ++round) {
            const Result j = run_keepalive(client_port, threads, per_thread, "/json");
            std::println("json      #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                         round, j.rps, j.requests, j.failures);
        }
        return 0;
    }

    server srv(0, workers, use_iocp ? server_io::iocp : server_io::threads);
    BenchApi api;
    srv.mount(api);
    std::jthread t([&] { (void)srv.run(); });
    for (int i = 0; i < 200 && srv.port() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const unsigned short port = srv.port();
    if (port == 0) { srv.stop(); return 100; }

    std::println("cboot bench: port={} backend={} threads={} per_thread={}", port,
                 use_iocp ? "iocp" : "threads", threads, per_thread);

    // 连接规模先跑（fresh 状态；close 模式会耗尽客户端临时端口产生 TIME_WAIT）
    if (conn_scale > 0) {
        const int served = run_conn_scale(port, conn_scale);
        std::println("connections : {}/{} served ({} 条并发 keep-alive)",
                     served, conn_scale, conn_scale);
    }

    // 预热（2000 请求，不计入）
    (void)run_keepalive(port, threads, 2000 / threads + 1);

    for (int round = 1; round <= 3; ++round) {
        const Result ka = run_keepalive(port, threads, per_thread, "/ping");
        std::println("plaintext #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                     round, ka.rps, ka.requests, ka.failures);
    }
    for (int round = 1; round <= 3; ++round) {
        const Result kj = run_keepalive(port, threads, per_thread, "/json");
        std::println("json      #{}: {:>9.0f} req/s  ({} reqs, {} failures)",
                     round, kj.rps, kj.requests, kj.failures);
    }

    const Result cl = run_close(port, threads, std::max(50, per_thread / 20));
    std::println("close       : {:>9.0f} req/s  ({} reqs, {} failures)",
                 cl.rps, cl.requests, cl.failures);

    srv.stop();
    return 0;
}
