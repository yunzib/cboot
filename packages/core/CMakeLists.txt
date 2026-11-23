# ============================================================================
# cboot.core — 注解词汇表 + 编译期 DI 容器
#
# 依赖：meta（反射）、config（配置绑定）。
# core 不是“核心容器目录”：它与 json/config/web 平级，只是位于基础层。
# ============================================================================
cboot_package(core
    VERSION ${CBOOT_VERSION}
    TIER foundation
    MODULES annotations.cppm context.cppm
    DEPENDS config meta
)
