/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SUSFS_ANTI_DATA_GATE_H
#define __SUSFS_ANTI_DATA_GATE_H
#ifdef __aarch64__
int anti_data_gate_init(void);
void anti_data_gate_exit(void);
int anti_data_gate_arm(void);
void anti_data_gate_disarm(void);
#else
static inline int anti_data_gate_init(void) { return 0; }
static inline void anti_data_gate_exit(void) { }
static inline int anti_data_gate_arm(void) { return -ENODEV; }
static inline void anti_data_gate_disarm(void) { }
#endif
#endif /* __SUSFS_ANTI_DATA_GATE_H */