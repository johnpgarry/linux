// SPDX-License-Identifier: GPL-2.0-only
/*
 */
#include <linux/bio.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <linux/libmpath.h>

/*
 * SCSI multipath will only allow 'NUMA' or 'round-robin' policy for IO.
 * In Future, if more apropriate IO-policy is introduced will be added
 * based on community feedback.
 */

static const char *mpath_iopolicy_names[] = {
	[MPATH_IOPOLICY_NUMA]	= "numa",
	[MPATH_IOPOLICY_RR]	= "round-robin",
	[MPATH_IOPOLICY_QD]	= "queue-depth",
};

static int iopolicy = MPATH_IOPOLICY_NUMA;
static int mpath_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	if (!val)
		return -EINVAL;
	if (!strncmp(val, "numa", 4))
		iopolicy = MPATH_IOPOLICY_NUMA;
	else if (!strncmp(val, "round-robin", 11))
		iopolicy = MPATH_IOPOLICY_RR;
	else if (!strncmp(val, "queue-depth", 11))
		iopolicy = MPATH_IOPOLICY_QD;
	else
		return -EINVAL;

	return 0;
}


static int mpath_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", mpath_iopolicy_names[iopolicy]);
}

module_param_call(iopolicy, mpath_set_iopolicy, mpath_get_iopolicy,
	&iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");

void multipath_partition_scan_work(struct work_struct *work)
{
	struct mpath_disk *mpath_disk =
		container_of(work, struct mpath_disk, partition_scan_work);

	pr_err("%s mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d\n",
		__func__, mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_disk->gd->state)))
		return;

	//mutex_lock(&head->disk->open_mutex);
//	pr_err("%s2 mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d calling bdev_disk_changed\n",
//		__func__, scsi_mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
//	bdev_disk_changed(mpath_disk->gd, false);
	//mutex_unlock(&head->disk->open_mutex);
}
EXPORT_SYMBOL_GPL(multipath_partition_scan_work);

bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	bool changed = false;
	int node;

	if (!mpath_disk)
		return changed;

	for_each_node(node) {
		if (mpath_device == rcu_access_pointer(mpath_disk->current_path[node])) {
			rcu_assign_pointer(mpath_disk->current_path[node], NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(mpath_clear_current_path);

/*
 * Search path based on iopolicy and numa node affinity
 * and return the scsi_device for that path
 */
struct mpath_device *__mpath_find_path(struct mpath_disk *mpath_disk, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	//struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;
	struct mpath_device *mpath_dev_found, *mpath_dev_fallback, *mpath_device;

	//pr_err("%s mpath_disk=%pS\n", __func__, scsi_mpath_disk);
	list_for_each_entry_rcu(mpath_device, &mpath_disk->dev_list, siblings) {
	//	pr_err("%s1 itering mpath_disk=%pS mpath_dev=%pS disabled=%d\n",
	//		__func__, scsi_mpath_disk, scsi_mpath_dev, scsi_mpath_is_disabled(scsi_mpath_dev->sdev));
		if (mpath_disk->is_disabled(mpath_device))
			continue;

		if (mpath_device->numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(mpath_disk->iopolicy) == MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, mpath_device->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_device->state) {
		case MPATH_STATE_OPTIMAL:
		    if (distance < found_distance) {
			    found_distance = distance;
			    mpath_dev_found = mpath_device;
		    }
		    break;
		case MPATH_STATE_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    mpath_dev_fallback = mpath_device;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!mpath_dev_found)
		mpath_dev_found = mpath_dev_fallback;

	if (mpath_dev_found)
		rcu_assign_pointer(mpath_disk->current_path[node], mpath_dev_found);

	return mpath_dev_found;
}
EXPORT_SYMBOL_GPL(__mpath_find_path);

static struct mpath_device *mpath_next_dev(struct mpath_disk *mpath_disk,
			struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_disk->dev_list, &mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_disk->dev_list, struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(struct mpath_disk *mpath_disk)
{
	struct mpath_device *mpath_device, *found = NULL;
	int node = numa_node_id();
	struct mpath_device *old = srcu_dereference(mpath_disk->current_path[node],
					       &mpath_disk->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_disk, node);

	if (list_is_singular(&mpath_disk->dev_list)) {
		if(mpath_disk->is_disabled(mpath_device))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_disk, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_disk, mpath_device)) {

		if (mpath_disk->is_disabled(mpath_device))
			continue;
		if (mpath_device->state == MPATH_STATE_OPTIMAL) {
			found = mpath_device;
			goto out;
		}
		if (mpath_device->state == MPATH_STATE_ACTIVE)
			found = mpath_device;
	}

//	scsi_mpath_dev = to_scsi_mpath_device(old);
	if (!mpath_disk->is_disabled(mpath_device) &&
	    (mpath_device->state == MPATH_STATE_OPTIMAL ||
	    (!found && mpath_device->state == MPATH_STATE_ACTIVE)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_disk->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(struct mpath_disk *mpath_disk)
{
	struct mpath_device *best_opt = NULL, *best_nonopt = NULL, *mpath_device;
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;
	unsigned int depth;

	pr_err("%s mpath_disk=%pS min_depth_nonopt=%d\n", __func__, mpath_disk, min_depth_nonopt);
	list_for_each_entry_srcu(mpath_device, &mpath_disk->dev_list, siblings,
				 srcu_read_lock_held(&mpath_disk->srcu)) {
	//	if (nvme_path_is_disabled(ns))
	//		continue;

		depth = atomic_read(&mpath_device->nr_active);

/*
		switch (ns->ana_state) {
		case NVME_ANA_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = ns;
			}
			break;
		case NVME_ANA_NONOPTIMIZED:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = ns;
			}
			break;
		default:
			break;
		}
*/
		if (min_depth_opt == 0)
			return best_opt;
	}

	return best_opt ? best_opt : best_nonopt;
}

static ssize_t mpath_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mpath_disk *mpath_disk =
		container_of(dev, struct mpath_disk, dev);

	return sysfs_emit(buf, "%s\n",
			  mpath_iopolicy_names[READ_ONCE(mpath_disk->iopolicy)]);
}

static void mpath_iopolicy_update(struct mpath_disk *mpath_disk,
		int iopolicy)
{
	int old_iopolicy = READ_ONCE(mpath_disk->iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(mpath_disk->iopolicy, iopolicy);

	/* iopolicy changes clear the mpath by design */
	//mutex_lock(&nvme_subsystems_lock);
	//list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
	//	scsi_mpath_disk_clear_ctrl_paths(ctrl);
	//mutex_unlock(&nvme_subsystems_lock);

	pr_notice("mpath_disk %d iopolicy changed from %s to %s\n",
			-1/*scsi_mpath_disk->index*/,
			mpath_iopolicy_names[old_iopolicy],
			mpath_iopolicy_names[iopolicy]);
}

static ssize_t mpath_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mpath_disk *mpath_disk =
		container_of(dev, struct mpath_disk, dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, mpath_iopolicy_names[i])) {
			mpath_iopolicy_update(mpath_disk, i);
			return count;
		}
	}

	return -EINVAL;
}

struct device_attribute mpath_iopolicy = \
		__ATTR(iopolicy, S_IRUGO | S_IWUSR, mpath_iopolicy_show, mpath_iopolicy_store);
EXPORT_SYMBOL_GPL(mpath_iopolicy);

ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device, char *buf)
{
	int node, srcu_idx;
	nodemask_t numa_nodes;
	struct mpath_device *current_mpath_dev;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;

	pr_err("%s mpath_device=%pS mpath_disk=%pS\n", __func__,
		mpath_device, mpath_disk);

	if (mpath_disk->iopolicy != MPATH_IOPOLICY_NUMA)
		return 0;

	nodes_clear(numa_nodes);

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	for_each_node(node) {
		current_mpath_dev = srcu_dereference(mpath_disk->current_path[node],
				&mpath_disk->srcu);
		pr_err("%s3 node=%d current_mpath_dev=%pS mpath_device=%pS\n",
			__func__, node, current_mpath_dev, mpath_device);
		if (current_mpath_dev == mpath_device)
			node_set(node, numa_nodes);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);

	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&numa_nodes));
}
EXPORT_SYMBOL_GPL(mpath_numa_nodes_show);

static struct mpath_device *mpath_numa_path(struct mpath_disk *mpath_disk)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;

	pr_err_once("%s mpath_disk=%pS\n", __func__, mpath_disk);
	mpath_device = srcu_dereference(mpath_disk->current_path[node], &mpath_disk->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_disk, node);
	//scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	pr_err_once("%s1 mpath_disk=%pS mpath_device=%pS\n", __func__, mpath_disk, mpath_device);
	if (unlikely(!mpath_disk->is_optimized(mpath_device)))
		return __mpath_find_path(mpath_disk, node);
	return mpath_device;
}

struct mpath_device *mpath_find_path(struct mpath_disk *mpath_disk)
{
	//struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	pr_err_once("%s mpath_disk=%pS iopolicy=%d\n", __func__, mpath_disk, READ_ONCE(mpath_disk->iopolicy));
	switch (READ_ONCE(mpath_disk->iopolicy)) {
	case MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_disk);
	case MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_disk);
	default:
		return mpath_numa_path(mpath_disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_find_path);

void mpath_revalidate_path(struct gendisk *disk, sector_t capacity)
{
	struct mpath_disk *mpath_disk;
	int srcu_idx;
	int node;

	mpath_disk = disk->private_data;
	pr_err("%s disk=%pS mpath_disk=%pS\n", __func__, disk, mpath_disk);


	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	pr_err("%s3 srcu_idx=%d\n", __func__, srcu_idx);
	#if 0
	list_for_each_entry_rcu(sdev, &scsi_mpath_disk->dev_list, mpath_dev_entry) {
		if (capacity != get_capacity(sdev->scsi_mpath_dev->gd))
			clear_bit(SCSI_MPATH_DISK_LIVE, &scsi_mpath_dev->flags);
	}
	#endif
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	pr_err("%s4 srcu_idx=%d\n", __func__, srcu_idx);

	for_each_node(node) {
		pr_err("%s5 node=%d\n", __func__, node);
		rcu_assign_pointer(mpath_disk->current_path[node], NULL);
	}
	pr_err("%s6 calling kblockd_schedule_work\n", __func__);
	kblockd_schedule_work(&mpath_disk->requeue_work);
	pr_err("%s6.1 called kblockd_schedule_work\n", __func__);
}
EXPORT_SYMBOL_GPL(mpath_revalidate_path);

void mpath_requeue_work(struct work_struct *work)
{
	struct mpath_disk *mpath_disk =
	    container_of(work, struct mpath_disk, requeue_work);
	__maybe_unused
	struct bio *bio, *next;

	pr_err("%s mpath_disk=%pS\n", __func__, mpath_disk);

	spin_lock_irq(&mpath_disk->requeue_lock);
	next = bio_list_get(&mpath_disk->requeue_list);
	spin_unlock_irq(&mpath_disk->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}
EXPORT_SYMBOL_GPL(mpath_requeue_work);

static bool mpath_available_path(struct mpath_disk *mpath_disk)
{
	struct mpath_device *mpath_device;
	//struct scsi_device *sdev;

	list_for_each_entry_rcu(mpath_device, &mpath_disk->dev_list, siblings) {
	//	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//	sdev = scsi_mpath_dev->sdev;
	//	if (scsi_device_online(sdev))
			return true;
	}
	return false;
}

void multipath_submit_bio(struct bio *bio)
{
	struct mpath_disk *mpath_disk = bio->bi_bdev->bd_disk->private_data;
	int srcu_idx;
	bool special = false;
	//struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	struct mpath_device *mpath_device;

	//WARN_ON_ONCE(1);
	if (bio->bi_iter.bi_size == 16384) {
		special = true;
		special = false;
	}

	/*
	 * The scsi device might be going away and the bio might be
	 * moved to a difference queue via blk_steal_bios(), so we
	 * need to use bio_split pool from the original queue to
	 * allocate the bvecs from.
	 */
	if (special)
		pr_err("%s bio=%pS bi size=%d mpath_disk=%pS bio->bi_bdev=%pS\n",
			__func__, bio, bio->bi_iter.bi_size, mpath_disk, bio->bi_bdev);
	bio = bio_split_to_limits(bio);
	if (special)
		pr_err("%s1 bio=%pS mpath_disk=%pS called bio_split_to_limits\n",
			__func__, bio, mpath_disk);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_device = mpath_find_path(mpath_disk);
	if (likely(mpath_device)) {
		bio_set_dev(bio, mpath_device->gd->part0);
		bio->bi_opf |= REQ_MPATH;
		atomic_inc(&mpath_device->nr_total);
		if (special)
			pr_err("%s3 bio=%pS bio->bi_bdev=%pS called bio_set_dev calling submit_bio_noacct\n",
				__func__, bio, bio->bi_bdev);
		submit_bio_noacct(bio);
		if (special)
			pr_err("%s4 bio=%pS called submit_bio_noacct\n",
				__func__, bio);
	//	BUG();
	} else if (mpath_available_path(mpath_disk)) {
		pr_err("No Usable Path - Requeing I/O \n");

		spin_lock_irq(&mpath_disk->requeue_lock);
		bio_list_add(&mpath_disk->requeue_list, bio);
		spin_unlock_irq(&mpath_disk->requeue_lock);
	} else {
		pr_err("No available path = Failing I/O \n");

		bio_io_error(bio);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}
EXPORT_SYMBOL_GPL(multipath_submit_bio);


static void mpath_free_disk(struct kref *ref)
{
	struct mpath_disk *mpath_disk =
		container_of(ref, struct mpath_disk, ref);
	//struct mpath_disk *mpath_disk = &scsi_mpath_disk->mpath_disk;

	pr_err("%s mpath_disk=%pS ref=%pS\n", __func__, mpath_disk, ref);
	#ifdef dsdsd
	nvme_mpath_put_disk(head);
	#else
	pr_err("%s1 mpath_disk=%pS ref=%pS calling put_disk gd=%pS\n",
		__func__, mpath_disk, ref, mpath_disk->gd);
	put_disk(mpath_disk->gd);
	#endif
//	ida_free(&sd_mpath_index_ida, scsi_mpath_disk->index);
	cleanup_srcu_struct(&mpath_disk->srcu);
//	nvme_put_subsystem(head->subsys);
//	mutex_lock(&scsi_mpath_disks_lock);
//	list_del(&scsi_mpath_disk->entry);
//	mutex_unlock(&scsi_mpath_disks_lock);

	pr_err("%s2 mpath_disk=%pS calling device_del\n", __func__, mpath_disk);
	device_del(&mpath_disk->dev);
	pr_err("%s3 mpath_disk=%pS calling put_device\n", __func__, mpath_disk);
	put_device(&mpath_disk->dev);
//	kfree(head->plids);
	pr_err("%s4 mpath_disk=%pS calling kfree\n", __func__, mpath_disk);
	kfree(mpath_disk);
}

void mpath_put_disk(struct mpath_disk *mpath_disk)
{
	kref_put(&mpath_disk->ref, mpath_free_disk);
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

int mpath_get_disk(struct mpath_disk *mpath_disk)
{
	if (!kref_get_unless_zero(&mpath_disk->ref)) {
		pr_err("%s1 mpath_disk=%pS ENXIO\n", __func__, mpath_disk);
		return -ENXIO;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_get_disk);

static int mpath_open(struct gendisk *disk, blk_mode_t mode)
{
	struct mpath_disk *mpath_disk = disk->private_data;
	pr_err("%s disk=%pS mpath_disk=%pS\n", __func__, disk, mpath_disk);

	return mpath_get_disk(mpath_disk);
}

static void mpath_release(struct gendisk *disk)
{
	struct mpath_disk *mpath_disk = disk->private_data;

	kref_put(&mpath_disk->ref, mpath_free_disk);
}


static int mpath_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct mpath_disk *mpath_disk = disk->private_data;
	int srcu_idx, ret = -EWOULDBLOCK;
	struct mpath_device *mpath_device;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_device = mpath_find_path(mpath_disk);
	if (mpath_device)
		ret = mpath_disk->get_unique_id(mpath_device, id, type);
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);

	return ret;
}

static int mpath_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;
	struct mpath_disk *mpath_disk = disk->private_data;
	struct mpath_device *mpath_device;
	int srcu_idx, err;

	pr_err("%s cmd=0x%x arg=%ld mpath_disk=%pS\n", __func__, cmd, arg, mpath_disk);
	
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_device = mpath_find_path(mpath_disk);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_device=%pS called scsi_find_path srcu_idx=%d mpath_device=%pS\n",
		__func__, cmd, arg, mpath_device, srcu_idx, mpath_device);
	if (!mpath_device)
		goto out_unlock;

	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO)) {
		err = -ENOIOCTLCMD;
		goto out_unlock;
	}

	err = mpath_disk->ioctl(mpath_device, mode, cmd, arg);

out_unlock:
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	return err;
}

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.submit_bio	= multipath_submit_bio,
	.open		= mpath_open,
	.release	= mpath_release,
	.ioctl			= mpath_ioctl,
	.get_unique_id	= mpath_get_unique_id,
};
EXPORT_SYMBOL_GPL(mpath_ops);

static int mpath_generic_chr_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_disk *mpath_disk = container_of(cdev, struct mpath_disk, cdev);

	pr_err("%s cdev=%pS mpath_disk=%pS\n", __func__, cdev, mpath_disk);

	return mpath_get_disk(mpath_disk);
}

static void mpath_free(struct kref *ref)
{
	struct mpath_disk *mpath_disk =
		container_of(ref, struct mpath_disk, ref);
	pr_err("%s mpath_disk=%pS\n", __func__, mpath_disk);
}

static int mpath_generic_chr_release(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_disk *mpath_disk = container_of(cdev, struct mpath_disk, cdev);

	pr_err("%s cdev=%pS mpath_disk=%pS\n", __func__, cdev, mpath_disk);

	kref_put(&mpath_disk->ref, mpath_free);
	return 0;
}

static long mpath_generic_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_disk *mpath_disk = container_of(cdev, struct mpath_disk, cdev);
	struct scsi_device *sdev;
	struct mpath_device *mpath_device;
	fmode_t mode = file->f_mode;
	int srcu_idx, err;

	pr_err("%s cdev=%pS cmd=0x%x arg=%ld mpath_disk=%pS\n",
		__func__, cdev, cmd, arg, mpath_disk);

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_device = mpath_find_path(mpath_disk);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_disk=%pS called scsi_find_path srcu_idx=%d mpath_device=%pS\n",
		__func__, cmd, arg, mpath_disk, srcu_idx, mpath_device);
	if (!mpath_device)
		goto out_unlock;
	//scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//sdev = scsi_mpath_dev->sdev;
	pr_err("%s2 cmd=0x%x arg=%ld sdev=%pS\n", __func__, cmd, arg, sdev);

//	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO)) {
//		err = -ENOIOCTLCMD;
//		goto out_unlock;
//	}

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	err = mpath_disk->ioctl(mpath_device, mode, cmd, arg);

out_unlock:
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	return err;
}

static void mpath_cdev_rel(struct device *dev)
{
	dev_err(dev, "%s\n", __func__);
//	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt));
}

void mpath_cdev_del(struct cdev *cdev, struct device *cdev_device)
{
	dev_err(cdev_device, "%s cdev=%pS calling cdev_device_del\n", __func__, cdev);
	cdev_device_del(cdev, cdev_device);
	dev_err(cdev_device, "%s2 calling put_device cdev_device=%pS\n", __func__, cdev_device);
	put_device(cdev_device);
}
EXPORT_SYMBOL_GPL(mpath_cdev_del);

static const struct file_operations mpath_generic_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= mpath_generic_chr_open,
	.release	= mpath_generic_chr_release,
	.unlocked_ioctl	= mpath_generic_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

int mpath_disk_add_cdev(struct mpath_disk *mpath_disk)
{
	int ret, minor = 0 /*mpath_disk->index*/;

	mpath_disk->cdev_device.parent = &mpath_disk->dev;
	ret = dev_set_name(&mpath_disk->cdev_device, "smpg%d",
						minor);
	pr_err("%s called dev_set_name ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	//mpath_disk->cdev_device.devt = MKDEV(MAJOR(scsi_mpath_disk_chr_devt), minor);
	//mpath_disk->cdev_device.class = &scsi_mpath_generic_class;
	mpath_disk->cdev_device.release = mpath_cdev_rel;
	device_initialize(&mpath_disk->cdev_device);
	cdev_init(&mpath_disk->cdev, &mpath_generic_chr_fops);
	mpath_disk->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&mpath_disk->cdev, &mpath_disk->cdev_device);
	pr_err("%s1 called cdev_device_add ret=%d\n", __func__, ret);
	if (ret)
		put_device(&mpath_disk->cdev_device);
	return ret;
}
EXPORT_SYMBOL_GPL(mpath_disk_add_cdev);

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

	dev_err(dev, "%s dev=%pS disk=%pS fops=%pS\n", __func__, dev, disk, disk->fops);
	return disk->fops == &mpath_ops;
}

static bool multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);

	dev_err(dev, "%s dev=%pS\n", __func__, dev);
	return false;
}

DEFINE_SYSFS_GROUP_VISIBLE(multipath_sysfs)

const struct attribute_group mpath_attr_group = {
	.name           = "multipath",
	.attrs		= mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),
};

const struct attribute_group *mpath_device_groups[] = {
	&mpath_attr_group,
	NULL
};
EXPORT_SYMBOL_GPL(mpath_device_groups);

static int __init mpath_init(void)
{
	pr_err("%s\n", __func__);
	
	return 0;
}

static void __exit mpath_exit(void)
{
	pr_err("%s\n", __func__);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmpath");
