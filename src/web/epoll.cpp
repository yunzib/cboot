// cboot.web.server — epoll 后端（Linux；非 Linux 平台为空实现）
//
// 架构（单线程 reactor，与 IOCP 后端共享 cboot.web.session 协议层）：
//   epoll_wait → 就绪事件 → accept / read / write（非阻塞 + EAGAIN 处理）
//   - 每连接：fd + 会话 + 读缓冲 + 待写缓冲（部分写偏移）
//   - 监听 fd 与 eventfd（停机唤醒）均注册在 epoll
//   - 500ms 超时：空闲 15s / 半包 30s sweep
//
// 说明：本文件在 Windows 上编译为空实现（epoll_create 返回空句柄，
// server::run() 会明确报错"requires Linux build"）。Linux 侧需 CI 验证。
module;

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#endif

module cboot.web.server;

import std;
import cboot.web.http;
import cboot.web.routing;
import cboot.web.session;

namespace detail {

#if defined(__linux__)

namespace {

constexpr std::size_t kReadBufSize = 8192;
constexpr int kMaxEvents = 256;
constexpr auto kSweepInterval = std::chrono::milliseconds(500);
constexpr auto kIdleTimeout = std::chrono::milliseconds(15'000);
constexpr auto kRequestDeadline = std::chrono::milliseconds(30'000);

int set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct conn {
    conn(int f, const router& r, std::size_t handler_timeout_ms)
        : fd(f), session(r, handler_timeout_ms) {}
    int fd;
    http_session session;
    char rbuf[kReadBufSize];
    std::string pending;      // 待发送
    std::size_t sent = 0;     // pending 已发送偏移
    std::chrono::steady_clock::time_point write_started {};
    bool want_write = false;
    bool close_after = false;    // 响应已排入 pending，冲刷完再关
    bool closing = false;
    std::chrono::steady_clock::time_point request_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_active = request_start;
};

struct engine {
    int listen_fd = -1;
    int epfd = -1;
    int wake_fd = -1;             // eventfd：停机唤醒
    const router* routes = nullptr;
    std::atomic<bool>* stopping = nullptr;
    std::size_t max_conns = 0;      // 0 = 不限
    std::size_t handler_timeout_ms = 0;
    std::size_t send_timeout_ms = 30'000;
    std::unordered_map<int, std::unique_ptr<conn>> conns;   // key = fd
};

void close_conn(engine& e, conn& c) {
    if (c.closing) return;
    c.closing = true;
    ::epoll_ctl(e.epfd, EPOLL_CTL_DEL, c.fd, nullptr);
    ::close(c.fd);
    c.fd = -1;
}

void want_write(engine& e, conn& c, bool on) {
    if (c.want_write == on || c.fd < 0) return;
    c.want_write = on;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0u);
    ev.data.fd = c.fd;
    ::epoll_ctl(e.epfd, EPOLL_CTL_MOD, c.fd, &ev);
}

/** @brief 尽力刷新 pending；返回 false = 连接错误 */
bool flush(engine& e, conn& c) {
    while (c.sent < c.pending.size()) {
        const ssize_t n = ::send(c.fd, c.pending.data() + c.sent,
                                 c.pending.size() - c.sent, MSG_NOSIGNAL);
        if (n > 0) { c.sent += std::size_t(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            want_write(e, c, true);
            return true;
        }
        return false;
    }
    c.pending.clear();
    c.sent = 0;
    want_write(e, c, false);
    return true;
}

void on_readable(engine& e, conn& c) {
    while (true) {
        const ssize_t n = ::recv(c.fd, c.rbuf, sizeof c.rbuf, 0);
        if (n > 0) {
            c.last_active = std::chrono::steady_clock::now();
            const bool close = c.session.on_bytes(
                {c.rbuf, std::size_t(n)}, c.pending, e.stopping->load());
            if (!flush(e, c)) { close_conn(e, c); return; }
            if (close) {
                c.close_after = true;                 // 冲刷完 pending 再关
                if (c.pending.empty()) close_conn(e, c);
                return;
            }
            continue;
        }
        if (n == 0) { close_conn(e, c); return; }        // 对端关闭
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_conn(e, c);
        return;
    }
}

void on_writable(engine& e, conn& c) {
    if (!flush(e, c)) { close_conn(e, c); return; }
    if (c.close_after && c.pending.empty()) close_conn(e, c);
}

void on_accept(engine& e) {
    while (true) {
        const int fd = ::accept4(e.listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        // 背压：连接数超限 → 503 后断开
        if (e.max_conns != 0 && e.conns.size() >= e.max_conns) {
            static constexpr std::string_view busy =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 24\r\n"
                "Connection: close\r\n\r\n"
                "503 service unavailable\n";
            ::send(fd, busy.data(), busy.size(), MSG_NOSIGNAL);
            ::close(fd);
            continue;
        }
        auto c = std::make_unique<conn>(fd, *e.routes, e.handler_timeout_ms);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        if (::epoll_ctl(e.epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            ::close(fd);
            continue;
        }
        e.conns.emplace(fd, std::move(c));
    }
}

void sweep(engine& e) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = e.conns.begin(); it != e.conns.end();) {
        conn& c = *it->second;
        if (c.closing) { it = e.conns.erase(it); continue; }
        if (now - c.last_active > kIdleTimeout
            || (c.session.pending() && now - c.request_start > kRequestDeadline)
            || (c.want_write && now - c.write_started
                    > std::chrono::milliseconds(e.send_timeout_ms))) {
            close_conn(e, c);
            it = e.conns.erase(it);
            continue;
        }
        if (!c.session.pending()) c.request_start = now;
        ++it;
    }
}

int create_listen(unsigned short port, unsigned short& bound) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) != 0) { ::close(fd); return -1; }
    sockaddr_in actual{};
    socklen_t len = sizeof actual;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0)
        bound = ntohs(actual.sin_port);
    return fd;
}

} // namespace

epoll_handle epoll_create(router& r, unsigned short port, std::size_t workers,
                          std::size_t max_conns, std::size_t handler_timeout_ms) {
    (void)workers;    // 单线程 reactor（多 reactor 需 SO_REUSEPORT 分片，后续项）
    auto e = std::make_unique<engine>();
    e->routes = &r;
    e->max_conns = max_conns;
    e->handler_timeout_ms = handler_timeout_ms;

    unsigned short bound = 0;
    e->listen_fd = create_listen(port, bound);
    if (e->listen_fd < 0) return {};

    e->epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (e->epfd < 0) { ::close(e->listen_fd); return {}; }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = e->listen_fd;
    if (::epoll_ctl(e->epfd, EPOLL_CTL_ADD, e->listen_fd, &ev) != 0) {
        ::close(e->epfd);
        ::close(e->listen_fd);
        return {};
    }

    e->wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->wake_fd >= 0) {
        epoll_event wev{};
        wev.events = EPOLLIN;
        wev.data.fd = e->wake_fd;
        ::epoll_ctl(e->epfd, EPOLL_CTL_ADD, e->wake_fd, &wev);
    }

    return {e.release(), bound};
}

int epoll_run(epoll_handle h, std::atomic<bool>& stopping) {
    engine& e = *static_cast<engine*>(h.engine);
    e.stopping = &stopping;
    auto last_sweep = std::chrono::steady_clock::now();
    epoll_event events[kMaxEvents];

    while (!stopping.load()) {
        const int n = ::epoll_wait(e.epfd, events, kMaxEvents, 200);
        if (stopping.load()) break;
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t evs = events[i].events;
            if (fd == e.listen_fd) { on_accept(e); continue; }
            if (fd == e.wake_fd) {
                std::uint64_t v = 0;
                (void)::read(e.wake_fd, &v, sizeof v);
                continue;
            }
            auto it = e.conns.find(fd);
            if (it == e.conns.end()) continue;
            conn& c = *it->second;
            if (evs & (EPOLLERR | EPOLLHUP)) { close_conn(e, c); continue; }
            if (evs & EPOLLOUT) on_writable(e, c);
            if (!c.closing && (evs & (EPOLLIN | EPOLLRDHUP))) on_readable(e, c);
            if (c.closing) e.conns.erase(it);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_sweep >= kSweepInterval) {
            sweep(e);
            last_sweep = now;
        }
    }

    for (auto& [fd, c] : e.conns) {
        if (fd >= 0) ::close(fd);
        c->fd = -1;
    }
    e.conns.clear();
    if (e.wake_fd >= 0) ::close(e.wake_fd);
    if (e.listen_fd >= 0) ::close(e.listen_fd);
    if (e.epfd >= 0) ::close(e.epfd);
    return 0;
}

void epoll_stop(void* p) {
    engine& e = *static_cast<engine*>(p);
    if (e.wake_fd >= 0) {
        std::uint64_t one = 1;
        (void)::write(e.wake_fd, &one, sizeof one);
    }
    if (e.listen_fd >= 0) {
        ::epoll_ctl(e.epfd, EPOLL_CTL_DEL, e.listen_fd, nullptr);
        ::close(e.listen_fd);
        e.listen_fd = -1;
    }
}

void epoll_destroy(void* p) {
    engine& e = *static_cast<engine*>(p);
    delete &e;
}

#else  // 非 Linux：空实现（server::run() 明确报错）

epoll_handle epoll_create(router&, unsigned short, std::size_t, std::size_t,
                              std::size_t) { return {}; }
int epoll_run(epoll_handle, std::atomic<bool>&) { return 1; }
void epoll_stop(void*) {}
void epoll_destroy(void*) {}

#endif

} // namespace detail
