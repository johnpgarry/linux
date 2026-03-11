/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_SCSI_MULTIPATH_H
#define _SCSI_SCSI_MULTIPATH_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/blk-mq.h>
#include <linux/multipath.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_devinfo.h>
#include <scsi/scsi_driver.h>

#ifdef CONFIG_SCSI_MULTIPATH
#define SCSI_MPATH_DEVICE_ID_LEN 40

struct scsi_mpath_head {
	char			wwid[SCSI_MPATH_DEVICE_ID_LEN];
	struct list_head	entry;
	int			dev_count;
	struct ida		ida;
	struct mutex		lock;
	struct mpath_iopolicy	iopolicy;
	struct bio_set		bio_pool;
	struct mpath_head	*mpath_head;
	struct device		dev;
	int			index;
	struct task_struct	*kua;
};

struct scsi_mpath_device {
	struct mpath_device	mpath_device;
	struct scsi_device 	*sdev;
	int			index;
	atomic_t		nr_active;
	struct scsi_mpath_head	*scsi_mpath_head;

	char			device_id_str[SCSI_MPATH_DEVICE_ID_LEN];
};

struct scsi_mpath_pr_ops {
	int (*pr_register)(struct scsi_device *, u64 old_key,
			u64 new_key, u32 flags);
	int (*pr_reserve)(struct scsi_device *e, u64 key,
			enum pr_type type, u32 flags);
	int (*pr_release)(struct scsi_device *, u64 key,
			enum pr_type type);
	int (*pr_preempt)(struct scsi_device *, u64 old_key,
			u64 new_key, enum pr_type type, bool abort);
	int (*pr_clear)(struct scsi_device *, u64 key);
	int (*pr_read_keys)(struct scsi_device *,
			struct pr_keys *keys_info);
	int (*pr_read_reservation)(struct scsi_device *,
			struct pr_held_reservation *rsv);
};

#define to_scsi_mpath_device(d) \
	container_of(d, struct scsi_mpath_device, mpath_device)

void scsi_mpath_failover_req(struct request *);
int scsi_mpath_failover_disposition(struct scsi_cmnd *);
bool scsi_is_mpath_request(struct request *req);
int scsi_mpath_dev_alloc(struct scsi_device *sdev);
void scsi_mpath_dev_release(struct scsi_device *sdev);
int scsi_multipath_init(void);
void scsi_multipath_exit(void);
void scsi_mpath_dev_clear_path(struct scsi_mpath_device *scsi_mpath_dev);
void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev);
void scsi_mpath_add_sysfs_link(struct scsi_device *sdev);
void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev);
int scsi_mpath_get_head(struct scsi_mpath_head *);
void scsi_mpath_put_head(struct scsi_mpath_head *);

static inline void scsi_mpath_start_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);

	if (is_mpath_request(req))
		scsi_cmd_to_driver(cmd)->mpath_start_cmd(cmd);
}

static inline void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);

	if (is_mpath_request(req))
		scsi_cmd_to_driver(cmd)->mpath_start_cmd(cmd);
}

#else /* CONFIG_SCSI_MULTIPATH */

struct scsi_mpath_head {
};
struct scsi_mpath_device {
};

static inline void scsi_mpath_failover_req(struct request *)
{
}
static inline int scsi_mpath_failover_disposition(struct scsi_cmnd *)
{
	return 0;
}
static inline bool scsi_is_mpath_request(struct request *req)
{
	return false;
}
static inline int scsi_mpath_dev_alloc(struct scsi_device *sdev)
{
	return 0;
}
static inline void scsi_mpath_dev_release(struct scsi_device *sdev)
{
}
static inline int scsi_multipath_init(void)
{
	return 0;
}
static inline void scsi_multipath_exit(void)
{
}
static inline void scsi_mpath_dev_clear_path(
			struct scsi_mpath_device *scsi_mpath_dev)
{
}
static inline void scsi_mpath_remove_device(struct scsi_mpath_device
					*scsi_mpath_dev)
{
}
static inline int scsi_mpath_get_head(struct scsi_mpath_head *)
{
	return 0;
}
static inline void scsi_mpath_put_head(struct scsi_mpath_head *)
{
}

static inline void scsi_mpath_start_request(struct request *)
{
}
static inline void scsi_mpath_end_request(struct request *)
{
}

static inline void scsi_mpath_add_sysfs_link(struct scsi_device *sdev)
{
}
static inline void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev)
{
}
#endif /* CONFIG_SCSI_MULTIPATH */
#endif /* _SCSI_SCSI_MULTIPATH_H */
