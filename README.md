# SeriousShot

最后更新：2026-09-20 23:11:25 CST

面向 HDR / 广色域桌面的截图、标注与图像分析工具。共享核心使用 C++20，界面使用 Qt；平台后端分别处理原生捕获和呈现。

## 当前能力

- 截图选区与标注、保存/另存/复制；Display P3 SDR 与 PQ HDR PNG，以及 JPEG / Ultra HDR JPEG 输出。
- Mac 分析器：Source Signal、Waveform / Parade、Histogram、Vectorscope、采样色块、Mask、False Color、分析图保存与复制。
- 四种分析工作空间，分析参考白、Blur / Gain；图表缩放和平移，工作色域虚线轮廓，均值与区域分布高亮。
- 共享领域/应用逻辑、CPU 参考实现与测试；macOS 使用 ScreenCaptureKit / Metal，Windows 包含原生截图后端。

Mac 分析器当前阶段已获用户试用接受。Windows 分析器接入、各系统/显示器/消费者的完整验证以及正式签名发行仍需独立完成；本仓库不宣称全平台发行验收通过。

## macOS 开发构建

最低目标版本 macOS 15.5。需要 Xcode Command Line Tools、Python 3 和 Git。当前配置使用 Qt 6.11.1；第三方源码按 CMake 中固定提交获取，首次构建需要网络。

```sh
./scripts/bootstrap_macos_dependencies.sh
.build-tools/cmake-venv/bin/cmake --preset mac-qt-release
.build-tools/cmake-venv/bin/cmake --build --preset mac-qt-release -j 6
.build-tools/cmake-venv/bin/ctest --preset mac-qt-release --output-on-failure
```

应用构建产物为 `build-qt/SeriousShot.app`。首次运行需要系统屏幕录制授权。使用自己的稳定开发签名；`scripts/sign_macos_development_app.sh` 接受 `HDRSHOT_CODESIGN_SHA1` 和 `HDRSHOT_CODESIGN_NAME`，其默认身份仅属于原开发环境，不包含私钥，也不能在其他机器直接使用。更新应用前正常退出旧实例；重建磁盘上的 App 不会替换正在运行的进程。正式分发还需框架部署、签名与许可审查，不将开发构建直接当发行包。

仅共享核心可用 `mac-release` preset；原生/图形测试有相应环境要求。部分样图回归依赖外部 fixture，缺失时跳过不代表通过。

## Windows 开发构建

目标 Windows 11、MSVC x64 / Windows SDK、CMake、Ninja、Qt 6.11.1。Qt 安装辅助脚本为 `scripts/install-windows-qt.py`（需 Python 的 requests 和 py7zr）。将 zlib 1.3.1 的源码和安装结果分别准备至 `.build-tools/zlib/src` 与 `.build-tools/zlib/install`，或按本机环境调整 preset 的 `ZLIB_ROOT`。

```powershell
. ./scripts/windows-dev-environment.ps1
python scripts/install-windows-qt.py
./scripts/build-windows.ps1
```

Windows 验证需要在真实目标机进行；Mac 上通过共享测试不能替代 Windows 原生验证。

## 源码布局

| 目录 | 内容 |
|---|---|
| `src/domain`、`src/application` | 平台无关的数据、算法、用例及端口 |
| `src/platform` | CPU、macOS、Windows 适配器 |
| `src/ui` | 共享 Qt 界面 |
| `tests` | 模块与回归测试 |
| `cmake`、`assets`、`scripts`、`tools` | 构建、必要资源及诊断工具 |

`artifacts/2026-09-19_analyzer_interaction_fixes/checks/interaction_probe.mm` 是构建引用的合成输入交互探针，不包含用户截图。仓库保留既有 `hdrshot_*` 构建目标和命名空间；可见产品名称为 SeriousShot。

## 许可与来源

项目主许可证尚待作者确定；本次公开源码没有新增 MIT、Apache 或其他主许可证。第三方组件仍遵循各自许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 及资源目录内的原始许可文件。不要将仓库可见性解释为已授予某种尚未选择的许可。

本仓库接收经审查的项目源码快照；`.seriousshot-export.json` 记录来源提交与逐文件校验值，不包含私有工作区历史。修改建议可使用 issue / pull request；维护者需先协调两端源码，避免覆盖公开仓独立修改。
