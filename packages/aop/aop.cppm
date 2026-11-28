export module cboot.aop;

import std;

/**
 * @brief 编译期切面 — Spring AOP 的 C++ 形态（零动态代理）
 *
 * 与 Spring 的对照：
 *    Spring：运行时生成代理类 → 每次调用走代理链（反射 + 装箱）
 *    cboot ：切面在**编译期**经模板展开成静态调用链——概念约束校验、
 *            可内联、无堆分配、无虚调用；切面对象本身可有状态（计数器等）
 *
 * 切面契约（结构类型，无需继承）：
 *     struct MyAspect {
 *         template <class Next>
 *         Response around(const call_info& ci, Next&& next) { ... }
 *     };
 *   - next() 调一次即"放行到下一环"，可不调（短路）或调多次（重试）
 *   - 抛异常可被外层切面捕获（onion 语义：外层先入后出）
 *
 * 挂载点：框架真正控制调用的地方——
 *   - 路由 handler：全局切面 App::aspects / mount(obj, aspects...)，
 *     或方法级 [[=web::UseAspect{"name"}]] 选择切面 Bean
 *   - 生命周期：[[=PostConstruct{}]] / [[=PreDestroy{}]]，
 *     由 App::lifecycle_aspects 包裹
 *   - 事件监听：ctx.publish(event, wrap) 显式接链
 * 直接 bean 间调用不经切面（C++ 无隐式代理；需要时用 aspect_chain 显式包）。
 */

/**
 * @brief 连接点 — 切面所包裹的调用种类
 *
 * 生命周期切面同时包裹 PostConstruct 与 PreDestroy，靠 point 区分。
 */
export enum class join_point : std::uint8_t {
    method,          // 路由 handler / 事件监听 / 手工链
    post_construct,  // [[=PostConstruct{}]]
    pre_destroy,     // [[=PreDestroy{}]]
};

/** @brief 调用信息 — 方法名与 bean 名均来自编译期反射（identifier_of） */
export struct call_info {
    std::string_view bean;      // bean 类型名（identifier_of(^^T)）
    std::string_view method;    // 方法名（identifier_of(handler)）
    join_point point = join_point::method;
};

/**
 * @brief 切面概念：around(ci, next) 且可调用 next() 返回 R
 *
 * Next 为 continuation 类型（lambda），按值/转发均可
 */
export template <class A, class R, class Next>
concept AspectFor = requires (A& a, const call_info& ci, Next&& next) {
    { a.around(ci, std::forward<Next>(next)) } -> std::same_as<R>;
};

/**
 * @brief 编译期切面链（洋葱模型：As[0] 最外层）
 *
 *   aspect_chain<Trace, Auth> chain{Trace{}, Auth{}};
 *   Response r = chain.run(ci, [&]{ return handler(...); });
 *
 * 展开结果（全部静态调用，优化器可完全内联）：
 *   Trace::around(ci, [&]{ return Auth::around(ci, [&]{ return handler(...); }); })
 *
 * As 可含引用类型（如 aspect_chain<Trace&, Auth&>）：链持有切面的引用而非
 * 副本，方法级 [[=web::UseAspect]] 用它共享容器中的切面 Bean 实例。
 * core() 返回 void 时同样适用（生命周期方法）。
 */
export template <class... As>
class aspect_chain {
public:
    // forward 而非 move：As 为引用类型时（切面 Bean 实例）须保持左值引用，
    // move 会产生亡值，无法绑定到 tuple 里的 T& 元素。
    explicit aspect_chain(As... as) : aspects_(std::forward<As>(as)...) {}

    /** @brief 执行：core() 为最内层实际调用，R 由其返回类型推导 */
    template <class Core>
    auto run(const call_info& ci, Core&& core) {
        using R = std::decay_t<decltype(std::declval<Core&>()())>;
        return run_at<0, R>(ci, core);
    }

private:
    std::tuple<As...> aspects_;

    template <std::size_t I, class R, class Core>
    R run_at(const call_info& ci, Core& core) {
        if constexpr (I == sizeof...(As)) {
            return core();
        } else {
            auto& aspect = std::get<I>(aspects_);
            return aspect.around(ci, [&]() -> R {
                return run_at<I + 1, R>(ci, core);
            });
        }
    }
};

/** @brief 单切面便捷形态（无链、直接包裹一次调用） */
export template <class A, class Core>
auto around(A& aspect, const call_info& ci, Core&& core) {
    using R = std::decay_t<decltype(std::declval<Core&>()())>;
    return aspect.around(ci, [&]() -> R { return core(); });
}
