/**
 * @file orm.migration.cppm
 * @brief schema 迁移 — 版本化脚本与启动期自动升级（Flyway 最小子集）
 *
 * 单体长生命周期的关键设施：schema 演进有版本记录，可重复启动幂等执行。
 *
 * 模型：
 *   - migration_script：{version, description, sql}；version 严格递增
 *   - 版本表（默认 cboot_schema_version）：version / description / applied_ms
 *   - run()：建版本表（如无）→ 读当前版本 → 按序执行 version 更大的脚本；
 *     每个脚本与版本行插入在**同一事务**提交，失败回滚并抛 orm::error
 *
 * 边界（刻意不做）：
 *   - 不做 SQL 解析/语句拆分：PG 驱动支持单脚本多语句（PQexec 批量），
 *     MySQL 文本协议一脚本一语句，多语句请拆成多个脚本
 *   - 不做 undo/down 脚本（只前进，与 Flyway/liquibase 前进式策略一致）
 *   - 不做校验和/描述变更检测
 *   - 多实例并发：版本行先占位 + version UNIQUE，第二个实例插入冲突
 *     即失败退出（不会重复执行脚本）；k8s 滚动更新建议迁移阶段单副本
 *     （Job / initContainer），脚本本身仍需可重入（MySQL DDL 隐式提交）
 *
 * 用法（应用 bean 的 PostConstruct 中调用；脚本清单写在代码里）：
 *   struct [[=Bean{}]] Migrator {
 *       Migrator(orm::MigrationRunner& mig) : mig_(mig) {}
 *       [[=PostConstruct{}]] void init() {
 *           static const std::array<orm::migration_script, 2> scripts{{
 *               {1, "create users",
 *                "CREATE TABLE users (id BIGINT GENERATED ... , name TEXT)"},
 *               {2, "add email", "ALTER TABLE users ADD COLUMN email TEXT"},
 *           }};
 *           mig_.run(std::span<const orm::migration_script>(scripts));
 *       }
 *       orm::MigrationRunner& mig_;
 *   };
 */
export module cboot.orm.migration;

import std;
import cboot.core.annotations;  // Bean（组件 bean 契约要求显式注解）
import cboot.log;               // 迁移日志（cboot.orm.migration logger）
import cboot.orm.annotations;   // orm::error（orm.driver 依赖它，但类型定义在此单元）
import cboot.orm.driver;        // DataSource / connection / db_value / dialect

export namespace orm {

/** @brief 一个迁移脚本：version 严格递增；sql 为方言相关 DDL/DML */
struct migration_script {
    int version;                // 从 1 开始，严格递增
    std::string description;    // 变更说明（写入版本表，供审计）
    std::string sql;            // PG 可多语句；MySQL 一脚本一语句
};

/**
 * @brief 迁移执行器 — 幂等启动升级
 *
 * 依赖 DataSource（由驱动组件提供）；不拥有连接，操作期间租借。
 * 线程安全口径：run() 只应在启动期单线程调用（PostConstruct 阶段）。
 * 由 orm_component 贡献为 bean（注入 DataSource 后可用）。
 */
class [[=Bean{}]] MigrationRunner {
public:
    explicit MigrationRunner(DataSource& ds) : ds_(ds) {}

    /** @brief 当前版本号：版本表不存在或为空 → 0 */
    int current_version(std::string_view table = default_table()) const {
        auto lease = ds_.acquire();
        ensure_table(*lease, table);
        // version/applied_ms 列名不进方言引号：两方言均为小写非保留字
        auto stmt = lease->prepare(
            "SELECT MAX(version) FROM " + ds_.dialect().quote_identifier(table));
        if (!stmt->next() || stmt->is_null(0)) return 0;
        return static_cast<int>(stmt->get_int(0));
    }

    /**
     * @brief 执行全部未应用脚本（version 升序）；返回本次应用数
     *
     * 每个脚本独立事务：脚本 SQL + 版本行插入一起提交；
     * 任一脚本失败 → 回滚该脚本、抛 orm::error（含版本号与原因），
     * 已提交的前序脚本保留（前进式策略）。
     */
    std::size_t run(std::span<const migration_script> scripts,
                    std::string_view table = default_table()) {
        // 校验：version 正数且严格递增（排序前后一致）
        std::vector<const migration_script*> ordered;
        ordered.reserve(scripts.size());
        for (const auto& s : scripts) ordered.push_back(&s);
        std::ranges::stable_sort(ordered, [](auto* a, auto* b) {
            return a->version < b->version;
        });
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (ordered[i]->version <= 0)
                throw error("cboot.orm: migration version must be positive");
            if (i > 0 && ordered[i]->version == ordered[i - 1]->version)
                throw error("cboot.orm: duplicate migration version "
                            + std::to_string(ordered[i]->version));
        }

        auto lease = ds_.acquire();
        ensure_table(*lease, table);
        const int current = read_version(*lease, table);

        auto log = logging::get("cboot.orm.migration");
        std::size_t applied = 0;
        for (const migration_script* s : ordered) {
            if (s->version <= current) continue;   // 已应用 → 跳过（幂等）
            apply_one(*lease, table, *s);
            log.info("applied migration v{}: {}", s->version,
                     s->description.empty() ? "(no description)"
                                            : s->description);
            ++applied;
        }
        if (applied == 0)
            log.info("schema up to date (version {})", current);
        return applied;
    }

    /** @brief 默认版本表名 */
    static constexpr std::string_view default_table() {
        return "cboot_schema_version";
    }

private:
    /** @brief 建版本表（幂等；类型名取自方言，兼容 PG/MySQL） */
    void ensure_table(connection& conn, std::string_view table) const {
        const dialect& d = ds_.dialect();
        const std::string qt = d.quote_identifier(table);
        // version UNIQUE：多实例并发启动时，第二个实例的版本行插入会
        // 冲突失败而不是重复执行脚本（见文件头多实例边界）
        conn.execute("CREATE TABLE IF NOT EXISTS " + qt + " ("
                     + d.quote_identifier("version") + " "
                     + d.type_name(sql_type::big_integer) + " NOT NULL UNIQUE, "
                     + d.quote_identifier("description") + " "
                     + d.type_name(sql_type::text) + ", "
                     + d.quote_identifier("applied_ms") + " "
                     + d.type_name(sql_type::big_integer) + " NOT NULL)");
    }

    /** @brief 读取当前最大版本；表为空/无行 → 0（调用方先 ensure_table） */
    int read_version(connection& conn, std::string_view table) const {
        auto stmt = conn.prepare(
            "SELECT MAX(version) FROM " + ds_.dialect().quote_identifier(table));
        if (!stmt->next() || stmt->is_null(0)) return 0;
        return static_cast<int>(stmt->get_int(0));
    }

    /**
     * @brief 单脚本事务：先占位版本行（UNIQUE），再执行脚本，一起提交
     *
     * 顺序刻意"版本行在前"：并发实例同时启动时，第二个实例的 INSERT
     * 因 UNIQUE 冲突失败并回滚，脚本不会被执行两次（宁可启动失败，
     * 也不重复执行 DDL）。MySQL 的 DDL 会隐式提交，此顺序下若脚本
     * 失败，版本行仍随隐式提交落库——这是 MySQL DDL 事务性限制，
     * 文档已注明脚本需可重入。
     */
    void apply_one(connection& conn, std::string_view table,
                   const migration_script& s) const {
        const dialect& d = ds_.dialect();
        conn.execute("BEGIN");
        bool committed = false;
        try {
            // 版本行：占位 + 审计（占位失败 = 已被其他实例应用/并发冲突）
            auto stmt = conn.prepare(
                "INSERT INTO " + d.quote_identifier(table) + " ("
                + d.quote_identifier("version") + ", "
                + d.quote_identifier("description") + ", "
                + d.quote_identifier("applied_ms") + ") VALUES ("
                + d.placeholder(1) + ", " + d.placeholder(2) + ", "
                + d.placeholder(3) + ")");
            const std::int64_t applied_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            stmt->bind(0, db_value{std::int64_t{s.version}});
            stmt->bind(1, db_value{s.description});
            stmt->bind(2, db_value{applied_ms});
            (void)stmt->next();

            conn.execute(s.sql);       // DDL 在 MySQL 隐式提交——见文件头边界

            conn.execute("COMMIT");
            committed = true;
        } catch (...) {
            if (!committed) {
                try {
                    conn.execute("ROLLBACK");
                } catch (...) {
                    // 回滚失败：连接归还池后由借出校验/服务器回收
                }
            }
            // 带版本号的失败诊断；并发冲突也落在这里（附原因链）
            std::throw_with_nested(error(
                "cboot.orm: migration v" + std::to_string(s.version)
                + " (" + (s.description.empty() ? "no description"
                                                : s.description)
                + ") failed"));
        }
    }

    DataSource& ds_;
};

} // namespace orm
