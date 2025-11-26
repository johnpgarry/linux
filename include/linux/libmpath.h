
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/cdev.h>
#include <linux/device.h>

struct mpath_disk {
	struct srcu_struct 	srcu;
};

#define cdev_to_mpath_disk(cdev) container_of(cdev, struct mpath_disk, cdev)

#endif // _LIBMULTIPATH_H
