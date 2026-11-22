# ============================================================================
# cboot.meta — 反射基建层（最底层，无内部依赖）
#
# 内容：GCC16 纪律下的 R1 反射白名单工具集（成员三访问器 / for_each_member
# / enum 注册表）。后续 core/config/json/web 全部建立在本层之上。
# ============================================================================
add_library(cboot_meta)
# 导出给外部消费方：模块化目标必须自带标准要求（否则 find_package 生成失败）
target_compile_features(cboot_meta PUBLIC cxx_std_26)

target_sources(cboot_meta
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/meta
        FILES
            modules/meta/reflect.cppm
)

# -freflection PUBLIC：消费方实例化含 [:r:] splice 的模板同样需要旗标
# （kwik-ui kwik_bridge 同款纪律；不放全局以防未来三方目标被旗标打崩）
target_compile_options(cboot_meta
    PUBLIC "$<$<CXX_COMPILER_ID:GNU>:-freflection>"
)

# R2 探针结果（0/1）PUBLIC 导出：下游按能力门控写法
target_compile_definitions(cboot_meta PUBLIC
    CBOOT_HAS_ANNOTATIONS=$<BOOL:${CBOOT_PROBE_ANNOTATIONS}>
    CBOOT_HAS_ANNOTATION_VALUE=$<BOOL:${CBOOT_PROBE_ANNOTATION_VALUE}>
    CBOOT_HAS_EXPANSION=$<BOOL:${CBOOT_PROBE_EXPANSION}>
    CBOOT_HAS_CTORS=$<BOOL:${CBOOT_PROBE_CTORS}>
)

# 工具链运行时要求（-static / stdc++exp / 多定义放行）经接口目标传递
target_link_libraries(cboot_meta PUBLIC cboot_runtime)
