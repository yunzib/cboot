/**
 * @file log.cpp
 * @brief 日志实现 — console/file/rolling、text/JSON、全局状态
 */
module cboot.log;

import std;

namespace logging {

namespace {

struct time_parts {
    int year{};
    int month{};
    int day{};
    int hour{};
    int minute{};
    int second{};
    int millis{};
};

time_parts split_time(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    if (auto* p = std::gmtime(&t)) tm = *p;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;
    if (ms.count() < 0) ms += std::chrono::milliseconds(1000);

    return time_parts{
        .year = tm.tm_year + 1900,
        .month = tm.tm_mon + 1,
        .day = tm.tm_mday,
        .hour = tm.tm_hour,
        .minute = tm.tm_min,
        .second = tm.tm_sec,
        .millis = static_cast<int>(ms.count()),
    };
}

std::string text_time(std::chrono::system_clock::time_point tp) {
    const auto t = split_time(tp);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
                       t.year, t.month, t.day, t.hour, t.minute, t.second,
                       t.millis);
}

std::string iso_time(std::chrono::system_clock::time_point tp) {
    const auto t = split_time(tp);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                       t.year, t.month, t.day, t.hour, t.minute, t.second,
                       t.millis);
}

std::string level_text(level lv) {
    std::string out(level_name(lv));
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - ('a' - 'A'));
    return out;
}

std::string thread_text() {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

void append_padded(std::string& out, std::string_view value,
                   std::size_t width, bool left) {
    if (value.size() >= width) {
        out.append(value);
        return;
    }
    const std::string pad(width - value.size(), ' ');
    if (left) {
        out.append(value);
        out.append(pad);
    } else {
        out.append(pad);
        out.append(value);
    }
}

std::string_view context_value(const record& rec, std::string_view key) {
    for (const auto& [k, v] : rec.context)
        if (k == key) return v;
    return {};
}

struct sink_entry {
    std::shared_ptr<sink> target;
    level threshold{level::trace};
};

struct state {
    std::mutex mu;
    level root{level::info};
    std::vector<std::pair<std::string, level>> overrides;
    std::shared_ptr<layout> formatter{std::make_shared<text_layout>()};
    std::vector<sink_entry> sinks;
};

state& global_state() {
    // 有意不析构：避免静态销毁顺序导致退出阶段写已销毁 sink。
    static state* instance = [] {
        auto* s = new state();
        s->sinks.push_back({std::make_shared<console_sink>(), level::trace});
        return s;
    }();
    return *instance;
}

level effective_level(const state& s, std::string_view name) {
    level result = s.root;
    std::size_t best = 0;
    for (const auto& [prefix, lv] : s.overrides) {
        if (name.size() < prefix.size()) continue;
        if (name.substr(0, prefix.size()) != prefix) continue;
        if (name.size() > prefix.size() && name[prefix.size()] != '.')
            continue;
        if (prefix.size() >= best) {
            best = prefix.size();
            result = lv;
        }
    }
    return result;
}

bool enabled_locked(const state& s, std::string_view name, level lv) {
    const level eff = effective_level(s, name);
    return eff != level::off && lv >= eff;
}

} // namespace

// ==================== 文本 / JSON layout ====================

text_layout::text_layout(std::string pattern)
    : pattern_(pattern.empty() ? std::string(default_pattern())
                               : std::move(pattern)) {}

std::string text_layout::format(const record& rec) const {
    std::string out;
    out.reserve(pattern_.size() + rec.message.size() + 64);

    for (std::size_t i = 0; i < pattern_.size();) {
        if (pattern_[i] != '%') {
            out.push_back(pattern_[i++]);
            continue;
        }

        ++i;
        if (i >= pattern_.size()) {
            out.push_back('%');
            break;
        }

        bool left = false;
        if (pattern_[i] == '-') {
            left = true;
            ++i;
        }

        std::size_t width = 0;
        while (i < pattern_.size() && pattern_[i] >= '0'
               && pattern_[i] <= '9') {
            width = width * 10 + std::size_t(pattern_[i] - '0');
            ++i;
        }
        if (i >= pattern_.size()) {
            out.push_back('%');
            break;
        }

        const auto starts = [&](std::string_view token) {
            return pattern_.compare(i, token.size(), token) == 0;
        };

        if (pattern_[i] == 'd') {
            ++i;
            append_padded(out, text_time(rec.time), width, left);
        } else if (starts("level")) {
            i += 5;
            append_padded(out, level_text(rec.lv), width, left);
        } else if (starts("logger")) {
            i += 6;
            append_padded(out, rec.logger, width, left);
        } else if (starts("msg")) {
            i += 3;
            append_padded(out, rec.message, width, left);
        } else if (starts("thread")) {
            i += 6;
            append_padded(out, thread_text(), width, left);
        } else if (pattern_[i] == 'n') {
            ++i;
            out.push_back('\n');
        } else if (pattern_[i] == 'X' && i + 1 < pattern_.size()
                   && pattern_[i + 1] == '{') {
            const std::size_t close = pattern_.find('}', i + 2);
            if (close == std::string::npos) {
                out.push_back('%');
                break;
            }
            const std::string_view key(pattern_.data() + i + 2,
                                       close - i - 2);
            append_padded(out, context_value(rec, key), width, left);
            i = close + 1;
        } else {
            out.push_back('%');
            out.push_back(pattern_[i++]);
        }
    }
    return out;
}

std::string json_layout::format(const record& rec) const {
    std::string out;
    out.reserve(rec.message.size() + 128);
    out += "{\"time\":\"";
    out += iso_time(rec.time);
    out += "\",\"level\":\"";
    out += level_name(rec.lv);
    out += "\",\"logger\":\"";
    out += json_escape(rec.logger);
    out += "\",\"msg\":\"";
    out += json_escape(rec.message);
    out += '"';
    for (const auto& [key, value] : rec.context) {
        out += ",\"";
        out += json_escape(key);
        out += "\":\"";
        out += json_escape(value);
        out += '"';
    }
    out += "}\n";
    return out;
}

// ==================== sinks ====================

void console_sink::write(level lv, std::string_view line) {
    std::ostream& os = lv >= level::warn ? std::cerr : std::cout;
    os.write(line.data(), static_cast<std::streamsize>(line.size()));
    os.flush();
}

void console_sink::flush() {
    std::cout.flush();
    std::cerr.flush();
}

struct file_sink::impl {
    std::filesystem::path path;
    std::ofstream out;
};

file_sink::file_sink(std::string path)
    : impl_(std::make_unique<impl>()) {
    impl_->path = std::move(path);

    const auto parent = impl_->path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
            throw std::runtime_error(
                "cboot.log: cannot create log directory: "
                + parent.string());
    }

    impl_->out.open(impl_->path, std::ios::binary | std::ios::app);
    if (!impl_->out)
        throw std::runtime_error(
            "cboot.log: cannot open log file: " + impl_->path.string());
}

file_sink::~file_sink() = default;

void file_sink::write(level, std::string_view line) {
    if (!impl_->out) return;
    impl_->out.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->out.flush();
}

void file_sink::flush() {
    if (impl_->out) impl_->out.flush();
}

struct rolling_file_sink::impl {
    std::filesystem::path path;
    std::size_t max_bytes{};
    int max_files{};
    std::ofstream out;
    std::size_t size{};

    void open(bool truncate) {
        auto mode = std::ios::binary | std::ios::app;
        if (truncate) mode = std::ios::binary | std::ios::trunc;
        out.open(path, mode);
        if (!out)
            throw std::runtime_error(
                "cboot.log: cannot open log file: " + path.string());

        if (!truncate) {
            std::error_code ec;
            size = static_cast<std::size_t>(std::filesystem::file_size(path, ec));
            if (ec) size = 0;
        } else {
            size = 0;
        }
    }

    void rotate() {
        out.close();

        if (max_files <= 0) {
            open(true);
            return;
        }

        std::error_code ec;
        for (int i = max_files - 1; i >= 1; --i) {
            const auto from = path.string() + "." + std::to_string(i);
            const auto to = path.string() + "." + std::to_string(i + 1);
            std::filesystem::remove(to, ec);
            ec.clear();
            std::filesystem::rename(from, to, ec);
            ec.clear();
        }

        const auto first = path.string() + ".1";
        std::filesystem::remove(first, ec);
        ec.clear();
        std::filesystem::rename(path, first, ec);
        ec.clear();

        open(true);
    }
};

rolling_file_sink::rolling_file_sink(std::string path,
                                     std::size_t max_bytes,
                                     int max_files)
    : impl_(std::make_unique<impl>()) {
    impl_->path = std::move(path);
    impl_->max_bytes = max_bytes;
    impl_->max_files = max_files;

    const auto parent = impl_->path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
            throw std::runtime_error(
                "cboot.log: cannot create log directory: "
                + parent.string());
    }
    impl_->open(false);
}

rolling_file_sink::~rolling_file_sink() = default;

void rolling_file_sink::write(level, std::string_view line) {
    if (!impl_->out) return;
    if (impl_->max_bytes > 0 && impl_->size > 0
        && impl_->size + line.size() > impl_->max_bytes) {
        impl_->rotate();
    }
    impl_->out.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->out.flush();
    impl_->size += line.size();
}

void rolling_file_sink::flush() {
    if (impl_->out) impl_->out.flush();
}

// ==================== 全局配置 / logger ====================

void set_level(level lv) {
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    s.root = lv;
}

void set_layout(std::shared_ptr<layout> formatter) {
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    s.formatter = formatter ? std::move(formatter)
                            : std::make_shared<text_layout>();
}

void add_sink(std::shared_ptr<sink> target, level threshold) {
    if (!target) return;
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    s.sinks.push_back({std::move(target), threshold});
}

void clear_sinks() {
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    s.sinks.clear();
}

void set_logger_level(std::string_view name, level lv) {
    if (name.empty()) return;
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    for (auto& [prefix, value] : s.overrides) {
        if (prefix == name) {
            value = lv;
            return;
        }
    }
    s.overrides.emplace_back(std::string(name), lv);
}

void flush() {
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    for (auto& entry : s.sinks) {
        try {
            entry.target->flush();
        } catch (...) {
        }
    }
}

logger::logger(std::string_view name) : name_(name) {}

bool logger::enabled(level lv) const {
    if (lv == level::off) return false;
    auto& s = global_state();
    std::lock_guard lock(s.mu);
    return enabled_locked(s, name_, lv);
}

void logger::log(level lv, std::string_view message) {
    if (!enabled(lv)) return;

    record rec{
        .lv = lv,
        .time = std::chrono::system_clock::now(),
        .logger = name_,
        .message = message,
        .context = context::snapshot(),
    };

    auto& s = global_state();
    std::lock_guard lock(s.mu);
    if (!enabled_locked(s, name_, lv)) return;

    try {
        std::string line;
        bool formatted = false;
        for (const auto& entry : s.sinks) {
            if (lv < entry.threshold) continue;
            if (!formatted) {
                line = s.formatter->format(rec);
                formatted = true;
            }
            entry.target->write(lv, line);
        }
    } catch (...) {
        // 日志失败不能影响业务流程
    }
}

logger get(std::string_view name) { return logger(name); }

logger root() { return logger("root"); }

// ==================== MDC ====================

namespace context {

namespace {
thread_local std::vector<std::pair<std::string, std::string>> tls_context;
}

void put(std::string key, std::string value) {
    for (auto& [k, v] : tls_context) {
        if (k == key) {
            v = std::move(value);
            return;
        }
    }
    tls_context.emplace_back(std::move(key), std::move(value));
}

std::optional<std::string_view> get(std::string_view key) {
    for (const auto& [k, v] : tls_context)
        if (k == key) return v;
    return std::nullopt;
}

void erase(std::string_view key) {
    std::erase_if(tls_context,
                  [&](const auto& item) { return item.first == key; });
}

void clear() { tls_context.clear(); }

std::vector<std::pair<std::string, std::string>> snapshot() {
    return tls_context;
}

scope::scope(std::string key, std::string value) : key_(std::move(key)) {
    if (auto old = get(key_)) {
        old_.emplace(*old);
        had_ = true;
    }
    put(key_, std::move(value));
}

scope::~scope() {
    if (had_) put(key_, std::move(*old_));
    else erase(key_);
}

} // namespace context

} // namespace logging
