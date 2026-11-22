// cboot.json 冒烟测试
// 验证：解析（字面量/数字/转义/Unicode 代理对/嵌套/数组）
//       序列化（紧凑/转义/最短浮点往返）
//       反射绑定（struct ↔ value 往返相等）
import cboot.json;
import std;

struct Address {
    std::string city;
    int zip{};
    bool operator==(const Address&) const = default;
};

enum class Level { Debug, Info, Error };

// 自定义类型 codec：Money ↔ JSON 整数
struct Money {
    std::int64_t cents{};
    bool operator==(const Money&) const = default;
};

template <>
struct json::codec<Money> {
    static json::value to_value(const Money& m) {
        return json::value(m.cents);
    }
    static Money from_value(const json::value& v) {
        if (!v.is_int()) throw json::error("cboot.json: money expected integer");
        return Money{v.as_int()};
    }
};

struct User {
    std::string name;
    int age{};
    bool active{};
    double score{};
    std::vector<std::string> tags;
    std::optional<std::string> nick;
    Address addr;
    Level level{};
    bool operator==(const User&) const = default;
};

int main() {
    // ── 1. 解析 ──
    auto v = json::parse(R"({
        "s": "a\"b\\c\n\t中\ud83d\ude00",
        "i": -42, "d": 3.25,
        "arr": [1, 2.5, "x", null, true],
        "obj": {"k": "v", "nested": {"deep": 7}}
    })");
    if (!v.is_object()) return 1;
    if (v.find("s")->as_string() != "a\"b\\c\n\t中\xF0\x9F\x98\x80") return 2;
    if (v.find("i")->as_int() != -42) return 3;
    if (v.find("d")->as_double() != 3.25) return 4;
    if (v.find("arr")->as_array().size() != 5) return 5;
    if (!v.find("arr")->as_array()[3].is_null()) return 6;
    if (v.find("obj")->find("nested")->find("deep")->as_int() != 7) return 7;

    // ── 2. 错误定位 ──
    bool threw = false;
    try { (void)json::parse("{\"a\": }"); } catch (const json::error&) { threw = true; }
    if (!threw) return 8;

    // ── 3. 序列化 + 浮点往返 ──
    auto d2 = json::parse(json::dump(json::value(0.1)));
    if (d2.as_double() != 0.1) return 9;
    if (json::dump(json::parse(R"({"k":"a\"b"})")) != R"({"k":"a\"b"})") {
        return 10;
    }

    // ── 4. 反射绑定：struct → value → dump → parse → struct（往返相等）──
    User u{
        .name = "张三", .age = 30, .active = true, .score = 99.5,
        .tags = {"a", "b", "c"}, .nick = std::nullopt,
        .addr = {.city = "Beijing", .zip = 100000},
        .level = Level::Info,
    };
    std::string text = json::dump(json::from(u));
    User back = json::get_as<User>(json::parse(text));
    if (!(back == u)) return 11;

    // 缺失键保持默认 / 未知键忽略 / 枚举名绑定
    User partial = json::get_as<User>(json::parse(
        R"({"name":"n","age":1,"level":"Error","__unknown__":true})"));
    if (partial.name != "n" || partial.age != 1 || partial.level != Level::Error
        || !partial.tags.empty() || partial.active) return 12;

    // optional null 路径
    if (json::get_as<User>(json::parse(
            R"({"name":"n","nick":null})")).nick.has_value()) return 13;

    // ── 5. 小响应缓冲：内联 / 溢出 / 拷贝 ──
    json::small_buffer<64> sb;
    sb = std::string("pong");
    if (sb.view() != "pong" || sb.size() != 4) return 14;

    sb.clear();
    std::string expect;
    for (int i = 0; i < 100; ++i) {
        const char c = char('a' + i % 26);
        sb.push_back(c);
        expect.push_back(c);
    }
    if (sb.view() != expect) return 15;      // 超过 64 → 溢出到堆

    json::small_buffer<64> copy = sb;        // 拷贝后内容独立
    sb.clear();
    if (copy.view() != expect || !sb.empty()) return 16;

    std::string dumped_expected;
    json::write_to(dumped_expected, u);
    json::small_buffer<64> dumped;           // User 序列化 > 64 字节 → 溢出
    json::write_to(dumped, u);
    if (dumped.view() != dumped_expected) return 17;
    if (dumped.size() <= 64) return 18;

    // ── 6. map / unordered_map ↔ object ──
    std::map<std::string, int> dict{{"a", 1}, {"b", 2}};
    auto dict_v = json::from(dict);
    if (json::dump(dict_v) != R"({"a":1,"b":2})") return 19;
    if (!(json::get_as<std::map<std::string, int>>(dict_v) == dict)) return 20;

    std::string dict_out;
    json::write_to(dict_out, dict);
    if (dict_out != R"({"a":1,"b":2})") return 21;

    std::unordered_map<std::string, int> udict{{"x", 3}, {"y", 4}};
    auto udict_v = json::from(udict);
    if (!(json::get_as<std::unordered_map<std::string, int>>(udict_v) == udict))
        return 22;

    // ── 7. json::codec<T> ──
    Money money{1234};
    auto money_v = json::from(money);
    if (!money_v.is_int() || money_v.as_int() != 1234) return 23;
    if (!(json::get_as<Money>(money_v) == money)) return 24;

    std::string money_out;
    json::write_to(money_out, money);
    if (money_out != "1234") return 25;

    // ── 8. value 直接写出 / get_as<value> ──
    std::string value_out;
    json::write_to(value_out, dict_v);
    if (value_out != R"({"a":1,"b":2})") return 26;
    if (!(json::get_as<json::value>(dict_v) == dict_v)) return 27;

    // ── 9. 错误行列 ──
    try {
        (void)json::parse("{\n  \"a\": }");
        return 28;
    } catch (const json::error& e) {
        const std::string msg = e.what();
        if (msg.find("line 2") == std::string::npos
            || msg.find("column") == std::string::npos) return 29;
    }

    // ── 10. 嵌套深度 ──
    std::string deep(129, '[');
    deep.append(129, ']');
    bool depth_threw = false;
    try {
        (void)json::parse(deep);
    } catch (const json::error& e) {
        depth_threw = std::string(e.what()).find("maximum nesting depth")
            != std::string::npos;
    }
    if (!depth_threw) return 30;

    std::string ok_depth(128, '[');
    ok_depth.append(128, ']');
    try {
        (void)json::parse(ok_depth);
    } catch (...) {
        return 31;
    }

    try {
        (void)json::parse("[[[]]]", json::parse_options{.max_depth = 2});
        return 32;
    } catch (const json::error&) {
        // expected
    }

    std::println("JSON SMOKE: PASS");
    return 0;
}
