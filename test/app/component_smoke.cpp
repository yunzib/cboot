// 契约组件冒烟测试
//
// 覆盖：组件闭包顺序、传递依赖、bean 合并、hooks 收集、命名空间隔离。
import cboot.app;
import std;

template <class L, class T>
struct contains;
template <class... Ts, class T>
struct contains<type_list<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts, T> || ...)> {};

namespace cache {

struct [[=Bean{}]] MemoryCache {
    int value() const { return 42; }
};

} // namespace cache

// cache_health 依赖 cache；应用只选 cache_health
struct cache_component {
    using beans = type_list<cache::MemoryCache>;
    using dependencies = type_list<>;
};

struct cache_health_component {
    using beans = type_list<>;
    using dependencies = type_list<cache_component>;
};

struct CacheApp : application<CacheApp> {
    using components = type_list<cache_health_component>;
};

static_assert(std::is_same_v<
    component_closure_t<CacheApp>,
    type_list<cache_component, cache_health_component>>,
    "closure must be dependency-first");
static_assert(contains<app_beans_t<CacheApp>, cache::MemoryCache>::value,
    "dependency component bean must be merged");
static_assert(std::is_same_v<app_hooks_t<CacheApp>, type_list<>>);

// 组件可以只贡献 hook
struct NoopHook {
    template <class Ctx>
    struct instance {
        explicit instance(Ctx&) {}
    };
};

struct hook_component {
    using beans = type_list<>;
    using hooks = type_list<NoopHook>;
    using dependencies = type_list<>;
};

struct HookApp : application<HookApp> {
    using components = type_list<hook_component>;
};

static_assert(std::is_same_v<app_hooks_t<HookApp>, type_list<NoopHook>>);

int main() {
    std::println("COMPONENT SMOKE: PASS");
    return 0;
}
