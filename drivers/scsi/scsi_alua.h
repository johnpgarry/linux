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

/* see spc 5 r22 rable 273 byte 0 3:0*/
#define TPGS_SUPPORT_NONE		0x00
#define TPGS_SUPPORT_OPTIMIZED		0x01
#define TPGS_SUPPORT_NONOPTIMIZED	0x02
#define TPGS_SUPPORT_STANDBY		0x04
#define TPGS_SUPPORT_UNAVAILABLE	0x08
#define TPGS_SUPPORT_LBA_DEPENDENT	0x10
#define TPGS_SUPPORT_OFFLINE		0x40
#define TPGS_SUPPORT_TRANSITION		0x80
#define TPGS_SUPPORT_ALL		0xdf

/* see spc 5 r22 rable 273 byte 0 6:4*/
#define RTPG_FMT_MASK			0x70
#define RTPG_FMT_EXT_HDR		0x10

/* sdev->inquiry[5] >> 4) & 0x3 : 0 scsi_alua_check_tpgs() -> scsi_device_tpgs() */
#define TPGS_MODE_UNINITIALIZED		 -1
#define TPGS_MODE_NONE			0x0
#define TPGS_MODE_IMPLICIT		0x1
#define TPGS_MODE_EXPLICIT		0x2

#define ALUA_RTPG_SIZE			128 // buff size for report groups command 
#define ALUA_FAILOVER_TIMEOUT		60 // driver value for submit_rtpg req timeout
#define ALUA_FAILOVER_RETRIES		5 // driver value for submit_rtpg scmd retries
#define ALUA_RTPG_DELAY_MSECS		5 // driver value alua_rtpg_queue() delay to queue work for submitted rtpg
#define ALUA_RTPG_RETRY_DELAY		2 // driver value interval for state transistioning

/* device handler flags */
#define ALUA_OPTIMIZE_STPG		0x01 // flag for stpg (--start target port group) command
#define ALUA_RTPG_EXT_HDR_UNSUPP	0x02
/* State machine flags */
#define ALUA_PG_RUN_RTPG		0x10
#define ALUA_PG_RUN_STPG		0x20
#define ALUA_PG_RUNNING			0x40

struct alua_port_group {
	struct kref		kref;
	struct rcu_head		rcu;
	struct list_head	node;
	struct list_head	list;
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

struct alua_data {
	struct list_head	node;
	struct alua_port_group __rcu *pg;
	int			group_id;
	spinlock_t		pg_lock;
	struct scsi_device	*sdev;
	int			init_error;
	struct mutex		init_mutex;
	bool			disabled;
};

int alua_check_tpgs(struct scsi_device *sdev);

struct alua_port_group *alua_alloc_pg(struct scsi_device *sdev,
					     int group_id, int tpgs);

int alua_tur(struct scsi_device *sdev);
void alua_put_pg(struct alua_port_group *pg);
extern bool alua_optimize_stpg;
struct alua_port_group *alua_find_get_pg_locked(char *id_str, size_t id_size,
						int group_id);
char print_alua_state(unsigned char state);
int submit_stpg(struct scsi_device *sdev, int group_id,
		       struct scsi_sense_hdr *sshdr);
int scsi_alua_init(struct scsi_device *sdev);
int alua_rtpg(struct scsi_device *sdev, struct alua_port_group *pg);
#endif // _SCSI_ALUA_H
