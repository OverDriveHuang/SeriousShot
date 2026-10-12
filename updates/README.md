# 更新清单

最后更新：2026-10-12 22:43:10 CST

`testing.json` 用于自动更新测试，只有编译时选择 testing 渠道并内置相应公钥的 App 会读取它。测试签名 key ID 为 `testing-20261012`，公钥见 `testing.public`。

本轮使用本地 0.1.4 测试 App 检查并下载 0.1.5。两个版本均保持 testing 渠道；Mac 点击安装后打开 DMG，退出旧 App 并拖拽替换，再从原路径启动，设置页显示 0.1.5。

本轮没有部署 `stable.json`。GitHub Latest 保留 0.1.4；0.1.5 Release 标注为更新测试。testing 清单仍只接受非 draft、非 pre-release 的 Release。

清单由发布工具签名生成。直接编辑版本、下载地址或摘要会使签名失效。
