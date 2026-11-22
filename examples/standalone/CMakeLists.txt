cmake_minimum_required(VERSION 4.3.2)

# 消费方口径：必须在 project() 之前设置（CMake 4.3 + GCC16 模块/import std）
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(cboot_example_standalone LANGUAGES CXX)

# 默认使用仓库内 Release SDK 安装目录（README 第 1 步安装到这里）：
#   <repo>/build-release/install
# 外部使用/CI 可用 -DCBOOT_SDK_DIR=... 或 -DCMAKE_PREFIX_PATH=... 覆盖。
set(CBOOT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build-release/install"
    CACHE PATH "Installed cboot SDK prefix")
if(CBOOT_SDK_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${CBOOT_SDK_DIR}")
endif()

# 必须在 add_executable 之前：包配置会目录级注入 -freflection，
# 让 CMake 合成的 std 模块也带反射旗标。
find_package(cboot REQUIRED)

add_executable(cboot_example_standalone main.cpp)
# 必须直接链接模块 provider 目标（cboot::app），否则 import cboot.app 解析不到。
target_link_libraries(cboot_example_standalone PRIVATE cboot::app)
