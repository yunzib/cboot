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

    std::println("JSON SMOKE: PASS");
    return 0;
}
