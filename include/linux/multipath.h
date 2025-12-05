
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cdev.h>
#include <linux/device.h>


struct mpath_device;

enum mpath_access_state {
	MPATH_STATE_OPTIMIZED,
	MPATH_STATE_ACTIVE,
	MPATH_STATE_INVALID	= 0xFF
};

enum mpath_iopolicy {
	MPATH_IOPOLICY_NUMA,
	MPATH_IOPOLICY_RR,
	MPATH_IOPOLICY_QD,
};

/*
 * Mark bio as coming from scsi multipath node
 */
#define MPATH_DISK_LIVE            0

#define MPATH_DEVICE_SYSFS_ATTR_LINK      0

struct mpath_subsys {
	enum mpath_iopolicy	iopolicy;
};

struct mpath_head {
	struct mpath_subsys *mpath_subsys;
	struct srcu_struct 	srcu;
	struct list_head	dev_list;	/* list of all mpath_sdevs */
	struct gendisk		*disk;
	struct kref		ref;
	struct	bio_list	requeue_list; /* list for requeing bio */
	spinlock_t		requeue_lock;
	struct work_struct	requeue_work; /* work struct for requeue */

	struct cdev		cdev;
	struct device		cdev_device;

	unsigned long           flags;		/* flag for multipath devices*/

	struct work_struct	partition_scan_work;
	struct mutex            lock;

	struct device *parent;

	struct mpath_device __rcu *current_path[MAX_NUMNODES]; /* scsi_device of current path */
	const struct mpath_head_template *mpdt;
	unsigned long hostdata[]  /* Used for storage of host specific stuff */
			__attribute__ ((aligned (sizeof(unsigned long))));
};

#define MPATH_REQ_IO_STATS (1 << 0)
#define MPATH_REQ_CNT_ACTIVE (1 << 1)

struct mpath_request {
	unsigned long		flags;
	unsigned long		start_time;
};

struct mpath_device {
	struct mpath_head *mpath_head;
	struct list_head siblings;
	enum mpath_access_state	state;
	atomic_t nr_active;
	atomic_t nr_total;
	struct gendisk *disk;
	unsigned long           flags;		/* flag for multipath devices*/
	int				numa_node; /* NUMA node for Path  */
};

#define REQ_MPATH		REQ_DRV

struct mpath_head_template {
	//const struct class *class;
	const struct class *cdev_class;
	bool (*is_disabled)(struct mpath_device *);
	bool (*is_optimized)(struct mpath_device *);
	int (*get_unique_id)(struct mpath_device *, u8 id[16], enum blk_unique_id type);
	int (*ioctl)(struct mpath_device *, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg);
	const struct attribute_group **device_groups;
};

struct mpath_head *mpath_alloc_head(const struct mpath_head_template *mpdt,
						int privsize);

int mpath_alloc_head_disk(struct mpath_head *mpath_head);
int __must_check mpath_add_head(struct mpath_head *);


static inline struct mpath_head *mpath_priv_to_head(void *d)
{
	return d - sizeof(struct mpath_head);
}

static inline void *mpath_to_priv_head(struct mpath_head *mpath_head)
{
	return mpath_head + 1;
}

static inline bool is_mpath_request(struct request *req)
{
	return req->cmd_flags & REQ_MPATH;
}

#define cdev_mpath_priv_to_head(cdev) container_of(cdev, struct mpath_head, cdev)

bool mpath_clear_current_path(struct mpath_device *);
struct mpath_device *mpath_find_path(struct mpath_head *mpath_head);
struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head, int node);
void mpath_requeue_work(struct work_struct *work);
void mpath_revalidate_path(struct gendisk *disk, sector_t capacity);
ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device, char *buf);
void mpath_put_disk(struct mpath_head *mpath_head);
int mpath_get_disk(struct mpath_head *mpath_head);
//int mpath_head_add_cdev(struct mpath_head *mpath_head);
void mpath_cdev_del(struct cdev *cdev, struct device *cdev_device);
//void multipath_partition_scan_work(struct work_struct *work);
void mpath_add_sysfs_link(struct mpath_head *mpath_head);
void mpath_remove_sysfs_link(struct mpath_device *mpath_device);
void mpath_add_device(struct mpath_device *mpath_device);
bool mpath_device_is_live(struct mpath_device *mpath_device);
void mpath_remove_device(struct mpath_device *mpath_device);
//void mpath_cdev_rel(struct device *dev);
int mpath_set_iopolicy(const char *val, const struct kernel_param *kp);
int mpath_get_iopolicy(char *buf, const struct kernel_param *kp);

ssize_t mpath_iopolicy_show(struct mpath_subsys *mpath_subsys, char *buf);
ssize_t mpath_iopolicy_store(struct mpath_subsys *mpath_subsys, const char *buf, size_t count);

void mpath_device_set_live(struct mpath_device *mpath_device);

//extern struct device_attribute mpath_iopolicy;
extern const struct block_device_operations mpath_ops;
extern const struct attribute_group *mpath_device_groups[];
extern const struct attribute_group mpath_attr_group;

bool is_mpath_head(struct gendisk *disk);

#endif // _LIBMULTIPATH_H
