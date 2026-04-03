# Irrlicht FFT 可视化系统：编译与稳定性技术报告 (v2.0)

本报告详细记录了在开发基于 WASAPI 的 32 频段 FFT 音频可视化系统过程中遭遇的多维冲突、底层驱动故障及其对应解决方案。

---

## 📋 编译故障深度诊断 (Compilation Post-Mortem)

### 1. 致命错误：隐式函数声明 (Implicit Declaration)
*   **现象**：编译器报错 `error: implicit declaration of function 'fft'` 和 `wasapi_get_recent_pcm`。
*   **根因**：C 语言遵循严格的“先声明后使用”原则。在重构过程中，底层 FFT 算法和 PCM 获取函数的定义被放置在了调用点之后，导致编译器在解析 `wasapi_get_fft` 时由于不具备函数原型而中断流程。
*   **修复**：在 `wasapi_bridge.c` 顶部注入了显式的函数原型声明（Forward Declarations），建立了正确的符号查找路径。

### 2. 语法干扰：变量阴影 (Variable Shadowing)
*   **现象**：编译器警告 `count` 变量重名。
*   **根因**：在 `wasapi_get_fft` 函数体内，循环局部的变量名与全局状态变量重名，在严苛的 C99 编译设置下引发了歧义。
*   **修复**：将循环内部计数器重命名为 `binCount`，实现了逻辑隔离。

### 3. 自动化陷阱：Makefile 变量与路径保护
*   **现象**：`mingw32-make` 无法定位编译器或路径报错。
*   **根因**：
    -   Makefile 中未显式定义 `CXX`，导致系统调用默认且不兼容的 `g++`。
    -   `copy` 指令直接依赖路径，当 `bin` 目录不存在时导致 Windows 文件操作报错。
*   **修复**：强制锁定 `i686-w64-mingw32-g++` 编译器，并为 `all_win32` 目标加入了路径保护逻辑（`if not exist mkdir`）。

### 4. 运行时冲突：资源锁定 (Permission Denied)
*   **现象**：`01.MyHelloWorld.exe: Permission denied`。
*   **根因**：由于程序正在运行中，系统内核锁定了 `.exe` 文件，阻断了编译器的写入。
*   **修复**：在构建指令中集成了 `taskkill /F` 操作，并辅以 `ExitProcess(0)` 强力退出锁，实现了“快速清库、即时发布”的流程。

---

## 💡 运行时稳定性与视觉优化

### 2.1 WASAPI 驱动同步超时
驱动层将 `WaitForSingleObject` 等待时间从 50ms 提升至 **200ms**，为 COM 对象（`IAudioClient`）释放提供充足窗口。

### 2.2 FFT 归一化与视觉灵敏度
*   **归一化**：应用 `mag = sqrt(...) / FFT_SIZE` 解决“顶边”问题。
*   **动态配置引擎**：引入 `settings.cfg`。你可以通过外部配置表动态调整 `VisualMultiplier`、`Smoothness` 和 `Gain`，实现零编译实时调优。

---
*修订日期：2026-04-03*
*贡献者：Antigravity & 用户深度复盘*
