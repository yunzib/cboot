// cboot.config 实现单元 — JSON 展平 / 占位符展开
module cboot.config;

import std;
import cboot.json;

namespace config {
namespace detail {

// 嵌套 JSON → "a.b.c" 扁平键（数组 → "a.0" "a.1" ...）
void flatten(std::string_view prefix, const json::value& v, properties& out) {
    if (v.is_object()) {
        for (const auto& [k, child] : v.as_object())
            flatten(join_key(prefix, k), child, out);
    } else if (v.is_array()) {
        for (std::size_t i = 0; i < v.as_array().size(); ++i)
            flatten(join_key(prefix, std::to_string(i)), v.as_array()[i], out);
    } else if (!v.is_null()) {
        std::string val;
        if (v.is_string())      val = v.as_string();
        else if (v.is_bool())   val = v.as_bool() ? "true" : "false";
        else if (v.is_int())    val = std::to_string(v.as_int());
        else if (v.is_double()) val = std::to_string(v.as_double());
        out.set(std::string(prefix), std::move(val));
    }
}

// ${ENV_VAR} / ${ENV_VAR:default} 占位符展开
// 无环境变量且无默认值 → 保留原文（缺配置在类型转换处 fail-loud）
std::string expand(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
            std::size_t end = raw.find('}', i + 2);
            if (end == std::string_view::npos) {            // 未闭合 → 原文
                out.append(raw.substr(i));
                break;
            }
            std::string_view expr = raw.substr(i + 2, end - i - 2);
            std::size_t colon = expr.find(':');
            std::string_view name = (colon == std::string_view::npos)
                ? expr : expr.substr(0, colon);
            std::string_view def  = (colon == std::string_view::npos)
                ? std::string_view{} : expr.substr(colon + 1);
            const char* env = std::getenv(std::string(name).c_str());
            if (env) out.append(env);
            else if (colon != std::string_view::npos) out.append(def);
            else out.append(raw.substr(i, end - i + 1));
            i = end + 1;
        } else {
            out.push_back(raw[i++]);
        }
    }
    return out;
}

} // namespace detail

properties properties::from_json(std::string_view src) {
    properties out;
    detail::flatten("", json::parse(src), out);
    return out;
}

std::optional<std::string> properties::get_expanded(std::string_view key) const {
    if (auto raw = get_raw(key)) return detail::expand(*raw);
    return std::nullopt;
}

} // namespace config
