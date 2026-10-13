# 软件更新协议

最后更新：2026-10-13 22:45:00 CST

客户端通过 GitHub 托管的签名清单选择本平台的新版本，安装包保存在 Release 附件。正式源为：

```text
https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/stable.json
```

更新支持需要在构建中配置可信的 `HDRSHOT_UPDATE_PUBLIC_KEY` 和 `HDRSHOT_UPDATE_KEY_ID`。公钥可公开；私钥只用于签发清单，不能放入仓库或客户端。未配置可信公钥时，更新器不会接受远端更新。第一个包含更新器的版本需要手工安装。

正式公钥见 [`updates/stable.public`](../updates/stable.public)，key ID 为 `stable-20261013`。Mac 0.1.5 是首个包含正式更新信任的版本。

## 版本与发行状态

版本格式为三个以点分隔的非负整数，不允许前导零、溢出或预发布后缀。按主版本、次版本、修订号依次比较数值。只接受 GitHub Release 的 `draft=false`、`prerelease=false`；Latest 标记不替代这两个条件。Mac与Windows可以指向不同的正式Release。

清单使用 `schema_version=1` 的 envelope，字段为 `key_id`、`payload_base64`、`signature_base64`。Ed25519签名覆盖固定前缀 `SeriousShot.UpdateCatalog.v1` 加解码后的payload原字节。客户端先验签再解析，不能在签名后手工改版本或链接。

payload 包含 `product=SeriousShot`、`channel`、递增的目录 `revision` 和 `targets`。每条目标包含平台、架构、三段版本、最低系统、Release tag/ID、附件 ID/名称/永久直链、大小、SHA-256、正式发布状态和安装协议版本。目录revision用于阻止旧目录回放，与软件三段版本号分别维护。

客户端下载前还核对GitHub Release的实际状态及附件身份；下载完成后核对大小和SHA-256。更新清单签名在Windows和Mac共用，不能代替Apple代码签名、公证或Windows Authenticode，也不会绕过系统安全判断。

## 构建和签名工具

`hdrshot_update_catalog_tool` 提供本地签名和验证：

```text
hdrshot_update_catalog_tool sign PAYLOAD_JSON SECRET_KEY KEY_ID NEW_OUTPUT
hdrshot_update_catalog_tool verify ENVELOPE PUBLIC_HEX_FILE KEY_ID CHANNEL
```

私钥文件为64字节原始Ed25519私钥；公钥文件为32字节公钥的64位hex文本。`generate-test-key`仅为隔离测试生成密钥；测试信任不能直接用作正式信任。

测试目录为 `updates/testing.json`，需要显式隔离构建；正式构建不会读取它。测试和正式清单均只引用正式Release。发布时先上传并核验安装包，再生成／签名并更新相应清单；保留其他平台条目，防止并发发行互相覆盖。

测试构建使用独立的更新数据目录。Windows 测试构建还将设置、日志和默认截图目录放在应用数据目录的 `testing` 子目录下；测试版本之间沿用同一目录，升级会保留测试配置。带 `testing` 的 ZIP 仅供测试渠道使用，不是正式 Windows 版本。

Mac默认使用DMG安装路径。Windows自动覆盖必须在原生事务验收后显式开启 `HDRSHOT_WINDOWS_UPDATE_NATIVE_VALIDATED`；默认关闭。首次部署可信源和平台验证应在宣告更新可用之前完成。
