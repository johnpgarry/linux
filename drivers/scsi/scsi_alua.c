//EXPORT_SYMBOL_NS_GPL(alua_bus_attach, "SCSI_DH_ALUA");

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include "scsi_alua.h"

#define DRV_NAME "scsi_alua"

static struct workqueue_struct *kaluad_wq;

static LIST_HEAD(port_group_list);
static DEFINE_SPINLOCK(port_group_lock);

static void scsi_release_port_group(struct kref *kref)
{
	struct scsi_alua_port_group *pg;

	pg = container_of(kref, struct scsi_alua_port_group, kref);
	if (pg->rtpg_sdev)
		flush_delayed_work(&pg->rtpg_work);
	spin_lock(&port_group_lock);
	list_del(&pg->node);
	spin_unlock(&port_group_lock);
	kfree_rcu(pg, rcu);
}

/*
 * alua_check_tpgs - Evaluate TPGS setting
 * @sdev: device to be checked
 *
 * Examine the TPGS setting of the sdev to find out if ALUA
 * is supported.
 */
int scsi_alua_check_tpgs(struct scsi_device *sdev)
{
	int tpgs = TPGS_MODE_NONE;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
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
bool scsi_alua_rtpg_queue(struct scsi_alua_port_group *pg,
			    struct scsi_device *sdev,
			    struct scsi_alua_queue_data *qdata, bool force)
{
	int start_queue = 0;
	unsigned long flags;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	if (WARN_ON_ONCE(!pg) || scsi_device_get(sdev))
		return false;

	spin_lock_irqsave(&pg->lock, flags);
	if (qdata) {
		list_add_tail(&qdata->entry, &pg->rtpg_list);
		pg->flags |= ALUA_PG_RUN_STPG;
		force = true;
	}
	if (pg->rtpg_sdev == NULL) {
	//	struct alua_dh_data *h = sdev->handler_data;

		rcu_read_lock();
	//	if (h && rcu_dereference(h->pg) == pg) {
	//		pg->interval = 0;
	//		pg->flags |= ALUA_PG_RUN_RTPG;
	//		kref_get(&pg->kref);
	//		pg->rtpg_sdev = sdev;
	//		start_queue = 1;
	//	}
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

	if (start_queue) {
		if (queue_delayed_work(kaluad_wq, &pg->rtpg_work,
				msecs_to_jiffies(ALUA_RTPG_DELAY_MSECS)))
			sdev = NULL;
		else
			kref_put(&pg->kref, scsi_release_port_group);
	}
	if (sdev)
		scsi_device_put(sdev);

	return true;
}
