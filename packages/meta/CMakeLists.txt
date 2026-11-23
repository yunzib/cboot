# ============================================================================
# cboot.meta — 反射基建层（最底层，无内部包依赖）
#
# 内容：GCC16 纪律下的反射工具集（成员/注解/enum）、for_each_member、
# type_list、type_of_t 桥接。后续所有包建立在本层之上。
# ============================================================================
cboot_package(meta
    VERSION ${CBOOT_VERSION}
    TIER foundation
    MODULES reflect.cppm
)

# -freflection PUBLIC：消费方实例化含 [:r:] splice 的模板同样需要旗标
# （不放目录级全局，避免未来三方目标被旗标打崩）
target_compile_options(cboot_meta PUBLIC
    "$<$<CXX_COMPILER_ID:GNU>:-freflection>"
)

# R2 探针结果（0/1）PUBLIC 导出：下游按能力门控写法
target_compile_definitions(cboot_meta PUBLIC
    CBOOT_HAS_ANNOTATIONS=$<BOOL:${CBOOT_PROBE_ANNOTATIONS}>
    CBOOT_HAS_ANNOTATION_VALUE=$<BOOL:${CBOOT_PROBE_ANNOTATION_VALUE}>
    CBOOT_HAS_EXPANSION=$<BOOL:${CBOOT_PROBE_EXPANSION}>
    CBOOT_HAS_CTORS=$<BOOL:${CBOOT_PROBE_CTORS}>
)

# 工具链运行时要求（-static / stdc++exp / 多定义放行）经接口目标传递
target_link_libraries(cboot_meta PUBLIC cboot::runtime)
