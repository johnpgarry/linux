// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic SCSI-3 ALUA SCSI Device Handler
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */
#ifndef _SCSI_ALUA_H
#define _SCSI_ALUA_H

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>

#define TPGS_SUPPORT_NONE		0x00
#define TPGS_SUPPORT_OPTIMIZED		0x01
#define TPGS_SUPPORT_NONOPTIMIZED	0x02
#define TPGS_SUPPORT_STANDBY		0x04
#define TPGS_SUPPORT_UNAVAILABLE	0x08
#define TPGS_SUPPORT_LBA_DEPENDENT	0x10
#define TPGS_SUPPORT_OFFLINE		0x40
#define TPGS_SUPPORT_TRANSITION		0x80
#define TPGS_SUPPORT_ALL		0xdf

#define RTPG_FMT_MASK			0x70
#define RTPG_FMT_EXT_HDR		0x10

#define TPGS_MODE_UNINITIALIZED		 -1
#define TPGS_MODE_NONE			0x0
#define TPGS_MODE_IMPLICIT		0x1
#define TPGS_MODE_EXPLICIT		0x2

#define ALUA_RTPG_SIZE			128
#define ALUA_FAILOVER_TIMEOUT		60

struct alua_data {
	int			group_id;
	int			tpgs;
	int			state;
	int			pref;
	int			valid_states;

	bool			rtpg_ext_hdr_unsupp;
	unsigned char		transition_tmo;
	unsigned long		expiry;
	unsigned long		interval;
	struct delayed_work	work;

	struct scsi_device *sdev;
};

int alua_check_tpgs(struct scsi_device *sdev);
int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
		       int bufflen, struct scsi_sense_hdr *sshdr);
int alua_tur(struct scsi_device *sdev);
void alua_print_info(struct scsi_device *sdev);
int submit_stpg(struct scsi_device *sdev, int group_id,
		       struct scsi_sense_hdr *sshdr);
int scsi_alua_init(struct scsi_device *sdev);
int scsi_mpath_run_rtpg(struct scsi_device *sdev);
#endif // _SCSI_ALUA_H
