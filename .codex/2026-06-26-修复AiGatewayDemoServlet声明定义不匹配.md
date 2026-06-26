# 修复 AiGatewayDemoServlet 声明定义不匹配

## 排查命令

1. `sed -n '1,160p' modules/ai_gateway/ai_gateway_demo_servlet.h`
   - 原因：查看类声明中的构造函数签名，确认头文件是否声明了实现文件里的构造函数。

2. `sed -n '1,180p' modules/ai_gateway/ai_gateway_demo_servlet.cc`
   - 原因：查看 out-of-line 构造函数定义，和头文件声明逐字对比。

3. `rg -n "AiGatewayDemoServlet\\(" modules tests`
   - 原因：确认是否存在其他同名声明、调用点或旧构造函数签名。

4. `cmake --build build --target test_ai_gateway_demo_servlet ai_gateway_module`
   - 原因：用真实编译判断这是编译错误还是 IDE/clangd 诊断；结果显示编译通过。

5. `rg -n "ai_gateway_demo_servlet.cc|ai_gateway_demo_servlet.h" build/compile_commands.json CMakeLists.txt`
   - 原因：确认该源文件在 CMake 和 compile_commands.json 中有正确编译入口。

## 结论

真实编译通过，说明原始代码在编译器层面可用；IDE 报错更可能来自 clangd 对默认参数构造函数的缓存或解析不稳。为降低误报和提高接口可读性，把构造函数拆成两个明确声明：

- `AiGatewayDemoServlet()`
- `AiGatewayDemoServlet(const std::string &html_path)`

实现文件分别给出两个 out-of-line 定义，默认构造函数委托到指定路径构造函数。
