# 从源码构建 SeriousShot

最后更新：2026-09-21 08:55:45 CST

[返回产品介绍](../README.md)

当前使用 C++20、CMake 与 Qt 6.11.1。第三方源码按 CMake 中固定提交获取，首次构建需要网络。以下是开发构建，不是已完成签名、部署与发行审查的安装包。

## macOS

需要 macOS 15.5 起、Xcode Command Line Tools、Python 3 和 Git。若已安装项目记录的 Qt SDK，无需重复下载。

```sh
./scripts/bootstrap_macos_dependencies.sh
.build-tools/cmake-venv/bin/cmake --preset mac-qt-release
.build-tools/cmake-venv/bin/cmake --build --preset mac-qt-release -j 6
.build-tools/cmake-venv/bin/ctest --preset mac-qt-release --output-on-failure
```

应用产物为 `build-qt/SeriousShot.app`。首次运行需要屏幕录制授权。使用自己的稳定开发签名；`scripts/sign_macos_development_app.sh` 接受 `HDRSHOT_CODESIGN_SHA1` 和 `HDRSHOT_CODESIGN_NAME`，默认身份仅属于原开发环境，不包含私钥，不能在其他机器直接使用。更新前正常退出旧实例：重建磁盘上的 App 不会替换正在运行的进程。

不启用 Qt 宿主可使用 `mac-release` preset。原生和图形测试有相应环境要求；部分样图回归依赖外部 fixture，缺失时跳过不代表通过。macOS 15.5 外接 HDR 截图不受支持，详见首页系统支持。

## Windows

需要 Windows 11、MSVC x64 / Windows SDK、CMake、Ninja 和 Qt 6.11.1。Qt 安装脚本需要 Python 的 requests 与 py7zr。将 zlib 1.3.1 的源码和安装结果分别准备至 `.build-tools/zlib/src` 与 `.build-tools/zlib/install`，或按本机环境调整 preset 的 `ZLIB_ROOT`。

```powershell
. ./scripts/windows-dev-environment.ps1
python scripts/install-windows-qt.py
./scripts/build-windows.ps1
```

Windows 分析器接入仍待完成；Mac 共享测试通过不能替代 Windows 原生验证。

## 代码与分发

`src/domain`、`src/application` 为共享逻辑；`src/platform` 为平台适配器，`src/ui` 为 Qt 界面，`tests` 为模块回归。既有 `hdrshot_*` 目标和命名空间保留，可见产品名称为 SeriousShot。

`artifacts/2026-09-19_analyzer_interaction_fixes/checks/interaction_probe.mm` 是 CMake 引用的合成输入探针，不包含用户截图。公开快照的逐文件校验与来源提交记录在 `.seriousshot-export.json`。

正式分发前还需要框架部署、签名和许可审查。项目主许可证仍未指定；第三方声明见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
