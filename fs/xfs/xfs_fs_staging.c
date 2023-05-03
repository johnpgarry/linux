/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 */

#include "xfs.h"
#include "xfs_fs_staging.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"

#include "linux/security.h"
#include "linux/fsnotify.h"

extern long _xfs_file_fallocate(
	struct file		*file,
	int			mode,
	loff_t			offset,
	loff_t			len,
	loff_t 			alignment);

int xfs_fallocate2(	struct file		*filp,
	void			__user *arg)
{
	struct inode		*inode = file_inode(filp);
	//struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_fallocate2 fallocate2;
	int ret;

	if (copy_from_user(&fallocate2, arg, sizeof(fallocate2)))
		return -EFAULT;

	if (fallocate2.flags & XFS_FALLOC2_ALIGNED) {
		if (!fallocate2.alignment || !is_power_of_2(fallocate2.alignment))
			return -EINVAL;

		if (fallocate2.offset % fallocate2.alignment)
			return -EINVAL;

		if (fallocate2.length % fallocate2.alignment)
			return -EINVAL;
	} else if (fallocate2.alignment) {
		return -EINVAL;
	}

	/* These are all just copied from vfs_fallocate() */
	if (fallocate2.offset < 0 || fallocate2.length <= 0)
		return -EINVAL;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	/*
	 * We cannot allow any fallocate operation on an active swapfile
	 */
	if (IS_SWAPFILE(inode))
		return -ETXTBSY;

	/*
	 * Revalidate the write permissions, in case security policy has
	 * changed since the files were opened.
	 */
	ret = security_file_permission(filp, MAY_WRITE);
	if (ret)
		return ret;

	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		return -ENODEV;

	/* Check for wrap through zero too */
	if (((fallocate2.offset + fallocate2.length) > inode->i_sb->s_maxbytes) ||
		((fallocate2.offset + fallocate2.length) < 0))
		return -EFBIG;

	if (!filp->f_op->fallocate)
		return -EOPNOTSUPP;

	file_start_write(filp);
	ret = _xfs_file_fallocate(filp, 0, fallocate2.offset, fallocate2.length, fallocate2.alignment);

	if (ret == 0)
		fsnotify_modify(filp);

	file_end_write(filp);

	return ret;
}
