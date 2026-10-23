# OAID（广告标识）拦截尝试记录

## 结论（2026-10-23 实测）

OPPO/一加机型的 OAID 由系统应用 `com.heytap.openid`（`/product/priv-app/OpenID/OpenID.apk`）
通过 Binder 服务下发：

```
action : com.heytap.openid.OPEN_ID_SERVICE
接口   : com.heytap.openid.IOpenID（AIDL，方法 getId / getIdentifier / getIDResult）
返回值 : **UTF-16LE 的 64 个十六进制字符**（= 32 字节），放在一个 140 字节的 parcel 里
```

应用侧（实测 `com.tencent.tmgp.dfm`）确实会查询，抓包结果：

```
tgid=23437 dsz=140 u16hex=64
....@...B.2.F.A.D.E.8.0.E.C.6.7.4.D.C.D.9.3.3.0.7.E.B.C.7.2.6.1.E.2.B.D.b.c.0.e.3.4.3.d.3.4.1.9.
```

**所以“OAID 是明文、可定位、理论上可替换”这一条成立。**

## 抓取方法（只读探针）

在 `binder_ioctl` 的 BR_REPLY 处理里增加一个可选探针：

```c
pstr 1        # 开启（节点命令）
```

规则：只在回包里出现「连续 ≥60 个 UTF-16 十六进制字符」时打印；打印内容为前 96 字节的 ASCII 视图。
排查时注意：

- GKI 内核上 `print_hex_dump_bytes()` **不可用**（输出不进 dmesg），要用 `pr_info` 拼字符串
- 探针必须放在 **替换之后**，这样日志里看到的就是替换后的内容，可直接验证是否生效

## 替换实现（已提交，未生效）

分支 `oaid-apply`，节点命令：

```sh
echo 'oaid <64hex>' > /proc/susfs_drm_guard   # 目标值（注意：是 64 个 hex 字符，不是 64 字节）
echo 'oapply 1'      > /proc/susfs_drm_guard   # 开启替换
```

匹配逻辑：扫描回包，找连续 64 个 UTF-16 hex 字符，等长替换为 32 字节（128 字节 UTF-16）的目标值。

### 踩过的坑

1. **单位搞错**：OAID 是 64 个 hex **字符**（32 字节）。若按 `drm_hex2bin(..., 64)` 解析，
   会去读 128 个字符 → 越界读到 `\0` 之后 → 永远 `-EINVAL`。正确做法是直接按字符串保存 64 个字符。
2. **内核代码块语法**：`else if (...) { {int x = ...; ...} else if ... }` 这种写法会因为花括号不配对导致
   `expected expression`。要么在函数开头声明变量，要么省略内层 `{`。

### 现状

```
oaid target set      ✅（rc=0，值已写入内核）
oaid patched          ❌（从未出现）
```

探针在替换后输出仍然是原值，说明 **最后一步写入失败**：140 字节 parcel 的目标页未驻留，
`copy_to_user_nofault()` 和回退的 `copy_to_user()` 都没成功——和 `deviceUniqueId`（40 字节
parcel）当初的症状一样，但后者能写进去，这个不行（可能跨越页边界）。

## 后续可选方向

| 方案 | 说明 | 风险 |
|---|---|---|
| 分页写入 | 按页对齐拆分写入，规避跨页失败 | 中 |
| `kmap` 内核Buffer | 走 `kwrite` 通道（`binder_alloc_copy_to_buffer`） | **高，曾导致硬挂起** |
| 用户态 hook | 注入 `com.heytap.openid` 进程 hook `getId()` | 高，需 LSPosed，且服务可能校验 appId |
| 放弃指定值 | 用 `rotate` 轮换（每次换新真值，断开关联） | 无 |

## 当前建议

OAID 保持“**只能轮换、不能指定**”：

- panel「广告标识 → 随机」= 调 OPLUS 接口 rotate，返回值记录在 `state/oaid.current`
- 注意 rotate 后应用缓存不会立即刷新，面板优先显示 rotate 返回值
- 涉及 /proc 命令时 **绝不要把 `haluid` 设为 0**（见 `drm-guard.md` 第 4 节）

---

# 附：QIMEI 实测（2026-10-23）

腾讯 QIMEI SDK 会把自己的结果缓存在应用私有目录：

```
/data/data/<pkg>/shared_prefs/QV1com.tencent.tdm.qimei.sdk.QimeiSDK*.xml
  <string name="tt">B87B47FD765E9A1ED63F7B5A944581A5</string>   ← QIMEI 本体（32 hex）
/data/data/<pkg>/files/qm/QV1com.tencent.tdm.qimei.sdk.QimeiSDK*
```

## A/B/C 对照实验（同一台设备、同一账号）

| 组 | 操作 | QIMEI |
|---|---|---|
| A | 原始 | `B87B47FD765E9A1ED63F7B5A944581A5` |
| B | rotate OAID 后清缓存 | `78511BDAE9238A0EDDF313DDB7E1D9FF` |
| C | **不改任何字段**，只清缓存 | `58EBC5E4FC8CB47A2CD71BDC3C7FFAD4` |

A ≠ B ≠ C，且 **C 在字段完全没变的情况下也变了**。

## 结论

1. **QIMEI 不是设备字段的哈希**。缓存缺失时 SDK 会重新生成一个新值，与 OAID/SSAID/Serial 当前值无关。
2. 所以“需要改一堆标识才能换 QIMEI”这个假设 **不成立**；换 QIMEI 只需：
   ```sh
   am force-stop <pkg>
   find /data/data/<pkg> -maxdepth 3 -iname '*qimei*' -exec rm -f {} \;
   # 冷启动应用
   ```
   不需要重启，也不需要动任何标识。
3. 因此 QIMEI 也不是“改不动的稳定输入”，真正的稳定锚点是 **账号**（OpenID / 微信 / QQ）。

## 风险（重要）

- QIMEI 的设计意图是**稳定的设备标识**，正常设备一辈子只有少数几个值。
  同一账号短期内出现多个 QIMEI，本身就是异常特征，比“设备指纹全真”更容易命中风控。
- 正确用法是 **在需要彻底重置环境时清一次**（换账号、换环境重开），而不是频繁清。
