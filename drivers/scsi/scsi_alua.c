// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic SCSI-3 ALUA SCSI driver
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_alua.h>

#define DRV_NAME "alua"

#define ALUA_FAILOVER_RETRIES		5

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

/*
 * alua_tur - Send a TEST UNIT READY
 * @sdev: device to which the TEST UNIT READY command should be send
 *
 * Send a TEST UNIT READY to @sdev to figure out the device state
 * Returns SCSI_DH_RETRY if the sense code is NOT READY/ALUA TRANSITIONING,
 * 0 if no error occurred, and SCSI_DH_IO otherwise.
 */
int alua_tur(struct scsi_device *sdev)
{
	struct scsi_sense_hdr sense_hdr;
	int retval;

	retval = scsi_test_unit_ready(sdev, ALUA_FAILOVER_TIMEOUT * HZ,
				      ALUA_FAILOVER_RETRIES, &sense_hdr);
	if ((sense_hdr.sense_key == NOT_READY ||
	     sense_hdr.sense_key == UNIT_ATTENTION) &&
	    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a)
		return -EAGAIN;
	else if (retval)
		return -EIO;
	else
		return 0;
}
EXPORT_SYMBOL_GPL(alua_tur);

/*
 * submit_rtpg - Issue a REPORT TARGET GROUP STATES command
 * @sdev: sdev the command should be sent to
 */
int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
		       int bufflen, struct scsi_sense_hdr *sshdr,
		       bool alua_rtpg_ext_hdr_unsupp)
{
	u8 cdb[MAX_COMMAND_SIZE];
	blk_opf_t opf = REQ_OP_DRV_IN | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_IN;
	if (alua_rtpg_ext_hdr_unsupp)
		cdb[1] = MI_REPORT_TARGET_PGS;
	else
		cdb[1] = MI_REPORT_TARGET_PGS | MI_EXT_HDR_PARAM_FMT;
	put_unaligned_be32(bufflen, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, buff, bufflen,
				ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}
EXPORT_SYMBOL_GPL(submit_rtpg);

static char print_alua_state(unsigned char state)
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

void alua_print_info(struct scsi_device *sdev, int group_id, int state,
		int pref, int valid_states)
{
	sdev_printk(KERN_INFO, sdev,
		"alua: port group %02x state %c %s supports %c%c%c%c%c%c%c\n",
		group_id, print_alua_state(state),
		pref ? "preferred" : "non-preferred",
		valid_states&TPGS_SUPPORT_TRANSITION?'T':'t',
		valid_states&TPGS_SUPPORT_OFFLINE?'O':'o',
		valid_states&TPGS_SUPPORT_LBA_DEPENDENT?'L':'l',
		valid_states&TPGS_SUPPORT_UNAVAILABLE?'U':'u',
		valid_states&TPGS_SUPPORT_STANDBY?'S':'s',
		valid_states&TPGS_SUPPORT_NONOPTIMIZED?'N':'n',
		valid_states&TPGS_SUPPORT_OPTIMIZED?'A':'a');
}
EXPORT_SYMBOL_GPL(alua_print_info);

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
