// SPDX-License-Identifier: GPL-2.0-only
/*
 */

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include "scsi_alua.h"

#define DRV_NAME "scsi_alua"


#define ALUA_NAME "alua"

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01
#define ALUA_RTPG_EXT_HDR_UNSUPP	0x02
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
#define ALUA_PG_RUNNING			0x40

typedef void (*activate_complete)(void *, int);

struct alua_queue_data {
	struct list_head	entry;
	activate_complete	callback_fn;
	void			*callback_data;
};

enum {
	SCSI_DH_OK = 0,
	/*
	 * device errors
	 */
	SCSI_DH_DEV_FAILED,	/* generic device error */
	SCSI_DH_DEV_TEMP_BUSY,
	SCSI_DH_DEV_UNSUPP,	/* device handler not supported */
	SCSI_DH_DEVICE_MAX,	/* max device blkerr definition */

	/*
	 * transport errors
	 */
	SCSI_DH_NOTCONN = SCSI_DH_DEVICE_MAX + 1,
	SCSI_DH_CONN_FAILURE,
	SCSI_DH_TRANSPORT_MAX,	/* max transport blkerr definition */

	/*
	 * driver and generic errors
	 */
	SCSI_DH_IO = SCSI_DH_TRANSPORT_MAX + 1,	/* generic error */
	SCSI_DH_INVALID_IO,
	SCSI_DH_RETRY,		/* retry the req, but not immediately */
	SCSI_DH_IMM_RETRY,	/* immediately retry the req */
	SCSI_DH_TIMED_OUT,
	SCSI_DH_RES_TEMP_UNAVAIL,
	SCSI_DH_DEV_OFFLINED,
	SCSI_DH_NOMEM,
	SCSI_DH_NOSYS,
	SCSI_DH_DRIVER_MAX,
};

static LIST_HEAD(port_group_list);
static DEFINE_SPINLOCK(port_group_lock);

bool alua_optimize_stpg;

/*
 * alua_check_tpgs - Evaluate TPGS setting
 * @sdev: device to be checked
 *
 * Examine the TPGS setting of the sdev to find out if ALUA
 * is supported.
 */
int alua_check_tpgs(struct scsi_device *sdev)
{
	int tpgs = TPGS_MODE_NONE;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	/*
	 * ALUA support for non-disk devices is fraught with
	 * difficulties, so disable it for now.
	 */
	if (sdev->type != TYPE_DISK) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: disable for non-disk devices\n",
			    DRV_NAME);
		return tpgs;
	}

	tpgs = scsi_device_tpgs(sdev);
	switch (tpgs) {
	case TPGS_MODE_EXPLICIT|TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev,
			    "%s: supports implicit and explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_EXPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports implicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_NONE:
		sdev_printk(KERN_INFO, sdev, "%s: not supported\n",
			    DRV_NAME);
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: unsupported TPGS setting %d\n",
			    DRV_NAME, tpgs);
		tpgs = TPGS_MODE_NONE;
		break;
	}

	return tpgs;
}
EXPORT_SYMBOL_GPL(alua_check_tpgs);

static struct alua_port_group *alua_find_get_pg(char *id_str, size_t id_size,
						int group_id)
{
	struct alua_port_group *pg;

	if (!id_str || !id_size || !strlen(id_str))
		return NULL;
	list_for_each_entry(pg, &port_group_list, node) {
		if (pg->group_id != group_id)
			continue;
		if (!pg->device_id_len || pg->device_id_len != id_size)
			continue;
		if (strncmp(pg->device_id_str, id_str, id_size))
			continue;
		if (!kref_get_unless_zero(&pg->kref))
			continue;
		return pg;
	}

	return NULL;
}

struct alua_port_group *alua_find_get_pg_locked(char *id_str, size_t id_size, int group_id)
{
	struct alua_port_group *pg;
	unsigned long flags;

	spin_lock_irqsave(&port_group_lock, flags);
	pg = alua_find_get_pg(id_str, id_size, group_id);
	spin_unlock_irqrestore(&port_group_lock, flags);

	return pg;
}
EXPORT_SYMBOL_GPL(alua_find_get_pg_locked);

/*
 * alua_alloc_pg - Allocate a new port_group structure
 * @sdev: scsi device
 * @group_id: port group id
 * @tpgs: target port group settings
 *
 * Allocate a new port_group structure for a given
 * device.
 */
struct alua_port_group *alua_alloc_pg(struct scsi_device *sdev,
					     int group_id, int tpgs)
{
	struct alua_port_group *pg, *tmp_pg;

	pg = kzalloc(sizeof(struct alua_port_group), GFP_KERNEL);
	if (!pg)
		return ERR_PTR(-ENOMEM);
	sdev_printk(KERN_ERR, sdev, "%s sdev=%pS group_id=%d tpgs=%d pg=%pS\n",
		__func__, sdev, group_id, tpgs, pg);

	pg->device_id_len = scsi_vpd_lun_id(sdev, pg->device_id_str,
					    sizeof(pg->device_id_str));
	if (pg->device_id_len <= 0) {
		/*
		 * TPGS supported but no device identification found.
		 * Generate private device identification.
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No device descriptors found\n",
			    ALUA_NAME);
		pg->device_id_str[0] = '\0';
		pg->device_id_len = 0;
	}
	pg->group_id = group_id;
	pg->tpgs = tpgs;
	pg->state = SCSI_ACCESS_STATE_OPTIMAL;
	pg->valid_states = TPGS_SUPPORT_ALL;
	if (alua_optimize_stpg)
		pg->flags |= ALUA_OPTIMIZE_STPG;
	kref_init(&pg->kref);
	//INIT_DELAYED_WORK(&pg->rtpg_work, alua_rtpg_work);
	INIT_LIST_HEAD(&pg->rtpg_list);
	INIT_LIST_HEAD(&pg->node);
	INIT_LIST_HEAD(&pg->dh_list);
	spin_lock_init(&pg->lock);

	spin_lock(&port_group_lock);
	tmp_pg = alua_find_get_pg(pg->device_id_str, pg->device_id_len,
				  group_id);
	if (tmp_pg) {
		spin_unlock(&port_group_lock);
		kfree(pg);
		sdev_printk(KERN_ERR, sdev, "%s0 sdev=%pS group_id=%d tpgs=%d tmp_pg=%pS calling configure_scsi_mpath\n",
			__func__, sdev, group_id, tpgs, tmp_pg);


		return tmp_pg;
	}

	sdev_printk(KERN_ERR, sdev, "%s1 sdev=%pS group_id=%d tpgs=%d tmp_pg=%pS calling configure_scsi_mpath\n",
			__func__, sdev, group_id, tpgs, tmp_pg);


	list_add(&pg->node, &port_group_list);
	spin_unlock(&port_group_lock);

	return pg;
}
EXPORT_SYMBOL_GPL(alua_alloc_pg);

static void release_port_group(struct kref *kref)
{
	struct alua_port_group *pg;

	pg = container_of(kref, struct alua_port_group, kref);
	if (pg->rtpg_sdev)
		flush_delayed_work(&pg->rtpg_work);
	spin_lock(&port_group_lock);
	list_del(&pg->node);
	spin_unlock(&port_group_lock);
	kfree_rcu(pg, rcu);
}

void alua_put_pg(struct alua_port_group *pg)
{
	kref_put(&pg->kref, release_port_group);
}
EXPORT_SYMBOL_GPL(alua_put_pg);

/*
 * alua_tur - Send a TEST UNIT READY
 * @sdev: device to which the TEST UNIT READY command should be send
 *
 * Send a TEST UNIT READY to @sdev to figure out the device state
 * Returns SCSI_DH_RETRY if the sense code is NOT READY/ALUA TRANSITIONING,
 * SCSI_DH_OK if no error occurred, and SCSI_DH_IO otherwise.
 */
int alua_tur(struct scsi_device *sdev)
{
	struct scsi_sense_hdr sense_hdr;
	int retval;

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	retval = scsi_test_unit_ready(sdev, ALUA_FAILOVER_TIMEOUT * HZ,
				      ALUA_FAILOVER_RETRIES, &sense_hdr);
	if ((sense_hdr.sense_key == NOT_READY ||
	     sense_hdr.sense_key == UNIT_ATTENTION) &&
	    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a)
		return SCSI_DH_RETRY;
	else if (retval)
		return SCSI_DH_IO;
	else
		return SCSI_DH_OK;
}
EXPORT_SYMBOL_GPL(alua_tur);

/*
 * submit_rtpg - Issue a REPORT TARGET GROUP STATES command
 * @sdev: sdev the command should be sent to
 */
int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
		       int bufflen, struct scsi_sense_hdr *sshdr, int flags)
{
	u8 cdb[MAX_COMMAND_SIZE];
	blk_opf_t opf = REQ_OP_DRV_IN | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	sdev_printk(KERN_ERR, sdev, "%s\n", __func__);
	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_IN;
	if (!(flags & ALUA_RTPG_EXT_HDR_UNSUPP))
		cdb[1] = MI_REPORT_TARGET_PGS | MI_EXT_HDR_PARAM_FMT;
	else
		cdb[1] = MI_REPORT_TARGET_PGS;
	put_unaligned_be32(bufflen, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, buff, bufflen,
				ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}
EXPORT_SYMBOL_GPL(submit_rtpg);

char print_alua_state(unsigned char state)
{
	switch (state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return 'A';
	case SCSI_ACCESS_STATE_ACTIVE:
		return 'N';
	case SCSI_ACCESS_STATE_STANDBY:
		return 'S';
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		return 'U';
	case SCSI_ACCESS_STATE_LBA:
		return 'L';
	case SCSI_ACCESS_STATE_OFFLINE:
		return 'O';
	case SCSI_ACCESS_STATE_TRANSITIONING:
		return 'T';
	default:
		return 'X';
	}
}
EXPORT_SYMBOL_GPL(print_alua_state);


/*
 * submit_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Currently we're only setting the current target port group state
 * to 'active/optimized' and let the array firmware figure out
 * the states of the remaining groups.
 */
int submit_stpg(struct scsi_device *sdev, int group_id,
		       struct scsi_sense_hdr *sshdr)
{
	u8 cdb[MAX_COMMAND_SIZE];
	unsigned char stpg_data[8];
	int stpg_len = 8;
	blk_opf_t opf = REQ_OP_DRV_OUT | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the data buffer */
	memset(stpg_data, 0, stpg_len);
	stpg_data[4] = SCSI_ACCESS_STATE_OPTIMAL;
	put_unaligned_be16(group_id, &stpg_data[6]);

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_OUT;
	cdb[1] = MO_SET_TARGET_PGS;
	put_unaligned_be32(stpg_len, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, stpg_data,
				stpg_len, ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}
EXPORT_SYMBOL_GPL(submit_stpg);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_alua");