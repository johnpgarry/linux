// SPDX-License-Identifier: GPL-2.0-only
/*
 */
#include <linux/bio.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <linux/libmpath.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dh.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_multipath.h>
#include <scsi/scsi_ioctl.h>


bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	struct mpath_disk *mpath_disk = mpath_device->mpath_disk;
	bool changed = false;
	int node;

	if (!mpath_disk)
		return changed;

	for_each_node(node) {
		if (mpath_device == rcu_access_pointer(mpath_disk->current_path[node])) {
			rcu_assign_pointer(mpath_disk->current_path[node], NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(mpath_clear_current_path);

/*
 * Search path based on iopolicy and numa node affinity
 * and return the scsi_device for that path
 */
struct mpath_device *__mpath_find_path(struct mpath_disk *mpath_disk, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	//struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;
	struct mpath_device *mpath_dev_found, *mpath_dev_fallback, *mpath_device;
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);

	//pr_err("%s mpath_disk=%pS\n", __func__, scsi_mpath_disk);
	list_for_each_entry_rcu(mpath_device, &mpath_disk->dev_list, siblings) {
		struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//	pr_err("%s1 itering mpath_disk=%pS mpath_dev=%pS disabled=%d\n",
	//		__func__, scsi_mpath_disk, scsi_mpath_dev, scsi_mpath_is_disabled(scsi_mpath_dev->sdev));
		if (mpath_disk->mpath_is_disabled(mpath_device))
			continue;

		if (scsi_mpath_dev->numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(scsi_mpath_disk->iopolicy) == SCSI_MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, scsi_mpath_dev->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(scsi_mpath_dev->state) {
		case SCSI_MPATH_OPTIMAL:
		    if (distance < found_distance) {
			    found_distance = distance;
			    mpath_dev_found = mpath_device;
		    }
		    break;
		case SCSI_MPATH_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    mpath_dev_fallback = mpath_device;
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
EXPORT_SYMBOL_GPL(__mpath_find_path);

static struct mpath_device *mpath_next_dev(struct mpath_disk *mpath_disk,
			struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_disk->dev_list, &mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_disk->dev_list, struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(struct mpath_disk *mpath_disk)
{
	struct mpath_device *mpath_device, *found = NULL;
	struct scsi_mpath_device *scsi_mpath_dev;
	int node = numa_node_id();
	struct mpath_device *old = srcu_dereference(mpath_disk->current_path[node],
					       &mpath_disk->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_disk, node);

	if (list_is_singular(&mpath_disk->dev_list)) {
		if(mpath_disk->mpath_is_disabled(mpath_device))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_disk, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_disk, mpath_device)) {

		if (mpath_disk->mpath_is_disabled(mpath_device))
			continue;
		if (scsi_mpath_dev->state == SCSI_MPATH_OPTIMAL) {
			found = mpath_device;
			goto out;
		}
		if (scsi_mpath_dev->state == SCSI_MPATH_ACTIVE)
			found = mpath_device;
	}

	scsi_mpath_dev = to_scsi_mpath_device(old);
	if (!mpath_disk->mpath_is_disabled(mpath_device) &&
	    (scsi_mpath_dev->state == SCSI_MPATH_OPTIMAL ||
	    (!found && scsi_mpath_dev->state == SCSI_MPATH_ACTIVE)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_disk->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(struct mpath_disk *mpath_disk)
{
	struct mpath_device *best_opt = NULL, *best_nonopt = NULL, *mpath_device;
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;
	unsigned int depth;

	pr_err("%s mpath_disk=%pS min_depth_nonopt=%d\n", __func__, mpath_disk, min_depth_nonopt);
	list_for_each_entry_srcu(mpath_device, &mpath_disk->dev_list, siblings,
				 srcu_read_lock_held(&mpath_disk->srcu)) {
		struct scsi_mpath_device *scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	//	if (nvme_path_is_disabled(ns))
	//		continue;

		depth = atomic_read(&scsi_mpath_dev->nr_active);

/*
		switch (ns->ana_state) {
		case NVME_ANA_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = ns;
			}
			break;
		case NVME_ANA_NONOPTIMIZED:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = ns;
			}
			break;
		default:
			break;
		}
*/
		if (min_depth_opt == 0)
			return best_opt;
	}

	return best_opt ? best_opt : best_nonopt;
}

static struct mpath_device *mpath_numa_path(struct mpath_disk *mpath_disk)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;
	struct scsi_mpath_device *scsi_mpath_dev;

	pr_err_once("%s mpath_disk=%pS\n", __func__, mpath_disk);
	mpath_device = srcu_dereference(mpath_disk->current_path[node], &mpath_disk->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_disk, node);
	scsi_mpath_dev = to_scsi_mpath_device(mpath_device);
	pr_err_once("%s1 mpath_disk=%pS mpath_device=%pS\n", __func__, mpath_disk, mpath_device);
	if (unlikely(!mpath_disk->mpath_is_optimized(mpath_device)))
		return __mpath_find_path(mpath_disk, node);
	return mpath_device;
}

struct mpath_device *mpath_find_path(struct mpath_disk *mpath_disk)
{
	struct scsi_mpath_disk *scsi_mpath_disk = to_scsi_mpath_disk(mpath_disk);
	pr_err_once("%s mpath_disk=%pS iopolicy=%d\n", __func__, scsi_mpath_disk, READ_ONCE(scsi_mpath_disk->iopolicy));
	switch (READ_ONCE(scsi_mpath_disk->iopolicy)) {
	case SCSI_MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_disk);
	case SCSI_MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_disk);
	default:
		return mpath_numa_path(mpath_disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_find_path);


static int __init mpath_init(void)
{
	pr_err("%s\n", __func__);
	
	return 0;
}

static void __exit mpath_exit(void)
{
	pr_err("%s\n", __func__);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmpath");
