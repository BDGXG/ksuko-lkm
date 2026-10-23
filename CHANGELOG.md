# Changelog

## 2026-10-23

### Fixed
- **drm_guard**: `drm_u_write()` 在 `copy_to_user_nofault()` 失败后回退到 `copy_to_user()`。
  修复 Widevine `deviceUniqueId` 伪装“命中但不生效”的问题。

  症状：`exact_hits` 正常增长，但 `written` 恒为 0 且 `nofault_fail` 持续增长。
  根因：binder 回包 parcel 的用户页在写入那一刻通常尚未驻留；
  `copy_to_user_nofault()` 不触发缺页处理，直接返回 `-EFAULT`。

  验证：`exact_hits=4 written=2 w_cand=2 w_written=2`，目标应用读到伪造的 32 字节 ID。

### Docs
- 新增 `docs/drm-guard.md`：A/B 两条写入路径的取舍、`haluid` 语义（uid，且 0 = 全系统扫描，禁止）、
  nofault 缺页陷阱、计数自检表、回滚步骤。

### Changed
- dim 面板「当前状态」改为逐项卡片：**原值 / 当前 / 待应用** 三行对照，每行带「复制」；
  卡片标题行提供「生成 / 清除」（Serial、Android ID、OAID、DRM ID），不支持的项自动置灰。
- `dim drm_push()` 增加下发后**回读校验**；`drm_off()` 改为 `apply 0` + `uids 0`，不再写 `enable 0`（后者会搞掉 kretprobe）。
- 新增 `dim drm-stats`（CLI + CGI + 面板状态行），直接展示内核真实计数。

### Docs
- 新增 `docs/dim-integration.md`：节点命令集、配置键、生效判据（`written>0`）、禁止项（`haluid 0`、`enable 0`）、故障对照表。

### Investigated
- **OAID**：实测确认为明文——系统应用 `com.heytap.openid` 通过 Binder 返回
  **UTF-16LE 的 64 个十六进制字符**（140 字节 parcel）。已实现内核侧匹配与等长替换
  （分支 `oaid-apply`，节点命令 `oaid <64hex>` / `oapply 1`），但**写入未成功**，
  原因与 `deviceUniqueId` 早期症状一致（目标页未驻留）。细节见 `docs/oaid-notes.md`。
- 探针（`pstr 1`）可在 GKI 内核上定位 binder 回包中的字符串，注意
  `print_hex_dump_bytes()` 不可用，需用 `pr_info` 拼 ASCII。
