# ============================================================================
# cboot.log — 日志门面 + 内置 console/file/rolling 实现
#
# 贡献：level / logger / sink / layout / context；可选 log_component 负责
# 从 log.* 配置装配 sink。默认 console + info 始终可用。
# 依赖：core（注解/生命周期）、meta（type_list）。
# ============================================================================
cboot_package(log
    VERSION ${CBOOT_VERSION}
    TIER foundation
    MODULES log.cppm log.component.cppm
    SOURCES src/log.cpp
    DEPENDS meta core
    TEST_SOURCES test/log_smoke.cpp
)
