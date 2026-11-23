# ============================================================================
# cboot.orm — 通用 ORM 契约组件
#
# 贡献：EntityManager（CRUD/DSL）。
# 依赖：meta/core；不链接任何数据库客户端。
# 具体驱动在 packages/orm.postgres、packages/orm.mysql。
# ============================================================================

cboot_component(orm
    TIER contract
    VERSION 0.1.0
    MODULES
        orm.annotations.cppm
        orm.driver.cppm
        orm.meta.cppm
        orm.dsl.cppm
        orm.cppm
    DEPENDS meta core config
    TEST_SOURCES test/orm_smoke.cpp
)

# 测试只为了校验组件描述符/闭包；ORM 库本身不依赖 app。
if(TARGET cboot_orm_test)
    target_link_libraries(cboot_orm_test PRIVATE cboot_app)
endif()
