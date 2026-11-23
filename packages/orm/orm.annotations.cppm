/**
 * @file orm.annotations.cppm
 * @brief ORM 实体注解
 *
 * 只放类型注解：不依赖驱动、SQL、DSL。
 * 实体 bean 的注解值必须 structural，字符串统一用 bean_name。
 */
export module cboot.orm.annotations;

import std;
import cboot.core.annotations;   // bean_name

/**
 * @brief ORM 实体注解
 *
 * 独立模块：只依赖 core 注解词汇，不依赖驱动/SQL。
 */

export namespace orm {

// ── 实体注解 ──────────────────────────────────────────────────────────

/** @brief 表名与可选 schema；schema 为空时使用当前 search_path */
struct Table {
    bean_name name{};
    bean_name schema{};
};

/** @brief 列名覆盖；省略时使用成员名 */
struct Column {
    bean_name name{};
};

/** @brief 主键注解；generated=true 表示数据库自增 */
struct Id {
    bool generated{};
};

/** @brief ORM 运行期错误（连接/查询/映射失败） */
class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace orm
