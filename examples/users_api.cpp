// cboot 全家桶示例 — DI + 配置绑定 + JSON + REST 路由（一行启动）
//
// 运行：
//   cmake --build --preset gcc --target cboot_example_users
//   ./build/examples/cboot_example_users.exe         # 监听 8080
//   curl http://127.0.0.1:8080/api/users/42
//   curl -X POST -d '{"name":"bob","age":30}' http://127.0.0.1:8080/api/users
import cboot.aop;
import cboot.core.annotations;
import cboot.core.context;
import cboot.json;
import cboot.meta.reflect;
import cboot.web.http;
import cboot.app;
import std;

// ==================== 切面（编译期织入，零代理）====================

struct RequestLog {                     // 耗时 + 方法名（call_info 来自反射）
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        const auto t0 = std::chrono::steady_clock::now();
        Response r = next();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::println("[aop] {}.{} -> {} ({} us)", ci.bean, ci.method, r.status, us);
        return r;
    }
};

// ==================== 配置 ====================

struct [[=Config{"db"}]] DbProps {          // 通用配置 bean：任意结构体 + 前缀
    std::string host{"localhost"};
    int port{5432};
    int pool_size{8};
};

// ==================== 领域 ====================

struct UserDto {
    std::string name;
    int age{};
};

struct CreateUser {
    std::string name;
    int age{};
};

// ==================== Bean 层 ====================

struct [[=Bean{"repo"}]] UserRepository {
    UserRepository() { std::println("[life] repo ctor"); }
    UserDto find(int id) {
        return UserDto{.name = "user" + std::to_string(id), .age = 20 + id};
    }
    UserDto save(const CreateUser& in) {
        return UserDto{.name = in.name + "#new", .age = in.age};
    }
    [[=PreDestroy{}]] void close() { std::println("[life] repo close"); }
};

struct [[=Bean{}]] UserService {
    UserService(UserRepository& repo) : repo_(repo) {}
    [[=PostConstruct{}]] void warm_up() { std::println("[life] service warm_up"); }
    UserDto find(int id) { return repo_.find(id); }
    UserDto create(const CreateUser& in) { return repo_.save(in); }
    UserRepository& repo_;
};

// ==================== 控制器（bean + 方法路由注解）====================

struct [[=Bean{}]] UserApi {              // 控制器也是 bean：显式标注
    UserApi(UserService& svc, DbProps& cfg) : svc_(svc), cfg_(cfg) {}

    [[=Get{"/api/config"}]]
    Response show_config() { return json_response(cfg_); }

    [[=Get{"/api/users/{id}"}]]
    Response get_user(int id) {
        return json_response(svc_.find(id));
    }

    [[=Post{"/api/users"}]]
    Response create_user(const CreateUser& in) {
        return json_response(svc_.create(in));
    }

    [[=Get{"/api/health"}]]
    Response health(const Request& req) {
        return json_response(
            json::value(std::string("ok?verbose=")
                + std::string(req.query_get("verbose").value_or("0"))));
    }

    UserService& svc_;
    DbProps& cfg_;
};

// ==================== 应用（身份 + 配置载体）====================

struct ShopApp : application<ShopApp> {
    static constexpr std::string_view config_path = "app.json"; // JSON 配置（替代 YAML）
    static constexpr unsigned short default_port = 8080;        // 配置无 server.port 时用
    static constexpr auto aspects = std::tuple<RequestLog>{};   // 可选（0..N 个切面）
};

int main(int argc, char** argv) {
    return ShopApp{}.run();
}
