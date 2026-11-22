// cboot.web.server — IOCP 后端实现（Windows 完成端口，多 worker）
//
// 架构：
//   - N 个 worker 线程共取完成端口（GetQueuedCompletionStatus）
//   - **每连接独立互斥锁**：同一连接的读/写完成在同一时刻只被一个 worker
//     处理（IOCP 不保证同 socket 完成包串行，故状态需连接级串行化）
//   - 连接注册表由 engine 互斥锁保护（accept 插入 / 关闭清理）
//   - 独立 sweeper 线程：空闲 15s / 半包 30s 超时
//   - 生命周期：每 op 持 inflight 计数；关闭后 inflight==0 才释放
//
// 协议层复用 cboot.web.session（与线程后端共享）。
// GMF：winsock2/mswsock（AcceptEx）与守卫宏本单元自带
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
#include <cstdio>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

module cboot.web.server;

import std;
import cboot.async;            // task / async_event（连接级协程）
import cboot.web.http;
import cboot.web.routing;
import cboot.web.session;

namespace detail {
namespace {

constexpr std::size_t kReadBufSize = 8192;
constexpr int kAddrLen = sizeof(sockaddr_in) + 16;
constexpr std::size_t kMaxWorkers = 32;
constexpr auto kSweepInterval = std::chrono::milliseconds(250);
constexpr auto kIdleTimeout = std::chrono::milliseconds(15'000);
constexpr auto kRequestDeadline = std::chrono::milliseconds(30'000);

struct conn {
    conn(SOCKET s, const router& r, std::size_t handler_timeout_ms)
        : sock(s), session(r, handler_timeout_ms) {
        pending.reserve(512);        // 热路径零分配起步
        wbuf.reserve(512);
    }
    SOCKET sock;
    http_session session;

    char rbuf[kReadBufSize];
    std::string pending;      // 累积待发送
    std::string wbuf;         // 正在发送（WSASend 期间不可变）
    OVERLAPPED rov{};
    OVERLAPPED wov{};

    std::mutex mu;            // 连接级串行化（同一时刻只一个 worker 处理）
    int inflight = 0;         // 未完成 op 数（关闭后到 0 才释放）
    bool reading = false;
    bool writing = false;
    bool closing = false;
    bool close_after_write = false;
    DWORD read_bytes = 0;     // 读完成结果（供协程消费）
    bool read_ok = true;
    bool write_ok = true;
    std::chrono::steady_clock::time_point write_started {};
    async_event read_ev;      // I/O 完成 → 恢复连接协程
    async_event write_ev;
    std::chrono::steady_clock::time_point request_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_active = request_start;

    // 连接协程：声明在最后 → 析构最先（协程帧先于连接状态释放）
    task<void> serve;
};

struct engine {
    SOCKET listen_sock = INVALID_SOCKET;
    HANDLE iocp = nullptr;
    LPFN_ACCEPTEX accept_ex = nullptr;
    const router* routes = nullptr;
    std::atomic<bool>* stopping = nullptr;

    std::mutex mu;                                  // 保护 conns
    std::vector<std::unique_ptr<conn>> conns;

    // 单次只挂一个 AcceptEx；accept_mu 串行化 worker 与 sweeper 的重挂
    std::mutex accept_mu;
    std::atomic<bool> accept_outstanding{false};
    OVERLAPPED aov{};
    char abuf[2 * kAddrLen];
    SOCKET asock = INVALID_SOCKET;

    std::size_t worker_count = 1;
    std::size_t max_conns = 0;      // 0 = 不限
    std::size_t handler_timeout_ms = 0;
    std::size_t send_timeout_ms = 30'000;
};

/** @brief 关闭连接（调用方须持有 conn.mu） */
void close_conn(conn& c) {
    if (c.closing) return;
    c.closing = true;
    ::CancelIoEx(reinterpret_cast<HANDLE>(c.sock), &c.rov);
    ::CancelIoEx(reinterpret_cast<HANDLE>(c.sock), &c.wov);
    ::closesocket(c.sock);
    c.sock = INVALID_SOCKET;
}

void start_write(conn& c) {
    if (c.writing || c.pending.empty() || c.sock == INVALID_SOCKET) return;
    c.wbuf.swap(c.pending);
    c.writing = true;
    c.write_started = std::chrono::steady_clock::now();
    ++c.inflight;
    WSABUF b{static_cast<ULONG>(c.wbuf.size()), c.wbuf.data()};
    DWORD sent = 0;
    const int rc = ::WSASend(c.sock, &b, 1, &sent, 0, &c.wov, nullptr);
    if (rc == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING) {
        --c.inflight;
        c.writing = false;
        close_conn(c);
    }
}

void post_read(conn& c) {
    if (c.closing || c.reading || c.sock == INVALID_SOCKET) return;
    c.reading = true;
    ++c.inflight;
    WSABUF b{static_cast<ULONG>(sizeof(c.rbuf)), c.rbuf};
    DWORD flags = 0;
    const int rc = ::WSARecv(c.sock, &b, 1, nullptr, &flags, &c.rov, nullptr);
    if (rc == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING) {
        --c.inflight;
        c.reading = false;
        close_conn(c);
    }
}

task<void> serve_conn(engine& e, conn& c);

/** @brief 重挂 AcceptEx（调用方须持 accept_mu；同一时刻只有一个挂起） */
void post_accept_locked(engine& e) {
    if (e.accept_outstanding) return;
    if (e.listen_sock == INVALID_SOCKET) return;
    if (e.stopping && e.stopping->load()) return;
    // 关键：完成端口要求 overlapped 套接字（::socket() 默认不带该标志）
    e.asock = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                          WSA_FLAG_OVERLAPPED);
    if (e.asock == INVALID_SOCKET) {
        std::println(stderr, "[iocp] WSASocket failed {}", ::WSAGetLastError());
        return;                    // sweeper 下一轮重试
    }
    DWORD bytes = 0;
    const BOOL ok = e.accept_ex(e.listen_sock, e.asock, e.abuf, 0,
                                kAddrLen, kAddrLen, &bytes, &e.aov);
    if (ok || ::WSAGetLastError() == ERROR_IO_PENDING) {
        e.accept_outstanding = true;
        return;
    }
    std::println(stderr, "[iocp] AcceptEx failed {}", ::WSAGetLastError());
    ::closesocket(e.asock);
    e.asock = INVALID_SOCKET;      // sweeper 下一轮重试
}

void post_accept(engine& e) {
    std::lock_guard al(e.accept_mu);
    post_accept_locked(e);
}

void on_accept(engine& e, BOOL ok) {
    std::lock_guard al(e.accept_mu);
    e.accept_outstanding = false;  // 本次 accept 已完成

    if (!ok) {
        if (e.asock != INVALID_SOCKET) ::closesocket(e.asock);
        e.asock = INVALID_SOCKET;
        post_accept_locked(e);
        return;
    }
    ::setsockopt(e.asock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                 reinterpret_cast<const char*>(&e.listen_sock), sizeof(e.listen_sock));

    // 背压：连接数超限 → 503 后断开
    if (e.max_conns != 0) {
        bool full = false;
        {
            std::lock_guard el(e.mu);
            full = e.conns.size() >= e.max_conns;
        }
        if (full) {
            static constexpr std::string_view busy =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 24\r\n"
                "Connection: close\r\n\r\n"
                "503 service unavailable\n";
            ::send(e.asock, busy.data(), int(busy.size()), 0);
            ::shutdown(e.asock, SD_SEND);
            ::closesocket(e.asock);
            e.asock = INVALID_SOCKET;
            post_accept_locked(e);
            return;
        }
    }

    auto c = std::make_unique<conn>(e.asock, *e.routes, e.handler_timeout_ms);
    e.asock = INVALID_SOCKET;
    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(c->sock), e.iocp,
                                 reinterpret_cast<ULONG_PTR>(c.get()), 0) == nullptr) {
        ::closesocket(c->sock);
        post_accept_locked(e);
        return;
    }
    conn* p = c.get();
    {
        std::lock_guard el(e.mu);
        e.conns.push_back(std::move(c));
    }
    {
        std::lock_guard cl(p->mu);
        post_read(*p);                       // 先挂读（完成经事件唤醒协程）
        p->serve = serve_conn(e, *p);        // 每连接一个协程
        p->serve.start();                    // 立即挂起在 read_ev 上
    }
    post_accept_locked(e);
}

/** @brief 读完成 → 置结果并恢复连接协程（调用方持有 conn.mu） */
void on_read_complete(conn& c, DWORD bytes, BOOL ok) {
    c.reading = false;
    --c.inflight;
    c.read_bytes = bytes;
    c.read_ok = ok != FALSE;
    c.read_ev.set();          // 同步恢复协程（同线程、锁内；协程不再加锁）
}

/** @brief 写完成 → 异步续写 / 收尾（调用方持有 conn.mu；不唤醒协程） */
void on_write_complete(conn& c, DWORD bytes, BOOL ok) {
    (void)bytes;
    c.writing = false;
    --c.inflight;
    c.wbuf.clear();               // 上一轮数据已发完（容量保留，供下次 swap 复用）
    if (!ok) { close_conn(c); return; }
    if (!c.pending.empty()) { start_write(c); return; }   // 异步续写
    if (c.close_after_write) { close_conn(c); return; }
    // 空闲：读侧已单独挂起（全双工），无需动作
}

/**
 * @brief 连接协程：读侧驱动（全双工）
 *
 * 读完成 → 协议处理 → **立即投递下一次读**；写由完成回调异步续写
 * （on_write_complete），不再 await 写完成——这是与 Java reactor 对齐的
 * 关键：每请求只占一次读完成往返，写与下一次读重叠。
 *
 * 与完成端口协作：worker 在锁内置结果 + set() 同步恢复本协程；
 * 协程内联运行时不重复加 conn 锁（调用方已持有）。
 */
task<void> serve_conn(engine& e, conn& c) {
    for (;;) {
        co_await c.read_ev;
        if (c.closing) co_return;
        if (!c.read_ok || c.read_bytes == 0) { close_conn(c); co_return; }

        c.last_active = std::chrono::steady_clock::now();
        const bool close = c.session.on_bytes({c.rbuf, c.read_bytes}, c.pending,
                                              e.stopping->load());

        // 异步写：未在写则立即启动；正在写则 pending 累积，由写完成续写
        if (!c.pending.empty() && !c.writing) start_write(c);
        if (c.closing) co_return;

        if (close) {
            // 冲刷完 pending 后由 on_write_complete 关闭
            c.close_after_write = true;
            if (!c.writing && c.pending.empty()) { close_conn(c); co_return; }
            co_return;                        // 读侧完成；写侧负责收尾
        }
        post_read(c);                         // 全双工：立即挂下一次读
    }
}

/** @brief 清理已关闭且无在途 op、协程已结束的连接（engine.mu 保护） */
void reap(engine& e) {
    std::erase_if(e.conns, [](const std::unique_ptr<conn>& p) {
        std::lock_guard cl(p->mu);
        return p->closing && p->inflight == 0 && p->serve.done();
    });
}

void sweep(engine& e) {
    // accept 链自愈：重挂失败或完成丢失时，下一轮补上
    {
        std::lock_guard al(e.accept_mu);
        post_accept_locked(e);
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard el(e.mu);
    for (auto& p : e.conns) {
        conn& c = *p;
        std::lock_guard cl(c.mu);
        if (c.closing) continue;
        if (now - c.last_active > kIdleTimeout) { close_conn(c); continue; }
        // 写超时：慢客户端不读 → 放弃连接
        if (c.writing && now - c.write_started > std::chrono::milliseconds(e.send_timeout_ms)) {
            close_conn(c);
            continue;
        }
        if (c.session.pending() && now - c.request_start > kRequestDeadline) {
            close_conn(c);
            continue;
        }
        if (!c.session.pending()) c.request_start = now;
    }
    reap(e);
}

void worker_loop(engine& e) {
    OVERLAPPED_ENTRY entries[32];
    for (;;) {
        ULONG count = 0;
        const BOOL ok = ::GetQueuedCompletionStatusEx(e.iocp, entries, 32, &count,
                                                      500, FALSE);
        if (e.stopping->load()) return;
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != WAIT_TIMEOUT)
                std::println(stderr, "[iocp] GQCSEx failed {}", err);
            continue;
        }

        // 批量出队：一次 syscall 处理最多 32 个完成包（摊薄内核开销）
        for (ULONG i = 0; i < count; ++i) {
            const DWORD bytes = entries[i].dwNumberOfBytesTransferred;
            const ULONG_PTR key = entries[i].lpCompletionKey;
            LPOVERLAPPED ov = entries[i].lpOverlapped;
            if (ov == nullptr) continue;          // 停机唤醒包
            if (key == 0) {                       // AcceptEx 完成
                DWORD got = 0;
                const BOOL aok = ::GetOverlappedResult(
                    reinterpret_cast<HANDLE>(e.listen_sock), &e.aov, &got, FALSE);
                on_accept(e, aok);
                continue;
            }
            conn* c = reinterpret_cast<conn*>(key);
            // 锁顺序纪律：热路径只持连接锁；回收交给 sweeper（见上）
            std::lock_guard cl(c->mu);
            // 完成包本身不带状态：读/写失败（RST、取消）必须显式查询，
            // 否则失败会被当成成功（写失败丢失响应、accept 建出无效连接）
            DWORD got = bytes;
            const BOOL iok = ::GetOverlappedResult(
                reinterpret_cast<HANDLE>(c->sock), ov, &got, FALSE);
            if (ov == &c->rov) on_read_complete(*c, got, iok);
            else on_write_complete(*c, got, iok);
        }
    }
}

/** @brief 停机收尾（worker 全部退出后调用，无并发） */
void close_all(engine& e) {
    std::lock_guard el(e.mu);
    for (auto& p : e.conns) {
        if (p->sock != INVALID_SOCKET) ::closesocket(p->sock);
        p->sock = INVALID_SOCKET;
    }
    e.conns.clear();
    if (e.asock != INVALID_SOCKET) ::closesocket(e.asock);
    e.asock = INVALID_SOCKET;
    SOCKET ls = e.listen_sock;
    e.listen_sock = INVALID_SOCKET;
    if (ls != INVALID_SOCKET) ::closesocket(ls);
}

engine* as_engine(void* p) { return static_cast<engine*>(p); }

} // namespace

iocp_handle iocp_create(router& r, unsigned short port, std::size_t workers,
                        std::size_t max_conns, std::size_t handler_timeout_ms) {
    auto e = std::make_unique<engine>();
    e->routes = &r;
    e->max_conns = max_conns;
    e->handler_timeout_ms = handler_timeout_ms;
    std::size_t n = workers;
    if (n == 0) n = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    e->worker_count = std::min(n, kMaxWorkers);

    e->listen_sock = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                 WSA_FLAG_OVERLAPPED);
    if (e->listen_sock == INVALID_SOCKET) return {};

    BOOL reuse = TRUE;
    ::setsockopt(e->listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof reuse);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(e->listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::closesocket(e->listen_sock);
        return {};
    }
    if (::listen(e->listen_sock, SOMAXCONN) != 0) {
        ::closesocket(e->listen_sock);
        return {};
    }

    unsigned short bound = 0;
    {
        sockaddr_in actual{};
        int len = sizeof actual;
        if (::getsockname(e->listen_sock, reinterpret_cast<sockaddr*>(&actual), &len) == 0)
            bound = ntohs(actual.sin_port);
    }

    e->iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!e->iocp) { ::closesocket(e->listen_sock); return {}; }

    // 关键：AcceptEx 的完成包投递到**监听套接字所关联**的端口——
    // 必须先把 listen socket 关联进完成端口（key=0；完成时以 ov 区分 accept）
    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(e->listen_sock),
                                 e->iocp, 0, 0) == nullptr) {
        ::CloseHandle(e->iocp);
        ::closesocket(e->listen_sock);
        return {};
    }

    GUID guid = WSAID_ACCEPTEX;
    DWORD got = 0;
    if (::WSAIoctl(e->listen_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &guid, sizeof guid, &e->accept_ex, sizeof e->accept_ex,
                   &got, nullptr, nullptr) != 0) {
        ::CloseHandle(e->iocp);
        ::closesocket(e->listen_sock);
        return {};
    }

    post_accept(*e);
    return {e.release(), bound};
}

int iocp_run(iocp_handle h, std::atomic<bool>& stopping) {
    engine& e = *as_engine(h.engine);
    e.stopping = &stopping;

    std::vector<std::jthread> workers;
    workers.reserve(e.worker_count);
    for (std::size_t i = 0; i < e.worker_count; ++i)
        workers.emplace_back([&e] { worker_loop(e); });

    std::jthread sweeper([&e, &stopping] {
        while (!stopping.load()) {
            std::this_thread::sleep_for(kSweepInterval);
            if (stopping.load()) break;
            sweep(e);
        }
    });

    for (auto& t : workers) t.join();
    sweeper.join();
    close_all(e);
    return 0;
}

void iocp_stop(void* p) {
    engine& e = *as_engine(p);
    SOCKET ls = e.listen_sock;
    if (ls != INVALID_SOCKET) {
        ::shutdown(ls, SD_BOTH);
        ::closesocket(ls);
        e.listen_sock = INVALID_SOCKET;
    }
    // 唤醒全部 worker（每个一个唤醒包）
    for (std::size_t i = 0; i < e.worker_count + 1; ++i)
        if (e.iocp) ::PostQueuedCompletionStatus(e.iocp, 0, 0, nullptr);
}

void iocp_destroy(void* p) {
    engine& e = *as_engine(p);
    close_all(e);
    if (e.iocp) ::CloseHandle(e.iocp);
    delete &e;
}

} // namespace detail
