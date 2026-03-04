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
	INIT_LIST_HEAD(&pg->list);
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
static int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
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

/*
 * alua_rtpg - Evaluate REPORT TARGET GROUP STATES
 * @sdev: the device to be evaluated.
 *
 * Evaluate the Target Port Group State.
 * Returns SCSI_DH_DEV_OFFLINED if the path is
 * found to be unusable.
 */
int alua_rtpg(struct scsi_device *sdev, struct alua_port_group *pg)
{
	struct scsi_sense_hdr sense_hdr;
	struct alua_port_group *tmp_pg;
	int len, k, off, bufflen = ALUA_RTPG_SIZE;
	int group_id_old, state_old, pref_old, valid_states_old;
	unsigned char *desc, *buff;
	unsigned err;
	int retval;
	unsigned int tpg_desc_tbl_off;
	unsigned char orig_transition_tmo;
	unsigned long flags;
	bool transitioning_sense = false;

	sdev_printk(KERN_ERR, sdev, "%s sdev=%pS pg=%pS\n", __func__, sdev, pg);
	group_id_old = pg->group_id;
	state_old = pg->state;
	pref_old = pg->pref;
	valid_states_old = pg->valid_states;

	if (!pg->expiry) {
		unsigned long transition_tmo = ALUA_FAILOVER_TIMEOUT * HZ;

		if (pg->transition_tmo)
			transition_tmo = pg->transition_tmo * HZ;

		pg->expiry = round_jiffies_up(jiffies + transition_tmo);
	}

	buff = kzalloc(bufflen, GFP_KERNEL);
	if (!buff)
		return SCSI_DH_DEV_TEMP_BUSY;

 retry:
	err = 0;
	sdev_printk(KERN_ERR, sdev, "%s1 sdev=%pS pg=%pS calling submit_rtpg\n", __func__, sdev, pg);
	retval = submit_rtpg(sdev, buff, bufflen, &sense_hdr, pg->flags);

	if (retval) {
		/*
		 * Some (broken) implementations have a habit of returning
		 * an error during things like firmware update etc.
		 * But if the target only supports active/optimized there's
		 * not much we can do; it's not that we can switch paths
		 * or anything.
		 * So ignore any errors to avoid spurious failures during
		 * path failover.
		 */
		if ((pg->valid_states & ~TPGS_SUPPORT_OPTIMIZED) == 0) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: ignoring rtpg result %d\n",
				    ALUA_NAME, retval);
			kfree(buff);
			return SCSI_DH_OK;
		}
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: rtpg failed, result %d\n",
				    ALUA_NAME, retval);
			kfree(buff);
			if (retval < 0)
				return SCSI_DH_DEV_TEMP_BUSY;
			if (host_byte(retval) == DID_NO_CONNECT)
				return SCSI_DH_RES_TEMP_UNAVAIL;
			return SCSI_DH_IO;
		}

		/*
		 * submit_rtpg() has failed on existing arrays
		 * when requesting extended header info, and
		 * the array doesn't support extended headers,
		 * even though it shouldn't according to T10.
		 * The retry without rtpg_ext_hdr_req set
		 * handles this.
		 * Note:  some arrays return a sense key of ILLEGAL_REQUEST
		 * with ASC 00h if they don't support the extended header.
		 */
		if (!(pg->flags & ALUA_RTPG_EXT_HDR_UNSUPP) &&
		    sense_hdr.sense_key == ILLEGAL_REQUEST) {
			pg->flags |= ALUA_RTPG_EXT_HDR_UNSUPP;
			goto retry;
		}
		/*
		 * If the array returns with 'ALUA state transition'
		 * sense code here it cannot return RTPG data during
		 * transition. So set the state to 'transitioning' directly.
		 */
		if (sense_hdr.sense_key == NOT_READY &&
		    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a) {
			transitioning_sense = true;
			goto skip_rtpg;
		}
		/*
		 * Retry on any other UNIT ATTENTION occurred.
		 */
		if (sense_hdr.sense_key == UNIT_ATTENTION)
			err = SCSI_DH_RETRY;
		if (err == SCSI_DH_RETRY &&
		    pg->expiry != 0 && time_before(jiffies, pg->expiry)) {
			sdev_printk(KERN_ERR, sdev, "%s: rtpg retry\n",
				    ALUA_NAME);
			scsi_print_sense_hdr(sdev, ALUA_NAME, &sense_hdr);
			kfree(buff);
			return err;
		}
		sdev_printk(KERN_ERR, sdev, "%s: rtpg failed\n",
			    ALUA_NAME);
		scsi_print_sense_hdr(sdev, ALUA_NAME, &sense_hdr);
		kfree(buff);
		pg->expiry = 0;
		return SCSI_DH_IO;
	}

	len = get_unaligned_be32(&buff[0]) + 4;

	if (len > bufflen) {
		/* Resubmit with the correct length */
		kfree(buff);
		bufflen = len;
		buff = kmalloc(bufflen, GFP_KERNEL);
		if (!buff) {
			sdev_printk(KERN_WARNING, sdev,
				    "%s: kmalloc buffer failed\n",__func__);
			/* Temporary failure, bypass */
			pg->expiry = 0;
			return SCSI_DH_DEV_TEMP_BUSY;
		}
		goto retry;
	}

	orig_transition_tmo = pg->transition_tmo;
	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR && buff[5] != 0)
		pg->transition_tmo = buff[5];
	else
		pg->transition_tmo = ALUA_FAILOVER_TIMEOUT;

	if (orig_transition_tmo != pg->transition_tmo) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: transition timeout set to %d seconds\n",
			    ALUA_NAME, pg->transition_tmo);
		pg->expiry = jiffies + pg->transition_tmo * HZ;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR)
		tpg_desc_tbl_off = 8;
	else
		tpg_desc_tbl_off = 4;

	for (k = tpg_desc_tbl_off, desc = buff + tpg_desc_tbl_off;
	     k < len;
	     k += off, desc += off) {
		u16 group_id = get_unaligned_be16(&desc[2]);

		
		tmp_pg = alua_find_get_pg_locked(pg->device_id_str, pg->device_id_len,
					  group_id);
		
		if (tmp_pg) {
			if (spin_trylock_irqsave(&tmp_pg->lock, flags)) {
				if ((tmp_pg == pg) ||
				    !(tmp_pg->flags & ALUA_PG_RUNNING)) {
					struct alua_data *alua;

					tmp_pg->state = desc[0] & 0x0f;
					pr_err("%s x2 setting %d tmp_pg=%pS\n",
						__func__, tmp_pg->state, tmp_pg);
					tmp_pg->pref = desc[0] >> 7;
					rcu_read_lock();
					list_for_each_entry_rcu(alua,
						&tmp_pg->list, node) {
						pr_err("%s x1.1 alua=%pS\n", __func__, alua);
						pr_err("%s x1.2 alua->sdev=%pS\n", __func__, alua->sdev);
						alua->sdev->access_state = desc[0];
					}
					rcu_read_unlock();
				}
				if (tmp_pg == pg)
					tmp_pg->valid_states = desc[1];
				spin_unlock_irqrestore(&tmp_pg->lock, flags);
			}
			alua_put_pg(tmp_pg);
		}
		off = 8 + (desc[7] * 4);
	}
	pr_err("%s y1\n", __func__);
 skip_rtpg:
	spin_lock_irqsave(&pg->lock, flags);
	if (transitioning_sense) {
		pg->state = SCSI_ACCESS_STATE_TRANSITIONING;
		pr_err("%s x3 setting SCSI_ACCESS_STATE_TRANSITIONING pg=%pS\n",
						__func__, pg);
	}

	pr_err("%s y2\n", __func__);
	if (group_id_old != pg->group_id || state_old != pg->state ||
		pref_old != pg->pref || valid_states_old != pg->valid_states)
		sdev_printk(KERN_INFO, sdev,
			"%s: port group %02x state %c %s supports %c%c%c%c%c%c%c\n",
			ALUA_NAME, pg->group_id, print_alua_state(pg->state),
			pg->pref ? "preferred" : "non-preferred",
			pg->valid_states&TPGS_SUPPORT_TRANSITION?'T':'t',
			pg->valid_states&TPGS_SUPPORT_OFFLINE?'O':'o',
			pg->valid_states&TPGS_SUPPORT_LBA_DEPENDENT?'L':'l',
			pg->valid_states&TPGS_SUPPORT_UNAVAILABLE?'U':'u',
			pg->valid_states&TPGS_SUPPORT_STANDBY?'S':'s',
			pg->valid_states&TPGS_SUPPORT_NONOPTIMIZED?'N':'n',
			pg->valid_states&TPGS_SUPPORT_OPTIMIZED?'A':'a');

	switch (pg->state) {
	case SCSI_ACCESS_STATE_TRANSITIONING:
		if (time_before(jiffies, pg->expiry)) {
			/* State transition, retry */
			pg->interval = ALUA_RTPG_RETRY_DELAY;
			err = SCSI_DH_RETRY;
		} else {
			struct alua_data *alua;

			/* Transitioning time exceeded, set port to standby */
			err = SCSI_DH_IO;
			pg->state = SCSI_ACCESS_STATE_STANDBY;
			pr_err("%s x1 setting SCSI_ACCESS_STATE_STANDBY pg=%pS\n",
				__func__, pg);
			pg->expiry = 0;
			rcu_read_lock();
			list_for_each_entry_rcu(alua, &pg->list, node) {
				if (!alua->sdev)
					continue;
				alua->sdev->access_state =
					(pg->state & SCSI_ACCESS_STATE_MASK);
				if (pg->pref)
					alua->sdev->access_state |=
						SCSI_ACCESS_STATE_PREFERRED;
			}
			rcu_read_unlock();
		}
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		/* Path unusable */
		err = SCSI_DH_DEV_OFFLINED;
		pg->expiry = 0;
		break;
	default:
		/* Useable path if active */
		err = SCSI_DH_OK;
		pg->expiry = 0;
		break;
	}
	pr_err("%s y3\n", __func__);
	spin_unlock_irqrestore(&pg->lock, flags);
	kfree(buff);
	return err;
}
EXPORT_SYMBOL_GPL(alua_rtpg);

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

int scsi_alua_init(struct scsi_device *sdev)
{
	int tpgs;
	pr_err("%s sdev=%pS\n", __func__, sdev);

	tpgs = alua_check_tpgs(sdev);
	pr_err("%s1 sdev=%pS\n", __func__, sdev);
	if (tpgs == TPGS_MODE_NONE)
		return 0;
	sdev->alua = kzalloc(sizeof(*sdev->alua), GFP_KERNEL);
	if (!sdev->alua)
		return -ENOMEM;

	spin_lock_init(&sdev->alua->pg_lock);
	rcu_assign_pointer(sdev->alua->pg, NULL);
	sdev->alua->init_error = SCSI_DH_OK;
	sdev->alua->sdev = sdev;
	INIT_LIST_HEAD(&sdev->alua->node);

	mutex_init(&sdev->alua->init_mutex);

	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_alua");
