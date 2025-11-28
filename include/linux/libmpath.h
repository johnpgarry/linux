
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/cdev.h>
#include <linux/device.h>

struct mpath_device;

enum mpath_access_state {
	MPATH_STATE_OPTIMAL,
	MPATH_STATE_ACTIVE,
	MPATH_STATE_INVALID	= 0xFF
};

enum mpath_iopolicy {
	MPATH_IOPOLICY_NUMA,
	MPATH_IOPOLICY_RR,
	MPATH_IOPOLICY_QD,
};

struct mpath_disk {
	struct srcu_struct 	srcu;
	struct list_head	dev_list;	/* list of all mpath_sdevs */
	struct gendisk		*gd;
	enum mpath_iopolicy	iopolicy;
	bool (*mpath_is_disabled)(struct mpath_device *);
	bool (*mpath_is_optimized)(struct mpath_device *mpath_device);
	struct mpath_device __rcu *current_path[]; /* scsi_device of current path */
};

struct mpath_device {
	struct mpath_disk *mpath_disk;
	struct list_head siblings;
	enum mpath_access_state	state;
	int				numa_node; /* NUMA node for Path  */
};

#define cdev_to_mpath_disk(cdev) container_of(cdev, struct mpath_disk, cdev)

bool mpath_clear_current_path(struct mpath_device *);
struct mpath_device *mpath_find_path(struct mpath_disk *mpath_disk);
struct mpath_device *__mpath_find_path(struct mpath_disk *mpath_disk, int node);

#endif // _LIBMULTIPATH_H
