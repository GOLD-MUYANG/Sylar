# module_lifecycle 基线失败排查

## 现象

在仅构建 `test_ai_gateway_servlet` 与 `test_http_load_balance_client` 后运行
`ctest --test-dir build -L unit --output-on-failure`，`module_lifecycle` 找不到
`lifecycle/1.0` 插件。

## 使用的命令与原因

- `ctest --test-dir build -L unit --output-on-failure`：运行当前 worktree 的 unit 基线，定位失败测试。
- `rg -n -C 4 'module_lifecycle|add_library\\(module|LIBRARY_OUTPUT_DIRECTORY|module.path' CMakeLists.txt tests/test_module_lifecycle.cc`：核对测试配置的插件路径及 CMake 目标输出目录。
- `ls -l bin/module`：确认失败时插件输出目录为空。
- `rg -n -C 3 'test_module_lifecycle_plugin|add_dependencies\\(test_module_lifecycle' build/CMakeFiles/Makefile2 build/Makefile`：核对生成的构建依赖图。

## 结论

失败来自基线构建范围不完整，而非 G3 或既有业务代码：
`test_module_lifecycle` 的 CMake 依赖 `test_module_lifecycle_plugin`，但前一条只构建了两个无关测试目标，
因此插件 `.so` 尚未生成。后续使用完整 `cmake --build build` 后再运行 unit 基线即可验证。
