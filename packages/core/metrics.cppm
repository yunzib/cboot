/**
 * @file metrics.cppm
 * @brief 指标注册表 — 计数器 / 直方图 / Prometheus 文本渲染
 *
 * 微服务/k8s 前置能力的指标基座（对标 prometheus/client_golang 的
 * 最小子集）：
 *   - counter：单调递增计数（原子累加）
 *   - histogram：固定桶边界 + sum/count（桶计数为原子，无需锁）
 *   - registry：进程级单例（metrics::global()），系列只增不删，
 *     指针稳定——记录端可在首次查表后缓存系列指针，热路径零查表
 *
 * 边界（刻意不做）：
 *   - 不做 summary/quantile、exemplar、Pushgateway
 *   - 不做标签去重/校验（调用方保证标签名合法：[a-zA-Z_][a-zA-Z0-9_]*）
 *   - 系列 key 用 '\x1f' 拼接 name + labels；注册表查表持 shared_mutex，
 *     但计数/观测本身无锁
 *
 * 线程安全口径：get_counter/get_histogram 可并发调用（查表加锁，
 * 返回的系列对象终身有效）；inc/observe 无锁可并发。
 */
export module cboot.core.metrics;

import std;

export namespace metrics {

/** @brief 组装 Prometheus 标签对：label("route", "/x") → route="/x" */
inline std::string label(std::string_view name, std::string_view value) {
    std::string out;
    out.reserve(name.size() + value.size() + 4);
    out.append(name);
    out += '=';
    out += '"';
    // 值中的反斜杠/引号按 Prometheus 文本格式转义
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/** @brief 单调计数器（原子；系列由 registry 持有，终身有效） */
class counter {
public:
    void inc(std::uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

/**
 * @brief 瞬时值（gauge；原子 double）
 *
 * 适合"当前值"语义（池占用、队列长度）；x64 上 lock-free。
 */
class gauge {
public:
    void set(double v) noexcept { value_.store(v, std::memory_order_relaxed); }
    double value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

/**
 * @brief 固定桶直方图（毫秒口径；桶边界编译期固定）
 *
 * 桶计数为 Prometheus 累积口径（≤ le），最后一个桶是 +Inf，
 * 即 +Inf 桶 == 总 count；sum 用纳秒整数累加避免浮点原子，
 * 渲染时换算回毫秒。
 */
class histogram {
public:
    /** @brief 桶上界（毫秒）；最后一个元素视为 +Inf */
    static constexpr std::array<double, 13> default_buckets_ms{
        1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};

    /** @brief 记录一次观测（毫秒；桶为累积口径：每个 le ≥ 值的桶都 +1） */
    void observe(double ms) noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_ns_.fetch_add(static_cast<std::uint64_t>(ms * 1e6),
                          std::memory_order_relaxed);
        // Prometheus bucket 是累积计数：值落入的桶及其上方全部桶都 +1
        for (std::size_t i = 0; i < default_buckets_ms.size(); ++i) {
            if (ms <= default_buckets_ms[i])
                buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
        buckets_[default_buckets_ms.size()].fetch_add(1,
                                                      std::memory_order_relaxed);
    }

    std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    double sum_ms() const noexcept {
        return static_cast<double>(sum_ns_.load(std::memory_order_relaxed)) / 1e6;
    }
    std::uint64_t bucket(std::size_t i) const noexcept {
        return buckets_[i].load(std::memory_order_relaxed);
    }

private:
    // +Inf 桶额外一个：default_buckets.size() + 1
    std::array<std::atomic<std::uint64_t>, default_buckets_ms.size() + 1> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_ns_{0};
};

/**
 * @brief 指标注册表 — 系列（counter/histogram）的唯一归属
 *
 * 系列 key：name + '\x1f' + 各 label 串（含序）以 '\x1f' 拼接。
 * get_* 首次调用创建系列；同 key 重复调用返回同一对象。
 * 渲染按名称排序输出，保证快照可 diff。
 */
class registry {
public:
    /** @brief 取/建计数器系列；labels 为已组装的 label("k","v") 串 */
    counter& get_counter(std::string_view name,
                         std::span<const std::string> labels = {}) {
        std::string key = series_key(name, labels);
        std::shared_lock lock(mu_);
        if (auto it = counters_.find(key); it != counters_.end()) return *it->second;
        lock.unlock();
        std::unique_lock wlock(mu_);
        // 双检：并发路径下可能已被他人创建
        if (auto it = counters_.find(key); it != counters_.end()) return *it->second;
        auto [it, _] = counters_.emplace(std::move(key),
                                         std::make_unique<counter>());
        return *it->second;
    }

    /** @brief 便捷重载：花括号列表（临时串；查表即拷入 key，不保留引用） */
    counter& get_counter(std::string_view name,
                         std::initializer_list<std::string> labels) {
        const std::vector<std::string> tmp(labels);
        return get_counter(name, std::span<const std::string>(tmp));
    }

    /** @brief 取/建直方图系列（桶边界固定，见 histogram） */
    histogram& get_histogram(std::string_view name,
                             std::span<const std::string> labels = {}) {
        std::string key = series_key(name, labels);
        std::shared_lock lock(mu_);
        if (auto it = hists_.find(key); it != hists_.end()) return *it->second;
        lock.unlock();
        std::unique_lock wlock(mu_);
        if (auto it = hists_.find(key); it != hists_.end()) return *it->second;
        auto [it, _] = hists_.emplace(std::move(key),
                                       std::make_unique<histogram>());
        return *it->second;
    }

    /** @brief 便捷重载：花括号列表（临时串；语义同 span 版） */
    histogram& get_histogram(std::string_view name,
                             std::initializer_list<std::string> labels) {
        const std::vector<std::string> tmp(labels);
        return get_histogram(name, std::span<const std::string>(tmp));
    }

    /** @brief 取/建 gauge 系列（当前值语义） */
    gauge& get_gauge(std::string_view name,
                     std::span<const std::string> labels = {}) {
        std::string key = series_key(name, labels);
        std::shared_lock lock(mu_);
        if (auto it = gauges_.find(key); it != gauges_.end()) return *it->second;
        lock.unlock();
        std::unique_lock wlock(mu_);
        if (auto it = gauges_.find(key); it != gauges_.end()) return *it->second;
        auto [it, _] = gauges_.emplace(std::move(key), std::make_unique<gauge>());
        return *it->second;
    }

    /** @brief 便捷重载：花括号列表 */
    gauge& get_gauge(std::string_view name,
                     std::initializer_list<std::string> labels) {
        const std::vector<std::string> tmp(labels);
        return get_gauge(name, std::span<const std::string>(tmp));
    }

    /** @brief 渲染 Prometheus 文本格式（0.0.4；名称排序，系列只读遍历） */
    std::string render_prometheus() const {
        // 序列化时复制 key 列表（读锁内），避免渲染期间长期持锁
        std::vector<std::pair<std::string, const counter*>> cs;
        std::vector<std::pair<std::string, const gauge*>> gs;
        std::vector<std::pair<std::string, const histogram*>> hs;
        {
            std::shared_lock lock(mu_);
            cs.reserve(counters_.size());
            for (const auto& [k, c] : counters_) cs.emplace_back(k, c.get());
            gs.reserve(gauges_.size());
            for (const auto& [k, g] : gauges_) gs.emplace_back(k, g.get());
            hs.reserve(hists_.size());
            for (const auto& [k, h] : hists_) hs.emplace_back(k, h.get());
        }
        std::ranges::sort(cs);
        std::ranges::sort(gs);
        std::ranges::sort(hs);

        std::string out;
        out.reserve(512 * (cs.size() + hs.size() + gs.size() + 1));

        // TYPE 行按指标名分组输出一次（Prometheus 文本格式要求；
        // 系列按 key 排序，同名系列必然相邻）
        std::string last_name;
        auto type_line = [&](std::string_view name, std::string_view kind) {
            if (name == last_name) return;
            last_name.assign(name);
            out += "# TYPE ";
            out += name;
            out += ' ';
            out += kind;
            out += '\n';
        };

        for (const auto& [key, c] : cs) {
            const auto [name, labels] = split_key(key);
            type_line(name, "counter");
            out += name;
            out += labels;
            out += ' ';
            out += std::to_string(c->value());
            out += '\n';
        }

        last_name.clear();
        for (const auto& [key, g] : gs) {
            const auto [name, labels] = split_key(key);
            type_line(name, "gauge");
            out += name;
            out += labels;
            out += ' ';
            out += trim_number(g->value());
            out += '\n';
        }

        last_name.clear();
        for (const auto& [key, h] : hs) {
            const auto [name, labels] = split_key(key);
            type_line(name, "histogram");
            constexpr auto& bounds = histogram::default_buckets_ms;
            for (std::size_t i = 0; i <= bounds.size(); ++i) {
                out += name;
                out += "_bucket";
                // le 追加到既有标签尾部（空标签时直接开大括号）
                if (labels.empty()) {
                    out += "{le=\"";
                } else {
                    out += labels.substr(0, labels.size() - 1);   // 去掉收尾 '}'
                    out += ",le=\"";
                }
                if (i < bounds.size())
                    out += trim_number(bounds[i]);
                else
                    out += "+Inf";
                out += "\"} ";
                out += std::to_string(h->bucket(i));
                out += '\n';
            }
            out += name;
            out += "_sum";
            out += labels;
            out += ' ';
            out += trim_number(h->sum_ms());
            out += '\n';
            out += name;
            out += "_count";
            out += labels;
            out += ' ';
            out += std::to_string(h->count());
            out += '\n';
        }
        return out;
    }

    /** @brief 清空全部系列（仅测试用；运行期不要调用） */
    void clear() {
        std::unique_lock lock(mu_);
        counters_.clear();
        gauges_.clear();
        hists_.clear();
    }

    std::size_t size() const {
        std::shared_lock lock(mu_);
        return counters_.size() + gauges_.size() + hists_.size();
    }

private:
    static std::string series_key(std::string_view name,
                                   std::span<const std::string> labels) {
        std::string key;
        key.reserve(name.size() + 8);
        key.append(name);
        for (const auto& l : labels) {
            key += '\x1f';
            key += l;
        }
        return key;
    }

    /** @brief key → (name, "{l1,l2,...}" 渲染形态) */
    static std::pair<std::string, std::string> split_key(const std::string& key) {
        const std::size_t sep = key.find('\x1f');
        if (sep == std::string::npos) return {key, {}};
        std::string labels = "{";
        bool first = true;
        for (std::size_t i = sep + 1; i < key.size();) {
            const std::size_t next = key.find('\x1f', i);
            const std::size_t end = next == std::string::npos ? key.size() : next;
            if (!first) labels += ',';
            first = false;
            labels.append(key, i, end - i);
            i = end + 1;
        }
        labels += '}';
        return {key.substr(0, sep), std::move(labels)};
    }

    /** @brief 数值最短形态（去掉无意义的尾零；Prometheus 接受整数） */
    static std::string trim_number(double v) {
        std::string s = std::format("{}", v);
        return s;
    }

    mutable std::shared_mutex mu_;
    std::map<std::string, std::unique_ptr<counter>> counters_;
    std::map<std::string, std::unique_ptr<gauge>> gauges_;
    std::map<std::string, std::unique_ptr<histogram>> hists_;
};

/**
 * @brief 进程级全局注册表（与 client_golang 的默认 registry 对应）
 *
 * 定义在实现单元 src/metrics.cpp：inline 函数局部静态在模块跨 TU
 * 场景下可能被各自实例化（Release 实测指标分裂），故这里只声明。
 */
registry& global();

} // namespace metrics
