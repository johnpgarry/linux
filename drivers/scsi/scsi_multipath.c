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

static const struct class scsi_mp_disk_class = {
	.name = "scsi_mp_disk",
};

struct scsi_mp_diskw {
	int			instance;
	struct device		dev;
	/*
	 * Because we unregister the device on the last put we need
	 * a separate refcount.
	 */
	struct kref		ref;
	struct list_head	entry;
	struct work_struct	partition_scan_work;
	struct gendisk          	*gd;	/* Multipath disk */




	#ifdef dsdsd
	struct mutex		lock;
	struct list_head	ctrls;
	struct list_head	nsheads;
	char			subnqn[NVMF_NQN_SIZE];
	char			serial[20];
	char			model[40];
	char			firmware_rev[8];
	u8			cmic;
	enum nvme_subsys_type	subtype;
	u16			vendor_id;
	u16			awupf; /* 0's based value. */
	struct ida		ns_ida;
#ifdef CONFIG_NVME_MULTIPATH
	enum nvme_iopolicy	iopolicy;
#endif
	#endif
};

static ssize_t scsi_mp_disk_attr_model_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
//	struct scsi_mp_disk *scsi_mp_disk =
//		container_of(dev, struct scsi_mp_disk, dev);

	return sysfs_emit(buf, "%d\n", 123);
}

struct device_attribute scsi_mp_disk_attr_model = \
		__ATTR(model, S_IRUGO, scsi_mp_disk_attr_model_show, NULL);


static struct attribute *scsi_mp_disk_attrs[] = {
	&scsi_mp_disk_attr_model.attr,
	NULL,
};

static const struct attribute_group scsi_mp_disk_attrs_group = {
	.attrs = scsi_mp_disk_attrs,
};

const struct attribute_group *scsi_mp_disk_attrs_groups[] = {
	&scsi_mp_disk_attrs_group,
	NULL
};

static void scsi_multipath_partition_scan_work(struct work_struct *work)
{
	struct scsi_mpath_device *mpath_dev =
		container_of(work, struct scsi_mpath_device, partition_scan_work);

	pr_err("%s mpath_dev=%pS GD_SUPPRESS_PART_SCAN=%d\n",
		__func__, mpath_dev, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_dev->gd->state));
	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_dev->gd->state)))
		return;

	//mutex_lock(&head->disk->open_mutex);
	pr_err("%s2 mpath_dev=%pS GD_SUPPRESS_PART_SCAN=%d calling bdev_disk_changed\n",
		__func__, mpath_dev, test_bit(GD_SUPPRESS_PART_SCAN, &mpath_dev->gd->state));
	bdev_disk_changed(mpath_dev->gd, false);
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

void scsi_mpath_default_iopolicy(struct scsi_device *sdev)
{
	sdev->mpath_iopolicy = iopolicy;
}

void scsi_multipath_iopolicy_update(struct scsi_device *sdev, int iopolicy)
{
	pr_err("%s sdev=%pS iopolicy=%d\n", __func__, sdev, iopolicy);
	#if 0
	struct Scsi_Host *shost =  sdev->host;
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	int old_iopolicy = READ_ONCE(sdev->mpath_iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(sdev->mpath_iopolicy, iopolicy);

	/* iopoliocy changes clear the multipath */
	mutex_lock(&mpath_dev->mpath_lock);
	list_for_each_entry_rcu(sdev, &shost->mpath_dev, mpath_entry)
		//scsi_mpath_clear_paths(shost);
	mutex_unlock(&mpath_dev->mpath_lock);

	sdev_printk(KERN_NOTICE, sdev, "Multipath iopolocy changed from %s to %s\n",
	    scsi_iopolicy_names[old_iopolicy], scsi_iopolicy_names[iopolicy]);
	  #endif
}

bool scsi_mpath_clear_current_path(struct scsi_device *sdev)
{
	__maybe_unused struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
	bool changed = false;
	int node;

	if (!mpath_dev)
		return changed;

	for_each_node(node) {
		if (sdev == rcu_access_pointer(mpath_dev->current_path[node])) {
			rcu_assign_pointer(mpath_dev->current_path[node], NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(scsi_mpath_clear_current_path);

void scsi_mpath_clear_paths(struct scsi_mpath_device *mpath_dev)
{
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	list_for_each_entry_rcu(sdev, &mpath_dev->mpath_sdev_list, mpath_entry) {
		scsi_mpath_clear_current_path(sdev);
		kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);

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
	if (!scsi_mpath_state_is_live(sdev->mpath_state))
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
	unsigned long flags;
	struct bio *bio;

	if (!scsi_device_online(sdev) || sdev->was_reset || sdev->locked)
		return;

	scsi_mpath_clear_current_path(sdev);

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
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &sdev->mpath_flags);
		queue_work(shost->work_q, &mpath_dev->mpath_requeue_work);
	}

	/*
	 * following logic tries to steal bio, check if the bio has polled
	 * operation, if yes, then clear polled reqeust and reqeue bio
	 */
	spin_lock_irqsave(&mpath_dev->mpath_requeue_lock, flags);
	for (bio = req->bio; bio; bio = bio->bi_next) {
		bio_set_dev(bio, req->q->disk->part0);
		if (bio->bi_opf & REQ_POLLED) {
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;
		}
	}
	blk_steal_bios(&mpath_dev->mpath_requeue_list, req);
	spin_unlock_irqrestore(&mpath_dev->mpath_requeue_lock, flags);

	scmd->result = 0;

	blk_mq_end_request(req, 0);

	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_req);

static inline bool scsi_mpath_is_optimized(struct scsi_device *sdev)
{
	return (!scsi_device_online(sdev) &&
	    ((sdev->mpath_state == SCSI_MPATH_OPTIMAL) ||
	     (sdev->mpath_state == SCSI_MPATH_ACTIVE)));
}

static struct scsi_device *scsi_next_mpath_sdev(struct scsi_mpath_device *mpath_dev,
			struct scsi_device *sdev)
{
	sdev = list_next_or_null_rcu(&mpath_dev->mpath_sdev_list, &sdev->siblings,
	    struct scsi_device, siblings);

	if (sdev)
		return sdev;

	return list_first_or_null_rcu(&mpath_dev->mpath_sdev_list, struct scsi_device,
	    siblings);
}

static struct scsi_device *scsi_mpath_round_robin_path(struct scsi_mpath_device *mpath_dev,
	int node, struct scsi_device *old_sdev)
{
	struct scsi_device *sdev, *found = NULL;

	if (list_is_singular(&mpath_dev->mpath_sdev_list)) {
		if(scsi_mpath_is_disabled(old_sdev))
			return NULL;
		return old_sdev;
	}

	for (sdev = scsi_next_mpath_sdev(mpath_dev, old_sdev);
	    sdev && sdev != old_sdev;
	    sdev = scsi_next_mpath_sdev(mpath_dev, sdev)) {
		if (scsi_mpath_is_disabled(sdev))
			continue;
		if (sdev->mpath_state == SCSI_MPATH_OPTIMAL) {
			found = sdev;
			goto out;
		}
		if (sdev->mpath_state == SCSI_MPATH_ACTIVE)
			found = sdev;
	}

	if (!scsi_mpath_is_disabled(old_sdev) &&
	    (old_sdev->mpath_state == SCSI_MPATH_OPTIMAL ||
	    (!found && old_sdev->mpath_state == SCSI_MPATH_ACTIVE)))
		return old_sdev;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_dev->current_path[node], found);

	return found;
}

/*
 * Search path based on iopolicy and numa node affinity
 * and return the scsi_device for that path
 */
inline struct scsi_device *__scsi_find_path(struct scsi_mpath_device *mpath_dev, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;

	list_for_each_entry_rcu(sdev, &mpath_dev->mpath_sdev_list, mpath_entry) {
		if (scsi_mpath_is_disabled(sdev))
			continue;

		if (sdev->mpath_numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(sdev->mpath_iopolicy) == SCSI_MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, sdev->mpath_numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(sdev->mpath_state) {
		case SCSI_MPATH_OPTIMAL:
		    if (distance < found_distance) {
			    found_distance = distance;
			    sdev_found = sdev;
		    }
		    break;
		case SCSI_MPATH_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    sdev_fallback = sdev;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!sdev_found)
		sdev_found = sdev_fallback;

	if (sdev_found)
		rcu_assign_pointer(mpath_dev->current_path[node], sdev_found);

	return sdev_found;
}

inline struct scsi_device *scsi_find_path(struct scsi_mpath_device *mpath_dev)
{
	int node = numa_node_id();
	struct scsi_device *sdev;

	sdev = NULL;
	srcu_dereference(mpath_dev->current_path[node],
	    &mpath_dev->srcu);

	if (unlikely(!sdev))
		sdev = __scsi_find_path(mpath_dev, node);

	if (READ_ONCE(sdev->mpath_iopolicy) == SCSI_MPATH_IOPOLICY_RR)
		return scsi_mpath_round_robin_path(mpath_dev, node, sdev);

	if (unlikely(!scsi_mpath_is_optimized(sdev)))
		return __scsi_find_path(mpath_dev, node);

	return sdev;
}

void scsi_mpath_requeue_work(struct work_struct *work)
{
	struct scsi_mpath_device *mpath_dev =
	    container_of(work, struct scsi_mpath_device, mpath_requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&mpath_dev->mpath_requeue_lock);
	next = bio_list_get(&mpath_dev->mpath_requeue_list);
	spin_unlock(&mpath_dev->mpath_requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}

void scsi_mpath_set_live(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;
//	int ret;

	pr_err("%s sdev=%pS mpath_dev=%pS\n", __func__, sdev, mpath_dev);
	if (!sdev->mpath_dev)
		return;

	#if 0
	if (!test_and_set_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags)) {
		struct scsi_mp_disk *scsi_mp_disk = sdev->scsi_mp_disk;
		pr_err("%s calling device_add_disk\n", __func__);
		ret = device_add_disk(&scsi_mp_disk->dev, sdev->mpath_dev, NULL);
		pr_err("%s1 called device_add_disk ret=%d\n", __func__, ret);
		if (ret) {
			clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags);
			return;
		}
		pr_err("%s2 calling kblockd_schedule_work partition_scan_work\n", __func__);
		kblockd_schedule_work(&scsi_mp_disk->partition_scan_work);
	}

	pr_info("Attached SCSI %s disk\n", sdev->mpath_dev->disk_name);

	mutex_lock(&mpath_dev->mpath_lock);
	if (scsi_mpath_is_optimized(sdev)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_dev->srcu);
		for_each_online_node(node)
			__scsi_find_path(mpath_dev, node);
		srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_dev->mpath_lock);

	synchronize_srcu(&mpath_dev->srcu);
	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
	#endif
}

/**
 * Callback function for activating multipath devices
 */
static __maybe_unused void activate_mpath(void *data, int err)
{
	struct scsi_device *sdev = data;
	struct scsi_mpath_dh_data *mpath_h = sdev->mpath_pg_data;
	bool retry = false;

	if (!mpath_h)
		return;

	switch (err) {
	case SCSI_DH_OK:
		break;
	case SCSI_DH_NOSYS:
		sdev_printk(KERN_ERR, sdev,
			"Could not failover the device scsi_dh_%s, Error %d\n",
			sdev->handler->name, err);
		scsi_mpath_clear_current_path(sdev);
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
			scsi_mpath_clear_current_path(sdev);
		err = 0;
		break;
	case SCSI_DH_DEV_OFFLINED:
	default:
		sdev_printk(KERN_ERR, sdev, "Device Handler Path offlined \n");
		scsi_mpath_clear_current_path(sdev);
		break;
	}

	if (retry)
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &sdev->mpath_flags);

        if (scsi_mpath_state_is_live(sdev->mpath_state)) {
			pr_err("%s calling scsi_mpath_set_live\n", __func__);
			scsi_mpath_set_live(sdev);
        }
}

void scsi_activate_path(struct scsi_device *sdev)
{
	struct scsi_mpath_dh_data *mpath_dh = sdev->mpath_pg_data;

	pr_err("%s mpath_dh=%pS sdev=%pS q\n",
		__func__, mpath_dh, mpath_dh);
	#if 0
	if (!mpath_dh)
		return;

        if (!(scsi_mpath_state_is_live(sdev->mpath_state))) {
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
        struct scsi_device *sdev = container_of(work,
            struct scsi_device, activate_mpath);

	if (!sdev)
		return;

	scsi_activate_path(sdev);
}

int scsi_mpath_add_disk(struct scsi_device *sdev)
{
	if (!sdev->mpath_pg_data) {
		/* Re initialize ALUA */
		sdev->handler->rescan(sdev);
	} else {
		sdev->mpath_state = SCSI_MPATH_OPTIMAL;
		pr_err("%s calling scsi_mpath_set_live\n", __func__);
		scsi_mpath_set_live(sdev);
	}

	return (test_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags));
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_disk);

int scsi_multipath_init(struct scsi_device *sdev)
{
	#if 0
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_dh_data *h;
	struct scsi_mpath_device *mpath_dev;
	int ret = -ENOMEM;

	mpath_dev = kzalloc(sizeof(struct scsi_mpath_device), GFP_KERNEL);
	if (!mpath_dev)
		return ret;

	h = kzalloc(sizeof(struct scsi_mpath_dh_data), GFP_KERNEL);
	if (!h)
		goto out_mpath_dev;

	sdev->mpath_pg_data = h;

	ret = init_srcu_struct(&mpath_dev->srcu);
	if (ret) {
		cleanup_srcu_struct(&mpath_dev->srcu);
		goto out_handler;
	}

	pr_err("%s sdev=%pS mpath_dev=%pS h=%pS shost=%pS\n",
		__func__, sdev, mpath_dev, h, shost);

	mutex_init(&mpath_dev->mpath_lock);
	bio_list_init(&mpath_dev->mpath_requeue_list);
	spin_lock_init(&mpath_dev->mpath_requeue_lock);
	INIT_WORK(&mpath_dev->mpath_requeue_work, scsi_mpath_requeue_work);
	INIT_LIST_HEAD(&mpath_dev->mpath_list);
	INIT_WORK(&sdev->activate_mpath, scsi_activate_mpath_work);
	INIT_LIST_HEAD(&sdev->mpath_entry);
	sdev->mpath_numa_node = NUMA_NO_NODE;
	sdev->is_shared = 1;

	return 0;

out_handler:
	kfree(h);
out_mpath_dev:
	if (mpath_dev)
		kfree(mpath_dev);

	return ret;
	#else
	return 0;
	#endif
}
EXPORT_SYMBOL_GPL(scsi_multipath_init);

static bool scsi_available_mpath(struct scsi_mpath_device *mpath_dev)
{
	struct scsi_device *sdev;

	list_for_each_entry_rcu(sdev, &mpath_dev->mpath_sdev_list, mpath_entry) {
		if (scsi_device_online(sdev))
			return true;
	}
	return false;
}

/*  called when shost is being freed */
void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;

	if (!mpath_dev)
		return;

	cancel_work_sync(&mpath_dev->mpath_requeue_work);
	cleanup_srcu_struct(&mpath_dev->srcu);

	if (sdev->mpath_pg_data)
                kfree(sdev->mpath_pg_data);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_release);

void scsi_put_mpath_sdev(struct scsi_device *sdev)
{
	scsi_device_put(sdev);
}

void scsi_mpath_revalidate_path(struct gendisk *disk, sector_t capacity)
{
	struct scsi_mpath_device *mpath_dev = disk->private_data;
	__maybe_unused struct scsi_device *sdev;
	int srcu_idx;
	int node;

	if (!mpath_dev)
		return;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	#if 0
	list_for_each_entry_rcu(sdev, &mpath_dev->mpath_sdev_list, mpath_sdev_entry) {
		if (capacity != get_capacity(sdev->mpath_dev->gd))
			clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags);
	}
	#endif
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);

	for_each_node(node)
		rcu_assign_pointer(mpath_dev->current_path[node], NULL);
	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_revalidate_path);

static int scsi_mpath_open(struct gendisk *disk, blk_mode_t mode)
{
	pr_err("%s disk=%pS\n", __func__, disk);
	if (!scsi_get_device(disk->private_data)) {
		pr_err("%s1 disk=%pS ENXIO\n", __func__, disk);
		return -ENXIO;
	}

	return 0;
}

static void scsi_mpath_release(struct gendisk *disk)
{
	struct scsi_mpath_device *mpath_dev = disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	sdev = scsi_find_path(mpath_dev);
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
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
	struct scsi_mpath_device *mpath_dev = bio->bi_bdev->bd_disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx;
	bool special = false;

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
		pr_err("%s bio=%pS bi size=%d mpath_dev=%pS bio->bi_bdev=%pS\n", __func__, bio, bio->bi_iter.bi_size, mpath_dev, bio->bi_bdev);
	bio = bio_split_to_limits(bio);
	if (special)
		pr_err("%s1 bio=%pS mpath_dev=%pS called bio_split_to_limits\n", __func__, bio, mpath_dev);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	sdev = scsi_find_path(mpath_dev);
	if (special) {
		pr_err("%s2 bio=%pS bio->bi_bdev=%pS bio->bi_bdev->bd_disk=%pS bio->bi_bdev->bd_disk->part0=%pS mpath_dev=%pS\n",
			__func__, bio, bio->bi_bdev, bio->bi_bdev->bd_disk, bio->bi_bdev->bd_disk->part0, mpath_dev);
		pr_err("%s2.1 bio=%pS sdev=%pS request_queue=%pS request_queue->disk=%pS request_queue->disk->part0=%pS\n",
			__func__, bio, sdev, sdev->request_queue, sdev->request_queue->disk, sdev->request_queue->disk->part0);
	}
	if (likely(sdev)) {
		bio_set_dev(bio, sdev->request_queue->disk->part0);
		bio->bi_opf |= REQ_SCSI_MPATH;
		if (special)
			pr_err("%s3 bio=%pS bio->bi_bdev=%pS called bio_set_dev mpath_dev=%pS sdev=%pS calling submit_bio_noacct\n", __func__, bio, bio->bi_bdev, mpath_dev, sdev);
		submit_bio_noacct(bio);
		if (special)
			pr_err("%s4 bio=%pS mpath_dev=%pS sdev=%pS called submit_bio_noacct\n", __func__, bio, mpath_dev, sdev);
	//	BUG();
	} else if (scsi_available_mpath(mpath_dev)) {
		sdev_printk(KERN_NOTICE, NULL,
		    "No Usable Path - Requeing I/O \n");

		spin_lock_irq(&mpath_dev->mpath_requeue_lock);
		bio_list_add(&mpath_dev->mpath_requeue_list, bio);
		spin_unlock_irq(&mpath_dev->mpath_requeue_lock);
	} else {
		sdev_printk(KERN_NOTICE, NULL,
		    "No available path = Failing I/O \n");

		bio_io_error(bio);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
}

static int scsi_mpath_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct scsi_mpath_device *mpath_dev = disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	sdev = scsi_find_path(mpath_dev);
	if (sdev)
		ret = scsi_mpath_unique_id(sdev, id, type);
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);

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
	struct scsi_mpath_dh_data *dh_data = sdev->mpath_pg_data;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	if (type != BLK_UID_NAA)
		return -EINVAL;

	if (strncmp(dh_data->device_id_str, id, 16) == 0)
		return dh_data->device_id_len;

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(scsi_mpath_unique_id);

int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	struct scsi_mpath_dh_data *dh_data;
	char device_id_str[40];
	int ret = -EINVAL;

	pr_err("%s sdev=%pS\n", __func__, sdev);
	dh_data = sdev->mpath_pg_data;
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

	return -EINVAL;
}

static void scsi_mp_disk_release(struct device *dev)
{
	pr_err("%s dev=%pS\n", __func__, dev);
}

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_alloc_disk(struct scsi_device *sdev)
{
	struct queue_limits lim;
	int ret;
	static int disk_count;
	struct scsi_mpath_device *mpath_dev;

	pr_err("%s dev=%pS\n", __func__, sdev);
	/*
	 * Don't allocate mpath disk if ALUA handler is not attached
	 */
	if (!sdev->handler || strncmp(sdev->handler->name, "alua", 4) != 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "No Handler or correct handler attached for multipath\n");
		return 0;
	}

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
		sdev_printk(KERN_NOTICE, sdev,
		    "existing sdev with path, return\n");
		return 0;
	}

	mpath_dev = kzalloc(sizeof(*mpath_dev), GFP_KERNEL);
	if (!mpath_dev)
		return -ENOMEM;

	INIT_LIST_HEAD(&mpath_dev->entry);
	INIT_LIST_HEAD(&mpath_dev->mpath_sdev_list);
	INIT_WORK(&mpath_dev->partition_scan_work, scsi_multipath_partition_scan_work);

	mpath_dev->dev.class = &scsi_mp_disk_class;
	mpath_dev->dev.release = scsi_mp_disk_release;
	mpath_dev->dev.groups = scsi_mp_disk_attrs_groups;
	dev_set_name(&mpath_dev->dev, "scsi_mp_disk%d", disk_count);
	disk_count++;
	device_initialize(&mpath_dev->dev);

	blk_set_stacking_limits(&lim);

	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL;
	lim.max_zone_append_sectors = 0;
	lim.dma_alignment = 3;

	mpath_dev->gd = blk_alloc_disk(&lim, sdev->mpath_numa_node);
	pr_err("%s2 dev=%pS sdev->mpath_dev=%pS\n", __func__, sdev, sdev->mpath_dev);
	if (IS_ERR(sdev->mpath_dev))
		return PTR_ERR(sdev->mpath_dev);

	mpath_dev->gd->private_data = mpath_dev;
	mpath_dev->gd->fops = &scsi_mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &sdev->mpath_dev->gd->state);
	ret = device_add(&mpath_dev->dev); // see nvme_init_subsystem()
	pr_err("%s3 called device_add ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	sdev->mpath_dev = mpath_dev;
	list_add_tail(&sdev->mpath_entry, &mpath_dev->mpath_sdev_list);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_alloc_disk);

void scsi_mpath_start_request(struct request *req)
{
	__maybe_unused struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	__maybe_unused struct scsi_device *sdev = cmd->device;
	__maybe_unused struct scsi_mpath_device *mpath_dev = sdev->mpath_dev;

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
	struct scsi_mpath_device *mpath_dev = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		if (sdev->is_shared)
			continue;

		kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
		if (sdev->sdev_state == SDEV_RUNNING)
			disk_uevent(sdev->mpath_dev, KOBJ_CHANGE);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
}
#endif

void scsi_mpath_shutdown_disk(struct scsi_device *sdev)
{
	if (!sdev->mpath_dev)
		return;

	if (test_and_clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags)) {
		synchronize_srcu(&sdev->mpath_dev->srcu);
		kblockd_schedule_work(&sdev->mpath_dev->mpath_requeue_work);
	//	del_gendisk(sdev->mpath_dev);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_shutdown_disk);

void scsi_mpath_remove_disk(struct scsi_device *sdev)
{
	if (!sdev->mpath_dev)
		return;

	if (!sdev->is_shared)
		return;

	/* Make sure All pending bio's are cleaned up */
	kblockd_schedule_work(&sdev->mpath_dev->mpath_requeue_work);
	flush_work(&sdev->mpath_dev->mpath_requeue_work);
	//put_disk(sdev->mpath_dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_disk);

int scsi_mpath_update_state(struct scsi_device *sdev)
{
        struct scsi_mpath_dh_data *mpath_h;

        mpath_h = sdev->mpath_pg_data;
        if (!mpath_h)
		return SCSI_MPATH_UNAVAILABLE;

	switch(mpath_h->state) {
		case SCSI_ACCESS_STATE_OPTIMAL:
			sdev->mpath_state = SCSI_MPATH_OPTIMAL;
			break;
		case SCSI_ACCESS_STATE_ACTIVE:
			sdev->mpath_state = SCSI_MPATH_ACTIVE;
			break;
		case SCSI_ACCESS_STATE_STANDBY:
			sdev->mpath_state = SCSI_MPATH_STANDBY;
			break;
		case SCSI_ACCESS_STATE_UNAVAILABLE:
			sdev->mpath_state = SCSI_MPATH_UNAVAILABLE;
			break;
		case SCSI_ACCESS_STATE_TRANSITIONING:
			sdev->mpath_state = SCSI_MPATH_TRANSITIONING;
			break;
		case SCSI_ACCESS_STATE_OFFLINE:
		default:
                    sdev->mpath_state = SCSI_MPATH_OFFLINE;
		    break;
	}

	return sdev->mpath_state;
}

static int __init init_scsi_mp(void)
{
	return class_register(&scsi_mp_disk_class);
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