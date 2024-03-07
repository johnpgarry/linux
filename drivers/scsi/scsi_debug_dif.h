/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_DEBUG_DIF_H
#define _SCSI_DEBUG_DIF_H

#include <linux/kconfig.h>
#include <linux/types.h>
#include <linux/spinlock_types.h>
#include <scsi/scsi_cmnd.h>

struct sdebug_dev_info;
struct t10_pi_tuple;

extern int dix_writes;
extern int dix_reads;
extern int dif_errors;
extern struct xarray *const per_store_ap;
extern int sdebug_dif;
extern int sdebug_dix;
extern unsigned int sdebug_guard;
extern int sdebug_sector_size;
extern unsigned int sdebug_store_sectors;

/* There is an xarray of pointers to this struct's objects, one per host */
struct sdeb_store_info {
	rwlock_t macc_lck;	/* for atomic media access on this store */
	u8 *storep;		/* user data storage (ram) */
	struct t10_pi_tuple *dif_storep; /* protection info */
	void *map_storep;	/* provisioning map */
};

struct sdeb_store_info *devip2sip(struct sdebug_dev_info *devip,
				  bool bug_if_fake_rw);

#if IS_ENABLED(CONFIG_CRC_T10DIF)

int prot_verify_read(struct scsi_cmnd *scp, sector_t start_sec,
		     unsigned int sectors, u32 ei_lba);
int prot_verify_write(struct scsi_cmnd *SCpnt, sector_t start_sec,
		      unsigned int sectors, u32 ei_lba);

#else /* CONFIG_CRC_T10DIF */

static inline int prot_verify_read(struct scsi_cmnd *scp, sector_t start_sec,
				   unsigned int sectors, u32 ei_lba)
{
	return 0x01; /* GUARD check failed */
}

static inline int prot_verify_write(struct scsi_cmnd *SCpnt, sector_t start_sec,
				    unsigned int sectors, u32 ei_lba)
{
	return 0x01; /* GUARD check failed */
}

#endif /* CONFIG_CRC_T10DIF */

#endif /* _SCSI_DEBUG_DIF_H */
