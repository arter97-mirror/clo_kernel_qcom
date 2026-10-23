/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM named_dmabuf

#if !defined(_NAMED_DMABUF_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NAMED_DMABUF_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(named_dmabuf_create,
	TP_PROTO(const char *name, bool rdonly),
	TP_ARGS(name, rdonly),

	TP_STRUCT__entry(
		__string(name, name)
		__field(bool, rdonly)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->rdonly = rdonly;
	),

	TP_printk("name=%s rdonly=%d", __get_str(name), __entry->rdonly)
);

TRACE_EVENT(named_dmabuf_create_err,
	TP_PROTO(const char *name, int err),
	TP_ARGS(name, err),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, err)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->err = err;
	),

	TP_printk("name=%s err=%d", __get_str(name), __entry->err)
);

TRACE_EVENT(named_dmabuf_recover,
	TP_PROTO(const char *name, int fd, bool rdonly),
	TP_ARGS(name, fd, rdonly),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, fd)
		__field(bool, rdonly)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->fd = fd;
		__entry->rdonly = rdonly;
	),

	TP_printk("name=%s fd=%d rdonly=%d", __get_str(name), __entry->fd, __entry->rdonly)
);

TRACE_EVENT(named_dmabuf_recover_err,
	TP_PROTO(const char *name, int err),
	TP_ARGS(name, err),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, err)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->err = err;
	),

	TP_printk("name=%s err=%d", __get_str(name), __entry->err)
);

TRACE_EVENT(named_dmabuf_destroy,
	TP_PROTO(const char *name),
	TP_ARGS(name),

	TP_STRUCT__entry(
		__string(name, name)
	),

	TP_fast_assign(
		__assign_str(name);
	),

	TP_printk("name=%s", __get_str(name))
);

TRACE_EVENT(named_dmabuf_destroy_err,
	TP_PROTO(const char *name, int err),
	TP_ARGS(name, err),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, err)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->err = err;
	),

	TP_printk("name=%s err=%d", __get_str(name), __entry->err)
);

TRACE_EVENT(named_dmabuf_open,
	TP_PROTO(const char *name, bool rdonly),
	TP_ARGS(name, rdonly),

	TP_STRUCT__entry(
		__string(name, name)
		__field(bool, rdonly)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->rdonly = rdonly;
	),

	TP_printk("name=%s rdonly=%d", __get_str(name), __entry->rdonly)
);

TRACE_EVENT(named_dmabuf_open_err,
	TP_PROTO(const char *name, int err),
	TP_ARGS(name, err),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, err)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->err = err;
	),

	TP_printk("name=%s err=%d", __get_str(name), __entry->err)
);

TRACE_EVENT(named_dmabuf_release,
	TP_PROTO(const char *name),
	TP_ARGS(name),

	TP_STRUCT__entry(
		__string(name, name)
	),

	TP_fast_assign(
		__assign_str(name);
	),

	TP_printk("name=%s", __get_str(name))
);

TRACE_EVENT(named_dmabuf_mmap,
	TP_PROTO(const char *name, bool rdonly),
	TP_ARGS(name, rdonly),

	TP_STRUCT__entry(
		__string(name, name)
		__field(bool, rdonly)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->rdonly = rdonly;
	),

	TP_printk("name=%s rdonly=%d", __get_str(name), __entry->rdonly)
);

#endif /* _NAMED_DMABUF_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE named_dmabuf_trace

#include <trace/define_trace.h>
