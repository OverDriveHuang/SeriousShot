# 固定标注字体

最后更新：2026-10-29 23:45:41 CST

`NotoSansSC-VF.ttf` 是 SeriousShot 文字标注的固定字体。已提交文字的预览、PNG 和 JPEG 消费同一个 `TextRasterizerPort` 生成的灰度 coverage；输入框由 Qt 绘制同一字体的显式变量实例。字体加载失败时必须报错，不得回退到系统字体后继续导出。

- 上游：`notofonts/noto-cjk`
- 固定提交：`f8d157532fbfaeda587e826d4cd5b21a49186f7c`
- 上游路径：`Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf`
- 字体 SHA-256：`d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964`
- 许可证：SIL Open Font License 1.1；原文保存在 `OFL.txt`
- 许可证 SHA-256：`6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`

生产配置由 `src/ports/annotation_font_contract.hpp` 固定：`wght=450`、灰度 coverage、禁用自动 hinting。FreeType 测量与绘制均使用 `FT_LOAD_NO_AUTOHINT`，保留现有物理字号取整、整数 advance 和行高计算。小字号中文在旧 `FT_LOAD_DEFAULT` 下进入自动 hinting，24px“在”的底横出现三行全覆盖；当前配置保留一行全覆盖与两侧抗锯齿边缘。

Qt 输入态使用同一 family，显式 `setVariableAxis("wght",450)`，请求 `PreferNoHinting`，并设置 `PreferAntialias | NoFontMerging`。单独 `setWeight(450)` 在本机 Windows Qt 上匹配到 Regular 400，不能代替变量轴。Qt 的原生字体后端与 FreeType 独立栅格器仍可能产生不同像素；hinting preference 的效果需要分别验证各平台，不能把 Windows 离屏结果视为 macOS 验收。

项目通过 Qt 的 `Qt6::FreetypePrivate` / `Qt6::BundledFreetype` 构建。Windows Qt 6.11.2 SDK 的 `FT_Library_Version` 于 2026-10-29 实测为 **2.14.2**；SDK 的 SBOM 标记 2.14.3，与运行时报告不同，不能将 SBOM 版本当作运行时实测。此前 Qt 6.11.1 SDK 也实测为 2.14.2。当前 Windows golden：`"HDR 截图"`、20 pt、2 px/pt、320×64 mask 的 FNV-1a 64 为 `2678395914446492182`。底横形状、九字号与四尺度边界回归另外验证修复，不只依赖该 hash。

`tools/run_text_raster_probe.ps1` 使用本地已有 MSVC/Qt，每次在 `build/text-raster-probe-results/` 写入独立时间戳目录，生成前后对照。紧贴 nominal 行高的混排 mask 存在既有 descender 边界限制；产品文字框的最小高度和 padding 已在四尺度回归覆盖，本次不改变字框布局。
