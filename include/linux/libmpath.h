
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/bio.h>
#include <linux/blkdev.h>
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
	struct device		dev;
	struct kref		ref;
	struct	bio_list	requeue_list; /* list for requeing bio */
	spinlock_t		requeue_lock;
	struct work_struct	requeue_work; /* work struct for requeue */
	bool (*is_disabled)(struct mpath_device *);
	bool (*is_optimized)(struct mpath_device *);
	int (*get_unique_id)(struct mpath_device *, u8 id[16], enum blk_unique_id type);
	int (*ioctl)(struct mpath_device *, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg);
	struct mpath_device __rcu *current_path[]; /* scsi_device of current path */
};

struct mpath_device {
	struct mpath_disk *mpath_disk;
	struct list_head siblings;
	enum mpath_access_state	state;
	atomic_t nr_active;
	atomic_t nr_total;
	struct gendisk *gd;
	int				numa_node; /* NUMA node for Path  */
};

#define REQ_MPATH		REQ_DRV


#define cdev_to_mpath_disk(cdev) container_of(cdev, struct mpath_disk, cdev)

bool mpath_clear_current_path(struct mpath_device *);
struct mpath_device *mpath_find_path(struct mpath_disk *mpath_disk);
struct mpath_device *__mpath_find_path(struct mpath_disk *mpath_disk, int node);
void mpath_requeue_work(struct work_struct *work);
void mpath_revalidate_path(struct gendisk *disk, sector_t capacity);
void multipath_submit_bio(struct bio *bio);
ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device, char *buf);
void mpath_put_disk(struct mpath_disk *mpath_disk);
int mpath_get_disk(struct mpath_disk *mpath_disk);

extern struct device_attribute mpath_iopolicy;
extern const struct block_device_operations mpath_ops;

#endif // _LIBMULTIPATH_H
