# ── 正向测试 ──
add_executable(cboot_smoke_reflect smoke_reflect.cpp)
target_link_libraries(cboot_smoke_reflect PRIVATE cboot_meta)
add_test(NAME smoke_reflect COMMAND cboot_smoke_reflect)

add_executable(cboot_context_smoke context_smoke.cpp)
target_link_libraries(cboot_context_smoke PRIVATE cboot_core)
add_test(NAME context_smoke COMMAND cboot_context_smoke)

add_executable(cboot_json_smoke json_smoke.cpp)
target_link_libraries(cboot_json_smoke PRIVATE cboot_json)
add_test(NAME json_smoke COMMAND cboot_json_smoke)

add_executable(cboot_config_smoke config_smoke.cpp)
target_link_libraries(cboot_config_smoke PRIVATE cboot_config cboot_core)
add_test(NAME config_smoke COMMAND cboot_config_smoke)

add_executable(cboot_web_routing_smoke web_routing_smoke.cpp)
target_link_libraries(cboot_web_routing_smoke PRIVATE cboot_web)
add_test(NAME web_routing_smoke COMMAND cboot_web_routing_smoke)

add_executable(cboot_web_server_smoke web_server_smoke.cpp)
target_link_libraries(cboot_web_server_smoke PRIVATE cboot_web)
add_test(NAME web_server_smoke COMMAND cboot_web_server_smoke)

# 同一份协议测试跑 IOCP 后端（Windows 完成端口）
add_executable(cboot_web_server_iocp_smoke web_server_smoke.cpp)
target_compile_definitions(cboot_web_server_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_web_server_iocp_smoke PRIVATE cboot_web)
add_test(NAME web_server_iocp_smoke COMMAND cboot_web_server_iocp_smoke)

add_executable(cboot_class_smoke class_smoke.cpp)
target_link_libraries(cboot_class_smoke PRIVATE cboot_web)
add_test(NAME class_smoke COMMAND cboot_class_smoke)

add_executable(cboot_aop_smoke aop_smoke.cpp)
target_link_libraries(cboot_aop_smoke PRIVATE cboot_web)
add_test(NAME aop_smoke COMMAND cboot_aop_smoke)

add_executable(cboot_autoscan_smoke autoscan_smoke.cpp)
target_link_libraries(cboot_autoscan_smoke PRIVATE cboot_app)
add_test(NAME autoscan_smoke COMMAND cboot_autoscan_smoke)

add_executable(cboot_async_smoke async_smoke.cpp)
target_link_libraries(cboot_async_smoke PRIVATE cboot_async)
add_test(NAME async_smoke COMMAND cboot_async_smoke)

add_executable(cboot_backend_select_smoke backend_select_smoke.cpp)
target_link_libraries(cboot_backend_select_smoke PRIVATE cboot_web)
add_test(NAME backend_select_smoke COMMAND cboot_backend_select_smoke)

# 背压（max_connections → 503）：线程 / IOCP 双后端
add_executable(cboot_backpressure_smoke backpressure_smoke.cpp)
target_link_libraries(cboot_backpressure_smoke PRIVATE cboot_web)
add_test(NAME backpressure_smoke COMMAND cboot_backpressure_smoke)

add_executable(cboot_backpressure_iocp_smoke backpressure_smoke.cpp)
target_compile_definitions(cboot_backpressure_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_backpressure_iocp_smoke PRIVATE cboot_web)
add_test(NAME backpressure_iocp_smoke COMMAND cboot_backpressure_iocp_smoke)

# 请求级超时（handler SLA → 504）：线程 / IOCP 双后端
add_executable(cboot_handler_timeout_smoke handler_timeout_smoke.cpp)
target_link_libraries(cboot_handler_timeout_smoke PRIVATE cboot_web)
add_test(NAME handler_timeout_smoke COMMAND cboot_handler_timeout_smoke)

add_executable(cboot_handler_timeout_iocp_smoke handler_timeout_smoke.cpp)
target_compile_definitions(cboot_handler_timeout_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_handler_timeout_iocp_smoke PRIVATE cboot_web)
add_test(NAME handler_timeout_iocp_smoke COMMAND cboot_handler_timeout_iocp_smoke)

# ── 基准工具（非 ctest 用例；手动运行：./build/test/cboot_bench 8 20000 [iocp] [conns]）──
add_executable(cboot_bench bench.cpp)
target_link_libraries(cboot_bench PRIVATE cboot_web)

# ── 负面用例（编译期诊断演示；预期编译失败，不进默认构建）──
# 开启方式：cmake -B build-neg -DCBOOT_BUILD_NEGATIVE_TESTS=ON && cmake --build build-neg
option(CBOOT_BUILD_NEGATIVE_TESTS "Build negative (expected-fail) diagnostics demos" OFF)
if(CBOOT_BUILD_NEGATIVE_TESTS)
    add_executable(cboot_neg_cycle negative_cycle.cpp)
    target_link_libraries(cboot_neg_cycle PRIVATE cboot_core)

    add_executable(cboot_neg_missing_dep negative_missing_dep.cpp)
    target_link_libraries(cboot_neg_missing_dep PRIVATE cboot_core)

    add_executable(cboot_neg_private_ctor negative_private_ctor.cpp)
    target_link_libraries(cboot_neg_private_ctor PRIVATE cboot_core)

    add_executable(cboot_neg_controller_no_bean negative_controller_no_bean.cpp)
    target_link_libraries(cboot_neg_controller_no_bean PRIVATE cboot_app)

    add_executable(cboot_neg_ambiguous_ctor negative_ambiguous_ctor.cpp)
    target_link_libraries(cboot_neg_ambiguous_ctor PRIVATE cboot_core)
endif()
set_tests_properties(web_server_smoke PROPERTIES TIMEOUT 30)
set_tests_properties(context_smoke json_smoke config_smoke web_routing_smoke smoke_reflect class_smoke aop_smoke autoscan_smoke async_smoke backend_select_smoke backpressure_smoke backpressure_iocp_smoke handler_timeout_smoke handler_timeout_iocp_smoke PROPERTIES TIMEOUT 30)
