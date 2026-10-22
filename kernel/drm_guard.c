/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drm_guard.c - Widevine "deviceUniqueId" interception, M1.5 (dry-run + diagnostics).
 *
 * Same dry-run behaviour as M1 - not a single byte is ever modified - plus the three
 * measurements M2 depends on:
 *
 *   1. whether BR_REPLY carries code == 11 (if it does, the matcher does not need the
 *      request side at all:  code==11 && status==0 && len==32  is enough);
 *   2. whether request and reply land in the SAME binder_ioctl (libbinder usually
 *      writes and reads in one BINDER_WRITE_READ, but a slow HAL splits it, and the
 *      M1 matcher required both halves in one call);
 *   3. who the reply actually belongs to: the BR_REPLY is consumed by the APP, so
 *      gating the entry handler on the HAL tgid filters out every hit.  That gate
 *      stays OFF here (g_hal_enforce is only reported, never used to skip).
 *
 * Recognition rules are unchanged from M1 / DRMID 1.4.0:
 *   request : BC_TRANSACTION, code 11 (IDrmPlugin::getPropertyByteArray), parcel
 *             carries "deviceUniqueId"
 *   reply   : BR_REPLY, status == 0, byte-array length exactly 32
 */
#define pr_fmt(fmt) "drm_guard: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/linux/android/binder.h>

#include "drm_guard.h"

#define DRM_ID_BYTES 32
/* Bounded prefix scan, same idea as the original 256-byte parcel prefix with a
 * wider window: a DRM property parcel is small, we never walk a megabyte. */
#define DRM_SCAN_BYTES 4096
#define DRM_CHUNK_BYTES 256
#define DRM_OVERLAP_BYTES 48
#define DRM_GET_PROPERTY_BYTE_ARRAY 11
#define DRM_HAL_LIMIT 4
/* Distinct (code,len) pairs reported to dmesg before the sampler goes quiet. */
#define DRM_COMBO_LIMIT 8

static const char kDrmDescriptor[] = "android.hardware.drm.IDrmPlugin";
static const char kDeviceUniqueId[] = "deviceUniqueId";

struct drm_call_ctx {
	unsigned long arg;
	unsigned int cmd;
	unsigned int reserved;
};

struct drm_stats {
	atomic64_t calls;
	atomic64_t write_streams;
	atomic64_t read_streams;
	atomic64_t parse_faults;
	atomic64_t descriptor_hits;
	atomic64_t property_hits;
	atomic64_t request_hits;
	atomic64_t reply_candidates;
	atomic64_t would_replace;
	atomic64_t reported;
	/* ---- M1.5 ---- */
	atomic64_t reply_code11;	/* candidate replies whose code == 11 */
	atomic64_t reply_bad_code;	/* candidate replies with any other code */
	atomic64_t reply_only;		/* candidate reply, no request in the same ioctl */
	atomic64_t request_only;	/* request seen, no reply in the same ioctl */
};

struct drm_reply_info {
	bool candidate;
	u32 code;			/* binder_transaction_data.code of the reply */
	u32 sender_pid;
	u32 data_size;
	u32 array_len;			/* the [status][len] header's len */
};

static struct drm_stats g_stats;
static u32 g_hal_tgids[DRM_HAL_LIMIT];
static u32 g_hal_count;
/* Reported only.  Non-zero would make drm_hal_allowed() reject every app-side
 * reply (the reply is read by the caller, not by the HAL), so nothing skips. */
static u32 g_hal_enforce;

/* Distinct (code, len) sampler: enough to tell "reply carries code 11" from
 * "reply carries 0" on the very first DRM access, without flooding dmesg. */
static u32 g_combo_code[DRM_COMBO_LIMIT];
static u32 g_combo_len[DRM_COMBO_LIMIT];
static u32 g_combo_size[DRM_COMBO_LIMIT];
static atomic_t g_combo_n = ATOMIC_INIT(0);

static bool drm_hal_gate_active(void)
{
	return READ_ONCE(g_hal_enforce) != 0 && READ_ONCE(g_hal_count) != 0;
}

static bool drm_hal_allowed(u32 tgid)
{
	u32 count = READ_ONCE(g_hal_count);
	u32 i;

	if (!READ_ONCE(g_hal_enforce))
		return true;
	for (i = 0; i < count && i < DRM_HAL_LIMIT; i++) {
		if (READ_ONCE(g_hal_tgids[i]) == tgid)
			return true;
	}
	return false;
}

static void drm_note_combo(u32 code, u32 len, u32 data_size)
{
	u32 i, n;

	for (i = 0; i < DRM_COMBO_LIMIT; i++) {
		if (READ_ONCE(g_combo_code[i]) == code &&
		    READ_ONCE(g_combo_len[i]) == len)
			return;
	}
	n = (u32)atomic_inc_return(&g_combo_n);
	if (n > DRM_COMBO_LIMIT)
		return;
	WRITE_ONCE(g_combo_code[n - 1], code);
	WRITE_ONCE(g_combo_len[n - 1], len);
	WRITE_ONCE(g_combo_size[n - 1], data_size);
	pr_info("BR_REPLY sample: code=%u len=%u data_size=%u\n",
		code, len, data_size);
}

/* Byte search over a kernel buffer, both UTF-16LE (Parcel::writeString16) and
 * raw UTF-8 (the NDK String framing the original also has a fallback for). */
static bool drm_buf_has_token(const u8 *buf, size_t len, const char *token)
{
	size_t tlen = strlen(token);
	size_t i, j;

	if (len < tlen)
		return false;
	for (i = 0; i + tlen <= len; i++) {
		for (j = 0; j < tlen; j++) {
			if (buf[i + j] != (u8)token[j])
				break;
		}
		if (j == tlen)
			return true;
	}
	if (len < tlen * 2)
		return false;
	for (i = 0; i + tlen * 2 <= len; i++) {
		for (j = 0; j < tlen; j++) {
			if (buf[i + j * 2] != (u8)token[j] ||
			    buf[i + j * 2 + 1] != 0)
				break;
		}
		if (j == tlen)
			return true;
	}
	return false;
}

/* Chunked user-memory scan: a fault is never read as a hit. */
static bool drm_user_has_token(const void __user *base, size_t size,
			       const char *token)
{
	u8 chunk[DRM_CHUNK_BYTES + DRM_OVERLAP_BYTES];
	size_t cap = min_t(size_t, size, DRM_SCAN_BYTES);
	size_t off = 0;

	if (!base || size < strlen(token))
		return false;
	while (off < cap) {
		size_t n = min_t(size_t, cap - off, sizeof(chunk));

		if (copy_from_user(chunk, (const u8 __user *)base + off, n))
			return false;
		if (drm_buf_has_token(chunk, n, token))
			return true;
		if (n <= DRM_OVERLAP_BYTES)
			break;
		off += n - DRM_OVERLAP_BYTES;
	}
	return false;
}

struct drm_request_result {
	bool code_match;
	bool property_match;
	bool descriptor_match;
	u32 data_size_log;
	u32 last_data_size;
};

/* Exact size of one write-stream command including its header, or 0 when the
 * opcode is not in the table (the caller resynchronises by one word).  Sizes
 * follow <uapi/linux/android/binder.h>: the payload is word aligned, so the
 * whole record always lands on a 4-byte boundary. */
static u32 drm_bc_record_size(u32 cmd)
{
	switch (cmd) {
	case BC_TRANSACTION:
	case BC_REPLY:
		return sizeof(u32) + sizeof(struct binder_transaction_data);
#ifdef BC_TRANSACTION_SEC_CTX
	case BC_TRANSACTION_SEC_CTX:
		return sizeof(u32) + sizeof(struct binder_transaction_data) +
		       sizeof(binder_uintptr_t);
#endif
	case BC_FREE_BUFFER:
	case BC_DEAD_BINDER_DONE:
		return sizeof(u32) + sizeof(binder_uintptr_t);
	case BC_INCREFS:
	case BC_ACQUIRE:
	case BC_RELEASE:
	case BC_DECREFS:
		return sizeof(u32) + sizeof(u32);
	case BC_ACQUIRE_RESULT:
		return sizeof(u32) + sizeof(u32);
	case BC_ATTEMPT_ACQUIRE:
		return sizeof(u32) + 2 * sizeof(u32);
	case BC_INCREFS_DONE:
	case BC_ACQUIRE_DONE:
	case BC_REQUEST_DEATH_NOTIFICATION:
	case BC_CLEAR_DEATH_NOTIFICATION:
		return sizeof(u32) + 2 * sizeof(binder_uintptr_t);
	case BC_REGISTER_LOOPER:
	case BC_ENTER_LOOPER:
	case BC_EXIT_LOOPER:
		return sizeof(u32);
	default:
		return 0;
	}
}

/* Walk the write stream (BC_* commands) looking for one DRM getPropertyByteArray
 * request carrying "deviceUniqueId". */
static void drm_scan_write_stream(struct binder_write_read *bwr,
				  struct drm_request_result *out)
{
	u32 consumed = READ_ONCE(bwr->write_consumed);
	const u8 __user *stream = (const u8 __user *)bwr->write_buffer;
	u32 off = 0;

	if (!stream)
		return;
	while (off + sizeof(u32) <= consumed) {
		u32 cmd;
		struct binder_transaction_data td;
		u32 step;

		if (copy_from_user(&cmd, stream + off, sizeof(cmd)))
			return;

		if (cmd == BC_TRANSACTION) {
			if (off + sizeof(u32) + sizeof(td) > consumed)
				return;
			if (copy_from_user(&td, stream + off + sizeof(u32),
					   sizeof(td)))
				return;
			if (td.code == DRM_GET_PROPERTY_BYTE_ARRAY &&
			    td.data_size >= 8 && td.data.ptr.buffer) {
				const void __user *parcel =
					(const void __user *)td.data.ptr.buffer;

				out->code_match = true;
				if (drm_user_has_token(parcel, td.data_size,
						       kDeviceUniqueId))
					out->property_match = true;
				if (drm_user_has_token(parcel, td.data_size,
						       kDrmDescriptor))
					out->descriptor_match = true;
				if (out->code_match && out->data_size_log < 4) {
					out->data_size_log++;
					out->last_data_size = td.data_size;
				}
			}
		}

		step = drm_bc_record_size(cmd);
		/* Unknown opcode: resynchronise by one word instead of giving up
		 * on the whole stream - one alien record must not hide a DRM
		 * request that comes after it. */
		if (!step)
			step = sizeof(u32);
		if (off + step <= off)
			return;
		off += step;
	}
}

/* Walk the read stream (BR_* commands) looking for the 32-byte reply. */
static bool drm_scan_read_stream(struct binder_write_read *bwr,
				 struct drm_reply_info *info)
{
	u32 consumed = READ_ONCE(bwr->read_consumed);
	const u8 __user *stream = (const u8 __user *)bwr->read_buffer;
	u32 off = 0;

	if (!stream)
		return false;
	while (off + sizeof(u32) <= consumed) {
		u32 cmd;
		u32 step = 0;
		bool candidate = false;
		struct binder_transaction_data td;

		if (copy_from_user(&cmd, stream + off, sizeof(cmd)))
			return false;

		switch (cmd) {
		case BR_REPLY:
		case BR_TRANSACTION:
			if (off + sizeof(u32) + sizeof(td) > consumed)
				return false;
			if (copy_from_user(&td, stream + off + sizeof(u32),
					   sizeof(td)))
				return false;
			step = sizeof(u32) + sizeof(td);
			if (cmd == BR_REPLY && td.data_size >= 8 &&
			    td.data.ptr.buffer) {
				u32 hdr[2];

				if (copy_from_user(
					    hdr,
					    (const void __user *)td.data.ptr.buffer,
					    sizeof(hdr)))
					return false;
				/* Every BR_REPLY feeds the sampler, candidate or
				 * not: what M2 needs to know is what `code` a
				 * reply packet really carries. */
				drm_note_combo(td.code, hdr[1], td.data_size);
				if (hdr[0] == 0 && hdr[1] == DRM_ID_BYTES) {
					candidate = true;
					info->candidate = true;
					info->code = td.code;
					info->sender_pid = td.sender_pid;
					info->data_size = td.data_size;
					info->array_len = hdr[1];
				}
			}
			break;
		case BR_INCREFS:
		case BR_ACQUIRE:
		case BR_RELEASE:
		case BR_DECREFS:
			step = sizeof(u32) + 2 * sizeof(binder_uintptr_t);
			break;
		case BR_DEAD_BINDER:
		case BR_CLEAR_DEATH_NOTIFICATION_DONE:
			step = sizeof(u32) + sizeof(binder_uintptr_t);
			break;
		case BR_NOOP:
		case BR_TRANSACTION_COMPLETE:
		case BR_DEAD_REPLY:
		case BR_FAILED_REPLY:
		case BR_SPAWN_LOOPER:
			step = sizeof(u32);
			break;
		default:
			return false;
		}
		if (candidate)
			return true;
		if (step == 0 || off + step <= off)
			return false;
		off += step;
	}
	return false;
}

static int drm_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;

	/* Deliberately NOT gated on the HAL tgid: see the header.  The gate is
	 * reported through /proc so a user-space publisher can be diagnosed, but
	 * it must not decide whether a reply is inspected. */
	ctx->arg = regs->regs[2];
	ctx->cmd = (unsigned int)regs->regs[1];
	ctx->reserved = 0;
	atomic64_inc(&g_stats.calls);
	return 0;
}

static int drm_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct drm_call_ctx *ctx = (struct drm_call_ctx *)ri->data;
	struct binder_write_read bwr;
	struct drm_request_result req = { };
	struct drm_reply_info rep = { };
	bool reply;

	if (!ctx->arg)
		return 0;
	if (copy_from_user(&bwr, (void __user *)ctx->arg, sizeof(bwr))) {
		atomic64_inc(&g_stats.parse_faults);
		return 0;
	}

	if (bwr.write_size && bwr.write_consumed) {
		atomic64_inc(&g_stats.write_streams);
		drm_scan_write_stream(&bwr, &req);
	}
	if (!bwr.read_size || !bwr.read_consumed)
		return 0;

	atomic64_inc(&g_stats.read_streams);
	reply = drm_scan_read_stream(&bwr, &rep);

	if (req.code_match) {
		/* Diagnostics: the first few code==11 requests, whatever interface
		 * they belong to, so a wrong assumption is visible in dmesg. */
		if (atomic64_inc_return(&g_stats.request_hits) <= 6)
			pr_info("code11 tgid=%d data_size=%u desc=%d prop=%d\n",
				current->tgid, req.last_data_size,
				req.descriptor_match ? 1 : 0,
				req.property_match ? 1 : 0);
	}
	if (req.property_match)
		atomic64_inc(&g_stats.property_hits);
	if (req.descriptor_match)
		atomic64_inc(&g_stats.descriptor_hits);

	if (reply) {
		atomic64_inc(&g_stats.reply_candidates);
		if (rep.code == DRM_GET_PROPERTY_BYTE_ARRAY)
			atomic64_inc(&g_stats.reply_code11);
		else
			atomic64_inc(&g_stats.reply_bad_code);
	}
	if (reply && !req.property_match) {
		/* Either the HAL answered after libbinder had already returned
		 * from the write call (two ioctls), or the request side was not
		 * recognised.  Both cases break the M1 matcher. */
		if (atomic64_inc_return(&g_stats.reply_only) <= 6)
			pr_info("reply without request: tgid=%d code=%u sender_pid=%u data_size=%u len=%u\n",
				current->tgid, rep.code, rep.sender_pid,
				rep.data_size, rep.array_len);
	}
	if (req.property_match && !reply)
		atomic64_inc(&g_stats.request_only);

	/* Dry-run: observe only. A rewrite needs request and reply in the same
	 * synchronous ioctl, which is how getPropertyByteArray is issued. */
	if (!req.property_match || !reply)
		return 0;

	atomic64_inc(&g_stats.would_replace);
	if (atomic64_inc_return(&g_stats.reported) <= 3) {
		pr_info("dry-run hit: tgid=%d pid=%d code=%u descriptor=%d reply_code=%u\n",
			current->tgid, current->pid,
			DRM_GET_PROPERTY_BYTE_ARRAY,
			req.descriptor_match ? 1 : 0, rep.code);
	}
	return 0;
}

static struct kretprobe g_drm_krp = {
	.entry_handler = drm_entry_handler,
	.handler = drm_ret_handler,
	.data_size = sizeof(struct drm_call_ctx),
	.maxactive = 64,
};

static const char *const g_drm_probe_symbols[] = {
	"binder_ioctl",
	"binder_ioctl_write_read",
};

static int drm_probe_arm(void)
{
	size_t i;
	int ret = -ENOENT;

	for (i = 0; i < ARRAY_SIZE(g_drm_probe_symbols); i++) {
		g_drm_krp.kp.symbol_name = g_drm_probe_symbols[i];
		g_drm_krp.kp.addr = NULL;
		ret = register_kretprobe(&g_drm_krp);
		if (!ret) {
			pr_info("kretprobe armed on %s\n", g_drm_krp.kp.symbol_name);
			return 0;
		}
		g_drm_krp.kp.addr = NULL;
	}
	pr_warn("no probeable binder entry (%d)\n", ret);
	return ret;
}

static void drm_probe_disarm(void)
{
	if (g_drm_krp.kp.symbol_name)
		unregister_kretprobe(&g_drm_krp);
	g_drm_krp.kp.addr = NULL;
}

static int drm_proc_show(struct seq_file *m, void *v)
{
	u32 i, n;

	seq_printf(m, "mode=dry-run\n");
	seq_printf(m, "probe=%s\n",
		   g_drm_krp.kp.symbol_name ? g_drm_krp.kp.symbol_name : "off");
	seq_printf(m, "hal_enforce=%u hal_count=%u (reported only, never filters)\n",
		   READ_ONCE(g_hal_enforce), READ_ONCE(g_hal_count));
	seq_printf(m, "calls=%lld\n", atomic64_read(&g_stats.calls));
	seq_printf(m, "write_streams=%lld\n",
		   atomic64_read(&g_stats.write_streams));
	seq_printf(m, "read_streams=%lld\n",
		   atomic64_read(&g_stats.read_streams));
	seq_printf(m, "parse_faults=%lld\n",
		   atomic64_read(&g_stats.parse_faults));
	seq_printf(m, "request_hits=%lld\n",
		   atomic64_read(&g_stats.request_hits));
	seq_printf(m, "property_hits=%lld\n",
		   atomic64_read(&g_stats.property_hits));
	seq_printf(m, "descriptor_hits=%lld\n",
		   atomic64_read(&g_stats.descriptor_hits));
	seq_printf(m, "reply_candidates=%lld\n",
		   atomic64_read(&g_stats.reply_candidates));
	seq_printf(m, "reply_code11=%lld\n",
		   atomic64_read(&g_stats.reply_code11));
	seq_printf(m, "reply_bad_code=%lld\n",
		   atomic64_read(&g_stats.reply_bad_code));
	seq_printf(m, "reply_only=%lld\n",
		   atomic64_read(&g_stats.reply_only));
	seq_printf(m, "request_only=%lld\n",
		   atomic64_read(&g_stats.request_only));
	seq_printf(m, "would_replace=%lld\n",
		   atomic64_read(&g_stats.would_replace));

	n = min_t(u32, (u32)atomic_read(&g_combo_n), DRM_COMBO_LIMIT);
	for (i = 0; i < n; i++)
		seq_printf(m, "sample[%u]=code:%u len:%u data_size:%u\n", i,
			   READ_ONCE(g_combo_code[i]),
			   READ_ONCE(g_combo_len[i]),
			   READ_ONCE(g_combo_size[i]));
	return 0;
}

static int drm_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, drm_proc_show, NULL);
}

static ssize_t drm_proc_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	char buf[64];
	size_t len = min_t(size_t, count, sizeof(buf) - 1);
	u32 tgid = 0;
	u32 i;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sysfs_streq(buf, "off") || sysfs_streq(buf, "reset")) {
		WRITE_ONCE(g_hal_enforce, 0);
		WRITE_ONCE(g_hal_count, 0);
		memset(g_hal_tgids, 0, sizeof(g_hal_tgids));
		return count;
	}
	if (sscanf(buf, "hal %u", &tgid) != 1)
		return -EINVAL;
	if (tgid == 0) {
		WRITE_ONCE(g_hal_enforce, 0);
		return count;
	}
	for (i = 0; i < DRM_HAL_LIMIT; i++) {
		if (READ_ONCE(g_hal_tgids[i]) == tgid)
			return count;
	}
	for (i = 0; i < DRM_HAL_LIMIT; i++) {
		if (!READ_ONCE(g_hal_tgids[i])) {
			WRITE_ONCE(g_hal_tgids[i], tgid);
			WRITE_ONCE(g_hal_count, READ_ONCE(g_hal_count) + 1);
			WRITE_ONCE(g_hal_enforce, 1);
			pr_info("hal tgid %u published (%u total)\n", tgid, i + 1);
			return count;
		}
	}
	return -ENOSPC;
}

static const struct proc_ops drm_proc_ops = {
	.proc_open = drm_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = drm_proc_write,
};

static struct proc_dir_entry *g_drm_proc;

int drm_guard_init(void)
{
	int ret;

	memset(&g_stats, 0, sizeof(g_stats));
	memset(g_hal_tgids, 0, sizeof(g_hal_tgids));

	ret = drm_probe_arm();
	if (ret)
		return ret;

	g_drm_proc = proc_create("susfs_drm_guard", 0644, NULL, &drm_proc_ops);
	if (!g_drm_proc)
		pr_warn("proc entry creation failed; counters unavailable\n");
	return 0;
}

void drm_guard_exit(void)
{
	if (g_drm_proc) {
		proc_remove(g_drm_proc);
		g_drm_proc = NULL;
	}
	drm_probe_disarm();
}