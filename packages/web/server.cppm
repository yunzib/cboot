module;

// GCC16 模块边界守卫（kwik-ui 同款）：windows.h→stdlib.h→mm_malloc→xmmintrin
// 文本链与 import std 的同名声明产生语言链接冲突——C 标准头直取 mingw 纯 C
// 版本 + 预置 intrin guard（声明仍由 import std 提供，用户代码无感）
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

export module cboot.web.server;

import std;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.routing;

/** @brief 网络后端选择（平台/场景） */
export enum class server_io {
    threads,   // 阻塞 recv + 连接线程池（跨平台，连接数百级）
    iocp,      // 完成端口（Windows，连接万级）
    epoll,     // 就绪事件（Linux；Windows 上选择会明确报错）
};

/** @brief IOCP / epoll 引擎句柄（src/web/iocp.cpp、src/web/epoll.cpp 实现） */
namespace detail {
    struct iocp_handle {
        void* engine = nullptr;
        unsigned short port = 0;
    };
    iocp_handle iocp_create(router& r, unsigned short port, std::size_t workers,
                            std::size_t max_conns, std::size_t handler_timeout_ms);
    int iocp_run(iocp_handle h, std::atomic<bool>& stopping);
    void iocp_stop(void* engine);
    void iocp_destroy(void* engine);

    // epoll 后端（Linux 条件编译；非 Linux 平台返回空句柄 → run() 明确报错）
    struct epoll_handle {
        void* engine = nullptr;
        unsigned short port = 0;
    };
    epoll_handle epoll_create(router& r, unsigned short port, std::size_t workers,
                              std::size_t max_conns, std::size_t handler_timeout_ms);
    int epoll_run(epoll_handle h, std::atomic<bool>& stopping);
    void epoll_stop(void* engine);
    void epoll_destroy(void* engine);
} // namespace detail

/** @brief 服务器配置（可扩展；避免位置参数膨胀） */
export struct server_config {
    unsigned short port = 8080;
    std::size_t workers = 0;            // 0 = hardware_concurrency
    server_io io = server_io::threads;
    std::size_t max_connections = 0;    // 0 = 不限；超出 → 503 后断开
    std::size_t handler_timeout_ms = 0; // 0 = 不限；handler 超预算 → 504（SLA，不可抢占）
    std::size_t send_timeout_ms = 30'000; // 写超时（慢客户端）→ 断开
};

/**
 * @brief 嵌入式 HTTP 服务器
 *
 * 口径（M1.5）：
 *   - 协议层：增量解析（pipelining）/ keep-alive / 405+Allow / 413 / 超时
 *   - 后端：threads / iocp（Windows）/ epoll（Linux）
 *   - 背压：max_connections 超限 → 503（线程/IOCP/epoll 后端一致）
 *   - 连接先入队/完成通知驱动；workers=0 → hardware_concurrency
 *   - port=0 → 系统分配（实际端口经 port() 读取）
 */
export class server {
public:
    explicit server(server_config cfg)
        : port_(cfg.port), worker_count_(cfg.workers), io_(cfg.io),
          max_conns_(cfg.max_connections),
          handler_timeout_ms_(cfg.handler_timeout_ms),
          send_timeout_ms_(cfg.send_timeout_ms) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            throw std::runtime_error("cboot.web: WSAStartup failed");
        wsa_started_ = true;
    }
    /** @brief 兼容旧形式（port, workers, io） */
    explicit server(unsigned short port = 8080, std::size_t workers = 0,
                    server_io io = server_io::threads)
        : server(server_config{.port = port, .workers = workers, .io = io}) {}
    ~server() {
        if (iocp_engine_) detail::iocp_destroy(iocp_engine_);
        if (epoll_engine_) detail::epoll_destroy(epoll_engine_);
        shutdown_workers();
        SOCKET s = listen_sock_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) ::closesocket(s);
        if (wsa_started_) WSACleanup();
    }
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    /** @brief 挂载 bean 的路由（转 router::mount） */
    template <class T>
    void mount(T& obj) {
        router_.mount(obj);
    }

    /**
     * @brief 挂载 bean + 切面链（转 router::mount 切面版）
     *
     * 约束排除 AspectRegistry：mount(obj, registry, aspects...) 走下面的重载。
     */
    template <class T, class... As>
        requires (sizeof...(As) > 0 && (!web::AspectRegistry<As> && ...))
    void mount(T& obj, As... aspects) {
        router_.mount(obj, std::move(aspects)...);
    }

    /**
     * @brief 挂载 bean + 方法级切面选择（转 router::mount 注册表版）
     *
     * [[=web::UseAspect{"name"}]] 按名从 registry 选择切面 Bean；
     * aspects 为全局切面（最外层，可为空）。
     */
    template <class T, web::AspectRegistry Registry, class... As>
    void mount(T& obj, Registry registry, As... aspects) {
        router_.mount(obj, std::move(registry), std::move(aspects)...);
    }

    /** @brief 注册请求中间件（转 router::use） */
    void use(std::string name, int order, web::middleware_fn fn) {
        router_.use(std::move(name), order, std::move(fn));
    }

    /** @brief accept 循环（select 超时轮询 + 连接入队；实现于 src/web/server.cpp） */
    int run();

    /** @brief 实际绑定端口（构造函数传 0 时为系统分配端口） */
    unsigned short port() const { return bound_port_.load(); }

    /**
     * @brief 停止服务（幂等）
     *
     * WinSock 纪律：跨线程 closesocket 不能可靠唤醒阻塞中的 accept——
     * 循环侧用 select(100ms 超时) 轮询 stopping_ 标志；stop 置位、
     * 关闭 listen 套接字并唤醒工作线程（排空队列后退出）
     */
    void stop() {
        stopping_.store(true);
        if (iocp_engine_) detail::iocp_stop(iocp_engine_);
        if (epoll_engine_) detail::epoll_stop(epoll_engine_);
        SOCKET s = listen_sock_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            ::shutdown(s, SD_BOTH);      // 尽力唤醒阻塞方（非唯一唤醒手段）
            ::closesocket(s);
        }
        {
            std::lock_guard lk(pool_mu_);
            pool_stop_ = true;
        }
        pool_cv_.notify_all();
    }

private:
    void handle_client(SOCKET c);    // 单连接：读请求 → dispatch → 写响应
    void worker_loop();              // 线程池工作循环（实现于 src）
    void shutdown_workers();         // 停池 + join（析构/stop 共用）

    router router_;
    unsigned short port_;
    std::atomic<unsigned short> bound_port_{0};
    std::size_t worker_count_;
    server_io io_;
    std::size_t max_conns_ = 0;              // 0 = 不限
    std::size_t handler_timeout_ms_ = 0;
    std::size_t send_timeout_ms_ = 30'000;
    std::atomic<std::size_t> active_conns_{0}; // 线程后端计数（accept 起 → 处理完止）

    std::atomic<bool> stopping_{false};
    std::atomic<SOCKET> listen_sock_{INVALID_SOCKET};
    bool wsa_started_ = false;
    void* iocp_engine_ = nullptr;    // IOCP 引擎（run 时创建）
    void* epoll_engine_ = nullptr;   // epoll 引擎（Linux；run 时创建）

    // 连接队列 + 工作线程
    std::mutex pool_mu_;
    std::condition_variable pool_cv_;
    std::deque<SOCKET> pool_conns_;
    std::vector<std::jthread> pool_;
    bool pool_stop_ = false;
};
