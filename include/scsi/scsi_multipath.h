/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_SCSI_MULTIPATH_H
#define _SCSI_SCSI_MULTIPATH_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/blk-mq.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

struct scsi_device;

enum scsi_mpath_iopolicy {
	SCSI_MPATH_IOPOLICY_NUMA,
	SCSI_MPATH_IOPOLICY_RR,
	SCSI_MPATH_IOPOLICY_QD,
};

enum scsi_mpath_access_state {
	SCSI_MPATH_OPTIMAL	= SCSI_ACCESS_STATE_OPTIMAL,
	SCSI_MPATH_ACTIVE	= SCSI_ACCESS_STATE_ACTIVE,
	SCSI_MPATH_STANDBY	= SCSI_ACCESS_STATE_STANDBY,
	SCSI_MPATH_UNAVAILABLE	= SCSI_ACCESS_STATE_UNAVAILABLE,
	SCSI_MPATH_LBA		= SCSI_ACCESS_STATE_LBA,
	SCSI_MPATH_OFFLINE	= SCSI_ACCESS_STATE_OFFLINE,
	SCSI_MPATH_TRANSITIONING = SCSI_ACCESS_STATE_TRANSITIONING,
	SCSI_MPATH_INVALID	= 0xFF
};

struct scsi_mpath_disk;
struct scsi_mpath_device;

extern void scsi_mpath_failover_req(struct request *);
extern void scsi_mpath_start_request(struct request *);
extern void scsi_mpath_end_request(struct request *);
extern bool scsi_mpath_clear_current_path(struct scsi_mpath_device *);
extern int scsi_mpath_failover_disposition(struct scsi_cmnd *);
int scsi_mpath_alloc_disk(struct scsi_device *, struct gendisk *gd);
extern void scsi_mpath_remove_disk(struct scsi_device *);
extern void scsi_mpath_shutdown_disk(struct scsi_device *sdev);
extern void scsi_mpath_dev_release(struct scsi_device *);
void scsi_mpath_kick_requeue_lists(struct Scsi_Host *);
int scsi_mpath_update_state(struct scsi_mpath_device *mpath_dev);
extern void scsi_mpath_add_disk(struct scsi_device *);
void scsi_mpath_set_live(struct scsi_mpath_device *);
void scsi_multipath_iopolicy_update(struct scsi_device *, int);
void scsi_mpath_clear_paths(struct scsi_mpath_disk *);
int scsi_mpath_unique_lun_id(struct scsi_device *);

extern void scsi_mpath_revalidate_path(struct gendisk *, sector_t);
extern int scsi_mpath_unique_id(struct scsi_device *sdev, u8 id[16], enum blk_unique_id type);

void scsi_mpath_wait_freeze(struct scsi_mpath_disk *mpath_disk);
void scsi_mpath_start_freeze(struct scsi_mpath_disk *mpath_disk);
#endif /* _SCSI_SCSI_MULTIPATH_H */
