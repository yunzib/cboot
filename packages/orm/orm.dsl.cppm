/**
 * @file orm.dsl.cppm
 * @brief ORM 类型安全 DSL
 *
 * 提供单表 select / where / order_by / limit / offset，
 * 以及 update set / delete where 的类型安全构造。
 *
 * 条件与排序保存为轻量 AST，执行时才按 dialect 渲染标识符和占位符；
 * 这样同一 DSL 可同时服务 PostgreSQL（$n / 双引号）和 MySQL（? / 反引号）。
 *
 * 不提供 join、聚合、CTE、窗口函数；这些走原生 SQL 逃生口。
 * builder 通过 duck-typed Db 调用 dialect()/fetch_rows/execute，
 * 不依赖 EntityManager。
 */
export module cboot.orm.dsl;

import std;
import cboot.meta.reflect;      // member_count / nth_member / type_of_t
import cboot.orm.annotations;   // error
import cboot.orm.driver;        // db_value / dialect
import cboot.orm.meta;          // entity_meta_v / SQL builders / 值转换

export namespace orm {

/**
 * @brief 编译期列表达式
 *
 * 保存成员下标和列名：
 *   - index 用于按 SELECT 列顺序读取行；
 *   - name 指向 entity_meta_v<T> 的静态存储，生命周期与程序一致；
 *   - value_type 保留成员声明类型（可能是 optional），绑定时再剥 optional。
 */
template <class T, class V>
struct column_expr {
    std::size_t index{};
    std::string_view name{};
    using table_type = T;
    using value_type = V;
};

/**
 * @brief 列引用：col<^^User::name>()
 *
 * 使用成员反射而不是字符串：
 *   - 成员不存在时直接编译错；
 *   - 列名由 Column 注解或成员名决定，避免字符串漂移；
 *   - 下标编译期算出，保证 SELECT 列顺序和行映射一致。
 *
 * GCC16 已支持 ^^T::member；type_of_t 是 splice 的桥接别名。
 */
template <std::meta::info M>
    requires (std::meta::is_nonstatic_data_member(M))
constexpr auto col() {
    using T = type_of_t<std::meta::parent_of(M)>;
    using V = type_of_t<std::meta::type_of(M)>;

    // 用 index_sequence 展开成员，避免在 consteval 中持有 vector<info>。
    constexpr std::size_t I = [] consteval {
        constexpr std::size_t N = member_count<T>();
        std::size_t found = N;
        [&]<std::size_t... K>(std::index_sequence<K...>) {
            ((nth_member<T, K>() == M ? (found = K, 0) : 0), ...);
        }(std::make_index_sequence<N>{});
        if (found == N)
            throw std::define_static_string(
                "cboot.orm: column is not an entity member");
        return found;
    }();

    constexpr const auto& meta = entity_meta_v<T>;
    return column_expr<T, V>{I, meta.columns[I].view()};
}

namespace detail {

template <class C, class U>
inline constexpr bool comparable_v =
    (std::is_integral_v<C> && std::is_integral_v<U>)
    || (std::is_floating_point_v<C>
        && (std::is_floating_point_v<U> || std::is_integral_v<U>))
    || (std::is_same_v<C, bool> && std::is_same_v<U, bool>)
    || (std::is_same_v<C, std::string>
        && std::is_convertible_v<U, std::string_view>)
    || (std::is_enum_v<C> && std::is_same_v<C, U>);

/** @brief 一次条件渲染的累积上下文 */
struct render_context {
    const dialect& d;
    std::string sql;
    std::vector<db_value> params;

    /** @brief 追加参数并返回对应占位符 */
    std::string add_param(db_value value) {
        params.push_back(std::move(value));
        return d.placeholder(params.size());
    }
};

struct condition_node {
    virtual ~condition_node() = default;
    virtual void render(render_context& ctx) const = 0;
};

struct literal_node final : condition_node {
    std::string text;
    explicit literal_node(std::string t) : text(std::move(t)) {}
    void render(render_context& ctx) const override { ctx.sql += text; }
};

struct compare_node final : condition_node {
    std::string column;
    std::string op;
    db_value value;

    compare_node(std::string c, std::string o, db_value v)
        : column(std::move(c)), op(std::move(o)), value(std::move(v)) {}

    void render(render_context& ctx) const override {
        ctx.sql += ctx.d.quote_identifier(column);
        ctx.sql.push_back(' ');
        ctx.sql += op;
        ctx.sql.push_back(' ');
        ctx.sql += ctx.add_param(value);
    }
};

struct null_node final : condition_node {
    std::string column;
    bool negate = false;

    null_node(std::string c, bool n) : column(std::move(c)), negate(n) {}

    void render(render_context& ctx) const override {
        ctx.sql += ctx.d.quote_identifier(column);
        ctx.sql += negate ? " IS NOT NULL" : " IS NULL";
    }
};

struct in_node final : condition_node {
    std::string column;
    std::vector<db_value> values;

    explicit in_node(std::string c) : column(std::move(c)) {}

    void render(render_context& ctx) const override {
        if (values.empty()) {
            ctx.sql += "FALSE";
            return;
        }
        ctx.sql += ctx.d.quote_identifier(column);
        ctx.sql += " IN (";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) ctx.sql.push_back(',');
            ctx.sql += ctx.add_param(values[i]);
        }
        ctx.sql.push_back(')');
    }
};

struct like_node final : condition_node {
    std::string column;
    std::string pattern;
    bool case_insensitive = false;

    like_node(std::string c, std::string p, bool ci)
        : column(std::move(c)), pattern(std::move(p)),
          case_insensitive(ci) {}

    void render(render_context& ctx) const override {
        const std::string col = ctx.d.quote_identifier(column);
        if (case_insensitive && !ctx.d.supports_ilike()) {
            // MySQL 没有 ILIKE：两边统一 LOWER，保持跨库语义一致。
            ctx.sql += "LOWER(" + col + ") LIKE LOWER("
                + ctx.add_param(pattern) + ")";
            return;
        }
        ctx.sql += col;
        ctx.sql += case_insensitive ? " ILIKE " : " LIKE ";
        ctx.sql += ctx.add_param(pattern);
    }
};

struct binary_node final : condition_node {
    std::shared_ptr<const condition_node> lhs;
    std::shared_ptr<const condition_node> rhs;
    const char* op = nullptr;

    binary_node(std::shared_ptr<const condition_node> l,
                std::shared_ptr<const condition_node> r,
                const char* o)
        : lhs(std::move(l)), rhs(std::move(r)), op(o) {}

    void render(render_context& ctx) const override {
        ctx.sql.push_back('(');
        lhs->render(ctx);
        ctx.sql += ") ";
        ctx.sql += op;
        ctx.sql += " (";
        rhs->render(ctx);
        ctx.sql.push_back(')');
    }
};

struct not_node final : condition_node {
    std::shared_ptr<const condition_node> child;

    explicit not_node(std::shared_ptr<const condition_node> c)
        : child(std::move(c)) {}

    void render(render_context& ctx) const override {
        ctx.sql += "NOT (";
        child->render(ctx);
        ctx.sql.push_back(')');
    }
};

} // namespace detail

/**
 * @brief 条件 AST + 参数
 *
 * 条件在构造时只保存列名/运算符/值，不写死 SQL 方言；
 * execute/fetch 时调用 render(dialect) 得到最终 SQL 与参数。
 *
 * 所有值都进入 params，不拼接到 SQL 文本里。
 */
struct condition {
    std::shared_ptr<const detail::condition_node> node;

    [[nodiscard]] bool empty() const { return node == nullptr; }

    /** @brief 按方言渲染 SQL 与参数 */
    detail::render_context render(const dialect& d) const {
        detail::render_context ctx{d};
        if (node) node->render(ctx);
        return ctx;
    }
};

/** @brief ORDER BY 项：保存列名和方向，执行时再引用标识符 */
struct order_term {
    std::string column;
    bool ascending = true;
};

/** @brief 升序排序项：column_expr → ORDER BY 片段 */
template <class T, class V>
inline order_term asc(const column_expr<T, V>& c) {
    return {std::string(c.name), true};
}

/** @brief 降序排序项：column_expr → ORDER BY 片段 */
template <class T, class V>
inline order_term desc(const column_expr<T, V>& c) {
    return {std::string(c.name), false};
}

/**
 * @brief 列与值比较
 *
 * optional 列按底层类型比较；SQL NULL 用 is_null / is_not_null 表达，
 * 不要把 nullopt 当普通值传进来。
 *
 * 参数：
 *   - c   列表达式
 *   - op  SQL 比较运算符
 *   - v   绑定值
 */
template <class T, class V, class U>
condition compare(const column_expr<T, V>& c, const char* op, const U& v) {
    using C = detail::optional_value_t<V>;
    static_assert(detail::comparable_v<C, std::remove_cvref_t<U>>,
                  "cboot.orm: column type and value type are not comparable");
    return {std::make_shared<detail::compare_node>(
        std::string(c.name), std::string(op), detail::to_db_value(v))};
}

/** @brief 等于：column == value */
template <class T, class V, class U>
condition operator==(const column_expr<T, V>& c, const U& v) {
    return compare(c, "=", v);
}

/** @brief 不等于：column != value */
template <class T, class V, class U>
condition operator!=(const column_expr<T, V>& c, const U& v) {
    return compare(c, "<>", v);
}

/** @brief 小于：column < value */
template <class T, class V, class U>
condition operator<(const column_expr<T, V>& c, const U& v) {
    return compare(c, "<", v);
}

/** @brief 小于等于：column <= value */
template <class T, class V, class U>
condition operator<=(const column_expr<T, V>& c, const U& v) {
    return compare(c, "<=", v);
}

/** @brief 大于：column > value */
template <class T, class V, class U>
condition operator>(const column_expr<T, V>& c, const U& v) {
    return compare(c, ">", v);
}

/** @brief 大于等于：column >= value */
template <class T, class V, class U>
condition operator>=(const column_expr<T, V>& c, const U& v) {
    return compare(c, ">=", v);
}

/** @brief SQL NULL 判断：只生成 IS NULL，不加参数 */
template <class T, class V>
condition is_null(const column_expr<T, V>& c) {
    return {std::make_shared<detail::null_node>(
        std::string(c.name), false)};
}

/** @brief SQL NOT NULL 判断：只生成 IS NOT NULL，不加参数 */
template <class T, class V>
condition is_not_null(const column_expr<T, V>& c) {
    return {std::make_shared<detail::null_node>(
        std::string(c.name), true)};
}

/**
 * @brief IN 条件：column IN (?, ?, ...)
 *
 * 空集合没有合法 SQL 语义，直接生成 FALSE，避免生成 IN ()。
 * 元素类型必须与列类型兼容；每个元素一个参数。
 */
template <class T, class V, class Range>
condition in(const column_expr<T, V>& c, const Range& values) {
    using C = detail::optional_value_t<V>;
    using E = std::remove_cvref_t<decltype(*std::begin(values))>;
    static_assert(detail::comparable_v<C, E>,
                  "cboot.orm: IN value type does not match column type");

    auto n = std::make_shared<detail::in_node>(std::string(c.name));
    for (const auto& v : values) n->values.push_back(detail::to_db_value(v));
    if (n->values.empty())
        return {std::make_shared<detail::literal_node>("FALSE")};
    return {std::move(n)};
}

/** @brief LIKE：column LIKE ?（pattern 作为参数，不拼 SQL） */
template <class T, class V>
condition like(const column_expr<T, V>& c, std::string_view pattern) {
    return {std::make_shared<detail::like_node>(
        std::string(c.name), std::string(pattern), false)};
}

/**
 * @brief 大小写不敏感 LIKE
 *
 * PostgreSQL 使用 ILIKE；没有 ILIKE 的方言（MySQL）降级为
 * LOWER(column) LIKE LOWER(?)。
 */
template <class T, class V>
condition ilike(const column_expr<T, V>& c, std::string_view pattern) {
    return {std::make_shared<detail::like_node>(
        std::string(c.name), std::string(pattern), true)};
}

/**
 * @brief 条件 AND 组合
 *
 * 两侧都加括号，避免 AND/OR 优先级问题；
 * 参数按 SQL 出现顺序在渲染时追加。
 */
inline condition operator&&(condition a, condition b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::make_shared<detail::binary_node>(
        std::move(a.node), std::move(b.node), "AND")};
}

/** @brief 条件 OR 组合 */
inline condition operator||(condition a, condition b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::make_shared<detail::binary_node>(
        std::move(a.node), std::move(b.node), "OR")};
}

/** @brief 条件取反：NOT (...) */
inline condition operator!(condition a) {
    if (a.empty()) return a;
    return {std::make_shared<detail::not_node>(std::move(a.node))};
}

/**
 * @brief 单表 SELECT builder
 *
 * 典型用法：
 *   db.select<User>()
 *       .where(col<^^User::active>() == true)
 *       .order_by(desc(col<^^User::id>()))
 *       .limit(20)
 *       .fetch();
 *
 * Db 是执行器（当前是 EntityManager），只需要提供：
 *   - const dialect& dialect() const;
 *   - template <class T> std::vector<T> fetch_rows(sql, params);
 *
 * 不直接依赖 EntityManager，避免模块循环，也方便 mock 测试。
 */
template <class T, class Db>
class select_builder {
public:
    /** @brief 绑定执行器；builder 不拥有执行器 */
    explicit select_builder(Db& db) : db_(db) {}

    /** @brief 设置 WHERE；后一次调用覆盖前一次 */
    select_builder& where(condition c) {
        where_ = std::move(c);
        return *this;
    }

    /** @brief 追加一个 ORDER BY 项 */
    select_builder& order_by(order_term t) {
        order_.push_back(std::move(t));
        return *this;
    }

    /** @brief 设置 LIMIT */
    select_builder& limit(std::size_t n) {
        limit_ = n;
        return *this;
    }

    /** @brief 设置 OFFSET */
    select_builder& offset(std::size_t n) {
        offset_ = n;
        return *this;
    }

    /**
     * @brief 执行查询并返回全部行
     *
     * SQL 顺序固定：SELECT → WHERE → ORDER BY → LIMIT → OFFSET。
     * WHERE 的占位符在渲染时按方言生成。
     */
    std::vector<T> fetch() {
        const auto& d = db_.dialect();
        std::string sql = select_all_sql<T>(d);
        std::vector<db_value> params;

        if (!where_.empty()) {
            auto rendered = where_.render(d);
            sql += " WHERE " + rendered.sql;
            params = std::move(rendered.params);
        }
        if (!order_.empty()) {
            sql += " ORDER BY ";
            for (std::size_t i = 0; i < order_.size(); ++i) {
                if (i) sql += ", ";
                sql += d.quote_identifier(order_[i].column);
                sql += order_[i].ascending ? " ASC" : " DESC";
            }
        }
        if (limit_) sql += " LIMIT " + std::to_string(*limit_);
        if (offset_) sql += " OFFSET " + std::to_string(*offset_);

        return db_.template fetch_rows<T>(sql, params);
    }

    /**
     * @brief 执行查询并返回至多一行
     *
     * 用 LIMIT 1 让数据库提前停止；没有行返回 nullopt。
     */
    std::optional<T> fetch_one() {
        limit(1);
        auto rows = fetch();
        if (rows.empty()) return std::nullopt;
        return std::move(rows.front());
    }

private:
    Db& db_;
    condition where_{};
    std::vector<order_term> order_;
    std::optional<std::size_t> limit_;
    std::optional<std::size_t> offset_;
};

/**
 * @brief 类型安全 UPDATE builder
 *
 * 安全边界：
 *   - 至少一个 set()
 *   - 必须有 where()；第一版不提供无条件全表更新
 *   - set 的列必须是实体 T 的列，值类型编译期检查
 */
template <class T, class Db>
class update_builder {
public:
    /** @brief 绑定执行器；builder 不拥有执行器 */
    explicit update_builder(Db& db) : db_(db) {}

    /**
     * @brief 追加一个 SET 赋值
     *
     * 例子：
     *   .set(col<^^User::name>(), "bob")
     *
     * 值和列类型不兼容时在编译期报错。
     */
    template <class V, class U>
    update_builder& set(const column_expr<T, V>& c, const U& value) {
        using C = detail::optional_value_t<V>;
        static_assert(detail::comparable_v<C, std::remove_cvref_t<U>>,
                      "cboot.orm: column type and value type are not comparable");
        sets_.push_back({std::string(c.name), detail::to_db_value(value)});
        return *this;
    }

    /** @brief 设置 WHERE；后一次调用覆盖前一次 */
    update_builder& where(condition c) {
        where_ = std::move(c);
        return *this;
    }

    /**
     * @brief 执行 UPDATE，返回影响行数
     *
     * SET 参数在前、WHERE 参数在后，与渲染出的占位符顺序一致。
     */
    std::size_t execute() {
        if (sets_.empty())
            throw error("cboot.orm: update requires at least one set()");
        if (where_.empty())
            throw error("cboot.orm: update requires where()");

        const auto& d = db_.dialect();
        std::string sql = "UPDATE " + detail::table_sql<T>(d) + " SET ";
        std::vector<db_value> params;
        params.reserve(sets_.size());

        for (std::size_t i = 0; i < sets_.size(); ++i) {
            if (i) sql += ", ";
            sql += d.quote_identifier(sets_[i].first);
            sql += " = ";
            params.push_back(sets_[i].second);
            sql += d.placeholder(params.size());
        }

        auto rendered = where_.render(d);
        sql += " WHERE " + rendered.sql;
        params.insert(params.end(), rendered.params.begin(),
                      rendered.params.end());
        return db_.execute(sql, params);
    }

private:
    Db& db_;
    std::vector<std::pair<std::string, db_value>> sets_;
    condition where_{};
};

/**
 * @brief 类型安全 DELETE builder
 *
 * 必须有 where()：避免误删全表。
 * 全表清理请走显式原生 SQL，不在 builder 里提供隐式入口。
 */
template <class T, class Db>
class delete_builder {
public:
    /** @brief 绑定执行器；builder 不拥有执行器 */
    explicit delete_builder(Db& db) : db_(db) {}

    /** @brief 设置 WHERE；后一次调用覆盖前一次 */
    delete_builder& where(condition c) {
        where_ = std::move(c);
        return *this;
    }

    /** @brief 执行 DELETE，返回影响行数 */
    std::size_t execute() {
        if (where_.empty())
            throw error("cboot.orm: delete requires where()");
        const auto& d = db_.dialect();
        auto rendered = where_.render(d);
        std::string sql = "DELETE FROM " + detail::table_sql<T>(d)
            + " WHERE " + rendered.sql;
        return db_.execute(sql, rendered.params);
    }

private:
    Db& db_;
    condition where_{};
};

} // namespace orm
