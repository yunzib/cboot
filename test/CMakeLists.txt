# 测试按包/职责分类：meta / core / app / schedule / json / config / aop / async / web
# 负面用例统一放 negative/，基准工具放 bench/。

# ── meta ────────────────────────────────────────────────────────────────
add_executable(cboot_smoke_reflect meta/smoke_reflect.cpp)
target_link_libraries(cboot_smoke_reflect PRIVATE cboot_meta)
add_test(NAME smoke_reflect COMMAND cboot_smoke_reflect)

# ── core：DI / 生命周期 / 注解 ──────────────────────────────────────────
add_executable(cboot_context_smoke core/context_smoke.cpp)
target_link_libraries(cboot_context_smoke PRIVATE cboot_core)
add_test(NAME context_smoke COMMAND cboot_context_smoke)

add_executable(cboot_named_smoke core/named_smoke.cpp)
target_link_libraries(cboot_named_smoke PRIVATE cboot_core)
add_test(NAME named_smoke COMMAND cboot_named_smoke)

add_executable(cboot_value_smoke core/value_smoke.cpp)
target_link_libraries(cboot_value_smoke PRIVATE cboot_core)
add_test(NAME value_smoke COMMAND cboot_value_smoke)

add_executable(cboot_profile_smoke core/profile_smoke.cpp)
target_link_libraries(cboot_profile_smoke PRIVATE cboot_core)
add_test(NAME profile_smoke COMMAND cboot_profile_smoke)

add_executable(cboot_event_smoke core/event_smoke.cpp)
target_link_libraries(cboot_event_smoke PRIVATE cboot_core cboot_aop)
add_test(NAME event_smoke COMMAND cboot_event_smoke)

# ── app：自动发现 / 健康检查 / 组件装配 ────────────────────────────────
add_executable(cboot_component_smoke app/component_smoke.cpp)
target_link_libraries(cboot_component_smoke PRIVATE cboot_app)
add_test(NAME component_smoke COMMAND cboot_component_smoke)

add_executable(cboot_health_smoke app/health_smoke.cpp)
target_link_libraries(cboot_health_smoke PRIVATE cboot_app)
add_test(NAME health_smoke COMMAND cboot_health_smoke)

add_executable(cboot_autoscan_smoke app/autoscan_smoke.cpp)
target_link_libraries(cboot_autoscan_smoke PRIVATE cboot_app)
add_test(NAME autoscan_smoke COMMAND cboot_autoscan_smoke)

# ── schedule 组件：固定延迟 / cron / hook ───────────────────────────────
add_executable(cboot_scheduled_smoke schedule/scheduled_smoke.cpp)
target_link_libraries(cboot_scheduled_smoke PRIVATE cboot_core cboot_schedule)
add_test(NAME scheduled_smoke COMMAND cboot_scheduled_smoke)

add_executable(cboot_schedule_expr_smoke schedule/schedule_expr_smoke.cpp)
target_link_libraries(cboot_schedule_expr_smoke PRIVATE cboot_core cboot_schedule cboot_app)
add_test(NAME schedule_expr_smoke COMMAND cboot_schedule_expr_smoke)

# ── json / config / aop / async ─────────────────────────────────────────
add_executable(cboot_json_smoke json/json_smoke.cpp)
target_link_libraries(cboot_json_smoke PRIVATE cboot_json)
add_test(NAME json_smoke COMMAND cboot_json_smoke)

add_executable(cboot_config_smoke config/config_smoke.cpp)
target_link_libraries(cboot_config_smoke PRIVATE cboot_config cboot_core)
add_test(NAME config_smoke COMMAND cboot_config_smoke)

add_executable(cboot_aop_smoke aop/aop_smoke.cpp)
target_link_libraries(cboot_aop_smoke PRIVATE cboot_web)
add_test(NAME aop_smoke COMMAND cboot_aop_smoke)

add_executable(cboot_async_smoke async/async_smoke.cpp)
target_link_libraries(cboot_async_smoke PRIVATE cboot_async)
add_test(NAME async_smoke COMMAND cboot_async_smoke)

# ── web：路由 / 服务器 / 后端 / 背压 / 超时 ────────────────────────────
add_executable(cboot_web_routing_smoke web/web_routing_smoke.cpp)
target_link_libraries(cboot_web_routing_smoke PRIVATE cboot_web cboot_validation)
add_test(NAME web_routing_smoke COMMAND cboot_web_routing_smoke)

add_executable(cboot_web_server_smoke web/web_server_smoke.cpp)
target_link_libraries(cboot_web_server_smoke PRIVATE cboot_web)
add_test(NAME web_server_smoke COMMAND cboot_web_server_smoke)

# 同一份协议测试跑 IOCP 后端（Windows 完成端口）
add_executable(cboot_web_server_iocp_smoke web/web_server_smoke.cpp)
target_compile_definitions(cboot_web_server_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_web_server_iocp_smoke PRIVATE cboot_web)
add_test(NAME web_server_iocp_smoke COMMAND cboot_web_server_iocp_smoke)

add_executable(cboot_class_smoke web/class_smoke.cpp)
target_link_libraries(cboot_class_smoke PRIVATE cboot_web)
add_test(NAME class_smoke COMMAND cboot_class_smoke)

add_executable(cboot_backend_select_smoke web/backend_select_smoke.cpp)
target_link_libraries(cboot_backend_select_smoke PRIVATE cboot_web)
add_test(NAME backend_select_smoke COMMAND cboot_backend_select_smoke)

# 背压（max_connections → 503）：线程 / IOCP 双后端
add_executable(cboot_backpressure_smoke web/backpressure_smoke.cpp)
target_link_libraries(cboot_backpressure_smoke PRIVATE cboot_web)
add_test(NAME backpressure_smoke COMMAND cboot_backpressure_smoke)

add_executable(cboot_backpressure_iocp_smoke web/backpressure_smoke.cpp)
target_compile_definitions(cboot_backpressure_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_backpressure_iocp_smoke PRIVATE cboot_web)
add_test(NAME backpressure_iocp_smoke COMMAND cboot_backpressure_iocp_smoke)

# 请求级超时（handler SLA → 504）：线程 / IOCP 双后端
add_executable(cboot_handler_timeout_smoke web/handler_timeout_smoke.cpp)
target_link_libraries(cboot_handler_timeout_smoke PRIVATE cboot_web)
add_test(NAME handler_timeout_smoke COMMAND cboot_handler_timeout_smoke)

add_executable(cboot_handler_timeout_iocp_smoke web/handler_timeout_smoke.cpp)
target_compile_definitions(cboot_handler_timeout_iocp_smoke PRIVATE CBOOT_TEST_IO=1)
target_link_libraries(cboot_handler_timeout_iocp_smoke PRIVATE cboot_web)
add_test(NAME handler_timeout_iocp_smoke COMMAND cboot_handler_timeout_iocp_smoke)

# ── bench：非 ctest 用例，手动运行 ──────────────────────────────────────
add_executable(cboot_bench bench/bench.cpp)
target_link_libraries(cboot_bench PRIVATE cboot_web)

# ── negative：预期编译失败，不进默认构建 ────────────────────────────────
# 开启方式：cmake -B build-neg -DCBOOT_BUILD_NEGATIVE_TESTS=ON
option(CBOOT_BUILD_NEGATIVE_TESTS "Build negative (expected-fail) diagnostics demos" OFF)
if(CBOOT_BUILD_NEGATIVE_TESTS)
    add_executable(cboot_neg_cycle negative/negative_cycle.cpp)
    target_link_libraries(cboot_neg_cycle PRIVATE cboot_core)

    add_executable(cboot_neg_missing_dep negative/negative_missing_dep.cpp)
    target_link_libraries(cboot_neg_missing_dep PRIVATE cboot_core)

    add_executable(cboot_neg_private_ctor negative/negative_private_ctor.cpp)
    target_link_libraries(cboot_neg_private_ctor PRIVATE cboot_core)

    add_executable(cboot_neg_controller_no_bean negative/negative_controller_no_bean.cpp)
    target_link_libraries(cboot_neg_controller_no_bean PRIVATE cboot_app)

    add_executable(cboot_neg_ambiguous_ctor negative/negative_ambiguous_ctor.cpp)
    target_link_libraries(cboot_neg_ambiguous_ctor PRIVATE cboot_core)

    add_executable(cboot_neg_named_ambiguous negative/negative_named_ambiguous.cpp)
    target_link_libraries(cboot_neg_named_ambiguous PRIVATE cboot_core)

    add_executable(cboot_neg_value_type negative/negative_value_type.cpp)
    target_link_libraries(cboot_neg_value_type PRIVATE cboot_core)

    add_executable(cboot_neg_event_listener negative/negative_event_listener.cpp)
    target_link_libraries(cboot_neg_event_listener PRIVATE cboot_core)

    add_executable(cboot_neg_scheduled negative/negative_scheduled.cpp)
    target_link_libraries(cboot_neg_scheduled PRIVATE cboot_core cboot_schedule)

    add_executable(cboot_neg_cron negative/negative_cron.cpp)
    target_link_libraries(cboot_neg_cron PRIVATE cboot_core cboot_schedule)

    add_executable(cboot_neg_schedule_both negative/negative_schedule_both.cpp)
    target_link_libraries(cboot_neg_schedule_both PRIVATE cboot_core cboot_schedule)

    add_executable(cboot_neg_component_cycle negative/negative_component_cycle.cpp)
    target_link_libraries(cboot_neg_component_cycle PRIVATE cboot_app)

    add_executable(cboot_neg_component_global_bean negative/negative_component_global_bean.cpp)
    target_link_libraries(cboot_neg_component_global_bean PRIVATE cboot_app)

    add_executable(cboot_neg_component_bad_dep negative/negative_component_bad_dep.cpp)
    target_link_libraries(cboot_neg_component_bad_dep PRIVATE cboot_app)
endif()

set_tests_properties(web_server_smoke PROPERTIES TIMEOUT 30)
set_tests_properties(
    context_smoke named_smoke value_smoke profile_smoke event_smoke
    scheduled_smoke schedule_expr_smoke component_smoke health_smoke
    json_smoke config_smoke web_routing_smoke smoke_reflect class_smoke
    aop_smoke autoscan_smoke async_smoke backend_select_smoke
    backpressure_smoke backpressure_iocp_smoke
    handler_timeout_smoke handler_timeout_iocp_smoke
    PROPERTIES TIMEOUT 30)
