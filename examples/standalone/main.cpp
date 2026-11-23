// cboot 独立应用示例 — 通过 Release 安装的 SDK 构建
//
// 构建与运行见同目录 README.md；配置见同目录 app.json。
import cboot.app;
import std;

// ==================== 切面（编译期织入，零代理）====================

class RequestLog {
public:
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

class [[=Config{"db"}]] DbProps {
public:
    std::string host{"localhost"};
    int port{5432};
    int pool_size{8};
};

// ==================== 领域 DTO（聚合体，供 JSON 绑定）====================

struct UserDto {
    std::string name;
    int age{};
};

struct CreateUser {
    std::string name;
    int age{};
};

// ==================== Bean 层 ====================

class [[=Bean{"repo"}]] UserRepository {
public:
    UserRepository() { std::println("[life] repo ctor"); }

    UserDto find(int id) {
        ++hits_;
        return UserDto{.name = "user" + std::to_string(id), .age = 20 + id};
    }

    UserDto save(const CreateUser& in) {
        ++hits_;
        return UserDto{.name = in.name + "#new", .age = in.age};
    }

    int hits() const { return hits_; }

    [[=PreDestroy{}]] void close() {
        std::println("[life] repo close (hits={})", hits_);
    }

private:
    int hits_ = 0;      // 私有数据成员允许；框架不访问
};

class [[=Bean{}]] UserService {
public:
    UserService(UserRepository& repo, DbProps& db,
                [[=Value{"app.page_size"}]] int page_size)
        : repo_(repo), db_(db), page_size_(page_size) {}

    [[=PostConstruct{}]] void warm_up() {
        std::println("[life] service warm_up (db={}:{}, pool={}, page={})",
                     db_.host, db_.port, db_.pool_size, page_size_);
    }

    UserDto find(int id) { return repo_.find(id); }
    UserDto create(const CreateUser& in) { return repo_.save(in); }
    int page_size() const { return page_size_; }

private:
    UserRepository& repo_;
    DbProps& db_;
    int page_size_;
};

// ==================== 控制器（bean + 方法路由注解）====================

class [[=Bean{}]] UserApi {
public:
    UserApi(UserService& svc, DbProps& db) : svc_(svc), db_(db) {}

    [[=Get{"/api/config"}]]
    Response show_config() { return json_response(db_); }

    [[=Get{"/api/users/{id}"}]]
    Response get_user(int id) { return json_response(svc_.find(id)); }

    [[=Post{"/api/users"}]]
    Response create_user(const CreateUser& in) {
        return json_response(svc_.create(in));
    }

    [[=Get{"/api/page-size"}]]
    Response page_size() { return json_response(svc_.page_size()); }

    [[=Get{"/api/health"}]]
    Response health(const Request& req) {
        return json_response(json::value(
            std::string("ok page_size=") + std::to_string(svc_.page_size())
            + " verbose=" + std::string(req.query_get("verbose").value_or("0"))));
    }

private:
    UserService& svc_;
    DbProps& db_;
};

// ==================== 应用 ====================

class ShopApp : public application<ShopApp> {
public:
    ShopApp() = default;

    static constexpr std::string_view config_path = "app.json";
    static constexpr unsigned short default_port = 8080;
    static constexpr auto aspects = std::tuple<RequestLog>{};
};

int main(int argc, char** argv) {
    return ShopApp{}.run(argc, argv);
}
