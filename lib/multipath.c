// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 * Copyright (c) 2026 Oracle and/or its affiliates.
 */
#include <linux/bsg.h>
#include <linux/module.h>
#include <linux/multipath.h>
#include <linux/wait_bit.h>
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
	WRITE_ONCE(*iopolicy, policy);

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

int mpath_add_device(struct mpath_device *mpath_device, struct gendisk *disk,
		int numa_node, atomic_t *nr_active)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;

	if (!disk || !nr_active)
		return -EINVAL;

	mpath_device->disk = disk;
	mpath_device->numa_node = numa_node;
	mpath_device->nr_active = nr_active;
	mutex_lock(&mpath_head->lock);
	list_add_tail_rcu(&mpath_device->siblings, &mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);

	if (cancel_delayed_work(&mpath_head->remove_work))
		module_put(mpath_head->drv_module);

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_add_device);

bool mpath_delete_device(struct mpath_device *mpath_device)
{
	bool empty;

	mutex_lock(&mpath_device->mpath_head->lock);
	list_del_rcu(&mpath_device->siblings);
	empty = list_empty(&mpath_device->mpath_head->dev_list);
	mutex_unlock(&mpath_device->mpath_head->lock);

	mpath_synchronize(mpath_device->mpath_head);

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

void mpath_clear_paths(struct mpath_head *mpath_head)
{
	int node;

	for_each_node(node)
		rcu_assign_pointer(mpath_head->current_path[node], NULL);
}
EXPORT_SYMBOL_GPL(mpath_clear_paths);

void mpath_revalidate_paths(struct mpath_head *mpath_head)
{
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
		    (mpath_read_iopolicy(mpath_head) ==
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

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		depth = atomic_read(mpath_device->nr_active);

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
	if (unlikely(mpath_path_is_disabled(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, node);
	if (unlikely(!mpath_path_is_optimized(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, node);
	return mpath_device;
}

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head)
{
	enum mpath_iopolicy_e iopolicy = mpath_read_iopolicy(mpath_head);

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
		trace_block_bio_remap(bio, disk_devt(mpath_head->disk),
				      bio->bi_iter.bi_sector);
		submit_bio_noacct(bio);
	} else if (mpath_available_path(mpath_head)) {
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");

		spin_lock_irq(&mpath_head->requeue_lock);
		bio_list_add(&mpath_head->requeue_list, bio);
		spin_unlock_irq(&mpath_head->requeue_lock);
		atomic_long_inc(&mpath_head->requeue_no_usable_path_cnt);
	} else {
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");

		bio_io_error(bio);
		atomic_long_inc(&mpath_head->fail_no_avail_path_cnt);
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

void mpath_put_head(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (refcount_dec_and_test(&mpath_head->refcount))
		wake_up_var(refcount);
}
EXPORT_SYMBOL_GPL(mpath_put_head);

void mpath_head_uninit(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (!refcount_dec_and_test(refcount))
		wait_var_event(refcount, !refcount_read(refcount));
	cleanup_srcu_struct(&mpath_head->srcu);
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

static int mpath_bdev_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct mpath_head *mpath_head = mpath_gendisk_to_head(disk);
	int srcu_idx, ret = -EWOULDBLOCK;
	struct mpath_device *mpath_device;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		if (mpath_device->disk->fops->get_unique_id)
			ret = mpath_device->disk->fops->get_unique_id(
					mpath_device->disk, id, type);
		else
			ret = 0; /* referencing __dm_get_unique_id() */
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_bdev_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;
	struct mpath_head *mpath_head = mpath_gendisk_to_head(disk);
	struct mpath_device *mpath_device;
	int srcu_idx, err;

	/*
	 * This check is duplicated from sd_ioctl() as we don't pass the
	 * partition bdev to fops->ioctl. That is not yet possible as the
	 * per-path disk is hidden and, as such, does not have partitions
	 * scanned.
	 */
	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO))
		return -ENOIOCTLCMD;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (!mpath_device) {
		err = -EWOULDBLOCK;
		goto out_unlock;
	}

	if (!mpath_device->disk->fops->ioctl) {
		err = -ENOTTY;
		goto out_unlock;
	}

	err = mpath_device->disk->fops->ioctl(
			mpath_device->disk->part0, mode, cmd, arg);
out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return err;
}

static int mpath_bdev_getgeo(struct gendisk *disk, struct hd_geometry *geo)
{
	struct mpath_head *mpath_head = mpath_gendisk_to_head(disk);
	int srcu_idx, ret = -EWOULDBLOCK;
	struct mpath_device *mpath_device;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device) {
		if (mpath_device->disk->fops->getgeo)
			ret = mpath_device->disk->fops->getgeo(
					mpath_device->disk, geo);
		else
			ret = -ENOTTY; /* See blkdev_getgeo */
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
	.submit_bio	= mpath_bdev_submit_bio,
	.ioctl		= mpath_bdev_ioctl,
	/*
	 * Both NVMe and SCSI use generic blkdev_compat_ptr_ioctl, so would
	 * avoid their custom compat_ioctl implementation.
	 */
	.compat_ioctl	= blkdev_compat_ptr_ioctl,
	.get_unique_id	= mpath_bdev_get_unique_id,
	.getgeo		= mpath_bdev_getgeo,
};
EXPORT_SYMBOL_GPL(mpath_ops);

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
	unsigned long delay;
	bool remove = false;

	mutex_lock(&mpath_head->lock);
	/*
	 * Ensure that no one could remove this module while the head
	 * remove work is pending.
	 */
	if (mpath_head_queue_if_no_path(mpath_head) &&
	    !check_mul_overflow(mpath_head->delayed_removal_secs, HZ, &delay) &&
	    try_module_get(mpath_head->drv_module)) {
		mod_delayed_work(mpath_wq, &mpath_head->remove_work, delay);
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
		mpath_synchronize(mpath_head);
		mpath_schedule_requeue_work(mpath_head);

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
	mpath_head->disk = NULL;
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

int mpath_alloc_head_disk(struct mpath_head *mpath_head,
			struct queue_limits *lim, int numa_node)
{
	struct gendisk *disk;

	if (!mpath_head->disk_groups || !mpath_head->parent ||
	     !mpath_head->iopolicy || mpath_head->disk)
		return -EINVAL;

	disk = blk_alloc_disk(lim, numa_node);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	mpath_head->disk = disk;
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

ssize_t mpath_iopolicy_show(enum mpath_iopolicy_e *iopolicy, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		mpath_iopolicy_names[READ_ONCE(*iopolicy)]);
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_show);

static void mpath_iopolicy_update(enum mpath_iopolicy_e *iopolicy,
		int new)
{
	int old = READ_ONCE(*iopolicy);

	if (old == new)
		return;

	WRITE_ONCE(*iopolicy, new);

	pr_info("iopolicy changed from %s to %s\n",
		mpath_iopolicy_names[old],
		mpath_iopolicy_names[new]);
}

bool mpath_iopolicy_store(enum mpath_iopolicy_e *iopolicy, const char *buf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, mpath_iopolicy_names[i])) {
			mpath_iopolicy_update(iopolicy, i);
			return true;
		}
	}

	return false;
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_store);

ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device, char *buf)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	int node, srcu_idx;
	nodemask_t numa_nodes;
	struct mpath_device *current_mpath_dev;

	if (mpath_read_iopolicy(mpath_head) != MPATH_IOPOLICY_NUMA)
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
	unsigned int sec;
	ssize_t ret;

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

static enum rq_end_io_ret mpath_bsg_end_clone(struct request *clone,
					    blk_status_t error,
					    const struct io_comp_batch *iob)
{
	struct request *orig = clone->end_io_data;
	//if (error)
	//WARN_ON_ONCE(1);
	pr_err("%s clone=%pS bio=%pS len=%d tag=%d error=%d iob=%pS end_io_data=%pS orig=%pS tag=%d\n",
			__func__, clone, clone->bio, blk_rq_bytes(clone), clone->tag, error, iob, clone->end_io_data, orig, orig->tag);
	blk_mq_end_request(orig, error);
	pr_err("%s1 clone=%pS bio=%pS\n",
			__func__, clone, clone->bio);
	return RQ_END_IO_FREE;
}

static blk_status_t mpath_bsg_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct request_queue *q = hctx->queue;
	struct mpath_head *mpath_head = q->queuedata;
	struct mpath_device *mpath_device;
	struct device *dev = mpath_head->parent;
	struct request *rq = bd->rq;
	blk_status_t blk_sts;
	int srcu_idx;

	pr_err("%s q=%pS hctx=%pS bd=%pS mpath_head=%pS rq=%pS\n",
		__func__, q, hctx, bd, mpath_head, rq);
	pr_err("%s0 rq=%pS end_io=%pS\n",
		__func__, rq, rq->end_io);

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	pr_err("%s1 q=%pS hctx=%pS bd=%pS mpath_head=%pS mpath_device=%pS\n",
		__func__, q, hctx, bd, mpath_head, mpath_device);



	if (likely(mpath_device)) {
		struct request *clone;
		struct block_device *bdev = mpath_device->disk->part0;
		struct request_queue *q = bdev_get_queue(bdev);
		struct bio *clone_bio;
		int ret;

	//	pr_err("%s2 mpath_head=%pS rq=%pS end_io=%pS\n",
	//		__func__, mpath_head, rq, rq->end_io);


		clone = blk_mq_alloc_request(q, rq->cmd_flags | REQ_NOMERGE,
			BLK_MQ_REQ_NOWAIT);
		
		if (IS_ERR(clone)) {
			pr_err("%s3 clone=%pS mpath_device=%pS bdev=%pS q=%pS rq=%pS\n",
				__func__, clone, mpath_device, bdev, q, rq);
			if (clone == ERR_PTR(-EAGAIN))
				blk_sts = BLK_STS_RESOURCE;
			else
				blk_sts = BLK_STS_IOERR;
			goto out_unlock;
		}

		ret = blk_rq_prep_clone(clone, rq, NULL, GFP_KERNEL,
				      NULL, NULL);
	
		if (ret) {
			pr_err("%s4 clone=%pS ret=%d bdev=%pS q=%pS rq=%pS\n",
				__func__, clone, ret, bdev, q, rq);
			blk_sts = BLK_STS_IOERR;
			goto out_unlock;
		}

		clone_bio = clone->bio;

		if (clone_bio) {
			pr_err("%s5.0 clone=%pS bio=%pS bi_vcnt=%d\n", __func__, clone, clone_bio, clone_bio->bi_vcnt);


			if (clone_bio->bi_vcnt) {
				struct bio_vec *bi_io_vec = clone_bio->bi_io_vec;
				pr_err("%s5.1 bi_io_vec=%pS\n", __func__, bi_io_vec);
				if (bi_io_vec) {
					pr_err("%s5.2 bv_page=%pS bv_len=%d bv_offset=%d\n",
						__func__, bi_io_vec->bv_page, bi_io_vec->bv_len, bi_io_vec->bv_offset);
					print_hex_dump(KERN_WARNING, "bio mpath_bsg_queue_rq ", DUMP_PREFIX_OFFSET, 16, 1,
		  				page_to_virt(bi_io_vec->bv_page), bi_io_vec->bv_len, true);
				}
			}

		}

		clone->end_io = mpath_bsg_end_clone;
		clone->end_io_data = rq;

		pr_err("%s5.3 clone=%pS len=%d tag=%d bio=%pS rq=%pS tag=%d bdev q=%pS bio=%pS\n",
			__func__, clone, blk_rq_bytes(clone), clone->tag, clone->bio, rq, rq->tag, q, rq->bio);
		ret = blk_insert_cloned_request(clone);
		pr_err("%s5.4 clone=%pS tag=%d rq=%pS tag=%d bdev q=%pS\n",
			__func__, clone, clone->tag, rq, rq->tag, q);
		switch (ret) {
		case BLK_STS_OK:
			break;
		case BLK_STS_RESOURCE:
		case BLK_STS_DEV_RESOURCE:
			pr_err_once("%s6.1 calling blk_rq_unprep_clone clone=%pS\n", __func__, clone);
			blk_rq_unprep_clone(clone);
			pr_err_once("%s6.2 calling blk_mq_cleanup_rq clone=%pS\n", __func__, clone);
			blk_mq_cleanup_rq(clone);
			pr_err_once("%s6.3 calling blk_mq_free_request clone=%pS\n", __func__, clone);
			blk_mq_free_request(clone);
			pr_err_once("%s6.4 called blk_mq_free_request clone=%pS\n", __func__, clone);
			blk_sts = BLK_STS_RESOURCE;
			goto out_unlock;
		default:
			pr_err("%s7 clone=%pS ret=%d bdev=%pS q=%pS from blk_insert_cloned_request rq=%pS\n",
				__func__, clone, ret, bdev, q, rq);
			/* must complete clone in terms of original request */
			BUG();
		}

	} else if (mpath_available_path(mpath_head)) {
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");

		blk_sts = BLK_STS_TRANSPORT;
	} else {
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");

		blk_sts = BLK_STS_IOERR;
	}

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	pr_err("%s10 q=%pS hctx=%pS bd=%pS mpath_head=%pS mpath_device=%pS blk_sts=%d\n",
		__func__, q, hctx, bd, mpath_head, mpath_device, blk_sts);
	return blk_sts;

	#ifdef dsdsdd
	struct request *req = bd->rq;
	struct bsg_set *bset =
		container_of(q->tag_set, struct bsg_set, tag_set);
	blk_status_t sts = BLK_STS_IOERR;
	int ret;

	blk_mq_start_request(req);

	if (!get_device(dev))
		return BLK_STS_IOERR;

	if (!bsg_prepare_job(dev, req))
		goto out;

	ret = bset->job_fn(blk_mq_rq_to_pdu(req));
	if (!ret)
		sts = BLK_STS_OK;

out:
	put_device(dev);
	return sts;
	#endif
}

bool mpath_is_bsg_request(struct request *rq)
{
	return rq->end_io == mpath_bsg_end_clone;
}
EXPORT_SYMBOL_GPL(mpath_is_bsg_request);

static void mpath_bsg_complete(struct request *rq)
{
	pr_err("%s rq=%pS\n", __func__, rq);
}

static const struct blk_mq_ops mpath_bsg_mq_ops = {
	.queue_rq		= mpath_bsg_queue_rq,
//	.init_request		= bsg_init_rq,
//	.exit_request		= bsg_exit_rq,
	.complete		= mpath_bsg_complete,
//	.timeout		= bsg_timeout,
};
extern int scsi_bsg_sg_io_fn(struct request_queue *q, struct sg_io_v4 *hdr,
		bool open_for_write, unsigned int timeout);

int mpath_setup_bsg(struct mpath_head *mpath_head, const char *name)
{
	struct device *bsg_dev = &mpath_head->bsg_dev;
	struct queue_limits lim;
	struct blk_mq_tag_set *set;
	int ret = -ENOMEM;

	blk_set_stacking_limits(&lim);

	device_initialize(bsg_dev);

	bsg_dev->parent = get_device(mpath_head->parent);

	dev_set_name(bsg_dev, "%s", name);

	if (device_add(bsg_dev)) {
		BUG();
	}

	set = &mpath_head->tag_set;
	set->ops = &mpath_bsg_mq_ops;
	set->nr_hw_queues = 1;
	set->queue_depth = 128;
	set->numa_node = NUMA_NO_NODE;
	set->cmd_size = 1280;//sizeof(struct bsg_job);
	set->flags = BLK_MQ_F_BLOCKING;
	if (blk_mq_alloc_tag_set(set)) {
		pr_err("%s2\n", __func__);
		goto out_tag_set;
	}

	mpath_head->bsg_q = blk_mq_alloc_queue(set, &lim, mpath_head);
	if (IS_ERR(mpath_head->bsg_q)) {
		pr_err("%s3\n", __func__);
		ret = PTR_ERR(mpath_head->bsg_q);
		goto out_queue;
	}

	blk_queue_rq_timeout(mpath_head->bsg_q, BLK_DEFAULT_SG_TIMEOUT);

	pr_err("%s3.1 mpath_head->bsg_q=%pS\n", __func__, mpath_head->bsg_q);
	pr_err("%s3.2 &mpath_head->bsg_dev=%pS\n", __func__, &mpath_head->bsg_dev);
	pr_err("%s3.3 name=%s\n", __func__, name);
	mpath_head->bsg_device = bsg_register_queue(mpath_head->bsg_q, &mpath_head->bsg_dev, name, scsi_bsg_sg_io_fn, NULL);
	if (IS_ERR(mpath_head->bsg_device)) {
		pr_err("%s4\n", __func__);
		ret = PTR_ERR(mpath_head->bsg_device);
		goto out_cleanup_queue;
	}

	pr_err("%s10\n", __func__);
	return 0;
out_cleanup_queue:
	blk_mq_destroy_queue(mpath_head->bsg_q);
	blk_put_queue(mpath_head->bsg_q);
out_queue:
	blk_mq_free_tag_set(set);
out_tag_set:
//	kfree(bset);

	return 0;
}

int mpath_head_init(struct mpath_head *mpath_head)
{
	memset(mpath_head, 0, sizeof(*mpath_head));
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
