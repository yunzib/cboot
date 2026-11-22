# ============================================================================
# cboot.app — 应用层（引导编排：自动发现 / 装配 / 启动）
#
# 依赖方向：app → web.server → routing/http → core/json/aop → meta
# app 是装配器/引导层，不包含契约组件；组件由应用 TU 的 components 选择。
# ============================================================================
cboot_package(app
    TIER bootstrap
    MODULES app.cppm
    SOURCES src/app.cpp
    DEPENDS aop config core json meta web
)
