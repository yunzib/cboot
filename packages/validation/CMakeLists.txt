# ============================================================================
# cboot.validation — DTO 字段校验基础包
#
# 不贡献 bean / hook，不参与 components 选择；应用 import 后使用注解。
# 依赖：meta（反射）、core（bean_name）。
# ============================================================================
cboot_package(validation
    VERSION ${CBOOT_VERSION}
    TIER foundation
    MODULES validation.cppm
    DEPENDS meta core
    TEST_SOURCES test/validation_smoke.cpp
)
