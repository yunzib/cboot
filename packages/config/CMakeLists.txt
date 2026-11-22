# ============================================================================
# cboot.config — 配置绑定（properties + 反射结构体）
#
# 注意：模块提供者必须直接链接；导出场景不解析传递依赖。
# config.cppm 直接 import cboot.meta.reflect 与 cboot.json。
# ============================================================================
cboot_package(config
    TIER foundation
    MODULES config.cppm
    SOURCES src/config.cpp
    DEPENDS json meta
)
