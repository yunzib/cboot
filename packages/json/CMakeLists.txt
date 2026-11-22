# ============================================================================
# cboot.json — 轻量 JSON（值模型 + 解析 + 反射绑定）
# 接口单元在包根，实现单元在 src/
# ============================================================================
cboot_package(json
    TIER foundation
    MODULES json.cppm
    SOURCES src/json.cpp
    DEPENDS meta
)
