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
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dh.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_multipath.h>
#include <scsi/scsi_ioctl.h>
#include "scsi_alua.h"
#include "scsi_priv.h"

#include <linux/sysfs.h>

#define SCSI_MPATH_DEVICE_IO_PENDING      0

MODULE_IMPORT_NS("SCSI_DH_ALUA");

static DEFINE_IDA(sd_mpath_index_ida);

static dev_t scsi_mpath_disk_major;
static struct scsi_mpath_disk *scsi_mpath_find_disk(struct scsi_device *sdev);
static void mpath_add_sysfs_link(struct mpath_disk *mpath_disk);
struct mpath_device *mpath_find_path(struct mpath_disk *mpath_disk);
static void mpath_remove_sysfs_link(struct mpath_device *mpath_device);

#define SCSI_MPATH_DISK_MINORS		(1U << MINORBITS)

bool scsi_multipath = false;
EXPORT_SYMBOL_GPL(scsi_multipath);
module_param(scsi_multipath, bool, 0444);
MODULE_PARM_DESC(scsi_multipath,
    "turn on native support for multiple scsi devices set this value to false to disable multipath, \n");

//static DEFINE_IDA(nvme_ns_chr_minor_ida);
static dev_t scsi_mpath_disk_chr_devt;

static LIST_HEAD(scsi_mpath_disks_list);
static DEFINE_MUTEX(scsi_mpath_disks_lock);


/* Check for path error */
static inline bool scsi_is_mpath_error(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);
	struct scsi_device *sdev = req->q->queuedata;

	if (sdev->handler && sdev->handler->prep_fn) {
		blk_status_t ret = sdev->handler->prep_fn(sdev, req);

		if (ret != BLK_STS_OK)
			return true;
	}

	return false;
}

static const struct class scsi_mpath_generic_class = {
	.name = "scsi_mpath_generic",
};

static __maybe_unused void scsi_mpath_disk_release1(struct device *dev)
{
	struct mpath_disk *mpath_disk = container_of(dev, struct mpath_disk, dev);
	dev_err(dev, "%s dev=%pS mpath_disk=%pS\n", __func__, dev, mpath_disk);

}

static const struct class scsi_mpath_disk_class = {
	.name = "scsi_mpath_disk",
	.dev_release	= scsi_mpath_disk_release1,
	.dev_groups = scsi_mpath_groups,
};

static int scsi_multipath_init(struct scsi_device *sdev)
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

	pr_err("%s2 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost);

	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_device->numa_node = dev_to_node(shost->dma_dev);

	return 0;
}

static __maybe_unused void nvme_mpath_unfreeze(struct scsi_mpath_disk *scsi_mpath_disk)
{
	pr_err("%s mpath_disk=%pS\n", __func__, scsi_mpath_disk);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_mq_unfreeze_queue_nomemrestore(h->disk->queue);
}


void scsi_mpath_wait_freeze(struct scsi_mpath_disk *scsi_mpath_disk)
{
//	struct nvme_ns_head *h;

	pr_err("%s mpath_disk=%pS\n", __func__, scsi_mpath_disk);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_mq_freeze_queue_wait(h->disk->queue);
}

void scsi_mpath_start_freeze(struct scsi_mpath_disk *scsi_mpath_disk)
{
//	struct nvme_ns_head *h;

	pr_err("%s mpath_disk=%pS\n", __func__, scsi_mpath_disk);
//	lockdep_assert_held(&subsys->lock);
//	list_for_each_entry(h, &subsys->nsheads, entry)
//		if (h->disk)
//			blk_freeze_queue_start(h->disk->queue);
}

void scsi_mpath_failover_req(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct Scsi_Host *shost = scmd->device->host;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	//struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	unsigned long flags;
	struct bio *bio;

	if (!scsi_device_online(sdev) || sdev->was_reset || sdev->locked)
		return;

	mpath_clear_current_path(mpath_device);

	/*
	 * if we got device handler error, we know that device is alive but not
	 * ready to process command. kick off a requeue of scsi command and try
	 * other available path
	 */
	if (scsi_is_mpath_error(scmd)) {
		/*
		 * Set flag as pending and requeue bio for retry on
		 * another path
		 */
		set_bit(SCSI_MPATH_DEVICE_IO_PENDING, &scsi_mpath_dev->flags);
		queue_work(shost->work_q, &mpath_disk->requeue_work);
	}

	/*
	 * following logic tries to steal bio, check if the bio has polled
	 * operation, if yes, then clear polled reqeust and reqeue bio
	 */
	spin_lock_irqsave(&mpath_disk->requeue_lock, flags);
	for (bio = req->bio; bio; bio = bio->bi_next) {
		bio_set_dev(bio, req->q->disk->part0);
		if (bio->bi_opf & REQ_POLLED) {
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;
		}
	}
	blk_steal_bios(&mpath_disk->requeue_list, req);
	spin_unlock_irqrestore(&mpath_disk->requeue_lock, flags);

	scmd->result = 0;

	blk_mq_end_request(req, 0);

	kblockd_schedule_work(&mpath_disk->requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_req);

void scsi_mpath_start_request(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct gendisk *disk = mpath_disk->gd;


	if ((READ_ONCE(mpath_disk->iopolicy) == MPATH_IOPOLICY_QD) &&
	    !(scmd->flags & SCMD_MPATH_CNT_ACTIVE)) {
		atomic_inc(&mpath_device->nr_active);
		scmd->flags |= SCMD_MPATH_CNT_ACTIVE;
	}

	if (!blk_queue_io_stat(disk->queue) || blk_rq_is_passthrough(req) ||
	    (scmd->flags & SCMD_MPATH_IO_STATS))
		return;

	scmd->flags |= SCMD_MPATH_IO_STATS;
	scmd->mpath_start_time = bdev_start_io_acct(disk->part0, req_op(req),
						      jiffies);
}

void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct gendisk *disk = mpath_disk->gd;

	//pr_err("%s req=%pS bio=%pS cmd=%pS sdev=%pS\n", __func__, req, req->bio, scmd, sdev);
	if (scmd->flags & SCMD_MPATH_CNT_ACTIVE)
		atomic_dec_if_positive(&mpath_device->nr_active);

	if (!(scmd->flags & SCMD_MPATH_IO_STATS))
		return;
	bdev_end_io_acct(disk->part0, req_op(req),
			 blk_rq_bytes(req) >> SECTOR_SHIFT,
			  scmd->mpath_start_time);
}


#if 0
void scsi_mpath_kick_requeue_lists(struct Scsi_Host *shost)
{
	struct scsi_mpath_disk *scsi_mpath_disk = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_dev, entry) {
		if (sdev->is_shared)
			continue;

		kblockd_schedule_work(&mpath_disk->requeue_lock);
		if (sdev->sdev_state == SDEV_RUNNING)
			disk_uevent(sdev->scsi_mpath_dev, KOBJ_CHANGE);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}
#endif

static bool scsi_mpath_state_is_live(enum mpath_access_state state)
{
	switch (state) {
	case MPATH_STATE_OPTIMAL:
	case MPATH_STATE_ACTIVE:
		return true;
	default:
		return false;
	}
}

static bool scsi_mpath_is_disabled(struct mpath_device *mpath_device)
{
//	enum scsi_device_state sdev_state = sdev->sdev_state;

	/*
	 * if device multipath state is not set to LIVE
	 * then return true
	 */
	if (!scsi_mpath_state_is_live(mpath_device->state))
		return true;

	/*
	 * Do not treat DELETING as a disabled path as I/O should
	 * still be able to complete assuming that scsi_device is
	 * within timeout limit.
	 * Otherwise I/O will fail immeadiately and return to
	 * requeue list
	 */
//	if (sdev_state != SDEV_RUNNING && sdev_state != SDEV_CANCEL)
//		return true;

	return false;
}

static inline bool scsi_mpath_is_optimized(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//pr_err("%s mpath_dev=%pS\n", __func__, scsi_mpath_dev);

	if (!scsi_mpath_dev)
		return false;

	return (!scsi_device_online(scsi_mpath_dev->sdev) &&
	    ((mpath_device->state == MPATH_STATE_OPTIMAL) ||
	     (mpath_device->state == MPATH_STATE_ACTIVE)));
}

static int scsi_mpath_ioctl(struct mpath_device *mpath_device, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	int err;
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
		return err;

	pr_err("%s3 cmd=0x%x arg=%ld sdev=%pS calling scsi_ioctl\n", __func__, cmd, arg, sdev);
	err = scsi_ioctl(sdev, mode & BLK_OPEN_WRITE, cmd, (void __user *)arg);
	pr_err("%s3.1 cmd=0x%x arg=%ld sdev=%pS called scsi_ioctl err=%d\n", __func__, cmd, arg, sdev, err);

	return err;
}

static __maybe_unused void scsi_multipath_partition_scan_work(struct work_struct *work)
{
	struct scsi_mpath_disk *scsi_mpath_disk = NULL;
	struct mpath_disk *mpath_disk = NULL;

	pr_err("%s scsi_mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d\n",
		__func__, scsi_mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_disk->gd->state)))
		return;

	//mutex_lock(&head->disk->open_mutex);
	pr_err("%s2 mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d calling bdev_disk_changed\n",
		__func__, scsi_mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
	bdev_disk_changed(mpath_disk->gd, false);
	//mutex_unlock(&head->disk->open_mutex);
}

static __maybe_unused void scsi_mpath_disk_release(struct device *dev)
{
	struct mpath_disk *mpath_disk = container_of(dev, struct mpath_disk, dev);
	dev_err(dev, "%s dev=%pS mpath_disk=%pS\n", __func__, dev, mpath_disk);
}

static int scsi_mpath_get_unique_id(struct mpath_device *mpath_device, u8 id[16],
	enum blk_unique_id type)
{
	struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);

	return scsi_mpath_unique_id(scsi_mpath_dev->sdev, id, type);
}

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_alloc_disk(struct scsi_device *sdev, struct gendisk *gd)
{
	struct queue_limits lim;
	int ret;
	static int disk_count;
	struct scsi_mpath_disk *scsi_mpath_disk;
	__maybe_unused int index;
	struct Scsi_Host *shost = sdev->host;
	struct device *shost_dev = &shost->shost_dev;
	size_t size;
	struct mpath_disk *mpath_disk;
	struct mpath_device *mpath_device;

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

	if (!scsi_device_tpgs(sdev)) {
		sdev_printk(KERN_NOTICE, sdev, "tpgs are required for mpath support\n");
		return -ENODEV;
	}

	pr_err("%s1 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS calling scsi_multipath_init\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev);
	scsi_multipath_init(sdev);
	pr_err("%s1.1 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS called scsi_multipath_init\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev);

	mpath_device = &sdev->scsi_mpath_dev->mpath_device;
	mpath_device->gd = gd;

	ret = scsi_mpath_unique_lun_id(sdev);
	if (ret < 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s sdev=%pS scsi_mpath_unique_lun_id failed\n", __func__, sdev);
		return ret;
	}

	pr_err("%s4 calling scsi_mpath_find_disk sdev=%pS\n", __func__, sdev);
	scsi_mpath_disk = scsi_mpath_find_disk(sdev);
	pr_err("%s4.1 called scsi_mpath_find_disk sdev=%pS mpath_disk=%pS\n", __func__, sdev, scsi_mpath_disk);
	if (scsi_mpath_disk) {
		mpath_disk = &scsi_mpath_disk->mpath_disk;
		mutex_lock(&scsi_mpath_disk->lock);
		list_add_tail(&mpath_device->siblings, &mpath_disk->dev_list);
		mutex_unlock(&scsi_mpath_disk->lock);
		mpath_device->mpath_disk = mpath_disk;
		return 0;
	}

	size = sizeof(*scsi_mpath_disk);
	size += num_possible_nodes() * sizeof(struct mpath_device *);

	scsi_mpath_disk = kzalloc(size, GFP_KERNEL);
	pr_err("%s5 sdev=%pS sdev->scsi_mpath_dev=%pS shost=%pS shost_dev=%pS mpath_disk=%pS mpath_device=%pS\n",
		__func__, sdev, sdev->scsi_mpath_dev, shost, shost_dev, scsi_mpath_disk, mpath_device);
	if (!scsi_mpath_disk)
		return -ENOMEM;
	mpath_disk = &scsi_mpath_disk->mpath_disk;
	mpath_device->mpath_disk = mpath_disk;
	mpath_disk->is_disabled = scsi_mpath_is_disabled;
	mpath_disk->is_optimized = scsi_mpath_is_optimized;
	mpath_disk->get_unique_id = scsi_mpath_get_unique_id;
	mpath_disk->ioctl = scsi_mpath_ioctl;

	scsi_mpath_disk->index = ida_alloc(&sd_mpath_index_ida, GFP_KERNEL);

	INIT_LIST_HEAD(&scsi_mpath_disk->entry);
	INIT_LIST_HEAD(&mpath_disk->dev_list);
	INIT_WORK(&mpath_disk->partition_scan_work, multipath_partition_scan_work);
	pr_err("%s6\n", __func__);
	mutex_init(&scsi_mpath_disk->lock);
	kref_init(&mpath_disk->ref);

	mpath_disk->dev.class = &scsi_mpath_disk_class;
//	scsi_mpath_disk->dev.release = scsi_mpath_disk_release;
//	scsi_mpath_disk->dev.groups = scsi_mpath_groups;
	pr_err("%s7 &mpath_disk->dev=%pS\n", __func__, &mpath_disk->dev);
	dev_set_name(&mpath_disk->dev, "smpd%d", scsi_mpath_disk->index);
	disk_count++;
	device_initialize(&mpath_disk->dev);

	blk_set_stacking_limits(&lim);
	pr_err("%s8\n", __func__);

	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL;
	lim.max_zone_append_sectors = 0;
	lim.dma_alignment = 3;

	mpath_disk->gd = blk_alloc_disk(&lim, dev_to_node(shost_dev));
	pr_err("%s9 dev=%pS sdev->scsi_mpath_dev=%pS mpath_disk->gd=%pS\n", __func__, sdev, sdev->scsi_mpath_dev, mpath_disk->gd);
	if (IS_ERR(mpath_disk->gd))
		return PTR_ERR(mpath_disk->gd);

	mpath_disk->gd->private_data = mpath_disk;
	mpath_disk->gd->fops = &mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state);
	sprintf(mpath_disk->gd->disk_name, "smpd%d", scsi_mpath_disk->index);

	dev_err(&mpath_disk->dev, "%s10 calling device_add for &mpath_disk->dev\n", __func__);
	ret = device_add(&mpath_disk->dev); // see nvme_init_subsystem()
	pr_err("%s11 called device_add ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	ret = init_srcu_struct(&mpath_disk->srcu);
	pr_err("%s12 ret=%d after init_srcu_struct mpath_disk=%pS\n", __func__, ret, scsi_mpath_disk);
	if (ret)
		return ret;

	INIT_WORK(&mpath_disk->requeue_work, mpath_requeue_work);
	pr_err("%s12.1 ret=%d after INIT_WORK mpath_disk=%pS\n", __func__, ret, scsi_mpath_disk);
	spin_lock_init(&mpath_disk->requeue_lock);
	pr_err("%s12.2 ret=%d after spin_lock_init mpath_disk=%pS\n", __func__, ret, scsi_mpath_disk);
	bio_list_init(&mpath_disk->requeue_list);
	pr_err("%s12.3 ret=%d after bio_list_init mpath_disk=%pS sdev->scsi_mpath_dev=%pS\n",
		__func__, ret, scsi_mpath_disk, sdev->scsi_mpath_dev);
	pr_err("%s12.3.1 device_id_str=%s\n",
		__func__, sdev->scsi_mpath_dev->device_id_str);

	sprintf(scsi_mpath_disk->wwid, sdev->scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN);

	pr_err("%s13 ret=%d after bio_list_init sdev->scsi_mpath_dev=%pS\n", __func__, ret, sdev->scsi_mpath_dev);
	list_add_tail(&mpath_device->siblings, &mpath_disk->dev_list);

	mutex_lock(&scsi_mpath_disks_lock);
	list_add_tail(&scsi_mpath_disk->entry, &scsi_mpath_disks_list);

	pr_err("%s16\n", __func__);
	mutex_unlock(&scsi_mpath_disks_lock);

	pr_err("%s16 out\n", __func__);
	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_alloc_disk);

void scsi_mpath_set_live(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	int ret;

	pr_err("%s mpath_disk=%pS MPATH_DISK_LIVE=%d\n",
		__func__, scsi_mpath_disk, test_bit(MPATH_DISK_LIVE, &mpath_disk->flags));

	if (!test_and_set_bit(MPATH_DISK_LIVE, &mpath_disk->flags)) {
		pr_err("%s calling device_add_disk &mpath_disk->dev=%pS\n", __func__, &mpath_disk->dev);
		ret = device_add_disk(&mpath_disk->dev, mpath_disk->gd, mpath_device_groups);
		pr_err("%s1 called device_add_disk ret=%d\n", __func__, ret);
		if (ret) {
			clear_bit(MPATH_DISK_LIVE, &mpath_disk->flags);
			return;
		}
		pr_err("%s2 calling scsi_mpath_disk_add_cdev partition_scan_work\n", __func__);
		mpath_disk_add_cdev(mpath_disk);
		pr_err("%s3 calling kblockd_schedule_work partition_scan_work\n", __func__);
		kblockd_schedule_work(&mpath_disk->partition_scan_work);
	}

	pr_info("Attached SCSI %s disk calling mpath_add_sysfs_link\n", "fixme");

	mpath_add_sysfs_link(mpath_disk);

	mutex_lock(&scsi_mpath_disk->lock);
	if (scsi_mpath_is_optimized(mpath_device)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_disk->srcu);
		for_each_online_node(node)
			__mpath_find_path(mpath_disk, node);
		srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	}
	mutex_unlock(&scsi_mpath_disk->lock);

	#if 0
	synchronize_srcu(&mpath_disk->srcu);
	kblockd_schedule_work(&mpath_disk->requeue_lock);
	#endif
}

/**
 * Callback function for activating multipath devices
 */
static __maybe_unused void activate_mpath(void *data, int err)
{
	struct scsi_device *sdev = data;	
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	bool retry = false;

	pr_err("%s sdev=%pS mpath_disk=%pS mpath_dev=%pS\n",
		__func__, sdev, scsi_mpath_disk, scsi_mpath_dev);

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

        if (scsi_mpath_state_is_live(mpath_device->state)) {
			pr_err("%s calling scsi_mpath_set_live\n", __func__);
			scsi_mpath_set_live(scsi_mpath_dev);
        }
}

/*  called when shost is being freed */
void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);

	pr_err("%s sdev=%pS mpath_dev=%pS mpath_disk=%pS\n",
		__func__, sdev, scsi_mpath_dev, scsi_mpath_disk);

//	if (!mpath_disk)
//		return;

//	cancel_work_sync(&mpath_disk->requeue_work);
//	cleanup_srcu_struct(&mpath_disk->srcu);

//	pr_err("%s2 sdev=%pS mpath_dev=%pS mpath_disk=%pS calling kfree mpath_dev\n",
//		__func__, sdev, scsi_mpath_dev, scsi_mpath_disk);
	sdev->scsi_mpath_dev = NULL;
	kfree(scsi_mpath_dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_release);

static ssize_t scsi_mpath_wwid_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct mpath_disk *mpath_disk =
		container_of(dev, struct mpath_disk, dev);
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);

	return sysfs_emit(buf, "%s\n", scsi_mpath_disk->wwid);
}

struct device_attribute scsi_mpath_wwid = \
		__ATTR(wwid, S_IRUGO, scsi_mpath_wwid_show, NULL);

static ssize_t scsi_mpath_numa_nodes_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct mpath_device *mpath_device;
	struct scsi_mpath_disk *scsi_mpath_disk;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct mpath_disk *mpath_disk;

	dev_err(dev, "%s sdev=%pS mpath_dev=%pS\n", __func__,
		sdev, sdev->scsi_mpath_dev);

	scsi_mpath_dev = sdev->scsi_mpath_dev;
	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_disk = mpath_device->mpath_disk;

	scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	dev_err(dev, "%s2 mpath_disk->iopolicy=%d SCSI_MPATH_IOPOLICY_NUMA=%d\n", __func__,
		mpath_disk->iopolicy, MPATH_IOPOLICY_NUMA);

	return mpath_numa_nodes_show(mpath_device, buf);
}

struct device_attribute scsi_mpath_numa_nodes = \
		__ATTR(numa_nodes, S_IRUGO, scsi_mpath_numa_nodes_show, NULL);

static ssize_t scsi_mpath_nr_total_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device;

	if (!scsi_mpath_dev)
		return 0;
	mpath_device = &scsi_mpath_dev->mpath_device;

	return sysfs_emit(buf, "%d\n", atomic_read(&mpath_device->nr_total));
}

struct device_attribute scsi_mpath_nr_total = \
		__ATTR(nr_total, S_IRUGO, scsi_mpath_nr_total_show, NULL);

int scsi_mpath_failover_disposition(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);

	pr_err("%s scmd=%pS req=%pS\n", __func__, scmd, req);
	if (req->cmd_flags & REQ_MPATH) {
		if (scsi_is_mpath_error(scmd) ||
		    blk_queue_dying(req->q)) {
			return NEEDS_RETRY;
		}
	} else {
		if (blk_queue_dying(req->q))
			return SUCCESS;
	}

	return SUCCESS;
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_disposition);

int scsi_mpath_unique_id(struct scsi_device *sdev, u8 id[16],
		enum blk_unique_id type)
{
	#if 0
	struct scsi_mpath_dh_data *dh_data = sdev->pg_data;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	if (type != BLK_UID_NAA)
		return -EINVAL;

	if (strncmp(dh_data->device_id_str, id, 16) == 0)
		return dh_data->device_id_len;
	#endif

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(scsi_mpath_unique_id);

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

static struct scsi_mpath_disk *scsi_mpath_find_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_disk *scsi_mpath_disk;
	int ret;

	mutex_lock(&scsi_mpath_disks_lock);
	list_for_each_entry(scsi_mpath_disk, &scsi_mpath_disks_list, entry) {
		struct mpath_disk *mpath_disk = &scsi_mpath_disk->mpath_disk;

		pr_err("%s itering mpath_disk=%pS sdev->scsi_mpath_dev=%pS\n", __func__, scsi_mpath_disk, sdev->scsi_mpath_dev);
		pr_err("%s1 wwid=%s\n", __func__, scsi_mpath_disk->wwid);
		pr_err("%s2 sdev->scsi_mpath_dev->device_id_str=%s\n", __func__, sdev->scsi_mpath_dev->device_id_str);

		ret = mpath_get_disk(mpath_disk);
		if (ret)
			continue;
		if (strncmp(scsi_mpath_disk->wwid, sdev->scsi_mpath_dev->device_id_str, SCSI_MPATH_DEVICE_ID_LEN) == 0) {
			pr_err("%s3 matches wwid\n", __func__);
			
			mutex_unlock(&scsi_mpath_disks_lock);
			return scsi_mpath_disk;
		}
		mpath_put_disk(mpath_disk);
	}
	mutex_unlock(&scsi_mpath_disks_lock);
	return NULL;
}

static void mpath_add_sysfs_link(struct mpath_disk *mpath_disk)
{
	__maybe_unused struct device *target;
	__maybe_unused int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct device *mpath_disk_dev = &mpath_disk->dev;
	struct kobject *mpath_device_kobj;
	struct mpath_device *mpath_device;

	pr_err("%s mpath_disk=%pS GD_ADDED=%d\n",
		__func__, mpath_disk, test_bit(GD_ADDED, &mpath_disk->gd->state));
	dev_err(mpath_disk_dev, "%s2\n", __func__);
	pr_err("%s3 mpath_disk->gd=%pS\n", __func__, mpath_disk->gd);
	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_disk->gd->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_disk->gd)->kobj;
	mpath_device_kobj = &mpath_disk_dev->kobj;
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	pr_err("%s4 mpath_disk->gd=%pS srcu_idx=%d mpath_device_kobj=%pS\n",
		__func__, mpath_disk->gd, srcu_idx, mpath_device_kobj);

	list_for_each_entry_srcu(mpath_device, &mpath_disk->dev_list, siblings,
				 srcu_read_lock_held(&mpath_disk->srcu)) {
		struct device *sdev_gendev;
		//struct scsi_mpath_device *scsi_mpath_dev;

		pr_err("%s5 mpath_device=%pS\n", __func__, mpath_device);
		if (!mpath_device)
			continue;
		//scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
		pr_err("%s5.1 mpath_device=%pS mpath_dev->sdev=%pS\n", __func__, mpath_device, NULL);
	//	if (!scsi_mpath_dev->sdev)
//			continue;
	//	sdev = scsi_mpath_dev->sdev;
		sdev_gendev = NULL;//&sdev->sdev_gendev; fixme
		/*
		 * Ensure that ns path disk node is already added otherwise we
		 * may get invalid kobj name for target
		 */
		dev_err(sdev_gendev, "%s6 itering mpath_device=%pS sdev=%pS\n",
			__func__, mpath_device, NULL);
		pr_err("%s6.1 itering mpath_device=%pS sdev->request_queue=%pS\n",
			__func__, mpath_device, NULL);
	//	if (!sdev->request_queue)
	//		continue;
		pr_err("%s6.2 itering mpath_device=%pS mpath_device->gd=%pS checking GD_ADDED=%d\n",
			__func__, mpath_device, mpath_device->gd,
			test_bit(GD_ADDED, &mpath_device->gd->state));
		if (!test_bit(GD_ADDED, &mpath_device->gd->state))
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
		pr_err("%s6.3 itering mpath_device=%pS mpath_device->gd=%pS GD_ADDED=%d checking SCSI_MPATH_SYSFS_ATTR_LINK=%d\n",
			__func__, mpath_device, mpath_device->gd,
			test_bit(GD_ADDED, &mpath_device->gd->state),
			test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags));
		if (test_and_set_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
			continue;

		pr_err("%s7.3 itering mpath_device=%pS mpath_device->gd=%pS\n",
			__func__, mpath_device, mpath_device->gd);
		target = disk_to_dev(mpath_device->gd);
		pr_err("%s7.4 itering mpath_device=%pS mpath_device->gd=%pS target=%pS scsi_mpath_attr_group.name=%s\n",
			__func__, mpath_device, mpath_device->gd, target, "multipath");
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, "multipath",
				&target->kobj, dev_name(target));
		pr_err("%s7.5 called sysfs_add_link_to_group rc=%d mpath_gd_kobj=%pS &target->kobj=%pS dev_name=%s\n",
			__func__, rc, mpath_gd_kobj, &target->kobj, dev_name(target));
		if (unlikely(rc)) {
	//		dev_err(disk_to_dev(mpath_disk->gd),
	//				"failed to create link to %s rc=%d\n",
	//				dev_name(target), rc);
		//	clear_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &scsi_mpath_dev->flags);
		}

		rc = sysfs_create_link(mpath_device_kobj, NULL/* &sdev_gendev->kobj*/,
				""/*dev_name(sdev_gendev)*/);
		pr_err("%s7.6 called sysfs_create_link for mpath_device_kobj=%pS rc=%d &sdev_gendev->kobj=%pS dev_name(sdev_gendev)=%s\n",
			__func__, mpath_device_kobj, rc, NULL /*&sdev_gendev->kobj*/, ""/*dev_name(sdev_gendev)*/);
		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_disk->gd),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
		}

	}

	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}

static void mpath_remove_sysfs_link(struct mpath_device *mpath_device)
{
	struct device *target;
	struct kobject *mpath_gd_kobj;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct device *sdev_gendev;
	struct scsi_device *sdev;
	struct device *mpath_disk_dev = &mpath_disk->dev;
	struct kobject *mpath_device_kobj = &mpath_disk_dev->kobj;

	pr_err("%s mpath_device=%pS mpath_disk=%pS SCSI_MPATH_SYSFS_ATTR_LINK set=%d\n",
		__func__, mpath_device, mpath_disk,
		test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags));
	if (!test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
		return;

	target = disk_to_dev(mpath_device->gd);
	mpath_gd_kobj = &disk_to_dev(mpath_disk->gd)->kobj;

	pr_err("%s2 calling sysfs_remove_link_from_group mpath_gd_kobj=%pS dev_name(target)=%s\n",
		__func__, mpath_gd_kobj, dev_name(target));

	sysfs_remove_link_from_group(mpath_gd_kobj, "multipath",
			dev_name(target));

	pr_err("%s3 calling sysfs_delete_link mpath_gd_kobj=%pS dev_name(target)=%s\n",
		__func__, mpath_gd_kobj, dev_name(target));

	sdev = NULL;//scsi_mpath_dev->sdev;
	sdev_gendev = NULL;//&sdev->sdev_gendev;
	sysfs_delete_link(mpath_device_kobj, NULL /*&sdev_gendev->kobj*/,
			""/*dev_name(sdev_gendev)*/);

	clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
}

void scsi_mpath_shutdown_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device;
	struct mpath_disk *mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk;

	if (!scsi_mpath_dev)
		return;

	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_disk = mpath_device->mpath_disk;
	scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);

	pr_err("%s clearing SCSI_MPATH_DISK_LIVE (if set) sdev=%pS\n", __func__, sdev);
	if (test_and_clear_bit(MPATH_DISK_LIVE, &mpath_disk->flags)) {
		synchronize_srcu(&mpath_disk->srcu);
		kblockd_schedule_work(&mpath_disk->requeue_work);
	//	del_gendisk(sdev->scsi_mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_shutdown_disk);

void scsi_mpath_add_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	pr_err("%s mpath_dev=%pS\n", __func__, scsi_mpath_dev);

	pr_err("%s1 mpath_disk=%pS\n", __func__, scsi_mpath_disk);

	if (scsi_mpath_state_is_live(mpath_device->state)) {
		mpath_device->state = MPATH_STATE_OPTIMAL;
		pr_err("%s calling scsi_mpath_set_live\n", __func__);
		scsi_mpath_set_live(scsi_mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_disk);

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

void scsi_mpath_remove_disk(struct scsi_device *sdev)
{
	bool last_path = false;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_device *mpath_device;
	struct mpath_disk *mpath_disk;
	struct scsi_mpath_disk *scsi_mpath_disk;
	
	dev_err(&sdev->sdev_gendev, "%s scsi_mpath_dev=%pS\n", __func__, scsi_mpath_dev);

	if (!sdev->scsi_mpath_dev)
		return;
	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_disk = mpath_device->mpath_disk;
	scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);

	dev_err(&sdev->sdev_gendev, "%s1 scsi_mpath_dev=%pS calling mpath_remove_sysfs_link\n",
		__func__, scsi_mpath_dev);
	mpath_remove_sysfs_link(mpath_device);

	synchronize_srcu(&mpath_disk->srcu);

	/* wait for concurrent submissions */
	if (mpath_clear_current_path(mpath_device))
		synchronize_srcu(&mpath_disk->srcu);

	dev_err(&sdev->sdev_gendev, "%s2 scsi_mpath_dev=%pS called scsi_mpath_remove_sysfs_link\n",
		__func__, scsi_mpath_dev);
//	put_disk(sdev->scsi_mpath_dev->scsi_mpath_disk->gd);
//	if (!sdev->is_shared)
//		return;

	/* Make sure All pending bio's are cleaned up */
	kblockd_schedule_work(&mpath_disk->requeue_work);
	flush_work(&mpath_disk->requeue_work);
	//put_disk(sdev->scsi_mpath_dev);

	mutex_lock(&scsi_mpath_disk->lock);
	
	list_del_rcu(&mpath_device->siblings);
	dev_err(&sdev->sdev_gendev, "%s3 list_empty=%d\n", __func__, list_empty(&mpath_disk->dev_list));
	if (list_empty(&mpath_disk->dev_list)) {
//		if (!nvme_mpath_queue_if_no_path(ns->head))
//			list_del_init(&ns->head->entry);
		last_path = true;
	}
	mutex_unlock(&scsi_mpath_disk->lock);

	dev_err(&sdev->sdev_gendev, "%s4 last_path=%d\n",
		__func__, last_path);

	if (last_path) {
		dev_err(&sdev->sdev_gendev, "%s5 MPATH_DISK_LIVE set=%d\n",
			__func__, test_bit(MPATH_DISK_LIVE, &mpath_disk->flags));
		if (test_and_clear_bit(MPATH_DISK_LIVE, &mpath_disk->flags)) {
			/*
			 * requeue I/O after NVME_NSHEAD_DISK_LIVE has been cleared
			 * to allow multipath to fail all I/O.
			 */
		//	kblockd_schedule_work(&head->requeue_work);

			mpath_cdev_del(&mpath_disk->cdev, &mpath_disk->cdev_device);
			synchronize_srcu(&mpath_disk->srcu);
			dev_err(&sdev->sdev_gendev, "%s5.1 not calling device_del\n", __func__);
		//	device_del(&scsi_mpath_disk->dev);
			dev_err(&sdev->sdev_gendev, "%s5.2 calling del_gendisk\n", __func__);
			del_gendisk(mpath_disk->gd);
		}
		dev_err(&sdev->sdev_gendev, "%s6 calling put_disk on mpath_disk->gd=%pS\n", __func__, mpath_disk->gd);
		
	}
	mpath_put_disk(mpath_disk);

	dev_err(&sdev->sdev_gendev, "%s10 mpath_dev=%pS\n", __func__, sdev->scsi_mpath_dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_disk);

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

static void scsi_mpath_disk_probe(dev_t devt)
{
	pr_err("%s devt=%d\n", __func__, devt);
}

static int __init init_scsi_mp(void)
{
	int err = class_register(&scsi_mpath_disk_class);

	pr_err("%s scsi_multipath=%d\n", __func__, scsi_multipath);

	if (err < 0)
		return err;
	err = __register_blkdev(0, "scsi-mpath-disk", scsi_mpath_disk_probe);
	if (err < 0)
		goto destroy_disk_class;
	scsi_mpath_disk_major = err;
	err = alloc_chrdev_region(&scsi_mpath_disk_chr_devt, 0, 1U << MINORBITS,
				     "scsi-mpath-generic");
	if (err < 0)
		goto unregister_blkdev;
	err = class_register(&scsi_mpath_generic_class);
	if (err < 0)
		goto unregister_chrdev;

	return 0;
unregister_chrdev:
	unregister_chrdev_region(scsi_mpath_disk_chr_devt, 1U << MINORBITS);
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
}

module_init(init_scsi_mp);
module_exit(exit_scsi_mp);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_multipath");