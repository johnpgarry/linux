/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 */
#ifndef __XFS_FS_STAGING_H__
#define __XFS_FS_STAGING_H__

struct xfs_fallocate2 {
	s64 offset;	/* bytes */
	s64 length;	/* bytes */
	u64 flags;
	u32 alignment;	/* bytes */
	u32 padding[8];
};

#define XFS_FALLOC2_ALIGNED (1U << 0)

int xfs_fallocate2(	struct file		*filp,
	void			__user *arg);

#endif	/* __XFS_FS_STAGING_H__ */
