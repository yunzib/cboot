# packages/ — 同级包

所有包在目录和构建图上平级。目录分组不代表包含关系，依赖只由
`target_link_libraries` 和组件描述符的 `dependencies` 表达。

## 两类包

| 类型 | 定义方式 | 是否参与 `components` 选择 |
|---|---|---|
| 基础包 | `cboot_package(...)` | 否；随 `cboot` 一起安装导出 |
| 契约组件 | `cboot_component(...)` | 是；独立包 `cboot.<name>` |

基础包：`meta`、`json`、`config`、`core`、`aop`、`async`、`web`、`app`。
契约组件：`schedule`（固定延迟 / cron，贡献 hook，不贡献 bean）。
`core` 特殊在职责（注解/DI 容器）和依赖位置，不是其他包的父目录。

## 包内布局

```text
packages/<name>/
├── CMakeLists.txt
├── <module>.cppm          # 模块接口单元
├── src/                   # 实现单元（可选）
├── test/                  # 包内测试（可选）
└── README.md
```

模块名保持 `cboot.<name>`；target 为 `cboot_<name>`，别名 `cboot::<name>`。

跨包回归测试在根 `test/`；包内冒烟测试放在 `packages/<name>/test/`，
通过 `TEST_SOURCES` 注册。

## 新增基础包

```cmake
# packages/<name>/CMakeLists.txt
cboot_package(<name>
    MODULES <module>.cppm
    SOURCES src/<module>.cpp
    DEPENDS <direct-package>...
)
```

然后在 `packages/CMakeLists.txt` 按依赖顺序加一行。

## 新增契约组件

```cmake
# 独立构建时先找 cboot，并复用安装的 helper
find_package(cboot REQUIRED)
include("${cboot_CMAKE_DIR}/cbootComponent.cmake")

# packages/<name>/CMakeLists.txt
cboot_component(<name>
    VERSION 0.1.0
    MODULES <module>.cppm
    SOURCES src/<module>.cpp
    DEPENDS core config
    PACKAGE_DEPENDS cboot.<dependency-component>
    TEST_SOURCES test/<name>_smoke.cpp)
```

契约组件的 bean 必须放在组件自己的 namespace，描述符 `beans` /
`dependencies` 见 [docs/契约组件.md](../docs/契约组件.md)。
