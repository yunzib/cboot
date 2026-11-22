# ============================================================================
# packages/CMakeLists.txt — 同级包注册表
#
# 只负责 add_subdirectory 和开关；包的模块、依赖、安装、测试都在各自
# packages/<name>/CMakeLists.txt 中声明。
# 目录平级；依赖由 target_link_libraries 表达，不用目录嵌套。
# ============================================================================

# ── 基础包 ──────────────────────────────────────────────────────────────
# 随 cboot 一起安装导出；不参与应用 components 选择。
add_subdirectory(meta)
add_subdirectory(json)
add_subdirectory(config)
add_subdirectory(core)
add_subdirectory(aop)
add_subdirectory(async)
add_subdirectory(web)
add_subdirectory(app)

# ── 契约组件 ────────────────────────────────────────────────────────────
# 独立包 + 独立导出 + components 选择。Phase B 再接入；
# 新增组件时先建目录，再在这里 add_subdirectory。
#
# add_subdirectory(cache)
