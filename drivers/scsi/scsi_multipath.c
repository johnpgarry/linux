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

static DEFINE_IDA(sd_mpath_index_ida);



bool scsi_multipath = true;
module_param(scsi_multipath, bool, 0444);
MODULE_PARM_DESC(scsi_multipath,
    "turn on native support for multiple scsi devices \n"
    "set this value to false to disable multipath, \n");

static const char *scsi_iopolicy_names[] = {
	[SCSI_MPATH_IOPOLICY_NUMA]	= "numa",
	[SCSI_MPATH_IOPOLICY_RR]	= "round-robin",
};

static int iopolicy = SCSI_MPATH_IOPOLICY_NUMA;

static LIST_HEAD(mpath_disks_list);
static DEFINE_MUTEX(mpath_disks_lock);

static const struct class scsi_mpath_disk_class = {
	.name = "scsi_mpath_disk",
};

static ssize_t scsi_mpath_disk_attr_model_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
//	struct scsi_mp_disk *scsi_mp_disk =
//		container_of(dev, struct scsi_mp_disk, dev);

	return sysfs_emit(buf, "%d\n", 123);
}

struct device_attribute scsi_mpath_disk_attr_model = \
		__ATTR(model, S_IRUGO, scsi_mpath_disk_attr_model_show, NULL);


static struct attribute *scsi_mpath_disk_attrs[] = {
	&scsi_mpath_disk_attr_model.attr,
	NULL,
};

static const struct attribute_group scsi_mpath_disk_attrs_group = {
	.attrs = scsi_mpath_disk_attrs,
};

const struct attribute_group *scsi_mpath_disk_attrs_groups[] = {
	&scsi_mpath_disk_attrs_group,
	NULL
};

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
	return sprintf(buf, "%s\n", scsi_iopolicy_names[iopolicy]);
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

static inline bool scsi_mpath_state_is_live(enum scsi_mpath_access_state state)
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
	if (!scsi_mpath_state_is_live(sdev->mpath_dev->state))
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

	list_for_each_entry_rcu(mpath_dev, &mpath_disk->dev_list, entry) {
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
	__maybe_unused struct device *dev = container_of(kobj, struct device, kobj);

	//return nvme_disk_is_ns_head(dev_to_disk(dev));
	return true;
}

static bool multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	return false;
}

DEFINE_SYSFS_GROUP_VISIBLE(multipath_sysfs)

const struct attribute_group scsi_mpath_attr_group = {
	.name           = "multipath",
	.attrs		= scsi_mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),
};


static void scsi_mpath_add_sysfs_link(struct scsi_mpath_disk *mpath_disk)
{
	__maybe_unused struct device *target;
	__maybe_unused int rc, srcu_idx;
	struct kobject *kobj;
	struct scsi_device *sdev;
	struct scsi_mpath_device *mpath_dev;

	pr_err("%s mpath_disk=%pS GD_ADDED=%d\n",
		__func__, mpath_disk, test_bit(GD_ADDED, &mpath_disk->gd->state));

	pr_err("%s3 mpath_disk->gd=%pS\n", __func__, mpath_disk->gd);
	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_disk->gd->state))
		return;

	kobj = &disk_to_dev(mpath_disk->gd)->kobj;
	srcu_idx = srcu_read_lock(&mpath_disk->srcu);
	pr_err("%s4 mpath_disk->gd=%pS srcu_idx=%d\n", __func__, mpath_disk->gd, srcu_idx);

	list_for_each_entry_srcu(mpath_dev, &mpath_disk->dev_list, entry,
				 srcu_read_lock_held(&mpath_disk->srcu)) {

		pr_err("%s5 mpath_dev=%pS\n", __func__, mpath_dev);
		if (!mpath_dev)
			continue;
		
		/*
		 * Ensure that ns path disk node is already added otherwise we
		 * may get invalid kobj name for target
		 */
		sdev = mpath_dev->sdev;
		pr_err("%s1 itering mpath_dev=%pS sdev=%pS\n", __func__, mpath_dev, sdev);
		pr_err("%s1.1 itering mpath_dev=%pS sdev->request_queue=%pS\n", __func__, mpath_dev, sdev->request_queue);
		if (!sdev->request_queue)
			continue;
		pr_err("%s1.2 itering mpath_dev=%pS mpath_dev->gd=%pS\n", __func__, mpath_dev, mpath_dev->gd);
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
		if (test_and_set_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags))
			continue;

		pr_err("%s1.3 itering mpath_dev=%pS mpath_dev->gd=%pS\n", __func__, mpath_dev, mpath_dev->gd);
		target = disk_to_dev(mpath_dev->gd);
		pr_err("%s1.4 itering mpath_dev=%pS mpath_dev->gd=%pS target=%pS\n", __func__, mpath_dev, mpath_dev->gd, target);
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(kobj, scsi_mpath_attr_group.name,
				&target->kobj, dev_name(target));
		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_disk->gd),
					"failed to create link to %s\n",
					dev_name(target));
			clear_bit(SCSI_MPATH_SYSFS_ATTR_LINK, &mpath_dev->flags);
		}
	}


	srcu_read_unlock(&mpath_disk->srcu, srcu_idx);
}

void scsi_mpath_set_live(struct scsi_mpath_device *mpath_dev)
{
	int ret;
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;

	pr_err("%s mpath_disk=%pS SCSI_MPATH_DISK_LIVE=%d\n",
		__func__, mpath_disk, test_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags));


	if (!test_and_set_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags)) {
		pr_err("%s calling device_add_disk\n", __func__);
		ret = device_add_disk(&mpath_disk->dev, mpath_disk->gd, NULL);
		pr_err("%s1 called device_add_disk ret=%d\n", __func__, ret);
		if (ret) {
			clear_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags);
			return;
		}
		pr_err("%s2 calling kblockd_schedule_work partition_scan_work\n", __func__);
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
	struct scsi_mpath_dh_data *mpath_h = mpath_dev->pg_data;
	bool retry = false;

	pr_err("%s sdev=%pS mpath_disk=%pS mpath_h=%pS\n",
		__func__, sdev, mpath_disk, mpath_h);
	if (!mpath_h)
		return;

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
		if ((mpath_h->state == SCSI_ACCESS_STATE_OFFLINE) ||
		    (mpath_h->state == SCSI_ACCESS_STATE_UNAVAILABLE))
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

        if (scsi_mpath_state_is_live(mpath_dev->state)) {
			pr_err("%s calling scsi_mpath_set_live\n", __func__);
			scsi_mpath_set_live(mpath_dev);
        }
}

void scsi_activate_path(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	struct scsi_mpath_dh_data *mpath_dh = mpath_dev->pg_data;

	pr_err("%s mpath_dh=%pS sdev=%pS q\n",
		__func__, mpath_dh, mpath_dh);
	#if 0
	if (!mpath_dh)
		return;

        if (!(scsi_mpath_state_is_live(sdev->state))) {
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
    struct scsi_device *sdev = mpath_dev->sdev;

	if (!sdev)
		return;

	scsi_activate_path(sdev);
}

int scsi_mpath_add_disk(struct scsi_mpath_device *mpath_dev)
{
	struct scsi_mpath_disk *mpath_disk = mpath_dev->disk;
	pr_err("%s mpath_dev=%pS\n", __func__, mpath_dev);
	mpath_disk = mpath_dev->disk;
	pr_err("%s1 mpath_disk=%pS\n", __func__, mpath_disk);
	
	if (!mpath_dev->pg_data) {
		/* Re initialize ALUA */
	//	sdev->handler->rescan(sdev);
	} else {
		//mpath_disk->state = SCSI_MPATH_OPTIMAL;
		pr_err("%s not calling scsi_mpath_set_live\n", __func__);
		scsi_mpath_set_live(mpath_dev);
	}

	return (test_bit(SCSI_MPATH_DISK_LIVE, &mpath_disk->flags));
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_disk);

int scsi_multipath_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_dh_data *h;
	struct scsi_mpath_device *mpath_dev;
	int ret = -ENOMEM;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	mpath_dev = kzalloc(sizeof(*mpath_dev), GFP_KERNEL);
	if (!mpath_dev)
		return ret;
	mpath_dev->sdev = sdev;
	sdev->mpath_dev = mpath_dev;


	h = kzalloc(sizeof(struct scsi_mpath_dh_data), GFP_KERNEL);
	if (!h)
		goto out_mpath_dev;

	mpath_dev->pg_data = h;
	
#if 0
struct gendisk          	*mpath_disk;	/* Multipath disk */
	
	enum scsi_mpath_access_state	state;	/* Multipath State */
	//enum scsi_mpath_iopolicy	mpath_iopolicy;	/* IO Policy */
	struct list_head		entry;	/* list of all mpath_sdevs */
	struct scsi_mpath_dh_data	*pg_data; /* Place holder for Port group data */
	struct work_struct		activate; /* Activate path work */
	int				numa_node; /* NUMA node for Path  */
	//atomic_t			nr_mpath;	/* Number of Active mpath */


#define SCSI_MPATH_DISK_LIVE            0
#define SCSI_MPATH_DISK_IO_PENDING      1
#define SCSI_MPATH_IO_STATS             2

	unsigned long           flags;		/* flag for multipath devices*/
	struct scsi_mpath_disk *disk;
	#endif
//	ret = init_srcu_struct(&mpath_disk->srcu);
//	if (ret) {
//		cleanup_srcu_struct(&mpath_disk->srcu);
//		goto out_handler;
//	}

	pr_err("%s sdev=%pS sdev->mpath_dev=%pS h=%pS shost=%pS\n",
		__func__, sdev, sdev->mpath_dev, h, shost);

//	mutex_init(&mpath_dev->mpath_lock);
//	bio_list_init(&mpath_disk->requeue_list);
//	spin_lock_init(&mpath_disk->requeue_lock);
//	INIT_WORK(&mpath_disk->requeue_lock, scsi_requeue_work);
//	INIT_LIST_HEAD(&mpath_dev->mpath_list);
	INIT_WORK(&mpath_dev->activate, scsi_activate_mpath_work);
//	INIT_LIST_HEAD(&sdev->mpath_entry);
	mpath_dev->numa_node = NUMA_NO_NODE;
//	sdev->is_shared = 1;

	return 0;

//out_handler:
//	kfree(h);
out_mpath_dev:
//	if (mpath_dev)
//		kfree(mpath_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(scsi_multipath_init);

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

	kfree(mpath_dev->pg_data);
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
//	if (bio->bi_iter.bi_size == 16384)
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
	if (special) {
		pr_err("%s2 bio=%pS bio->bi_bdev=%pS bio->bi_bdev->bd_disk=%pS bio->bi_bdev->bd_disk->part0=%pS mpath_disk=%pS\n",
			__func__, bio, bio->bi_bdev, bio->bi_bdev->bd_disk, bio->bi_bdev->bd_disk->part0, mpath_disk);
		pr_err("%s2.1 bio=%pS sdev=%pS request_queue=%pS request_queue->disk=%pS request_queue->disk->part0=%pS\n",
			__func__, bio, sdev, sdev->request_queue, sdev->request_queue->disk, sdev->request_queue->disk->part0);
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
	struct scsi_mpath_dh_data *dh_data_a = a->pg_data, *dh_data_b = b->pg_data;

	if (!dh_data_a || !dh_data_b)
		return false;

	if (dh_data_a->device_id_len != dh_data_b->device_id_len)
		return false;

	return !strncmp(dh_data_a->device_id_str, dh_data_b->device_id_str, dh_data_a->device_id_len);
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

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_alloc_disk(struct scsi_device *sdev, struct gendisk *gd)
{
	struct queue_limits lim;
	int ret;
	static int disk_count;
	struct scsi_mpath_disk *mpath_disk;
	int index;
	struct Scsi_Host *shost = sdev->host;
	struct device *shost_dev = &shost->shost_dev;

	pr_err("%s sdev=%pS sdev->mpath_dev=%pS shost=%pS shost_dev=%pS\n",
		__func__, sdev, sdev->mpath_dev, shost, shost_dev);
	/*
	 * Don't allocate mpath disk if ALUA handler is not attached
	 */
	if (!sdev->handler || strncmp(sdev->handler->name, "alua", 4) != 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "No Handler or correct handler attached for multipath\n");
		return 0;
	}

	if (!sdev->mpath_dev)
		return 0;

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

	if (scsi_mpath_unique_lun_id(sdev) == 0) {
	//	sdev_printk(KERN_NOTICE, sdev,
	//	    "existing sdev with path, return\n");
	//	return 0;
	}

	mutex_lock(&mpath_disks_lock);
	list_for_each_entry(mpath_disk, &mpath_disks_list, entry) {
		struct scsi_mpath_device *mpath_dev;

		pr_err("%s itering mpath_dev=%pS\n", __func__, mpath_dev);
		mpath_dev = list_first_entry(&mpath_disk->dev_list, struct scsi_mpath_device, entry);
		pr_err("%s2 itering mpath_dev=%pS mpath_dev->sdev=%pS sdev=%pS\n", __func__, mpath_dev, mpath_dev->sdev, sdev);


		if (scsi_mpath_lun_id_match(sdev->mpath_dev, mpath_dev)) {
			pr_err("%s3 matches device_id_str\n", __func__);
			//sdev->mpath_dev = mpath_dev; allocate
			mutex_lock(&mpath_disk->lock);
			list_add_tail(&mpath_dev->entry, &mpath_disk->dev_list);
			mutex_unlock(&mpath_disk->lock);
			mutex_unlock(&mpath_disks_lock);
			return 0;
		}
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
	dev_set_name(&mpath_disk->dev, "scsi_mpath_disk%d", mpath_disk->index);
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
	sprintf(mpath_disk->gd->disk_name, "scsi_mpath_disk%d", mpath_disk->index);

	pr_err("%s10\n", __func__);
	ret = device_add(&mpath_disk->dev); // see nvme_init_subsystem()
	pr_err("%s11 called device_add ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	//sdp->mpath_dev->major = sd_major((mpath_dev->index & 0xf0) >> 4);
	//sdp->mpath_dev->first_minor = ((mpath_dev->index & 0xf) << 4) | (mpath_dev->index & 0xfff00);
	//sdp->mpath_dev->minors = SD_MINORS;

	if (0 && !test_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_dev->flags)) {
		BUG();
	//	device_unregister(&sdkp->disk_dev);
	//	clear_bit(SCSI_MPATH_DISK_LIVE, &mpath_dev->flags);
	//	put_disk(sdp->mpath_disk);
	//	goto out;
	}

	ret = init_srcu_struct(&mpath_disk->srcu);
	pr_err("%s12 ret=%d after init_srcu_struct\n", __func__, ret);
	if (ret)
		return ret;

	INIT_WORK(&mpath_disk->requeue_work, scsi_requeue_work);
	spin_lock_init(&mpath_disk->requeue_lock);
	bio_list_init(&mpath_disk->requeue_list);

	pr_err("%s13 ret=%d after bio_list_init sdev->mpath_dev=%pS\n", __func__, ret, sdev->mpath_dev);
	list_add_tail(&sdev->mpath_dev->entry, &mpath_disk->dev_list);

	pr_err("%s14 major=%d first_minor=%d index=%d mpath_disk->index=%d calling scsi_mpath_add_disk\n",
		__func__, 0, 0, index, mpath_disk->index);
	scsi_mpath_add_disk(sdev->mpath_dev);
	pr_err("%s15 major=%d first_minor=%d index=%d mpath_disk->index=%d called scsi_mpath_add_disk\n",
		__func__, 0, 0, index, mpath_disk->index);

	mutex_lock(&mpath_disks_lock);
	list_add_tail(&mpath_disk->entry, &mpath_disks_list);

	pr_err("%s16\n", __func__);
	mutex_unlock(&mpath_disks_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_alloc_disk);

void scsi_mpath_start_request(struct request *req)
{
	__maybe_unused struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	__maybe_unused struct scsi_device *sdev = cmd->device;
	__maybe_unused struct scsi_mpath_disk *mpath_disk = sdev->mpath_dev->disk;

//	if (!blk_queue_io_stat(mpath_dev->queue) ||
//	    blk_rq_is_passthrough(req))
//		return;

	//req->rq_flags |= SCSI_MPATH_IO_STATS;
//	mpath_dev->mpath_start_time = bdev_start_io_acct(sdev->mpath_dev->part0,
//	    req_op(req), jiffies);
}

void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = cmd->device;

	pr_err("%s req=%pS bio=%pS cmd=%pS sdev=%pS\n", __func__, req, req->bio, cmd, sdev);
//	if (!(req->rq_flags & SCSI_MPATH_IO_STATS))
		return;

	pr_err("%s1 req=%pS bio=%pS cmd=%pS sdev=%pS calling bdev_end_io_acct\n", __func__, req, req->bio, cmd, sdev);
//	bdev_end_io_acct(sdev->mpath_dev->part0, req_op(req),
//	    blk_rq_bytes(req) >> SECTOR_SHIFT,
//	    mpath_dev->mpath_start_time);
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
        struct scsi_mpath_dh_data *mpath_h;

        mpath_h = mpath_dev->pg_data;
        if (!mpath_h)
		return SCSI_MPATH_UNAVAILABLE;

	switch(mpath_h->state) {
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

static int __init init_scsi_mp(void)
{
	return class_register(&scsi_mpath_disk_class);
}

/**
 *	exit_sd - exit point for this driver (when it is a module).
 *
 *	Note: this function unregisters this driver from the scsi mid-level.
 **/
static void __exit exit_scsi_mp(void)
{
	pr_err("%s\n", __func__);
}

module_init(init_scsi_mp);
module_exit(exit_scsi_mp);