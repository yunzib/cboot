/**
 * @file log.component.cppm
 * @brief 日志配置组件 — log.* 绑定、sink 装配、级别覆盖
 */
export module cboot.log.component;

import std;
import cboot.core.annotations;  // Bean / Config / PostConstruct / PreDestroy / bean_name
import cboot.log;               // logger / sinks / layouts
import cboot.meta.reflect;      // type_list

export namespace logging {

/** @brief 日志配置（前缀 log） */
struct [[=Config{"log"}]] LogProps {
    std::string level{"info"};
    std::string format{"text"};
    std::string pattern{"%d %-5level [%thread] %logger - %msg%n"};
    std::string output{"console"};
    std::string file;
    int max_size_mb{10};
    int max_files{5};
    std::string file_level;
    std::vector<std::string> logger_levels;
};

namespace detail {

inline std::string trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return std::string(text);
}

inline level parse_level(std::string_view text, std::string_view key) {
    const auto parsed = level_from(trim(text));
    if (!parsed)
        throw std::invalid_argument(
            "cboot.log: invalid " + std::string(key) + ": "
            + std::string(text));
    return *parsed;
}

} // namespace detail

/**
 * @brief 启动期安装日志配置
 *
 * PostConstruct 在容器构造阶段执行；此后框架/业务日志按配置输出。
 * PreDestroy flush 所有 sink。
 */
class [[=Bean{}]] LogSetup {
public:
    explicit LogSetup(LogProps& props) : props_(props) {}

    [[=PostConstruct{}]] void configure() {
        set_level(detail::parse_level(props_.level, "log.level"));

        if (props_.format == "json") {
            set_layout(std::make_shared<json_layout>());
        } else if (props_.format.empty() || props_.format == "text") {
            set_layout(std::make_shared<text_layout>(props_.pattern));
        } else {
            throw std::invalid_argument(
                "cboot.log: invalid log.format: " + props_.format);
        }

        const bool console =
            props_.output == "console" || props_.output == "both";
        const bool file =
            props_.output == "file" || props_.output == "both";
        if (!console && !file)
            throw std::invalid_argument(
                "cboot.log: invalid log.output: " + props_.output);

        clear_sinks();
        if (console)
            add_sink(std::make_shared<console_sink>());

        if (file) {
            if (props_.file.empty())
                throw std::invalid_argument(
                    "cboot.log: log.file is required when output is file/both");

            const std::size_t max_bytes =
                props_.max_size_mb > 0
                    ? static_cast<std::size_t>(props_.max_size_mb) * 1024 * 1024
                    : 0;
            const level threshold = props_.file_level.empty()
                ? level::trace
                : detail::parse_level(props_.file_level, "log.file_level");
            add_sink(std::make_shared<rolling_file_sink>(
                         props_.file, max_bytes, props_.max_files),
                     threshold);
        }

        for (const auto& entry : props_.logger_levels) {
            const auto eq = entry.find('=');
            if (eq == std::string::npos)
                throw std::invalid_argument(
                    "cboot.log: invalid log.logger_levels entry: " + entry);
            const auto name = detail::trim(std::string_view(entry).substr(0, eq));
            if (name.empty())
                throw std::invalid_argument(
                    "cboot.log: empty logger name in log.logger_levels");
            set_logger_level(
                name, detail::parse_level(
                          std::string_view(entry).substr(eq + 1),
                          "log.logger_levels"));
        }
    }

    [[=PreDestroy{}]] void shutdown() { flush(); }

private:
    LogProps& props_;
};

} // namespace logging

/**
 * @brief 日志契约组件
 *
 * 应用显式加入 components；不加入时默认 console + info 仍可用。
 */
export struct log_component {
    static constexpr bean_name name{"log"};
    using beans = type_list<logging::LogProps, logging::LogSetup>;
    using dependencies = type_list<>;
};
