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
