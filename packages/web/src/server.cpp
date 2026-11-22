// cboot.web.server 实现单元 — 后端分派 / 线程后端（accept 循环 + 连接池）
// GMF 不跨单元共享：winsock 头与守卫宏在本单元重复声明（kwik src 同款纪律）
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
#include <winsock2.h>
#include <ws2tcpip.h>

module cboot.web.server;

import std;
import cboot.web.http;
import cboot.web.routing;
import cboot.web.session;      // 传输无关会话（IOCP 后端共用）

namespace {

constexpr std::size_t max_workers = 16;
constexpr DWORD idle_timeout_ms = 15'000;              // keep-alive 空闲上限
constexpr std::size_t request_deadline_ms = 30'000;    // 单请求读入时限（slowloris）

void send_all(SOCKET s, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(s, data.data() + sent, int(data.size() - sent), 0);
        if (n <= 0) return;
        sent += std::size_t(n);
    }
}

} // namespace

int server::run() {
    // ── IOCP 后端（Windows 高并发；实现于 src/web/iocp.cpp）──
    if (io_ == server_io::iocp) {
        detail::iocp_handle h = detail::iocp_create(router_, port_, worker_count_, max_conns_,
                                 handler_timeout_ms_);
        if (!h.engine)
            throw std::runtime_error("cboot.web: iocp init failed");
        bound_port_ = h.port;
        iocp_engine_ = h.engine;      // 生命周期至析构（stop 需能触达）
        return detail::iocp_run(h, stopping_);
    }

    // ── epoll 后端（Linux；实现于 src/web/epoll.cpp，非 Linux 明确报错）──
    if (io_ == server_io::epoll) {
        detail::epoll_handle h = detail::epoll_create(router_, port_, worker_count_, max_conns_,
                                 handler_timeout_ms_);
        if (!h.engine)
            throw std::runtime_error(
                "cboot.web: epoll backend unavailable (requires Linux build)");
        bound_port_ = h.port;
        epoll_engine_ = h.engine;
        return detail::epoll_run(h, stopping_);
    }

    // ── 线程后端：阻塞 recv + 连接线程池 ──
    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET)
        throw std::runtime_error("cboot.web: socket() failed");

    BOOL reuse = TRUE;
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof reuse);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        throw std::runtime_error("cboot.web: bind() failed (port in use?)");
    if (::listen(listen_sock_, 64) != 0)
        throw std::runtime_error("cboot.web: listen() failed");

    // 实际端口（port_=0 时由系统分配）
    {
        sockaddr_in actual{};
        int len = sizeof actual;
        if (::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&actual), &len) == 0)
            bound_port_ = ntohs(actual.sin_port);
    }

    // 启动连接线程池（0 = hardware_concurrency，1..max_workers）
    std::size_t n = worker_count_;
    if (n == 0) n = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    n = std::min(n, max_workers);
    pool_.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        pool_.emplace_back([this] { worker_loop(); });

    // select 超时轮询：WinSock 跨线程 closesocket 不能可靠唤醒阻塞 accept，
    // 以 100ms 超时让 stopping_ 标志成为唯一且确定的退出通道
    while (!stopping_.load()) {
        SOCKET ls = listen_sock_.load();
        if (ls == INVALID_SOCKET) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        timeval tv{0, 100'000};    // 100ms
        int sel = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (stopping_.load()) break;
        if (sel <= 0) continue;    // 超时 / 错误 → 回到标志检查

        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        // 背压：超过连接上限 → 503 后断开（快速失败，不排入队列）
        if (max_conns_ != 0 && active_conns_.load() >= max_conns_) {
            static constexpr std::string_view busy =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 24\r\n"
                "Connection: close\r\n\r\n"
                "503 service unavailable\n";
            ::send(c, busy.data(), int(busy.size()), 0);
            ::shutdown(c, SD_SEND);
            ::closesocket(c);
            continue;
        }

        active_conns_.fetch_add(1);
        {
            std::lock_guard lk(pool_mu_);
            pool_conns_.push_back(c);
        }
        pool_cv_.notify_one();
    }

    shutdown_workers();    // 排空队列并 join（未 stop 的正常返回同样收敛）
    return 0;
}

void server::worker_loop() {
    while (true) {
        SOCKET c = INVALID_SOCKET;
        {
            std::unique_lock lk(pool_mu_);
            pool_cv_.wait(lk, [this] { return pool_stop_ || !pool_conns_.empty(); });
            if (pool_conns_.empty()) {
                if (pool_stop_) return;
                continue;
            }
            c = pool_conns_.front();
            pool_conns_.pop_front();
        }
        handle_client(c);
    }
}

void server::shutdown_workers() {
    {
        std::lock_guard lk(pool_mu_);
        pool_stop_ = true;
    }
    pool_cv_.notify_all();
    for (auto& t : pool_) {
        if (t.joinable()) t.join();    // 工作线程会排空剩余连接后退出
    }
    pool_.clear();
}

void server::handle_client(SOCKET c) {
    // 背压计数：连接从 accept 计入，处理结束（任意返回路径）递减
    struct active_guard {
        std::atomic<std::size_t>& n;
        ~active_guard() { n.fetch_sub(1); }
    } guard{active_conns_};

    // 空闲连接上限 15s（keep-alive）——防止连接永久占住 worker
    DWORD rcv_timeout = idle_timeout_ms;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&rcv_timeout), sizeof rcv_timeout);
    // 写超时（慢客户端不读 → send 失败 → 断开）
    DWORD snd_timeout = static_cast<DWORD>(send_timeout_ms_);
    ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&snd_timeout), sizeof snd_timeout);

    http_session session(router_, handler_timeout_ms_);
    std::string out;
    std::string buf(8192, '\0');
    auto request_start = std::chrono::steady_clock::now();

    while (true) {
        const int n = ::recv(c, buf.data(), int(buf.size()), 0);
        if (n <= 0) { ::closesocket(c); return; }    // 超时/对端关闭/错误

        const bool close = session.on_bytes({buf.data(), std::size_t(n)}, out,
                                            stopping_.load());
        if (!out.empty()) {
            send_all(c, out);
            out.clear();
        }
        if (close) {
            ::shutdown(c, SD_SEND);    // 半关闭：让对端读完响应
            ::closesocket(c);
            return;
        }

        // 半包超时（slowloris）：有残留字节且超过读入时限 → 400 断连
        const auto now = std::chrono::steady_clock::now();
        if (session.pending()) {
            if (now - request_start > std::chrono::milliseconds(request_deadline_ms)) {
                std::string err;
                http_append_response(err, http_make_error(400, "400 request timeout"),
                                     false);
                send_all(c, err);
                ::closesocket(c);
                return;
            }
        } else {
            request_start = now;    // 请求边界重置时限
        }
    }
}
