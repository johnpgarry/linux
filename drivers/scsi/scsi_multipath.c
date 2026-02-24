// SPDX-License-Indentifier: GPL-2.0
/*
 * Copyright (c) 2026 Oracle Corp
 *
 */

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_multipath.h>

#include "scsi_priv.h"

enum {
	SCSI_MULTIPATH_OFF,
	SCSI_MULTIPATH_ON,
	SCSI_MULTIPATH_ALWAYS,
};

static const char *scsi_multipath_modes[] = {
	[SCSI_MULTIPATH_OFF]	= "off",
	[SCSI_MULTIPATH_ON]	= "on",
	[SCSI_MULTIPATH_ALWAYS]	= "always",
};

static int scsi_multipath = SCSI_MULTIPATH_OFF;

static int scsi_multipath_param_set(const char *val, const struct kernel_param *kp)
{
	if (!val)
		return -EINVAL;
	if (!strncmp(val, "on", 2))
		scsi_multipath = SCSI_MULTIPATH_ON;
	else if (!strncmp(val, "always", 6))
		scsi_multipath = SCSI_MULTIPATH_ALWAYS;
	else if (!strncmp(val, "off", 3))
		scsi_multipath = SCSI_MULTIPATH_OFF;
	else
		return -EINVAL;

	return 0;
}

static int scsi_multipath_param_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", scsi_multipath_modes[scsi_multipath]);
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = scsi_multipath_param_set,
	.get = scsi_multipath_param_get,
};

module_param_cb(multipath, &multipath_param_ops, &scsi_multipath, 0444);
MODULE_PARM_DESC(multipath, "turn on native multipath support, options: on, off, always");

static int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	int ret;

	ret = scsi_vpd_lun_id(sdev, scsi_mpath_dev->device_id_str,
				SCSI_MPATH_DEVICE_ID_LEN);
	if (ret < 0)
		return ret;

	return 0;
}

static int scsi_multipath_sdev_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct mpath_device *mpath_device;

	scsi_mpath_dev = kzalloc(sizeof(*scsi_mpath_dev), GFP_KERNEL);
	if (!scsi_mpath_dev)
		return -ENOMEM;
	scsi_mpath_dev->sdev = sdev;
	sdev->scsi_mpath_dev = scsi_mpath_dev;

	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_device->numa_node = dev_to_node(shost->dma_dev);
	mpath_device->access_state = MPATH_STATE_OPTIMIZED;

	return 0;
}

static void scsi_multipath_sdev_uninit(struct scsi_device *sdev)
{
	kfree(sdev->scsi_mpath_dev);
	sdev->scsi_mpath_dev = NULL;
}

int scsi_mpath_dev_alloc(struct scsi_device *sdev)
{
	int ret;

	if (scsi_multipath == SCSI_MULTIPATH_OFF)
		return 0;

	if (!scsi_device_tpgs(sdev) && (scsi_multipath != SCSI_MULTIPATH_ALWAYS)) {
		sdev_printk(KERN_DEBUG, sdev, "IMPLICIT TPGS are required for multipath support\n");
		return 0;
	}

	ret = scsi_multipath_sdev_init(sdev);
	if (ret)
		return ret;

	ret = scsi_mpath_unique_lun_id(sdev);
	if (ret < 0) {
		ret = 0;
		goto out_uninit;
	}

	return 0;

out_uninit:
	scsi_multipath_sdev_uninit(sdev);
	return ret;
}

void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;

	if (!scsi_mpath_dev)
		return;

	scsi_multipath_sdev_uninit(sdev);
}

int __init scsi_multipath_init(void)
{
	return 0;
}

void __exit scsi_multipath_exit(void)
{
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_multipath");
