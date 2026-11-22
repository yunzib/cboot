# ============================================================================
# cbootComponent.cmake — 契约组件公共构建函数
#
# 与基础包共用 cboot_package_target 的 target/模块/测试逻辑，但独立安装导出：
#   - 导出集：Cboot<Name>Targets
#   - 包名：cboot.<name>
#   - 包配置：cboot.<name>Config.cmake（find_dependency(cboot) 先行）
# ============================================================================
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/cbootPackage.cmake")
include(CMakePackageConfigHelpers)

function(cboot_component name)
    cmake_parse_arguments(ARG ""
        "VERSION;TIER"
        "MODULES;SOURCES;DEPENDS;TEST_SOURCES;PACKAGE_DEPENDS"
        ${ARGN})

    if(NOT ARG_VERSION)
        message(FATAL_ERROR "cboot_component(${name}): VERSION is required")
    endif()

    # TIER 是声明式分类；契约组件固定为 contract。
    if(NOT ARG_TIER)
        set(ARG_TIER contract)
    endif()
    if(NOT ARG_TIER STREQUAL "contract")
        message(FATAL_ERROR
            "cboot_component(${name}): TIER must be 'contract', got '${ARG_TIER}'")
    endif()

    cboot_package_target(${name}
        MODULES ${ARG_MODULES}
        SOURCES ${ARG_SOURCES}
        DEPENDS ${ARG_DEPENDS}
        TEST_SOURCES ${ARG_TEST_SOURCES})

    install(TARGETS "cboot_${name}" EXPORT "Cboot${name}Targets"
        FILE_SET cxx_modules
            DESTINATION "${CMAKE_INSTALL_DATADIR}/cboot/modules/${name}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

    install(EXPORT "Cboot${name}Targets"
        NAMESPACE cboot::
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/cboot.${name}")

    # 包配置模板随 cboot 包安装；in-tree 由根 CMake 提供 CBOOT_CMAKE_DIR
    if(NOT CBOOT_CMAKE_DIR AND cboot_CMAKE_DIR)
        set(CBOOT_CMAKE_DIR "${cboot_CMAKE_DIR}")
    endif()
    if(NOT CBOOT_CMAKE_DIR)
        message(FATAL_ERROR
            "cboot_component(${name}): CBOOT_CMAKE_DIR is not set "
            "(in-tree: root CMake; standalone: find_package(cboot))")
    endif()

    set(CBOOT_COMPONENT_NAME "cboot.${name}")
    set(CBOOT_COMPONENT_EXPORT "Cboot${name}Targets")
    set(CBOOT_COMPONENT_DEPENDS "${ARG_PACKAGE_DEPENDS}")
    configure_package_config_file(
        "${CBOOT_CMAKE_DIR}/cbootComponentConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cboot.${name}Config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/cboot.${name}")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cboot.${name}ConfigVersion.cmake"
        VERSION ${ARG_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/cboot.${name}Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cboot.${name}ConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/cboot.${name}")
endfunction()
