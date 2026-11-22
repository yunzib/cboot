cmake_minimum_required(VERSION 4.3.2)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C++ 命名模块基建（与 kwik-ui 同源配置，GCC16 + CMake 4.3 验证过）
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# import std 实验 UUID（CMake 4.3 口径）
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

# 版本语义自 v0.1.0 启动（Phase 0：骨架 + 反射基建 + 探针）
project(cboot VERSION 0.1.0 LANGUAGES CXX)

# ── 工具链口径 ─────────────────────────────────────────────
# 主链：GCC 16+（niXman MinGW）——唯一实现 P2996 反射的主流工具链（-freflection）
# Clang/MSVC：主线无实现，Phase 0 不做承诺（宏轨出口后续按需引入）
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING "cboot Phase 0 仅验证 GCC16 反射链；当前编译器(${CMAKE_CXX_COMPILER_ID})未经验证")
endif()

# ============================================================================
# 构建类型配置（KWIK_BUILD_TYPE 同款口径）
#
#   "Debug"       — 完整调试符号, 无优化
#   "Release"     — O3 + gc-sections
#   "MinSizeRel"  — Os + gc-sections
# ============================================================================
set(CBOOT_BUILD_TYPE "Debug" CACHE STRING "Debug | Release | MinSizeRel")
set_property(CACHE CBOOT_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel)
set(CMAKE_BUILD_TYPE "${CBOOT_BUILD_TYPE}" CACHE STRING "" FORCE)
# ── Debug flags ──────────────────────────────────────────
set(CMAKE_CXX_FLAGS_DEBUG "-g -Og" CACHE STRING "")
# ── Release/MinSizeRel flags（配合链接期 gc-sections 裁剪死代码）──
set(CMAKE_CXX_FLAGS_RELEASE    "-O3 -DNDEBUG -ffunction-sections -fdata-sections" CACHE STRING "")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG -ffunction-sections -fdata-sections" CACHE STRING "")
add_link_options(
    "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:LINKER:--gc-sections>"
    "$<$<AND:$<CONFIG:MinSizeRel>,$<CXX_COMPILER_ID:GNU>>:LINKER:--gc-sections>"
)
# IPO(LTO)：GNU 暂关——全局 IPO 使三方归档同为 LTO 对象时与 COFF 成员混合
# 报 "COFF object format mismatch"（kwik-ui harfbuzz 实测）；根因明确前不开
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# ── GCC16 模块工程坑（kwik-ui 趟平，直接继承）───────────────
# 同时以接口目标承载（cboot_runtime EXPORT 给外部消费方）
add_library(cboot_runtime INTERFACE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(
        # 运行时静态链接：单 exe 分发（libstdc++/libgcc/libwinpthread 全内嵌）
        -static
        # 模块接口 inline 函数局部静态在 BMI 消费者 obj 中重复实例化为强符号 → 多定义；
        # 同一实体语义安全，先放行保 spike（根因跟踪同 kwik-ui）
        -Wl,--allow-multiple-definition
    )
    # std::print 运行时自 GCC15 拆入 libstdc++exp.a，g++ 默认不链；
    # link_libraries 使其位于链接行对象/库区之后（归档按需拉入的顺序要求）
    link_libraries(stdc++exp)

    # 外部消费方（find_package）同样需要这三项——经接口目标传递
    target_link_options(cboot_runtime INTERFACE
        -static
        -Wl,--allow-multiple-definition
    )
    target_link_libraries(cboot_runtime INTERFACE stdc++exp)
endif()

# ── C++26 静态反射（P2996，GCC16）── 全局注入
# <meta> 头以 __cpp_impl_reflection 门控（仅 -freflection 定义）——std 模块
# 目标（__cmake_cxx_std_26，generate 期挂顶层目录属性）自身也须带旗标构建，
# 否则 std::meta 整体缺席 std BMI，模块单元 import std 后不可见（探针直
# include <meta> 不受影响，仅模块路径踩坑——实测复现 kwik-ui 记录）。
# 位置纪律：未来引入三方依赖时，本调用必须移至三方 add_subdirectory 之后
#（目录 COMPILE_OPTIONS 向前继承，前置会把旗标泄给自设低标准的三方）。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-freflection)
endif()

# ── R2 能力探针（注解 P3394 / template for P1306 / 构造器反射）──
# 须在分层模块之前：探针结果以编译定义注入 cboot_meta（PUBLIC 下游可见）
include(cmake/probes.cmake)

# ==================== 分层模块 ====================
include(cmake/modules/Meta.cmake)
include(cmake/modules/Core.cmake)
include(cmake/modules/Json.cmake)
include(cmake/modules/Config.cmake)
include(cmake/modules/Aop.cmake)
include(cmake/modules/Async.cmake)
include(cmake/modules/Web.cmake)
include(cmake/modules/App.cmake)

# ==================== 示例 ====================
option(CBOOT_BUILD_EXAMPLES "Build examples" ON)
if(CBOOT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ==================== 测试 ====================
enable_testing()    # 须在 add_subdirectory 之前调用，ctest 才能发现用例
add_subdirectory(test)

# ==================== 安装 / 导出（find_package(cboot)）====================
# 仅安装模块接口与静态库；消费方要求 CMake ≥ 4.3 + GCC16（-freflection 随
# 目标 PUBLIC 选项传递）。构建树同时导出，便于本地消费/测试。
install(TARGETS
        cboot_runtime cboot_meta cboot_core cboot_json cboot_config
        cboot_aop cboot_async cboot_web cboot_app
    EXPORT CbootTargets
    FILE_SET cxx_modules DESTINATION share/cboot/modules
    ARCHIVE DESTINATION lib
)

install(EXPORT CbootTargets
    NAMESPACE cboot::
    DESTINATION share/cmake/cboot
)
export(EXPORT CbootTargets
    NAMESPACE cboot::
    FILE ${CMAKE_CURRENT_BINARY_DIR}/cbootTargets.cmake
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/cbootConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cbootConfig.cmake
    INSTALL_DESTINATION share/cmake/cboot
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cbootConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cbootConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cbootConfigVersion.cmake
    DESTINATION share/cmake/cboot
)
