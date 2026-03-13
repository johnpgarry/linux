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
#include <scsi/scsi_alua.h>

#define ALUA_DH_NAME "alua"
#define ALUA_DH_VER "2.0"

#define ALUA_FAILOVER_RETRIES		5
#define ALUA_RTPG_DELAY_MSECS		5
#define ALUA_RTPG_RETRY_DELAY		2

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
//#define ALUA_PG_RUNNING			0x40

static uint optimize_stpg;
module_param(optimize_stpg, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(optimize_stpg, "Allow use of a non-optimized path, rather than sending a STPG, when implicit TPGS is supported (0=No,1=Yes). Default is 0.");

static struct workqueue_struct *kaluad_wq;

#ifdef olds
struct alua_port_group {
	struct kref		kref;
	struct rcu_head		rcu;
	struct list_head	node;
	struct list_head	dh_list;
	unsigned char		device_id_str[256];
	int			device_id_len;
	int			group_id;
	int			tpgs;
	int			state;
	int			pref;
	int			valid_states;
	unsigned		flags; /* used for optimizing STPG */
	unsigned char		transition_tmo;
	unsigned long		expiry;
	//unsigned long		interval;
	struct delayed_work	rtpg_work;
	spinlock_t		lock;
	struct list_head	rtpg_list;
	struct scsi_device	*rtpg_sdev;
};
#endif

struct alua_dh_data {
	int			group_id;
	struct scsi_device	*sdev;
	int			init_error;
	struct mutex		init_mutex;
	bool			disabled;


	unsigned		flags; /* used for optimizing STPG */

	/* alua stuff */
	unsigned char		transition_tmo;
	unsigned long		expiry;
	//unsigned long		interval;
	struct delayed_work	rtpg_work;
	struct list_head	rtpg_list;
};

struct alua_queue_data {
	struct list_head	entry;
	activate_complete	callback_fn;
	void			*callback_data;
};

#define ALUA_POLICY_SWITCH_CURRENT	0
#define ALUA_POLICY_SWITCH_ALL		1

static void alua_rtpg_work(struct work_struct *work);
static bool alua_rtpg_queue(struct scsi_device *sdev,
			    struct alua_queue_data *qdata, bool force);
static void alua_check(struct scsi_device *sdev, bool force);

#ifdef oldss
/*
 * alua_alloc_pg - Allocate a new port_group structure
 * @sdev: scsi device
 * @group_id: port group id
 * @tpgs: target port group settings
 *
 * Allocate a new port_group structure for a given
 * device.
 */
static struct alua_port_group *alua_alloc_pg(struct scsi_device *sdev,
					     int group_id, int tpgs)
{
	struct alua_port_group *pg, *tmp_pg;

	pg = kzalloc(sizeof(struct alua_port_group), GFP_KERNEL);
	if (!pg)
		return ERR_PTR(-ENOMEM);

	pg->device_id_len = scsi_vpd_lun_id(sdev, pg->device_id_str,
					    sizeof(pg->device_id_str));
	if (pg->device_id_len <= 0) {
		/*
		 * TPGS supported but no device identification found.
		 * Generate private device identification.
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No device descriptors found\n",
			    ALUA_DH_NAME);
		pg->device_id_str[0] = '\0';
		pg->device_id_len = 0;
	}
	pg->group_id = group_id;
	pg->tpgs = tpgs;
	pg->state = SCSI_ACCESS_STATE_OPTIMAL;
	pg->valid_states = TPGS_SUPPORT_ALL;
	if (optimize_stpg)
		h->flags |= ALUA_OPTIMIZE_STPG;
	kref_init(&pg->kref);
	INIT_DELAYED_WORK(&pg->rtpg_work, alua_rtpg_work);
	INIT_LIST_HEAD(&pg->rtpg_list);
	INIT_LIST_HEAD(&pg->node);
	INIT_LIST_HEAD(&pg->dh_list);
	spin_lock_init(&pg->lock);

	spin_lock(&port_group_lock);
	tmp_pg = alua_find_get_pg(pg->device_id_str, pg->device_id_len,
				  group_id);
	if (tmp_pg) {
		spin_unlock(&port_group_lock);
		kfree(pg);
		return tmp_pg;
	}

	list_add(&pg->node, &port_group_list);
	spin_unlock(&port_group_lock);

	return pg;
}
#endif

/*
 * alua_check_vpd - Evaluate INQUIRY vpd page 0x83
 * @sdev: device to be checked
 *
 * Extract the relative target port and the target port group
 * descriptor from the list of identificators.
 */
static int alua_check_vpd(struct scsi_device *sdev, struct alua_dh_data *h,
			  int tpgs)
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
			    ALUA_DH_NAME);
		return SCSI_DH_DEV_UNSUPP;
	}

	alua_rtpg_queue(sdev, NULL, true);

	return SCSI_DH_OK;
}

#ifdef dsdsdsd
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
#endif

static void alua_handle_state_transition(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;

	WRITE_ONCE(alua->state, SCSI_ACCESS_STATE_TRANSITIONING);

	alua_check(sdev, false);
}

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

#ifdef olddsddsd
/*
 * alua_rtpg - Evaluate REPORT TARGET GROUP STATES
 * @sdev: the device to be evaluated.
 *
 * Evaluate the Target Port Group State.
 * Returns SCSI_DH_DEV_OFFLINED if the path is
 * found to be unusable.
 */
static int alua_rtpg(struct scsi_device *sdev)
{
	struct scsi_sense_hdr sense_hdr;
	struct alua_dh_data *h = sdev->handler_data;
	int len, k, off, bufflen = ALUA_RTPG_SIZE;
	int group_id_old, state_old, pref_old, valid_states_old;
	unsigned char *desc, *buff;
	unsigned err;
	int retval;
	unsigned int tpg_desc_tbl_off;
	unsigned char orig_transition_tmo;
	bool transitioning_sense = false;
	int rel_port, group_id = scsi_vpd_tpg_id(sdev, &rel_port);

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

	group_id_old = h->group_id;
	state_old = h->state;
	pref_old = h->pref;
	valid_states_old = h->valid_states;

	if (!h->expiry) {
		unsigned long transition_tmo = ALUA_FAILOVER_TIMEOUT * HZ;

		if (h->transition_tmo)
			transition_tmo = h->transition_tmo * HZ;

		h->expiry = round_jiffies_up(jiffies + transition_tmo);
	}

	buff = kzalloc(bufflen, GFP_KERNEL);
	if (!buff)
		return SCSI_DH_DEV_TEMP_BUSY;

 retry:
	err = 0;
	retval = submit_rtpg(sdev, buff, bufflen, &sense_hdr,
			h->rtpg_ext_hdr_unsupp);

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
		if ((h->valid_states & ~TPGS_SUPPORT_OPTIMIZED) == 0) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: ignoring rtpg result %d\n",
				    ALUA_DH_NAME, retval);
			kfree(buff);
			return SCSI_DH_OK;
		}
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: rtpg failed, result %d\n",
				    ALUA_DH_NAME, retval);
			kfree(buff);
			if (retval < 0)
				return SCSI_DH_DEV_TEMP_BUSY;
			if (host_byte(retval) == DID_NO_CONNECT)
				return SCSI_DH_RES_TEMP_UNAVAIL;
			return SCSI_DH_IO;
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
		if (!h->rtpg_ext_hdr_unsupp &&
		    sense_hdr.sense_key == ILLEGAL_REQUEST) {
			h->rtpg_ext_hdr_unsupp = true;
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
			err = SCSI_DH_RETRY;
		if (err == SCSI_DH_RETRY &&
		    h->expiry != 0 && time_before(jiffies, h->expiry)) {
			sdev_printk(KERN_ERR, sdev, "%s: rtpg retry\n",
				    ALUA_DH_NAME);
			scsi_print_sense_hdr(sdev, ALUA_DH_NAME, &sense_hdr);
			kfree(buff);
			return err;
		}
		sdev_printk(KERN_ERR, sdev, "%s: rtpg failed\n",
			    ALUA_DH_NAME);
		scsi_print_sense_hdr(sdev, ALUA_DH_NAME, &sense_hdr);
		kfree(buff);
		h->expiry = 0;
		return SCSI_DH_IO;
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
			h->expiry = 0;
			return SCSI_DH_DEV_TEMP_BUSY;
		}
		goto retry;
	}

	orig_transition_tmo = h->transition_tmo;
	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR && buff[5] != 0)
		h->transition_tmo = buff[5];
	else
		h->transition_tmo = ALUA_FAILOVER_TIMEOUT;

	if (orig_transition_tmo != h->transition_tmo) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: transition timeout set to %d seconds\n",
			    ALUA_DH_NAME, h->transition_tmo);
		h->expiry = jiffies + h->transition_tmo * HZ;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR)
		tpg_desc_tbl_off = 8;
	else
		tpg_desc_tbl_off = 4;

	for (k = tpg_desc_tbl_off, desc = buff + tpg_desc_tbl_off;
	     k < len;
	     k += off, desc += off) {
		u16 group_id_desc = get_unaligned_be16(&desc[2]);

		if (group_id_desc == group_id) {
			h->group_id = group_id;
			WRITE_ONCE(h->state, desc[0] & 0x0f);
			h->pref = desc[0] >> 7;
			WRITE_ONCE(sdev->access_state, desc[0]);
			h->valid_states = desc[1];
		}
		off = 8 + (desc[7] * 4);
	}

 skip_rtpg:
	
	if (transitioning_sense)
		WRITE_ONCE(h->state, SCSI_ACCESS_STATE_TRANSITIONING);

	if (group_id_old != h->group_id || state_old != h->state ||
		pref_old != h->pref || valid_states_old != h->valid_states)
		sdev_printk(KERN_INFO, sdev,
			"%s: port group %02x state %c %s supports %c%c%c%c%c%c%c\n",
			ALUA_DH_NAME, h->group_id, print_alua_state(h->state),
			h->pref ? "preferred" : "non-preferred",
			h->valid_states&TPGS_SUPPORT_TRANSITION?'T':'t',
			h->valid_states&TPGS_SUPPORT_OFFLINE?'O':'o',
			h->valid_states&TPGS_SUPPORT_LBA_DEPENDENT?'L':'l',
			h->valid_states&TPGS_SUPPORT_UNAVAILABLE?'U':'u',
			h->valid_states&TPGS_SUPPORT_STANDBY?'S':'s',
			h->valid_states&TPGS_SUPPORT_NONOPTIMIZED?'N':'n',
			h->valid_states&TPGS_SUPPORT_OPTIMIZED?'A':'a');

	switch (h->state) {
	case SCSI_ACCESS_STATE_TRANSITIONING:
		if (time_before(jiffies, h->expiry)) {
			/* State transition, retry */
			h->interval = ALUA_RTPG_RETRY_DELAY;
			err = SCSI_DH_RETRY;
		} else {
			struct alua_dh_data *h;

			/* Transitioning time exceeded, set port to standby */
			err = SCSI_DH_IO;
			WRITE_ONCE(h->state, SCSI_ACCESS_STATE_STANDBY);
			h->expiry = 0;
			// FIXME use READ once for all the code below
			sdev->access_state = h->state & SCSI_ACCESS_STATE_MASK;
			if (h->pref)
				sdev->access_state |=
						SCSI_ACCESS_STATE_PREFERRED;
		}
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		/* Path unusable */
		err = SCSI_DH_DEV_OFFLINED;
		h->expiry = 0;
		break;
	default:
		/* Useable path if active */
		err = SCSI_DH_OK;
		h->expiry = 0;
		break;
	}
	
	kfree(buff);
	return err;
}

/*
 * alua_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Issue a SET TARGET PORT GROUP command and evaluate the
 * response. Returns SCSI_DH_RETRY per default to trigger
 * a re-evaluation of the target group state or SCSI_DH_OK
 * if no further action needs to be taken.
 */
static unsigned alua_stpg(struct scsi_device *sdev)
{
	int retval;
	struct scsi_sense_hdr sense_hdr;
	struct alua_dh_data *h = sdev->handler_data;

	if (!(h->tpgs & TPGS_MODE_EXPLICIT)) {
		/* Only implicit ALUA supported, retry */
		return SCSI_DH_RETRY;
	}
	switch (h->state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return SCSI_DH_OK;
	case SCSI_ACCESS_STATE_ACTIVE:
		if ((h->flags & ALUA_OPTIMIZE_STPG) &&
		    !h->pref &&
		    (h->tpgs & TPGS_MODE_IMPLICIT))
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
			    ALUA_DH_NAME, h->state);
		return SCSI_DH_NOSYS;
	}
	retval = submit_stpg(sdev, h->group_id, &sense_hdr);

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
	struct alua_dh_data *h;
	struct scsi_device *sdev = NULL, *prev_sdev;

	lockdep_assert_held(&pg->lock);
	if (WARN_ON(!pg->rtpg_sdev))
		return NULL;

	/*
	 * RCU protection isn't necessary for dh_list here
	 * as we hold pg->lock, but for access to h->pg.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(h, &pg->dh_list, node) {
		if (!h->sdev)
			continue;
		if (h->sdev == pg->rtpg_sdev) {
			h->disabled = true;
			continue;
		}
		if (rcu_dereference(h->pg) == pg &&
		    !h->disabled &&
		    !scsi_device_get(h->sdev)) {
			sdev = h->sdev;
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
#endif

static void alua_rtpg_work(struct work_struct *work)
{
	struct alua_dh_data *h =
		container_of(work, struct alua_dh_data, rtpg_work.work);
	struct scsi_device *sdev = h->sdev;
	LIST_HEAD(qdata_list);
	int err = SCSI_DH_OK;
	struct alua_queue_data *qdata, *tmp;
	struct alua_data *alua = sdev->alua;
	unsigned long interval = 0;

	pr_err("%s sdev=%pS h=%pS\n", __func__, sdev, h);
	if (h->flags & ALUA_PG_RUN_RTPG) {
		int state = alua->state;

		h->flags &= ~ALUA_PG_RUN_RTPG;
		
		if (state == SCSI_ACCESS_STATE_TRANSITIONING) {
			if (alua_tur(sdev) == -EAGAIN) {

				h->flags |= ALUA_PG_RUN_RTPG;
				
				queue_delayed_work(kaluad_wq, &h->rtpg_work,
						   ALUA_RTPG_RETRY_DELAY * HZ);
				return;
			}
			/* Send RTPG on failure or if TUR indicates SUCCESS */
		}
		err = alua_rtpg2(sdev);

		/* -EAGAIN Retry on any other UNIT ATTENTION occurred. */
		if (err == -EAGAIN || h->flags & ALUA_PG_RUN_RTPG) {
			if (!interval && !(h->flags & ALUA_PG_RUN_RTPG))
				interval = ALUA_RTPG_RETRY_DELAY;
			h->flags |= ALUA_PG_RUN_RTPG;
			
			goto queue_rtpg;
		}
		if (err != 0)
			h->flags &= ~ALUA_PG_RUN_STPG;
	}
	if (h->flags & ALUA_PG_RUN_STPG) {
		h->flags &= ~ALUA_PG_RUN_STPG;
		
		err = alua_stpg2(sdev);
		
		if (err == -EAGAIN || h->flags & ALUA_PG_RUN_RTPG) {
			h->flags |= ALUA_PG_RUN_RTPG;
			interval = 0;
			
			goto queue_rtpg;
		}
	}

	list_splice_init(&h->rtpg_list, &qdata_list);
	/*
	 * We went through an RTPG, for good or bad.
	 * Re-enable all devices for the next attempt.
	 */
	h->disabled = false;


	pr_err("%s2 sdev=%pS h=%pS checking qdata_list\n", __func__, sdev, h);
	list_for_each_entry_safe(qdata, tmp, &qdata_list, entry) {
		list_del(&qdata->entry);
		if (qdata->callback_fn)
			qdata->callback_fn(qdata->callback_data, err);
		kfree(qdata);
	}
	
	scsi_device_put(sdev);
	
	return;

queue_rtpg:
	queue_delayed_work(kaluad_wq, &h->rtpg_work, interval * HZ);
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
static bool alua_rtpg_queue(struct scsi_device *sdev,
			    struct alua_queue_data *qdata, bool force)
{
	int start_queue = 0;
	struct alua_dh_data *h = sdev->handler_data;

	if (scsi_device_get(sdev))
		return false;
	
	if (qdata) {
		list_add_tail(&qdata->entry, &h->rtpg_list);
		h->flags |= ALUA_PG_RUN_STPG;
		force = true;
	}
	if (!(h->flags & ALUA_PG_RUN_RTPG) && force) {
		h->flags |= ALUA_PG_RUN_RTPG;
		start_queue = 1;
	}

	if (start_queue) {
		if (queue_delayed_work(kaluad_wq, &h->rtpg_work,
				msecs_to_jiffies(ALUA_RTPG_DELAY_MSECS)))
			sdev = NULL;
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
static int alua_initialize(struct scsi_device *sdev, struct alua_dh_data *h)
{
	int err = SCSI_DH_DEV_UNSUPP, tpgs;

	mutex_lock(&h->init_mutex);
	h->disabled = false;
	tpgs = alua_check_tpgs(sdev);
	if (tpgs != TPGS_MODE_NONE)
		err = alua_check_vpd(sdev, h, tpgs);
	h->init_error = err;
	mutex_unlock(&h->init_mutex);
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
	struct alua_dh_data *h = sdev->handler_data;
	unsigned int optimize = 0, argc;
	const char *p = params;
	int result = SCSI_DH_OK;

	if ((sscanf(params, "%u", &argc) != 1) || (argc != 1))
		return -EINVAL;

	while (*p++)
		;
	if ((sscanf(p, "%u", &optimize) != 1) || (optimize > 1))
		return -EINVAL;

	//spin_lock_irqsave(&pg->lock, flags); maybe should lock this
	if (optimize)
		h->flags |= ALUA_OPTIMIZE_STPG;
	else
		h->flags &= ~ALUA_OPTIMIZE_STPG;

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

	qdata = kzalloc(sizeof(*qdata), GFP_KERNEL);
	if (!qdata) {
		err = SCSI_DH_RES_TEMP_UNAVAIL;
		goto out;
	}
	qdata->callback_fn = fn;
	qdata->callback_data = data;
	pr_err("%s sdev=%pS calling alua_rtpg_queue\n", __func__, sdev);
	if (alua_rtpg_queue(sdev, qdata, true)) {
		fn = NULL;
	} else {
		kfree(qdata);
		err = SCSI_DH_DEV_OFFLINED;
	}
	pr_err("%s2 sdev=%pS called alua_rtpg_queue fn=%pS\n", __func__, sdev, fn);
	
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
	alua_rtpg_queue(sdev, NULL, force);
}

/*
 * alua_prep_fn - request callback
 *
 * Fail I/O to all paths not in state
 * active/optimized or active/non-optimized.
 */
static blk_status_t alua_prep_fn(struct scsi_device *sdev, struct request *req)
{
	struct alua_data *alua = sdev->alua;
	unsigned char state = SCSI_ACCESS_STATE_OPTIMAL;

	state = READ_ONCE(alua->state);

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
	struct alua_dh_data *h = sdev->handler_data;

	alua_initialize(sdev, h);
}

/*
 * alua_bus_attach - Attach device handler
 * @sdev: device to be attached to
 */
static int alua_bus_attach(struct scsi_device *sdev)
{
	struct alua_dh_data *h;
	int err;

	pr_err("%s sdev->alua=%pS\n", __func__, sdev);
	if (sdev->scsi_mpath_dev)
		return SCSI_DH_DEV_UNSUPP;
	h = kzalloc(sizeof(*h) , GFP_KERNEL);
	if (!h)
		return SCSI_DH_NOMEM;
	h->init_error = SCSI_DH_OK;
	h->sdev = sdev;
	INIT_DELAYED_WORK(&h->rtpg_work, alua_rtpg_work);
	INIT_LIST_HEAD(&h->rtpg_list);

	mutex_init(&h->init_mutex);
	sdev->handler_data = h;
	err = alua_initialize(sdev, h);
	pr_err("%s2 sdev->alua=%pS err=%d\n", __func__, sdev, err);
	if (err != SCSI_DH_OK && err != SCSI_DH_DEV_OFFLINED)
		goto failed;

	return SCSI_DH_OK;
failed:
	sdev->handler_data = NULL;
	kfree(h);
	return err;
}

/*
 * alua_bus_detach - Detach device handler
 * @sdev: device to be detached from
 */
static void alua_bus_detach(struct scsi_device *sdev)
{
	struct alua_dh_data *h = sdev->handler_data;
	sdev->handler_data = NULL;
	kfree(h);
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
