// SPDX-License-Indentifier: GPL-2.0
/*
 * Copyright (c) 2024 Himanshu Madhani
 *
 * SCSI Multipath support using ALUA (Asymmetric Logical Unit Access)
 * capable devices.
 */

#include <linux/bio.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <linux/kmemleak.h>
#include <linux/hdreg.h>
#include <linux/sysfs.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dh.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_multipath.h>
#include <scsi/scsi_ioctl.h>
#include "scsi_alua.h"
#include "scsi_priv.h"

static dev_t mpath_head_chr_devt;

static DEFINE_IDA(scsi_mpath_index_ida);

#define SCSI_MPATH_DEVICE_IO_PENDING      0

MODULE_IMPORT_NS("SCSI_DH_ALUA");


bool scsi_multipath = false; //todo: turn off
static bool scsi_multipath_always_on;

static void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head);
static int scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head);

static int multipath_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	pr_err("%s val=%s\n", __func__, val);
	if (ret)
		return ret;

	if (scsi_multipath_always_on && !*arg) {
		pr_err("Can't disable multipath when multipath_always_on is configured.\n");
		*arg = true;
		return -EINVAL;
	}

	return 0;
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = multipath_param_set,
	.get = param_get_bool,
};

module_param_cb(enable, &multipath_param_ops, &scsi_multipath, 0444);
MODULE_PARM_DESC(multipath,
	"turn on native multipath support");

static int multipath_always_on_set(const char *val,
		const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	if (ret < 0)
		return ret;

	if (*arg)
		scsi_multipath = true;

	return 0;
}

static const struct kernel_param_ops multipath_always_on_ops = {
	.set = multipath_always_on_set,
	.get = param_get_bool,
};

module_param_cb(enable_always, &multipath_always_on_ops,
		&scsi_multipath_always_on, 0444);
MODULE_PARM_DESC(multipath_always_on,
	"create multipath node always even for no ALUA support");

static dev_t scsi_mpath_head_major;
static struct scsi_mpath_head *scsi_mpath_find_disk(struct scsi_device *sdev);

static LIST_HEAD(scsi_mpath_heads_list);
static DEFINE_MUTEX(scsi_mpath_heads_lock);

struct scsi_mpath_clone_bio {
//	union {
//		struct kiocb		*iocb;
//		struct task_struct	*waiter;
//	};
//	size_t			size;
//	atomic_t		ref;
//	unsigned int		flags;
	struct bio *master_bio;
	struct bio		clone ____cacheline_aligned_in_smp;
};

/* Check for path error */
static inline bool scsi_is_mpath_error(struct scsi_cmnd *scmd)
{
	#ifdef dsdsdsd
	struct request *req = scsi_cmd_to_rq(scmd);
	struct scsi_device *sdev = req->q->queuedata;

	if (sdev->handler && sdev->handler->prep_fn) {
		blk_status_t ret = sdev->handler->prep_fn(sdev, req);

		if (ret != BLK_STS_OK)
			return true;
	}

	return false;
	#else
	struct scsi_device *sdev = scmd->device;
	if (sdev->sdev_state == SDEV_TRANSPORT_OFFLINE)
		return true;
	return false;
	#endif
}

static const struct class scsi_mpath_generic_class = {
	.name = "scsi_mpath_generic",
};

static __maybe_unused void scsi_mpath_head_release1(struct device *dev)
{
	struct mpath_head *mpath_head = NULL;//container_of(dev, struct mpath_head, dev);
	dev_err(dev, "%s dev=%pS mpath_head=%pS\n", __func__, dev, mpath_head);

}

static const struct class scsi_mpath_disk_class = {
	.name = "scsi_mpath_disk",
	.dev_release	= scsi_mpath_head_release1,
	.dev_groups = scsi_mpath_groups,
};

static int scsi_multipath_sdev_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct mpath_device *mpath_device;
	int ret = -ENOMEM;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	scsi_mpath_dev = kzalloc(sizeof(*scsi_mpath_dev), GFP_KERNEL);
	if (!scsi_mpath_dev)
		return ret;
	scsi_mpath_dev->sdev = sdev;
	sdev->scsi_mpath_dev = scsi_mpath_dev;


	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_device->numa_node = dev_to_node(shost->dma_dev);
	pr_err("%s2 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS mpath_device=%pS\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, mpath_device);

	return 0;
}

static __maybe_unused void nvme_mpath_unfreeze(struct scsi_mpath_head *scsi_mpath_head)
{
	pr_err("%s mpath_head=%pS\n", __func__, scsi_mpath_head);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_mq_unfreeze_queue_nomemrestore(h->disk->queue);
}


void scsi_mpath_wait_freeze(struct scsi_mpath_head *scsi_mpath_head)
{
//	struct nvme_ns_head *h;

	pr_err("%s mpath_head=%pS\n", __func__, scsi_mpath_head);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_mq_freeze_queue_wait(h->disk->queue);
}

void scsi_mpath_start_freeze(struct scsi_mpath_head *scsi_mpath_head)
{
//	struct nvme_ns_head *h;

	pr_err("%s mpath_head=%pS\n", __func__, scsi_mpath_head);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_freeze_queue_start(h->disk->queue);
}

void scsi_mpath_start_request(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct mpath_request *mpath_request = &scmd->mpath_request;
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct gendisk *disk = mpath_head->disk;
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);

	if (mpath_qd_iopolicy(&scsi_mpath_head->iopolicy) &&
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
EXPORT_SYMBOL_GPL(scsi_mpath_start_request);

void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct mpath_request *mpath_request = &scmd->mpath_request;
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
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
EXPORT_SYMBOL_GPL(scsi_mpath_end_request);

void scsi_mpath_failover_req(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct gendisk *disk = mpath_head->disk;
	//struct scsi_mpath_head *scsi_mpath_head = mpath_to_priv_head(mpath_head);
	unsigned long flags;
	struct bio *clone = req->bio;
	struct bio *bio_loop; 

	struct scsi_mpath_clone_bio *scsi_mpath_clone_bio;
	struct bio *master_bio;

	scsi_mpath_clone_bio = container_of(clone, struct scsi_mpath_clone_bio, clone);
	master_bio = scsi_mpath_clone_bio->master_bio;


	pr_err("%s req=%pS clone=%pS master_bio=%pS online=%d was_reset=%d locked=%d mpath_device=%pS\n",
		__func__, req, clone, master_bio, scsi_device_online(sdev), sdev->was_reset, sdev->locked, mpath_device);
	pr_err("%s1 req=%pS blk_rq_bytes=%d clone=%pS bi_iter.bi_size=%d clone->bi_next=%pS\n",
		__func__, req, blk_rq_bytes(req), clone, clone->bi_iter.bi_size, clone->bi_next);

//	if (!scsi_device_online(sdev) || sdev->was_reset || sdev->locked)
//		return;

	mpath_clear_current_path(mpath_device);

	/*
	 * if we got device handler error, we know that device is alive but not
	 * ready to process command. kick off a requeue of scsi command and try
	 * other available path
	 */
//	pr_err("%s2 req=%pS clone=%pS scsi_is_mpath_error=%d mpath_device=%pS calling mpath_clear_current_path\n",
//		__func__, req, clone, scsi_is_mpath_error(scmd), mpath_device);
	if (scsi_is_mpath_error(scmd)) {
		/*
		 * Set flag as pending and requeue bio for retry on
		 * another path
		 */
//		pr_err("%s3 req=%pS clone=%pS scsi_is_mpath_error=%d mpath_device=%pS mpath_head=%pS\n",
//		__func__, req, clone, scsi_is_mpath_error(scmd), mpath_device, mpath_head);
	//	set_bit(SCSI_MPATH_DEVICE_IO_PENDING, &scsi_mpath_dev->flags);
//		pr_err("%s3.1 req=%pS bytes=%d clone=%pS bi_size=%d scsi_is_mpath_error=%d mpath_device=%pS shost->work_q=%pS\n",
//		__func__, req, blk_rq_bytes(req), clone, clone->bi_iter.bi_size, scsi_is_mpath_error(scmd), mpath_device, shost->work_q);
	//	queue_work(shost->work_q, &mpath_head->requeue_work);
	}

	/*
	 * following logic tries to steal bio, check if the bio has polled
	 * operation, if yes, then clear polled reqeust and reqeue bio
	 */
	spin_lock_irqsave(&mpath_head->requeue_lock, flags);
	for (bio_loop = master_bio; bio_loop; bio_loop = bio_loop->bi_next) {
	//	pr_err("%s4 looping bio_loop=%pS bi_size=%d bio_loop->bi_bdev=%pS part0=%pS disk->part0=%pS\n",
	//		__func__, bio_loop, bio_loop->bi_iter.bi_size, bio_loop->bi_bdev, part0, disk->part0);
		BUG_ON(bio_loop->bi_bdev != disk->part0);
	}
	#ifdef dssda_old
	blk_steal_bios(&mpath_head->requeue_list, req);
	#else
	bio_list_add(&mpath_head->requeue_list, master_bio);
	#endif

	spin_unlock_irqrestore(&mpath_head->requeue_lock, flags);

	/* End old request with clone detached and then release that io */
	req->bio = NULL;
	req->biotail = NULL;
	req->__data_len = 0;
	blk_mq_end_request(req, 0);
	bio_put(clone);

	scmd->result = 0;
//	pr_err("%s5 req=%pS bio=%pS calling blk_mq_end_request\n",
//		__func__, req, req->bio);
//	pr_err("%s5.1 req=%pS bio=%pS called blk_mq_end_request, calling kblockd_schedule_work\n",
//		__func__, req, req->bio);

	kblockd_schedule_work(&mpath_head->requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_req);

#if 0
void scsi_mpath_kick_requeue_lists(struct Scsi_Host *shost)

{
	struct scsi_mpath_head *scsi_mpath_head = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_dev, entry) {
		if (sdev->is_shared)
			continue;

		kblockd_schedule_work(&mpath_head->requeue_lock);
		if (sdev->sdev_state == SDEV_RUNNING)
			disk_uevent(sdev->scsi_mpath_dev, KOBJ_CHANGE);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}
#endif
static bool scsi_mpath_is_disabled(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	enum scsi_device_state sdev_state = sdev->sdev_state;

	/*
	 * if device multipath state is not set to LIVE
	 * then return true
	 */
	if (!mpath_device_is_live(mpath_device))
		return true;

	/*
	 * Do not treat DELETING as a disabled path as I/O should
	 * still be able to complete assuming that scsi_device is
	 * within timeout limit.
	 * Otherwise I/O will fail immeadiately and return to
	 * requeue list
	 */
	if (sdev_state == SDEV_RUNNING || sdev_state == SDEV_CANCEL)
		return false;

	return true;
}

static inline bool scsi_mpath_is_optimized(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//pr_err("%s mpath_dev=%pS\n", __func__, scsi_mpath_dev);

	if (!scsi_mpath_dev)
		return false;

	return (!scsi_device_online(scsi_mpath_dev->sdev) &&
	    ((mpath_device->state == MPATH_STATE_OPTIMIZED) ||
	     (mpath_device->state == MPATH_STATE_ACTIVE)));
}

static int scsi_mpath_ioctl(struct mpath_device *mpath_device, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg, int srcu_idx)
{
	int err;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	err = scsi_ioctl_block_when_processing_errors(sdev, cmd,
			(mode & BLK_OPEN_NDELAY));
	if (err)
		goto out;

	pr_err("%s3 cmd=0x%x arg=%ld sdev=%pS calling scsi_ioctl\n", __func__, cmd, arg, sdev);
	err = scsi_ioctl(sdev, mode & BLK_OPEN_WRITE, cmd, (void __user *)arg);
	pr_err("%s3.1 cmd=0x%x arg=%ld sdev=%pS called scsi_ioctl err=%d\n", __func__, cmd, arg, sdev, err);

out:
	mpath_head_read_unlock(mpath_head, srcu_idx);
	return err;
}

static __maybe_unused void scsi_mpath_head_release(struct device *dev)
{
	struct mpath_head *mpath_head = NULL;//container_of(dev, struct mpath_head, dev);
	dev_err(dev, "%s dev=%pS mpath_head=%pS\n", __func__, dev, mpath_head);
}

static struct bio_set scsi_mpath_bio_pool;

static void scsi_mpath_clone_end_io(struct bio *clone)
{
	struct scsi_mpath_clone_bio *scsi_mpath_clone_bio;
	struct bio *master_bio;

	scsi_mpath_clone_bio = container_of(clone, struct scsi_mpath_clone_bio, clone);
	master_bio = scsi_mpath_clone_bio->master_bio;

//	pr_err("%s clone=%pS scsi_mpath_clone_bio=%pS master_bio=%pS clone->bi_status=%d\n",
//		__func__, clone, scsi_mpath_clone_bio, master_bio, clone->bi_status);

	master_bio->bi_status = clone->bi_status;
	bio_put(clone);
	bio_endio(master_bio);
}

static struct bio *scsi_mpath_clone_bio(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct gendisk *bd_disk;
	struct mpath_head *mpath_head;
	struct bio *clone;
	struct scsi_mpath_clone_bio *scsi_mpath_clone_bio;

	//pr_err_once("%s bio=%pS bdev=%pS\n",
	//	__func__, bio, bdev);
	bd_disk = bdev->bd_disk;
	//pr_err_once("%s1 bio=%pS bdev=%pS bd_disk=%pS\n",
	//	__func__, bio, bdev, bd_disk);
	mpath_head = bd_disk->private_data;
	//pr_err_once("%s2 bio=%pS bdev=%pS bd_disk=%pS mpath_head=%pS\n",
	//	__func__, bio, bdev, bd_disk, mpath_head);

	clone = bio_alloc_clone(bio->bi_bdev, bio, GFP_NOWAIT, &scsi_mpath_bio_pool);

//	pr_err("%s9 bio=%pS clone=%pS\n", __func__, bio, clone);

	if (!clone)
		return NULL;

	clone->bi_end_io = scsi_mpath_clone_end_io;

	scsi_mpath_clone_bio = container_of(clone, struct scsi_mpath_clone_bio, clone);
//	pr_err("%s9.1 bio=%pS bi_end_io=%pS clone=%pS bi_end_io=%pS scsi_mpath_clone_bio=%pS\n",
//		__func__, bio, bio->bi_end_io, clone, clone->bi_end_io, scsi_mpath_clone_bio);
	scsi_mpath_clone_bio->master_bio = bio;

	return clone;
}

static int scsi_mpath_pr_register(struct mpath_device *mpath_device, u64 old_key, u64 new_key,
			u32 flags)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_register(bdev, old_key, new_key, flags);
}

static int scsi_mpath_pr_reserve(struct mpath_device *mpath_device, u64 key, enum pr_type type,
		u32 flags)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_reserve(bdev, key, type, flags);
}

static int scsi_mpath_pr_release(struct mpath_device *mpath_device, u64 key, enum pr_type type)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_release(bdev, key, type);
}

static int scsi_mpath_pr_preempt(struct mpath_device *mpath_device, u64 old_key, u64 new_key,
		enum pr_type type, bool abort)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_preempt(bdev, old_key, new_key, type, abort);
}

static int scsi_mpath_pr_clear(struct mpath_device *mpath_device, u64 key)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_clear(bdev, key);
}

static int scsi_mpath_pr_read_keys(struct mpath_device *mpath_device, struct pr_keys *keys_info)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_read_keys(bdev, keys_info);
}

static int scsi_mpath_pr_read_reservation(struct mpath_device *mpath_device,
				  struct pr_held_reservation *rsv)
{
	struct scsi_mpath_device *scsi_multipath_dev = to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_multipath_dev->sdev;
	struct block_device *bdev = mpath_device->disk->part0;

	pr_err("%s scsi_multipath_dev=%pS sdev=%pS\n", __func__, scsi_multipath_dev, sdev);

	if (!mpath_device->disk->fops->pr_ops)
		return -EOPNOTSUPP;

	return mpath_device->disk->fops->pr_ops->pr_read_reservation(bdev, rsv);
}

static void scsi_mpath_cdev_rel(struct device *dev)
{
//	dev_err(dev, "%s\n", __func__);
//	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt));
	dev_err(dev, "%s dev=%pS ida_free(nvme_ns_chr_minor_ida) for nvme, we don't really need this for SCSI so\n", __func__, dev);
}

static int scsi_mpath_add_cdev(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);
	unsigned int minor = scsi_mpath_head->index;
	int ret;

	pr_err("%s mpath_head=%pS\n", __func__, mpath_head);

	// following can be moved to disk alloc code
	mpath_head->cdev_device.parent = mpath_head->parent;
	ret = dev_set_name(&mpath_head->cdev_device, "smpg%d", minor);
	pr_err("%s called dev_set_name ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	mpath_head->cdev_device.devt = MKDEV(MAJOR(mpath_head_chr_devt), minor);
	mpath_head->cdev_device.class = &scsi_mpath_generic_class;
	mpath_head->cdev_device.release = scsi_mpath_cdev_rel;
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

static void scsi_mpath_del_cdev(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);

	pr_err("%s mpath_head=%pS scsi_mpath_head=%pS calling cdev_device_del\n", __func__, mpath_head, scsi_mpath_head);

	cdev_device_del(&mpath_head->cdev, &mpath_head->cdev_device);
	pr_err("%s2 mpath_head=%pS scsi_mpath_head=%pS calling put_device\n", __func__, mpath_head, scsi_mpath_head);
	put_device(&mpath_head->cdev_device);
}

static void scsi_mpath_free_head_xxx(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);
	struct kref *ref = &scsi_mpath_head->ref;

	pr_err("%s mpath_head=%pS scsi_mpath_head=%pS ref=%pS refcount=%d calling scsi_mpath_put_head\n",
		__func__, mpath_head, scsi_mpath_head, ref, refcount_read(&ref->refcount));
	scsi_mpath_put_head(scsi_mpath_head);
}

static enum mpath_iopolicy_e scsi_mpath_get_iopolicy(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);

	return mpath_read_iopolicy(&scsi_mpath_head->iopolicy);
}

static const struct mpath_pr_ops mapth_pr_ops = {
	#ifdef dsdsd
	.pr_register	= sd_pr_register,
	.pr_reserve	= sd_pr_reserve,
	.pr_release	= sd_pr_release,
	.pr_preempt	= sd_pr_preempt,
	.pr_clear	= sd_pr_clear,
	.pr_read_keys	= sd_pr_read_keys,
	.pr_read_reservation = sd_pr_read_reservation,
	#endif
	.pr_register	= scsi_mpath_pr_register,
	.pr_reserve	= scsi_mpath_pr_reserve,
	.pr_release	= scsi_mpath_pr_release,
	.pr_preempt	= scsi_mpath_pr_preempt,
	.pr_clear	= scsi_mpath_pr_clear,
	.pr_read_keys	= scsi_mpath_pr_read_keys,
	.pr_read_reservation = scsi_mpath_pr_read_reservation,
};

struct mpath_head_template smpdt = {
//	.class = &scsi_mpath_disk_class,
//	.cdev_class = &scsi_mpath_generic_class,
	.is_disabled = scsi_mpath_is_disabled,
	.is_optimized = scsi_mpath_is_optimized,
	.ioctl = scsi_mpath_ioctl,
	.device_groups = mpath_device_groups,
	.clone_bio = scsi_mpath_clone_bio,
	.pr_ops = &mapth_pr_ops,
	.add_cdev = scsi_mpath_add_cdev,
	.del_cdev = scsi_mpath_del_cdev,
	.get_iopolicy = scsi_mpath_get_iopolicy,
	.free_head = scsi_mpath_free_head_xxx,
};

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_dev_alloc(struct scsi_device *sdev, struct gendisk *disk)
{
//	struct queue_limits lim;
	int ret;
	struct scsi_mpath_head *scsi_mpath_head;
	struct Scsi_Host *shost = sdev->host;
	struct device *shost_dev = &shost->shost_dev;
	struct mpath_head *mpath_head;
	struct mpath_device *mpath_device;
	struct scsi_mpath_device *scsi_mpath_dev;
	char name[256];
	int index;
	size_t size = sizeof(*scsi_mpath_head);
	struct kref *ref;

	pr_err("%s sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS scsi_device_tpgs=%d\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev, scsi_device_tpgs(sdev));

	/*
	 * Add multipath disk only if scsi host supports multipath modparam
	 */
	if (!scsi_multipath) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s modparam scsi_multipath is set to false \n",
		    sdev->handler->name);
		return 0;
	}

	if (!scsi_device_tpgs(sdev) && !scsi_multipath_always_on) {
		sdev_printk(KERN_NOTICE, sdev, "tpgs are required for mpath support\n");
		return -ENODEV;
	}

	pr_err("%s1 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS calling scsi_multipath_sdev_init\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev);
	if (sdev->scsi_mpath_dev)
		return 0;
	scsi_multipath_sdev_init(sdev);
	pr_err("%s1.1 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS called scsi_multipath_sdev_init\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev);

	scsi_mpath_dev = sdev->scsi_mpath_dev;
	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_device->disk = disk;

	ret = scsi_mpath_unique_lun_id(sdev);
	if (ret < 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s sdev=%pS scsi_mpath_unique_lun_id failed\n", __func__, sdev);
		return ret;
	}

	pr_err("%s4 calling scsi_mpath_find_disk sdev=%pS\n", __func__, sdev);
	scsi_mpath_head = scsi_mpath_find_disk(sdev);
	pr_err("%s4.1 called scsi_mpath_find_disk sdev=%pS mpath_head=%pS\n", __func__, sdev, scsi_mpath_head);
	if (scsi_mpath_head) {
	//	struct mpath_subsys *mpath_subsys;

		mpath_head = &scsi_mpath_head->mpath_head;
		ref = &scsi_mpath_head->ref;

	//	mpath_subsys = mpath_head->mpath_subsys;
	//	mutex_lock(&mpath_subsys->lock);
		pr_err("%s4.1.1 scsi_mpath_head=%pS calling mpath_add_device ref=%pS refcount=%d\n",
			__func__, scsi_mpath_head, ref, refcount_read(&ref->refcount));
		mpath_add_device(mpath_head, mpath_device);
	//	mutex_unlock(&mpath_subsys->lock);
		return 0;
	}

	index = ida_alloc(&scsi_mpath_index_ida, GFP_KERNEL);
	pr_err("%s4.1 index=%d\n", __func__, index);
	if (index < 0)
		return scsi_mpath_head->index;
	snprintf(name, sizeof(name), "smpd%d", index);

	pr_err("%s4.2 calling kzalloc\n", __func__);
	scsi_mpath_head = kzalloc(size, GFP_KERNEL);
	if (!scsi_mpath_head)
		return -ENOMEM; //fixme free ida
	mpath_head = &scsi_mpath_head->mpath_head;

	mpath_init_head(mpath_head);
	mpath_head->mpdt = &smpdt;
	mpath_head->parent = &scsi_mpath_head->dev;
	ret = 0;
	pr_err("%s5 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS mpath_head=%pS mpath_device=%pS\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev, mpath_head, mpath_device);
	if (ret) {
		goto out_free_ida; //fixme free scsi_mpath_head
	}
	
	//scsi_mpath_head = mpath_to_priv_head(mpath_head);
	//mpath_head->mpath_subsys = kzalloc(sizeof(struct mpath_subsys), GFP_KERNEL);
	//if (!mpath_head->mpath_subsys) {
	//	ret = -ENOMEM;
	//	goto out_free_ida;
	//}

	//INIT_LIST_HEAD(&mpath_head->mpath_subsys->heads);
	//mutex_init(&mpath_head->mpath_subsys->lock);
	//list_add_tail(&mpath_head->entry, &mpath_head->mpath_subsys->heads);

	device_initialize(&scsi_mpath_head->dev);
	set_dev_node(&scsi_mpath_head->dev, dev_to_node(shost_dev));
	ret = dev_set_name(&scsi_mpath_head->dev, name);

	scsi_mpath_head->dev.class = &scsi_mpath_disk_class;

	dev_err(&scsi_mpath_head->dev, "%s6 mpath_head=%pS scsi_mpath_head=%pS called dev_set_name ret=%d, calling device_add\n",
		__func__, mpath_head, scsi_mpath_head, ret);
	ret = device_add(&scsi_mpath_head->dev);
	dev_err(&scsi_mpath_head->dev, "%s6.1 mpath_head=%pS scsi_mpath_head=%pS called device_add ret=%d\n",
		__func__, mpath_head, scsi_mpath_head, ret);
	if (ret)
		return ret;

	scsi_mpath_head->index = index;

	INIT_LIST_HEAD(&scsi_mpath_head->entry);
	kref_init(&scsi_mpath_head->ref);
	pr_err("%s6 called kref_init for scsi_mpath_head count=%d ref=%pS\n",
		__func__, kref_read(&scsi_mpath_head->ref), &scsi_mpath_head->ref); // set 1

//	mpath_head->cdev_device.devt = MKDEV(MAJOR(scsi_mpath_head_chr_devt), scsi_mpath_head->index);
//	mpath_head->cdev_device.class = &scsi_mpath_generic_class;
//	mpath_head->cdev_device.release = mpath_cdev_rel;

	pr_err("%s7 &scsi_mpath_head->dev=%pS\n", __func__, &scsi_mpath_head->dev);

	ret = mpath_alloc_head_disk(mpath_head);
	pr_err("%s8 ret=%d from dev_set_name\n", __func__, ret);
	if (ret)
		return ret;
	sprintf(mpath_head->disk->disk_name, name);


	sprintf(scsi_mpath_head->wwid, sdev->scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN);

	pr_err("%s13 ret=%d after bio_list_init sdev->scsi_mpath_dev=%pS scsi_mpath_head->wwid=%s\n",
		__func__, ret, sdev->scsi_mpath_dev, scsi_mpath_head->wwid);
	mpath_add_device(mpath_head, mpath_device);

	mutex_lock(&scsi_mpath_heads_lock);
	list_add_tail(&scsi_mpath_head->entry, &scsi_mpath_heads_list);

	pr_err("%s16\n", __func__);
	mutex_unlock(&scsi_mpath_heads_lock);

	pr_err("%s17 calling mpath_head_add\n", __func__);
	ret = mpath_add_head(mpath_head);
	if (ret)
		goto out_free_disk;
	pr_err("%s17.1 called mpath_head_add ret=%d\n", __func__, ret);

	pr_err("%s16 calling scsi_mpath_get_head\n", __func__);
	// same as nvme_mpath_alloc_disk -> nvme_tryget_ns_head
	scsi_mpath_get_head(scsi_mpath_head);

	return 0;

out_free_disk:
	mpath_put_disk(mpath_head);
out_free_ida:
	ida_free(&scsi_mpath_index_ida, scsi_mpath_head->index);
	return ret;
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_alloc);

static void scsi_mpath_free_head(struct kref *ref)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(ref, struct scsi_mpath_head, ref);
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;


	mutex_lock(&scsi_mpath_heads_lock);
	pr_err("%s0 scsi_mpath_head=%pS calling list_del mpath_head=%pS\n", __func__, scsi_mpath_head, mpath_head);
	list_del(&scsi_mpath_head->entry);
	mutex_unlock(&scsi_mpath_heads_lock);
	pr_err("%s1 scsi_mpath_head=%pS calling ida_free\n", __func__, scsi_mpath_head);
	ida_free(&scsi_mpath_index_ida, scsi_mpath_head->index);
//	pr_err("%s2 scsi_mpath_head=%pS calling mpath_put_disk\n", __func__, scsi_mpath_head);
//	mpath_put_disk(mpath_head);
	pr_err("%s4 scsi_mpath_head=%pS calling\n", __func__, scsi_mpath_head);
	device_unregister(&scsi_mpath_head->dev);
	kfree(scsi_mpath_head);
}

static int scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
	if (!kref_get_unless_zero(&scsi_mpath_head->ref)) {
		pr_err("%s1 scsi_mpath_head=%pS ENXIO\n", __func__, scsi_mpath_head);
		return -ENXIO;
	}
	return 0;
}

static void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head)
{
	kref_put(&scsi_mpath_head->ref, scsi_mpath_free_head);
}

void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;

	pr_err("%s scsi_mpath_dev=%pS mpath_device=%pS calling mpath_remove_device\n", __func__,
		scsi_mpath_dev, mpath_device);

	mpath_remove_device(mpath_device);
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_device);

/**
 * Callback function for activating multipath devices
 */
static __maybe_unused void activate_mpath(void *data, int err)
{
	struct scsi_device *sdev = data;	
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);
	bool retry = false;

	pr_err("%s sdev=%pS mpath_head=%pS mpath_dev=%pS\n",
		__func__, sdev, scsi_mpath_head, scsi_mpath_dev);

	switch (err) {
	case SCSI_DH_OK:
		break;
	case SCSI_DH_NOSYS:
		sdev_printk(KERN_ERR, sdev,
			"Could not failover the device scsi_dh_%s, Error %d\n",
			sdev->handler->name, err);
		mpath_clear_current_path(mpath_device);
		break;
	case SCSI_DH_DEV_TEMP_BUSY:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Busy\n");
		break;
	case SCSI_DH_RETRY:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Retry \n");
		retry = true;
		fallthrough;
	case SCSI_DH_IMM_RETRY:
	case SCSI_DH_RES_TEMP_UNAVAIL:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Unavailable, Clear current path \n");
		//if ((scsi_mpath_dev->state == SCSI_ACCESS_STATE_OFFLINE) ||
		//   (scsi_mpath_dev->state == SCSI_ACCESS_STATE_UNAVAILABLE))
		//	mpath_clear_current_path(mpath_device);
		pr_err("%s FIXME\n", __func__);
		err = 0;
		break;
	case SCSI_DH_DEV_OFFLINED:
	default:
		sdev_printk(KERN_ERR, sdev, "Device Handler Path offlined \n");
		mpath_clear_current_path(mpath_device);
		break;
	}

	if (retry)
		set_bit(SCSI_MPATH_DEVICE_IO_PENDING, &scsi_mpath_dev->flags);

        if (mpath_device_is_live(mpath_device)) {
			pr_err("%s calling scsi_mpath_set_live\n", __func__);
		//	mpath_device_set_live(mpath_device);
        }
}

/*  called when shost is being freed */
void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	struct scsi_mpath_head *scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);
	struct kref *ref = &scsi_mpath_head->ref;

	pr_err("%s sdev=%pS mpath_dev=%pS scsi_mpath_head=%pS\n",
		__func__, sdev, scsi_mpath_dev, scsi_mpath_head);

//	if (!mpath_head)
//		return;

//	cancel_work_sync(&mpath_head->requeue_work);
//	cleanup_srcu_struct(&mpath_head->srcu);

//	pr_err("%s2 sdev=%pS mpath_dev=%pS mpath_head=%pS calling kfree mpath_dev\n",
//		__func__, sdev, scsi_mpath_dev, scsi_mpath_head);
	sdev->scsi_mpath_dev = NULL;
	kfree(scsi_mpath_dev);

	pr_err("%s2 scsi_mpath_dev=%pS calling scsi_mpath_put_head ref=%pS refcount=%d\n",
		__func__, scsi_mpath_dev, ref, refcount_read(&ref->refcount));
	scsi_mpath_put_head(scsi_mpath_head);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_release);

static ssize_t scsi_mpath_wwid_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return sysfs_emit(buf, "%s\n", scsi_mpath_head->wwid);
}

struct device_attribute scsi_mpath_wwid = \
		__ATTR(wwid, S_IRUGO, scsi_mpath_wwid_show, NULL);

static ssize_t scsi_mpath_numa_nodes_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct mpath_device *mpath_device;
	struct scsi_mpath_head *scsi_mpath_head;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct mpath_head *mpath_head;
	struct mpath_iopolicy *mpath_iopolicy;

	dev_err(dev, "%s sdev=%pS mpath_dev=%pS\n", __func__,
		sdev, sdev->scsi_mpath_dev);

	scsi_mpath_dev = sdev->scsi_mpath_dev;
	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_head = mpath_device->mpath_head;

	scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);
	mpath_iopolicy = &scsi_mpath_head->iopolicy;
	dev_err(dev, "%s2 iopolicy=%d SCSI_MPATH_IOPOLICY_NUMA=%d\n", __func__,
		scsi_mpath_head->iopolicy.iopolicy, MPATH_IOPOLICY_NUMA);

	return mpath_numa_nodes_show(mpath_device, mpath_iopolicy, buf);
}

struct device_attribute scsi_mpath_numa_nodes = \
		__ATTR(numa_nodes, S_IRUGO, scsi_mpath_numa_nodes_show, NULL);

static ssize_t scsi_mpath_nr_active_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct mpath_device *mpath_device;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct scsi_mpath_head *scsi_mpath_head;
	struct mpath_head *mpath_head;

	dev_err(dev, "%s sdev=%pS mpath_dev=%pS\n", __func__,
		sdev, sdev->scsi_mpath_dev);

	scsi_mpath_dev = sdev->scsi_mpath_dev;
	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_head = mpath_device->mpath_head;
	scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);

	if (!mpath_qd_iopolicy(&scsi_mpath_head->iopolicy))
		return 0;

	return sysfs_emit(buf, "%d\n", atomic_read(&mpath_device->nr_active));
}
struct device_attribute scsi_mpath_nr_active = \
		__ATTR(nr_active, S_IRUGO, scsi_mpath_nr_active_show, NULL);

static ssize_t scsi_mpath_nr_total_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device;

	if (!scsi_mpath_dev) {
		pr_err("%s scsi_mpath_dev is NULL sdev=%pS\n", __func__, sdev);
		return 0;
	}
	mpath_device = &scsi_mpath_dev->mpath_device;

	return sysfs_emit(buf, "%d\n", atomic_read(&mpath_device->nr_total));
}

struct device_attribute scsi_mpath_nr_total = \
		__ATTR(nr_total, S_IRUGO, scsi_mpath_nr_total_show, NULL);

static ssize_t scsi_mpath_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return mpath_iopolicy_store(&scsi_mpath_head->iopolicy, buf, count);
}

static __maybe_unused ssize_t scsi_mpath_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return mpath_iopolicy_show(&scsi_mpath_head->iopolicy, buf);
}

struct device_attribute scsi_mpath_iopolicy = \
		__ATTR(iopolicy, S_IRUGO | S_IWUSR, scsi_mpath_iopolicy_show, scsi_mpath_iopolicy_store);
EXPORT_SYMBOL_GPL(scsi_mpath_iopolicy);


static ssize_t scsi_mpath_delayed_removal_secs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;
	unsigned int sec;
	int ret;

	ret = kstrtouint(buf, 0, &sec);
	if (ret < 0)
		return ret;

	mutex_lock(&mpath_head->lock);
	mpath_head->delayed_removal_secs = sec;
//	if (sec)
//		set_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);
//	else
//		clear_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);
	mutex_unlock(&mpath_head->lock);
	/*
	 * Ensure that update to NVME_NSHEAD_QUEUE_IF_NO_PATH is seen
	 * by its reader.
	 */
	//synchronize_srcu(&head->srcu);
	mpath_synchronize_head(mpath_head);

	return count;
}

static ssize_t scsi_mpath_delayed_removal_secs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;
	int ret;

	mutex_lock(&mpath_head->lock);
	ret = sysfs_emit(buf, "%u\n", mpath_head->delayed_removal_secs);
	mutex_unlock(&mpath_head->lock);
	return ret;

}

struct device_attribute scsi_mpath_delayed_removal_secs = \
		__ATTR(delayed_removal_secs, S_IRUGO | S_IWUSR, scsi_mpath_delayed_removal_secs_show,
			scsi_mpath_delayed_removal_secs_store);
EXPORT_SYMBOL_GPL(scsi_mpath_delayed_removal_secs);

int scsi_mpath_failover_disposition(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);
	struct scsi_device *sdev = scmd->device;

	pr_err("%s scmd=%pS req=%pS bio=%pS blk_queue_dying=%d scsi_device_online()=%d sdev_state=%d\n",
		__func__, scmd, req, req->bio, blk_queue_dying(req->q), scsi_device_online(sdev), sdev->sdev_state);
	if (is_mpath_request(req)) {
		if (scsi_is_mpath_error(scmd) ||
		    blk_queue_dying(req->q)) {
			pr_err("%s2 scmd=%pS req=%pS bio=%pS returning FAILOVER\n",
				__func__, scmd, req, req->bio);
			return FAILOVER;
		}
		pr_err("%s2.1 scmd=%pS req=%pS bio=%pS returning NEEDS_RETRY\n",
				__func__, scmd, req, req->bio);
		return NEEDS_RETRY;
	} else {
		if (blk_queue_dying(req->q)) {
			pr_err("%s3 scmd=%pS req=%pS bio=%pS returning SUCCESS\n",
				__func__, scmd, req, req->bio);
			return SUCCESS;
		}
	}

	pr_err("%s10 scmd=%pS req=%pS bio=%pS returning SUCCESS\n",
				__func__, scmd, req, req->bio);
	return SUCCESS;
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_disposition);

int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	int ret = -EINVAL;

	pr_err("%s sdev=%pS sizeof(scsi_mpath_dev->device_id_str)=%zd sizeof(&scsi_mpath_dev->device_id_str)=%zd\n",
		__func__, sdev, sizeof(scsi_mpath_dev->device_id_str), sizeof(&scsi_mpath_dev->device_id_str));
	pr_err("%s sdev=%pS  &scsi_mpath_dev->device_id_str[0]=%pS mpath_dev->device_id_str=%pS\n",
		__func__, sdev,  &scsi_mpath_dev->device_id_str[0], scsi_mpath_dev->device_id_str);

	ret = scsi_vpd_lun_id(sdev, scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN);
	pr_err("%s1 sdev=%pS called scsi_vpd_lun_id ret=%d\n", __func__, sdev, ret);
	if (ret < 0)
		return ret;

	pr_err("%s2 sdev=%pS mpath_dev->device_id_str=%s\n", __func__, sdev, scsi_mpath_dev->device_id_str);

	return 0;
}

static struct scsi_mpath_head *scsi_mpath_find_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int ret;

	mutex_lock(&scsi_mpath_heads_lock);
	list_for_each_entry(scsi_mpath_head, &scsi_mpath_heads_list, entry) {

		pr_err("%s itering mpath_head=%pS sdev->scsi_mpath_dev=%pS\n", __func__, scsi_mpath_head, sdev->scsi_mpath_dev);
		pr_err("%s1 wwid=%s\n", __func__, scsi_mpath_head->wwid);
		pr_err("%s2 sdev->scsi_mpath_dev->device_id_str=%s\n", __func__, sdev->scsi_mpath_dev->device_id_str);

		ret = scsi_mpath_get_head(scsi_mpath_head);
		if (ret)
			continue;
		if (strncmp(scsi_mpath_head->wwid, sdev->scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN) == 0) {
			pr_err("%s3 matches wwid\n", __func__);
			
			mutex_unlock(&scsi_mpath_heads_lock);
			return scsi_mpath_head;
		}
		scsi_mpath_put_head(scsi_mpath_head);
	}
	mutex_unlock(&scsi_mpath_heads_lock);
	return NULL;
}

void scsi_mpath_shutdown_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device;
	struct mpath_head *mpath_head;
	struct scsi_mpath_head *scsi_mpath_head;

	if (!scsi_mpath_dev)
		return;

	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_head = mpath_device->mpath_head;
	scsi_mpath_head = container_of(mpath_head, struct scsi_mpath_head, mpath_head);

	pr_err("%s clearing SCSI_MPATH_DISK_LIVE (if set) sdev=%pS\n", __func__, sdev);
	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		mpath_synchronize_device(mpath_device);
		kblockd_schedule_work(&mpath_head->requeue_work);
	//	del_gendisk(sdev->scsi_mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_shutdown_disk);

#ifdef dsdsd
void scsi_mpath_put_disk(struct nvme_ns_head *head)
{
	if (!head->disk)
		return;
	/* make sure all pending bios are cleaned up */
	kblockd_schedule_work(&head->requeue_work);
	flush_work(&head->requeue_work);
	flush_work(&head->partition_scan_work);
	put_disk(head->disk);
}

#endif

__maybe_unused static void scsi_mpath_alua_activate_done(void *data, int errors)
{
//	struct pgpath *pgpath = data;
//	struct priority_group *pg = pgpath->pg;
//	struct multipath *m = pg->m;
//	unsigned long flags;

	pr_err("%s data=%pS errors=%d SCSI_DH_OK=%d\n", __func__, data, errors, SCSI_DH_OK);

	/* device or driver problems */
	switch (errors) {
	case SCSI_DH_OK:
		break;
	case SCSI_DH_NOSYS:
		if (1) {
			errors = 0;
			break;
		}
		//DMERR("Could not failover the device: Handler scsi_dh_%s "
		 //     "Error %d.", m->hw_handler_name, errors);
		/*
		 * Fail path for now, so we do not ping pong
		 */
	//	fail_path(pgpath);
		break;
	case SCSI_DH_DEV_TEMP_BUSY:
		/*
		 * Probably doing something like FW upgrade on the
		 * controller so try the other pg.
		 */
	//	bypass_pg(m, pg, true, false);
		break;
	case SCSI_DH_RETRY:
		/* Wait before retrying. */
	//	delay_retry = true;
		fallthrough;
	case SCSI_DH_IMM_RETRY:
	case SCSI_DH_RES_TEMP_UNAVAIL:
	//	if (pg_init_limit_reached(m, pgpath))
	//		fail_path(pgpath);
		errors = 0;
		break;
	case SCSI_DH_DEV_OFFLINED:
	default:
		/*
		 * We probably do not want to fail the path for a device
		 * error, but this is what the old dm did. In future
		 * patches we can do more advanced handling.
		 */
	//	fail_path(pgpath);
	}

}

static void scsi_mpath_head_probe(dev_t devt)
{
	pr_err("%s devt=%d\n", __func__, devt);
}

static int __init init_scsi_mp(void)
{
	int err = class_register(&scsi_mpath_disk_class);

	pr_err("%s scsi_multipath=%d\n", __func__, scsi_multipath);

	if (err < 0)
		return err;
	err = __register_blkdev(0, "scsi-mpath-disk", scsi_mpath_head_probe);
	if (err < 0)
		goto destroy_disk_class;
	scsi_mpath_head_major = err;
	err = class_register(&scsi_mpath_generic_class);
	if (err < 0)
		goto unregister_blkdev;

	err = bioset_init(&scsi_mpath_bio_pool, 100,
				offsetof(struct scsi_mpath_clone_bio, clone),
				BIOSET_NEED_BVECS|BIOSET_PERCPU_CACHE);
	if (err < 0)
		goto class_unregister;
	err = alloc_chrdev_region(&mpath_head_chr_devt, 0, 1U << MINORBITS,
				     "mpath-generic");
	if (err < 0)
		goto bioset_uninit;
	return 0;
bioset_uninit:
	bioset_exit(&scsi_mpath_bio_pool);
class_unregister:
	class_unregister(&scsi_mpath_disk_class);
unregister_blkdev:
	unregister_blkdev(0, "scsi-mpath-disk");
destroy_disk_class:
	class_unregister(&scsi_mpath_disk_class);
	return err;
}

/**
 *	exit_sd - exit point for this driver (when it is a module).
 *
 *	Note: this function unregisters this driver from the scsi mid-level.
 **/
static void __exit exit_scsi_mp(void)
{
	pr_err("%s\n", __func__);
	class_unregister(&scsi_mpath_disk_class);
	unregister_blkdev(0, "scsi-mpath-disk");
	bioset_exit(&scsi_mpath_bio_pool);
	unregister_chrdev_region(mpath_head_chr_devt, 1U << MINORBITS);
}

module_init(init_scsi_mp);
module_exit(exit_scsi_mp);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_multipath");