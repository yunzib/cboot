export module cboot.schedule;

import std;
import cboot.core.annotations;   // bean_name
import cboot.meta.reflect;       // type_list

/**
 * @brief 定时任务注解（契约组件 cboot.schedule）
 *
 *   [[=Scheduled{1000}]]                              // 固定延迟 1s
 *   [[=Scheduled{fixed_delay_ms = 500}]]              // 固定延迟 0.5s
 *   [[=Scheduled{.expression = "* * * * * *"}]]       // 6 段 cron：每秒
 *   [[=Scheduled{.expression = "0/5 * * * *"}]]       // 5 段 cron：每 5 分钟
 *
 * fixed_delay_ms 与 expression 必须二选一。方法必须 public、非静态、无参数。
 */
export struct Scheduled {
    std::int64_t fixed_delay_ms{};
    bean_name expression{};
};

// ==================== cron 表达式 ====================

/**
 * @brief 解析后的 cron 规格（位图）
 *
 * 5 段：分 时 日 月 周；6 段：秒 分 时 日 月 周。
 * 周字段 0/7 = 周日，1-6 = 周一至周六。
 * 日字段与周字段同时受限时按 OR 匹配（经典 cron 语义），否则按 AND。
 */
export struct cron_spec {
    std::uint64_t seconds{};
    std::uint64_t minutes{};
    std::uint64_t hours{};
    std::uint64_t days_of_month{};
    std::uint64_t months{};
    std::uint64_t days_of_week{};
    bool has_seconds{};
    bool dom_restricted{};
    bool dow_restricted{};
};

export namespace cron_detail {

consteval void fail(std::string_view expr, std::string_view why) {
    throw std::define_static_string(
        "cboot: invalid cron expression '" + std::string(expr)
        + "': " + std::string(why));
}

consteval std::uint64_t bit(int v) {
    return std::uint64_t{1} << v;
}

consteval std::uint64_t range_bits(int lo, int hi) {
    std::uint64_t m = 0;
    for (int v = lo; v <= hi; ++v) m |= bit(v);
    return m;
}

consteval int number(std::string_view s, std::string_view expr) {
    if (s.empty()) fail(expr, "empty number");
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') fail(expr, "expected numeric field");
        v = v * 10 + (c - '0');
        if (v > 1000) fail(expr, "number out of range");
    }
    return v;
}

consteval std::uint64_t item(std::string_view s, int lo, int hi,
                             std::string_view expr) {
    if (s.empty()) fail(expr, "empty field item");

    std::uint64_t step = 1;
    const auto slash = s.find('/');
    if (slash != std::string_view::npos) {
        step = static_cast<std::uint64_t>(number(s.substr(slash + 1), expr));
        if (step == 0) fail(expr, "step must be > 0");
        s = s.substr(0, slash);
    }

    int a = lo;
    int b = hi;
    if (s == "*") {
        // 全范围
    } else {
        const auto dash = s.find('-');
        if (dash == std::string_view::npos) {
            a = number(s, expr);
            b = (slash != std::string_view::npos) ? hi : a;
        } else {
            a = number(s.substr(0, dash), expr);
            b = number(s.substr(dash + 1), expr);
        }
    }

    if (a < lo || b > hi || a > b) fail(expr, "field value out of range");

    std::uint64_t mask = 0;
    for (int v = a; v <= b; v += static_cast<int>(step)) {
        // 周字段 7 = 周日，归一到 0
        const int nv = (hi == 7 && v == 7) ? 0 : v;
        mask |= bit(nv);
    }
    return mask;
}

consteval std::uint64_t field(std::string_view s, int lo, int hi,
                              bool& restricted, bool allow_question,
                              std::string_view expr) {
    if (s == "*" || (allow_question && s == "?")) {
        restricted = false;
        return range_bits(lo, hi);
    }
    restricted = true;

    std::uint64_t mask = 0;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const auto comma = s.find(',', pos);
        const auto part = s.substr(pos, comma == std::string_view::npos
                                            ? std::string_view::npos
                                            : comma - pos);
        mask |= item(part, lo, hi, expr);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    if (mask == 0) fail(expr, "field is empty");
    return mask;
}

consteval auto split(std::string_view expr) {
    std::array<std::string_view, 6> out{};
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < expr.size()) {
        while (i < expr.size() && (expr[i] == ' ' || expr[i] == '\t')) ++i;
        if (i >= expr.size()) break;
        const auto start = i;
        while (i < expr.size() && expr[i] != ' ' && expr[i] != '\t') ++i;
        if (n >= out.size()) fail(expr, "expected 5 or 6 fields");
        out[n++] = expr.substr(start, i - start);
    }
    if (n != 5 && n != 6) fail(expr, "expected 5 or 6 fields");
    return std::pair{out, n};
}

inline std::tm local_tm(std::time_t t) {
    // std::localtime 返回静态缓冲区；调度器可能多 hook 并发，复制时加锁
    static std::mutex mu;
    std::lock_guard lock(mu);
    std::tm out{};
    if (auto* p = std::localtime(&t)) out = *p;
    return out;
}

inline std::time_t normalize(std::tm tm) {
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

inline bool day_matches(const std::tm& tm, const cron_spec& s) {
    const bool dom = ((s.days_of_month >> tm.tm_mday) & 1u) != 0;
    const bool dow = ((s.days_of_week >> tm.tm_wday) & 1u) != 0;
    if (s.dom_restricted && s.dow_restricted) return dom || dow;
    return dom && dow;
}

} // namespace cron_detail

/** @brief 解析 cron 表达式；非法表达式在编译期报错 */
export consteval cron_spec parse_cron(std::string_view expr) {
    auto [f, n] = cron_detail::split(expr);
    cron_spec s{};
    bool ignored = false;

    if (n == 5) {
        s.has_seconds = false;
        s.seconds = cron_detail::bit(0);
        s.minutes = cron_detail::field(f[0], 0, 59, ignored, false, expr);
        s.hours = cron_detail::field(f[1], 0, 23, ignored, false, expr);
        s.days_of_month =
            cron_detail::field(f[2], 1, 31, s.dom_restricted, true, expr);
        s.months = cron_detail::field(f[3], 1, 12, ignored, false, expr);
        s.days_of_week =
            cron_detail::field(f[4], 0, 7, s.dow_restricted, true, expr);
    } else {
        s.has_seconds = true;
        s.seconds = cron_detail::field(f[0], 0, 59, ignored, false, expr);
        s.minutes = cron_detail::field(f[1], 0, 59, ignored, false, expr);
        s.hours = cron_detail::field(f[2], 0, 23, ignored, false, expr);
        s.days_of_month =
            cron_detail::field(f[3], 1, 31, s.dom_restricted, true, expr);
        s.months = cron_detail::field(f[4], 1, 12, ignored, false, expr);
        s.days_of_week =
            cron_detail::field(f[5], 0, 7, s.dow_restricted, true, expr);
    }
    return s;
}

/**
 * @brief 计算 after 之后的下一次匹配（本地时区）
 *
 * 按字段逐级跳转，避免秒级线性扫描；最多向后找 8 年。
 * 无匹配（如 2 月 30 日）返回 nullopt。
 */
export inline std::optional<std::chrono::system_clock::time_point>
next_occurrence(const cron_spec& s,
                std::chrono::system_clock::time_point after) {
    std::time_t t = std::chrono::system_clock::to_time_t(after);
    std::tm tm = cron_detail::local_tm(t);

    // 严格找 after 之后的匹配
    if (s.has_seconds) {
        tm.tm_sec += 1;
    } else {
        tm.tm_sec = 0;
        tm.tm_min += 1;
    }
    t = cron_detail::normalize(tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;

    const int start_year = cron_detail::local_tm(t).tm_year;

    for (int iter = 0; iter < 200000; ++iter) {
        tm = cron_detail::local_tm(t);
        if (tm.tm_year > start_year + 8) return std::nullopt;

        if (!((s.months >> (tm.tm_mon + 1)) & 1u)) {
            tm.tm_mon += 1;
            tm.tm_mday = 1;
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_sec = 0;
            t = cron_detail::normalize(tm);
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            continue;
        }
        if (!cron_detail::day_matches(tm, s)) {
            tm.tm_mday += 1;
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_sec = 0;
            t = cron_detail::normalize(tm);
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            continue;
        }
        if (!((s.hours >> tm.tm_hour) & 1u)) {
            tm.tm_hour += 1;
            tm.tm_min = 0;
            tm.tm_sec = 0;
            t = cron_detail::normalize(tm);
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            continue;
        }
        if (!((s.minutes >> tm.tm_min) & 1u)) {
            tm.tm_min += 1;
            tm.tm_sec = 0;
            t = cron_detail::normalize(tm);
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            continue;
        }
        if (s.has_seconds && !((s.seconds >> tm.tm_sec) & 1u)) {
            tm.tm_sec += 1;
            t = cron_detail::normalize(tm);
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            continue;
        }
        return std::chrono::system_clock::from_time_t(cron_detail::normalize(tm));
    }
    return std::nullopt;
}

// ==================== 调度 hook ====================

export namespace schedule_detail {

consteval bool is_scheduled(std::meta::info m) {
    return std::meta::is_function(m)
        && std::meta::annotations_of_with_type(m, ^^Scheduled).size() > 0;
}

template <class B>
consteval std::size_t scheduled_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked()))
        if (is_scheduled(m)) ++n;
    return n;
}

template <class B, std::size_t I>
consteval std::meta::info nth_scheduled() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked()))
        if (is_scheduled(m)) {
            if (n == I) return m;
            ++n;
        }
    throw "cboot: scheduled index out of range";
}

consteval Scheduled annotation_of(std::meta::info m) {
    for (auto a : std::meta::annotations_of_with_type(m, ^^Scheduled))
        return std::meta::extract<Scheduled>(a);
    return Scheduled{};
}

/** @brief 方法契约 + 表达式合法性（编译期） */
template <class B>
consteval void validate_bean() {
    for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked())) {
        if (!is_scheduled(m)) continue;

        const std::string name = std::string(std::meta::identifier_of(^^B))
            + "." + std::string(std::meta::identifier_of(m));

        if (std::meta::is_static_member(m))
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must be a non-static member function");
        if (!std::meta::is_public(m))
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must be public");
        if (!std::meta::parameters_of(m).empty())
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must take no parameters");

        const auto anno = annotation_of(m);
        const bool has_delay = anno.fixed_delay_ms > 0;
        const bool has_expr = !anno.expression.empty();
        if (has_delay == has_expr)
            throw std::define_static_string(
                "cboot: scheduled '" + name
                + "' requires exactly one of fixed_delay_ms or expression");
        if (has_expr) (void)parse_cron(anno.expression.view());
    }
}

template <class Ctx, std::size_t... I>
consteval void validate_all_impl(std::index_sequence<I...>) {
    (validate_bean<typename Ctx::template bean_type<I>>(), ...);
}

template <class Ctx>
consteval void validate_all() {
    validate_all_impl<Ctx>(std::make_index_sequence<Ctx::bean_count>{});
}

struct job {
    std::function<void()> fn;
    std::string name;
    bool cron{};
    bool disabled{};
    std::chrono::milliseconds delay{};
    cron_spec spec{};
    std::chrono::steady_clock::time_point deadline{};
};

inline void report(std::string_view name, std::string_view what) {
    std::cerr << "[cboot] scheduled task " << name << " failed: " << what << '\n';
}

inline void schedule_cron(job& j) {
    const auto now = std::chrono::system_clock::now();
    auto next = next_occurrence(j.spec, now);
    if (!next) {
        j.disabled = true;
        report(j.name, "cron expression has no next occurrence");
        return;
    }
    auto delta = *next - now;
    if (delta < std::chrono::system_clock::duration::zero())
        delta = std::chrono::system_clock::duration::zero();
    j.deadline = std::chrono::steady_clock::now() + delta;
}

} // namespace schedule_detail

/**
 * @brief 组件 hook：扫描 bean 的 Scheduled 方法并启动单线程调度器
 *
 * instance 在 context 构造完成后创建；析构时停止并 join 调度线程。
 */
export struct schedule_hook {
    template <class Ctx>
    struct instance {
        explicit instance(Ctx& ctx) {
            (void)schedule_detail::validate_all<Ctx>();
            collect<0>(ctx);
            if (!jobs_.empty()) {
                for (auto& j : jobs_)
                    if (j.cron) schedule_detail::schedule_cron(j);
                    else j.deadline = std::chrono::steady_clock::now() + j.delay;
                thread_ = std::jthread(
                    [this](std::stop_token st) { loop(st); });
            }
        }

        ~instance() {
            thread_.request_stop();
            cv_.notify_all();
            if (thread_.joinable()) thread_.join();
        }

        instance(const instance&) = delete;
        instance& operator=(const instance&) = delete;

    private:
        template <std::size_t I, std::size_t J>
        void add_job(Ctx& ctx) {
            using B = typename Ctx::template bean_type<I>;
            constexpr auto m = schedule_detail::nth_scheduled<B, J>();
            constexpr auto anno = schedule_detail::annotation_of(m);

            schedule_detail::job j;
            j.fn = [&ctx] { ctx.template get_at<I>().[:m:](); };
            j.name = std::string(std::meta::identifier_of(^^B)) + "."
                   + std::string(std::meta::identifier_of(m));
            // if constexpr：固定延迟任务不能实例化表达式解析
            if constexpr (anno.fixed_delay_ms > 0) {
                j.cron = false;
                j.delay = std::chrono::milliseconds(anno.fixed_delay_ms);
            } else {
                j.cron = true;
                j.spec = parse_cron(anno.expression.view());
            }
            jobs_.push_back(std::move(j));
        }

        template <std::size_t I = 0>
        void collect(Ctx& ctx) {
            if constexpr (I < Ctx::bean_count) {
                if (ctx.template is_active<I>()) {
                    using B = typename Ctx::template bean_type<I>;
                    constexpr std::size_t N =
                        schedule_detail::scheduled_count<B>();
                    [&]<std::size_t... J>(std::index_sequence<J...>) {
                        (add_job<I, J>(ctx), ...);
                    }(std::make_index_sequence<N>{});
                }
                collect<I + 1>(ctx);
            }
        }

        void loop(std::stop_token st) {
            std::unique_lock lock(mu_);
            while (!st.stop_requested()) {
                schedule_detail::job* next = nullptr;
                for (auto& j : jobs_)
                    if (!j.disabled && (!next || j.deadline < next->deadline))
                        next = &j;
                if (!next) return;

                if (cv_.wait_until(lock, st, next->deadline,
                        [&] { return st.stop_requested(); }))
                    break;
                if (st.stop_requested()) break;

                const auto now = std::chrono::steady_clock::now();
                std::vector<std::size_t> due;
                for (std::size_t i = 0; i < jobs_.size(); ++i)
                    if (!jobs_[i].disabled && jobs_[i].deadline <= now)
                        due.push_back(i);

                lock.unlock();
                for (std::size_t i : due) {
                    try {
                        jobs_[i].fn();
                    } catch (const std::exception& ex) {
                        schedule_detail::report(jobs_[i].name, ex.what());
                    } catch (...) {
                        schedule_detail::report(jobs_[i].name, "unknown exception");
                    }
                }
                const auto after = std::chrono::steady_clock::now();
                lock.lock();
                for (std::size_t i : due) {
                    if (jobs_[i].cron) {
                        schedule_detail::schedule_cron(jobs_[i]);
                    } else {
                        jobs_[i].deadline = after + jobs_[i].delay;
                    }
                }
            }
        }

        std::vector<schedule_detail::job> jobs_;
        std::mutex mu_;
        std::condition_variable_any cv_;
        std::jthread thread_;
    };
};

/**
 * @brief 契约组件描述符：无 bean，仅贡献调度 hook
 *
 * 应用选择：
 *   using components = type_list<schedule_component>;
 */
export struct schedule_component {
    static constexpr bean_name name{"schedule"};
    using beans = type_list<>;
    using dependencies = type_list<>;
    using hooks = type_list<schedule_hook>;
};
