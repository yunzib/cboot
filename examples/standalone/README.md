# cboot 独立应用示例

使用 Release 安装的 cboot SDK，通过 `find_package(cboot)` 构建，不依赖源码树。

示例代码、CMakeLists、配置文件均在本目录：`main.cpp` / `CMakeLists.txt` / `app.json`。

## 1. 安装 SDK

在仓库根目录：

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
cmake --install build-release --prefix build-release/install
```

## 2. 构建独立应用

```bash
cmake -S examples/standalone -B examples/standalone/build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build-release/install" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="<g++16 路径>"
cmake --build examples/standalone/build -j
```

`find_package(cboot)` 由包配置自动注入 `-freflection`、`cxx_std_26` 与运行时链接项；
消费方只需在 `project()` 之前设置 `CMAKE_CXX_MODULE_STD` /
`CMAKE_CXX_SCAN_FOR_MODULES` / `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`（见 CMakeLists.txt）。

## 3. 运行

配置路径是相对工作目录的 `app.json`，因此从本目录启动：

```bash
cd examples/standalone
./build/cboot_example_standalone                     # 8080
./build/cboot_example_standalone --server.port=9000  # 覆盖端口
```

Windows 下可执行文件为 `build/cboot_example_standalone.exe`。

```bash
curl http://127.0.0.1:8080/api/users/42
curl -X POST -d '{"name":"bob","age":30}' http://127.0.0.1:8080/api/users
curl http://127.0.0.1:8080/api/config
curl http://127.0.0.1:8080/api/page-size
curl "http://127.0.0.1:8080/api/health?verbose=1"
```

`app.json` 中的 `app.page_size` 通过 `[[=Value{"app.page_size"}]]` 注入 `UserService`；
`db.*` 通过 `[[=Config{"db"}]] DbProps` 绑定；`RequestLog` 作为全局切面记录每次路由调用。
