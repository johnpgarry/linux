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

bool scsi_multipath;
static bool scsi_multipath_always;

static int multipath_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	if (ret)
		return ret;

	if (scsi_multipath_always && !*arg) {
		pr_err("Can't disable multipath when multipath_always_on is configured.\n");
		*arg = true;
		return -EINVAL;
	}

	return 0;
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = multipath_param_set,
	.get = param_get_bool,
};

module_param_cb(scsi_multipath, &multipath_param_ops, &scsi_multipath, 0444);
MODULE_PARM_DESC(scsi_multipath, "turn on native multipath support");

static int multipath_always_on_set(const char *val,
		const struct kernel_param *kp)
{
	int ret;
	bool *arg = kp->arg;

	ret = param_set_bool(val, kp);
	if (ret < 0)
		return ret;

	if (*arg)
		scsi_multipath = true;

	return 0;
}

static const struct kernel_param_ops multipath_always_on_ops = {
	.set = multipath_always_on_set,
	.get = param_get_bool,
};

module_param_cb(scsi_multipath_always, &multipath_always_on_ops,
		&scsi_multipath_always, 0444);
MODULE_PARM_DESC(scsi_multipath_always,
	"create multipath node always even for no ALUA support");

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

	if (!scsi_multipath)
		return 0;

	if (!scsi_device_tpgs(sdev) && !scsi_multipath_always) {
		sdev_printk(KERN_NOTICE, sdev, "tpgs are required for multipath support\n");
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
