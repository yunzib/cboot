# ============================================================================
# cbootPackage.cmake — 平铺包（foundation/runtime/bootstrap）公共构建函数
#
# 约定：
#   - 每个包一个同名 target：cboot_<name>，别名 cboot::<name>
#   - 模块接口单元放包根目录，实现单元放 <package>/src/
#   - 基础包统一导出到共享 CbootTargets，由根 CMake 一次 install(EXPORT)
#   - 契约组件的独立导出见 cbootComponent.cmake
# ============================================================================
include_guard(GLOBAL)
include(GNUInstallDirs)

# 公共 target 构造：模块文件集 / 实现源 / 依赖链接 / 别名 / 包内测试。
# 只建 target，不做 install；导出策略由 cboot_package / cboot_component 决定。
function(cboot_package_target name)
    cmake_parse_arguments(ARG "" "" "MODULES;SOURCES;DEPENDS;TEST_SOURCES" ${ARGN})

    if(NOT ARG_MODULES)
        message(FATAL_ERROR "cboot_package_target(${name}): MODULES is required")
    endif()

    set(target "cboot_${name}")
    add_library(${target})
    target_compile_features(${target} PUBLIC cxx_std_26)

    # BASE_DIRS 固定为包根：模块文件路径在导入方以包为基准解析
    target_sources(${target}
        PUBLIC
            FILE_SET cxx_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES ${ARG_MODULES}
    )

    # 纯接口/纯头文件包可以没有实现单元
    if(ARG_SOURCES)
        target_sources(${target} PRIVATE ${ARG_SOURCES})
    endif()

    # 包依赖只写包名，统一解析为 cboot::<name>
    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(${target} PUBLIC "cboot::${dep}")
    endforeach()

    # in-tree 与 find_package 消费方使用同一个 cboot::<name> 名字
    add_library("cboot::${name}" ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME "${name}")

    # 包内冒烟测试：in-tree 由根 CMake 统一 enable_testing()。
    # 测试常直接 import 依赖包的模块；模块 provider 必须直接链接，
    # 所以把本包的 DEPENDS 一并挂到测试目标上。
    if(ARG_TEST_SOURCES)
        set(test_dep_targets)
        foreach(dep IN LISTS ARG_DEPENDS)
            list(APPEND test_dep_targets "cboot::${dep}")
        endforeach()

        add_executable("${target}_test" ${ARG_TEST_SOURCES})
        target_link_libraries("${target}_test" PRIVATE ${target} ${test_dep_targets})
        add_test(NAME "${name}_smoke" COMMAND "${target}_test")
    endif()
endfunction()

# 基础包（foundation/runtime/bootstrap）：
#   - target 进入共享 CbootTargets
#   - 模块文件安装到 share/cboot/modules/<name>/
#   - install(EXPORT CbootTargets) 由根 CMake 在所有基础包定义完后统一执行
function(cboot_package name)
    cmake_parse_arguments(ARG ""
        "VERSION;TIER"
        "MODULES;SOURCES;DEPENDS;TEST_SOURCES"
        ${ARGN})

    # TIER 是声明式分类（文档/CI/诊断），不改变导出方式；
    # 基础包只能是 foundation/runtime/bootstrap。
    if(NOT ARG_TIER)
        set(ARG_TIER foundation)
    endif()
    if(NOT ARG_TIER MATCHES "^(foundation|runtime|bootstrap)$")
        message(FATAL_ERROR
            "cboot_package(${name}): TIER must be foundation/runtime/bootstrap, "
            "got '${ARG_TIER}'")
    endif()

    cboot_package_target(${name}
        MODULES ${ARG_MODULES}
        SOURCES ${ARG_SOURCES}
        DEPENDS ${ARG_DEPENDS}
        TEST_SOURCES ${ARG_TEST_SOURCES})

    install(TARGETS "cboot_${name}" EXPORT CbootTargets
        FILE_SET cxx_modules
            DESTINATION "${CMAKE_INSTALL_DATADIR}/cboot/modules/${name}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()
