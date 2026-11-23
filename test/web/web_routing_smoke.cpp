/**
 * @file web_routing_smoke.cpp
 * @brief cboot.web 路由冒烟测试（纯逻辑，无 socket）
 *
 * 验证：编译期路由表 / 路径参数 / body DTO / DTO 字段校验与 400 映射 /
 *       query 解码 / URL 百分号解码 / PUT-DELETE-PATCH / 405+Allow / 响应头
 */
import cboot.async;
import cboot.json;
import cboot.validation;
import cboot.web.http;
import cboot.web.routing;
import std;

struct CreateUser {
    std::string name;
    int age{};
};

struct ValidatedUser {
    [[=validation::NotBlank{}]] std::string name;
    [[=validation::Range{.min = 18, .max = 120}]] int age{};
};

struct UserDto {
    std::string name;
    int age{};
};

// 协程 handler 的内部任务（co_await 链）
task<int> doubled(int id) { co_return id * 2; }

struct UserApi {
    // 协程 handler：返回 task<Response>（可 co_await 其他任务）
    [[=Get{"/co/{id}"}]]
    task<Response> co_get(int id) {
        const int v = co_await doubled(id);
        co_return json_response(json::value(v));
    }

    [[=Get{"/users/{id}"}]]
    Response get_user(int id) {
        return json_response(UserDto{.name = "u" + std::to_string(id), .age = id});
    }

    [[=Post{"/users"}]]
    Response create_user(const CreateUser& dto) {
        return json_response(UserDto{.name = dto.name + "!", .age = dto.age});
    }

    [[=Post{"/validated"}]]
    Response create_validated(const ValidatedUser& dto) {
        return json_response(dto);
    }

    [[=Put{"/users/{id}"}]]
    Response replace_user(int id, const CreateUser& dto) {
        return json_response(UserDto{.name = dto.name + "#put" + std::to_string(id),
                                     .age = dto.age});
    }

    [[=Delete{"/users/{id}"}]]
    Response delete_user(int id) {
        return json_response(json::value("deleted " + std::to_string(id)));
    }

    [[=Patch{"/users/{id}"}]]
    Response patch_user(int id) {
        return json_response(json::value("patched " + std::to_string(id)));
    }

    [[=Get{"/users/{id}/fields/{field}"}]]
    Response get_field(int id, std::string_view field) {
        return json_response(
            json::value("id=" + std::to_string(id) + ",field=" + std::string(field)));
    }

    [[=Get{"/echo"}]]
    Response echo(const Request& req) {
        return json_response(std::string(req.query_get("q").value_or("<none>")));
    }

    [[=Get{"/esc"}]]                       // 流式 JSON 转义正确性
    Response esc() {
        struct EscDto { std::string s; int n; };
        std::string tricky = "a\"b\\c\n\t";
        tricky.push_back('\x01');
        return json_response(EscDto{tricky, 7});
    }

    [[=Get{"/hdr"}]]
    Response hdr() {
        Response r = json_response(json::value("ok"));
        return r.header("X-Trace", "abc").header("Cache-Control", "no-store");
    }
};

int main() {
    UserApi api;
    router r;
    r.mount(api);
    if (r.size() != 11) return 1;

    // ── 路径参数（数值）──
    auto g = r.dispatch("GET", "/users/42", "");
    auto v = json::parse(g.body);
    if (g.status != 200 || v.find("name")->as_string() != "u42"
        || v.find("age")->as_int() != 42) return 2;

    // ── body DTO（POST）──
    auto p = r.dispatch("POST", "/users", R"({"name":"bob","age":30})");
    auto v2 = json::parse(p.body);
    if (v2.find("name")->as_string() != "bob!" || v2.find("age")->as_int() != 30) return 3;

    // ── body DTO 字段校验：合法 200 / 非法 400 + 错误列表 ──────────────
    auto valid = r.dispatch("POST", "/validated", R"({"name":"alice","age":30})");
    if (valid.status != 200) return 18;

    auto invalid = r.dispatch("POST", "/validated", R"({"name":"  ","age":17})");
    if (invalid.status != 400) return 19;
    auto invalid_body = json::parse(invalid.body);
    const json::value* errors = invalid_body.find("errors");
    if (!errors || !errors->is_array() || errors->as_array().size() != 2)
        return 20;
    if (errors->as_array()[0].find("field")->as_string() != "name"
        || errors->as_array()[1].find("field")->as_string() != "age")
        return 21;

    // JSON 语法错误 / 字段类型错误同样归入 400
    if (r.dispatch("POST", "/validated", R"({"name":)").status != 400)
        return 22;
    if (r.dispatch("POST", "/validated", R"({"name":1,"age":30})").status != 400)
        return 23;

    // ── PUT / DELETE / PATCH ──
    auto u = r.dispatch("PUT", "/users/5", R"({"name":"carl","age":9})");
    if (json::parse(u.body).find("name")->as_string() != "carl#put5") return 4;
    auto d = r.dispatch("DELETE", "/users/5", "");
    if (json::parse(d.body).as_string() != "deleted 5") return 5;
    auto pc = r.dispatch("PATCH", "/users/5", "");
    if (json::parse(pc.body).as_string() != "patched 5") return 6;

    // ── 多路径参数 + URL 解码（%E4%B8%AD = "中"）──
    auto f = r.dispatch("GET", "/users/7/fields/%E4%B8%AD", "");
    if (json::parse(f.body).as_string() != "id=7,field=中") return 7;

    // ── query 解码（'+' → 空格；%XX）──
    auto e = r.dispatch("GET", "/echo?q=hello+world%21", "");
    if (json::parse(e.body).as_string() != "hello world!") return 8;

    // ── 响应头 ──
    auto h = r.dispatch("GET", "/hdr", "");
    bool has_trace = false, has_cache = false;
    for (auto& [k, val] : h.headers) {
        if (k == "X-Trace" && val == "abc") has_trace = true;
        if (k == "Cache-Control" && val == "no-store") has_cache = true;
    }
    if (!has_trace || !has_cache) return 9;

    // ── 404 / 405 + Allow / 路径参数转换失败 ──
    if (r.dispatch("GET", "/nope", "").status != 404) return 10;
    auto mis = r.dispatch("POST", "/users/1", "");        // 路径存在，方法不允许
    if (mis.status != 405) return 11;
    bool allow_ok = false;
    for (auto& [k, val] : mis.headers)
        if (k == "Allow" && val.find("GET") != std::string::npos
            && val.find("PUT") != std::string::npos) allow_ok = true;
    if (!allow_ok) return 12;
    if (r.dispatch("GET", "/users/not_a_number", "").status != 500) return 13;

    // ── 流式 JSON 转义（引号/反斜杠/换行/制表/控制字符）──
    auto esc = r.dispatch("GET", "/esc", "");
    if (esc.status != 200) return 16;
    auto ev = json::parse(esc.body);
    std::string expected = "a\"b\\c\n\t";
    expected.push_back('\x01');
    if (ev.find("s")->as_string() != expected
        || ev.find("n")->as_int() != 7) return 17;

    // ── 协程 handler（task<Response>）──
    auto co = r.dispatch("GET", "/co/21", "");
    if (co.status != 200 || json::parse(co.body).as_int() != 42) return 15;

    std::println("WEB ROUTING SMOKE: PASS");
    return 0;
}
