// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 * Copyright (c) 2025 Oracle and/or its affiliates.
 */
#include <linux/bio.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <linux/multipath.h>

static int mpath_head_add_cdev(struct mpath_head *mpath_head);
static void mpath_free_disk(struct kref *ref);
static dev_t mpath_head_chr_devt;

#define SCSI_MPATH_DISK_MINORS		(1U << MINORBITS)

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
int mpath_set_iopolicy(const char *val, const struct kernel_param *kp)
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
EXPORT_SYMBOL_GPL(mpath_set_iopolicy);

int mpath_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", mpath_iopolicy_names[iopolicy]);
}
EXPORT_SYMBOL_GPL(mpath_get_iopolicy);

module_param_call(iopolicy, mpath_set_iopolicy, mpath_get_iopolicy,
	&iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");

void mpath_start_request(struct request *req)
{
	struct mpath_request *mpath_request = blk_mq_rq_to_pdu(req);
	struct mpath_device *mpath_device = mpath_request->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct gendisk *disk = mpath_head->disk;

	if ((READ_ONCE(mpath_head->mpath_subsys->iopolicy) == MPATH_IOPOLICY_QD) &&
	    !(mpath_request->flags & MPATH_REQ_CNT_ACTIVE)) {
		atomic_inc(&mpath_device->nr_active);
		mpath_request->flags |= MPATH_REQ_CNT_ACTIVE;
	}

	if (!blk_queue_io_stat(disk->queue) || blk_rq_is_passthrough(req) ||
	    (mpath_request->flags & MPATH_REQ_IO_STATS))
		return;

	mpath_request->flags |= MPATH_REQ_IO_STATS;
	mpath_request->start_time = bdev_start_io_acct(disk->part0, req_op(req),
						      jiffies);
}
EXPORT_SYMBOL_GPL(mpath_start_request);

void mpath_end_request(struct request *req)
{
	struct mpath_request *mpath_request = blk_mq_rq_to_pdu(req);
	struct mpath_device *mpath_device = mpath_request->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct gendisk *disk = mpath_head->disk;

	//pr_err("%s req=%pS bio=%pS cmd=%pS sdev=%pS\n", __func__, req, req->bio, scmd, sdev);
	if (mpath_request->flags & MPATH_REQ_CNT_ACTIVE) {
		atomic_dec_if_positive(&mpath_device->nr_active);
		mpath_request->flags &= ~MPATH_REQ_CNT_ACTIVE;
	}

	if (!(mpath_request->flags & MPATH_REQ_IO_STATS))
		return;
	bdev_end_io_acct(disk->part0, req_op(req),
			 blk_rq_bytes(req) >> SECTOR_SHIFT,
			  mpath_request->start_time);
	mpath_request->flags &= ~MPATH_REQ_IO_STATS;
}
EXPORT_SYMBOL_GPL(mpath_end_request);

bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	bool changed = false;
	int node;

	if (!mpath_head)
		return changed;

	for_each_node(node) {
		if (mpath_device == rcu_access_pointer(mpath_head->current_path[node])) {
			rcu_assign_pointer(mpath_head->current_path[node], NULL);
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
struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	//struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;
	struct mpath_device *mpath_dev_found, *mpath_dev_fallback, *mpath_device;

	//pr_err("%s mpath_head=%pS\n", __func__, scsi_mpath_head);
	list_for_each_entry_rcu(mpath_device, &mpath_head->dev_list, siblings) {
	//	pr_err("%s1 itering mpath_head=%pS mpath_dev=%pS disabled=%d\n",
	//		__func__, scsi_mpath_head, scsi_mpath_dev, scsi_mpath_is_disabled(scsi_mpath_dev->sdev));
		if (mpath_head->mpdt->is_disabled(mpath_device)) {

			continue;
		}

		if (mpath_device->numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(mpath_head->mpath_subsys->iopolicy) == MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, mpath_device->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_device->state) {
		case MPATH_STATE_OPTIMIZED:
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
		rcu_assign_pointer(mpath_head->current_path[node], mpath_dev_found);

	return mpath_dev_found;
}
EXPORT_SYMBOL_GPL(__mpath_find_path);

static struct mpath_device *mpath_next_dev(struct mpath_head *mpath_head,
			struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_head->dev_list, &mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_head->dev_list, struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device, *found = NULL;
	int node = numa_node_id();
	struct mpath_device *old = srcu_dereference(mpath_head->current_path[node],
					       &mpath_head->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_head, node);

	if (list_is_singular(&mpath_head->dev_list)) {
		if(mpath_head->mpdt->is_disabled(mpath_device))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_head, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_head, mpath_device)) {

		if (mpath_head->mpdt->is_disabled(mpath_device))
			continue;
		if (mpath_device->state == MPATH_STATE_OPTIMIZED) {
			found = mpath_device;
			goto out;
		}
		if (mpath_device->state == MPATH_STATE_ACTIVE)
			found = mpath_device;
	}

//	scsi_mpath_dev = to_scsi_mpath_device(old);
	if (!mpath_head->mpdt->is_disabled(mpath_device) &&
	    (mpath_device->state == MPATH_STATE_OPTIMIZED ||
	    (!found && mpath_device->state == MPATH_STATE_ACTIVE)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(struct mpath_head *mpath_head)
{
	struct mpath_device *best_opt = NULL, *mpath_device;
	__maybe_unused struct mpath_device *best_nonopt = NULL;
	unsigned int min_depth_opt = UINT_MAX;//, min_depth_nonopt = UINT_MAX;
	unsigned int depth;

	//pr_err("%s mpath_head=%pS min_depth_opt=%d\n", __func__, mpath_head, min_depth_opt);
	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_head->mpdt->is_disabled(mpath_device)) {
			pr_err("%s0 mpath_device=%pS ->state=%d is_disabled\n",
				__func__, mpath_device, mpath_device->state);
			continue;
		}

		depth = atomic_read(&mpath_device->nr_active);

		switch (mpath_device->state) {
		case MPATH_STATE_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = mpath_device;
			}
			break;
//		case NVME_ANA_NONOPTIMIZED:
//			if (depth < min_depth_nonopt) {
//				min_depth_nonopt = depth;
//				best_nonopt = ns;
//			}
//			break;
		default:
			pr_err("%s1 mpath_device=%pS ->state=%d\n",
				__func__, mpath_device, mpath_device->state);
			break;
		}

		if (min_depth_opt == 0) {
	//		pr_err_ratelimited("%s2 min_depth_opt=0 best_opt=%pS\n", __func__, best_opt);
			return best_opt;
		}
	}

//	pr_err_ratelimited("%s3 min_depth_opt=%d best_opt=%pS\n", __func__, min_depth_opt, best_opt);
	return best_opt;
}

static struct mpath_device *mpath_numa_path(struct mpath_head *mpath_head)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;

	pr_err_once("%s mpath_head=%pS\n", __func__, mpath_head);
	mpath_device = srcu_dereference(mpath_head->current_path[node], &mpath_head->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_head, node);
	//scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	pr_err_once("%s1 mpath_head=%pS mpath_device=%pS\n", __func__, mpath_head, mpath_device);
	if (unlikely(!mpath_head->mpdt->is_optimized(mpath_device)))
		return __mpath_find_path(mpath_head, node);
	return mpath_device;
}

struct mpath_device *mpath_find_path(struct mpath_head *mpath_head)
{
	//struct scsi_mpath_head *scsi_mpath_head = to_scsi_mpath_head(mpath_head);
	pr_err_once("%s mpath_head=%pS iopolicy=%d\n", __func__, mpath_head, READ_ONCE(mpath_head->mpath_subsys->iopolicy));
	switch (READ_ONCE(mpath_head->mpath_subsys->iopolicy)) {
	case MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_head);
	case MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_head);
	default:
		return mpath_numa_path(mpath_head);
	}
}
EXPORT_SYMBOL_GPL(mpath_find_path);

static bool mpath_available_path(struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device;

	//pr_err("%s mpath_head=%pS MPATH_DISK_LIVE=%d\n", __func__, mpath_head, test_bit(MPATH_DISK_LIVE, &mpath_head->flags));
	if (!test_bit(MPATH_DISK_LIVE, &mpath_head->flags))
		return false;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (!mpath_head->mpdt->is_disabled(mpath_device))
			return true;
	}

	return false;
}

static void multipath_submit_bio(struct bio *bio)
{
	struct mpath_head *mpath_head = bio->bi_bdev->bd_disk->private_data;
	int srcu_idx;
	bool special = false;
	//struct scsi_mpath_head *scsi_mpath_head = to_scsi_mpath_head(mpath_head);
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
		pr_err("%s bio=%pS bi size=%d mpath_head=%pS bio->bi_bdev=%pS\n",
			__func__, bio, bio->bi_iter.bi_size, mpath_head, bio->bi_bdev);
	bio = bio_split_to_limits(bio);
	if (special)
		pr_err("%s1 bio=%pS mpath_head=%pS called bio_split_to_limits\n",
			__func__, bio, mpath_head);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (likely(mpath_device)) {
		bio_set_dev(bio, mpath_device->disk->part0);
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
	} else if (mpath_available_path(mpath_head)) {
		pr_err_ratelimited("No Usable Path - Requeing I/O \n");

		spin_lock_irq(&mpath_head->requeue_lock);
		bio_list_add(&mpath_head->requeue_list, bio);
		spin_unlock_irq(&mpath_head->requeue_lock);
	} else {
		dev_err(mpath_head->parent, "No available path = Failing I/O \n");

		bio_io_error(bio);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}

static int mpath_open(struct gendisk *disk, blk_mode_t mode)
{
	struct mpath_head *mpath_head = disk->private_data;
	pr_err("%s disk=%pS mpath_head=%pS\n", __func__, disk, mpath_head);

	return mpath_get_disk(mpath_head);
}

static void mpath_release(struct gendisk *disk)
{
	struct mpath_head *mpath_head = disk->private_data;

	kref_put(&mpath_head->ref, mpath_free_disk);
}

static int mpath_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct mpath_head *mpath_head = disk->private_data;
	int srcu_idx, ret = -EWOULDBLOCK;
	struct mpath_device *mpath_device;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device)
		ret = mpath_head->mpdt->get_unique_id(mpath_device, id, type);
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;
	struct mpath_head *mpath_head = disk->private_data;
	struct mpath_device *mpath_device;
	int srcu_idx, err;

	pr_err("%s cmd=0x%x arg=%ld mpath_head=%pS\n", __func__, cmd, arg, mpath_head);
	
	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_device=%pS called scsi_find_path srcu_idx=%d mpath_device=%pS\n",
		__func__, cmd, arg, mpath_device, srcu_idx, mpath_device);
	if (!mpath_device)
		goto out_unlock;

	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO)) {
		err = -ENOIOCTLCMD;
		goto out_unlock;
	}

	err = mpath_head->mpdt->ioctl(mpath_device, mode, cmd, arg);

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
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
	struct mpath_head *mpath_head = container_of(cdev, struct mpath_head, cdev);

	pr_err("%s cdev=%pS mpath_head=%pS\n", __func__, cdev, mpath_head);

	return mpath_get_disk(mpath_head);
}

static void mpath_free(struct kref *ref)
{
	struct mpath_head *mpath_head =
		container_of(ref, struct mpath_head, ref);
	pr_err("%s mpath_head=%pS\n", __func__, mpath_head);
}

static int mpath_generic_chr_release(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head = container_of(cdev, struct mpath_head, cdev);

	pr_err("%s cdev=%pS mpath_head=%pS\n", __func__, cdev, mpath_head);

	kref_put(&mpath_head->ref, mpath_free);
	return 0;
}

static long mpath_generic_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head = container_of(cdev, struct mpath_head, cdev);
	struct scsi_device *sdev;
	struct mpath_device *mpath_device;
	fmode_t mode = file->f_mode;
	int srcu_idx, err;

	pr_err("%s cdev=%pS cmd=0x%x arg=%ld mpath_head=%pS\n",
		__func__, cdev, cmd, arg, mpath_head);

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_head=%pS called scsi_find_path srcu_idx=%d mpath_device=%pS\n",
		__func__, cmd, arg, mpath_head, srcu_idx, mpath_device);
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
	err = mpath_head->mpdt->ioctl(mpath_device, mode, cmd, arg);

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return err;
}

static void mpath_cdev_rel(struct device *dev)
{
//	dev_err(dev, "%s\n", __func__);
//	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt));
	pr_err("%s dev=%pS\n", __func__, dev);
}

void mpath_cdev_del(struct cdev *cdev, struct device *cdev_device)
{
	struct device *dd1 = cdev_device->parent;
	struct kobject *dd1_kobj = NULL;
	struct kref *dd1_kobj_kref = NULL;

	if (dd1)
		dd1_kobj = &dd1->kobj;
	if (dd1_kobj)
		dd1_kobj_kref = &dd1_kobj->kref;

	pr_err("%s cdev=%pS calling cdev_device_del cdev_device=%pS parent=%pS ref=%d\n",
		__func__, cdev, cdev_device, dd1, dd1_kobj_kref ? kref_read(dd1_kobj_kref) : -1);
	cdev_device_del(cdev, cdev_device);
	pr_err("%s2 calling put_device cdev_device=%pS parent=%pS ref=%d\n",
		__func__, cdev_device, dd1, dd1_kobj_kref ? kref_read(dd1_kobj_kref) : -1);
	put_device(cdev_device);
	pr_err("%s3 called put_device cdev_device=%pS parent=%pS ref=%d\n",
		__func__, cdev_device, dd1, dd1_kobj_kref ? kref_read(dd1_kobj_kref) : -1);
}
EXPORT_SYMBOL_GPL(mpath_cdev_del);

static const struct file_operations mpath_generic_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= mpath_generic_chr_open,
	.release	= mpath_generic_chr_release,
	.unlocked_ioctl	= mpath_generic_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static void multipath_partition_scan_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
		container_of(work, struct mpath_head, partition_scan_work);

	pr_err("%s mpath_head=%pS GD_SUPPRESS_PART_SCAN=%d\n",
		__func__, mpath_head, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_head->disk->state));
	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_head->disk->state)))
		return;

	mutex_lock(&mpath_head->disk->open_mutex);
	pr_err("%s2 mpath_head=%pS GD_SUPPRESS_PART_SCAN=%d calling bdev_disk_changed\n",
		__func__, mpath_head, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_head->disk->state));
	bdev_disk_changed(mpath_head->disk, false);
	mutex_unlock(&mpath_head->disk->open_mutex);
}

void mpath_requeue_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
	    container_of(work, struct mpath_head, requeue_work);
	__maybe_unused
	struct bio *bio, *next;

	pr_err("%s mpath_head=%pS\n", __func__, mpath_head);

	spin_lock_irq(&mpath_head->requeue_lock);
	next = bio_list_get(&mpath_head->requeue_list);
	spin_unlock_irq(&mpath_head->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}
EXPORT_SYMBOL_GPL(mpath_requeue_work);

struct mpath_head *mpath_alloc_head(const struct mpath_head_template *mpdt, int privsize)
{
	struct mpath_head *mpath_head;
	int ret;

	mpath_head = kzalloc(sizeof(*mpath_head) + privsize, GFP_KERNEL);
	pr_err("%s5 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS mpath_head=%pS mpath_device=%pS\n",
		__func__, NULL, NULL, NULL, NULL, mpath_head, NULL);
	if (!mpath_head)
		return NULL;
	//scsi_mpath_head = to_scsi_mpath_head(mpath_head);
	//pr_err("%s mpath_head=%pS scsi_mpath_head=%pS\n",
	//	__func__, NULL, mpath_head);
	//mpath_device->mpath_head = mpath_head;
	//mpath_head->is_disabled = scsi_mpath_is_disabled;
	//mpath_head->is_optimized = scsi_mpath_is_optimized;
	//mpath_head->get_unique_id = scsi_mpath_get_unique_id;
	//mpath_head->ioctl = scsi_mpath_ioctl;
	mpath_head->mpdt = mpdt;

	//INIT_LIST_HEAD(&scsi_mpath_head->entry);
	INIT_LIST_HEAD(&mpath_head->dev_list);
	INIT_WORK(&mpath_head->partition_scan_work, multipath_partition_scan_work);
	pr_err("%s6\n", __func__);
	mutex_init(&mpath_head->lock);
	kref_init(&mpath_head->ref);

	//mpath_head->dev.class = mpdt->class; //&scsi_mpath_head_class;

	// see mpath_head_add_cdev()

//	scsi_mpath_head->dev.release = scsi_mpath_head_release;
//	scsi_mpath_head->dev.groups = scsi_mpath_groups;
//	pr_err("%s7 &mpath_head->dev=%pS name=%s\n", __func__, &mpath_head->dev, name);

	
//	dev_set_name(&mpath_head->dev, "smpd%d", 0/* fixme scsi_mpath_head->index*/);
//	disk_count++;
//	device_initialize(&mpath_head->dev);
//	set_dev_node(&mpath_head->dev, node_id);
//	ret = dev_set_name(&mpath_head->dev, name);
	pr_err("%s8 ret=%d from dev_set_name\n", __func__, ret);



	//pr_err("%s12.3.1 device_id_str=%s len=%zd scsi_mpath_head=%pS\n",
	//	__func__, sdev->scsi_mpath_dev->device_id_str,
	//	strlen(sdev->scsi_mpath_dev->device_id_str),
	//	scsi_mpath_head);

	//sprintf(scsi_mpath_head->wwid, sdev->scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN);

	//pr_err("%s13 ret=%d after bio_list_init sdev->scsi_mpath_dev=%pS scsi_mpath_head->wwid=%s\n",
	//	__func__, ret, sdev->scsi_mpath_dev, scsi_mpath_head->wwid);
	//list_add_tail(&mpath_device->siblings, &mpath_head->dev_list);

	//mutex_lock(&scsi_mpath_heads_lock);
	//list_add_tail(&scsi_mpath_head->entry, &scsi_mpath_heads_list);

	pr_err("%s16\n", __func__);
	//mutex_unlock(&scsi_mpath_heads_lock);

	pr_err("%s16 out\n", __func__);

	return mpath_head;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head);


int mpath_alloc_head_disk(struct mpath_head *mpath_head)
{
	struct queue_limits lim;
	int ret;

	blk_set_stacking_limits(&lim);
	pr_err("%s8 mpath_head->parent=%pS\n", __func__, mpath_head->parent);

	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL;
	lim.max_zone_append_sectors = 0;
	lim.dma_alignment = 3;

	mpath_head->disk = blk_alloc_disk(&lim, dev_to_node(mpath_head->parent));
	pr_err("%s9 dev=%pS sdev->scsi_mpath_dev=%pS mpath_head->disk=%pS\n", __func__, NULL, NULL, mpath_head->disk);
	if (IS_ERR(mpath_head->disk))
		return -ENOMEM;

//	sprintf(mpath_head->disk->disk_name, dev_name(&mpath_head->dev));
	mpath_head->disk->private_data = mpath_head;
	mpath_head->disk->fops = &mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_head->disk->state);
	//sprintf(mpath_head->disk->disk_name, "smpd%d", scsi_mpath_head->index);

//	dev_err(&mpath_head->dev, "%s10 calling device_add for &mpath_head->dev\n", __func__);
//	ret = device_add(&mpath_head->dev); // see nvme_init_subsystem()
//	pr_err("%s11 called device_add ret=%d\n", __func__, ret);
//	if (ret)
//		return NULL;

	ret = init_srcu_struct(&mpath_head->srcu);
	pr_err("%s12 ret=%d mpath_head=%pS mpath_head->disk->major=%d first_minor=%d minors=%d\n",
		__func__, ret, mpath_head, mpath_head->disk->major, mpath_head->disk->first_minor, mpath_head->disk->minors);
	if (ret)
		return ret;

	INIT_WORK(&mpath_head->requeue_work, mpath_requeue_work);
	pr_err("%s12.1 ret=%d after INIT_WORK mpath_head=%pS\n", __func__, ret, mpath_head);
	spin_lock_init(&mpath_head->requeue_lock);
	pr_err("%s12.2 ret=%d after spin_lock_init mpath_head=%pS\n", __func__, ret, mpath_head);
	bio_list_init(&mpath_head->requeue_list);
	pr_err("%s12.3 ret=%d after bio_list_init mpath_head=%pS sdev->scsi_mpath_dev=%pS\n",
		__func__, ret, NULL, NULL);
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head_disk);

void mpath_device_set_live(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	int ret;

	pr_err("%s disk=%pS MPATH_DISK_LIVE=%d\n",
		__func__, mpath_head, test_bit(MPATH_DISK_LIVE, &mpath_head->flags));

	if (!test_and_set_bit(MPATH_DISK_LIVE, &mpath_head->flags)) {
//		struct device *dd1 = &mpath_head->dev;
//		struct kobject *dd1_kobj = &dd1->kobj;
//		struct kref *dd1_kobj_kref = &dd1_kobj->kref;
		struct device *parent = mpath_head->parent;
		dev_err(parent, "%s0 calling device_add_disk &mpath_head->dev=%pS ref count=%d parent=%pS\n",
			__func__, NULL /* dd1->parent */, 0 /* kref_read(dd1_kobj_kref) */, parent);

		ret = device_add_disk(parent, mpath_head->disk, mpath_head->mpdt->device_groups);
		pr_err("%s1 called device_add_disk ret=%d ref count=%d\n", __func__, ret, 0/*kref_read(dd1_kobj_kref)*/);
		if (ret) {
			clear_bit(MPATH_DISK_LIVE, &mpath_head->flags);
			return;
		}
		pr_err("%s2 calling scsi_mpath_head_add_cdev partition_scan_work\n", __func__);
		//mpath_head_add_cdev(mpath_head);
		pr_err("%s3 calling kblockd_schedule_work partition_scan_work\n", __func__);
		kblockd_schedule_work(&mpath_head->partition_scan_work);
	}

	pr_info("Attached SCSI %s disk calling mpath_add_sysfs_link\n", "fixme");

	mpath_add_sysfs_link(mpath_head);

	mutex_lock(&mpath_head->lock);
	if (mpath_head->mpdt->is_optimized(mpath_device)) { //checkme is proper CB
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_head->srcu);
		for_each_online_node(node)
			__mpath_find_path(mpath_head, node);
		srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_head->lock);

	synchronize_srcu(&mpath_head->srcu);
	kblockd_schedule_work(&mpath_head->requeue_work);
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

bool mpath_device_is_live(struct mpath_device *mpath_device)
{
	switch (READ_ONCE(mpath_device->state)) {
	case MPATH_STATE_OPTIMIZED:
	case MPATH_STATE_ACTIVE:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(mpath_device_is_live);

ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device, char *buf)
{
	int node, srcu_idx;
	nodemask_t numa_nodes;
	struct mpath_device *current_mpath_dev;
	struct mpath_head *mpath_head = mpath_device->mpath_head;

	pr_err("%s mpath_device=%pS mpath_head=%pS\n", __func__,
		mpath_device, mpath_head);

	if (mpath_head->mpath_subsys->iopolicy != MPATH_IOPOLICY_NUMA)
		return 0;

	nodes_clear(numa_nodes);

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	for_each_node(node) {
		current_mpath_dev = srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);
		pr_err("%s3 node=%d current_mpath_dev=%pS mpath_device=%pS\n",
			__func__, node, current_mpath_dev, mpath_device);
		if (current_mpath_dev == mpath_device)
			node_set(node, numa_nodes);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&numa_nodes));
}
EXPORT_SYMBOL_GPL(mpath_numa_nodes_show);

void mpath_add_sysfs_link(struct mpath_head *mpath_head)
{
	__maybe_unused struct device *target;
	__maybe_unused int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct mpath_device *mpath_device;

	pr_err("%s mpath_head=%pS GD_ADDED=%d\n",
		__func__, mpath_head, test_bit(GD_ADDED, &mpath_head->disk->state));
	pr_err("%s3 mpath_head->disk=%pS\n", __func__, mpath_head->disk);
	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_head->disk->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;
	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	pr_err("%s4 mpath_head->disk=%pS srcu_idx=%d\n",
		__func__, mpath_head->disk, srcu_idx);

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		pr_err("%s5 mpath_device=%pS\n", __func__, mpath_device);
		if (!mpath_device)
			continue;
		//scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
		pr_err("%s5.1 mpath_device=%pS mpath_dev->sdev=%pS\n", __func__, mpath_device, NULL);
	//	if (!scsi_mpath_dev->sdev)
//			continue;
		/*
		 * Ensure that ns path disk node is already added otherwise we
		 * may get invalid kobj name for target
		 */
	//	if (!sdev->request_queue)
	//		continue;
		pr_err("%s6.2 itering mpath_device=%pS mpath_device->disk=%pS checking GD_ADDED=%d\n",
			__func__, mpath_device, mpath_device->disk,
			test_bit(GD_ADDED, &mpath_device->disk->state));
		if (!test_bit(GD_ADDED, &mpath_device->disk->state))
			continue;

		/*
		 * Avoid creating link if it already exists for the given path.
		 * When path ana state transitions from optimized to non-
		 * optimized or vice-versa, the nvme_mpath_set_live() is
		 * invoked which in truns call this function. Now if the sysfs
		 * link already exists for the given path and we attempt to re-
		 * create the link then sysfs code would warn about it loudly.
		 * So we evaluate NVME_NS_SYSFS_ATTR_LINK flag here to ensure
		 * that we're not creating duplicate link.
		 * The test_and_set_bit() is used because it is protecting
		 * against multiple nvme paths being simultaneously added.
		 */
		pr_err("%s6.3 itering mpath_device=%pS mpath_device->disk=%pS GD_ADDED=%d checking SCSI_MPATH_SYSFS_ATTR_LINK=%d\n",
			__func__, mpath_device, mpath_device->disk,
			test_bit(GD_ADDED, &mpath_device->disk->state),
			test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags));
		if (test_and_set_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
			continue;

		pr_err("%s7.3 itering mpath_device=%pS mpath_device->disk=%pS\n",
			__func__, mpath_device, mpath_device->disk);
		target = disk_to_dev(mpath_device->disk);
		pr_err("%s7.4 itering mpath_device=%pS mpath_device->disk=%pS target=%pS scsi_mpath_attr_group.name=%s\n",
			__func__, mpath_device, mpath_device->disk, target, "multipath");
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, "multipath",
				&target->kobj, dev_name(target));
		pr_err("%s7.5 called sysfs_add_link_to_group rc=%d mpath_gd_kobj=%pS &target->kobj=%pS dev_name=%s\n",
			__func__, rc, mpath_gd_kobj, &target->kobj, dev_name(target));
		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_head->disk),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
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

	pr_err("%s mpath_device=%pS mpath_head=%pS SCSI_MPATH_SYSFS_ATTR_LINK set=%d\n",
		__func__, mpath_device, mpath_head,
		test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags));
	if (!test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
		return;

	target = disk_to_dev(mpath_device->disk);
	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;

	pr_err("%s2 calling sysfs_remove_link_from_group mpath_gd_kobj=%pS dev_name(target)=%s\n",
		__func__, mpath_gd_kobj, dev_name(target));

	sysfs_remove_link_from_group(mpath_gd_kobj, "multipath",
			dev_name(target));

	pr_err("%s3 calling sysfs_delete_link mpath_gd_kobj=%pS dev_name(target)=%s\n",
		__func__, mpath_gd_kobj, dev_name(target));

	clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
}
EXPORT_SYMBOL_GPL(mpath_remove_sysfs_link);

int mpath_add_head(struct mpath_head *mpath_head)
{
	int ret = 0;
//	struct device *dd1 = &mpath_head->dev;
//	struct kobject *dd1_kobj = &dd1->kobj;
//	struct kref *dd1_kobj_kref = &dd1_kobj->kref;

//	dev_err(&mpath_head->dev, "%s calling device_add for &mpath_head->dev ref count=%d dd1=%pS mpath_head->parent=%pS\n",
//		__func__, kref_read(dd1_kobj_kref), dd1, mpath_head->parent);
//	ret = device_add(&mpath_head->dev); // see nvme_init_subsystem()
//	dev_err(&mpath_head->dev, "%s2 called device_add for &mpath_head->dev ref count=%d\n", __func__, kref_read(dd1_kobj_kref));

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_add_head);

void mpath_put_disk(struct mpath_head *mpath_head)
{
	kref_put(&mpath_head->ref, mpath_free_disk);
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

void mpath_revalidate_path(struct gendisk *disk, sector_t capacity)
{
	struct mpath_head *mpath_head;
	int srcu_idx;
	int node;

	mpath_head = disk->private_data;
	pr_err("%s disk=%pS mpath_head=%pS\n", __func__, disk, mpath_head);


	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	pr_err("%s3 srcu_idx=%d\n", __func__, srcu_idx);
	#if 0
	list_for_each_entry_rcu(sdev, &scsi_mpath_head->dev_list, mpath_dev_entry) {
		if (capacity != get_capacity(sdev->scsi_mpath_dev->gd))
			clear_bit(SCSI_MPATH_DISK_LIVE, &scsi_mpath_dev->flags);
	}
	#endif
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	pr_err("%s4 srcu_idx=%d\n", __func__, srcu_idx);

	for_each_node(node) {
		pr_err("%s5 node=%d\n", __func__, node);
		rcu_assign_pointer(mpath_head->current_path[node], NULL);
	}
	pr_err("%s6 calling kblockd_schedule_work\n", __func__);
	kblockd_schedule_work(&mpath_head->requeue_work);
	pr_err("%s6.1 called kblockd_schedule_work\n", __func__);
}
EXPORT_SYMBOL_GPL(mpath_revalidate_path);

void mpath_add_device(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	pr_err("%s mpath_device=%pS mpath_head=%pS\n", __func__, mpath_device, mpath_head);

	if (mpath_device_is_live(mpath_device)) {
		mpath_device->state = MPATH_STATE_OPTIMIZED;
		pr_err("%s calling scsi_mpath_set_live\n", __func__);
		mpath_device_set_live(mpath_device);
	}
}
EXPORT_SYMBOL_GPL(mpath_add_device);

static void mpath_free_disk(struct kref *ref)
{
	struct mpath_head *mpath_head =
		container_of(ref, struct mpath_head, ref);
	//struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;
	//struct device *dd1 = &mpath_head->dev;
	//struct kobject *dd1_kobj = &dd1->kobj;
	//struct kref *dd1_kobj_kref = &dd1_kobj->kref;

	pr_err("%s mpath_head=%pS dd1_kobj=%pS dd1_kobj_kref=%pS\n",
		__func__, mpath_head, NULL/* dd1_kobj */, NULL /* dd1_kobj_kref */);
	#ifdef dsdsd
	nvme_mpath_put_disk(head);
	#else
	pr_err("%s1 mpath_head=%pS ref=%pS calling put_disk gd=%pS ref count=%d dd1=%pS\n",
		__func__, mpath_head, ref, mpath_head->disk, 0 /* kref_read(dd1_kobj_kref) */, NULL /*dd1*/);
	put_disk(mpath_head->disk);
	pr_err("%s1.1 mpath_head=%pS ref=%pS called put_disk gd=%pS ref count=%d dd1=%pS\n",
		__func__, mpath_head, ref, mpath_head->disk, 0 /* kref_read(dd1_kobj_kref) */, NULL /* dd1*/);
	#endif
//	ida_free(&sd_mpath_index_ida, scsi_mpath_head->index);
	cleanup_srcu_struct(&mpath_head->srcu);
//	nvme_put_subsystem(head->subsys);
//	mutex_lock(&scsi_mpath_heads_lock);
//	list_del(&scsi_mpath_head->entry);
//	mutex_unlock(&scsi_mpath_heads_lock);

//	pr_err("%s2 mpath_head=%pS calling device_del ref count=%d dd1=%pS\n",
//		__func__, mpath_head, kref_read(dd1_kobj_kref), dd1);
//	device_del(&mpath_head->dev);
//	pr_err("%s3 mpath_head=%pS calling put_device ref count=%d dd1=%pS\n",
//		__func__, mpath_head, kref_read(dd1_kobj_kref), dd1);
//	put_device(&mpath_head->dev);
//	pr_err("%s3.1 mpath_head=%pS called put_device dd1=%pS\n",
//		__func__, mpath_head, dd1);
//	kfree(head->plids);
	pr_err("%s4 mpath_head=%pS calling kfree\n", __func__, mpath_head);
	kfree(mpath_head);
}

void mpath_remove_device(struct mpath_device *mpath_device)
{
	bool last_path = false;
	struct mpath_head *mpath_head;
	
	pr_err("%s mpath_device=%pS\n", __func__, mpath_device);

	mpath_head = mpath_device->mpath_head;

	pr_err("%s1 mpath_device=%pS calling mpath_remove_sysfs_link\n",
		__func__, mpath_device);
	mpath_remove_sysfs_link(mpath_device);

	synchronize_srcu(&mpath_head->srcu);

	/* wait for concurrent submissions */
	if (mpath_clear_current_path(mpath_device))
		synchronize_srcu(&mpath_head->srcu);

	pr_err("%s2 mpath_device=%pS called scsi_mpath_remove_sysfs_link\n",
		__func__, mpath_device);
//	put_disk(sdev->scsi_mpath_dev->scsi_mpath_head->disk);
//	if (!sdev->is_shared)
//		return;

	/* Make sure All pending bio's are cleaned up */
	kblockd_schedule_work(&mpath_head->requeue_work);
	flush_work(&mpath_head->requeue_work);
	//put_disk(sdev->scsi_mpath_dev);

	mutex_lock(&mpath_head->lock);
	
	list_del_rcu(&mpath_device->siblings);
	pr_err("%s3 list_empty=%d\n", __func__, list_empty(&mpath_head->dev_list));
	if (list_empty(&mpath_head->dev_list)) {
//		if (!nvme_mpath_queue_if_no_path(ns->head))
//			list_del_init(&ns->head->entry);
		last_path = true;
	}
	mutex_unlock(&mpath_head->lock);

	pr_err("%s4 last_path=%d\n",
		__func__, last_path);

	if (last_path) {
		pr_err("%s5 MPATH_DISK_LIVE set=%d\n",
			__func__, test_bit(MPATH_DISK_LIVE, &mpath_head->flags));
		if (test_and_clear_bit(MPATH_DISK_LIVE, &mpath_head->flags)) {
			/*
			 * requeue I/O after NVME_NSHEAD_DISK_LIVE has been cleared
			 * to allow multipath to fail all I/O.
			 */
			kblockd_schedule_work(&mpath_head->requeue_work);

			mpath_cdev_del(&mpath_head->cdev, &mpath_head->cdev_device);
			synchronize_srcu(&mpath_head->srcu);
			pr_err("%s5.1 not calling device_del\n", __func__);
		//	device_del(&scsi_mpath_head->dev);???
			pr_err("%s5.2 calling del_gendisk mpath_head->disk=%pS\n", __func__, mpath_head->disk);
			del_gendisk(mpath_head->disk);
		}
		pr_err("%s6 calling put_disk on mpath_head->disk=%pS\n", __func__, mpath_head->disk);
		
	}
	pr_err("%s9 mpath_device=%pS calling mpath_put_disk\n", __func__, mpath_device);

	pr_err("%s10 mpath_device=%pS\n", __func__, mpath_device);
}
EXPORT_SYMBOL_GPL(mpath_remove_device);

int mpath_get_disk(struct mpath_head *mpath_head)
{
	if (!kref_get_unless_zero(&mpath_head->ref)) {
		pr_err("%s1 mpath_head=%pS ENXIO\n", __func__, mpath_head);
		return -ENXIO;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_get_disk);

__maybe_unused int mpath_head_add_cdev(struct mpath_head *mpath_head)
{
	int ret, minor = 0 /*mpath_head->index*/;

	// following can be moved to disk alloc code
	mpath_head->cdev_device.parent = mpath_head->parent;
	ret = dev_set_name(&mpath_head->cdev_device, "smpg%d",
						minor);
	pr_err("%s called dev_set_name ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	mpath_head->cdev_device.devt = MKDEV(MAJOR(mpath_head_chr_devt), minor);
	mpath_head->cdev_device.class = mpath_head->mpdt->cdev_class;
	mpath_head->cdev_device.release = mpath_cdev_rel;
	// following can be moved to disk alloc code
	device_initialize(&mpath_head->cdev_device);
	cdev_init(&mpath_head->cdev, &mpath_generic_chr_fops);
	mpath_head->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&mpath_head->cdev, &mpath_head->cdev_device);
	pr_err("%s1 called cdev_device_add ret=%d\n", __func__, ret);
	if (ret)
		put_device(&mpath_head->cdev_device);
	return ret;
}


static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *mpath_attrs[] = {
	&dummy_attr,
	NULL
};

bool is_mpath_head(struct gendisk *disk)
{
	return disk->fops == &mpath_ops;
}

static bool multipath_sysfs_group_visible(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct gendisk *disk = dev_to_disk(dev);

	dev_err(dev, "%s dev=%pS disk=%pS fops=%pS\n", __func__, dev, disk, disk->fops);
	return is_mpath_head(disk);
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
EXPORT_SYMBOL_GPL(mpath_attr_group);

const struct attribute_group *mpath_device_groups[] = {
	&mpath_attr_group,
	NULL
};
EXPORT_SYMBOL_GPL(mpath_device_groups);

ssize_t mpath_iopolicy_show(struct mpath_subsys *mpath_subsys, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  mpath_iopolicy_names[READ_ONCE(mpath_subsys->iopolicy)]);
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_show);

static void mpath_iopolicy_update(struct mpath_subsys *mpath_subsys,
		int iopolicy)
{
	int old_iopolicy = READ_ONCE(mpath_subsys->iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(mpath_subsys->iopolicy, iopolicy);

	/* iopolicy changes clear the mpath by design */
	//mutex_lock(&nvme_subsystems_lock);
	//list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
	//	scsi_mpath_head_clear_ctrl_paths(ctrl);
	//mutex_unlock(&nvme_subsystems_lock);

	pr_err("iopolicy changed from %s to %s\n",
			mpath_iopolicy_names[old_iopolicy],
			mpath_iopolicy_names[iopolicy]);
}

ssize_t mpath_iopolicy_store(struct mpath_subsys *mpath_subsys, const char *buf, size_t count)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, mpath_iopolicy_names[i])) {
			mpath_iopolicy_update(mpath_subsys, i);
			return count;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_store);

static int __init mpath_init(void)
{
	int err;
	pr_err("%s\n", __func__);
	err = alloc_chrdev_region(&mpath_head_chr_devt, 0, 1U << MINORBITS,
				     "mpath-generic");

	return err;
}

static void __exit mpath_exit(void)
{
	pr_err("%s\n", __func__);
	unregister_chrdev_region(mpath_head_chr_devt, 1U << MINORBITS);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmpath");
