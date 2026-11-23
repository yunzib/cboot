/**
 * @file orm.meta.cppm
 * @brief ORM 实体元数据与方言无关 SQL 生成
 *
 * 编译期读取实体注解/成员类型，生成表名、列名和 CRUD SQL；
 * SQL 的标识符引用、占位符和类型名全部来自 orm::dialect，
 * 因此同一份实体元数据可以服务 PostgreSQL / MySQL 等驱动。
 *
 * 运行期负责参数转换和 ResultSet → 实体映射。
 */
export module cboot.orm.meta;

import std;
import cboot.core.annotations;  // bean_name（annotation 值类型）
import cboot.meta.reflect;      // type_list / 成员反射 / enum 注册表
import cboot.orm.annotations;   // Table / Column / Id / error
import cboot.orm.driver;        // db_value / statement / dialect

/**
 * @brief 实体元数据、SQL 生成、行映射与参数转换
 *
 * 编译期：表名/列名/主键/自增/类型校验
 * 运行期：值转换、ResultSet → 实体
 */

export namespace orm {

/**
 * @brief 第 I 个实体成员的声明类型（已剥 cv/ref）
 *
 * 为什么单独定义在这里：
 *   - read_member / SQL 类型映射都要用它；
 *   - nth_member<T,I>() 是 consteval，type_of_t 是 GCC16 splice 的桥接别名；
 *   - 必须在 detail 的行映射 helper 之前可见。
 */
template <class T, std::size_t I>
using member_type_t = std::remove_cvref_t<
    type_of_t<std::meta::type_of(nth_member<T, I>())>>;

namespace detail {

template <class T>
struct is_optional : std::false_type {};
template <class U>
struct is_optional<std::optional<U>> : std::true_type {};
template <class T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template <class T>
struct optional_value {
    using type = T;
};
template <class U>
struct optional_value<std::optional<U>> {
    using type = U;
};
template <class T>
using optional_value_t = typename optional_value<T>::type;

consteval bean_name make_name(std::string_view s) {
    bean_name out{};
    if (s.size() >= sizeof(out.data))
        throw std::define_static_string("cboot.orm: identifier exceeds 63 chars");
    for (std::size_t i = 0; i < s.size(); ++i) out.data[i] = s[i];
    return out;
}

template <class M>
inline constexpr bool supported_scalar_v =
    std::is_same_v<M, bool>
    || std::is_integral_v<M>
    || std::is_floating_point_v<M>
    || std::is_same_v<M, std::string>
    || std::is_enum_v<M>;

template <class M>
inline constexpr bool supported_member_v =
    supported_scalar_v<M>
    || (is_optional_v<M> && supported_scalar_v<optional_value_t<M>>);

/**
 * @brief 实体元数据存储
 *
 * 用 bean_name 固定数组保存表名/列名，避免 string_view 指向临时
 * annotation 提取值；entity_meta_v<T> 的存储生命周期与程序一致。
 */
template <std::size_t N>
struct meta_data {
    bean_name table{};
    bean_name schema{};
    std::array<bean_name, N> columns{};
    std::size_t id_index = N;
    bool id_generated = false;
};

template <class T, std::size_t I>
consteval void fill_column(meta_data<member_count<T>()>& m) {
    constexpr std::meta::info member = nth_member<T, I>();
    using Raw = std::remove_cvref_t<type_of_t<std::meta::type_of(member)>>;
    static_assert(supported_member_v<Raw>,
                  "cboot.orm: unsupported entity member type");

    if (auto c = annotation_of<Column>(member))
        m.columns[I] = c->name;
    else
        m.columns[I] = make_name(nth_member_name<T, I>());
    if (m.columns[I].empty())
        throw std::define_static_string(
            "cboot.orm: entity column name must not be empty");

    if (auto id = annotation_of<Id>(member)) {
        if (m.id_index != member_count<T>())
            throw std::define_static_string(
                "cboot.orm: entity has multiple [[=Id{}]] members");
        // 自增主键必须在编译期确认是整数：这里 Raw 是模板常量，
        // 不能用 m.id_index 再回头做模板实参。
        if (id->generated && !std::is_integral_v<Raw>)
            throw std::define_static_string(
                "cboot.orm: generated [[=Id{}]] must be an integer member");
        m.id_index = I;
        m.id_generated = id->generated;
    }
}

template <class T, std::size_t... I>
consteval meta_data<sizeof...(I)> make_meta_impl(std::index_sequence<I...>) {
    meta_data<sizeof...(I)> m{};

    if (auto t = annotation_of<Table>(^^T)) {
        m.table = t->name;
        m.schema = t->schema;
    }
    if (m.table.empty())
        m.table = make_name(std::meta::identifier_of(^^T));

    (fill_column<T, I>(m), ...);

    if (m.id_index == sizeof...(I))
        throw std::define_static_string(
            "cboot.orm: entity requires exactly one [[=Id{}]] member");

    for (std::size_t i = 0; i < sizeof...(I); ++i)
        for (std::size_t j = i + 1; j < sizeof...(I); ++j)
            if (m.columns[i].view() == m.columns[j].view())
                throw std::define_static_string(
                    "cboot.orm: entity has duplicate column name '"
                    + std::string(m.columns[i].view()) + "'");

    return m;
}

// ── 值转换 ─────────────────────────────────────────────────────────────

template <class T>
inline db_value to_db_value(const T& v) {
    if constexpr (is_optional_v<T>) {
        if (!v) return std::nullptr_t{};
        return to_db_value(*v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return db_value{v};
    } else if constexpr (std::is_integral_v<T>) {
        return db_value{std::int64_t(v)};
    } else if constexpr (std::is_floating_point_v<T>) {
        return db_value{double(v)};
    } else if constexpr (std::is_same_v<T, std::string>) {
        return db_value{v};
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return db_value{std::string(std::string_view(v))};
    } else if constexpr (std::is_enum_v<T>) {
        return db_value{std::string(enum_to_name(v))};
    } else {
        static_assert(sizeof(T) == 0, "cboot.orm: unsupported bind type");
    }
}

// ── 行映射 ─────────────────────────────────────────────────────────────

template <class M>
inline void read_scalar(statement& st, std::size_t col, M& out) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>) {
        auto s = st.get_text(col);
        out = (s == "t" || s == "true" || s == "1");
    } else if constexpr (std::is_integral_v<U>) {
        auto s = st.get_text(col);
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec != std::errc{} || p != s.data() + s.size())
            throw error("cboot.orm: invalid integer column");
    } else if constexpr (std::is_floating_point_v<U>) {
        auto s = st.get_text(col);
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec != std::errc{} || p != s.data() + s.size())
            throw error("cboot.orm: invalid floating column");
    } else if constexpr (std::is_same_v<U, std::string>) {
        out.assign(st.get_text(col));
    } else if constexpr (std::is_enum_v<U>) {
        auto name = st.get_text(col);
        auto v = enum_from_name<U>(name);
        if (!v)
            throw error("cboot.orm: unknown enum value '" + std::string(name) + "'");
        out = *v;
    } else {
        static_assert(sizeof(U) == 0, "cboot.orm: unsupported column type");
    }
}

template <class T, std::size_t I>
inline void read_member_at(statement& st, std::size_t col, T& out) {
    using M = member_type_t<T, I>;
    auto& member = out.[:nth_member<T, I>():];
    if constexpr (is_optional_v<M>) {
        if (st.is_null(col)) member.reset();
        else {
            typename M::value_type v{};
            read_scalar(st, col, v);
            member = std::move(v);
        }
    } else {
        if (st.is_null(col))
            throw error("cboot.orm: unexpected NULL column");
        read_scalar(st, col, member);
    }
}

template <class T, std::size_t I>
inline void read_member(statement& st, T& out) {
    read_member_at<T, I>(st, I, out);
}

} // namespace detail

// ── 公开元数据（编译期）──────────────────────────────────────────────

/**
 * @brief 实体元数据（编译期）
 *
 * 首次实例化时校验：恰好一个 Id、列名不重复、类型受支持、自增主键为整数。
 * 校验失败通过 consteval throw 报编译错。
 */
template <class T>
inline constexpr auto entity_meta_v =
    detail::make_meta_impl<T>(std::make_index_sequence<member_count<T>()>{});

template <class T>
using id_type = std::remove_cvref_t<type_of_t<std::meta::type_of(
    nth_member<T, entity_meta_v<T>.id_index>())>>;

/** @brief 成员类型 → 方言无关的逻辑 SQL 类型 */
template <class M>
inline constexpr sql_type sql_type_of() {
    using U = detail::optional_value_t<M>;
    if constexpr (std::is_same_v<U, bool>) return sql_type::boolean;
    else if constexpr (std::is_integral_v<U> && sizeof(U) <= 4)
        return sql_type::integer;
    else if constexpr (std::is_integral_v<U>) return sql_type::big_integer;
    else if constexpr (std::is_floating_point_v<U>) return sql_type::real;
    else if constexpr (std::is_same_v<U, std::string>) return sql_type::text;
    else if constexpr (std::is_enum_v<U>) return sql_type::text;
    else static_assert(sizeof(U) == 0, "cboot.orm: unsupported SQL type");
}

// ── SQL builders（全部接收 dialect）───────────────────────────────────

namespace detail {

template <class T>
inline std::string table_sql(const dialect& d) {
    const auto& m = entity_meta_v<T>;
    if (m.schema.empty()) return d.quote_identifier(m.table.view());
    return d.quote_identifier(m.schema.view()) + "."
        + d.quote_identifier(m.table.view());
}

template <class T>
inline std::string columns_sql(const dialect& d) {
    const auto& m = entity_meta_v<T>;
    std::string out;
    for (std::size_t i = 0; i < m.columns.size(); ++i) {
        if (i) out.push_back(',');
        out += d.quote_identifier(m.columns[i].view());
    }
    return out;
}

template <class T, std::size_t I>
inline std::string column_def_sql(const dialect& d) {
    using M = member_type_t<T, I>;
    const auto& m = entity_meta_v<T>;
    const bool generated_id = (I == m.id_index && m.id_generated);
    const auto type = generated_id
        ? d.identity_type(sql_type_of<M>())
        : d.type_name(sql_type_of<M>());

    std::string out = d.quote_identifier(m.columns[I].view()) + " " + type;
    // 方言的 identity_type 已包含自增语法；PRIMARY KEY 统一放在类型后。
    if (I == m.id_index) out += " PRIMARY KEY";
    if constexpr (!detail::is_optional_v<M>) {
        if (!generated_id) out += " NOT NULL";
    }
    return out;
}

template <class T, std::size_t... I>
inline std::string create_table_impl(const dialect& d,
                                     std::index_sequence<I...>) {
    std::string cols;
    ((cols += (I ? ", " : ""), cols += column_def_sql<T, I>(d)), ...);
    return "CREATE TABLE IF NOT EXISTS " + table_sql<T>(d)
        + " (" + cols + ")";
}

} // namespace detail

template <class T>
inline T read_row(statement& st) {
    T out{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (detail::read_member<T, I>(st, out), ...);
    }(std::make_index_sequence<member_count<T>()>{});
    return out;
}

/**
 * @brief 按列名映射结果集 → T
 *
 * 第一行到达后读取结果列名，按实体 Column 元数据建立映射。
 * 适合 join/alias 后的原生 SQL；缺列直接抛 orm::error。
 * 空结果集直接返回空 vector（不检查列名）。
 */
template <class T>
inline std::vector<T> fetch_rows_named(statement& st) {
    std::vector<T> out;
    if (!st.next()) return out;

    const std::size_t columns = st.column_count();
    std::vector<std::string_view> names;
    names.reserve(columns);
    for (std::size_t i = 0; i < columns; ++i)
        names.push_back(st.column_name(i));

    std::array<std::size_t, member_count<T>()> mapping{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            const auto want = entity_meta_v<T>.columns[I].view();
            for (std::size_t j = 0; j < names.size(); ++j) {
                if (names[j] == want) {
                    mapping[I] = j;
                    return;
                }
            }
            throw error("cboot.orm: query missing column '"
                        + std::string(want) + "'");
        }(), ...);
    }(std::make_index_sequence<member_count<T>()>{});

    do {
        T row{};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (detail::read_member_at<T, I>(st, mapping[I], row), ...);
        }(std::make_index_sequence<member_count<T>()>{});
        out.push_back(std::move(row));
    } while (st.next());

    return out;
}

template <class T>
inline std::vector<db_value> insert_values(const T& entity) {
    std::vector<db_value> out;
    out.reserve(member_count<T>());
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            const auto& meta = entity_meta_v<T>;
            if (meta.id_generated && I == meta.id_index) return;
            out.push_back(detail::to_db_value(entity.[:nth_member<T, I>():]));
        }(), ...);
    }(std::make_index_sequence<member_count<T>()>{});
    return out;
}

template <class T>
inline std::vector<db_value> update_values(const T& entity) {
    std::vector<db_value> out;
    out.reserve(member_count<T>());
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
            if (I != entity_meta_v<T>.id_index)
                out.push_back(detail::to_db_value(entity.[:nth_member<T, I>():]));
        }(), ...);
    }(std::make_index_sequence<member_count<T>()>{});
    out.push_back(detail::to_db_value(
        entity.[:nth_member<T, entity_meta_v<T>.id_index>():]));
    return out;
}

/** @brief SELECT 全部列；同一实体在不同方言下 SQL 不同 */
template <class T>
inline std::string select_all_sql(const dialect& d) {
    return "SELECT " + detail::columns_sql<T>(d)
        + " FROM " + detail::table_sql<T>(d);
}

/** @brief 按主键 SELECT；占位符由 dialect 决定 */
template <class T>
inline std::string select_by_id_sql(const dialect& d) {
    return select_all_sql<T>(d) + " WHERE "
        + d.quote_identifier(
            entity_meta_v<T>.columns[entity_meta_v<T>.id_index].view())
        + " = " + d.placeholder(1);
}

/**
 * @brief INSERT SQL
 *
 * 自增主键：
 *   - 支持 RETURNING 的方言（PostgreSQL）追加 RETURNING id；
 *   - 不支持的方言（MySQL）由 EntityManager 在 execute 后读 last_insert_id。
 */
template <class T>
inline std::string insert_sql(const dialect& d) {
    const auto& meta = entity_meta_v<T>;
    std::string cols;
    std::string ph;
    std::size_t n = 0;
    for (std::size_t i = 0; i < meta.columns.size(); ++i) {
        if (meta.id_generated && i == meta.id_index) continue;
        if (n++) { cols += ","; ph += ","; }
        cols += d.quote_identifier(meta.columns[i].view());
        ph += d.placeholder(n);
    }

    std::string out;
    if (n == 0) {
        out = d.insert_default_values(detail::table_sql<T>(d));
    } else {
        out = "INSERT INTO " + detail::table_sql<T>(d)
            + " (" + cols + ") VALUES (" + ph + ")";
    }
    if (meta.id_generated && d.supports_returning())
        out += " RETURNING "
            + d.quote_identifier(meta.columns[meta.id_index].view());
    return out;
}

/** @brief 按主键 UPDATE；SET 参数在前、WHERE 参数在后 */
template <class T>
inline std::string update_sql(const dialect& d) {
    const auto& meta = entity_meta_v<T>;
    std::string set;
    std::size_t n = 0;
    for (std::size_t i = 0; i < meta.columns.size(); ++i) {
        if (i == meta.id_index) continue;
        if (n++) set += ", ";
        set += d.quote_identifier(meta.columns[i].view())
            + " = " + d.placeholder(n);
    }
    if (set.empty())
        set = d.quote_identifier(meta.columns[meta.id_index].view())
            + " = " + d.quote_identifier(meta.columns[meta.id_index].view());
    return "UPDATE " + detail::table_sql<T>(d) + " SET " + set
        + " WHERE " + d.quote_identifier(meta.columns[meta.id_index].view())
        + " = " + d.placeholder(n + 1);
}

/** @brief 按主键 DELETE */
template <class T>
inline std::string delete_sql(const dialect& d) {
    return "DELETE FROM " + detail::table_sql<T>(d) + " WHERE "
        + d.quote_identifier(
            entity_meta_v<T>.columns[entity_meta_v<T>.id_index].view())
        + " = " + d.placeholder(1);
}

/** @brief 按实体元数据生成建表 SQL */
template <class T>
inline std::string create_table_sql(const dialect& d) {
    return detail::create_table_impl<T>(
        d, std::make_index_sequence<member_count<T>()>{});
}

/** @brief 按实体元数据生成删表 SQL */
template <class T>
inline std::string drop_table_sql(const dialect& d) {
    return "DROP TABLE IF EXISTS " + detail::table_sql<T>(d);
}

} // namespace orm
