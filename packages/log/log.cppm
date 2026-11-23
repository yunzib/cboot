/**
 * @file log.cppm
 * @brief 日志门面 — 级别、命名 logger、sink、layout 与 MDC
 *
 * 对标 Logback 核心模型：
 *   - logger 按名称取用，支持按名称前缀覆盖级别
 *   - sink 对标 Appender（console / file / rolling）
 *   - layout 对标 Encoder（text / json）
 *   - context 对标 MDC（线程本地）
 *
 * 本模块随 SDK 安装，默认 console + info 始终可用；
 * log_component 负责从 log.* 配置装配 sink 与级别。
 */
export module cboot.log;

import std;

export namespace logging {

/** @brief 日志级别；off 关闭全部输出 */
enum class level : std::uint8_t {
    trace = 0,
    debug,
    info,
    warn,
    error,
    off,
};

/** @brief 级别名（小写）；off 返回 "off" */
constexpr std::string_view level_name(level lv) noexcept {
    switch (lv) {
    case level::trace: return "trace";
    case level::debug: return "debug";
    case level::info:  return "info";
    case level::warn:  return "warn";
    case level::error: return "error";
    case level::off:   return "off";
    }
    return "info";
}

/** @brief 解析级别名（大小写不敏感，warning → warn）；失败 nullopt */
inline std::optional<level> level_from(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        lower.push_back(c);
    }
    if (lower == "trace") return level::trace;
    if (lower == "debug") return level::debug;
    if (lower == "info") return level::info;
    if (lower == "warn" || lower == "warning") return level::warn;
    if (lower == "error") return level::error;
    if (lower == "off") return level::off;
    return std::nullopt;
}

/**
 * @brief 日志事件
 *
 * logger/message 是视图，只在 write() 调用期间有效；异步 sink 必须复制。
 * context 是写入时的 MDC 快照。
 */
struct record {
    level lv{level::info};
    std::chrono::system_clock::time_point time{};
    std::string_view logger;
    std::string_view message;
    std::vector<std::pair<std::string, std::string>> context;
};

/** @brief 输出目标；line 已按 layout 格式化（可能含换行） */
class sink {
public:
    virtual ~sink() = default;
    virtual void write(level lv, std::string_view line) = 0;
    virtual void flush() {}
};

/** @brief 事件 → 单行文本；对标 Logback Encoder */
class layout {
public:
    virtual ~layout() = default;
    virtual std::string format(const record& rec) const = 0;
};

/**
 * @brief 文本 layout
 *
 * 支持精简 pattern：
 *   %d 时间、%level/%-5level、%logger、%msg、%thread、%n 换行、
 *   %X{key} MDC 字段。
 */
class text_layout final : public layout {
public:
    explicit text_layout(
        std::string pattern = std::string(default_pattern()));

    std::string format(const record& rec) const override;

    static std::string_view default_pattern() {
        return "%d %-5level [%thread] %logger - %msg%n";
    }

private:
    std::string pattern_;
};

/** @brief JSON Lines layout（一行一个对象） */
class json_layout final : public layout {
public:
    std::string format(const record& rec) const override;
};

/** @brief 控制台 sink：trace/debug/info → stdout，warn/error → stderr */
class console_sink final : public sink {
public:
    void write(level lv, std::string_view line) override;
    void flush() override;
};

/** @brief 追加文件 sink；目录不存在时自动创建 */
class file_sink : public sink {
public:
    explicit file_sink(std::string path);
    ~file_sink() override;

    void write(level lv, std::string_view line) override;
    void flush() override;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

/**
 * @brief 按大小轮转的文件 sink
 *
 * 超过 max_bytes 时：app.log → app.log.1 → app.log.2 ...
 * max_bytes <= 0 表示不轮转；max_files <= 0 表示轮转时直接截断。
 */
class rolling_file_sink final : public sink {
public:
    rolling_file_sink(std::string path, std::size_t max_bytes,
                      int max_files);
    ~rolling_file_sink() override;

    void write(level lv, std::string_view line) override;
    void flush() override;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

/** @brief 线程本地日志上下文（对标 MDC） */
namespace context {

void put(std::string key, std::string value);
std::optional<std::string_view> get(std::string_view key);
void erase(std::string_view key);
void clear();
std::vector<std::pair<std::string, std::string>> snapshot();

/** @brief RAII：进入作用域写入 key，离开时恢复旧值 */
class scope {
public:
    scope(std::string key, std::string value);
    ~scope();

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;

private:
    std::string key_;
    std::optional<std::string> old_;
    bool had_ = false;
};

} // namespace context

/** @brief 命名 logger；只保存名称，不持有全局状态 */
class logger {
public:
    explicit logger(std::string_view name);

    std::string_view name() const noexcept { return name_; }

    bool enabled(level lv) const;

    /** @brief 直接写入已格式化消息 */
    void log(level lv, std::string_view message);

    template <class... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level::trace)) return;
        log(level::trace, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level::debug)) return;
        log(level::debug, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level::info)) return;
        log(level::info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level::warn)) return;
        log(level::warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level::error)) return;
        log(level::error, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    std::string name_;
};

/** @brief 取命名 logger */
logger get(std::string_view name);

/** @brief 根 logger（名称 "root"） */
logger root();

// ==================== 全局配置（启动期调用）====================

void set_level(level lv);
void set_layout(std::shared_ptr<layout> formatter);
void add_sink(std::shared_ptr<sink> target,
              level threshold = level::trace);
void clear_sinks();
void set_logger_level(std::string_view name, level lv);
void flush();

} // namespace logging
