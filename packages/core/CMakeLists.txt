# ============================================================================
# cboot.core — 注解词汇表 + 编译期 DI 容器
#
# 依赖：meta（反射）、config（配置绑定）。
# core 不是“核心容器目录”：它与 json/config/web 平级，只是位于基础层。
# ============================================================================
cboot_package(core
    VERSION ${CBOOT_VERSION}
    TIER foundation
    # metrics.cppm：指标注册表（counter/gauge/histogram/Prometheus 渲染）
    # src/metrics.cpp：进程级注册表单例定义（避免模块 inline 静态跨 TU 分裂）
    MODULES annotations.cppm metrics.cppm context.cppm
    SOURCES src/metrics.cpp
    DEPENDS config meta
)
