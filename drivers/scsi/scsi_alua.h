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
#define ALUA_FAILOVER_RETRIES		5
#define ALUA_RTPG_DELAY_MSECS		5
#define ALUA_RTPG_RETRY_DELAY		2

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01
#define ALUA_RTPG_EXT_HDR_UNSUPP	0x02
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
#define ALUA_PG_RUNNING			0x40

typedef void (*scsi_activate_complete)(void *, int);

struct scsi_alua_queue_data {
	struct list_head	entry;
	scsi_activate_complete	callback_fn;
	void			*callback_data;
};

struct scsi_alua_port_group {
	struct kref		kref;
	struct rcu_head		rcu;
	struct list_head	node;
	struct list_head	dh_list;
	unsigned char		device_id_str[256];
	int			device_id_len;
	int			group_id;
	int			tpgs;
	int			state;
	int			pref;
	int			valid_states;
	unsigned		flags; /* used for optimizing STPG */
	unsigned char		transition_tmo;
	unsigned long		expiry;
	unsigned long		interval;
	struct delayed_work	rtpg_work;
	spinlock_t		lock;
	struct list_head	rtpg_list;
	struct scsi_device	*rtpg_sdev;
};

int scsi_alua_check_tpgs(struct scsi_device *sdev);


bool scsi_alua_rtpg_queue(struct scsi_alua_port_group *pg,
			    struct scsi_device *sdev,
			    struct scsi_alua_queue_data *qdata, bool force);

#endif // _SCSI_ALUA_H
