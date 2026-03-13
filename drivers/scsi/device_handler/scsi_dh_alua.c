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
	//int			group_id;
	int			tpgs;
	int			state;
	int			pref;
	int			valid_states;
	unsigned		flags; /* used for optimizing STPG */
	//unsigned char		transition_tmo;
	//unsigned long		expiry;
	//unsigned long		interval;
	struct delayed_work	rtpg_work;
	spinlock_t		lock;
	struct list_head	rtpg_list;
	struct scsi_device	*rtpg_sdev;
};
#endif

struct alua_dh_data {
	//int			group_id;
	struct scsi_device	*sdev;
	int			init_error;
	struct mutex		init_mutex;
	bool			disabled;


	unsigned		flags; /* used for optimizing STPG */

	/* alua stuff */
	//unsigned char		transition_tmo;
	//unsigned long		expiry;
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

static void alua_dh_rtpg_work(struct work_struct *work);
static bool alua_dh_rtpg_queue(struct scsi_device *sdev,
			    struct alua_queue_data *qdata, bool force);
static void alua_dh_check(struct scsi_device *sdev);

/*
 * alua_dh_check_vpd - Evaluate INQUIRY vpd page 0x83
 * @sdev: device to be checked
 *
 * Extract the relative target port and the target port group
 * descriptor from the list of identificators.
 */
static int alua_dh_check_vpd(struct scsi_device *sdev, struct alua_dh_data *h,
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

	alua_dh_rtpg_queue(sdev, NULL, true);

	return SCSI_DH_OK;
}

static enum scsi_disposition alua_dh_check_sense(struct scsi_device *sdev,
					      struct scsi_sense_hdr *sense_hdr)
{
	return alua_check_sense(sdev, sense_hdr, alua_dh_check);
}

static void alua_dh_rtpg_work(struct work_struct *work)
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
		err = alua_rtpg(sdev);

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
		
		err = alua_stpg(sdev, h->flags & ALUA_OPTIMIZE_STPG);
		
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
 * alua_dh_rtpg_queue() - cause RTPG to be submitted asynchronously
 * @pg: ALUA port group associated with @sdev.
 * @sdev: SCSI device for which to submit an RTPG.
 * @qdata: Information about the callback to invoke after the RTPG.
 * @force: Whether or not to submit an RTPG if a work item that will submit an
 *         RTPG already has been scheduled.
 *
 * Returns true if and only if alua_dh_rtpg_work() will be called asynchronously.
 * That function is responsible for calling @qdata->fn().
 *
 * Context: may be called from atomic context (alua_check()) only if the caller
 *	holds an sdev reference.
 */
static bool alua_dh_rtpg_queue(struct scsi_device *sdev,
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
 * alua_dh_initialize - Initialize ALUA state
 * @sdev: the device to be initialized
 *
 * For the prep_fn to work correctly we have
 * to initialize the ALUA state for the device.
 */
static int alua_dh_initialize(struct scsi_device *sdev, struct alua_dh_data *h)
{
	int err = SCSI_DH_DEV_UNSUPP, tpgs;

	mutex_lock(&h->init_mutex);
	h->disabled = false;
	tpgs = alua_check_tpgs(sdev);
	if (tpgs != TPGS_MODE_NONE)
		err = alua_dh_check_vpd(sdev, h, tpgs);
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
static int alua_dh_set_params(struct scsi_device *sdev, const char *params)
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
static int alua_dh_activate(struct scsi_device *sdev,
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
	if (alua_dh_rtpg_queue(sdev, qdata, true)) {
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
 * alua_dh_check - check path status
 * @sdev: device on the path to be checked
 *
 * Check the device status
 */
static void alua_dh_check(struct scsi_device *sdev)
{
	alua_dh_rtpg_queue(sdev, NULL, true);
}

static void alua_dh_rescan(struct scsi_device *sdev)
{
	struct alua_dh_data *h = sdev->handler_data;

	alua_dh_initialize(sdev, h);
}

/*
 * alua_dh_bus_attach - Attach device handler
 * @sdev: device to be attached to
 */
static int alua_dh_bus_attach(struct scsi_device *sdev)
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
	INIT_DELAYED_WORK(&h->rtpg_work, alua_dh_rtpg_work);
	INIT_LIST_HEAD(&h->rtpg_list);

	mutex_init(&h->init_mutex);
	sdev->handler_data = h;
	err = alua_dh_initialize(sdev, h);
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
 * alua_dh_bus_detach - Detach device handler
 * @sdev: device to be detached from
 */
static void alua_dh_bus_detach(struct scsi_device *sdev)
{
	struct alua_dh_data *h = sdev->handler_data;
	sdev->handler_data = NULL;
	kfree(h);
}

static struct scsi_device_handler alua_dh = {
	.name = ALUA_DH_NAME,
	.module = THIS_MODULE,
	.attach = alua_dh_bus_attach,
	.detach = alua_dh_bus_detach,
	.check_sense = alua_dh_check_sense,
	.activate = alua_dh_activate,
	.rescan = alua_dh_rescan,
	.set_params = alua_dh_set_params,
};

static int __init alua_dh_init(void)
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

static void __exit alua_dh_exit(void)
{
	scsi_unregister_device_handler(&alua_dh);
	destroy_workqueue(kaluad_wq);
}

module_init(alua_dh_init);
module_exit(alua_dh_exit);

MODULE_DESCRIPTION("DM Multipath ALUA support");
MODULE_AUTHOR("Hannes Reinecke <hare@suse.de>");
MODULE_LICENSE("GPL");
MODULE_VERSION(ALUA_DH_VER);
