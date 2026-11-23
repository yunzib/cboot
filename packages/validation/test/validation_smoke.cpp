/**
 * @file validation_smoke.cpp
 * @brief DTO 校验冒烟测试
 *
 * 覆盖：
 *   - NotBlank / Range 注解扫描
 *   - 默认文案与自定义文案
 *   - 多字段错误聚合、错误顺序
 *   - validate_or_throw 的 invalid 异常
 */
import cboot.validation;
import std;

struct Signup {
    [[=validation::NotBlank{}]] std::string name;
    [[=validation::Range{.min = 18, .max = 120}]] int age{};
};

struct CustomMessage {
    [[=validation::NotBlank{"nickname required"}]] std::string nickname;
    [[=validation::Range{.min = 1, .max = 5, .message = "score must be 1..5"}]]
    double score{};
};

struct Plain {
    std::string any;
    int value{};
};

int main() {
    // ── 合法输入 ───────────────────────────────────────────────────────
    Signup ok{.name = "alice", .age = 18};
    if (!validation::validate(ok).empty()) return 1;

    // ── 多字段错误 ─────────────────────────────────────────────────────
    Signup bad{.name = " \t", .age = 17};
    auto errors = validation::validate(bad);
    if (errors.size() != 2) return 2;
    if (errors[0].field != "name"
        || errors[0].message != "must not be blank") return 3;
    if (errors[1].field != "age"
        || errors[1].message != "must be between 18 and 120") return 4;

    // ── 自定义文案 ─────────────────────────────────────────────────────
    CustomMessage custom{.nickname = "", .score = 6.0};
    auto custom_errors = validation::validate(custom);
    if (custom_errors.size() != 2) return 5;
    if (custom_errors[0].message != "nickname required") return 6;
    if (custom_errors[1].message != "score must be 1..5") return 7;

    // ── 无注解 DTO 不产生错误 ──────────────────────────────────────────
    if (!validation::validate(Plain{.any = "", .value = 0}).empty()) return 8;

    // ── validate_or_throw ──────────────────────────────────────────────
    try {
        validation::validate_or_throw(bad);
        return 9;
    } catch (const validation::invalid& e) {
        if (e.errors().size() != 2) return 10;
    }

    std::println("VALIDATION SMOKE: PASS");
    return 0;
}
