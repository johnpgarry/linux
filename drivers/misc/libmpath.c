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
