# Poe2Study

Windows x64 平台上的 Path of Exile 2 C++ 源码学习项目，包含地图与寻路算法、实体与队伍信息读取、多窗口界面、命名管道通信，以及 Monitor DLL 的动作接口。

维护署名：**XiaoLaCoding**。

## 开发环境

- Windows x64
- Visual Studio C++ 工具集 **v145**、Windows SDK
- Python 3（构建脚本及测试入口）
- Git、PowerShell

主解决方案为 `plug/Poe2Study.sln`。x64 配置使用 C++17。

## 构建

在仓库根目录运行：

```powershell
.\plug\tools\build.ps1
```

该脚本执行架构与编码检查，并构建 Plug、Monitor、NavGUI。产物位于 `plug/bin/Release/`，中间文件位于 `plug/.build/`。构建默认传入 `/p:AutoInject=false`；加载 DLL 是界面中的独立操作。

```powershell
# Debug 构建
.\plug\tools\build.ps1 -Configuration Debug
# 只构建指定工程
.\plug\tools\build.ps1 -Project NavGUI
```

`NavGUI.exe`、`Plug.exe`、`Monitor.dll` 与生成的配置文件在同一个输出目录中使用。源码模板为 `plug/Plug/config.ini` 和 `plug/Plug/recovery.ini`。本仓库只发布源码，不附带运行包或调试符号。

## 目录

| 路径 | 内容 |
| --- | --- |
| `plug/Common/` | 共享偏移、协议、内存访问与算法 |
| `plug/Plug/` | 命令行、界面与用例层 |
| `plug/Monitor/` | DLL、命名管道服务与动作接口 |
| `plug/NavGUI/` | GUI 工程与图标资源 |
| `plug/tools/` | 构建、架构和编码检查 |
| `plug/tests/` | 回归源码及研究工具 |
| `plug/third_party/` | 第三方源码及原始许可声明 |

## 验证

```powershell
python -X utf8 .\plug\tools\check_arch.py --strict
python -X utf8 .\plug\tools\check_encoding.py
python -X utf8 .\plug\tests\run_navigation_tests.py
python -X utf8 .\plug\tests\run_monitor_lifecycle_test.py --dll .\plug\bin\Release\Monitor.dll
```

导航回归使用合成数据，DLL 生命周期测试使用隔离进程。它们不能替代游戏内实际行为验证。游戏更新后，版本指纹与偏移需要重新核对，源码中的旧版本数值不代表持续兼容。

`plug/tests/camera_map/research.py` 的输入镜像需通过 `--image` 显式指定。

可选的离线 PE 研究脚本依赖 NumPy、pefile 和 Capstone，可在独立虚拟环境中安装（主工程构建不需要这些包）：

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -r requirements-research.txt
```

## 隐私与贡献

提交代码、日志或 Issue 前，请移除个人路径、账号、角色资料、密钥及机器信息。`.gitignore` 已排除个人环境文件、会话数据、原始取证数据、日志、备份、编译产物与调试符号；配置模板可以公开，本机配置不要直接覆盖模板后提交。

历史运行记录和取证附件未随源码发布。维护约定见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

本项目新增内容以 [MIT License](LICENSE) 发布。第三方内容遵循各自许可证，原始版权声明保留不变，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
