# 从源码构建 SeriousShot

最后更新：2026-10-29 13:15:32 CST

[返回产品介绍](../README.md)

当前使用 C++20、CMake；macOS 使用 Qt 6.11.2（含 QTBUG-147602 修复），Windows 保持 Qt 6.11.1。第三方源码使用固定提交或版本与 SHA-256 校验；更新模块另需 libsodium 1.0.22 和 libarchive 3.8.9，由项目脚本准备在本地 `.build-tools/`，首次构建需要网络。以下是开发构建，不是已完成签名、部署与发行审查的安装包。

## macOS

需要 macOS 15.5 起、Xcode Command Line Tools、Python 3.9 或更高版本和 Git。若已安装项目记录的 Qt SDK，无需重复下载。

```sh
./scripts/bootstrap_macos_dependencies.sh
python3 scripts/build_update_dependencies.py
python3 scripts/build-macos.py
```

应用产物为 `build-qt/SeriousShot.app`。首次运行需要屏幕录制授权。使用自己的稳定开发签名；`scripts/sign_macos_development_app.sh` 接受 `HDRSHOT_CODESIGN_SHA1` 和 `HDRSHOT_CODESIGN_NAME`，默认身份仅属于原开发环境，不包含私钥，不能在其他机器直接使用。更新前正常退出旧实例：重建磁盘上的 App 不会替换正在运行的进程。

统一脚本固定执行配置、完整构建和串行CTest，并将每次的日志、JUnit与`summary.json`保存到`.agent/logs/macos-<日期时间>-<标识>/`。可用`--build-dir <目录>`选择独立构建目录；构建与测试使用同一目录。退出码0表示已注册的必需检查完成，1表示失败，2表示必需检查未完成；可选外部样图缺失与最终屏幕像素未验会另行列出，不能把退出码0扩大解释为所有人工／设备矩阵均已验证。脚本不启动App。原始`cmake --build`仍只编译；日常验证使用上述统一入口。

不启用 Qt 宿主可使用 `mac-release` preset。原生和图形测试有相应环境要求；固定PQ图片的libuhdr误差专项按已验证输入决定是否运行；需要执行时路径未配置或不可读会报告未完成。macOS 15.5 外接 HDR 截图不受支持，详见首页系统支持。

## Windows

需要 Windows 11、MSVC x64 / Windows SDK、CMake、Ninja 和 Qt 6.11.1。需要 Python 3.9 或更高版本；Qt 安装脚本还需要 requests 与 py7zr。将 zlib 1.3.1 的源码和安装结果分别准备至 `.build-tools/zlib/src` 与 `.build-tools/zlib/install`，或按本机环境调整 preset 的 `ZLIB_ROOT`。

```powershell
. ./scripts/windows-dev-environment.ps1
python scripts/install-windows-qt.py
./scripts/build-windows.ps1
```

Windows 已接入原生分析器、命令行截图，以及 Legacy／ACM／HDR 颜色处理。Windows 10 21H2 在同时启用 scRGB gain=0.5 与“HDR 捕获不使用 SDRWhiteLevel 归一化”的已测条件下，HDR 和 Legacy 广色域截图均未发现问题；完整条件见首页实测矩阵。Windows 更新器已验证 89 个原生事务场景，包括真实进程退出、原目录替换、文件占用、回滚和中断恢复；本机 66 个 CTest 用例通过。隔离 testing 构建可显式开启原生自动覆盖，最终用户安装试用仍需按测试包操作验证。部分 Windows 10 环境截图偏亮或发浅时，可尝试开启设置中的“Windows 10 截图亮度兼容”。总开关默认关闭，全选时启用 scRGB gain 和“HDR 捕获不使用 SDRWhiteLevel 归一化”两个独立子项，只启用其一时显示半选；scRGB gain 可编辑，预设 0.5，仅启用该子项后生效。应按实际截图结果判断是否开启，不能推断所有 Windows 10 都需要调整。已测试的 Windows 11 环境未见上述现象。

## 代码与分发

设置页显示产品版本及本包源代码的最后提交时间（UTC），并提供更新入口。更新检查需要构建时配置可信的更新目录签名公钥；默认开发配置未提供正式公钥时，自动更新不可用，仍可打开GitHub发布列表。同一代码快照重新编译不会改变此时间。版本来自CMake的`PROJECT_VERSION`，发布时才修改；诊断资料仍可记录独立的构建时间。

构建时校验实际输入与代码身份；改动相关代码会显示“含未提交修改”。缺少 Git 历史或有效来源清单时显示未知，不以编译时间补齐。带有效清单的源码归档不依赖 Git，也会校验输入文件散列。旧版清单缺少代码身份时显示未知。

正式分发配置时额外指定`-DHDRSHOT_RELEASE_BUILD=ON`，构建会拒绝来源未知或相关输入有修改的包。该开关独立于`CMAKE_BUILD_TYPE=Release`；普通本地优化构建不自动视作发行包。

`src/domain`、`src/application` 为共享逻辑；`src/platform` 为平台适配器，`src/ui` 为 Qt 界面，`tests` 为模块回归。既有 `hdrshot_*` 目标和命名空间保留，可见产品名称为 SeriousShot。

仓库包含 CMake 引用的合成输入探针，不包含用户截图。公开快照的逐文件校验记录在 `.seriousshot-export.json`。

正式分发前还需要框架部署、签名和许可审查。项目主许可证仍未指定；第三方声明见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

更新器构建选项：正式渠道使用 `HDRSHOT_UPDATE_CHANNEL=stable` 与正式 `HDRSHOT_UPDATE_PUBLIC_KEY`、`HDRSHOT_UPDATE_KEY_ID`；测试渠道须显式启用 `HDRSHOT_UPDATE_TESTING` 并配置独立测试公钥。私钥不得编入客户端。`HDRSHOT_WINDOWS_UPDATE_NATIVE_VALIDATED` 默认关闭，只有完成 Windows 原生更新事务验收后才启用自动覆盖；macOS 当前使用 DMG 安装路径。Windows 构建脚本已调用更新依赖准备脚本。

更新清单签名、版本规则及平台启用条件见 [软件更新协议](UPDATES.md)。

## 默认测试范围

核心颜色管理、元数据传递、显示参数／刷新、保存复制、设置实际生效及产品性能回归每次执行。自动更新测试允许复用同一构建目录中已验证的输入；文件内容／公共依赖／配置／实际测试程序／Qt动态库变化或依据缺失时仍运行。Mac使用`--force-updates`、Windows使用`-ForceUpdateTests`强制复跑；两端打包均强制更新测试。结果汇总会列出本次运行与已验证输入复用，不能混称本次全跑。

Mac的生产性能命令已接入默认CTest，原始输出、结构化测量和固定基线比较保存在构建目录`performance/`，每次使用独立时间戳；既有断言仍生效，首次测量和无既定阈值的变化不等于性能无退化。Windows保留现有连续标注性能门禁。原始CTest仍运行全部已注册测试，不做自动更新复用；正常构建入口统一使用选择和结果汇总脚本。

未采用的macOS ImageIO PNG编码候选方案不在默认测试中；只有评估该方案时，用独立构建目录配置`-DHDRSHOT_TEST_IMAGEIO_PNG_CANDIDATE=ON`，再运行`ctest -R '^hdrshot_macos_imageio_png_contract$'`。生产PNG编码／压缩测试保持必测，UHDR真实图片专项缺失依然明确记录。


固定 PQ 图片的 libuhdr 编码误差专项：Mac 使用 `--uhdr-fixture /absolute/path/to/sample.PNG` 指定图片并运行，通过后路径记在该构建目录；以后依赖／实际测试程序、配置、系统或样图变化自动复跑，同输入通过则省略。`--force-uhdr-fixture` 强制复跑已配置的图片；也支持 `HDRSHOT_ULTRAHDR_FIXTURE` 环境变量提供路径。首次或记录损坏时需完成一次，不把未配置路径写成图片不存在。上述参数适用于 `scripts/build-macos.py` 以及已完成构建后的 `scripts/test_execution_policy.py`。原始 CTest 命令不使用这些复用规则。
