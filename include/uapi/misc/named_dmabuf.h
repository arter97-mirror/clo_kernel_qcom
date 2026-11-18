/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _UAPI_LINUX_NAMED_DMABUF_H
#define _UAPI_LINUX_NAMED_DMABUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define NAMED_DMABUF_NAME_LEN	64

/* Flags for named_dmabuf_create.flags */
#define NAMED_DMABUF_F_RDONLY	(1U << 0)  /* consumer opens limited to O_RDONLY */
#define NAMED_DMABUF_F_MASK	(NAMED_DMABUF_F_RDONLY)

/*
 * NAMED_DMABUF_IOC_CREATE — register an existing dmabuf under a name.
 *
 * The producer allocates a dmabuf and passes its fd
 * here.  The driver takes a reference to that dmabuf and publishes it as
 * /dev/named_dmabuf/<name> for consumers to discover.
 */
struct named_dmabuf_create {
	__s32 dmabuf_fd;		/* fd of an existing dmabuf to register */
	__u32 flags;
	char  name[NAMED_DMABUF_NAME_LEN];
};

/*
 * NAMED_DMABUF_IOC_RECOVER_FD — re-acquire a dmabuf fd for a registered buffer.
 *
 * Producer-only: only the process that created the entry (or CAP_SYS_ADMIN)
 * may call this.  Intended for producers that need to recover their dmabuf fd
 * after a restart.  Consumers should instead open /dev/named_dmabuf_<peer>/<name>
 * and mmap() that fd directly.
 *
 * On success dmabuf_fd is filled with a new file descriptor backed by the
 * registered dmabuf.
 */
struct named_dmabuf_recover {
	char  name[NAMED_DMABUF_NAME_LEN];
	__s32 dmabuf_fd;
	__u32 _pad;
};

/*
 * NAMED_DMABUF_IOC_DESTROY — unregister a named buffer.
 *
 * Removes the entry from the registry so new consumers cannot discover it.
 * Existing consumers with an open cdev fd continue to work until they close.
 */
struct named_dmabuf_destroy {
	char name[NAMED_DMABUF_NAME_LEN];
};

#define NAMED_DMABUF_IOC_MAGIC	0xB5

#define NAMED_DMABUF_IOC_CREATE \
	_IOW(NAMED_DMABUF_IOC_MAGIC, 0x00, struct named_dmabuf_create)

#define NAMED_DMABUF_IOC_RECOVER_FD \
	_IOWR(NAMED_DMABUF_IOC_MAGIC, 0x01, struct named_dmabuf_recover)

#define NAMED_DMABUF_IOC_DESTROY \
	_IOW(NAMED_DMABUF_IOC_MAGIC, 0x02, struct named_dmabuf_destroy)

/*
 * NAMED_DMABUF_IOC_GET_EXPORT_INFO — get the export tuple for a registered buffer.
 *
 * Fails with -EOPNOTSUPP if the instance has no carveout or the buffer was
 * not validated against one.
 */
struct named_dmabuf_export_info {
	char  name[NAMED_DMABUF_NAME_LEN]; /* in:  buffer to query */
	char  inst_name[NAMED_DMABUF_NAME_LEN]; /* out: this instance's name */
	__u64 offset; /* out: bytes from carveout base */
	__u64 size; /* out: buffer size in bytes */
};

/*
 * NAMED_DMABUF_IOC_IMPORT_REMOTE — register a buffer described by an export tuple.
 *
 * Validates inst_name, range-checks offset+size against the carveout,
 * constructs a dmabuf from carveout_base+offset, and registers it under name.
 * Fails with -ENODEV if the instance has no carveout.
 * Fails with -EINVAL if inst_name does not match this instance.
 * Fails with -ERANGE if offset+size exceeds the carveout bounds.
 */
struct named_dmabuf_import {
	char  name[NAMED_DMABUF_NAME_LEN]; /* name to register under */
	char  inst_name[NAMED_DMABUF_NAME_LEN]; /* must match this instance's name */
	__u64 offset; /* bytes from carveout base */
	__u64 size; /* buffer size in bytes */
	__u32 flags; /* NAMED_DMABUF_F_* */
	__u32 _pad;
};

#define NAMED_DMABUF_IOC_GET_EXPORT_INFO \
	_IOWR(NAMED_DMABUF_IOC_MAGIC, 0x03, struct named_dmabuf_export_info)

#define NAMED_DMABUF_IOC_IMPORT_REMOTE \
	_IOW(NAMED_DMABUF_IOC_MAGIC, 0x04, struct named_dmabuf_import)

#endif
