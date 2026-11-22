// 后端选择冒烟：Windows 上选择 epoll 应明确报错（非 Linux 空实现）
import cboot.web.server;
import std;

int main() {
#if defined(_WIN32)
    server s(0, 1, server_io::epoll);
    bool threw = false;
    try {
        (void)s.run();
    } catch (const std::runtime_error& e) {
        threw = std::string_view(e.what()).find("epoll") != std::string_view::npos;
    }
    if (!threw) return 1;
    std::println("BACKEND SELECT SMOKE: PASS (epoll unsupported on Windows)");
#else
    std::println("BACKEND SELECT SMOKE: SKIP (non-Windows host)");
#endif
    return 0;
}
