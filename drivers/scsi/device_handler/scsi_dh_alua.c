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
#include <scsi/scsi_alua.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_dh.h>

#define ALUA_DH_NAME "alua"
#define ALUA_DH_VER "2.0"

#define TPGS_SUPPORT_NONE		0x00
#define TPGS_SUPPORT_OPTIMIZED		0x01
#define TPGS_SUPPORT_NONOPTIMIZED	0x02
#define TPGS_SUPPORT_STANDBY		0x04
#define TPGS_SUPPORT_UNAVAILABLE	0x08
#define TPGS_SUPPORT_LBA_DEPENDENT	0x10
#define TPGS_SUPPORT_OFFLINE		0x40
#define TPGS_SUPPORT_TRANSITION		0x80
#define TPGS_SUPPORT_ALL		0xdf

#define RTPG_FMT_MASK			0x70
#define RTPG_FMT_EXT_HDR		0x10

#define TPGS_MODE_UNINITIALIZED		 -1
#define TPGS_MODE_NONE			0x0
#define TPGS_MODE_IMPLICIT		0x1
#define TPGS_MODE_EXPLICIT		0x2

#define ALUA_RTPG_SIZE			128
#define ALUA_FAILOVER_TIMEOUT		60
#define ALUA_FAILOVER_RETRIES		5
#define ALUA_RTPG_DELAY_MSECS		5
#define ALUA_RTPG_RETRY_DELAY		2

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
#define ALUA_PG_RUNNING			0x40

static uint optimize_stpg;
module_param(optimize_stpg, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(optimize_stpg, "Allow use of a non-optimized path, rather than sending a STPG, when implicit TPGS is supported (0=No,1=Yes). Default is 0.");

static struct workqueue_struct *kaluad_wq;

struct alua_dh_data {
	int			group_id;
	struct scsi_device	*sdev;
	int			init_error;
	struct mutex		init_mutex;
	unsigned		flags; /* used for optimizing STPG */
	spinlock_t		lock;

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


/*
 * submit_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Currently we're only setting the current target port group state
 * to 'active/optimized' and let the array firmware figure out
 * the states of the remaining groups.
 */
static int submit_stpg(struct scsi_device *sdev, int group_id,
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
	alua_rtpg_queue(sdev, NULL, true);

	return SCSI_DH_OK;
}

static enum scsi_disposition alua_check_sense(struct scsi_device *sdev,
					      struct scsi_sense_hdr *sense_hdr)
{
	bool check = false;
	enum scsi_disposition dis = scsi_alua_check_sense(sdev, sense_hdr, &check);
	if (check)
		alua_check(sdev, true);
	return dis;
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
	struct alua_data *alua = sdev->alua;

	if (!(alua->tpgs & TPGS_MODE_EXPLICIT)) {
		/* Only implicit ALUA supported, retry */
		return SCSI_DH_RETRY;
	}
	switch (alua->state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return SCSI_DH_OK;
	case SCSI_ACCESS_STATE_ACTIVE:
		if ((h->flags & ALUA_OPTIMIZE_STPG) &&
		    !alua->pref &&
		    (alua->tpgs & TPGS_MODE_IMPLICIT))
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
			    ALUA_DH_NAME, alua->state);
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

static void alua_rtpg_work(struct work_struct *work)
{
	struct alua_dh_data *h =
		container_of(work, struct alua_dh_data, rtpg_work.work);
	struct scsi_device *sdev = h->sdev;
	LIST_HEAD(qdata_list);
	int err = SCSI_DH_OK;
	struct alua_queue_data *qdata, *tmp;
	struct alua_data *alua = sdev->alua;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&h->lock, flags);
	h->flags |= ALUA_PG_RUNNING;
	if (h->flags & ALUA_PG_RUN_RTPG) {
		int state = alua->state;

		h->flags &= ~ALUA_PG_RUN_RTPG;
		spin_unlock_irqrestore(&h->lock, flags);
		if (state == SCSI_ACCESS_STATE_TRANSITIONING) {
			if (scsi_alua_tur(sdev) == -EAGAIN) {
				spin_lock_irqsave(&h->lock, flags);
				h->flags &= ~ALUA_PG_RUNNING;
				h->flags |= ALUA_PG_RUN_RTPG;
				alua->interval = ALUA_RTPG_RETRY_DELAY;
				spin_unlock_irqrestore(&h->lock, flags);
				queue_delayed_work(kaluad_wq, &h->rtpg_work,
						   alua->interval * HZ);
				return;
			}
			/* Send RTPG on failure or if TUR indicates SUCCESS */
		}
		err = scsi_alua_rtpg(sdev);
		spin_lock_irqsave(&h->lock, flags);

		if (err == SCSI_DH_RETRY || h->flags & ALUA_PG_RUN_RTPG) {
			h->flags &= ~ALUA_PG_RUNNING;

			if (!alua->interval && !(h->flags & ALUA_PG_RUN_RTPG))
				alua->interval = ALUA_RTPG_RETRY_DELAY;
			h->flags |= ALUA_PG_RUN_RTPG;
			spin_unlock_irqrestore(&h->lock, flags);
			goto queue_rtpg;
		}
		if (err != SCSI_DH_OK)
			h->flags &= ~ALUA_PG_RUN_STPG;
	}
	spin_lock_irqsave(&h->lock, flags);
	if (h->flags & ALUA_PG_RUN_STPG) {
		h->flags &= ~ALUA_PG_RUN_STPG;
		spin_unlock_irqrestore(&h->lock, flags);
		ret = alua_stpg(sdev);
		if (err == -EAGAIN || h->flags & ALUA_PG_RUN_RTPG) {
			spin_lock_irqsave(&h->lock, flags);
			h->flags |= ALUA_PG_RUN_RTPG;
			h->flags &= ~ALUA_PG_RUNNING;
			spin_unlock_irqrestore(&h->lock, flags);
			goto queue_rtpg;
		}
	} else {
		spin_unlock_irqrestore(&h->lock, flags);
	}

	list_splice_init(&h->rtpg_list, &qdata_list);

	list_for_each_entry_safe(qdata, tmp, &qdata_list, entry) {
		list_del(&qdata->entry);
		if (qdata->callback_fn)
			qdata->callback_fn(qdata->callback_data, err);
		kfree(qdata);
	}
	spin_lock_irqsave(&h->lock, flags);
	h->flags &= ~ALUA_PG_RUNNING;
	spin_unlock_irqrestore(&h->lock, flags);
	scsi_device_put(sdev);

	return;

queue_rtpg:
	queue_delayed_work(kaluad_wq, &h->rtpg_work, sdev->alua->interval * HZ);
}

/**
 * alua_rtpg_queue() - cause RTPG to be submitted asynchronously
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
	unsigned long flags;

	if (scsi_device_get(sdev))
		return false;

	spin_lock_irqsave(&h->lock, flags);
	if (qdata) {
		list_add_tail(&qdata->entry, &h->rtpg_list);
		h->flags |= ALUA_PG_RUN_STPG;
		force = true;
	}
	if (!(h->flags & ALUA_PG_RUN_RTPG) && force) {
		h->flags |= ALUA_PG_RUN_RTPG;
		/* Do not queue if the worker is already running */
		if (!(h->flags & ALUA_PG_RUNNING))
			start_queue = 1;
	}

	spin_unlock_irqrestore(&h->lock, flags);
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
	tpgs = scsi_alua_check_tpgs(sdev);
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
	unsigned long flags;

	if ((sscanf(params, "%u", &argc) != 1) || (argc != 1))
		return -EINVAL;

	while (*p++)
		;
	if ((sscanf(p, "%u", &optimize) != 1) || (optimize > 1))
		return -EINVAL;

	spin_lock_irqsave(&h->lock, flags);
	if (optimize)
		h->flags |= ALUA_OPTIMIZE_STPG;
	else
		h->flags &= ~ALUA_OPTIMIZE_STPG;
	spin_unlock_irqrestore(&h->lock, flags);

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

	qdata = kzalloc_obj(*qdata);
	if (!qdata) {
		err = SCSI_DH_RES_TEMP_UNAVAIL;
		goto out;
	}
	qdata->callback_fn = fn;
	qdata->callback_data = data;

	if (alua_rtpg_queue(sdev, qdata, true)) {
		fn = NULL;
	} else {
		kfree(qdata);
		err = SCSI_DH_DEV_OFFLINED;
	}
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

	h = kzalloc_obj(*h);
	if (!h)
		return SCSI_DH_NOMEM;
	spin_lock_init(&h->lock);
	h->init_error = SCSI_DH_OK;
	h->sdev = sdev;
	INIT_DELAYED_WORK(&h->rtpg_work, alua_rtpg_work);
	INIT_LIST_HEAD(&h->rtpg_list);

	mutex_init(&h->init_mutex);

	if (optimize_stpg)
		h->flags |= ALUA_OPTIMIZE_STPG;

	sdev->handler_data = h;
	err = alua_initialize(sdev, h);
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
