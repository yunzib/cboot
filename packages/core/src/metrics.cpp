/**
 * @file metrics.cpp
 * @brief cboot.core.metrics 实现单元 — 进程级注册表单例
 *
 * 为什么需要实现单元：`inline` 函数里的函数局部静态在模块跨 TU 的
 * Release 构建中可能被不同 TU 各自实例化，导致 web/ORM 记录的指标与
 * /metrics 读取的注册表分裂（Debug 侥幸可见）。单例定义放在这里，
 * 全进程只有一个定义。
 */
module cboot.core.metrics;

namespace metrics {

registry& global() {
    // 函数局部静态：首次调用时构造，进程退出时销毁（C++11 起线程安全）
    static registry instance;
    return instance;
}

} // namespace metrics
