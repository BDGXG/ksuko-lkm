/* SPDX-License-Identifier: GPL-2.0-only */
/* taint_spoof.c - force get_taint() to report a clean kernel.  See taint_spoof.h. */
#define pr_fmt(fmt) "taint_spoof: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>

#include "susfs_kprobe.h"
#include "taint_spoof.h"

#ifdef __aarch64__

/* get_taint() returns the taint mask in x0; every caller that reads it - procfs
 * (proc_dointvec_taint), the oops header, module taint dumps - goes through the
 * same function, so masking it here covers all of them. */
static int taint_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	regs->regs[0] = 0;
	return 0;
}

static struct kretprobe g_taint_krp = {
	.handler = taint_ret_handler,
	.maxactive = 128,
};

int susfs_taint_spoof_init(void)
{
	int ret;

	/* Hot function: register_kretprobe() rewrites its prologue with a
	 * breakpoint, so it must not already be in use by another tool. */
	g_taint_krp.kp.symbol_name = "get_taint";
	g_taint_krp.kp.addr = NULL;
	ret = register_kretprobe(&g_taint_krp);
	if (ret) {
		susfs_krp_forget_addr(&g_taint_krp);
		pr_warn("get_taint is not probeable (%d) - taint stays visible\n", ret);
		return ret;
	}
	pr_info("get_taint hooked, taint reads back as 0\n");
	return 0;
}

void susfs_taint_spoof_exit(void)
{
	unregister_kretprobe(&g_taint_krp);
	susfs_krp_forget_addr(&g_taint_krp);
}

#endif /* __aarch64__ */