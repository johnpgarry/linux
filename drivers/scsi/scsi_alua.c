// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic SCSI-3 ALUA SCSI driver
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_alua.h>

#define DRV_NAME "alua"

#define ALUA_FAILOVER_RETRIES		5
#define ALUA_RTPG_RETRY_DELAY		2

#define ALUA_RTPG_DELAY_MSECS		5





#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_alua.h>


#define ALUA_RTPG_DELAY_MSECS		5
#define ALUA_RTPG_RETRY_DELAY		2

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01
#define ALUA_RTPG_EXT_HDR_UNSUPP	0x02
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
#define ALUA_PG_RUNNING			0x40


static void alua_rtpg_work(struct work_struct *work);
static bool alua_rtpg_queue(struct scsi_device *sdev, bool force);
static void alua_check(struct scsi_device *sdev, bool force);
static int alua_rtpg_run(struct scsi_device *sdev);
/*
 * alua_check_vpd - Evaluate INQUIRY vpd page 0x83
 * @sdev: device to be checked
 *
 * Extract the relative target port and the target port group
 * descriptor from the list of identificators.
 */
__maybe_unused
static int alua_check_vpd(struct scsi_device *sdev, int tpgs)
{
	int rel_port = -1, group_id;

	group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	if (group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    DRV_NAME);
		return -ENODEV; //SCSI_DH_DEV_UNSUPP
	}

	alua_rtpg_queue(sdev, true);

	return 0;
}


static void alua_handle_state_transition(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;

	alua->state = SCSI_ACCESS_STATE_TRANSITIONING;
	alua_check(sdev, false);
}

__maybe_unused
static enum scsi_disposition alua_check_sense(struct scsi_device *sdev,
					      struct scsi_sense_hdr *sense_hdr)
{
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
 * alua_rtpg - Evaluate REPORT TARGET GROUP STATES
 * @sdev: the device to be evaluated.
 *
 * Evaluate the Target Port Group State.
 * Returns SCSI_DH_DEV_OFFLINED if the path is
 * found to be unusable.
 */
/* same as alua_rtpg_run */
#ifdef oldstuff
static int alua_rtpg(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;
	struct scsi_sense_hdr sense_hdr;
	int len, k, off, bufflen = ALUA_RTPG_SIZE;
	int group_id_old, state_old, pref_old, valid_states_old;
	unsigned char *desc, *buff;
	int err;
	int retval;
	unsigned int tpg_desc_tbl_off;
	unsigned char orig_transition_tmo;
	//unsigned long flags;
	bool transitioning_sense = false;

	group_id_old = alua->group_id;
	state_old = alua->state;
	pref_old = alua->pref;
	valid_states_old = alua->valid_states;

	if (!alua->expiry) {
		unsigned long transition_tmo = ALUA_FAILOVER_TIMEOUT * HZ;

		if (alua->transition_tmo)
			transition_tmo = alua->transition_tmo * HZ;

		alua->expiry = round_jiffies_up(jiffies + transition_tmo);
	}

	buff = kzalloc(bufflen, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

 retry:
	err = 0;
	retval = submit_rtpg(sdev, buff, bufflen, &sense_hdr);

	if (retval) {
		/*
		 * Some (broken) implementations have a habit of returning
		 * an error during things like firmware update etc.
		 * But if the target only supports active/optimized there's
		 * not much we can do; it's not that we can switch paths
		 * or anything.
		 * So ignore any errors to avoid spurious failures during
		 * path failover.
		 */
		if ((alua->valid_states & ~TPGS_SUPPORT_OPTIMIZED) == 0) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: ignoring rtpg result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			return 0;
		}
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: rtpg failed, result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			if (retval < 0)
				return -EBUSY;
			if (host_byte(retval) == DID_NO_CONNECT)
				return -EBADF;// SCSI_DH_RES_TEMP_UNAVAIL
			return -EIO;
		}

		/*
		 * submit_rtpg() has failed on existing arrays
		 * when requesting extended header info, and
		 * the array doesn't support extended headers,
		 * even though it shouldn't according to T10.
		 * The retry without rtpg_ext_hdr_req set
		 * handles this.
		 * Note:  some arrays return a sense key of ILLEGAL_REQUEST
		 * with ASC 00h if they don't support the extended header.
		 */
		if (alua->rtpg_ext_hdr_unsupp == false  &&
		    sense_hdr.sense_key == ILLEGAL_REQUEST) {
			alua->rtpg_ext_hdr_unsupp = true;
			goto retry;
		}
		/*
		 * If the array returns with 'ALUA state transition'
		 * sense code here it cannot return RTPG data during
		 * transition. So set the state to 'transitioning' directly.
		 */
		if (sense_hdr.sense_key == NOT_READY &&
		    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a) {
			transitioning_sense = true;
			goto skip_rtpg;
		}
		/*
		 * Retry on any other UNIT ATTENTION occurred.
		 */
		if (sense_hdr.sense_key == UNIT_ATTENTION)
			err = -EAGAIN;
		if (err == -EAGAIN &&
		    alua->expiry != 0 && time_before(jiffies, alua->expiry)) {
			sdev_printk(KERN_ERR, sdev, "%s: rtpg retry\n",
				    DRV_NAME);
			scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
			kfree(buff);
			return err;
		}
		sdev_printk(KERN_ERR, sdev, "%s: rtpg failed\n",
			    DRV_NAME);
		scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
		kfree(buff);
		alua->expiry = 0;
		return -EIO;
	}

	len = get_unaligned_be32(&buff[0]) + 4;

	if (len > bufflen) {
		/* Resubmit with the correct length */
		kfree(buff);
		bufflen = len;
		buff = kmalloc(bufflen, GFP_KERNEL);
		if (!buff) {
			sdev_printk(KERN_WARNING, sdev,
				    "%s: kmalloc buffer failed\n",__func__);
			/* Temporary failure, bypass */
			alua->expiry = 0;
			return -EBUSY;
		}
		goto retry;
	}

	orig_transition_tmo = alua->transition_tmo;
	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR && buff[5] != 0)
		alua->transition_tmo = buff[5];
	else
		alua->transition_tmo = ALUA_FAILOVER_TIMEOUT;

	if (orig_transition_tmo != alua->transition_tmo) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: transition timeout set to %d seconds\n",
			    DRV_NAME, alua->transition_tmo);
		alua->expiry = jiffies + alua->transition_tmo * HZ;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR)
		tpg_desc_tbl_off = 8;
	else
		tpg_desc_tbl_off = 4;

	for (k = tpg_desc_tbl_off, desc = buff + tpg_desc_tbl_off;
	     k < len;
	     k += off, desc += off) {
		u16 group_id = get_unaligned_be16(&desc[2]);

		#ifdef oldpg
		spin_lock_irqsave(&port_group_lock, flags);
		tmp_pg = alua_find_get_pg(alua->device_id_str, alua->device_id_len,
					  group_id);
		spin_unlock_irqrestore(&port_group_lock, flags);
		if (tmp_pg) {
			if (spin_trylock_irqsave(&tmp_alua->lock, flags)) {
				if ((tmp_pg == pg) ||
				    !(tmp_alua->flags & ALUA_PG_RUNNING)) {

					tmp_alua->state = desc[0] & 0x0f;
					tmp_alua->pref = desc[0] >> 7;
					rcu_read_lock();
					list_for_each_entry_rcu(h,
						&tmp_alua->dh_list, node) {
						if (!h->sdev)
							continue;
						h->sdev->access_state = desc[0];
					}
					rcu_read_unlock();
				}
				if (tmp_pg == pg)
					tmp_alua->valid_states = desc[1];
				spin_unlock_irqrestore(&tmp_alua->lock, flags);
			}
			kref_put(&tmp_alua->kref, release_port_group);
		}
		#else
		if (group_id == alua->group_id) {
			alua->state = desc[0] & 0x0f;
			alua->pref = desc[0] >> 7;
			alua->valid_states = desc[1];
			sdev->access_state = desc[0];
			break;
		}
		#endif
		off = 8 + (desc[7] * 4);
	}

 skip_rtpg:
	//spin_lock_irqsave(&alua->lock, flags);
	if (transitioning_sense)
		alua->state = SCSI_ACCESS_STATE_TRANSITIONING;

	if (group_id_old != alua->group_id || state_old != alua->state ||
		pref_old != alua->pref || valid_states_old != alua->valid_states) {
	//	alua_print_info(sdev, alua->group_id, alua->state, alua->pref,
	//					alua->valid_states);
	}

	switch (alua->state) {
	case SCSI_ACCESS_STATE_TRANSITIONING:
		if (time_before(jiffies, alua->expiry)) {
			/* State transition, retry */
			alua->interval = ALUA_RTPG_RETRY_DELAY;
			err = -EAGAIN;
		} else {
			/* Transitioning time exceeded, set port to standby */
			err = -EIO;
			alua->state = SCSI_ACCESS_STATE_STANDBY;
			alua->expiry = 0;
			sdev->access_state = (alua->state & SCSI_ACCESS_STATE_MASK);
			if (alua->pref)
				sdev->access_state |= SCSI_ACCESS_STATE_PREFERRED;
			rcu_read_unlock();
		}
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		/* Path unusable */
		err = -ENODEV;//SCSI_DH_DEV_OFFLINED;
		alua->expiry = 0;
		break;
	default:
		/* Useable path if active */
		err = 0;
		alua->expiry = 0;
		break;
	}
	//spin_unlock_irqrestore(&alua->lock, flags);
	kfree(buff);
	return err;
}
#endif

/*
 * alua_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Issue a SET TARGET PORT GROUP command and evaluate the
 * response. Returns -EAGAIN per default to trigger
 * a re-evaluation of the target group state or 0
 * if no further action needs to be taken.
 */
static int alua_stpg(struct scsi_device *sdev)
{
	int retval;
	struct scsi_sense_hdr sense_hdr;
	struct alua_data *alua = sdev->alua;

	if (!(alua->tpgs & TPGS_MODE_EXPLICIT)) {
		/* Only implicit ALUA supported, retry */
		return -EAGAIN;
	}
	switch (alua->state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return 0;
	case SCSI_ACCESS_STATE_ACTIVE:
		if ((alua->flags & ALUA_OPTIMIZE_STPG) &&
		    !alua->pref &&
		    (alua->tpgs & TPGS_MODE_IMPLICIT))
			return 0;
		break;
	case SCSI_ACCESS_STATE_STANDBY:
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		return -EIO;
	case SCSI_ACCESS_STATE_TRANSITIONING:
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: stpg failed, unhandled TPGS state %d",
			    DRV_NAME, alua->state);
		return -EINVAL;
	}
	retval = submit_stpg(sdev, alua->group_id, &sense_hdr);

	if (retval) {
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: stpg failed, result %d",
				    DRV_NAME, retval);
			if (retval < 0)
				return -EBUSY;
		} else {
			sdev_printk(KERN_INFO, sdev, "%s: stpg failed\n",
				    DRV_NAME);
			scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
		}
	}
	/* Retry RTPG */
	return -EAGAIN;
}

__maybe_unused
static void alua_rtpg_work(struct work_struct *work)
{
	struct alua_data *alua =
		container_of(work, struct alua_data, work.work);
	struct scsi_device *sdev, *prev_sdev = NULL;
	LIST_HEAD(qdata_list);
	int err = 0;
	//unsigned long flags;

	//spin_lock_irqsave(&alua->lock, flags);
	sdev = alua->sdev;
	if (!sdev) {
		WARN_ON(alua->flags & ALUA_PG_RUN_RTPG);
		WARN_ON(alua->flags & ALUA_PG_RUN_STPG);
		//spin_unlock_irqrestore(&alua->lock, flags);
		//kref_put(&alua->kref, release_port_group);
		return;
	}
	alua->flags |= ALUA_PG_RUNNING;
	if (alua->flags & ALUA_PG_RUN_RTPG) {
		int state = alua->state;

		alua->flags &= ~ALUA_PG_RUN_RTPG;
		//spin_unlock_irqrestore(&alua->lock, flags);
		if (state == SCSI_ACCESS_STATE_TRANSITIONING) {
			if (alua_tur(sdev) == -EAGAIN) {
				//spin_lock_irqsave(&alua->lock, flags);
				alua->flags &= ~ALUA_PG_RUNNING;
				alua->flags |= ALUA_PG_RUN_RTPG;
				if (!alua->interval)
					alua->interval = ALUA_RTPG_RETRY_DELAY;
				//spin_unlock_irqrestore(&alua->lock, flags);
				queue_delayed_work(system_wq, &alua->work,
						   alua->interval * HZ);
				return;
			}
			/* Send RTPG on failure or if TUR indicates SUCCESS */
		}
		#ifdef oldstuff
		err = alua_rtpg(sdev);
		#else
		err = alua_rtpg_run(sdev);
		#endif
		//spin_lock_irqsave(&alua->lock, flags);

		/* If RTPG failed on the current device, try using another */
		//if (err == SCSI_DH_RES_TEMP_UNAVAIL &&
		 //   (prev_sdev = alua_rtpg_select_sdev(pg)))
		//	err = SCSI_DH_IMM_RETRY;

		if (err == -EAGAIN || 0 /* err == SCSI_DH_IMM_RETRY */ ||
		    alua->flags & ALUA_PG_RUN_RTPG) {
			alua->flags &= ~ALUA_PG_RUNNING;
			if (0 /* err == SCSI_DH_IMM_RETRY */)
				alua->interval = 0;
			else if (!alua->interval && !(alua->flags & ALUA_PG_RUN_RTPG))
				alua->interval = ALUA_RTPG_RETRY_DELAY;
			alua->flags |= ALUA_PG_RUN_RTPG;
			//spin_unlock_irqrestore(&alua->lock, flags);
			goto queue_rtpg;
		}
		if (err != 0)
			alua->flags &= ~ALUA_PG_RUN_STPG;
	}
	if (alua->flags & ALUA_PG_RUN_STPG) {
		alua->flags &= ~ALUA_PG_RUN_STPG;
		//spin_unlock_irqrestore(&alua->lock, flags);
		err = alua_stpg(sdev);
		//spin_lock_irqsave(&alua->lock, flags);
		if (err == -EAGAIN || alua->flags & ALUA_PG_RUN_RTPG) {
			alua->flags |= ALUA_PG_RUN_RTPG;
			alua->interval = 0;
			alua->flags &= ~ALUA_PG_RUNNING;
			//spin_unlock_irqrestore(&alua->lock, flags);
			goto queue_rtpg;
		}
	}

	//list_splice_init(&alua->rtpg_list, &qdata_list);
	/*
	 * We went through an RTPG, for good or bad.
	 * Re-enable all devices for the next attempt.
	 */
	//list_for_each_entry(h, &alua->dh_list, node)
	//	h->disabled = false;
	alua->sdev = NULL;
	//spin_unlock_irqrestore(&alua->lock, flags);

	if (prev_sdev)
		scsi_device_put(prev_sdev);

	//list_for_each_entry_safe(qdata, tmp, &qdata_list, entry) {
	//	list_del(&qdata->entry);
	//	if (qdata->callback_fn)
	//		qdata->callback_fn(qdata->callback_data, err);
	//	kfree(qdata);
	//}
	//spin_lock_irqsave(&alua->lock, flags);
	alua->flags &= ~ALUA_PG_RUNNING;
	//spin_unlock_irqrestore(&alua->lock, flags);
	scsi_device_put(sdev);
	//kref_put(&alua->kref, release_port_group);
	return;

queue_rtpg:
	if (prev_sdev)
		scsi_device_put(prev_sdev);
	queue_delayed_work(system_wq, &alua->work, alua->interval * HZ);
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
static bool alua_rtpg_queue(struct scsi_device *sdev, bool force)
{
	int start_queue = 0;
	struct alua_data *alua = sdev->alua;
	//unsigned long flags;

	//if (WARN_ON_ONCE(!pg) || scsi_device_get(sdev))
	//	return false;

	//spin_lock_irqsave(&alua->lock, flags);
	//if (qdata) {
	//	list_add_tail(&qdata->entry, &alua->rtpg_list);
	//	alua->flags |= ALUA_PG_RUN_STPG;
	//	force = true;
	//}
	//if (alua->sdev == NULL) {
	//	rcu_read_lock();
	//	if (h && rcu_dereference(h->pg) == pg) {
			alua->interval = 0;
			alua->flags |= ALUA_PG_RUN_RTPG;
	//		kref_get(&alua->kref);
	//		alua->sdev = sdev;
			start_queue = 1;
	//	}
	//	rcu_read_unlock();
	//} else if (!(alua->flags & ALUA_PG_RUN_RTPG) && force) {
	//	alua->flags |= ALUA_PG_RUN_RTPG;
	//	/* Do not queue if the worker is already running */
	//	if (!(alua->flags & ALUA_PG_RUNNING)) {
	//		kref_get(&alua->kref);
	//		start_queue = 1;
	//	}
	//}

	//spin_unlock_irqrestore(&alua->lock, flags);

	if (start_queue) {
		if (queue_delayed_work(system_wq, &alua->work,
				msecs_to_jiffies(ALUA_RTPG_DELAY_MSECS)))
			sdev = NULL;
		else {
			//	kref_put(&alua->kref, release_port_group);
		}
	}
	if (sdev)
		scsi_device_put(sdev);

	return true;
}


static void alua_check(struct scsi_device *sdev, bool force)
{

	alua_rtpg_queue(sdev, force);
}



/*
 * alua_check_tpgs - Evaluate TPGS setting
 * @sdev: device to be checked
 *
 * Examine the TPGS setting of the sdev to find out if ALUA
 * is supported.
 */
int alua_check_tpgs(struct scsi_device *sdev)
{
	int tpgs = TPGS_MODE_NONE;

	/*
	 * ALUA support for non-disk devices is fraught with
	 * difficulties, so disable it for now.
	 */
	if (sdev->type != TYPE_DISK) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: disable for non-disk devices\n",
			    DRV_NAME);
		return tpgs;
	}

	tpgs = scsi_device_tpgs(sdev);
	switch (tpgs) {
	case TPGS_MODE_EXPLICIT|TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev,
			    "%s: supports implicit and explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_EXPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports implicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_NONE:
		sdev_printk(KERN_INFO, sdev, "%s: not supported\n",
			    DRV_NAME);
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: unsupported TPGS setting %d\n",
			    DRV_NAME, tpgs);
		tpgs = TPGS_MODE_NONE;
		break;
	}

	return tpgs;
}
EXPORT_SYMBOL_GPL(alua_check_tpgs);

/*
 * alua_tur - Send a TEST UNIT READY
 * @sdev: device to which the TEST UNIT READY command should be send
 *
 * Send a TEST UNIT READY to @sdev to figure out the device state
 * Returns -EAGAIN if the sense code is NOT READY/ALUA TRANSITIONING,
 * 0 if no error occurred, and -EIO otherwise.
 */
int alua_tur(struct scsi_device *sdev)
{
	struct scsi_sense_hdr sense_hdr;
	int retval;

	retval = scsi_test_unit_ready(sdev, ALUA_FAILOVER_TIMEOUT * HZ,
				      ALUA_FAILOVER_RETRIES, &sense_hdr);
	if ((sense_hdr.sense_key == NOT_READY ||
	     sense_hdr.sense_key == UNIT_ATTENTION) &&
	    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a)
		return -EAGAIN;
	else if (retval)
		return -EIO;
	else
		return 0;
}
EXPORT_SYMBOL_GPL(alua_tur);

/*
 * submit_rtpg - Issue a REPORT TARGET GROUP STATES command
 * @sdev: sdev the command should be sent to
 */
int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
		       int bufflen, struct scsi_sense_hdr *sshdr)
{
	u8 cdb[MAX_COMMAND_SIZE];
	blk_opf_t opf = REQ_OP_DRV_IN | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_IN;
	if (sdev->alua->rtpg_ext_hdr_unsupp)
		cdb[1] = MI_REPORT_TARGET_PGS;
	else
		cdb[1] = MI_REPORT_TARGET_PGS | MI_EXT_HDR_PARAM_FMT;
	put_unaligned_be32(bufflen, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, buff, bufflen,
				ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}
EXPORT_SYMBOL_GPL(submit_rtpg);

static char print_alua_state(unsigned char state)
{
	switch (state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return 'A';
	case SCSI_ACCESS_STATE_ACTIVE:
		return 'N';
	case SCSI_ACCESS_STATE_STANDBY:
		return 'S';
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		return 'U';
	case SCSI_ACCESS_STATE_LBA:
		return 'L';
	case SCSI_ACCESS_STATE_OFFLINE:
		return 'O';
	case SCSI_ACCESS_STATE_TRANSITIONING:
		return 'T';
	default:
		return 'X';
	}
}

void alua_print_info(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;

	sdev_printk(KERN_INFO, sdev,
		"alua: port group %02x state %c %s supports %c%c%c%c%c%c%c\n",
		alua->group_id, print_alua_state(alua->state),
		alua->pref ? "preferred" : "non-preferred",
		alua->valid_states&TPGS_SUPPORT_TRANSITION?'T':'t',
		alua->valid_states&TPGS_SUPPORT_OFFLINE?'O':'o',
		alua->valid_states&TPGS_SUPPORT_LBA_DEPENDENT?'L':'l',
		alua->valid_states&TPGS_SUPPORT_UNAVAILABLE?'U':'u',
		alua->valid_states&TPGS_SUPPORT_STANDBY?'S':'s',
		alua->valid_states&TPGS_SUPPORT_NONOPTIMIZED?'N':'n',
		alua->valid_states&TPGS_SUPPORT_OPTIMIZED?'A':'a');
}
EXPORT_SYMBOL_GPL(alua_print_info);

/*
 * submit_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Currently we're only setting the current target port group state
 * to 'active/optimized' and let the array firmware figure out
 * the states of the remaining groups.
 */
int submit_stpg(struct scsi_device *sdev, int group_id,
		       struct scsi_sense_hdr *sshdr)
{
	u8 cdb[MAX_COMMAND_SIZE];
	unsigned char stpg_data[8];
	int stpg_len = 8;
	blk_opf_t opf = REQ_OP_DRV_OUT | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the data buffer */
	memset(stpg_data, 0, stpg_len);
	stpg_data[4] = SCSI_ACCESS_STATE_OPTIMAL;
	put_unaligned_be16(group_id, &stpg_data[6]);

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_OUT;
	cdb[1] = MO_SET_TARGET_PGS;
	put_unaligned_be32(stpg_len, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, stpg_data,
				stpg_len, ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}
EXPORT_SYMBOL_GPL(submit_stpg);

static int alua_rtpg_run(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;
	struct scsi_sense_hdr sense_hdr;
	int len, k, off, bufflen = ALUA_RTPG_SIZE;
	int group_id_old, state_old, pref_old, valid_states_old;
	unsigned char *desc, *buff;
	unsigned err;
	int retval;
	unsigned int tpg_desc_tbl_off;
	unsigned char orig_transition_tmo;
	bool transitioning_sense = false;
	int rel_port;

	group_id_old = alua->group_id;
	state_old = alua->state;
	pref_old = alua->pref;
	valid_states_old = alua->valid_states;

	alua->group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	if (sdev->alua->group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support.
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    __func__);
		return -EIO;

	}

	if (!alua->expiry) {
		unsigned long transition_tmo = ALUA_FAILOVER_TIMEOUT * HZ;

		if (alua->transition_tmo)
			transition_tmo = alua->transition_tmo * HZ;

		alua->expiry = round_jiffies_up(jiffies + transition_tmo);
	}
	buff = kzalloc(bufflen, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

 retry:
	err = 0;
	retval = submit_rtpg(sdev, buff, bufflen, &sense_hdr);

	if (retval) {
		/*
		 * Some (broken) implementations have a habit of returning
		 * an error during things like firmware update etc.
		 * But if the target only supports active/optimized there's
		 * not much we can do; it's not that we can switch paths
		 * or anything.
		 * So ignore any errors to avoid spurious failures during
		 * path failover.
		 */
		if ((alua->valid_states & ~TPGS_SUPPORT_OPTIMIZED) == 0) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: ignoring rtpg result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			return 0;
		}
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: rtpg failed, result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			if (retval < 0)
				return -EBUSY; //-EBUSY
			if (host_byte(retval) == DID_NO_CONNECT)
				return -EBADF;// SCSI_DH_RES_TEMP_UNAVAIL
			return -EIO; // -EIO
		}

		/*
		 * submit_rtpg() has failed on existing arrays
		 * when requesting extended header info, and
		 * the array doesn't support extended headers,
		 * even though it shouldn't according to T10.
		 * The retry without rtpg_ext_hdr_req set
		 * handles this.
		 * Note:  some arrays return a sense key of ILLEGAL_REQUEST
		 * with ASC 00h if they don't support the extended header.
		 */
		if (alua->rtpg_ext_hdr_unsupp == false &&
		    sense_hdr.sense_key == ILLEGAL_REQUEST) {
			alua->rtpg_ext_hdr_unsupp = true;
			goto retry;
		}
		/*
		 * If the array returns with 'ALUA state transition'
		 * sense code here it cannot return RTPG data during
		 * transition. So set the state to 'transitioning' directly.
		 */
		if (sense_hdr.sense_key == NOT_READY &&
		    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a) {
			transitioning_sense = true;
			goto skip_rtpg;
		}
		/*
		 * Retry on any other UNIT ATTENTION occurred.
		 */
		if (sense_hdr.sense_key == UNIT_ATTENTION)
			err = -EAGAIN;//-EAGAIN;
		if (err == -EAGAIN &&
		    alua->expiry != 0 && time_before(jiffies, alua->expiry)) {
			sdev_printk(KERN_ERR, sdev, "%s: rtpg retry\n",
				    DRV_NAME);
			scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
			kfree(buff);
			return err;
		}
		sdev_printk(KERN_ERR, sdev, "%s: rtpg failed\n",
			    DRV_NAME);
		scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
		kfree(buff);
		alua->expiry = 0;
		return -EIO; //-EIO
	}

	len = get_unaligned_be32(&buff[0]) + 4;

	if (len > bufflen) {
		/* Resubmit with the correct length */
		kfree(buff);
		bufflen = len;
		buff = kmalloc(bufflen, GFP_KERNEL);
		if (!buff) {
			sdev_printk(KERN_WARNING, sdev,
				    "%s: kmalloc buffer failed\n",__func__);
			/* Temporary failure, bypass */
			alua->expiry = 0;
			return -ENOMEM; //-EBUSY
		}
		goto retry;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR && buff[5] != 0)
		alua->transition_tmo = buff[5];
	else
		alua->transition_tmo = ALUA_FAILOVER_TIMEOUT;

	if (orig_transition_tmo != alua->transition_tmo) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: transition timeout set to %d seconds\n",
			    DRV_NAME, alua->transition_tmo);
		alua->expiry = jiffies + alua->transition_tmo * HZ;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR)
		tpg_desc_tbl_off = 8;
	else
		tpg_desc_tbl_off = 4;

	for (k = tpg_desc_tbl_off, desc = buff + tpg_desc_tbl_off;
	     k < len;
	     k += off, desc += off) {
		u16 group_id = get_unaligned_be16(&desc[2]);

		if (group_id == alua->group_id) {
			alua->state = desc[0] & 0x0f;
			alua->pref = desc[0] >> 7;
			alua->valid_states = desc[1];
			sdev->access_state = desc[0];
			break;
		}
		off = 8 + (desc[7] * 4);
	}

 skip_rtpg:
	//spin_lock_irqsave(&alua->lock, flags);
	if (transitioning_sense)
		alua->state = SCSI_ACCESS_STATE_TRANSITIONING;

	if (group_id_old != alua->group_id || state_old != alua->state ||
	    pref_old != alua->pref || valid_states_old != alua->valid_states) {
		alua_print_info(sdev);
	}

	switch (alua->state) {
	case SCSI_ACCESS_STATE_TRANSITIONING:
		if (time_before(jiffies, alua->expiry)) {
			/* State transition, retry */
			alua->interval = ALUA_RTPG_RETRY_DELAY;
			err = -EAGAIN; //-EAGAIN;
		} else {
			/* Transitioning time exceeded, set port to standby */
			err = -EIO;//-EIO;
			alua->state = SCSI_ACCESS_STATE_STANDBY;
			alua->expiry = 0;
			
			sdev->access_state = alua->state & SCSI_ACCESS_STATE_MASK;
			if (alua->pref)
				sdev->access_state |= SCSI_ACCESS_STATE_PREFERRED;
		}
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		/* Path unusable */
		err = -ENODEV;//SCSI_DH_DEV_OFFLINED;
		alua->expiry = 0;
		break;
	default:
		/* Useable path if active */
		err = 0;
		alua->expiry = 0;
		break;
	}
	//spin_unlock_irqrestore(&alua->lock, flags);
	kfree(buff);
	return err;
}


static void alua_work(struct work_struct *work)
{
	struct alua_data *alua =
		container_of(work, struct alua_data, work.work);
	int ret;

	ret = alua_rtpg_run(alua->sdev);
	pr_err("%s ret=%d from alua_rtpg_run\n", __func__, ret);

	if (ret == -EAGAIN || ret == -EBADF) {
		if (ret == -EBADF)
			alua->interval = 0;
		else if (!alua->interval)
			alua->interval = ALUA_RTPG_RETRY_DELAY;

		queue_delayed_work(system_wq, &alua->work, alua->interval * HZ);
	}
}

void scsi_mpath_run_rtpg(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;

	queue_delayed_work(system_wq, &alua->work,
		alua->interval ? alua->interval * HZ : msecs_to_jiffies(ALUA_RTPG_DELAY_MSECS));
}

void scsi_alua_init(struct scsi_device *sdev)
{
	sdev_printk(KERN_INFO, sdev,
			    "%s: tpgs=%d\n",
			    DRV_NAME, scsi_device_tpgs(sdev));
	sdev->alua = kzalloc(sizeof(*sdev->alua), GFP_KERNEL);
	if (!sdev->alua)
		return;

	sdev->alua->group_id = -1;
	sdev->alua->tpgs = scsi_device_tpgs(sdev);
	sdev->alua->state = SCSI_ACCESS_STATE_OPTIMAL;
	sdev->alua->valid_states = TPGS_SUPPORT_ALL;

	INIT_DELAYED_WORK(&sdev->alua->work, alua_work);
	sdev->alua->sdev = sdev;

	scsi_mpath_run_rtpg(sdev);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_alua");
