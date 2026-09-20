# 固定标注字体

最后更新：2026-08-29 17:20:40 CST

`NotoSansSC-VF.ttf` 是 HDRShot MVP 文字标注的权威栅格字体。Qt 预览与 PNG 导出必须通过同一个 `TextRasterizerPort` 和 FreeType 配置消费该文件，不得回退到系统字体后继续导出。

- 上游：`notofonts/noto-cjk`
- 固定提交：`f8d157532fbfaeda587e826d4cd5b21a49186f7c`
- 上游路径：`Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf`
- 字体 SHA-256：`d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964`
- 许可证：SIL Open Font License 1.1；原文保存在 `OFL.txt`
- 许可证 SHA-256：`6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`

字体使用 FreeType 2.14.3、grayscale coverage、`FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT` 栅格化。变量字体 `wght` 轴必须显式固定为 Regular 400，Qt 输入态也固定为相同 family/weight；不得依赖字体或框架的隐式默认实例。当前 golden：`"HDR 截图"`、20 pt、2 px/pt、320×64 mask 的 FNV-1a 64 为 `11504789224222780783`。
