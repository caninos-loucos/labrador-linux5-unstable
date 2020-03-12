// SPDX-License-Identifier: GPL-2.0
/* Lock down the kernel
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/security.h>
#include <linux/export.h>
#include <linux/lsm_hooks.h>
#include <linux/sysrq.h>
#include <asm/setup.h>

#ifdef CONFIG_ALLOW_LOCKDOWN_LIFT_BY_SYSRQ
static __read_mostly enum lockdown_reason kernel_locked_down;
#else
static __ro_after_init enum lockdown_reason kernel_locked_down;
#endif

static const char *const lockdown_reasons[LOCKDOWN_CONFIDENTIALITY_MAX+1] = {
	[LOCKDOWN_NONE] = "none",
	[LOCKDOWN_MODULE_SIGNATURE] = "unsigned module loading",
	[LOCKDOWN_DEV_MEM] = "/dev/mem,kmem,port",
	[LOCKDOWN_EFI_TEST] = "/dev/efi_test access",
	[LOCKDOWN_KEXEC] = "kexec of unsigned images",
	[LOCKDOWN_HIBERNATION] = "hibernation",
	[LOCKDOWN_PCI_ACCESS] = "direct PCI access",
	[LOCKDOWN_IOPORT] = "raw io port access",
	[LOCKDOWN_MSR] = "raw MSR access",
	[LOCKDOWN_ACPI_TABLES] = "modifying ACPI tables",
	[LOCKDOWN_PCMCIA_CIS] = "direct PCMCIA CIS storage",
	[LOCKDOWN_TIOCSSERIAL] = "reconfiguration of serial port IO",
	[LOCKDOWN_MODULE_PARAMETERS] = "unsafe module parameters",
	[LOCKDOWN_MMIOTRACE] = "unsafe mmio",
	[LOCKDOWN_DEBUGFS] = "debugfs access",
	[LOCKDOWN_INTEGRITY_MAX] = "integrity",
	[LOCKDOWN_KCORE] = "/proc/kcore access",
	[LOCKDOWN_KPROBES] = "use of kprobes",
	[LOCKDOWN_BPF_READ] = "use of bpf to read kernel RAM",
	[LOCKDOWN_PERF] = "unsafe use of perf",
	[LOCKDOWN_TRACEFS] = "use of tracefs",
	[LOCKDOWN_CONFIDENTIALITY_MAX] = "confidentiality",
};

static const enum lockdown_reason lockdown_levels[] = {LOCKDOWN_NONE,
						 LOCKDOWN_INTEGRITY_MAX,
						 LOCKDOWN_CONFIDENTIALITY_MAX};

/*
 * Put the kernel into lock-down mode.
 */
int lock_kernel_down(const char *where, enum lockdown_reason level)
{
	if (kernel_locked_down >= level)
		return -EPERM;

	kernel_locked_down = level;
	pr_notice("Kernel is locked down from %s; see https://wiki.debian.org/SecureBoot\n",
		  where);
	return 0;
}

static int __init lockdown_param(char *level)
{
	if (!level)
		return -EINVAL;

	if (strcmp(level, "integrity") == 0)
		lock_kernel_down("command line", LOCKDOWN_INTEGRITY_MAX);
	else if (strcmp(level, "confidentiality") == 0)
		lock_kernel_down("command line", LOCKDOWN_CONFIDENTIALITY_MAX);
	else
		return -EINVAL;

	return 0;
}

early_param("lockdown", lockdown_param);

/**
 * lockdown_is_locked_down - Find out if the kernel is locked down
 * @what: Tag to use in notice generated if lockdown is in effect
 */
static int lockdown_is_locked_down(enum lockdown_reason what)
{
	if (WARN(what >= LOCKDOWN_CONFIDENTIALITY_MAX,
		 "Invalid lockdown reason"))
		return -EPERM;

	if (kernel_locked_down >= what) {
		if (lockdown_reasons[what])
			pr_notice("Lockdown: %s: %s is restricted; see https://wiki.debian.org/SecureBoot\n",
				  current->comm, lockdown_reasons[what]);
		return -EPERM;
	}

	return 0;
}

static struct security_hook_list lockdown_hooks[] __lsm_ro_after_init = {
	LSM_HOOK_INIT(locked_down, lockdown_is_locked_down),
};

static int __init lockdown_lsm_init(void)
{
#if defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_INTEGRITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_INTEGRITY_MAX);
#elif defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_CONFIDENTIALITY_MAX);
#endif
	security_add_hooks(lockdown_hooks, ARRAY_SIZE(lockdown_hooks),
			   "lockdown");
	return 0;
}

static ssize_t lockdown_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	char temp[80];
	int i, offset = 0;

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];

		if (lockdown_reasons[level]) {
			const char *label = lockdown_reasons[level];

			if (kernel_locked_down == level)
				offset += sprintf(temp+offset, "[%s] ", label);
			else
				offset += sprintf(temp+offset, "%s ", label);
		}
	}

	/* Convert the last space to a newline if needed. */
	if (offset > 0)
		temp[offset-1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, temp, strlen(temp));
}

static ssize_t lockdown_write(struct file *file, const char __user *buf,
			      size_t n, loff_t *ppos)
{
	char *state;
	int i, len, err = -EINVAL;

	state = memdup_user_nul(buf, n);
	if (IS_ERR(state))
		return PTR_ERR(state);

	len = strlen(state);
	if (len && state[len-1] == '\n') {
		state[len-1] = '\0';
		len--;
	}

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];
		const char *label = lockdown_reasons[level];

		if (label && !strcmp(state, label))
			err = lock_kernel_down("securityfs", level);
	}

	kfree(state);
	return err ? err : n;
}

static const struct file_operations lockdown_ops = {
	.read  = lockdown_read,
	.write = lockdown_write,
};

static int __init lockdown_secfs_init(void)
{
	struct dentry *dentry;

	dentry = securityfs_create_file("lockdown", 0600, NULL, NULL,
					&lockdown_ops);
	return PTR_ERR_OR_ZERO(dentry);
}

core_initcall(lockdown_secfs_init);

#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY
DEFINE_EARLY_LSM(lockdown) = {
#else
DEFINE_LSM(lockdown) = {
#endif
	.name = "lockdown",
	.init = lockdown_lsm_init,
};

#ifdef CONFIG_ALLOW_LOCKDOWN_LIFT_BY_SYSRQ

/*
 * Take the kernel out of lockdown mode.
 */
static void lift_kernel_lockdown(void)
{
	pr_notice("Lifting lockdown\n");
	kernel_locked_down = LOCKDOWN_NONE;
}

/*
 * Allow lockdown to be lifted by pressing something like SysRq+x (and not by
 * echoing the appropriate letter into the sysrq-trigger file).
 */
static void sysrq_handle_lockdown_lift(int key)
{
	if (kernel_locked_down != LOCKDOWN_NONE)
		lift_kernel_lockdown();
}

static struct sysrq_key_op lockdown_lift_sysrq_op = {
	.handler	= sysrq_handle_lockdown_lift,
	.help_msg	= "unSB(x)",
	.action_msg	= "Disabling Secure Boot restrictions",
	.enable_mask	= SYSRQ_DISABLE_USERSPACE,
};

static int __init lockdown_lift_sysrq(void)
{
	if (kernel_locked_down != LOCKDOWN_NONE) {
		lockdown_lift_sysrq_op.help_msg[5] = LOCKDOWN_LIFT_KEY;
		register_sysrq_key(LOCKDOWN_LIFT_KEY, &lockdown_lift_sysrq_op);
	}
	return 0;
}

late_initcall(lockdown_lift_sysrq);

#endif /* CONFIG_ALLOW_LOCKDOWN_LIFT_BY_SYSRQ */