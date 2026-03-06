// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 */

#include <linux/backing-dev.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <trace/events/block.h>
#include "nvme.h"

bool multipath = true;
static bool multipath_always_on;

static int multipath_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	if (ret)
		return ret;

	if (multipath_always_on && !*arg) {
		pr_err("Can't disable multipath when multipath_always_on is configured.\n");
		*arg = true;
		return -EINVAL;
	}

	return 0;
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = multipath_param_set,
	.get = param_get_bool,
};

module_param_cb(multipath, &multipath_param_ops, &multipath, 0444);
MODULE_PARM_DESC(multipath,
	"turn on native support for multiple controllers per subsystem");

static int multipath_always_on_set(const char *val,
		const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	if (ret < 0)
		return ret;

	if (*arg)
		multipath = true;

	return 0;
}

static const struct kernel_param_ops multipath_always_on_ops = {
	.set = multipath_always_on_set,
	.get = param_get_bool,
};

module_param_cb(multipath_always_on, &multipath_always_on_ops,
		&multipath_always_on, 0444);
MODULE_PARM_DESC(multipath_always_on,
	"create multipath node always except for private namespace with non-unique nsid; note that this also implicitly enables native multipath support");


static int iopolicy = MPATH_IOPOLICY_NUMA;

static int nvme_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	return mpath_set_iopolicy(val, &iopolicy);
}

static int nvme_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return mpath_get_iopolicy(buf, iopolicy);
}

module_param_call(iopolicy, nvme_set_iopolicy, nvme_get_iopolicy,
	&iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");

void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys)
{
	subsys->iopolicy.iopolicy = iopolicy;
}

void nvme_mpath_unfreeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (h->mpath_head)
			blk_mq_unfreeze_queue_nomemrestore(
				h->mpath_head->disk->queue);
	}
}

void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (h->mpath_head)
			blk_mq_freeze_queue_wait(h->mpath_head->disk->queue);
	}
}

void nvme_mpath_start_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (h->mpath_head)
			blk_freeze_queue_start(h->mpath_head->disk->queue);
	}
}

void nvme_failover_req(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata;
	struct mpath_head *mpath_head = ns->head->mpath_head;
	u16 status = nvme_req(req)->status & NVME_SCT_SC_MASK;
	unsigned long flags;
	struct bio *bio;

	nvme_mpath_clear_current_path(ns);

	/*
	 * If we got back an ANA error, we know the controller is alive but not
	 * ready to serve this namespace.  Kick of a re-read of the ANA
	 * information page, and just try any other available path for now.
	 */
	if (nvme_is_ana_error(status) && ns->ctrl->ana_log_buf) {
		set_bit(NVME_NS_ANA_PENDING, &ns->flags);
		queue_work(nvme_wq, &ns->ctrl->ana_work);
	}

	spin_lock_irqsave(&mpath_head->requeue_lock, flags);
	for (bio = req->bio; bio; bio = bio->bi_next) {
		bio_set_dev(bio, mpath_head->disk->part0);
		if (bio->bi_opf & REQ_POLLED) {
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;
		}
		/*
		 * The alternate request queue that we may end up submitting
		 * the bio to may be frozen temporarily, in this case REQ_NOWAIT
		 * will fail the I/O immediately with EAGAIN to the issuer.
		 * We are not in the issuer context which cannot block. Clear
		 * the flag to avoid spurious EAGAIN I/O failures.
		 */
		bio->bi_opf &= ~REQ_NOWAIT;
	}
	blk_steal_bios(&mpath_head->requeue_list, req);
	spin_unlock_irqrestore(&mpath_head->requeue_lock, flags);

	nvme_req(req)->status = 0;
	nvme_end_req(req);
	mpath_schedule_requeue_work(mpath_head);
}

void nvme_mpath_start_request(struct request *rq)
{
	struct nvme_ns *ns = rq->q->queuedata;
	struct gendisk *disk = ns->head->mpath_head->disk;

	if (mpath_qd_iopolicy(&ns->head->subsys->iopolicy) &&
	    !(nvme_req(rq)->flags & NVME_MPATH_CNT_ACTIVE)) {
		atomic_inc(&ns->ctrl->nr_active);
		nvme_req(rq)->flags |= NVME_MPATH_CNT_ACTIVE;
	}

	if (!blk_queue_io_stat(disk->queue) || blk_rq_is_passthrough(rq) ||
	    (nvme_req(rq)->flags & NVME_MPATH_IO_STATS))
		return;

	nvme_req(rq)->flags |= NVME_MPATH_IO_STATS;
	nvme_req(rq)->start_time = bdev_start_io_acct(disk->part0, req_op(rq),
						      jiffies);
}
EXPORT_SYMBOL_GPL(nvme_mpath_start_request);

void nvme_mpath_end_request(struct request *rq)
{
	struct nvme_ns *ns = rq->q->queuedata;

	if (nvme_req(rq)->flags & NVME_MPATH_CNT_ACTIVE)
		atomic_dec_if_positive(&ns->ctrl->nr_active);

	if (!(nvme_req(rq)->flags & NVME_MPATH_IO_STATS))
		return;
	bdev_end_io_acct(ns->head->mpath_head->disk->part0, req_op(rq),
			 blk_rq_bytes(rq) >> SECTOR_SHIFT,
			 nvme_req(rq)->start_time);
}

bool nvme_mpath_head_queue_if_no_path(struct nvme_ns_head *head)
{
	return mpath_head_queue_if_no_path(head->mpath_head);
}

void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&ctrl->srcu);
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu)) {
		if (!ns->head->mpath_head)
			continue;
		mpath_schedule_requeue_work(ns->head->mpath_head);
		if (nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE)
			disk_uevent(ns->head->mpath_head->disk, KOBJ_CHANGE);
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);
}

static const char *nvme_ana_state_names[] = {
	[0]				= "invalid state",
	[NVME_ANA_OPTIMIZED]		= "optimized",
	[NVME_ANA_NONOPTIMIZED]		= "non-optimized",
	[NVME_ANA_INACCESSIBLE]		= "inaccessible",
	[NVME_ANA_PERSISTENT_LOSS]	= "persistent-loss",
	[NVME_ANA_CHANGE]		= "change",
};

bool nvme_mpath_clear_current_path(struct nvme_ns *ns)
{
	return mpath_clear_current_path(&ns->mpath_device);
}

void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&ctrl->srcu);
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu)) {
		nvme_mpath_clear_current_path(ns);
		mpath_schedule_requeue_work(ns->head->mpath_head);
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);
}

static void nvme_mpath_revalidate_paths_cb(struct mpath_device *mpath_device,
					sector_t capacity)
{
	struct nvme_ns *ns = nvme_mpath_to_ns(mpath_device);

	if (capacity != get_capacity(ns->disk))
		clear_bit(NVME_NS_READY, &ns->flags);
}

void nvme_mpath_revalidate_paths(struct nvme_ns_head *head)
{
	mpath_revalidate_paths(head->mpath_head, nvme_mpath_revalidate_paths_cb);
}

static bool nvme_path_is_disabled(struct nvme_ns *ns)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ns->ctrl);

	/*
	 * We don't treat NVME_CTRL_DELETING as a disabled path as I/O should
	 * still be able to complete assuming that the controller is connected.
	 * Otherwise it will fail immediately and return to the requeue list.
	 */
	if (state != NVME_CTRL_LIVE && state != NVME_CTRL_DELETING)
		return true;
	if (test_bit(NVME_NS_ANA_PENDING, &ns->flags) ||
	    !test_bit(NVME_NS_READY, &ns->flags))
		return true;
	return false;
}

static bool nvme_mpath_is_disabled(struct mpath_device *mpath_device)
{
	struct nvme_ns *ns = nvme_mpath_to_ns(mpath_device);

	return nvme_path_is_disabled(ns);
}

static inline bool nvme_path_is_optimized(struct nvme_ns *ns)
{
	return nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE &&
		ns->ana_state == NVME_ANA_OPTIMIZED;
}

static bool nvme_mpath_is_optimized(struct mpath_device *mpath_device)
{
	struct nvme_ns *ns = nvme_mpath_to_ns(mpath_device);

	return nvme_path_is_optimized(ns);
}

static bool nvme_mpath_available_path(struct mpath_device *mpath_device)
{
	struct nvme_ns *ns = nvme_mpath_to_ns(mpath_device);

	if (test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ns->ctrl->flags))
		return false;

	switch (nvme_ctrl_state(ns->ctrl)) {
	case NVME_CTRL_LIVE:
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return true;
	default:
		break;
	}

	return false;
}

static int nvme_mpath_get_unique_id(struct mpath_device *mpath_device,
		u8 id[16], enum blk_unique_id type)
{
	return nvme_ns_get_unique_id(nvme_mpath_to_ns(mpath_device), id, type);
}
#ifdef CONFIG_BLK_DEV_ZONED
static int nvme_mpath_report_zones(struct mpath_device *mpath_device,
		sector_t sector, unsigned int nr_zones,
		struct blk_report_zones_args *args)
{
	return nvme_ns_report_zones(nvme_mpath_to_ns(mpath_device), sector,
				nr_zones, args);
}
#else
#define nvme_mpath_report_zones		NULL
#endif /* CONFIG_BLK_DEV_ZONED */

static int nvme_mpath_add_cdev(struct mpath_head *mpath_head)
{
	struct nvme_ns_head *head = mpath_head->drvdata;
	int ret;

	mpath_head->cdev_device.parent = &head->subsys->dev;
	ret = dev_set_name(&mpath_head->cdev_device, "ng%dn%d",
			   head->subsys->instance, head->instance);
	if (ret)
		return ret;
	ret = nvme_cdev_add(&mpath_head->cdev, &mpath_head->cdev_device,
			    &mpath_generic_chr_fops, THIS_MODULE);
	return ret;
}

static void nvme_mpath_del_cdev(struct mpath_head *mpath_head)
{
	nvme_cdev_del(&mpath_head->cdev, &mpath_head->cdev_device);
}

bool nvme_mpath_has_disk(struct nvme_ns_head *head)
{
	return head->mpath_head;
}

static void nvme_remove_head_work(struct work_struct *work)
{
	struct mpath_head *mpath_head = container_of(to_delayed_work(work),
			struct mpath_head, remove_work);
	struct nvme_ns_head *head = mpath_head->drvdata;
	bool remove = false;
	struct kref *ref_head;
	struct kref *ref_mpath_head;

	ref_head = &head->ref;
	ref_mpath_head = &mpath_head->ref;
	mutex_lock(&head->subsys->lock);
	pr_err("%s mpath_head=%pS head=%pS head->ns_count=%d\n",
		__func__, mpath_head, head, head->ns_count);
	if (!head->ns_count) {
		list_del_init(&head->entry);
		remove = true;
	}
	mutex_unlock(&head->subsys->lock);

	pr_err("%s8 head=%pS refcount=%d remove=%d\n",
		__func__,
			head, refcount_read(&ref_head->refcount),
			remove);
	pr_err("%s8.1 mpath_head=%pS refcount=%d\n",
		__func__,
			mpath_head, refcount_read(&ref_mpath_head->refcount));
	if (remove) {
		mpath_unregister_disk(mpath_head);
		nvme_put_ns_head(head);
	}
	module_put(THIS_MODULE);
}

static int nvme_parse_ana_log(struct nvme_ctrl *ctrl, void *data,
		int (*cb)(struct nvme_ctrl *ctrl, struct nvme_ana_group_desc *,
			void *))
{
	void *base = ctrl->ana_log_buf;
	size_t offset = sizeof(struct nvme_ana_rsp_hdr);
	int error, i;

	lockdep_assert_held(&ctrl->ana_lock);

	for (i = 0; i < le16_to_cpu(ctrl->ana_log_buf->ngrps); i++) {
		struct nvme_ana_group_desc *desc = base + offset;
		u32 nr_nsids;
		size_t nsid_buf_size;

		if (WARN_ON_ONCE(offset > ctrl->ana_log_size - sizeof(*desc)))
			return -EINVAL;

		nr_nsids = le32_to_cpu(desc->nnsids);
		nsid_buf_size = flex_array_size(desc, nsids, nr_nsids);

		if (WARN_ON_ONCE(desc->grpid == 0))
			return -EINVAL;
		if (WARN_ON_ONCE(le32_to_cpu(desc->grpid) > ctrl->anagrpmax))
			return -EINVAL;
		if (WARN_ON_ONCE(desc->state == 0))
			return -EINVAL;
		if (WARN_ON_ONCE(desc->state > NVME_ANA_CHANGE))
			return -EINVAL;

		offset += sizeof(*desc);
		if (WARN_ON_ONCE(offset > ctrl->ana_log_size - nsid_buf_size))
			return -EINVAL;

		error = cb(ctrl, desc, data);
		if (error)
			return error;

		offset += nsid_buf_size;
	}

	return 0;
}

static inline bool nvme_state_is_live(enum nvme_ana_state state)
{
	return state == NVME_ANA_OPTIMIZED || state == NVME_ANA_NONOPTIMIZED;
}

static void nvme_mpath_update_ana_state(struct nvme_ns *ns, enum nvme_ana_state ana_state)
{
	ns->ana_state = ana_state;
	if (ana_state == NVME_ANA_OPTIMIZED)
		ns->mpath_device.access_state = MPATH_STATE_OPTIMIZED;
	else if (ana_state == NVME_ANA_NONOPTIMIZED)
		ns->mpath_device.access_state = MPATH_STATE_ACTIVE;
	else
		ns->mpath_device.access_state = MPATH_STATE_OTHER;
}

static void nvme_update_ns_ana_state(struct nvme_ana_group_desc *desc,
		struct nvme_ns *ns)
{
	struct mpath_head *mpath_head = ns->head->mpath_head;

	ns->ana_grpid = le32_to_cpu(desc->grpid);
	nvme_mpath_update_ana_state(ns, desc->state);
	clear_bit(NVME_NS_ANA_PENDING, &ns->flags);

	/*
	 * nvme_mpath_set_live() will trigger I/O to the multipath path device
	 * and in turn to this path device.  However we cannot accept this I/O
	 * if the controller is not live.  This may deadlock if called from
	 * nvme_mpath_init_identify() and the ctrl will never complete
	 * initialization, preventing I/O from completing.  For this case we
	 * will reprocess the ANA log page in nvme_mpath_update() once the
	 * controller is ready.
	 */
	if (nvme_state_is_live(ns->ana_state) &&
	    nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE)
		mpath_device_set_live(&ns->mpath_device);
	else {
		/*
		 * Add sysfs link from multipath head gendisk node to path
		 * device gendisk node.
		 * If path's ana state is live (i.e. state is either optimized
		 * or non-optimized) while we alloc the ns then sysfs link would
		 * be created from nvme_mpath_set_live(). In that case we would
		 * not fallthrough this code path. However for the path's ana
		 * state other than live, we call nvme_mpath_set_live() only
		 * after ana state transitioned to the live state. But we still
		 * want to create the sysfs link from head node to a path device
		 * irrespctive of the path's ana state.
		 * If we reach through here then it means that path's ana state
		 * is not live but still create the sysfs link to this path from
		 * head node if head node of the path has already come alive.
		 */
		if (test_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags))
			mpath_add_sysfs_link(mpath_head);
	}
}

void nvme_mpath_synchronize(struct nvme_ns_head *head)
{
	mpath_synchronize(head->mpath_head);
}

void nvme_mpath_add_ns(struct nvme_ns *ns)
{
	ns->mpath_device.disk = ns->disk;
	mpath_add_device(ns->head->mpath_head, &ns->mpath_device);
}

void nvme_mpath_delete_ns(struct nvme_ns *ns)
{
	mpath_delete_device(&ns->mpath_device);
}

void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns)
{
	mpath_remove_sysfs_link(&ns->mpath_device);
}

static int nvme_update_ana_state(struct nvme_ctrl *ctrl,
		struct nvme_ana_group_desc *desc, void *data)
{
	u32 nr_nsids = le32_to_cpu(desc->nnsids), n = 0;
	unsigned *nr_change_groups = data;
	struct nvme_ns *ns;
	int srcu_idx;

	dev_dbg(ctrl->device, "ANA group %d: %s.\n",
			le32_to_cpu(desc->grpid),
			nvme_ana_state_names[desc->state]);

	if (desc->state == NVME_ANA_CHANGE)
		(*nr_change_groups)++;

	if (!nr_nsids)
		return 0;

	srcu_idx = srcu_read_lock(&ctrl->srcu);
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu)) {
		unsigned nsid;
again:
		nsid = le32_to_cpu(desc->nsids[n]);
		if (ns->head->ns_id < nsid)
			continue;
		if (ns->head->ns_id == nsid)
			nvme_update_ns_ana_state(desc, ns);
		if (++n == nr_nsids)
			break;
		if (ns->head->ns_id > nsid)
			goto again;
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);
	return 0;
}

static int nvme_read_ana_log(struct nvme_ctrl *ctrl)
{
	u32 nr_change_groups = 0;
	int error;

	mutex_lock(&ctrl->ana_lock);
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_ANA, 0, NVME_CSI_NVM,
			ctrl->ana_log_buf, ctrl->ana_log_size, 0);
	if (error) {
		dev_warn(ctrl->device, "Failed to get ANA log: %d\n", error);
		goto out_unlock;
	}

	error = nvme_parse_ana_log(ctrl, &nr_change_groups,
			nvme_update_ana_state);
	if (error)
		goto out_unlock;

	/*
	 * In theory we should have an ANATT timer per group as they might enter
	 * the change state at different times.  But that is a lot of overhead
	 * just to protect against a target that keeps entering new changes
	 * states while never finishing previous ones.  But we'll still
	 * eventually time out once all groups are in change state, so this
	 * isn't a big deal.
	 *
	 * We also double the ANATT value to provide some slack for transports
	 * or AEN processing overhead.
	 */
	if (nr_change_groups)
		mod_timer(&ctrl->anatt_timer, ctrl->anatt * HZ * 2 + jiffies);
	else
		timer_delete_sync(&ctrl->anatt_timer);
out_unlock:
	mutex_unlock(&ctrl->ana_lock);
	return error;
}

static void nvme_ana_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(work, struct nvme_ctrl, ana_work);

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
		return;

	nvme_read_ana_log(ctrl);
}

void nvme_mpath_update(struct nvme_ctrl *ctrl)
{
	u32 nr_change_groups = 0;

	if (!ctrl->ana_log_buf)
		return;

	mutex_lock(&ctrl->ana_lock);
	nvme_parse_ana_log(ctrl, &nr_change_groups, nvme_update_ana_state);
	mutex_unlock(&ctrl->ana_lock);
}

static void nvme_anatt_timeout(struct timer_list *t)
{
	struct nvme_ctrl *ctrl = timer_container_of(ctrl, t, anatt_timer);

	dev_info(ctrl->device, "ANATT timeout, resetting controller.\n");
	nvme_reset_ctrl(ctrl);
}

void nvme_mpath_stop(struct nvme_ctrl *ctrl)
{
	if (!nvme_ctrl_use_ana(ctrl))
		return;
	timer_delete_sync(&ctrl->anatt_timer);
	cancel_work_sync(&ctrl->ana_work);
}

#define SUBSYS_ATTR_RW(_name, _mode, _show, _store)  \
	struct device_attribute subsys_attr_##_name =	\
		__ATTR(_name, _mode, _show, _store)

static ssize_t nvme_subsys_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);

	return mpath_iopolicy_show(&subsys->iopolicy, buf);
}

static ssize_t nvme_subsys_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);
	struct nvme_ctrl *ctrl;

	if (!mpath_iopolicy_store(&subsys->iopolicy, buf, count))
		return  -EINVAL;
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		nvme_mpath_clear_ctrl_paths(ctrl);
	mutex_unlock(&nvme_subsystems_lock);
	return count;
}
SUBSYS_ATTR_RW(iopolicy, S_IRUGO | S_IWUSR,
		      nvme_subsys_iopolicy_show, nvme_subsys_iopolicy_store);

static ssize_t ana_grpid_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%d\n", nvme_get_ns_from_dev(dev)->ana_grpid);
}
DEVICE_ATTR_RO(ana_grpid);

static ssize_t ana_state_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);

	return sysfs_emit(buf, "%s\n", nvme_ana_state_names[ns->ana_state]);
}
DEVICE_ATTR_RO(ana_state);

static ssize_t queue_depth_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);

	if (!mpath_qd_iopolicy(&ns->head->subsys->iopolicy))
		return 0;

	return sysfs_emit(buf, "%d\n", atomic_read(&ns->ctrl->nr_active));
}
DEVICE_ATTR_RO(queue_depth);

static ssize_t numa_nodes_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);

	return mpath_numa_nodes_show(&ns->mpath_device,
		&ns->head->subsys->iopolicy, buf);
}
DEVICE_ATTR_RO(numa_nodes);

static ssize_t delayed_removal_secs_show(struct device *bd_device,
		struct device_attribute *attr, char *buf)
{
	return mpath_delayed_removal_secs_show(
		mpath_bd_device_to_disk(bd_device), buf);
}

static ssize_t delayed_removal_secs_store(struct device *bd_device,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return mpath_delayed_removal_secs_store(
		mpath_bd_device_to_disk(bd_device), buf, count);
}

DEVICE_ATTR_RW(delayed_removal_secs);

static int nvme_lookup_ana_group_desc(struct nvme_ctrl *ctrl,
		struct nvme_ana_group_desc *desc, void *data)
{
	struct nvme_ana_group_desc *dst = data;

	if (desc->grpid != dst->grpid)
		return 0;

	*dst = *desc;
	return -ENXIO; /* just break out of the loop */
}

void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid)
{
	struct nvme_ns_head *head = ns->head;
	pr_err("%s head=%pS head->mpath_head=%pS\n", __func__, head, head->mpath_head);

	if (nvme_ctrl_use_ana(ns->ctrl)) {
		struct nvme_ana_group_desc desc = {
			.grpid = anagrpid,
			.state = 0,
		};

		mutex_lock(&ns->ctrl->ana_lock);
		ns->ana_grpid = le32_to_cpu(anagrpid);
		nvme_parse_ana_log(ns->ctrl, &desc, nvme_lookup_ana_group_desc);
		mutex_unlock(&ns->ctrl->ana_lock);
		if (desc.state) {
			/* found the group desc: update */
			nvme_update_ns_ana_state(&desc, ns);
		} else {
			/* group desc not found: trigger a re-read */
			set_bit(NVME_NS_ANA_PENDING, &ns->flags);
			queue_work(nvme_wq, &ns->ctrl->ana_work);
		}
	} else {
		nvme_mpath_update_ana_state(ns, NVME_ANA_OPTIMIZED);
		mpath_device_set_live(&ns->mpath_device);
	}

#ifdef CONFIG_BLK_DEV_ZONED
	if (blk_queue_is_zoned(ns->queue) && head->mpath_head)
		head->mpath_head->disk->nr_zones = ns->disk->nr_zones;
#endif
}

void nvme_mpath_remove_disk(struct nvme_ns_head *head)
{
	struct mpath_head *mpath_head = head->mpath_head;
	struct kref *ref_head;
	struct kref *ref_mpath_head;
	bool remove = false;

	pr_err("%s head=%pS mpath_head=%pS\n",
		__func__, head, mpath_head);
	if (!mpath_head)
		return;

	ref_head = &head->ref;
	ref_mpath_head = &mpath_head->ref;

	mutex_lock(&head->subsys->lock);
	pr_err("%s head=%pS refcount=%d mpath_head=%pS refcount=%d\n",
		__func__,
			head, refcount_read(&ref_head->refcount),
			mpath_head, refcount_read(&ref_mpath_head->refcount));
	/*
	 * We are called when all paths have been removed, and at that point
	 * head->list is expected to be empty. However, nvme_ns_remove() and
	 * nvme_init_ns_head() can run concurrently and so if head->delayed_
	 * removal_secs is configured, it is possible that by the time we reach
	 * this point, head->list may no longer be empty. Therefore, we recheck
	 * head->list here. If it is no longer empty then we skip enqueuing the
	 * delayed head removal work.
	 */
	pr_err("%s1 head=%pS head->ns_count=%d\n",
		__func__,
			head, head->ns_count);
	if (head->ns_count)
		goto out;

	if (mpath_can_remove_head(mpath_head)) {
		pr_err("%s2 head=%pS mpath_can_remove_head returned true\n",
		__func__, head);
		list_del_init(&head->entry);
		remove = true;
	}
out:
	mutex_unlock(&head->subsys->lock);
	pr_err("%s8 head=%pS refcount=%d remove=%d\n",
		__func__,
			head, refcount_read(&ref_head->refcount),
			remove);
	pr_err("%s8.1 mpath_head=%pS refcount=%d\n",
		__func__,
			mpath_head, refcount_read(&ref_mpath_head->refcount));
	if (remove) {
		mpath_unregister_disk(mpath_head);
		nvme_put_ns_head(head);
	}
}

void nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl)
{
	mutex_init(&ctrl->ana_lock);
	timer_setup(&ctrl->anatt_timer, nvme_anatt_timeout, 0);
	INIT_WORK(&ctrl->ana_work, nvme_ana_work);
}

int nvme_mpath_init_identify(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	size_t max_transfer_size = ctrl->max_hw_sectors << SECTOR_SHIFT;
	size_t ana_log_size;
	int error = 0;

	/* check if multipath is enabled and we have the capability */
	if (!multipath || !ctrl->subsys ||
	    !(ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA))
		return 0;

	/* initialize this in the identify path to cover controller resets */
	atomic_set(&ctrl->nr_active, 0);

	if (!ctrl->max_namespaces ||
	    ctrl->max_namespaces > le32_to_cpu(id->nn)) {
		dev_err(ctrl->device,
			"Invalid MNAN value %u\n", ctrl->max_namespaces);
		return -EINVAL;
	}

	ctrl->anacap = id->anacap;
	ctrl->anatt = id->anatt;
	ctrl->nanagrpid = le32_to_cpu(id->nanagrpid);
	ctrl->anagrpmax = le32_to_cpu(id->anagrpmax);

	ana_log_size = sizeof(struct nvme_ana_rsp_hdr) +
		ctrl->nanagrpid * sizeof(struct nvme_ana_group_desc) +
		ctrl->max_namespaces * sizeof(__le32);
	if (ana_log_size > max_transfer_size) {
		dev_err(ctrl->device,
			"ANA log page size (%zd) larger than MDTS (%zd).\n",
			ana_log_size, max_transfer_size);
		dev_err(ctrl->device, "disabling ANA support.\n");
		goto out_uninit;
	}
	if (ana_log_size > ctrl->ana_log_size) {
		nvme_mpath_stop(ctrl);
		nvme_mpath_uninit(ctrl);
		ctrl->ana_log_buf = kvmalloc(ana_log_size, GFP_KERNEL);
		if (!ctrl->ana_log_buf)
			return -ENOMEM;
	}
	ctrl->ana_log_size = ana_log_size;
	error = nvme_read_ana_log(ctrl);
	if (error)
		goto out_uninit;
	return 0;

out_uninit:
	nvme_mpath_uninit(ctrl);
	return error;
}

void nvme_mpath_uninit(struct nvme_ctrl *ctrl)
{
	kvfree(ctrl->ana_log_buf);
	ctrl->ana_log_buf = NULL;
	ctrl->ana_log_size = 0;
}

static enum mpath_iopolicy_e nvme_mpath_get_iopolicy(struct mpath_head *mpath_head)
{
	struct nvme_ns_head *head = mpath_head->drvdata;
	struct nvme_subsystem *subsys = head->subsys;

	return mpath_read_iopolicy(&subsys->iopolicy);
}

static int nvme_mpath_get_nr_active(struct mpath_device *mpath_device)
{
	struct nvme_ns *ns = nvme_mpath_to_ns(mpath_device);

	return atomic_read(&ns->ctrl->nr_active);
}

static const struct mpath_head_template mpdt = {
	.available_path = nvme_mpath_available_path,
	.add_cdev = nvme_mpath_add_cdev,
	.del_cdev = nvme_mpath_del_cdev,
	.is_disabled = nvme_mpath_is_disabled,
	.is_optimized = nvme_mpath_is_optimized,
	.bdev_ioctl = nvme_mpath_bdev_ioctl,
	.cdev_ioctl = nvme_mpath_cdev_ioctl,
	.report_zones = nvme_mpath_report_zones,
	.pr_ops = &nvme_mpath_pr_ops,
	.chr_uring_cmd = nvme_mpath_chr_uring_cmd,
	.chr_uring_cmd_iopoll = nvme_ns_chr_uring_cmd_iopoll,
	.get_iopolicy = nvme_mpath_get_iopolicy,
	.get_unique_id = nvme_mpath_get_unique_id,
	.device_groups = nvme_ns_attr_groups,
	.get_nr_active = nvme_mpath_get_nr_active,
};

int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl, struct nvme_ns_head *head)
{
	struct mpath_head *mpath_head;
	struct nvme_subsystem *subsys = ctrl->subsys;
	struct queue_limits lim;
	int ret;

	/*
	 * If "multipath_always_on" is enabled, a multipath node is added
	 * regardless of whether the disk is single/multi ported, and whether
	 * the namespace is shared or private. If "multipath_always_on" is not
	 * enabled, a multipath node is added only if the subsystem supports
	 * multiple controllers and the "multipath" option is configured. In
	 * either case, for private namespaces, we ensure that the NSID is
	 * unique.
	 */
	if (!multipath_always_on) {
		if (!(ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) ||
				!multipath)
			return 0;
	}

	if (!nvme_is_unique_nsid(ctrl, head))
		return 0;

	blk_set_stacking_limits(&lim);
	lim.dma_alignment = 3;
	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT |
		BLK_FEAT_POLL | BLK_FEAT_ATOMIC_WRITES;
	if (head->ids.csi == NVME_CSI_ZNS)
		lim.features |= BLK_FEAT_ZONED;

	mpath_head = mpath_alloc_head();
	if (IS_ERR(mpath_head))
		return PTR_ERR(mpath_head);

	ret = mpath_alloc_head_disk(mpath_head, &lim, ctrl->numa_node);
	if (ret) {
		mpath_put_head(mpath_head);
		return ret;
	}

	mpath_head->drvdata = head;
	head->mpath_head = mpath_head;
	mpath_head->parent = &subsys->dev;
	mpath_head->mpdt = &mpdt;
	INIT_DELAYED_WORK(&mpath_head->remove_work, nvme_remove_head_work);

	sprintf(mpath_head->disk->disk_name, "nvme%dn%d",
			ctrl->subsys->instance, head->instance);
	nvme_tryget_ns_head(head);
	return 0;
}
