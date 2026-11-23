/**
 * @file app.cppm
 * @brief 应用装配 — 组件选择与入口配置
 *
 * 各层 Bean 位于全局命名空间；本模块 export import 各层，
 * 使入口 TU 的自动扫描可以看到它们。
 */
export module todo.app;

import std;
import cboot.app;               // application / type_list
import cboot.auth.postgres;     // auth_postgres_component（含 postgres_component）
import cboot.meta.reflect;      // type_list
export import todo.api.auth;
export import todo.api.errors;
export import todo.api.page;
export import todo.api.task;
export import todo.repository;
export import todo.service;
export import todo.setup;

/** @brief 全栈待办应用 */
export class App : public application<App> {
public:
    static constexpr std::string_view config_path = "app.json";
    static constexpr unsigned short default_port = 8090;
    using components = type_list<auth_postgres_component>;
};
