
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/pr.h>
#include <linux/io_uring/cmd.h>

enum mpath_iopolicy_e {
	MPATH_IOPOLICY_NUMA,
	MPATH_IOPOLICY_RR,
	MPATH_IOPOLICY_QD,
};

#if defined(CONFIG_LIBMULTIPATH)
struct mpath_device;

enum mpath_access_state {
	MPATH_STATE_OPTIMIZED,
	MPATH_STATE_ACTIVE,
	MPATH_STATE_INVALID	= 0xFF
};

/*
 * Mark bio as coming from scsi multipath node
 */
#define MPATH_HEAD_DISK_LIVE            0
#define MPATH_HEAD_QUEUE_IF_NO_PATH		1

#define MPATH_DEVICE_SYSFS_ATTR_LINK      0

struct mpath_iopolicy {
	enum mpath_iopolicy_e	iopolicy;
};

struct mpath_head {
	struct srcu_struct 	srcu;
	struct list_head	dev_list;	/* list of all mpath_sdevs */
	struct mutex            lock;
	struct gendisk		*disk;
	struct kref		ref;
	struct	bio_list	requeue_list; /* list for requeing bio */
	spinlock_t		requeue_lock;
	struct work_struct	requeue_work; /* work struct for requeue */

	struct cdev		cdev;
	struct device		cdev_device;

	unsigned long           flags;		/* flag for multipath devices*/

	struct work_struct	partition_scan_work;

	struct device *parent;

	struct delayed_work	remove_work;
	unsigned int		delayed_removal_secs;

	struct mpath_device __rcu *current_path[MAX_NUMNODES]; /* scsi_device of current path */
	const struct mpath_head_template *mpdt;
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

struct mpath_pr_ops {
	int (*pr_register)(struct mpath_device *mpath_device, u64 old_key, u64 new_key,
			u32 flags);
	int (*pr_reserve)(struct mpath_device *mpath_device, u64 key,
			enum pr_type type, u32 flags);
	int (*pr_release)(struct mpath_device *mpath_device, u64 key,
			enum pr_type type);
	int (*pr_preempt)(struct mpath_device *mpath_device, u64 old_key, u64 new_key,
			enum pr_type type, bool abort);
	int (*pr_clear)(struct mpath_device *mpath_device, u64 key);
	/*
	 * pr_read_keys - Read the registered keys and return them in the
	 * pr_keys->keys array. The keys array will have been allocated at the
	 * end of the pr_keys struct, and pr_keys->num_keys must be set to the
	 * number of keys the array can hold. If there are more than can fit
	 * in the array, success will still be returned and pr_keys->num_keys
	 * will reflect the total number of keys the device contains, so the
	 * caller can retry with a larger array.
	 */
	int (*pr_read_keys)(struct mpath_device *mpath_device,
			struct pr_keys *keys_info);
	int (*pr_read_reservation)(struct mpath_device *mpath_device,
			struct pr_held_reservation *rsv);
};

struct mpath_head_template {
	//const struct class *class;
	//const struct class *cdev_class;
	int (*add_cdev)(struct mpath_head *);
	void (*del_cdev)(struct mpath_head *);
	bool (*is_disabled)(struct mpath_device *);
	bool (*is_optimized)(struct mpath_device *);
	int (*ioctl)(struct mpath_device *, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg, int srcu_idx);
	int (*report_zones)(struct mpath_device *, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args);
	int (*chr_uring_cmd)(struct mpath_device *, struct io_uring_cmd *ioucmd,
		unsigned int issue_flags);
	int (*chr_uring_cmd_iopoll)(struct io_uring_cmd *ioucmd,
				 struct io_comp_batch *iob,
				 unsigned int poll_flags);
	void (*free_head)(struct mpath_head *);
	void (*remove_head_work)(struct mpath_head *);
	enum mpath_iopolicy_e (*get_iopolicy)(struct mpath_head *);
	struct bio *(*clone_bio)(struct bio *);
	const struct mpath_pr_ops *pr_ops;
	const struct attribute_group **device_groups;
};

void mpath_head_read_unlock(struct mpath_head *mpath_head, int srcu_idx);

void mpath_init_head(struct mpath_head *mpath_head);

int mpath_alloc_head_disk(struct mpath_head *mpath_head);
int __must_check mpath_add_head(struct mpath_head *);

static inline bool mpath_head_queue_if_no_path(struct mpath_head *mpath_head)
{
	if (test_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags))
		return true;
	return false;
}

static inline bool is_mpath_request(struct request *req)
{
	return req->cmd_flags & REQ_MPATH;
}

#define cdev_mpath_priv_to_head(cdev) container_of(cdev, struct mpath_head, cdev)

void mpath_synchronize_device(struct mpath_device *mpath_device);
void mpath_synchronize_head(struct mpath_head *mpath_head);

bool mpath_clear_current_path(struct mpath_device *);
struct mpath_device *mpath_find_path(struct mpath_head *mpath_head);
struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head,
				enum mpath_iopolicy_e, int node);
void mpath_requeue_work(struct work_struct *work);
void mpath_revalidate_path(struct gendisk *disk, sector_t capacity);
ssize_t mpath_numa_nodes_show(struct mpath_device *mpath_device,
					struct mpath_iopolicy *iopolicy, char *buf);
void mpath_put_disk(struct mpath_head *mpath_head);
int mpath_get_disk(struct mpath_head *mpath_head);
//int mpath_head_add_cdev(struct mpath_head *mpath_head);
void mpath_cdev_del(struct cdev *cdev, struct device *cdev_device);
//void multipath_partition_scan_work(struct work_struct *work);
void mpath_add_sysfs_link(struct mpath_head *mpath_head);
void mpath_remove_sysfs_link(struct mpath_device *mpath_device);
void mpath_add_device_scsi(struct mpath_device *mpath_device);
bool mpath_device_is_live(struct mpath_device *mpath_device);
void mpath_remove_device(struct mpath_device *mpath_device);
//void mpath_cdev_rel(struct device *dev);
int mpath_set_iopolicy(const char *val, const struct kernel_param *kp);
int mpath_get_iopolicy(char *buf, const struct kernel_param *kp);

ssize_t mpath_iopolicy_show(struct mpath_iopolicy *mpath_iopolicy, char *buf);
ssize_t mpath_iopolicy_store(struct mpath_iopolicy *mpath_iopolicy, const char *buf, size_t count);

static inline enum mpath_iopolicy_e mpath_read_iopolicy(struct mpath_iopolicy *mpath_iopolicy)
{
	return READ_ONCE(mpath_iopolicy->iopolicy);
}

static inline bool mpath_qd_iopolicy(struct mpath_iopolicy *mpath_iopolicy)
{
	return mpath_read_iopolicy(mpath_iopolicy) == MPATH_IOPOLICY_QD;
}

static inline struct mpath_head *mpath_disk_to_head(struct gendisk *disk)
{
	return disk->private_data;
}

void mpath_device_set_live(struct mpath_device *mpath_device);
void mpath_remove_disk(struct mpath_device *mpath_device);
void mpath_add_device(struct mpath_head *mpath_head, struct mpath_device *mpath_device);
void mpath_delete_device(struct mpath_device *mpath_device);
void mpath_remove_head(struct mpath_head *mpath_head);

void mpath_put_disk(struct mpath_head *mpath_head);

int mpath_call_for_device(struct mpath_head *mpath_head, int (*cb)(struct mpath_device *mpath_device));
void mpath_iterate_devices(struct mpath_head *mpath_head, void (*cb)(struct mpath_device *mpath_device));
void mpath_clear_paths(struct mpath_head *mpath_head);

//extern struct device_attribute mpath_iopolicy;
extern const struct block_device_operations mpath_ops;
extern const struct attribute_group *mpath_device_groups[];
extern const struct attribute_group mpath_attr_group;

extern const struct file_operations mpath_generic_chr_fops;

static inline bool is_mpath_head(struct gendisk *disk)
{
	return disk->fops == &mpath_ops;
}


void mpath_head_set_queue_if_no_path(struct mpath_head *mpath_head, unsigned int sec);
#else // CONFIG_LIBMULTIPATH

struct mpath_head_template {
};
struct mpath_device {
};
struct mpath_head {
};
struct mpath_iopolicy {
};
struct mpath_request {
};

static inline struct mpath_head *mpath_disk_to_head(struct gendisk *disk)
{
	return NULL;
}
static inline void mpath_synchronize_device(struct mpath_device *mpath_device)
{
}
static inline void mpath_synchronize_head(struct mpath_head *mpath_head)
{
}
static inline bool is_mpath_request(struct request *req)
{
	return false;
}
static inline void mpath_add_device_scsi(struct mpath_device *mpath_device)
{
}
static inline void mpath_add_sysfs_link(struct mpath_head *mpath_head)
{
}
static inline void mpath_remove_sysfs_link(struct mpath_device *mpath_device)
{
}
static inline void mpath_remove_disk(struct mpath_device *mpath_device)
{
}
static inline bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	return false;
}
static inline bool mpath_head_queue_if_no_path(struct mpath_head *mpath_head)
{
	return false;
}
static inline int __must_check mpath_add_head(struct mpath_head *)
{
	return 0;
}
static inline void mpath_init_head(struct mpath_head *mpath_head)
{
}
static inline enum mpath_iopolicy_e mpath_read_iopolicy(struct mpath_iopolicy *mpath_iopolicy)
{
	return MPATH_IOPOLICY_NUMA;
}
static inline void mpath_add_device(struct mpath_head *mpath_head, struct mpath_device *mpath_device)
{
}
static inline void mpath_delete_device(struct mpath_device *mpath_device)
{
}
static inline void mpath_remove_head(struct mpath_head *mpath_head)
{
}
static inline void mpath_put_disk(struct mpath_head *mpath_head)
{
}
static inline int mpath_call_for_device(struct mpath_head *mpath_head, int (*cb)(struct mpath_device *mpath_device))
{
	return 0;
}
static inline void mpath_iterate_devices(struct mpath_head *mpath_head, void (*cb)(struct mpath_device *mpath_device))
{
}
static inline void mpath_clear_paths(struct mpath_head *mpath_head)
{
}
static inline void mpath_head_set_queue_if_no_path(struct mpath_head *mpath_head, unsigned int sec)
{
}
static inline bool is_mpath_head(struct gendisk *disk)
{
	return false;
}
#define mpath_device_groups NULL

#endif //CONFIG_LIBMULTIPATH

#endif // _LIBMULTIPATH_H
