/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drm_guard - Widevine DRM "deviceUniqueId" interception.
 *
 * Ported from the SKP "device identity manager" 1.0.1 / DRMID 1.4.0 kernel-side
 * logic (kernel_context.h, binder_hook_builder.cpp): the request is recognised by
 * (descriptor android.hardware.drm.IDrmPlugin) + (code 11 == getPropertyByteArray)
 * + (property name "deviceUniqueId"), the reply by (status == 0) + (byte-array
 * length exactly 32), scoped to Widevine HAL processes only.
 *
 * What is NOT ported: the SKRoot kernel_module_kit hook API and the asmjit
 * generated AArch64 trampoline.  Here the hook is a kretprobe on binder_ioctl, so
 * the matching logic is plain C and nothing is written into kernel text.
 *
 * M1 is observation only: dry-run counters, no byte is ever modified.
 */
#ifndef __SUSFS_DRM_GUARD_H
#define __SUSFS_DRM_GUARD_H

#ifdef __aarch64__
int drm_guard_init(void);
void drm_guard_exit(void);
#else
static inline int drm_guard_init(void) { return 0; }
static inline void drm_guard_exit(void) { }
#endif

#endif /* __SUSFS_DRM_GUARD_H */
