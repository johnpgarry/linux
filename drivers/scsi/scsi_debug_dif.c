// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/crc-t10dif.h>
#include <linux/t10-pi.h>
#include <net/checksum.h>
#include "scsi_debug_dif.h"

#define DEF_DIF 0
#define DEF_DIX 0
#define DEF_GUARD 0

int sdebug_dif = DEF_DIF;
int sdebug_dix = DEF_DIX;
unsigned int sdebug_guard = DEF_GUARD;
int dix_writes;
int dix_reads;
int dif_errors;

static __be16 dif_compute_csum(const void *buf, int len)
{
	__be16 csum;

	if (sdebug_guard)
		csum = (__force __be16)ip_compute_csum(buf, len);
	else
		csum = cpu_to_be16(crc_t10dif(buf, len));

	return csum;
}

static int dif_verify(struct t10_pi_tuple *sdt, const void *data,
		      sector_t sector, u32 ei_lba)
{
	__be16 csum = dif_compute_csum(data, sdebug_sector_size);

	if (sdt->guard_tag != csum) {
		pr_err("GUARD check failed on sector %lu rcvd 0x%04x, data 0x%04x\n",
			(unsigned long)sector,
			be16_to_cpu(sdt->guard_tag),
			be16_to_cpu(csum));
		return 0x01;
	}
	if (sdebug_dif == T10_PI_TYPE1_PROTECTION &&
	    be32_to_cpu(sdt->ref_tag) != (sector & 0xffffffff)) {
		pr_err("REF check failed on sector %lu\n",
			(unsigned long)sector);
		return 0x03;
	}
	if (sdebug_dif == T10_PI_TYPE2_PROTECTION &&
	    be32_to_cpu(sdt->ref_tag) != ei_lba) {
		pr_err("REF check failed on sector %lu\n",
			(unsigned long)sector);
		return 0x03;
	}
	return 0;
}

static struct t10_pi_tuple *dif_store(struct sdeb_store_info *sip,
				      sector_t sector)
{
	sector = sector_div(sector, sdebug_store_sectors);

	return sip->dif_storep + sector;
}

static void dif_copy_prot(struct scsi_cmnd *scp, sector_t sector,
			  unsigned int sectors, bool read)
{
	size_t resid;
	void *paddr;
	struct sdeb_store_info *sip = devip2sip((struct sdebug_dev_info *)
						scp->device->hostdata, true);
	struct t10_pi_tuple *dif_storep = sip->dif_storep;
	const void *dif_store_end = dif_storep + sdebug_store_sectors;
	struct sg_mapping_iter miter;

	/* Bytes of protection data to copy into sgl */
	resid = sectors * sizeof(*dif_storep);

	sg_miter_start(&miter, scsi_prot_sglist(scp),
		       scsi_prot_sg_count(scp), SG_MITER_ATOMIC |
		       (read ? SG_MITER_TO_SG : SG_MITER_FROM_SG));

	while (sg_miter_next(&miter) && resid > 0) {
		size_t len = min_t(size_t, miter.length, resid);
		void *start = dif_store(sip, sector);
		size_t rest = 0;

		if (dif_store_end < start + len)
			rest = start + len - dif_store_end;

		paddr = miter.addr;

		if (read)
			memcpy(paddr, start, len - rest);
		else
			memcpy(start, paddr, len - rest);

		if (rest) {
			if (read)
				memcpy(paddr + len - rest, dif_storep, rest);
			else
				memcpy(dif_storep, paddr + len - rest, rest);
		}

		sector += len / sizeof(*dif_storep);
		resid -= len;
	}
	sg_miter_stop(&miter);
}

static void *lba2fake_store(struct sdeb_store_info *sip,
			    unsigned long long lba)
{
	struct sdeb_store_info *lsip = sip;

	lba = do_div(lba, sdebug_store_sectors);
	if (!sip || !sip->storep) {
		WARN_ON_ONCE(true);
		lsip = xa_load(per_store_ap, 0);  /* should never be NULL */
	}
	return lsip->storep + lba * sdebug_sector_size;
}

int prot_verify_read(struct scsi_cmnd *scp, sector_t start_sec,
		     unsigned int sectors, u32 ei_lba)
{
	int ret = 0;
	unsigned int i;
	sector_t sector;
	struct sdeb_store_info *sip = devip2sip((struct sdebug_dev_info *)
						scp->device->hostdata, true);
	struct t10_pi_tuple *sdt;

	for (i = 0; i < sectors; i++, ei_lba++) {
		sector = start_sec + i;
		sdt = dif_store(sip, sector);

		if (sdt->app_tag == cpu_to_be16(0xffff))
			continue;

		/*
		 * Because scsi_debug acts as both initiator and
		 * target we proceed to verify the PI even if
		 * RDPROTECT=3. This is done so the "initiator" knows
		 * which type of error to return. Otherwise we would
		 * have to iterate over the PI twice.
		 */
		if (scp->cmnd[1] >> 5) { /* RDPROTECT */
			ret = dif_verify(sdt, lba2fake_store(sip, sector),
					 sector, ei_lba);
			if (ret) {
				dif_errors++;
				break;
			}
		}
	}

	dif_copy_prot(scp, start_sec, sectors, true);
	dix_reads++;

	return ret;
}

int prot_verify_write(struct scsi_cmnd *SCpnt, sector_t start_sec,
		      unsigned int sectors, u32 ei_lba)
{
	int ret;
	struct t10_pi_tuple *sdt;
	void *daddr;
	sector_t sector = start_sec;
	int ppage_offset;
	int dpage_offset;
	struct sg_mapping_iter diter;
	struct sg_mapping_iter piter;

	BUG_ON(scsi_sg_count(SCpnt) == 0);
	BUG_ON(scsi_prot_sg_count(SCpnt) == 0);

	sg_miter_start(&piter, scsi_prot_sglist(SCpnt),
			scsi_prot_sg_count(SCpnt),
			SG_MITER_ATOMIC | SG_MITER_FROM_SG);
	sg_miter_start(&diter, scsi_sglist(SCpnt), scsi_sg_count(SCpnt),
			SG_MITER_ATOMIC | SG_MITER_FROM_SG);

	/* For each protection page */
	while (sg_miter_next(&piter)) {
		dpage_offset = 0;
		if (WARN_ON(!sg_miter_next(&diter))) {
			ret = 0x01;
			goto out;
		}

		for (ppage_offset = 0; ppage_offset < piter.length;
		     ppage_offset += sizeof(struct t10_pi_tuple)) {
			/* If we're at the end of the current
			 * data page advance to the next one
			 */
			if (dpage_offset >= diter.length) {
				if (WARN_ON(!sg_miter_next(&diter))) {
					ret = 0x01;
					goto out;
				}
				dpage_offset = 0;
			}

			sdt = piter.addr + ppage_offset;
			daddr = diter.addr + dpage_offset;

			if (SCpnt->cmnd[1] >> 5 != 3) { /* WRPROTECT */
				ret = dif_verify(sdt, daddr, sector, ei_lba);
				if (ret)
					goto out;
			}

			sector++;
			ei_lba++;
			dpage_offset += sdebug_sector_size;
		}
		diter.consumed = dpage_offset;
		sg_miter_stop(&diter);
	}
	sg_miter_stop(&piter);

	dif_copy_prot(SCpnt, start_sec, sectors, false);
	dix_writes++;

	return 0;

out:
	dif_errors++;
	sg_miter_stop(&diter);
	sg_miter_stop(&piter);
	return ret;
}

module_param_named(dif, sdebug_dif, int, S_IRUGO);
module_param_named(dix, sdebug_dix, int, S_IRUGO);

MODULE_PARM_DESC(dif, "data integrity field type: 0-3 (def=0)");
MODULE_PARM_DESC(dix, "data integrity extensions mask (def=0)");
