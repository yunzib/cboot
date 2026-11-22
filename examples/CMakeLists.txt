# 源码树内仅做编译验证（target 为开发便利）；
# 独立应用构建（find_package + Release 安装 SDK）见 standalone/README.md。
add_executable(cboot_example_users standalone/main.cpp)
target_link_libraries(cboot_example_users PRIVATE cboot_app cboot_config)
