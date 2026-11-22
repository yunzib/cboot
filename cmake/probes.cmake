# ============================================================================
# R2 能力探针 — configure 期编译探测，一次探测全局消费
#
# 探针纪律：
#   - 探针代码 = 框架真实要写的 API 调用链（不是"语法能不能过"，而是
#     "cboot 要用的那条链路能不能过"），失败即视为该能力不可用
#   - 结果以 0/1 编译定义注入 cboot_meta（PUBLIC），下游模块与用户代码
#     以 #if CBOOT_HAS_* 门控写法
#   - 仅 GNU 探测（-freflection）；其他编译器一律 0（Phase 0 口径）
#
# 实现说明：不用 check_cxx_source_compiles——模块扫描模式下其对
# CMAKE_REQUIRED_FLAGS 的传递有干扰（实测 -freflection 丢失 → 全部
# 误报失败）。execute_process 直调编译器 + -fsyntax-only，旗标确定性
# 由本文件独占控制。
#
# 能力 → 后续用途：
#   ANNOTATIONS       [[=Anno]] 语法 + annotations_of 检索      → Bean/路由注解糖
#   ANNOTATION_VALUE  [[=Anno{v}]] 值注解 + extract 取值          → @Component("name") 式
#   EXPANSION         template for（P1306 展开语句）              → 编译期 AOP 织入
#   CTORS             members_of + is_constructor + parameters_of → 构造器自动装配
# ============================================================================

function(cboot_probe var src)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -freflection -fsyntax-only
                "${CMAKE_CURRENT_LIST_DIR}/probes/${src}"
        RESULT_VARIABLE rc
        OUTPUT_QUIET ERROR_QUIET
    )
    if(rc EQUAL 0)
        set(${var} 1 CACHE INTERNAL "cboot probe ${src}")
    else()
        set(${var} 0 CACHE INTERNAL "cboot probe ${src}")
    endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    cboot_probe(CBOOT_PROBE_ANNOTATIONS       annotations.cpp)
    cboot_probe(CBOOT_PROBE_ANNOTATION_VALUE  annotation_value.cpp)
    cboot_probe(CBOOT_PROBE_EXPANSION         expansion.cpp)
    cboot_probe(CBOOT_PROBE_CTORS             ctors.cpp)
else()
    set(CBOOT_PROBE_ANNOTATIONS      0 CACHE INTERNAL "")
    set(CBOOT_PROBE_ANNOTATION_VALUE 0 CACHE INTERNAL "")
    set(CBOOT_PROBE_EXPANSION        0 CACHE INTERNAL "")
    set(CBOOT_PROBE_CTORS            0 CACHE INTERNAL "")
endif()

message(STATUS "========================================")
message(STATUS "cboot R2 能力探针（${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}）")
message(STATUS "  annotations(P3394)       : ${CBOOT_PROBE_ANNOTATIONS}")
message(STATUS "  annotation_value(extract) : ${CBOOT_PROBE_ANNOTATION_VALUE}")
message(STATUS "  expansion(P1306)          : ${CBOOT_PROBE_EXPANSION}")
message(STATUS "  ctors(参数枚举)           : ${CBOOT_PROBE_CTORS}")
message(STATUS "========================================")
