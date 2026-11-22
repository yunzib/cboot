export module cboot.core.annotations;

import std;

/**
 * @brief cboot 注解词汇表 — 极简内核（"框架行为的键"才成为注解类型）
 *
 * 内核 = Bean + Inject + 生命周期键；角色注解（Service/Controller/...）不设：
 *   - 注册由显式清单承担（context<Beans...>），注解不是开关
 *   - 路由发现不需要 Controller 标记（编译期在清单闭包内扫方法能力注解）
 *   - 分层语义是文档，不是行为键——将来有真实消费者时经 Bean 字段演进
 *
 * GCC16.2 约束（探针实测）：
 *   - 注解值类型必须 **structural**（全公开、无用户构造器）——
 *     std::string/string_view 均不合格（私有成员），字符串经 bean_name
 *     （char[64] 聚合体 + 字面量直初始化）承载
 *   - 注解值必须是值表达式：[[=Bean{"svc"}]]（花括号不可省）
 *   - 类型注解须置于 class-key 之后：struct [[=...]] X {}
 *
 * 命名规则（扁平）：框架词汇一律模块作用域裸名（Bean/Inject/...），
 * 注解在声明处零限定；工具 API 仅保留一级命名空间（json::/config::）。
 * 模块名（cboot.*）仍承载分层文档价值——模块名不出现在用户代码里。
 */
/** @brief 注解字符串值：structural 聚合（char[64] 零填充；超长字面量编译错） */
export struct bean_name {
    char data[64]{};

    constexpr std::string_view view() const { return std::string_view(data); }
    constexpr bool empty() const { return data[0] == '\0'; }
    constexpr bool operator==(const bean_name&) const = default;
};

/**
 * @brief Bean 元数据载体（唯一类级注解）
 * 可选 name：未命名 = 默认实例；有名字供 [[=Named{"..."}]] 选择。
 * 结构体可持续加字段演进（scope/lazy/profiles...），替代注解类型分化。
 */
export struct Bean { bean_name name{}; };

/**
 * @brief 配置绑定注解（通用配置 bean，任意结构体可用）
 *
 *   struct [[=Config{"db"}]] DbProps { std::string host; int port; };
 *
 * 语义：容器不做构造，而以 config::bind<T>(props, prefix) 生成实例；
 * 其他 bean 构造器直接注入 T&。prefix 为空 = 根作用域。
 */
export struct Config { bean_name prefix{}; };

/** @brief 构造器选择注解：多候选构造器时显式指定注入用者（≤1 个） */
export struct Inject {};

/**
 * @brief 注入限定符：按 bean 名字选择同类型/派生类型的实例
 *
 *   UserService([[=Named{"primary"}]] DataSource& ds);
 *
 * 未加限定符时：候选唯一，或候选中存在唯一默认实例（[[=Bean{}]] 无名）。
 */
export struct Named { bean_name name{}; };

/**
 * @brief 构造器参数标量配置注入
 *
 *   UserService([[=Value{"db.pool_size"}]] int pool);
 *
 * 支持 bool / 整数 / 浮点 / std::string / 枚举；参数按值或 const 引用声明。
 * 缺键或值无法转换 → 启动期抛出带键名与 bean 名的错误。
 */
export struct Value { bean_name key{}; };

/**
 * @brief 条件装配：仅当 profile 激活时注册
 *
 *   struct [[=Profile{"dev"}]] DevSeeder { ... };
 *
 * 激活列表来自配置 `cboot.profiles.active`（逗号分隔，忽略空白）。
 * 无 Profile 注解的 bean 始终激活；未激活 bean 不构造、不挂载、不可注入。
 */
export struct Profile { bean_name name{}; };

/**
 * @brief 事件监听方法：参数类型即订阅的事件类型（精确匹配）
 *
 *   class Audit {
 *   public:
 *       [[=EventListener{}]] void on(const OrderPlaced& e);
 *   };
 *
 * 方法必须 public、非静态、恰一个参数（按值或 const 引用）。
 * 同一事件按 bean 清单顺序、方法声明顺序同步派发。
 */
export struct EventListener {};

/** @brief 初始化回调（@PostConstruct）：bean 构造后、依赖者构造前调用（≤1 个） */
export struct PostConstruct {};

/** @brief 销毁回调（@PreDestroy）：逆拓扑序调用（≤1 个） */
export struct PreDestroy {};
