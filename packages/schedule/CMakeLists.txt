# ============================================================================
# cboot.schedule — 定时任务契约组件
#
# 贡献：Scheduled 注解、cron 解析、schedule_hook（无 bean，纯 hook）。
# 独立包 cboot.schedule；应用通过 components 选择后生效。
# ============================================================================
cboot_component(schedule
    TIER contract
    VERSION 0.1.0
    MODULES schedule.cppm
    DEPENDS meta core
)
