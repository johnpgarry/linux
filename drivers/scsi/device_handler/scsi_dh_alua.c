// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic SCSI-3 ALUA SCSI Device Handler
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_dh.h>
#include "../scsi_alua.h"

#define ALUA_DH_NAME "alua"
#define ALUA_DH_VER "2.0"

static struct workqueue_struct *kaluad_wq;

struct alua_queue_data {
	struct list_head	entry;
	activate_complete	callback_fn;
	void			*callback_data;
};

#define ALUA_POLICY_SWITCH_CURRENT	0
#define ALUA_POLICY_SWITCH_ALL		1

__maybe_unused
static void alua_rtpg_work(struct work_struct *work);
static bool alua_rtpg_queue(struct alua_port_group *pg,
			    struct scsi_device *sdev,
			    struct alua_queue_data *qdata, bool force);
static void alua_check(struct scsi_device *sdev, bool force);

/*
 * alua_check_vpd - Evaluate INQUIRY vpd page 0x83
 * @sdev: device to be checked
 *
 * Extract the relative target port and the target port group
 * descriptor from the list of identificators.
 */
static int alua_check_vpd(struct scsi_device *sdev, int tpgs)
{
	int rel_port = -1, group_id;
	struct alua_port_group *pg, *old_pg = NULL;
	bool pg_updated = false;
	unsigned long flags;

	sdev_printk(KERN_ERR, sdev, "%s sdev=%pS alua=%pS\n", __func__, sdev, sdev->alua);
	group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	sdev_printk(KERN_ERR, sdev, "%s1 sdev=%pS group_id=%d\n", __func__, sdev, group_id);
	if (group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    ALUA_DH_NAME);
		return SCSI_DH_DEV_UNSUPP;
	}

	pg = alua_alloc_pg(sdev, group_id, tpgs);
	sdev_printk(KERN_ERR, sdev, "%s2 sdev=%pS group_id=%d pg=%pS\n",
		__func__, sdev, group_id, pg);
	if (IS_ERR(pg)) {
		if (PTR_ERR(pg) == -ENOMEM)
			return SCSI_DH_NOMEM;
		return SCSI_DH_DEV_UNSUPP;
	}
	INIT_DELAYED_WORK(&pg->rtpg_work, alua_rtpg_work);
	if (pg->device_id_len)
		sdev_printk(KERN_INFO, sdev,
			    "%s: device %s port group %x rel port %x\n",
			    ALUA_DH_NAME, pg->device_id_str,
			    group_id, rel_port);
	else
		sdev_printk(KERN_INFO, sdev,
			    "%s: port group %x rel port %x\n",
			    ALUA_DH_NAME, group_id, rel_port);

	kref_get(&pg->kref);

	/* Check for existing port group references */
	spin_lock(&sdev->alua->pg_lock);
	old_pg = rcu_dereference_protected(sdev->alua->pg, lockdep_is_held(&sdev->alua->pg_lock));
	if (old_pg != pg) {
		/* port group has changed. Update to new port group */
		if (sdev->alua->pg) {
			spin_lock_irqsave(&old_pg->lock, flags);
			list_del_rcu(&sdev->alua->node);
			spin_unlock_irqrestore(&old_pg->lock, flags);
		}
		rcu_assign_pointer(sdev->alua->pg, pg);
		pg_updated = true;
	}

	spin_lock_irqsave(&pg->lock, flags);
	if (pg_updated)
		list_add_rcu(&sdev->alua->node, &pg->list);
	spin_unlock_irqrestore(&pg->lock, flags);

	spin_unlock(&sdev->alua->pg_lock);

	sdev_printk(KERN_ERR, sdev, "%s3 sdev=%pS alua=%pS group_id=%d pg=%pS calling alua_rtpg_queue\n",
			__func__, sdev, sdev->alua, group_id, pg);
	alua_rtpg_queue(pg, sdev, NULL, true);
	alua_put_pg(pg);

	if (old_pg)
		alua_put_pg(old_pg);

	return SCSI_DH_OK;
}


static void alua_handle_state_transition(struct scsi_device *sdev)
{
	struct alua_port_group *pg;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	rcu_read_lock();
	pg = rcu_dereference(sdev->alua->pg);
	if (pg)
		pg->state = SCSI_ACCESS_STATE_TRANSITIONING;
	rcu_read_unlock();
	alua_check(sdev, false);
}

static enum scsi_disposition alua_check_sense(struct scsi_device *sdev,
					      struct scsi_sense_hdr *sense_hdr)
{
	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	switch (sense_hdr->sense_key) {
	case NOT_READY:
		if (sense_hdr->asc == 0x04 && sense_hdr->ascq == 0x0a) {
			/*
			 * LUN Not Accessible - ALUA state transition
			 */
			alua_handle_state_transition(sdev);
			return NEEDS_RETRY;
		}
		break;
	case UNIT_ATTENTION:
		if (sense_hdr->asc == 0x04 && sense_hdr->ascq == 0x0a) {
			/*
			 * LUN Not Accessible - ALUA state transition
			 */
			alua_handle_state_transition(sdev);
			return NEEDS_RETRY;
		}
		if (sense_hdr->asc == 0x29 && sense_hdr->ascq == 0x00) {
			/*
			 * Power On, Reset, or Bus Device Reset.
			 * Might have obscured a state transition,
			 * so schedule a recheck.
			 */
			alua_check(sdev, true);
			return ADD_TO_MLQUEUE;
		}
		if (sense_hdr->asc == 0x29 && sense_hdr->ascq == 0x04)
			/*
			 * Device internal reset
			 */
			return ADD_TO_MLQUEUE;
		if (sense_hdr->asc == 0x2a && sense_hdr->ascq == 0x01)
			/*
			 * Mode Parameters Changed
			 */
			return ADD_TO_MLQUEUE;
		if (sense_hdr->asc == 0x2a && sense_hdr->ascq == 0x06) {
			/*
			 * ALUA state changed
			 */
			alua_check(sdev, true);
			return ADD_TO_MLQUEUE;
		}
		if (sense_hdr->asc == 0x2a && sense_hdr->ascq == 0x07) {
			/*
			 * Implicit ALUA state transition failed
			 */
			alua_check(sdev, true);
			return ADD_TO_MLQUEUE;
		}
		if (sense_hdr->asc == 0x3f && sense_hdr->ascq == 0x03)
			/*
			 * Inquiry data has changed
			 */
			return ADD_TO_MLQUEUE;
		if (sense_hdr->asc == 0x3f && sense_hdr->ascq == 0x0e)
			/*
			 * REPORTED_LUNS_DATA_HAS_CHANGED is reported
			 * when switching controllers on targets like
			 * Intel Multi-Flex. We can just retry.
			 */
			return ADD_TO_MLQUEUE;
		break;
	}

	return SCSI_RETURN_NOT_HANDLED;
}

/*
 * alua_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Issue a SET TARGET PORT GROUP command and evaluate the
 * response. Returns SCSI_DH_RETRY per default to trigger
 * a re-evaluation of the target group state or SCSI_DH_OK
 * if no further action needs to be taken.
 */
static unsigned alua_stpg(struct scsi_device *sdev, struct alua_port_group *pg)
{
	int retval;
	struct scsi_sense_hdr sense_hdr;

	sdev_printk(KERN_ERR, sdev, "%s pg=%pS\n", __func__, pg);
	if (!(pg->tpgs & TPGS_MODE_EXPLICIT)) {
		/* Only implicit ALUA supported, retry */
		return SCSI_DH_RETRY;
	}
	sdev_printk(KERN_ERR, sdev, "%s0 pg=%pS pg->state=%d\n", __func__, pg, pg->state);
	switch (pg->state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return SCSI_DH_OK;
	case SCSI_ACCESS_STATE_ACTIVE:
		if ((pg->flags & ALUA_OPTIMIZE_STPG) &&
		    !pg->pref &&
		    (pg->tpgs & TPGS_MODE_IMPLICIT))
			return SCSI_DH_OK;
		break;
	case SCSI_ACCESS_STATE_STANDBY:
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		return SCSI_DH_IO;
	case SCSI_ACCESS_STATE_TRANSITIONING:
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: stpg failed, unhandled TPGS state %d",
			    ALUA_DH_NAME, pg->state);
		return SCSI_DH_NOSYS;
	}
	sdev_printk(KERN_ERR, sdev, "%s1 calling submit_stpg pg=%pS pg->group_id=%d\n", __func__, pg, pg->group_id);
	retval = submit_stpg(sdev, pg->group_id, &sense_hdr);
	sdev_printk(KERN_ERR, sdev, "%s1.1 called submit_stpg pg=%pS\n", __func__, pg);

	if (retval) {
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: stpg failed, result %d",
				    ALUA_DH_NAME, retval);
			if (retval < 0)
				return SCSI_DH_DEV_TEMP_BUSY;
		} else {
			sdev_printk(KERN_INFO, sdev, "%s: stpg failed\n",
				    ALUA_DH_NAME);
			scsi_print_sense_hdr(sdev, ALUA_DH_NAME, &sense_hdr);
		}
	}
	/* Retry RTPG */
	return SCSI_DH_RETRY;
}

/*
 * The caller must call scsi_device_put() on the returned pointer if it is not
 * NULL.
 */
static struct scsi_device * __must_check
alua_rtpg_select_sdev(struct alua_port_group *pg)
{
	struct alua_data *alua;
	struct scsi_device *sdev = NULL, *prev_sdev;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	lockdep_assert_held(&pg->lock);
	if (WARN_ON(!pg->rtpg_sdev))
		return NULL;

	/*
	 * RCU protection isn't necessary for dh_list here
	 * as we hold pg->lock, but for access to sdev->alua->pg.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(alua, &pg->list, node) {
		if (!alua->sdev)
			continue;
		if (alua->sdev == pg->rtpg_sdev) {
			alua->disabled = true;
			continue;
		}
		if (rcu_dereference(sdev->alua->pg) == pg &&
		    !sdev->alua->disabled &&
		    !scsi_device_get(sdev->alua->sdev)) {
			sdev = alua->sdev;
			break;
		}
	}
	rcu_read_unlock();

	if (!sdev) {
		pr_warn("%s: no device found for rtpg\n",
			(pg->device_id_len ?
			 (char *)pg->device_id_str : "(nameless PG)"));
		return NULL;
	}

	sdev_printk(KERN_INFO, sdev, "rtpg retry on different device\n");

	prev_sdev = pg->rtpg_sdev;
	pg->rtpg_sdev = sdev;

	return prev_sdev;
}

static void alua_rtpg_work(struct work_struct *work)
{
	struct alua_port_group *pg =
		container_of(work, struct alua_port_group, rtpg_work.work);
	struct scsi_device *sdev, *prev_sdev = NULL;
	LIST_HEAD(qdata_list);
	int err = SCSI_DH_OK;
	struct alua_queue_data *qdata, *tmp;
	struct alua_data *alua;
	unsigned long flags;
	pr_err("%s pg=%pS\n", __func__, pg);

	spin_lock_irqsave(&pg->lock, flags);
	sdev = pg->rtpg_sdev;
	sdev_printk(KERN_ERR, sdev, "%s pg=%pS state=%d sdev=%pS OPTIMAL=%d\n",
			__func__, pg, pg->state, sdev, SCSI_ACCESS_STATE_OPTIMAL);
	if (!sdev) {
		WARN_ON(pg->flags & ALUA_PG_RUN_RTPG);
		WARN_ON(pg->flags & ALUA_PG_RUN_STPG);
		spin_unlock_irqrestore(&pg->lock, flags);
		alua_put_pg(pg);
		return;
	}
	pg->flags |= ALUA_PG_RUNNING;
	if (pg->flags & ALUA_PG_RUN_RTPG) {
		int state = pg->state;

		pg->flags &= ~ALUA_PG_RUN_RTPG;
		spin_unlock_irqrestore(&pg->lock, flags);
		if (state == SCSI_ACCESS_STATE_TRANSITIONING) {
			if (alua_tur(sdev) == SCSI_DH_RETRY) {
				spin_lock_irqsave(&pg->lock, flags);
				pg->flags &= ~ALUA_PG_RUNNING;
				pg->flags |= ALUA_PG_RUN_RTPG;
				if (!pg->interval)
					pg->interval = ALUA_RTPG_RETRY_DELAY;
				spin_unlock_irqrestore(&pg->lock, flags);
				queue_delayed_work(kaluad_wq, &pg->rtpg_work,
						   pg->interval * HZ);
				return;
			}
			/* Send RTPG on failure or if TUR indicates SUCCESS */
		}
		sdev_printk(KERN_ERR, sdev, "%s1 pg=%pS sdev=%pS calling alua_rtpg\n", __func__, pg, sdev);
		err = alua_rtpg(sdev, pg);
		sdev_printk(KERN_ERR, sdev, "%s1.1 pg=%pS sdev=%pS called alua_rtpg err=%d\n", __func__, pg, sdev, err);
		spin_lock_irqsave(&pg->lock, flags);

		/* If RTPG failed on the current device, try using another */
		if (err == SCSI_DH_RES_TEMP_UNAVAIL &&
		    (prev_sdev = alua_rtpg_select_sdev(pg)))
			err = SCSI_DH_IMM_RETRY;

		if (err == SCSI_DH_RETRY || err == SCSI_DH_IMM_RETRY ||
		    pg->flags & ALUA_PG_RUN_RTPG) {
			pg->flags &= ~ALUA_PG_RUNNING;
			if (err == SCSI_DH_IMM_RETRY)
				pg->interval = 0;
			else if (!pg->interval && !(pg->flags & ALUA_PG_RUN_RTPG))
				pg->interval = ALUA_RTPG_RETRY_DELAY;
			pg->flags |= ALUA_PG_RUN_RTPG;
			spin_unlock_irqrestore(&pg->lock, flags);
			goto queue_rtpg;
		}
		if (err != SCSI_DH_OK)
			pg->flags &= ~ALUA_PG_RUN_STPG;
	}
	if (pg->flags & ALUA_PG_RUN_STPG) {
		pg->flags &= ~ALUA_PG_RUN_STPG;
		spin_unlock_irqrestore(&pg->lock, flags);
		sdev_printk(KERN_ERR, sdev, "%s2 pg=%pS sdev=%pS calling alua_stpg\n", __func__, pg, sdev);
		err = alua_stpg(sdev, pg);
		spin_lock_irqsave(&pg->lock, flags);
		if (err == SCSI_DH_RETRY || pg->flags & ALUA_PG_RUN_RTPG) {
			pg->flags |= ALUA_PG_RUN_RTPG;
			pg->interval = 0;
			pg->flags &= ~ALUA_PG_RUNNING;
			spin_unlock_irqrestore(&pg->lock, flags);
			goto queue_rtpg;
		}
	}

	list_splice_init(&pg->rtpg_list, &qdata_list);
	/*
	 * We went through an RTPG, for good or bad.
	 * Re-enable all devices for the next attempt.
	 */
	list_for_each_entry(alua, &pg->list, node)
		alua->disabled = false;
	pg->rtpg_sdev = NULL;
	spin_unlock_irqrestore(&pg->lock, flags);

	if (prev_sdev)
		scsi_device_put(prev_sdev);

	list_for_each_entry_safe(qdata, tmp, &qdata_list, entry) {
		sdev_printk(KERN_ERR, sdev, "%s4 looping qdata_list pg=%pS qdata=%pS callback_fn=%pS callback_data=%pS\n",
			__func__, pg, qdata, qdata->callback_fn, qdata->callback_data);
		list_del(&qdata->entry);
		if (qdata->callback_fn)
			qdata->callback_fn(qdata->callback_data, err);
		kfree(qdata);
	}
	spin_lock_irqsave(&pg->lock, flags);
	pg->flags &= ~ALUA_PG_RUNNING;
	spin_unlock_irqrestore(&pg->lock, flags);
	scsi_device_put(sdev);
	alua_put_pg(pg);
	return;

queue_rtpg:
	if (prev_sdev)
		scsi_device_put(prev_sdev);
	queue_delayed_work(kaluad_wq, &pg->rtpg_work, pg->interval * HZ);
}

/**
 * alua_rtpg_queue() - cause RTPG to be submitted asynchronously
 * @pg: ALUA port group associated with @sdev.
 * @sdev: SCSI device for which to submit an RTPG.
 * @qdata: Information about the callback to invoke after the RTPG.
 * @force: Whether or not to submit an RTPG if a work item that will submit an
 *         RTPG already has been scheduled.
 *
 * Returns true if and only if alua_rtpg_work() will be called asynchronously.
 * That function is responsible for calling @qdata->fn().
 *
 * Context: may be called from atomic context (alua_check()) only if the caller
 *	holds an sdev reference.
 */
static bool alua_rtpg_queue(struct alua_port_group *pg,
			    struct scsi_device *sdev,
			    struct alua_queue_data *qdata, bool force)
{
	int start_queue = 0;
	unsigned long flags;

	sdev_printk(KERN_ERR, sdev, "%s pg=%pS sdev=%pS\n", __func__, pg, sdev);
	if (WARN_ON_ONCE(!pg) || scsi_device_get(sdev))
		return false;

	spin_lock_irqsave(&pg->lock, flags);
	if (qdata) {
		list_add_tail(&qdata->entry, &pg->rtpg_list);
		pg->flags |= ALUA_PG_RUN_STPG;
		force = true;
	}
	sdev_printk(KERN_ERR, sdev, "%s2 pg=%pS sdev=%pS pg->rtpg_sdev=%pS\n",
		__func__, pg, sdev, pg->rtpg_sdev);
	if (pg->rtpg_sdev == NULL) {
		rcu_read_lock();
		if (rcu_dereference(sdev->alua->pg) == pg) {
			pg->interval = 0;
			pg->flags |= ALUA_PG_RUN_RTPG;
			kref_get(&pg->kref);
			pg->rtpg_sdev = sdev;
			start_queue = 1;
		}
		rcu_read_unlock();
	} else if (!(pg->flags & ALUA_PG_RUN_RTPG) && force) {
		pg->flags |= ALUA_PG_RUN_RTPG;
		/* Do not queue if the worker is already running */
		if (!(pg->flags & ALUA_PG_RUNNING)) {
			kref_get(&pg->kref);
			start_queue = 1;
		}
	}

	spin_unlock_irqrestore(&pg->lock, flags);

	sdev_printk(KERN_ERR, sdev, "%s3 pg=%pS sdev=%pS start_queue=%d\n",
		__func__, pg, sdev, start_queue);
	if (start_queue) {
		if (queue_delayed_work(kaluad_wq, &pg->rtpg_work,
				msecs_to_jiffies(ALUA_RTPG_DELAY_MSECS)))
			sdev = NULL;
		else
			alua_put_pg(pg);
	}
	if (sdev)
		scsi_device_put(sdev);

	return true;
}

/*
 * alua_initialize - Initialize ALUA state
 * @sdev: the device to be initialized
 *
 * For the prep_fn to work correctly we have
 * to initialize the ALUA state for the device.
 */
static int alua_initialize(struct scsi_device *sdev)
{
	int err = SCSI_DH_DEV_UNSUPP, tpgs;

	sdev_printk(KERN_ERR, sdev, "%s sdev=%pS sdev->alua=%pS calling alua_check_tpgs sdev->alua\n",
		__func__, sdev, sdev->alua);
	mutex_lock(&sdev->alua->init_mutex);
	sdev->alua->disabled = false;
	tpgs = alua_check_tpgs(sdev);
	sdev_printk(KERN_ERR, sdev, "%s1 sdev=%pS alua=%pS called alua_check_tpgs tpgs=%d\n",
			__func__, sdev, sdev->alua, tpgs);
	if (tpgs != TPGS_MODE_NONE) {
		sdev_printk(KERN_ERR, sdev, "%s2 sdev=%pS alua=%pS calling alua_check_vpd tpgs=%d\n",
			__func__, sdev, sdev->alua, tpgs);
		err = alua_check_vpd(sdev, tpgs);
		sdev_printk(KERN_ERR, sdev, "%s2.1 sdev=%pS alua=%pS called alua_check_vpd tpgs=%d err=%d\n",
			__func__, sdev, sdev->alua, tpgs, err);
	}
	sdev->alua->init_error = err;
	mutex_unlock(&sdev->alua->init_mutex);
	return err;
}
/*
 * alua_set_params - set/unset the optimize flag
 * @sdev: device on the path to be activated
 * params - parameters in the following format
 *      "no_of_params\0param1\0param2\0param3\0...\0"
 * For example, to set the flag pass the following parameters
 * from multipath.conf
 *     hardware_handler        "2 alua 1"
 */
static int alua_set_params(struct scsi_device *sdev, const char *params)
{
	struct alua_port_group *pg = NULL;
	unsigned int optimize = 0, argc;
	const char *p = params;
	int result = SCSI_DH_OK;
	unsigned long flags;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	if ((sscanf(params, "%u", &argc) != 1) || (argc != 1))
		return -EINVAL;

	while (*p++)
		;
	if ((sscanf(p, "%u", &optimize) != 1) || (optimize > 1))
		return -EINVAL;

	rcu_read_lock();
	pg = rcu_dereference(sdev->alua->pg);
	if (!pg) {
		rcu_read_unlock();
		return -ENXIO;
	}
	spin_lock_irqsave(&pg->lock, flags);
	if (optimize)
		pg->flags |= ALUA_OPTIMIZE_STPG;
	else
		pg->flags &= ~ALUA_OPTIMIZE_STPG;
	spin_unlock_irqrestore(&pg->lock, flags);
	rcu_read_unlock();

	return result;
}

/*
 * alua_activate - activate a path
 * @sdev: device on the path to be activated
 *
 * We're currently switching the port group to be activated only and
 * let the array figure out the rest.
 * There may be other arrays which require us to switch all port groups
 * based on a certain policy. But until we actually encounter them it
 * should be okay.
 */
static int alua_activate(struct scsi_device *sdev,
			activate_complete fn, void *data)
{
	int err = SCSI_DH_OK;
	struct alua_queue_data *qdata;
	struct alua_port_group *pg;

	//WARN_ON_ONCE(1);

	qdata = kzalloc(sizeof(*qdata), GFP_KERNEL);
	sdev_printk(KERN_ERR, sdev, "%s data=%pS\n", __func__, data);

	if (!qdata) {
		err = SCSI_DH_RES_TEMP_UNAVAIL;
		goto out;
	}
	qdata->callback_fn = fn;
	qdata->callback_data = data;

	mutex_lock(&sdev->alua->init_mutex);
	rcu_read_lock();
	pg = rcu_dereference(sdev->alua->pg);
	if (!pg || !kref_get_unless_zero(&pg->kref)) {
		rcu_read_unlock();
		kfree(qdata);
		err = sdev->alua->init_error;
		mutex_unlock(&sdev->alua->init_mutex);
		goto out;
	}
	rcu_read_unlock();
	mutex_unlock(&sdev->alua->init_mutex);

	sdev_printk(KERN_ERR, sdev, "%s2 data=%pS calling alua_rtpg_queue\n", __func__, data);
	if (alua_rtpg_queue(pg, sdev, qdata, true)) {
		fn = NULL;
	} else {
		kfree(qdata);
		err = SCSI_DH_DEV_OFFLINED;
	}
	alua_put_pg(pg);
out:
	if (fn)
		fn(data, err);
	return 0;
}

/*
 * alua_check - check path status
 * @sdev: device on the path to be checked
 *
 * Check the device status
 */
static void alua_check(struct scsi_device *sdev, bool force)
{
	struct alua_port_group *pg;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	rcu_read_lock();
	pg = rcu_dereference(sdev->alua->pg);
	if (!pg || !kref_get_unless_zero(&pg->kref)) {
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();
	alua_rtpg_queue(pg, sdev, NULL, force);
	alua_put_pg(pg);
}

/*
 * alua_prep_fn - request callback
 *
 * Fail I/O to all paths not in state
 * active/optimized or active/non-optimized.
 */
static blk_status_t alua_prep_fn(struct scsi_device *sdev, struct request *req)
{
	struct alua_port_group *pg;
	unsigned char state = SCSI_ACCESS_STATE_OPTIMAL;

	//pr_err("%s\n", __func__);
	rcu_read_lock();
	pg = rcu_dereference(sdev->alua->pg);
	if (pg)
		state = pg->state;
	rcu_read_unlock();

	switch (state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
	case SCSI_ACCESS_STATE_ACTIVE:
	case SCSI_ACCESS_STATE_LBA:
	case SCSI_ACCESS_STATE_TRANSITIONING:
		return BLK_STS_OK;
	default:
		req->rq_flags |= RQF_QUIET;
		return BLK_STS_IOERR;
	}
}

static void alua_rescan(struct scsi_device *sdev)
{
	alua_initialize(sdev);
}

/*
 * alua_bus_attach - Attach device handler
 * @sdev: device to be attached to
 */
static int alua_bus_attach(struct scsi_device *sdev)
{
	int err;

	sdev_printk(KERN_ERR, sdev, "%s sdev=%pS alua=%pS\n", __func__, sdev, sdev->alua);

	err = alua_initialize(sdev);
	if (err != SCSI_DH_OK && err != SCSI_DH_DEV_OFFLINED)
		goto failed;

	return SCSI_DH_OK;
failed:
	return err;
}

/*
 * alua_bus_detach - Detach device handler
 * @sdev: device to be detached from
 */
static void alua_bus_detach(struct scsi_device *sdev)
{
	struct alua_port_group *pg;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	spin_lock(&sdev->alua->pg_lock);
	pg = rcu_dereference_protected(sdev->alua->pg, lockdep_is_held(&sdev->alua->pg_lock));
	rcu_assign_pointer(sdev->alua->pg, NULL);
	spin_unlock(&sdev->alua->pg_lock);
	if (pg) {
		spin_lock_irq(&pg->lock);
		list_del_rcu(&sdev->alua->node);
		spin_unlock_irq(&pg->lock);
		alua_put_pg(pg);
	}
	synchronize_rcu();
}

static struct scsi_device_handler alua_dh = {
	.name = ALUA_DH_NAME,
	.module = THIS_MODULE,
	.attach = alua_bus_attach,
	.detach = alua_bus_detach,
	.prep_fn = alua_prep_fn,
	.check_sense = alua_check_sense,
	.activate = alua_activate,
	.rescan = alua_rescan,
	.set_params = alua_set_params,
};

static int __init alua_init(void)
{
	int r;

	kaluad_wq = alloc_workqueue("kaluad", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
	if (!kaluad_wq)
		return -ENOMEM;

	r = scsi_register_device_handler(&alua_dh);
	if (r != 0) {
		printk(KERN_ERR "%s: Failed to register scsi device handler",
			ALUA_DH_NAME);
		destroy_workqueue(kaluad_wq);
	}
	return r;
}

static void __exit alua_exit(void)
{
	scsi_unregister_device_handler(&alua_dh);
	destroy_workqueue(kaluad_wq);
}

module_init(alua_init);
module_exit(alua_exit);

MODULE_DESCRIPTION("DM Multipath ALUA support");
MODULE_AUTHOR("Hannes Reinecke <hare@suse.de>");
MODULE_LICENSE("GPL");
MODULE_VERSION(ALUA_DH_VER);
