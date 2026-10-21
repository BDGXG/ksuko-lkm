/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * taint_spoof - report /proc/sys/kernel/tainted as 0.
 *
 * The HideGuard build of this module carries this patch (its ko exports
 * susfs_taint_init / susfs_krp_ret_taint and hooks get_taint) while the upstream
 * source this tree is built from does not, so loading our build used to publish
 * 4608 (unsigned + out-of-tree) where the shipped ko published 0.  Same
 * technique, own implementation: a kretprobe on get_taint whose return value is
 * forced to 0.  Other readers of the taint mask are untouched.
 */
#ifndef __SUSFS_TAINT_SPOOF_H
#define __SUSFS_TAINT_SPOOF_H

#ifdef __aarch64__
int susfs_taint_spoof_init(void);
void susfs_taint_spoof_exit(void);
#else
static inline int susfs_taint_spoof_init(void) { return 0; }
static inline void susfs_taint_spoof_exit(void) { }
#endif

#endif /* __SUSFS_TAINT_SPOOF_H */