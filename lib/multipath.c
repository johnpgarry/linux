// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 * Copyright (c) 2026 Oracle and/or its affiliates.
 */
#include <linux/module.h>
#include <linux/multipath.h>
#include <trace/events/block.h>

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head);

static struct workqueue_struct *mpath_wq;

static const char * const mpath_iopolicy_names[] = {
	[MPATH_IOPOLICY_NUMA]	= "numa",
	[MPATH_IOPOLICY_RR]	= "round-robin",
	[MPATH_IOPOLICY_QD]	= "queue-depth",
};

static int mpath_iopolicy_parse(const char *str)
{
	return __sysfs_match_string(mpath_iopolicy_names,
		ARRAY_SIZE(mpath_iopolicy_names), str);
}

int mpath_set_iopolicy(const char *str, enum mpath_iopolicy_e *iopolicy)
{
	int policy;

	if (!str)
		return -EINVAL;
	policy = mpath_iopolicy_parse(str);
	if (policy < 0)
		return policy;
	*iopolicy = policy;

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_set_iopolicy);

int mpath_get_iopolicy(char *buf, int iopolicy)
{
	return sprintf(buf, "%s\n", mpath_iopolicy_names[iopolicy]);
}
EXPORT_SYMBOL_GPL(mpath_get_iopolicy);

void mpath_synchronize(struct mpath_head *mpath_head)
{
	synchronize_srcu(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_synchronize);

void mpath_add_device(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device)
{
	mpath_device->mpath_head = mpath_head;
	mutex_lock(&mpath_head->lock);
	list_add_tail_rcu(&mpath_device->siblings, &mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);
	if (cancel_delayed_work(&mpath_head->remove_work))
		module_put(mpath_head->drv_module);
}
EXPORT_SYMBOL_GPL(mpath_add_device);

bool mpath_delete_device(struct mpath_device *mpath_device)
{
	bool empty;

	mutex_lock(&mpath_device->mpath_head->lock);
	list_del_rcu(&mpath_device->siblings);
	empty = list_empty(&mpath_device->mpath_head->dev_list);
	mutex_unlock(&mpath_device->mpath_head->lock);

	return empty;
}
EXPORT_SYMBOL_GPL(mpath_delete_device);

bool mpath_head_devices_empty(struct mpath_head *mpath_head)
{
	bool empty;

	mutex_lock(&mpath_head->lock);
	empty = list_empty(&mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);

	return empty;
}
EXPORT_SYMBOL_GPL(mpath_head_devices_empty);

int mpath_call_for_device(struct mpath_head *mpath_head,
			int (*cb)(struct mpath_device *mpath_device))
{
	struct mpath_device *mpath_device;
	int ret = -EWOULDBLOCK, srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device)
		ret = cb(mpath_device);
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_call_for_device);

bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	bool changed = false;
	int node;

	for_each_node(node) {
		if (mpath_device ==
			rcu_access_pointer(mpath_head->current_path[node])) {
			rcu_assign_pointer(mpath_head->current_path[node],
				NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(mpath_clear_current_path);

static void mpath_revalidate_paths_iter(struct mpath_head *mpath_head,
	void (*not_ready_cb)(struct mpath_device *mpath_device))
{
	sector_t capacity = get_capacity(mpath_head->disk);
	struct mpath_device *mpath_device;
	int srcu_idx;

	if (!not_ready_cb)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (capacity != get_capacity(mpath_device->disk))
			not_ready_cb(mpath_device);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}

void mpath_clear_paths(struct mpath_head *mpath_head)
{
	int node;

	for_each_node(node)
		rcu_assign_pointer(mpath_head->current_path[node], NULL);
}
EXPORT_SYMBOL_GPL(mpath_clear_paths);

void mpath_revalidate_paths(struct mpath_head *mpath_head,
	void (*not_ready_cb)(struct mpath_device *mpath_device))
{
	mpath_revalidate_paths_iter(mpath_head, not_ready_cb);
	mpath_clear_paths(mpath_head);

	mpath_schedule_requeue_work(mpath_head);
}
EXPORT_SYMBOL_GPL(mpath_revalidate_paths);

static bool mpath_path_is_disabled(struct mpath_head *mpath_head,
				struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_disabled(mpath_device);
}

static struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head,
					int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	struct mpath_device *found = NULL, *fallback = NULL, *mpath_device;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
		srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		if (mpath_device->numa_node != NUMA_NO_NODE &&
		    (mpath_head->mpdt->get_iopolicy(mpath_head) ==
			MPATH_IOPOLICY_NUMA))
			distance = node_distance(node,
					mpath_device->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_device->access_state) {
		case MPATH_STATE_OPTIMIZED:
		    if (distance < found_distance) {
			    found_distance = distance;
			    found = mpath_device;
		    }
		    break;
		case MPATH_STATE_NONOPTIMIZED:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    fallback = mpath_device;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!found)
		found = fallback;

	if (found)
		rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_next_dev(struct mpath_head *mpath_head,
				struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_head->dev_list,
			&mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_head->dev_list,
				struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(
				struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device, *found = NULL;
	int node = numa_node_id();
	enum mpath_access_state access_state_old;
	struct mpath_device *old =
			srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_head, node);

	if (list_is_singular(&mpath_head->dev_list)) {
		if (mpath_path_is_disabled(mpath_head, old))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_head, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_head, mpath_device)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;
		if (mpath_device->access_state == MPATH_STATE_OPTIMIZED) {
			found = mpath_device;
			goto out;
		}
		if (mpath_device->access_state == MPATH_STATE_NONOPTIMIZED)
			found = mpath_device;
	}

	/*
	 * The loop above skips the current path for round-robin semantics.
	 * Fall back to the current path if either:
	 *  - no other optimized path found and current is optimized,
	 *  - no other usable path found and current is usable.
	 */
	access_state_old = old->access_state;
	if (!mpath_path_is_disabled(mpath_head, old) &&
	    (access_state_old == MPATH_STATE_OPTIMIZED ||
	    (!found && access_state_old == MPATH_STATE_NONOPTIMIZED)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(
				struct mpath_head *mpath_head)
{
	struct mpath_device *best_opt = NULL, *mpath_device;
	struct mpath_device *best_nonopt = NULL;
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;
	unsigned int depth;
	int (*get_nr_active)(struct mpath_device *) =
				mpath_head->mpdt->get_nr_active;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		depth = get_nr_active(mpath_device);

		switch (mpath_device->access_state) {
		case MPATH_STATE_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = mpath_device;
			}
			break;
		case MPATH_STATE_NONOPTIMIZED:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = mpath_device;
			}
			break;
		default:
			break;
		}

		if (min_depth_opt == 0)
			return best_opt;
	}

	return best_opt ? best_opt : best_nonopt;
}

static inline bool mpath_path_is_optimized(struct mpath_head *mpath_head,
				struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_optimized(mpath_device);
}

static struct mpath_device *mpath_numa_path(struct mpath_head *mpath_head)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;

	mpath_device = srcu_dereference(mpath_head->current_path[node],
					&mpath_head->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_head, node);
	if (unlikely(!mpath_path_is_optimized(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, node);
	return mpath_device;
}

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head)
{
	enum mpath_iopolicy_e iopolicy =
			mpath_head->mpdt->get_iopolicy(mpath_head);

	switch (iopolicy) {
	case MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_head);
	case MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_head);
	default:
		return mpath_numa_path(mpath_head);
	}
}

static bool mpath_available_path(struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device;

	if (!test_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags))
		return false;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_head->mpdt->available_path(mpath_device))
			return true;
	}

	/*
	 * If "mpath_head->delayed_removal_secs" is set (i.e., non-zero), do
	 * not immediately fail I/O. Instead, requeue the I/O for the configured
	 * duration, anticipating that if there's a transient link failure then
	 * it may recover within this time window. This parameter is exported to
	 * userspace via sysfs, and its default value is zero. It is internally
	 * mapped to MPATH_HEAD_QUEUE_IF_NO_PATH. When delayed_removal_secs is
	 * non-zero, this flag is set to true. When zero, the flag is cleared.
	 */
	return mpath_head_queue_if_no_path(mpath_head);

}

static void mpath_bdev_submit_bio(struct bio *bio)
{
	struct mpath_head *mpath_head = bio->bi_bdev->bd_disk->private_data;
	struct device *dev = mpath_head->parent;
	struct mpath_device *mpath_device;
	int srcu_idx;

	/*
	 * The mpath_device might be going away and the bio might be moved to a
	 * different queue in failover, so we need to use the bio_split
	 * pool from the original queue to allocate the bvecs from.
	 */
	bio = bio_split_to_limits(bio);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (likely(mpath_device)) {
		if (mpath_head->mpdt->clone_bio) {
			struct bio *orig = bio;

			bio = mpath_head->mpdt->clone_bio(bio);
			if (!bio) {
				bio_io_error(orig);
				goto out;
			}
		}

		bio_set_dev(bio, mpath_device->disk->part0);
		/*
		 * Use BIO_REMAPPED to skip bio_check_eod() when this bio
		 * enters submit_bio_noacct() for the per-path device. The EOD
		 * check already passed on the multipath head.
		 */
		bio_set_flag(bio, BIO_REMAPPED);
		bio->bi_opf |= REQ_MPATH;
		trace_block_bio_remap(bio, disk_devt(mpath_device->disk),
				      bio->bi_iter.bi_sector);
		submit_bio_noacct(bio);
	} else if (mpath_available_path(mpath_head)) {
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");

		spin_lock_irq(&mpath_head->requeue_lock);
		bio_list_add(&mpath_head->requeue_list, bio);
		spin_unlock_irq(&mpath_head->requeue_lock);
	} else {
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");

		bio_io_error(bio);
	}

out:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}


int mpath_get_head(struct mpath_head *mpath_head)
{
	if (!refcount_inc_not_zero(&mpath_head->refcount))
		return -ENXIO;
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_get_head);

static void mpath_head_cleanup(struct mpath_head *mpath_head)
{
	cleanup_srcu_struct(&mpath_head->srcu);
}

void mpath_put_head(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (refcount_dec_and_test(refcount)) {
		mpath_head_cleanup(mpath_head);
		wake_up_var(refcount);
	}
}
EXPORT_SYMBOL_GPL(mpath_put_head);

void mpath_head_uninit(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (refcount_dec_and_test(refcount)) {
		mpath_head_cleanup(mpath_head);
	} else {
		wait_var_event(refcount, !refcount_read(refcount));
	}
}
EXPORT_SYMBOL_GPL(mpath_head_uninit);

static int mpath_bdev_open(struct gendisk *disk, blk_mode_t mode)
{
	struct mpath_head *mpath_head = disk->private_data;

	return mpath_get_head(mpath_head);
}

static void mpath_bdev_release(struct gendisk *disk)
{
	struct mpath_head *mpath_head = disk->private_data;

	mpath_put_head(mpath_head);
}

static int mpath_pr_register(struct block_device *bdev, u64 old_key,
			u64 new_key, unsigned int flags)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_register) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_register(mpath_device->disk->part0,
				old_key, new_key, flags);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_reserve(struct block_device *bdev, u64 key,
		enum pr_type type, unsigned flags)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_reserve) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_reserve(mpath_device->disk->part0, key,
				type, flags);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_release(struct block_device *bdev, u64 key,
				enum pr_type type)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_release) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_release(mpath_device->disk->part0, key, type);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_preempt(struct block_device *bdev, u64 old, u64 new,
		enum pr_type type, bool abort)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_preempt) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_preempt(mpath_device->disk->part0, old,
				new, type, abort);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_clear(struct block_device *bdev, u64 key)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_clear) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_clear(mpath_device->disk->part0, key);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_read_keys(struct block_device *bdev,
		struct pr_keys *keys_info)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_read_keys) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_read_keys(mpath_device->disk->part0, keys_info);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_read_reservation(struct block_device *bdev,
		struct pr_held_reservation *resv)
{
	struct mpath_head *mpath_head = dev_get_drvdata(&bdev->bd_device);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		const struct pr_ops *ops = mpath_device->disk->fops->pr_ops;

		if (!ops || !ops->pr_read_reservation) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		ret = ops->pr_read_reservation(mpath_device->disk->part0,
				resv);
	}
unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static const struct pr_ops mpath_pr_ops = {
	.pr_register	= mpath_pr_register,
	.pr_reserve	= mpath_pr_reserve,
	.pr_release	= mpath_pr_release,
	.pr_preempt	= mpath_pr_preempt,
	.pr_clear	= mpath_pr_clear,
	.pr_read_keys	= mpath_pr_read_keys,
	.pr_read_reservation = mpath_pr_read_reservation,
};

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
	.submit_bio	= mpath_bdev_submit_bio,
	.pr_ops		= &mpath_pr_ops,
};
EXPORT_SYMBOL_GPL(mpath_ops);

static int mpath_chr_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	return mpath_get_head(mpath_head);
}

static int mpath_chr_release(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	mpath_put_head(mpath_head);
	return 0;
}

static long mpath_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);
	struct mpath_device *mpath_device;
	int srcu_idx, err = -EWOULDBLOCK;
	void *unlocked_ioctl_data = NULL;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (!mpath_device)
		goto out_unlock;
	if (mpath_head->mpdt->ioctl_begin)
		mpath_head->mpdt->ioctl_begin(mpath_device, cmd,
					&unlocked_ioctl_data);
	if (unlocked_ioctl_data)
		srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	err = mpath_head->mpdt->cdev_ioctl(mpath_device, cmd, arg,
					file->f_mode & FMODE_WRITE);
	if (unlocked_ioctl_data) {
		mpath_head->mpdt->ioctl_finish(unlocked_ioctl_data);
		return err;
	}

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return err;
}

static int mpath_chr_uring_cmd(struct io_uring_cmd *ioucmd,
		unsigned int issue_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);
	struct mpath_device *mpath_device;
	/* error code copied from nvme_ns_head_chr_uring_cmd */
	int srcu_idx, ret = -EINVAL;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (!mpath_device)
		goto out_unlock;

	if (!mpath_head->mpdt->chr_uring_cmd) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	ret = mpath_head->mpdt->chr_uring_cmd(mpath_device, ioucmd,
			issue_flags);
out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return ret;
}

static int mpath_chr_uring_cmd_iopoll(struct io_uring_cmd *ioucmd,
				 struct io_comp_batch *iob,
				 unsigned int poll_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	if (!mpath_head->mpdt->chr_uring_cmd_iopoll)
		return -EOPNOTSUPP;

	return mpath_head->mpdt->chr_uring_cmd_iopoll(ioucmd, iob, poll_flags);
}

const struct file_operations mpath_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= mpath_chr_open,
	.release	= mpath_chr_release,
	.unlocked_ioctl	= mpath_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= mpath_chr_uring_cmd,
	.uring_cmd_iopoll = mpath_chr_uring_cmd_iopoll,
};
EXPORT_SYMBOL_GPL(mpath_chr_fops);

static void mpath_head_add_cdev(struct mpath_head *mpath_head)
{
	if (!mpath_head->mpdt->add_cdev)
		return;

	if (mpath_head->mpdt->add_cdev(mpath_head)) {
		dev_err(disk_to_dev(mpath_head->disk),
			"Unable to create the cdev\n");
		return;
	}
	set_bit(MPATH_HEAD_CDEV_LIVE, &mpath_head->flags);
}

static void mpath_head_del_cdev(struct mpath_head *mpath_head)
{
	if (!mpath_head->mpdt->del_cdev)
		return;

	if (test_and_clear_bit(MPATH_HEAD_CDEV_LIVE, &mpath_head->flags))
		mpath_head->mpdt->del_cdev(mpath_head);
}

static void multipath_partition_scan_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
		container_of(work, struct mpath_head, partition_scan_work);

	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_head->disk->state)))
		return;

	mutex_lock(&mpath_head->disk->open_mutex);
	bdev_disk_changed(mpath_head->disk, false);
	mutex_unlock(&mpath_head->disk->open_mutex);
}

static void mpath_requeue_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
	    container_of(work, struct mpath_head, requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&mpath_head->requeue_lock);
	next = bio_list_get(&mpath_head->requeue_list);
	spin_unlock_irq(&mpath_head->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}

bool mpath_can_remove_head(struct mpath_head *mpath_head)
{
	bool remove = false;

	mutex_lock(&mpath_head->lock);
	/*
	 * Ensure that no one could remove this module while the head
	 * remove work is pending.
	 */
	if (mpath_head_queue_if_no_path(mpath_head) &&
		try_module_get(mpath_head->drv_module)) {

		mod_delayed_work(mpath_wq, &mpath_head->remove_work,
				mpath_head->delayed_removal_secs * HZ);
	} else {
		remove = true;
	}

	mutex_unlock(&mpath_head->lock);
	return remove;
}
EXPORT_SYMBOL_GPL(mpath_can_remove_head);

static void mpath_remove_head_work(struct work_struct *work)
{
	struct mpath_head *mpath_head = container_of(to_delayed_work(work),
			struct mpath_head, remove_work);
	struct module *drv_module = mpath_head->drv_module;

	mpath_head->mpdt->remove_head(mpath_head);
	module_put(drv_module);
}

void mpath_remove_disk(struct mpath_head *mpath_head)
{
	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		struct gendisk *disk = mpath_head->disk;

		/*
		 * requeue I/O after MPATH_HEAD_DISK_LIVE has been cleared
		 * to allow multipath to fail all I/O.
		 */
		mpath_schedule_requeue_work(mpath_head);

		mpath_head_del_cdev(mpath_head);
		mpath_synchronize(mpath_head);
		del_gendisk(disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_remove_disk);

void mpath_put_disk(struct mpath_head *mpath_head)
{
	if (!mpath_head->disk)
		return;

	/* make sure all pending bios are cleaned up */
	kblockd_schedule_work(&mpath_head->requeue_work);
	flush_work(&mpath_head->requeue_work);
	flush_work(&mpath_head->partition_scan_work);
	put_disk(mpath_head->disk);
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

int mpath_alloc_head_disk(struct mpath_head *mpath_head,
			struct queue_limits *lim, int numa_node)
{
	/* Do limited sanity checks on template */
	if (!mpath_head->mpdt->ioctl_begin ^ !mpath_head->mpdt->ioctl_finish)
		return -EINVAL;

	if (!mpath_head->mpdt->add_cdev ^ !mpath_head->mpdt->del_cdev)
		return -EINVAL;

	if (!mpath_head->mpdt->add_cdev ^ !mpath_head->mpdt->cdev_ioctl)
		return -EINVAL;

	mpath_head->disk = blk_alloc_disk(lim, numa_node);
	if (IS_ERR(mpath_head->disk))
		return PTR_ERR(mpath_head->disk);

	mpath_head->disk->private_data = mpath_head;
	mpath_head->disk->fops = &mpath_ops;

	INIT_DELAYED_WORK(&mpath_head->remove_work, mpath_remove_head_work);
	mpath_head->delayed_removal_secs = 0;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_head->disk->state);

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head_disk);

void mpath_device_set_live(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	int ret;

	if (!mpath_head->disk)
		return;

	if (!test_and_set_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		dev_set_drvdata(disk_to_dev(mpath_head->disk), mpath_head);
		ret = device_add_disk(mpath_head->parent, mpath_head->disk,
				mpath_head->disk_groups);
		if (ret) {
			clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags);
			return;
		}

		mpath_head_add_cdev(mpath_head);
		queue_work(mpath_wq, &mpath_head->partition_scan_work);
	}

	mpath_add_sysfs_link(mpath_head);

	mutex_lock(&mpath_head->lock);
	if (mpath_path_is_optimized(mpath_head, mpath_device)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_head->srcu);
		for_each_online_node(node)
			__mpath_find_path(mpath_head, node);
		srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_head->lock);

	mpath_synchronize(mpath_head);
	mpath_schedule_requeue_work(mpath_head);
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *mpath_attrs[] = {
	&dummy_attr,
	NULL
};

static bool multipath_sysfs_group_visible(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct gendisk *disk = dev_to_disk(dev);

	return is_mpath_disk(disk);
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(multipath_sysfs)

const struct attribute_group mpath_attr_group = {
	.name           = "multipath",
	.attrs		= mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),
};
EXPORT_SYMBOL_GPL(mpath_attr_group);

const struct attribute_group *mpath_device_groups[] = {
	&mpath_attr_group,
	NULL
};
EXPORT_SYMBOL_GPL(mpath_device_groups);

ssize_t mpath_iopolicy_show(struct mpath_iopolicy *mpath_iopolicy, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		mpath_iopolicy_names[mpath_read_iopolicy(mpath_iopolicy)]);
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_show);

static void mpath_iopolicy_update(struct mpath_iopolicy *mpath_iopolicy,
		int iopolicy)
{
	int old_iopolicy = READ_ONCE(mpath_iopolicy->iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(mpath_iopolicy->iopolicy, iopolicy);

	pr_info("iopolicy changed from %s to %s\n",
		mpath_iopolicy_names[old_iopolicy],
		mpath_iopolicy_names[iopolicy]);
}

bool mpath_iopolicy_store(struct mpath_iopolicy *mpath_iopolicy,
				const char *buf, size_t count)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, mpath_iopolicy_names[i])) {
			mpath_iopolicy_update(mpath_iopolicy, i);
			return true;
		}
	}

	return false;
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_store);

ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device,
			struct mpath_iopolicy *mpath_iopolicy, char *buf)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	int node, srcu_idx;
	nodemask_t numa_nodes;
	struct mpath_device *current_mpath_dev;

	if (mpath_read_iopolicy(mpath_iopolicy) != MPATH_IOPOLICY_NUMA)
		return 0;

	nodes_clear(numa_nodes);

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	for_each_node(node) {
		current_mpath_dev =
			srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);
		if (current_mpath_dev == mpath_device)
			node_set(node, numa_nodes);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&numa_nodes));
}
EXPORT_SYMBOL_GPL(mpath_numa_nodes_show);

ssize_t mpath_delayed_removal_secs_show(struct mpath_head *mpath_head,
					char *buf)
{
	int ret;

	mutex_lock(&mpath_head->lock);
	ret = sysfs_emit(buf, "%u\n", mpath_head->delayed_removal_secs);
	mutex_unlock(&mpath_head->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_show);

ssize_t mpath_delayed_removal_secs_store(struct mpath_head *mpath_head,
			const char *buf, size_t count)
{
	ssize_t ret;
	int sec;

	ret = kstrtouint(buf, 0, &sec);
	if (ret < 0)
		return ret;

	mutex_lock(&mpath_head->lock);
	mpath_head->delayed_removal_secs = sec;
	if (sec)
		set_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	else
		clear_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	mutex_unlock(&mpath_head->lock);

	/*
	 * Ensure that update to MPATH_HEAD_QUEUE_IF_NO_PATH is seen
	 * by its reader.
	 */
	mpath_synchronize(mpath_head);

	return count;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_store);

void mpath_add_sysfs_link(struct mpath_head *mpath_head)
{
	struct device *target;
	struct device *source;
	int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct mpath_device *mpath_device;

	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_head->disk->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;
	srcu_idx = srcu_read_lock(&mpath_head->srcu);

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (!test_bit(GD_ADDED, &mpath_device->disk->state))
			continue;

		if (test_and_set_bit(MPATH_DEVICE_SYSFS_ATTR_LINK,
					&mpath_device->flags))
			continue;

		target = disk_to_dev(mpath_device->disk);
		source = disk_to_dev(mpath_head->disk);
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, "multipath",
				&target->kobj, dev_name(target));

		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_head->disk),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK,
					&mpath_device->flags);
		} else {
			dev_info(source, "Created multipath sysfs link to %s\n",
					mpath_device->disk->disk_name);
		}
	}

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}
EXPORT_SYMBOL_GPL(mpath_add_sysfs_link);

void mpath_remove_sysfs_link(struct mpath_device *mpath_device)
{
	struct device *target;
	struct kobject *mpath_gd_kobj;
	struct mpath_head *mpath_head = mpath_device->mpath_head;

	if (!test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
		return;

	target = disk_to_dev(mpath_device->disk);
	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;

	sysfs_remove_link_from_group(mpath_gd_kobj, "multipath",
			dev_name(target));

	clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
}
EXPORT_SYMBOL_GPL(mpath_remove_sysfs_link);

int mpath_head_init(struct mpath_head *mpath_head)
{
	INIT_LIST_HEAD(&mpath_head->dev_list);
	mutex_init(&mpath_head->lock);
	refcount_set(&mpath_head->refcount, 1);

	INIT_WORK(&mpath_head->partition_scan_work,
		multipath_partition_scan_work);
	INIT_WORK(&mpath_head->requeue_work, mpath_requeue_work);
	spin_lock_init(&mpath_head->requeue_lock);
	bio_list_init(&mpath_head->requeue_list);

	return init_srcu_struct(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_head_init);

static int __init mpath_init(void)
{
	mpath_wq = alloc_workqueue("mpath-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!mpath_wq)
		return -ENOMEM;
	return 0;
}

static void __exit mpath_exit(void)
{
	destroy_workqueue(mpath_wq);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmultipath");
