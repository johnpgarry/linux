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
#include "device_handler/scsi_dh_alua.h"

MODULE_IMPORT_NS("SCSI_DH_ALUA");

static DEFINE_IDA(sd_mpath_index_ida);

static dev_t scsi_mpath_disk_major;

#define SCSI_MPATH_DISK_MINORS		(1U << MINORBITS)

bool scsi_multipath = true;
module_param(scsi_multipath, bool, 0444);
MODULE_PARM_DESC(scsi_multipath,
    "turn on native support for multiple scsi devices \n"
    "set this value to false to disable multipath, \n");

static const char *scsi_mpath_iopolicy_names[] = {
	[SCSI_MPATH_IOPOLICY_NUMA]	= "numa",
	[SCSI_MPATH_IOPOLICY_RR]	= "round-robin",
};

static int iopolicy = SCSI_MPATH_IOPOLICY_NUMA;

static LIST_HEAD(mpath_disks_list);
static DEFINE_MUTEX(mpath_disks_lock);

static const struct class scsi_mpath_disk_class = {
	.name = "scsi_mpath_disk",
};


//static DEFINE_IDA(nvme_ns_chr_minor_ida);
static dev_t scsi_mpath_disk_chr_devt;
static const struct class scsi_mpath_generic_class = {
	.name = "scsi_mpath_generic",
};

static bool scsi_mpath_state_is_live(unsigned int state)
{
	switch (state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
	case SCSI_ACCESS_STATE_ACTIVE:
	case SCSI_ACCESS_STATE_LBA:
	case SCSI_ACCESS_STATE_TRANSITIONING:
		return true;
	default:
		return false;
	}
}

static ssize_t scsi_mpath_disk_wwid_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct scsi_mpath_disk *mpath_disk =
		container_of(dev, struct scsi_mpath_disk, dev);

	return sysfs_emit(buf, "%s\n", mpath_disk->wwid);
}

struct device_attribute scsi_mpath_disk_wwid = \
		__ATTR(wwid, S_IRUGO, scsi_mpath_disk_wwid_show, NULL);

static ssize_t scsi_mpath_disk_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_mpath_disk *mpath_disk =
		container_of(dev, struct scsi_mpath_disk, dev);

	return sysfs_emit(buf, "%s\n",
			  scsi_mpath_iopolicy_names[READ_ONCE(mpath_disk->iopolicy)]);
}

static void scsi_mpath_disk_iopolicy_update(struct scsi_mpath_disk *mpath_disk,
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
			mpath_disk->index,
			scsi_mpath_iopolicy_names[old_iopolicy],
			scsi_mpath_iopolicy_names[iopolicy]);
}
static ssize_t scsi_mpath_disk_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_mpath_disk *mpath_disk =
		container_of(dev, struct scsi_mpath_disk, dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(scsi_mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, scsi_mpath_iopolicy_names[i])) {
			scsi_mpath_disk_iopolicy_update(mpath_disk, i);
			return count;
		}
	}

	return -EINVAL;
}

struct device_attribute scsi_mpath_disk_iopolicy = \
		__ATTR(iopolicy, S_IRUGO | S_IWUSR, scsi_mpath_disk_iopolicy_show, scsi_mpath_disk_iopolicy_store);


static struct attribute *scsi_mpath_disk_attrs[] = {
	&scsi_mpath_disk_wwid.attr,
	&scsi_mpath_disk_iopolicy.attr,
	NULL
};

static const struct attribute_group scsi_mpath_disk_attrs_group = {
	.attrs = scsi_mpath_disk_attrs,
};

const struct attribute_group *scsi_mpath_disk_attrs_groups[] = {
	&scsi_mpath_disk_attrs_group,
	NULL
};

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

static void scsi_multipath_partition_scan_work(struct work_struct *work)
{
	struct scsi_mpath_disk *mpath_disk =
		container_of(work, struct scsi_mpath_disk, partition_scan_work);

	pr_err("%s mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d\n",
		__func__, mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_disk->gd->state)))
		return;

	//mutex_lock(&head->disk->open_mutex);
	pr_err("%s2 mpath_disk=%pS GD_SUPPRESS_PART_SCAN=%d calling bdev_disk_changed\n",
		__func__, mpath_disk, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state));
	bdev_disk_changed(mpath_disk->gd, false);
	//mutex_unlock(&head->disk->open_mutex);
}

/*
 * SCSI multipath will only allow 'NUMA' or 'round-robin' policy for IO.
 * In Future, if more apropriate IO-policy is introduced will be added
 * based on community feedback.
 */
static int scsi_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	if (!val)
		return -EINVAL;
	if (!strncmp(val, "numa", 4))
		iopolicy = SCSI_MPATH_IOPOLICY_NUMA;
	else if (!strncmp(val, "round-robin", 11))
		iopolicy = SCSI_MPATH_IOPOLICY_RR;
	else
		return -EINVAL;

	return 0;
}

static int scsi_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", scsi_mpath_iopolicy_names[iopolicy]);
}

module_param_call(iopolicy, scsi_set_iopolicy, scsi_get_iopolicy,
    &iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
    "Default multipath I/O policy; 'numa' (default) or 'round-robin'");

void scsi_mpath_default_iopolicy(struct scsi_mpath_disk *mpath_disk)
{
	mpath_disk->iopolicy = iopolicy;
}

void scsi_multipath_iopolicy_update(struct scsi_device *sdev, int iopolicy)
{
	pr_err("%s sdev=%pS iopolicy=%d\n", __func__, sdev, iopolicy);
	#if 0
	struct Scsi_Host *shost =  sdev->host;
	struct scsi_mpath_disk *mpath_disk = sdev->mpath_dev;
	int old_iopolicy = READ_ONCE(sdev->iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(sdev->iopolicy, iopolicy);

	/* iopoliocy changes clear the multipath */
	mutex_lock(&mpath_dev->mpath_lock);
	list_for_each_entry_rcu(sdev, &shost->mpath_dev, entry)
		//scsi_mpath_clear_paths(shost);
	mutex_unlock(&mpath_dev->mpath_lock);

	sdev_printk(KERN_NOTICE, sdev, "Multipath iopolocy changed from %s to %s\n",
	    scsi_iopolicy_names[old_iopolicy], scsi_iopolicy_names[iopolicy]);
	  #endif
}

bool scsi_mpath_clear_current_path(struct scsi_mpath_device *mpath_dev)
{
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	bool changed = false;
	int node;

	if (!mpath_disk)
		return changed;

	for_each_node(node) {
		if (mpath_dev == rcu_access_pointer(mpath_disk->current_path[node])) {
			rcu_assign_pointer(mpath_disk->current_path[node], NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(scsi_mpath_clear_current_path);

void scsi_mpath_clear_paths(struct scsi_mpath_disk *mpath_disk)
{
	struct scsi_mpath_device *mpath_dev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	list_for_each_entry_rcu(mpath_dev, &mpath_disk->dev_list, entry) {
		scsi_mpath_clear_current_path(mpath_dev);
		kblockd_schedule_work(&mpath_disk->requeue_work);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);

}

static inline bool scsi_mpath_state_is_live2(enum scsi_mpath_access_state state)
{
	if (state == SCSI_MPATH_OPTIMAL ||
	    state == SCSI_MPATH_ACTIVE)
		return true;

	return false;
}

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

static bool scsi_mpath_is_disabled(struct scsi_device *sdev)
{
	enum scsi_device_state sdev_state = sdev->sdev_state;

	/*
	 * if device multipath state is not set to LIVE
	 * then return true
	 */
	if (!scsi_mpath_state_is_live2(sdev->mpath_dev->state))
		return true;

	/*
	 * Do not treat DELETING as a disabled path as I/O should
	 * still be able to complete assuming that scsi_device is
	 * within timeout limit.
	 * Otherwise I/O will fail immeadiately and return to
	 * requeue list
	 */
	if (sdev_state != SDEV_RUNNING && sdev_state != SDEV_CANCEL)
		return true;

	return false;
}

/* handle failover request for path */
void scsi_mpath_failover_req(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct Scsi_Host *shost = scmd->device->host;
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	unsigned long flags;
	struct bio *bio;

	if (!scsi_device_online(sdev) || sdev->was_reset || sdev->locked)
		return;

	scsi_mpath_clear_current_path(mpath_dev);

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
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &mpath_dev->flags);
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

static inline bool scsi_mpath_is_optimized(struct scsi_mpath_device *mpath_dev)
{
	pr_err("%s mpath_dev=%pS\n", __func__, mpath_dev);

	if (!mpath_dev)
		return false;

	return (!scsi_device_online(mpath_dev->sdev) &&
	    ((mpath_dev->state == SCSI_MPATH_OPTIMAL) ||
	     (mpath_dev->state == SCSI_MPATH_ACTIVE)));
}

static struct scsi_mpath_device *scsi_next_mpath_dev(struct scsi_mpath_disk *mpath_disk,
			struct scsi_mpath_device *mpath_dev)
{
	#if 0
	struct scsi_
	sdev = list_next_or_null_rcu(&mpath_disk->dev_list, &mpath_dev->siblings,
	    struct scsi_device, siblings);

	if (sdev)
		return sdev;

	return list_first_or_null_rcu(&mpath_disk->dev_list, struct scsi_device,
	    siblings);
	#endif
	return NULL;
}

static struct scsi_mpath_device *scsi_mpath_round_robin_path(struct scsi_mpath_disk *mpath_disk,
	int node, struct scsi_mpath_device *old_mpath_dev)
{
	struct scsi_mpath_device *mpath_dev, *found = NULL;

	if (list_is_singular(&mpath_disk->dev_list)) {
		if(scsi_mpath_is_disabled(old_mpath_dev->sdev))
			return NULL;
		return old_mpath_dev;
	}

	for (mpath_dev = scsi_next_mpath_dev(mpath_disk, mpath_dev);
	    mpath_dev && mpath_dev != old_mpath_dev;
	    mpath_dev = scsi_next_mpath_dev(mpath_disk, mpath_dev)) {
		if (scsi_mpath_is_disabled(mpath_dev->sdev))
			continue;
		if (mpath_dev->state == SCSI_MPATH_OPTIMAL) {
			found = mpath_dev;
			goto out;
		}
		if (mpath_dev->state == SCSI_MPATH_ACTIVE)
			found = mpath_dev;
	}

	if (!scsi_mpath_is_disabled(old_mpath_dev->sdev) &&
	    (old_mpath_dev->state == SCSI_MPATH_OPTIMAL ||
	    (!found && old_mpath_dev->state == SCSI_MPATH_ACTIVE)))
		return old_mpath_dev;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_disk->current_path[node], found);

	return found;
}

/*
 * Search path based on iopolicy and numa node affinity
 * and return the scsi_device for that path
 */
inline struct scsi_mpath_device *__scsi_find_path(struct scsi_mpath_disk *mpath_disk, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	//struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;
	struct scsi_mpath_device *mpath_dev_found, *mpath_dev_fallback, *mpath_dev;

	pr_err("%s mpath_disk=%pS\n", __func__, mpath_disk);
	list_for_each_entry_rcu(mpath_dev, &mpath_disk->dev_list, entry) {
		pr_err("%s1 itering mpath_disk=%pS mpath_dev=%pS disabled=%d\n",
			__func__, mpath_disk, mpath_dev, scsi_mpath_is_disabled(mpath_dev->sdev));
		if (scsi_mpath_is_disabled(mpath_dev->sdev))
			continue;

		if (mpath_dev->numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(mpath_disk->iopolicy) == SCSI_MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, mpath_dev->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_dev->state) {
		case SCSI_MPATH_OPTIMAL:
		    if (distance < found_distance) {
			    found_distance = distance;
			    mpath_dev_found = mpath_dev;
		    }
		    break;
		case SCSI_MPATH_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    mpath_dev_fallback = mpath_dev;
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

inline struct scsi_mpath_device *scsi_find_path(struct scsi_mpath_disk *mpath_disk)
{
	int node = numa_node_id();
	struct scsi_mpath_device *mpath_dev;

	mpath_dev = srcu_dereference(mpath_disk->current_path[node],
	    &mpath_disk->srcu);

	pr_err("%s mpath_dev=%pS mpath_disk=%pS\n", __func__, mpath_dev, mpath_disk);

	if (unlikely(!mpath_dev))
		mpath_dev = __scsi_find_path(mpath_disk, node);

	if (READ_ONCE(mpath_disk->iopolicy) == SCSI_MPATH_IOPOLICY_RR)
		return scsi_mpath_round_robin_path(mpath_disk, node, mpath_dev);

	if (unlikely(!scsi_mpath_is_optimized(mpath_dev)))
		return __scsi_find_path(mpath_disk, node);

	return mpath_dev;
}

static void scsi_requeue_work(struct work_struct *work)
{
	struct scsi_mpath_disk *mpath_disk =
	    container_of(work, struct scsi_mpath_disk, requeue_work);
	struct bio *bio, *next;

	pr_err("%s mpath_disk=%pS\n", __func__, mpath_disk);

	spin_lock_irq(&mpath_disk->requeue_lock);
	next = bio_list_get(&mpath_disk->requeue_list);
	spin_unlock(&mpath_disk->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}

#include <linux/sysfs.h>

static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *scsi_mpath_attrs[] = {
	&dummy_attr,
	NULL
};

static bool multipath_sysfs_group_visible(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct gendisk *disk = dev_to_disk(dev);

	dev_err(dev, "%s dev=%pS disk=%pS fops=%pS\n", __func__, dev, disk, disk->fops);
	return disk->fops == &scsi_mpath_ops;
}

static bool multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);

	dev_err(dev, "%s dev=%pS\n", __func__, dev);
	return false;
}

DEFINE_SYSFS_GROUP_VISIBLE(multipath_sysfs)

const struct attribute_group scsi_mpath_attr_group = {
	.name           = "multipath",
	.attrs		= scsi_mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),
};

const struct attribute_group *sd_attr_groups[] = {
	&scsi_mpath_attr_group,
	NULL
};

static void scsi_mpath_add_sysfs_link(struct scsi_mpath_disk *mpath_disk)
{
	__maybe_unused struct device *target;
	__maybe_unused int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct scsi_device *sdev;
	struct scsi_mpath_device *mpath_dev;
	struct device *mpath_device = &mpath_disk->dev;
	struct kobject *mpath_device_kobj;

	pr_err("%s mpath_disk=%pS GD_ADDED=%d\n",
		__func__, mpath_disk, test_bit(GD_ADDED, &mpath_disk->gd->state));
	dev_err(mpath_device, "%s2\n", __func__);
	pr_err("%s3 mpath_disk->gd=%pS\n", __func__, mpath_disk->gd);
	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_disk->gd->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_disk->gd)->kobj;
	mpath_device_kobj = &mpath_device->kobj;
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	pr_err("%s4 mpath_disk->gd=%pS srcu_idx=%d mpath_device_kobj=%pS\n", __func__, mpath_disk->gd, srcu_idx, mpath_device_kobj);

	list_for_each_entry_srcu(mpath_dev, &mpath_disk->dev_list, entry,
				 srcu_read_lock_held(&mpath_disk->srcu)) {
		struct device *sdev_gendev;

		pr_err("%s5 mpath_dev=%pS\n", __func__, mpath_dev);
		if (!mpath_dev)
			continue;
		pr_err("%s5.1 mpath_dev=%pS mpath_dev->sdev=%pS\n", __func__, mpath_dev, mpath_dev->sdev);
		if (!mpath_dev->sdev)
			continue;
		sdev = mpath_dev->sdev;
		sdev_gendev = &sdev->sdev_gendev;
		/*
		 * Ensure that ns path disk node is already added otherwise we
		 * may get invalid kobj name for target
		 */
		dev_err(sdev_gendev, "%s6 itering mpath_dev=%pS sdev=%pS\n", __func__, mpath_dev, sdev);
		pr_err("%s6.1 itering mpath_dev=%pS sdev->request_queue=%pS\n", __func__, mpath_dev, sdev->request_queue);
		if (!sdev->request_queue)
			continue;
		pr_err("%s6.2 itering mpath_dev=%pS mpath_dev->gd=%pS checking GD_ADDED=%d\n",
			__func__, mpath_dev, mpath_dev->gd, test_bit(GD_ADDED, &mpath_dev->gd->state));
		if (!test_bit(GD_ADDED, &mpath_dev->gd->state))
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
		pr_err("%s6.3 itering mpath_dev=%pS mpath_dev->gd=%pS GD_ADDED=%d checking SCSI_MPATH_SYSFS_ATTR_LINK=%d\n",
			__func__, mpath_dev, mpath_dev->gd, test_bit(GD_ADDED, &mpath_dev->gd->state),
			test_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags));
		if (test_and_set_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags))
			continue;

		pr_err("%s7.3 itering mpath_dev=%pS mpath_dev->gd=%pS\n", __func__, mpath_dev, mpath_dev->gd);
		target = disk_to_dev(mpath_dev->gd);
		pr_err("%s7.4 itering mpath_dev=%pS mpath_dev->gd=%pS target=%pS scsi_mpath_attr_group.name=%s\n",
			__func__, mpath_dev, mpath_dev->gd, target, scsi_mpath_attr_group.name);
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, scsi_mpath_attr_group.name,
				&target->kobj, dev_name(target));
		pr_err("%s7.5 called sysfs_add_link_to_group rc=%d\n", __func__, rc);
		if (unlikely(rc)) {
	//		dev_err(disk_to_dev(mpath_disk->gd),
	//				"failed to create link to %s rc=%d\n",
	//				dev_name(target), rc);
		//	clear_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags);
		}

		rc = sysfs_create_link(mpath_device_kobj, &sdev_gendev->kobj,
				dev_name(sdev_gendev));
		pr_err("%s7.6 called sysfs_create_link for mpath_gd_kobj rc=%d\n", __func__, rc);
		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_disk->gd),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags);
		}

	}


	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}

static int scsi_mpath_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;
	struct scsi_mpath_disk *mpath_disk = disk->private_data;
	struct scsi_mpath_device *mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx, err;

	pr_err("%s cmd=0x%x arg=%ld mpath_disk=%pS\n", __func__, cmd, arg, mpath_disk);
	
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_dev = scsi_find_path(mpath_disk);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_disk=%pS called scsi_find_path srcu_idx=%d mpath_dev=%pS\n",
		__func__, cmd, arg, mpath_disk, srcu_idx, mpath_dev);
	if (!mpath_dev)
		goto out_unlock;
	sdev = mpath_dev->sdev;
	pr_err("%s2 cmd=0x%x arg=%ld sdev=%pS\n", __func__, cmd, arg, sdev);

	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO)) {
		err = -ENOIOCTLCMD;
		goto out_unlock;
	}

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	err = scsi_ioctl_block_when_processing_errors(sdev, cmd,
			(mode & BLK_OPEN_NDELAY));
	if (err)
		goto out_unlock;

	pr_err("%s3 cmd=0x%x arg=%ld sdev=%pS calling scsi_ioctl\n", __func__, cmd, arg, sdev);
	err = scsi_ioctl(sdev, mode & BLK_OPEN_WRITE, cmd, (void __user *)arg);
	pr_err("%s3.1 cmd=0x%x arg=%ld sdev=%pS called scsi_ioctl err=%d\n", __func__, cmd, arg, sdev, err);

out_unlock:
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	return err;
}

#if 0
static inline struct nvme_ns_head *cdev_to_ns_head(struct cdev *cdev)
{
	return container_of(cdev, struct nvme_ns_head, cdev);
}

#endif

static void scsi_mpath_free(struct kref *ref)
{
	struct scsi_mpath_disk *mpath_disk =
		container_of(ref, struct scsi_mpath_disk, ref);

	pr_err("%s ref=%pS mpath_disk=%pS\n", __func__, ref, mpath_disk);
	/*
	nvme_mpath_put_disk(head);
	ida_free(&head->subsys->ns_ida, head->instance);
	cleanup_srcu_struct(&head->srcu);
	nvme_put_subsystem(head->subsys);
	kfree(head->plids);
	kfree(head);
	*/
}


static int scsi_mpath_generic_chr_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct scsi_mpath_disk *mpath_disk = container_of(cdev, struct scsi_mpath_disk, cdev);

	pr_err("%s cdev=%pS mpath_disk=%pS\n", __func__, cdev, mpath_disk);

	if (!kref_get_unless_zero(&mpath_disk->ref)) {
		pr_err("%s1 mpath_disk=%pS ENXIO\n", __func__, mpath_disk);
		return -ENXIO;
	}
	return 0;
}

static int scsi_mpath_generic_chr_release(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct scsi_mpath_disk *mpath_disk = container_of(cdev, struct scsi_mpath_disk, cdev);

	pr_err("%s cdev=%pS mpath_disk=%pS\n", __func__, cdev, mpath_disk);

	kref_put(&mpath_disk->ref, scsi_mpath_free);
	return 0;
}

static long scsi_mpath_generic_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct scsi_mpath_disk *mpath_disk = container_of(cdev, struct scsi_mpath_disk, cdev);
	struct scsi_mpath_device *mpath_dev;
	struct scsi_device *sdev;
	fmode_t mode = file->f_mode;
	int srcu_idx, err;

	pr_err("%s cdev=%pS cmd=0x%x arg=%ld mpath_disk=%pS\n", __func__, cdev, cmd, arg, mpath_disk);
	
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_dev = scsi_find_path(mpath_disk);
	pr_err("%s1 cmd=0x%x arg=%ld mpath_disk=%pS called scsi_find_path srcu_idx=%d mpath_dev=%pS\n",
		__func__, cmd, arg, mpath_disk, srcu_idx, mpath_dev);
	if (!mpath_dev)
		goto out_unlock;
	sdev = mpath_dev->sdev;
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
	err = scsi_ioctl_block_when_processing_errors(sdev, cmd,
			(mode & O_NDELAY));
	if (err)
		goto out_unlock;

	pr_err("%s3 cmd=0x%x arg=%ld sdev=%pS calling scsi_ioctl\n", __func__, cmd, arg, sdev);
	err = scsi_ioctl(sdev, file->f_mode & FMODE_WRITE, cmd, (void __user *)arg);
	pr_err("%s3.1 cmd=0x%x arg=%ld sdev=%pS called scsi_ioctl err=%d\n", __func__, cmd, arg, sdev, err);

out_unlock:
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	return err;
}
static const struct file_operations scsi_mpath_generic_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= scsi_mpath_generic_chr_open,
	.release	= scsi_mpath_generic_chr_release,
	.unlocked_ioctl	= scsi_mpath_generic_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

#ifdef dsdsddsds
int nvme_cdev_add(struct cdev *cdev, struct device *cdev_device,
		const struct file_operations *fops, struct module *owner)
{

	minor = ida_alloc(&nvme_ns_chr_minor_ida, GFP_KERNEL);
	if (minor < 0)
		return minor;
	cdev_device->devt = MKDEV(MAJOR(nvme_ns_chr_devt), minor);
	cdev_device->class = &nvme_ns_chr_class;
	cdev_device->release = nvme_cdev_rel;
	device_initialize(cdev_device);
	cdev_init(cdev, fops);
	cdev->owner = owner;
	ret = cdev_device_add(cdev, cdev_device);
	if (ret)
		put_device(cdev_device);

	return ret;
}
static void nvme_cdev_rel(struct device *dev)
{
	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt));
}
#endif


static int scsi_mpath_disk_add_cdev(struct scsi_mpath_disk *mpath_disk)
{
	int ret, minor = mpath_disk->index;

	mpath_disk->cdev_device.parent = &mpath_disk->dev;
	ret = dev_set_name(&mpath_disk->cdev_device, "smpg%d",
						mpath_disk->index);
	pr_err("%s called dev_set_name ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	mpath_disk->cdev_device.devt = MKDEV(MAJOR(scsi_mpath_disk_chr_devt), minor);
	mpath_disk->cdev_device.class = &scsi_mpath_generic_class;
	//mpath_disk->cdev_device.release = nvme_cdev_rel;
	device_initialize(&mpath_disk->cdev_device);
	cdev_init(&mpath_disk->cdev, &scsi_mpath_generic_chr_fops);
	mpath_disk->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&mpath_disk->cdev, &mpath_disk->cdev_device);
	pr_err("%s1 called cdev_device_add ret=%d\n", __func__, ret);
	if (ret)
		put_device(&mpath_disk->cdev_device);
	return ret;
}

void scsi_mpath_set_live(struct scsi_mpath_device *mpath_dev)
{
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	int ret;

	pr_err("%s mpath_disk=%pS SCSI_MPATH_DISK_LIVE=%d\n",
		__func__, mpath_disk, test_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags));

	if (!test_and_set_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags)) {
		pr_err("%s calling device_add_disk &mpath_disk->dev=%pS\n", __func__, &mpath_disk->dev);
		ret = device_add_disk(&mpath_disk->dev, mpath_disk->gd, sd_attr_groups);
		pr_err("%s1 called device_add_disk ret=%d\n", __func__, ret);
		if (ret) {
			clear_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags);
			return;
		}
		pr_err("%s2 calling kblockd_schedule_work partition_scan_work\n", __func__);
		scsi_mpath_disk_add_cdev(mpath_disk);
		kblockd_schedule_work(&mpath_disk->partition_scan_work);
	}

	pr_info("Attached SCSI %s disk calling scsi_mpath_add_sysfs_link\n", "fixme");

	scsi_mpath_add_sysfs_link(mpath_disk);

	mutex_lock(&mpath_disk->lock);
	if (scsi_mpath_is_optimized(NULL)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_disk->srcu);
		for_each_online_node(node)
			__scsi_find_path(mpath_disk, node);
		srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_disk->lock);

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
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	bool retry = false;

	pr_err("%s sdev=%pS mpath_disk=%pS mpath_dev=%pS\n",
		__func__, sdev, mpath_disk, mpath_dev);

	switch (err) {
	case SCSI_DH_OK:
		break;
	case SCSI_DH_NOSYS:
		sdev_printk(KERN_ERR, sdev,
			"Could not failover the device scsi_dh_%s, Error %d\n",
			sdev->handler->name, err);
		scsi_mpath_clear_current_path(mpath_dev);
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
		if ((mpath_dev->state == SCSI_ACCESS_STATE_OFFLINE) ||
		    (mpath_dev->state == SCSI_ACCESS_STATE_UNAVAILABLE))
			scsi_mpath_clear_current_path(mpath_dev);
		err = 0;
		break;
	case SCSI_DH_DEV_OFFLINED:
	default:
		sdev_printk(KERN_ERR, sdev, "Device Handler Path offlined \n");
		scsi_mpath_clear_current_path(mpath_dev);
		break;
	}

	if (retry)
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &mpath_dev->flags);

        if (scsi_mpath_state_is_live2(mpath_dev->state)) {
			pr_err("%s calling scsi_mpath_set_live\n", __func__);
			scsi_mpath_set_live(mpath_dev);
        }
}

void scsi_activate_path(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;

	pr_err("%s mpath_dev=%pS sdev=%pS q\n",
		__func__, mpath_dev, sdev);
	#if 0
	if (!mpath_dh)
		return;

        if (!(scsi_mpath_state_is_live2(sdev->state))) {
		sdev_printk(KERN_INFO, sdev, "Path state is not live \n");
                return;
	}

	if (!blk_queue_dying(q)) {
		pr_err("%s1 mpath_dh=%pS calling scsi_dh_activate\n", __func__, mpath_dh);
		scsi_dh_activate(q, activate_mpath, sdev);
	} else {
		pr_err("%s2 mpath_dh=%pS calling scsi_dh_activate\n", __func__, mpath_dh);
		activate_mpath(sdev, SCSI_DH_OK);
	}
	#endif
}

static __maybe_unused void scsi_activate_mpath_work(struct work_struct *work)
{
    struct scsi_mpath_device *mpath_dev = container_of(work,
            struct scsi_mpath_device, activate);
    struct scsi_device *sdev;

    pr_err("%s mpath_dev=%pS\n", __func__, mpath_dev);
    sdev = mpath_dev->sdev;
    pr_err("%s2 sdev=%pS\n", __func__, sdev);
	if (!sdev)
		return;
}

void scsi_mpath_add_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	pr_err("%s mpath_dev=%pS\n", __func__, mpath_dev);
	mpath_disk = mpath_dev->disk;
	pr_err("%s1 mpath_disk=%pS\n", __func__, mpath_disk);

	if (scsi_mpath_state_is_live(mpath_dev->state)) {
		//mpath_disk->state = SCSI_MPATH_OPTIMAL;
		pr_err("%s calling scsi_mpath_set_live\n", __func__);
		scsi_mpath_set_live(mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_disk);

static bool scsi_available_mpath(struct scsi_mpath_disk *mpath_disk)
{
	struct scsi_mpath_device *mpath_dev;
	struct scsi_device *sdev;

	list_for_each_entry_rcu(mpath_dev, &mpath_disk->dev_list, entry) {
		sdev = mpath_dev->sdev;
		if (scsi_device_online(sdev))
			return true;
	}
	return false;
}

/*  called when shost is being freed */
void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;

	if (!mpath_disk)
		return;

	cancel_work_sync(&mpath_disk->requeue_work);
	cleanup_srcu_struct(&mpath_disk->srcu);

	kfree(mpath_dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_release);

__maybe_unused static void scsi_put_mpath_dev(struct scsi_device *sdev)
{
	scsi_device_put(sdev);
}

void scsi_mpath_revalidate_path(struct gendisk *disk, sector_t capacity)
{
	__maybe_unused struct scsi_mpath_disk *mpath_disk;
	__maybe_unused struct scsi_device *sdev;
	int srcu_idx;
	int node;

	mpath_disk = disk->private_data;
	pr_err("%s disk=%pS mpath_disk=%pS\n", __func__, disk, mpath_disk);

	if (!mpath_disk)
		return;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	pr_err("%s3 srcu_idx=%d\n", __func__, srcu_idx);
	#if 0
	list_for_each_entry_rcu(sdev, &mpath_disk->dev_list, mpath_dev_entry) {
		if (capacity != get_capacity(sdev->mpath_dev->gd))
			clear_bit(SCSI_MPATH_DISK_LIVE, &mpath_dev->flags);
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
}
EXPORT_SYMBOL_GPL(scsi_mpath_revalidate_path);

static int scsi_mpath_open(struct gendisk *disk, blk_mode_t mode)
{
	struct scsi_mpath_disk *mpath_disk = disk->private_data;
	pr_err("%s disk=%pS mpath_disk=%pS\n", __func__, disk, mpath_disk);

	if (!kref_get_unless_zero(&mpath_disk->ref)) {
		pr_err("%s1 disk=%pS ENXIO\n", __func__, disk);
		return -ENXIO;
	}

	return 0;
}

static void scsi_mpath_release(struct gendisk *disk)
{
	struct scsi_mpath_disk *mpath_disk = disk->private_data;
	kref_put(&mpath_disk->ref, scsi_mpath_free);
}

int scsi_mpath_failover_disposition(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);

	pr_err("%s scmd=%pS req=%pS\n", __func__, scmd, req);
	if (req->cmd_flags & REQ_SCSI_MPATH) {
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

static void scsi_multipath_submit_bio(struct bio *bio)
{
	struct scsi_mpath_disk *mpath_disk = bio->bi_bdev->bd_disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx;
	bool special = false;
	struct scsi_mpath_device *mpath_dev;

	//WARN_ON_ONCE(1);
	if (bio->bi_iter.bi_size == 16384)
		special = true;

	/*
	 * The scsi device might be going away and the bio might be
	 * moved to a difference queue via blk_steal_bios(), so we
	 * need to use bio_split pool from the original queue to
	 * allocate the bvecs from.
	 */
	if (special)
		pr_err("%s bio=%pS bi size=%d mpath_disk=%pS bio->bi_bdev=%pS\n", __func__, bio, bio->bi_iter.bi_size, mpath_disk, bio->bi_bdev);
	bio = bio_split_to_limits(bio);
	if (special)
		pr_err("%s1 bio=%pS mpath_disk=%pS called bio_split_to_limits\n", __func__, bio, mpath_disk);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_dev = scsi_find_path(mpath_disk);
	sdev = mpath_dev->sdev;
	if (special) {
		pr_err("%s2 bio=%pS bio->bi_bdev=%pS bio->bi_bdev->bd_disk=%pS bio->bi_bdev->bd_disk->part0=%pS mpath_disk=%pS mpath_dev=%pS\n",
			__func__, bio, bio->bi_bdev, bio->bi_bdev->bd_disk, bio->bi_bdev->bd_disk->part0, mpath_disk, mpath_dev);
		
		pr_err("%s2.1 bio=%pS sdev=%pS request_queue=%pS request_queue=%pS\n",
			__func__, bio, sdev, sdev->request_queue, sdev->request_queue);
		pr_err("%s2.2 bio=%pS sdev=%pS request_queue=%pS request_queue->disk=%pS\n",
			__func__, bio, sdev, sdev->request_queue, sdev->request_queue->disk);
		pr_err("%s2.3 bio=%pS sdev=%pS request_queue=%pS request_queue->disk->part0=%pS\n",
			__func__, bio, sdev, sdev->request_queue, sdev->request_queue->disk->part0);
	}
	if (likely(mpath_dev)) {
		bio_set_dev(bio, mpath_dev->sdev->request_queue->disk->part0);
		bio->bi_opf |= REQ_SCSI_MPATH;
		if (special)
			pr_err("%s3 bio=%pS bio->bi_bdev=%pS called bio_set_dev mpath_disk=%pS sdev=%pS calling submit_bio_noacct\n",
				__func__, bio, bio->bi_bdev, mpath_disk, sdev);
		submit_bio_noacct(bio);
		if (special)
			pr_err("%s4 bio=%pS mpath_disk=%pS sdev=%pS called submit_bio_noacct\n", __func__, bio, mpath_disk, sdev);
	//	BUG();
	} else if (scsi_available_mpath(mpath_disk)) {
		sdev_printk(KERN_NOTICE, NULL,
		    "No Usable Path - Requeing I/O \n");

		spin_lock_irq(&mpath_disk->requeue_lock);
		bio_list_add(&mpath_disk->requeue_list, bio);
		spin_unlock_irq(&mpath_disk->requeue_lock);
	} else {
		sdev_printk(KERN_NOTICE, NULL,
		    "No available path = Failing I/O \n");

		bio_io_error(bio);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}

static int scsi_mpath_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct scsi_mpath_disk *mpath_disk = disk->private_data;
	__maybe_unused struct scsi_device *sdev;
	int srcu_idx, ret = -EWOULDBLOCK;
	struct scsi_mpath_device *mpath_dev;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	mpath_dev = scsi_find_path(mpath_disk);
	if (mpath_dev)
		ret = scsi_mpath_unique_id(mpath_dev->sdev, id, type);
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);

	return ret;
}

const struct block_device_operations scsi_mpath_ops = {
	.owner          = THIS_MODULE,
	.submit_bio	= scsi_multipath_submit_bio,
	.open		= scsi_mpath_open,
	.release	= scsi_mpath_release,
	.ioctl			= scsi_mpath_ioctl,
	.get_unique_id	= scsi_mpath_get_unique_id,
};

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

bool scsi_mpath_lun_id_match(struct scsi_mpath_device *a, struct scsi_mpath_device *b)
{
	if (a->device_id_len != b->device_id_len)
		return false;

	return !strncmp(a->device_id_str, b->device_id_str, a->device_id_len);
}
EXPORT_SYMBOL_GPL(scsi_mpath_lun_id_match);

int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	#if 0
	struct scsi_mpath_dh_data *dh_data;
	char device_id_str[40];
	int ret = -EINVAL;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	dh_data = sdev->pg_data;
	pr_err("%s1 sdev=%pS dh_data=%pS\n", __func__, sdev, dh_data);
	pr_err("%s2 sdev=%pS dh_data=%pS device_id_len=%d\n",
		__func__, sdev, dh_data, dh_data->device_id_len);
	pr_err("%s3 sdev=%pS dh_data=%pS device_id_str=%s\n",
		__func__, sdev, dh_data, dh_data->device_id_str);

	ret = scsi_vpd_lun_id(sdev, device_id_str, sizeof(device_id_str));
	if (ret < 0)
		return ret;
	pr_err("%s4 sdev=%pS dh_data=%pS \n", __func__, sdev, dh_data);
	pr_err("%s4.1 device_id_len=%d device_id_str=%s (len=%zd) dh_data->device_id_str=%s (len=%zd)\n",
		__func__,
		dh_data->device_id_len, device_id_str, strlen(dh_data->device_id_str),
		dh_data->device_id_str, strlen(dh_data->device_id_str));

	if (strncmp(dh_data->device_id_str, device_id_str,
	    dh_data->device_id_len) == 0) {
		pr_err("%s4.1 matches\n", __func__);
		return 0;
	}
	#endif
	return -EINVAL;
}

static void scsi_mpath_disk_release(struct device *dev)
{
	pr_err("%s dev=%pS\n", __func__, dev);
}

static int scsi_multipath_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_device *mpath_dev;
	int ret = -ENOMEM;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	mpath_dev = kzalloc(sizeof(*mpath_dev), GFP_KERNEL);
	if (!mpath_dev)
		return ret;
	mpath_dev->sdev = sdev;
	sdev->mpath_dev = mpath_dev;

	pr_err("%s2 sdev=%pS sdev->mpath_dev=%pS shost=%pS\n",
		__func__, sdev, sdev->mpath_dev, shost);

	INIT_WORK(&mpath_dev->activate, scsi_activate_mpath_work);
	mpath_dev->numa_node = NUMA_NO_NODE;

	return 0;
}

static struct scsi_mpath_disk *scsi_mpath_find_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_disk *mpath_disk;

	mutex_lock(&mpath_disks_lock);
	list_for_each_entry(mpath_disk, &mpath_disks_list, entry) {

		pr_err("%s itering mpath_disk=%pS sdev->mpath_dev=%pS\n", __func__, mpath_disk, sdev->mpath_dev);
		pr_err("%s1 wwid=%s\n", __func__, mpath_disk->wwid);
		pr_err("%s2 sdev->mpath_dev->device_id_str=%s\n", __func__, sdev->mpath_dev->device_id_str);
		pr_err("%s2.1 sdev->mpath_dev->device_id_len=%d\n", __func__, sdev->mpath_dev->device_id_len);

		if (strncmp(mpath_disk->wwid, sdev->mpath_dev->device_id_str, sdev->mpath_dev->device_id_len) == 0) {
			pr_err("%s3 matches wwid\n", __func__);
			mutex_unlock(&mpath_disks_lock);
			return mpath_disk;
		}
	}
	mutex_unlock(&mpath_disks_lock);
	return NULL;
}

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_alloc_disk(struct scsi_device *sdev, struct gendisk *gd)
{
	struct queue_limits lim;
	int ret;
	static int disk_count;
	struct scsi_mpath_disk *mpath_disk;
	__maybe_unused int index;
	struct Scsi_Host *shost = sdev->host;
	struct device *shost_dev = &shost->shost_dev;

	pr_err("%s sdev=%pS sdev->mpath_dev=%pS shost=%pS shost_dev=%pS scsi_device_tpgs=%d\n",
		__func__, sdev, sdev->mpath_dev, shost, shost_dev, scsi_device_tpgs(sdev));

	if (!scsi_device_tpgs(sdev)) {
		sdev_printk(KERN_NOTICE, sdev, "tpgs are required for mpath support\n");
		return -ENODEV;
	}

	pr_err("%s1 sdev=%pS sdev->mpath_dev=%pS shost=%pS shost_dev=%pS calling scsi_multipath_init\n",
		__func__, sdev, sdev->mpath_dev, shost, shost_dev);
	scsi_multipath_init(sdev);
	pr_err("%s1.1 sdev=%pS sdev->mpath_dev=%pS shost=%pS shost_dev=%pS called scsi_multipath_init\n",
		__func__, sdev, sdev->mpath_dev, shost, shost_dev);

	sdev->mpath_dev->gd = gd;

	/*
	 * Add multipath disk only if scsi host supports multipath modparam
	 */
	if (!scsi_multipath) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s Handler attached but modparam scsi_multipath is set to false \n",
		    sdev->handler->name);
		return 0;
	}

	if (alua_bus_attach(sdev)) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s sdev=%pS alua_bus_attach failed\n", __func__, sdev);

	}

	pr_err("%s4 calling scsi_mpath_find_disk sdev=%pS\n", __func__, sdev);
	mpath_disk = scsi_mpath_find_disk(sdev);
	pr_err("%s4.1 called scsi_mpath_find_disk sdev=%pS mpath_disk=%pS\n", __func__, sdev, mpath_disk);
	if (mpath_disk) {
		mutex_lock(&mpath_disk->lock);
		list_add_tail(&sdev->mpath_dev->entry, &mpath_disk->dev_list);
		mutex_unlock(&mpath_disk->lock);
		sdev->mpath_dev->disk = mpath_disk;
		return 0;
	}

	mutex_unlock(&mpath_disks_lock);

	mpath_disk = kzalloc(sizeof(*mpath_disk), GFP_KERNEL);
	pr_err("%s5 sdev=%pS sdev->mpath_dev=%pS shost=%pS shost_dev=%pS mpath_disk=%pS\n",
		__func__, sdev, sdev->mpath_dev, shost, shost_dev, mpath_disk);
	if (!mpath_disk)
		return -ENOMEM;
	sdev->mpath_dev->disk = mpath_disk;

	mpath_disk->index = ida_alloc(&sd_mpath_index_ida, GFP_KERNEL);

	INIT_LIST_HEAD(&mpath_disk->entry);
	INIT_LIST_HEAD(&mpath_disk->dev_list);
	INIT_WORK(&mpath_disk->partition_scan_work, scsi_multipath_partition_scan_work);
	pr_err("%s6\n", __func__);
	mutex_init(&mpath_disk->lock);
	kref_init(&mpath_disk->ref);

	mpath_disk->dev.class = &scsi_mpath_disk_class;
	mpath_disk->dev.release = scsi_mpath_disk_release;
	mpath_disk->dev.groups = scsi_mpath_disk_attrs_groups;
	pr_err("%s7\n", __func__);
	dev_set_name(&mpath_disk->dev, "smpd%d", mpath_disk->index);
	disk_count++;
	device_initialize(&mpath_disk->dev);

	blk_set_stacking_limits(&lim);
	pr_err("%s8\n", __func__);

	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL;
	lim.max_zone_append_sectors = 0;
	lim.dma_alignment = 3;

	mpath_disk->gd = blk_alloc_disk(&lim, dev_to_node(shost_dev));
	pr_err("%s9 dev=%pS sdev->mpath_dev=%pS mpath_disk->gd=%pS\n", __func__, sdev, sdev->mpath_dev, mpath_disk->gd);
	if (IS_ERR(mpath_disk->gd))
		return PTR_ERR(mpath_disk->gd);

	mpath_disk->gd->private_data = mpath_disk;
	mpath_disk->gd->fops = &scsi_mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->gd->state);
	sprintf(mpath_disk->gd->disk_name, "smpd%d", mpath_disk->index);

	dev_err(&mpath_disk->dev, "%s10 calling device_add for &mpath_disk->dev\n", __func__);
	ret = device_add(&mpath_disk->dev); // see nvme_init_subsystem()
	pr_err("%s11 called device_add ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	ret = init_srcu_struct(&mpath_disk->srcu);
	pr_err("%s12 ret=%d after init_srcu_struct\n", __func__, ret);
	if (ret)
		return ret;

	INIT_WORK(&mpath_disk->requeue_work, scsi_requeue_work);
	spin_lock_init(&mpath_disk->requeue_lock);
	bio_list_init(&mpath_disk->requeue_list);

	sprintf(mpath_disk->wwid, sdev->mpath_dev->device_id_str, sdev->mpath_dev->device_id_len);

	pr_err("%s13 ret=%d after bio_list_init sdev->mpath_dev=%pS\n", __func__, ret, sdev->mpath_dev);
	list_add_tail(&sdev->mpath_dev->entry, &mpath_disk->dev_list);

	mutex_lock(&mpath_disks_lock);
	list_add_tail(&mpath_disk->entry, &mpath_disks_list);

	pr_err("%s16\n", __func__);
	mutex_unlock(&mpath_disks_lock);

	pr_err("%s16 out\n", __func__);
	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_alloc_disk);

void scsi_mpath_start_request(struct request *req)
{
	__maybe_unused struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	__maybe_unused struct scsi_device *sdev = cmd->device;
	__maybe_unused struct scsi_mpath_disk *mpath_disk = sdev->mpath_dev->disk;
}

void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = cmd->device;

	pr_err("%s req=%pS bio=%pS cmd=%pS sdev=%pS\n", __func__, req, req->bio, cmd, sdev);
}

#if 0
void scsi_mpath_kick_requeue_lists(struct Scsi_Host *shost)
{
	struct scsi_mpath_disk *mpath_disk = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_dev, entry) {
		if (sdev->is_shared)
			continue;

		kblockd_schedule_work(&mpath_disk->requeue_lock);
		if (sdev->sdev_state == SDEV_RUNNING)
			disk_uevent(sdev->mpath_dev, KOBJ_CHANGE);
	}
	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}
#endif

void scsi_mpath_shutdown_disk(struct scsi_device *sdev)
{
	struct scsi_mpath_disk *mpath_disk;

	if (!sdev->mpath_dev || !sdev->mpath_dev->disk)
		return;

	mpath_disk = sdev->mpath_dev->disk;

	pr_err("%s clearing SCSI_MPATH_DISK_LIVE (if set) sdev=%pS\n", __func__, sdev);
	if (test_and_clear_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags)) {
		synchronize_srcu(&mpath_disk->srcu);
		kblockd_schedule_work(&mpath_disk->requeue_work);
	//	del_gendisk(sdev->mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_shutdown_disk);

void scsi_mpath_remove_disk(struct scsi_device *sdev)
{
	if (!sdev->mpath_dev || !sdev->mpath_dev->disk)
		return;

//	if (!sdev->is_shared)
//		return;

	/* Make sure All pending bio's are cleaned up */
	kblockd_schedule_work(&sdev->mpath_dev->disk->requeue_work);
	flush_work(&sdev->mpath_dev->disk->requeue_work);
	//put_disk(sdev->mpath_dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_disk);

int scsi_mpath_update_state(struct scsi_mpath_device *mpath_dev)
{
		switch(mpath_dev->state) {
		case SCSI_ACCESS_STATE_OPTIMAL:
			mpath_dev->state = SCSI_MPATH_OPTIMAL;
			break;
		case SCSI_ACCESS_STATE_ACTIVE:
			mpath_dev->state = SCSI_MPATH_ACTIVE;
			break;
		case SCSI_ACCESS_STATE_STANDBY:
			mpath_dev->state = SCSI_MPATH_STANDBY;
			break;
		case SCSI_ACCESS_STATE_UNAVAILABLE:
			mpath_dev->state = SCSI_MPATH_UNAVAILABLE;
			break;
		case SCSI_ACCESS_STATE_TRANSITIONING:
			mpath_dev->state = SCSI_MPATH_TRANSITIONING;
			break;
		case SCSI_ACCESS_STATE_OFFLINE:
		default:
			mpath_dev->state = SCSI_MPATH_OFFLINE;
		    break;
	}

	return mpath_dev->state;
}

static void scsi_mpath_disk_probe(dev_t devt)
{
	pr_err("%s devt=%d\n", __func__, devt);
}

static int __init init_scsi_mp(void)
{
	int err = class_register(&scsi_mpath_disk_class);

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