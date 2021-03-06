/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>

#include <linux/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>

#include <linux/sysctl.h>
#include <linux/debugfs.h>

# define DEBUG_SUBSYSTEM S_LNET

#include "../../include/linux/libcfs/libcfs.h"
#include <asm/div64.h>

#include "../../include/linux/libcfs/libcfs_crypto.h"
#include "../../include/linux/lnet/lib-lnet.h"
#include "../../include/linux/lnet/lnet.h"
#include "tracefile.h"

MODULE_AUTHOR("Peter J. Braam <braam@clusterfs.com>");
MODULE_DESCRIPTION("Portals v3.1");
MODULE_LICENSE("GPL");

static void insert_debugfs(void);
static void remove_debugfs(void);

static struct dentry *lnet_debugfs_root;

static void kportal_memhog_free(struct libcfs_device_userstate *ldu)
{
	struct page **level0p = &ldu->ldu_memhog_root_page;
	struct page **level1p;
	struct page **level2p;
	int	   count1;
	int	   count2;

	if (*level0p != NULL) {

		level1p = (struct page **)page_address(*level0p);
		count1 = 0;

		while (count1 < PAGE_CACHE_SIZE/sizeof(struct page *) &&
		       *level1p != NULL) {

			level2p = (struct page **)page_address(*level1p);
			count2 = 0;

			while (count2 < PAGE_CACHE_SIZE/sizeof(struct page *) &&
			       *level2p != NULL) {

				__free_page(*level2p);
				ldu->ldu_memhog_pages--;
				level2p++;
				count2++;
			}

			__free_page(*level1p);
			ldu->ldu_memhog_pages--;
			level1p++;
			count1++;
		}

		__free_page(*level0p);
		ldu->ldu_memhog_pages--;

		*level0p = NULL;
	}

	LASSERT(ldu->ldu_memhog_pages == 0);
}

static int kportal_memhog_alloc(struct libcfs_device_userstate *ldu, int npages,
		     gfp_t flags)
{
	struct page **level0p;
	struct page **level1p;
	struct page **level2p;
	int	   count1;
	int	   count2;

	LASSERT(ldu->ldu_memhog_pages == 0);
	LASSERT(ldu->ldu_memhog_root_page == NULL);

	if (npages < 0)
		return -EINVAL;

	if (npages == 0)
		return 0;

	level0p = &ldu->ldu_memhog_root_page;
	*level0p = alloc_page(flags);
	if (*level0p == NULL)
		return -ENOMEM;
	ldu->ldu_memhog_pages++;

	level1p = (struct page **)page_address(*level0p);
	count1 = 0;
	memset(level1p, 0, PAGE_CACHE_SIZE);

	while (ldu->ldu_memhog_pages < npages &&
	       count1 < PAGE_CACHE_SIZE/sizeof(struct page *)) {

		if (cfs_signal_pending())
			return -EINTR;

		*level1p = alloc_page(flags);
		if (*level1p == NULL)
			return -ENOMEM;
		ldu->ldu_memhog_pages++;

		level2p = (struct page **)page_address(*level1p);
		count2 = 0;
		memset(level2p, 0, PAGE_CACHE_SIZE);

		while (ldu->ldu_memhog_pages < npages &&
		       count2 < PAGE_CACHE_SIZE/sizeof(struct page *)) {

			if (cfs_signal_pending())
				return -EINTR;

			*level2p = alloc_page(flags);
			if (*level2p == NULL)
				return -ENOMEM;
			ldu->ldu_memhog_pages++;

			level2p++;
			count2++;
		}

		level1p++;
		count1++;
	}

	return 0;
}

/* called when opening /dev/device */
static int libcfs_psdev_open(unsigned long flags, void *args)
{
	struct libcfs_device_userstate *ldu;

	try_module_get(THIS_MODULE);

	LIBCFS_ALLOC(ldu, sizeof(*ldu));
	if (ldu != NULL) {
		ldu->ldu_memhog_pages = 0;
		ldu->ldu_memhog_root_page = NULL;
	}
	*(struct libcfs_device_userstate **)args = ldu;

	return 0;
}

/* called when closing /dev/device */
static int libcfs_psdev_release(unsigned long flags, void *args)
{
	struct libcfs_device_userstate *ldu;

	ldu = (struct libcfs_device_userstate *)args;
	if (ldu != NULL) {
		kportal_memhog_free(ldu);
		LIBCFS_FREE(ldu, sizeof(*ldu));
	}

	module_put(THIS_MODULE);
	return 0;
}

static DECLARE_RWSEM(ioctl_list_sem);
static LIST_HEAD(ioctl_list);

int libcfs_register_ioctl(struct libcfs_ioctl_handler *hand)
{
	int rc = 0;

	down_write(&ioctl_list_sem);
	if (!list_empty(&hand->item))
		rc = -EBUSY;
	else
		list_add_tail(&hand->item, &ioctl_list);
	up_write(&ioctl_list_sem);

	return rc;
}
EXPORT_SYMBOL(libcfs_register_ioctl);

int libcfs_deregister_ioctl(struct libcfs_ioctl_handler *hand)
{
	int rc = 0;

	down_write(&ioctl_list_sem);
	if (list_empty(&hand->item))
		rc = -ENOENT;
	else
		list_del_init(&hand->item);
	up_write(&ioctl_list_sem);

	return rc;
}
EXPORT_SYMBOL(libcfs_deregister_ioctl);

static int libcfs_ioctl_int(struct cfs_psdev_file *pfile, unsigned long cmd,
			    void *arg, struct libcfs_ioctl_data *data)
{
	int err = -EINVAL;

	switch (cmd) {
	case IOC_LIBCFS_CLEAR_DEBUG:
		libcfs_debug_clear_buffer();
		return 0;
	/*
	 * case IOC_LIBCFS_PANIC:
	 * Handled in arch/cfs_module.c
	 */
	case IOC_LIBCFS_MARK_DEBUG:
		if (data->ioc_inlbuf1 == NULL ||
		    data->ioc_inlbuf1[data->ioc_inllen1 - 1] != '\0')
			return -EINVAL;
		libcfs_debug_mark_buffer(data->ioc_inlbuf1);
		return 0;
	case IOC_LIBCFS_MEMHOG:
		if (pfile->private_data == NULL) {
			err = -EINVAL;
		} else {
			kportal_memhog_free(pfile->private_data);
			/* XXX The ioc_flags is not GFP flags now, need to be fixed */
			err = kportal_memhog_alloc(pfile->private_data,
						   data->ioc_count,
						   data->ioc_flags);
			if (err != 0)
				kportal_memhog_free(pfile->private_data);
		}
		break;

	case IOC_LIBCFS_PING_TEST: {
		extern void (kping_client)(struct libcfs_ioctl_data *);
		void (*ping)(struct libcfs_ioctl_data *);

		CDEBUG(D_IOCTL, "doing %d pings to nid %s (%s)\n",
		       data->ioc_count, libcfs_nid2str(data->ioc_nid),
		       libcfs_nid2str(data->ioc_nid));
		ping = symbol_get(kping_client);
		if (!ping)
			CERROR("symbol_get failed\n");
		else {
			ping(data);
			symbol_put(kping_client);
		}
		return 0;
	}

	default: {
		struct libcfs_ioctl_handler *hand;
		err = -EINVAL;
		down_read(&ioctl_list_sem);
		list_for_each_entry(hand, &ioctl_list, item) {
			err = hand->handle_ioctl(cmd, data);
			if (err != -EINVAL) {
				if (err == 0)
					err = libcfs_ioctl_popdata(arg,
							data, sizeof(*data));
				break;
			}
		}
		up_read(&ioctl_list_sem);
		break;
	}
	}

	return err;
}

static int libcfs_ioctl(struct cfs_psdev_file *pfile, unsigned long cmd, void *arg)
{
	char    *buf;
	struct libcfs_ioctl_data *data;
	int err = 0;

	LIBCFS_ALLOC_GFP(buf, 1024, GFP_IOFS);
	if (buf == NULL)
		return -ENOMEM;

	/* 'cmd' and permissions get checked in our arch-specific caller */
	if (libcfs_ioctl_getdata(buf, buf + 800, arg)) {
		CERROR("PORTALS ioctl: data error\n");
		err = -EINVAL;
		goto out;
	}
	data = (struct libcfs_ioctl_data *)buf;

	err = libcfs_ioctl_int(pfile, cmd, arg, data);

out:
	LIBCFS_FREE(buf, 1024);
	return err;
}


struct cfs_psdev_ops libcfs_psdev_ops = {
	libcfs_psdev_open,
	libcfs_psdev_release,
	NULL,
	NULL,
	libcfs_ioctl
};

static int init_libcfs_module(void)
{
	int rc;

	libcfs_arch_init();
	libcfs_init_nidstrings();

	rc = libcfs_debug_init(5 * 1024 * 1024);
	if (rc < 0) {
		pr_err("LustreError: libcfs_debug_init: %d\n", rc);
		return rc;
	}

	rc = cfs_cpu_init();
	if (rc != 0)
		goto cleanup_debug;

	rc = misc_register(&libcfs_dev);
	if (rc) {
		CERROR("misc_register: error %d\n", rc);
		goto cleanup_cpu;
	}

	rc = cfs_wi_startup();
	if (rc) {
		CERROR("initialize workitem: error %d\n", rc);
		goto cleanup_deregister;
	}

	/* max to 4 threads, should be enough for rehash */
	rc = min(cfs_cpt_weight(cfs_cpt_table, CFS_CPT_ANY), 4);
	rc = cfs_wi_sched_create("cfs_rh", cfs_cpt_table, CFS_CPT_ANY,
				 rc, &cfs_sched_rehash);
	if (rc != 0) {
		CERROR("Startup workitem scheduler: error: %d\n", rc);
		goto cleanup_deregister;
	}

	rc = cfs_crypto_register();
	if (rc) {
		CERROR("cfs_crypto_register: error %d\n", rc);
		goto cleanup_wi;
	}

	insert_debugfs();

	CDEBUG(D_OTHER, "portals setup OK\n");
	return 0;
 cleanup_wi:
	cfs_wi_shutdown();
 cleanup_deregister:
	misc_deregister(&libcfs_dev);
cleanup_cpu:
	cfs_cpu_fini();
 cleanup_debug:
	libcfs_debug_cleanup();
	return rc;
}

static void exit_libcfs_module(void)
{
	int rc;

	remove_debugfs();

	if (cfs_sched_rehash != NULL) {
		cfs_wi_sched_destroy(cfs_sched_rehash);
		cfs_sched_rehash = NULL;
	}

	cfs_crypto_unregister();
	cfs_wi_shutdown();

	misc_deregister(&libcfs_dev);

	cfs_cpu_fini();

	rc = libcfs_debug_cleanup();
	if (rc)
		pr_err("LustreError: libcfs_debug_cleanup: %d\n", rc);

	libcfs_arch_cleanup();
}

static int proc_call_handler(void *data, int write, loff_t *ppos,
		void __user *buffer, size_t *lenp,
		int (*handler)(void *data, int write,
		loff_t pos, void __user *buffer, int len))
{
	int rc = handler(data, write, *ppos, buffer, *lenp);

	if (rc < 0)
		return rc;

	if (write) {
		*ppos += *lenp;
	} else {
		*lenp = rc;
		*ppos += rc;
	}
	return 0;
}

static int __proc_dobitmasks(void *data, int write,
			     loff_t pos, void __user *buffer, int nob)
{
	const int     tmpstrlen = 512;
	char	 *tmpstr;
	int	   rc;
	unsigned int *mask = data;
	int	   is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;
	int	   is_printk = (mask == &libcfs_printk) ? 1 : 0;

	rc = cfs_trace_allocate_string_buffer(&tmpstr, tmpstrlen);
	if (rc < 0)
		return rc;

	if (!write) {
		libcfs_debug_mask2str(tmpstr, tmpstrlen, *mask, is_subsys);
		rc = strlen(tmpstr);

		if (pos >= rc) {
			rc = 0;
		} else {
			rc = cfs_trace_copyout_string(buffer, nob,
						      tmpstr + pos, "\n");
		}
	} else {
		rc = cfs_trace_copyin_string(tmpstr, tmpstrlen, buffer, nob);
		if (rc < 0) {
			cfs_trace_free_string_buffer(tmpstr, tmpstrlen);
			return rc;
		}

		rc = libcfs_debug_str2mask(mask, tmpstr, is_subsys);
		/* Always print LBUG/LASSERT to console, so keep this mask */
		if (is_printk)
			*mask |= D_EMERG;
	}

	cfs_trace_free_string_buffer(tmpstr, tmpstrlen);
	return rc;
}

static int proc_dobitmasks(struct ctl_table *table, int write,
			   void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_call_handler(table->data, write, ppos, buffer, lenp,
				 __proc_dobitmasks);
}

static int __proc_dump_kernel(void *data, int write,
			      loff_t pos, void __user *buffer, int nob)
{
	if (!write)
		return 0;

	return cfs_trace_dump_debug_buffer_usrstr(buffer, nob);
}

static int proc_dump_kernel(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_call_handler(table->data, write, ppos, buffer, lenp,
				 __proc_dump_kernel);
}

static int __proc_daemon_file(void *data, int write,
			      loff_t pos, void __user *buffer, int nob)
{
	if (!write) {
		int len = strlen(cfs_tracefile);

		if (pos >= len)
			return 0;

		return cfs_trace_copyout_string(buffer, nob,
						cfs_tracefile + pos, "\n");
	}

	return cfs_trace_daemon_command_usrstr(buffer, nob);
}

static int proc_daemon_file(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_call_handler(table->data, write, ppos, buffer, lenp,
				 __proc_daemon_file);
}

static int libcfs_force_lbug(struct ctl_table *table, int write,
			     void __user *buffer,
			     size_t *lenp, loff_t *ppos)
{
	if (write)
		LBUG();
	return 0;
}

static int proc_fail_loc(struct ctl_table *table, int write,
			 void __user *buffer,
			 size_t *lenp, loff_t *ppos)
{
	int rc;
	long old_fail_loc = cfs_fail_loc;

	rc = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
	if (old_fail_loc != cfs_fail_loc)
		wake_up(&cfs_race_waitq);
	return rc;
}

static int __proc_cpt_table(void *data, int write,
			    loff_t pos, void __user *buffer, int nob)
{
	char *buf = NULL;
	int   len = 4096;
	int   rc  = 0;

	if (write)
		return -EPERM;

	LASSERT(cfs_cpt_table != NULL);

	while (1) {
		LIBCFS_ALLOC(buf, len);
		if (buf == NULL)
			return -ENOMEM;

		rc = cfs_cpt_table_print(cfs_cpt_table, buf, len);
		if (rc >= 0)
			break;

		if (rc == -EFBIG) {
			LIBCFS_FREE(buf, len);
			len <<= 1;
			continue;
		}
		goto out;
	}

	if (pos >= rc) {
		rc = 0;
		goto out;
	}

	rc = cfs_trace_copyout_string(buffer, nob, buf + pos, NULL);
 out:
	if (buf != NULL)
		LIBCFS_FREE(buf, len);
	return rc;
}

static int proc_cpt_table(struct ctl_table *table, int write,
			   void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_call_handler(table->data, write, ppos, buffer, lenp,
				 __proc_cpt_table);
}

static struct ctl_table lnet_table[] = {
	/*
	 * NB No .strategy entries have been provided since sysctl(8) prefers
	 * to go via /proc for portability.
	 */
	{
		.procname = "debug",
		.data     = &libcfs_debug,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "subsystem_debug",
		.data     = &libcfs_subsystem_debug,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "printk",
		.data     = &libcfs_printk,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "cpu_partition_table",
		.maxlen   = 128,
		.mode     = 0444,
		.proc_handler = &proc_cpt_table,
	},

	{
		.procname = "upcall",
		.data     = lnet_upcall,
		.maxlen   = sizeof(lnet_upcall),
		.mode     = 0644,
		.proc_handler = &proc_dostring,
	},
	{
		.procname = "debug_log_upcall",
		.data     = lnet_debug_log_upcall,
		.maxlen   = sizeof(lnet_debug_log_upcall),
		.mode     = 0644,
		.proc_handler = &proc_dostring,
	},
	{
		.procname = "catastrophe",
		.data     = &libcfs_catastrophe,
		.maxlen   = sizeof(int),
		.mode     = 0444,
		.proc_handler = &proc_dointvec,
	},
	{
		.procname = "dump_kernel",
		.maxlen   = 256,
		.mode     = 0200,
		.proc_handler = &proc_dump_kernel,
	},
	{
		.procname = "daemon_file",
		.mode     = 0644,
		.maxlen   = 256,
		.proc_handler = &proc_daemon_file,
	},
	{
		.procname = "force_lbug",
		.data     = NULL,
		.maxlen   = 0,
		.mode     = 0200,
		.proc_handler = &libcfs_force_lbug
	},
	{
		.procname = "fail_loc",
		.data     = &cfs_fail_loc,
		.maxlen   = sizeof(cfs_fail_loc),
		.mode     = 0644,
		.proc_handler = &proc_fail_loc
	},
	{
		.procname = "fail_val",
		.data     = &cfs_fail_val,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dointvec
	},
	{
	}
};

struct lnet_debugfs_symlink_def {
	char *name;
	char *target;
};

static const struct lnet_debugfs_symlink_def lnet_debugfs_symlinks[] = {
	{ "console_ratelimit",
	  "/sys/module/libcfs/parameters/libcfs_console_ratelimit"},
	{ "debug_path",
	  "/sys/module/libcfs/parameters/libcfs_debug_file_path"},
	{ "panic_on_lbug",
	  "/sys/module/libcfs/parameters/libcfs_panic_on_lbug"},
	{ "libcfs_console_backoff",
	  "/sys/module/libcfs/parameters/libcfs_console_backoff"},
	{ "debug_mb",
	  "/sys/module/libcfs/parameters/libcfs_debug_mb"},
	{ "console_min_delay_centisecs",
	  "/sys/module/libcfs/parameters/libcfs_console_min_delay"},
	{ "console_max_delay_centisecs",
	  "/sys/module/libcfs/parameters/libcfs_console_max_delay"},
	{},
};

static ssize_t lnet_debugfs_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	int error;

	error = table->proc_handler(table, 0, (void __user *)buf, &count, ppos);
	if (!error)
		error = count;

	return error;
}

static ssize_t lnet_debugfs_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	int error;

	error = table->proc_handler(table, 1, (void __user *)buf, &count, ppos);
	if (!error)
		error = count;

	return error;
}

static const struct file_operations lnet_debugfs_file_operations = {
	.open		= simple_open,
	.read		= lnet_debugfs_read,
	.write		= lnet_debugfs_write,
	.llseek		= default_llseek,
};

static void insert_debugfs(void)
{
	struct ctl_table *table;
	struct dentry *entry;
	const struct lnet_debugfs_symlink_def *symlinks;

	if (lnet_debugfs_root == NULL)
		lnet_debugfs_root = debugfs_create_dir("lnet", NULL);

	/* Even if we cannot create, just ignore it altogether) */
	if (IS_ERR_OR_NULL(lnet_debugfs_root))
		return;

	for (table = lnet_table; table->procname; table++)
		entry = debugfs_create_file(table->procname, table->mode,
					    lnet_debugfs_root, table,
					    &lnet_debugfs_file_operations);

	for (symlinks = lnet_debugfs_symlinks; symlinks->name; symlinks++)
		entry = debugfs_create_symlink(symlinks->name,
					       lnet_debugfs_root,
					       symlinks->target);

}

static void remove_debugfs(void)
{
	if (lnet_debugfs_root != NULL)
		debugfs_remove_recursive(lnet_debugfs_root);

	lnet_debugfs_root = NULL;
}

MODULE_VERSION("1.0.0");

module_init(init_libcfs_module);
module_exit(exit_libcfs_module);
