
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/cdev.h>
#include <linux/device.h>

struct mpath_device;

struct mpath_disk {
	struct srcu_struct 	srcu;
	struct list_head	dev_list;	/* list of all mpath_sdevs */
	struct mpath_device __rcu *current_path[]; /* scsi_device of current path */
};

struct mpath_device {
	struct mpath_disk *mpath_disk;
	struct list_head siblings;
};

#define cdev_to_mpath_disk(cdev) container_of(cdev, struct mpath_disk, cdev)

bool mpath_clear_current_path(struct mpath_device *);

#endif // _LIBMULTIPATH_H
