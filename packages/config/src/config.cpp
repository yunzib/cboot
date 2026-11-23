// cboot.config 实现单元 — JSON 展平 / 占位符展开
module;

#if defined(_WIN32)
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
#include <windows.h>
#else
extern "C" char** environ;
#endif

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

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (ascii_lower(text[i]) != ascii_lower(prefix[i])) return false;
    return true;
}

} // namespace

properties environment(std::string_view prefix) {
    properties out;
    const auto add = [&](std::string_view entry) {
        const std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos) return;
        const std::string_view raw_key = entry.substr(0, eq);
        if (!starts_with_ci(raw_key, prefix)) return;

        std::string key;
        key.reserve(raw_key.size() - prefix.size());
        for (std::size_t i = prefix.size(); i < raw_key.size(); ++i) {
            const char c = ascii_lower(raw_key[i]);
            key.push_back(c == '_' ? '.' : c);
        }
        if (key.empty()) return;
        out.set(std::move(key), std::string(entry.substr(eq + 1)));
    };

#if defined(_WIN32)
    LPCH block = GetEnvironmentStringsA();
    if (!block) return out;
    for (LPCH p = block; *p; p += std::strlen(p) + 1)
        add(std::string_view(p));
    FreeEnvironmentStringsA(block);
#else
    for (char** p = ::environ; p && *p; ++p)
        add(std::string_view(*p));
#endif
    return out;
}

command_line parse_command_line(int argc, char** argv) {
    command_line out;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i] ? argv[i] : "";
        if (!arg.starts_with("--"))
            throw std::invalid_argument(
                "cboot.config: unexpected argument '" + std::string(arg)
                + "' (expected --key=value)");

        std::string_view body = arg.substr(2);
        if (body == "config") {
            if (i + 1 >= argc || !argv[i + 1])
                throw std::invalid_argument("cboot.config: --config requires a path");
            out.config_path = argv[++i];
            continue;
        }
        if (body.starts_with("config=")) {
            out.config_path = std::string(body.substr(7));
            if (out.config_path.empty())
                throw std::invalid_argument("cboot.config: --config path is empty");
            continue;
        }

        const std::size_t eq = body.find('=');
        if (eq == std::string_view::npos || eq == 0)
            throw std::invalid_argument(
                "cboot.config: expected --key=value, got '" + std::string(arg) + "'");
        out.values.set(std::string(body.substr(0, eq)),
                       std::string(body.substr(eq + 1)));
    }
    return out;
}

} // namespace config
