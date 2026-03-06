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

#include "scsi_alua.h"
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

static LIST_HEAD(scsi_mpath_heads_list);
static DEFINE_MUTEX(scsi_mpath_heads_lock);
static DEFINE_IDA(scsi_multipath_dev_ida);

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

module_param_cb(scsi_multipath, &multipath_param_ops, &scsi_multipath, 0444);
MODULE_PARM_DESC(scsi_multipath, "turn on native multipath support, options: on, off, always");

static int iopolicy = MPATH_IOPOLICY_NUMA;

static int scsi_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	return mpath_set_iopolicy(val, &iopolicy);
}

static int scsi_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return mpath_get_iopolicy(buf, iopolicy);
}

module_param_call(iopolicy, scsi_set_iopolicy, scsi_get_iopolicy,
	&iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");

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

static void scsi_mpath_delete_head(struct scsi_mpath_head *scsi_mpath_head)
{
	mutex_lock(&scsi_mpath_heads_lock);
	list_del_init(&scsi_mpath_head->entry);
	mutex_unlock(&scsi_mpath_heads_lock);
}

static void scsi_mpath_head_release(struct device *dev)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = scsi_mpath_head->mpath_head;

	scsi_mpath_delete_head(scsi_mpath_head);
	bioset_exit(&scsi_mpath_head->bio_pool);
	ida_free(&scsi_multipath_dev_ida, scsi_mpath_head->index);
	mpath_put_head(mpath_head);
	pr_err("%s3 calling kfree(scsi_mpath_head)\n", __func__);
	kfree(scsi_mpath_head);
}

static ssize_t scsi_mpath_device_wwid_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return sysfs_emit(buf, "%s\n", scsi_mpath_head->wwid);
}

static DEVICE_ATTR(wwid, S_IRUGO, scsi_mpath_device_wwid_show, NULL);

void scsi_mpath_dev_clear_path(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
	struct scsi_mpath_head *scsi_mpath_head = scsi_mpath_dev->scsi_mpath_head;
	struct mpath_head *mpath_head = scsi_mpath_head->mpath_head;

	if (mpath_clear_current_path(mpath_device))
		mpath_synchronize(mpath_head);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_clear_path);

static ssize_t scsi_mpath_device_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = scsi_mpath_head->mpath_head;

	if (!mpath_iopolicy_store(&scsi_mpath_head->iopolicy, buf, count))
		return -EINVAL;

	mpath_clear_paths(mpath_head);
	mpath_schedule_requeue_work(mpath_head);
	return count;
}

static ssize_t scsi_mpath_device_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return mpath_iopolicy_show(&scsi_mpath_head->iopolicy, buf);
}

static DEVICE_ATTR(iopolicy, S_IRUGO | S_IWUSR,
		scsi_mpath_device_iopolicy_show, scsi_mpath_device_iopolicy_store);

static struct attribute *scsi_mpath_device_attrs[] = {
	&dev_attr_wwid.attr,
	&dev_attr_iopolicy.attr,
	NULL
};

static const struct attribute_group scsi_mpath_device_attrs_group = {
	.attrs = scsi_mpath_device_attrs,
};

static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *scsi_mpath_attrs[] = {
	&dummy_attr,
	NULL
};

static bool scsi_multipath_sysfs_group_visible(struct kobject *kobj)
{
	return true;
}

static bool scsi_multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	return false;
}
DEFINE_SYSFS_GROUP_VISIBLE(scsi_multipath_sysfs)

static const struct attribute_group scsi_mpath_attr_group = {
	.name           = "multipath",
	.attrs		= scsi_mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(scsi_multipath_sysfs),
};

const struct attribute_group *scsi_mpath_device_groups[] = {
	&scsi_mpath_device_attrs_group,
	&scsi_mpath_attr_group,
	NULL
};

void scsi_mpath_add_sysfs_link(struct scsi_device *sdev)
{
	struct device *target = &sdev->sdev_gendev;
	struct scsi_mpath_head *scsi_mpath_head =
		sdev->scsi_mpath_dev->scsi_mpath_head;
	struct device *source = &scsi_mpath_head->dev;
	int error;

	error = sysfs_add_link_to_group(&source->kobj, "multipath",
			&target->kobj, dev_name(target));
	if (error) {
		sdev_printk(KERN_INFO, sdev, "Failed to create mpath sysfs link, errno=%d\n",
				    error);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_sysfs_link);

void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev)
{
	struct device *target = &sdev->sdev_gendev;
	struct scsi_mpath_head *scsi_mpath_head =
		sdev->scsi_mpath_dev->scsi_mpath_head;
	struct device *source = &scsi_mpath_head->dev;

	sysfs_remove_link_from_group(&source->kobj, "multipath",
		dev_name(target));
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_sysfs_link);

static const struct class scsi_mpath_device_class = {
	.name = "scsi_mpath_device",
	.dev_groups = scsi_mpath_device_groups,
	.dev_release = scsi_mpath_head_release,
};

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

static void scsi_mpath_clone_end_io(struct bio *clone)
{
	struct bio *master_bio = clone->bi_private;

	master_bio->bi_status = clone->bi_status;
	bio_put(clone);
	bio_endio(master_bio);
}

static struct bio *scsi_mpath_clone_bio(struct bio *bio)
{
	struct mpath_head *mpath_head = bio->bi_bdev->bd_disk->private_data;
	struct scsi_mpath_head *scsi_mpath_head = mpath_head->drvdata;
	struct bio *clone;

	clone = bio_alloc_clone(bio->bi_bdev, bio, GFP_NOWAIT,
				&scsi_mpath_head->bio_pool);
	if (!clone)
		return NULL;

	clone->bi_end_io = scsi_mpath_clone_end_io;
	clone->bi_private = bio;

	return clone;
}

static enum mpath_iopolicy_e scsi_mpath_get_iopolicy(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = mpath_head->drvdata;

	return mpath_read_iopolicy(&scsi_mpath_head->iopolicy);
}

static int scsi_mpath_ioctl(struct block_device *bdev,
			struct mpath_device *mpath_device,
			blk_mode_t mode, unsigned int cmd,
			unsigned long arg, int srcu_idx)
{
	struct gendisk *disk = bdev->bd_disk;
	struct mpath_head *mpath_head = mpath_gendisk_to_disk(disk);
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);
	int err;

	err = drv->mpath_ioctl(sdev, mode & BLK_OPEN_WRITE, cmd, arg);

	mpath_head_read_unlock(mpath_head, srcu_idx);
	return err;
}

static bool scsi_mpath_is_disabled(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	enum scsi_device_state sdev_state = sdev->sdev_state;

	if (sdev_state == SDEV_RUNNING || sdev_state == SDEV_CANCEL)
		return false;

	return true;
}

static bool scsi_mpath_is_optimized(struct mpath_device *mpath_device)
{
	if (scsi_mpath_is_disabled(mpath_device))
		return false;
	return true;
}

static bool scsi_mpath_available_path(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;

	if (scsi_device_blocked(sdev))
		return false;

	return scsi_device_online(sdev);
}

static int scsi_mpath_pr_register(struct mpath_device *mpath_device,
			u64 old_key, u64 new_key, u32 flags)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_register(sdev, old_key, new_key, flags);
}

static int scsi_mpath_pr_reserve(struct mpath_device *mpath_device, u64 key,
			enum pr_type type, u32 flags)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_reserve(sdev, key, type, flags);
}

static int scsi_mpath_pr_release(struct mpath_device *mpath_device, u64 key,
			enum pr_type type)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_release(sdev, key, type);
}

static int scsi_mpath_pr_preempt(struct mpath_device *mpath_device,
			u64 old_key, u64 new_key, enum pr_type type,
			bool abort)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_preempt(sdev, old_key, new_key,
			type, abort);
}

static int scsi_mpath_pr_clear(struct mpath_device *mpath_device, u64 key)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_clear(sdev, key);
}

static int scsi_mpath_pr_read_keys(struct mpath_device *mpath_device,
				struct pr_keys *keys_info)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_read_keys(sdev, keys_info);
}

static int scsi_mpath_pr_read_reservation(struct mpath_device *mpath_device,
				  struct pr_held_reservation *rsv)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);

	if (!drv->mpath_pr_ops)
		return -EOPNOTSUPP;

	return drv->mpath_pr_ops->pr_read_reservation(sdev, rsv);
}

static int scsi_mpath_get_nr_active(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);

	return atomic_read(&scsi_mpath_dev->nr_active);
}

static const struct mpath_pr_ops scsi_mpath_pr_ops = {
	.pr_register	= scsi_mpath_pr_register,
	.pr_reserve	= scsi_mpath_pr_reserve,
	.pr_release	= scsi_mpath_pr_release,
	.pr_preempt	= scsi_mpath_pr_preempt,
	.pr_clear	= scsi_mpath_pr_clear,
	.pr_read_keys	= scsi_mpath_pr_read_keys,
	.pr_read_reservation = scsi_mpath_pr_read_reservation,
};

struct mpath_head_template smpdt_pr = {
	.is_disabled = scsi_mpath_is_disabled,
	.is_optimized = scsi_mpath_is_optimized,
	.bdev_ioctl = scsi_mpath_ioctl,
	.available_path = scsi_mpath_available_path,
	.get_iopolicy = scsi_mpath_get_iopolicy,
	.clone_bio = scsi_mpath_clone_bio,
	.pr_ops = &scsi_mpath_pr_ops,
	.device_groups = mpath_device_groups,
	.get_nr_active = scsi_mpath_get_nr_active,
};

static struct scsi_mpath_head *scsi_mpath_alloc_head(void)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int ret;

	scsi_mpath_head = kzalloc(sizeof(*scsi_mpath_head), GFP_KERNEL);
	if (!scsi_mpath_head)
		return NULL;

	ida_init(&scsi_mpath_head->ida);
	mutex_init(&scsi_mpath_head->lock);

	if (bioset_init(&scsi_mpath_head->bio_pool, BIO_POOL_SIZE,
			0, BIOSET_PERCPU_CACHE))
		goto out_free;
	scsi_mpath_head->mpath_head = mpath_alloc_head();
	if (IS_ERR(scsi_mpath_head->mpath_head))
		goto out_bioset_exit;
	scsi_mpath_head->mpath_head->mpdt = &smpdt_pr;
	scsi_mpath_head->mpath_head->drvdata = scsi_mpath_head;

	scsi_mpath_head->index = ida_alloc(&scsi_multipath_dev_ida, GFP_KERNEL);
	if (scsi_mpath_head->index < 0)
		goto out_put_head;

	device_initialize(&scsi_mpath_head->dev);
	scsi_mpath_head->dev.class = &scsi_mpath_device_class;
	ret = dev_set_name(&scsi_mpath_head->dev, "%d", scsi_mpath_head->index);
	if (ret) {
		put_device(&scsi_mpath_head->dev);
		goto out_free_ida;
	}

	return scsi_mpath_head;

out_free_ida:
	ida_free(&scsi_multipath_dev_ida, scsi_mpath_head->index);
out_put_head:
	mpath_put_head(scsi_mpath_head->mpath_head);
out_bioset_exit:
	bioset_exit(&scsi_mpath_head->bio_pool);
out_free:
	kfree(scsi_mpath_head);
	return NULL;
}

static struct scsi_mpath_head *scsi_mpath_find_head(
			struct scsi_mpath_device *scsi_mpath_dev)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int ret;

	mutex_lock(&scsi_mpath_heads_lock);
	list_for_each_entry(scsi_mpath_head, &scsi_mpath_heads_list, entry) {
		ret = scsi_mpath_get_head(scsi_mpath_head);
		if (ret)
			continue;
		if (strncmp(scsi_mpath_head->wwid,
			scsi_mpath_dev->device_id_str,
			SCSI_MPATH_DEVICE_ID_LEN) == 0) {

			mutex_unlock(&scsi_mpath_heads_lock);
			return scsi_mpath_head;
		}
		scsi_mpath_put_head(scsi_mpath_head);
	}

	return NULL;
}

static void scsi_multipath_sdev_uninit(struct scsi_device *sdev)
{
	kfree(sdev->scsi_mpath_dev);
	sdev->scsi_mpath_dev = NULL;
}

int scsi_mpath_dev_alloc(struct scsi_device *sdev)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int rel_port = -1, group_id, tpgs;
	int ret;

	if (scsi_multipath == SCSI_MULTIPATH_OFF)
		return 0;

	tpgs = alua_check_tpgs(sdev);
	if (!(tpgs & TPGS_MODE_IMPLICIT) && (scsi_multipath != SCSI_MULTIPATH_ALWAYS)) {
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

	scsi_mpath_head = scsi_mpath_find_head(sdev->scsi_mpath_dev);
	if (scsi_mpath_head)
		goto found;
	/* scsi_mpath_disks_list lock held */
	scsi_mpath_head = scsi_mpath_alloc_head();
	if (!scsi_mpath_head)
		goto out_uninit;

	strcpy(scsi_mpath_head->wwid, sdev->scsi_mpath_dev->device_id_str);

	ret = device_add(&scsi_mpath_head->dev);
	if (ret)
		goto out_put_head;

	list_add_tail(&scsi_mpath_head->entry, &scsi_mpath_heads_list);

	mutex_unlock(&scsi_mpath_heads_lock);
	sdev->scsi_mpath_dev->scsi_mpath_head = scsi_mpath_head;

found:
	group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	sdev_printk(KERN_NOTICE, sdev, "group_id=%d rel_port=%d\n",
			rel_port, rel_port);

	sdev->scsi_mpath_dev->index = ida_alloc(&scsi_mpath_head->ida, GFP_KERNEL);
	if (sdev->scsi_mpath_dev->index < 0) {
		ret = sdev->scsi_mpath_dev->index;
		goto out_put_head;
	}

	mutex_lock(&scsi_mpath_head->lock);
	scsi_mpath_head->dev_count++;
	mutex_unlock(&scsi_mpath_head->lock);

	sdev->scsi_mpath_dev->scsi_mpath_head = scsi_mpath_head;
	return 0;

out_put_head:
	scsi_mpath_put_head(scsi_mpath_head);
out_uninit:
	mutex_unlock(&scsi_mpath_heads_lock);
	scsi_multipath_sdev_uninit(sdev);
	return ret;
}

static void scsi_mpath_remove_head(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct scsi_mpath_head *scsi_mpath_head =
			scsi_mpath_dev->scsi_mpath_head;
	bool last_path = false;

	mutex_lock(&scsi_mpath_head->lock);
	scsi_mpath_head->dev_count--;
	if (scsi_mpath_head->dev_count == 0)
		last_path = true;
	mutex_unlock(&scsi_mpath_head->lock);

	if (last_path)
		device_del(&scsi_mpath_head->dev);

	scsi_mpath_dev->scsi_mpath_head = NULL;
	scsi_mpath_put_head(scsi_mpath_head);
}

void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct scsi_mpath_head *scsi_mpath_head = scsi_mpath_dev->scsi_mpath_head;

	ida_free(&scsi_mpath_head->ida, scsi_mpath_dev->index);

	scsi_mpath_remove_head(scsi_mpath_dev);
}

void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;

	if (!scsi_mpath_dev)
		return;

	scsi_multipath_sdev_uninit(sdev);
}

int scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
	if (!get_device(&scsi_mpath_head->dev))
		return -ENXIO;
	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_get_head);

void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head)
{
	struct device *dev = &scsi_mpath_head->dev;
	struct kobject *kobj = &dev->kobj;
	struct kref *kref = &kobj->kref;
//	struct kref *ref = &scsi_mpath_head->ref;
	pr_err("%s calling put_device refcount=%d scsi_mpath_head=%pS\n",
		__func__, refcount_read(&kref->refcount), scsi_mpath_head);
	put_device(&scsi_mpath_head->dev);
}
EXPORT_SYMBOL_GPL(scsi_mpath_put_head);

bool scsi_is_mpath_request(struct request *req)
{
	return is_mpath_request(req);
}
EXPORT_SYMBOL_GPL(scsi_is_mpath_request);

static inline void bio_list_add_clone_master(struct bio_list *bl,
				struct bio *clone)
{
	struct bio *master_bio;

	if (clone->bi_next)
		bio_list_add_clone_master(bl, clone->bi_next);

	master_bio = clone->bi_private;

	if (bl->tail)
		bl->tail->bi_next = master_bio;
	else
		bl->head = master_bio;

	bl->tail = master_bio;

	bio_put(clone);
}

void scsi_mpath_failover_req(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct scsi_driver *drv = to_scsi_driver(sdev->sdev_gendev.driver);
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct mpath_head *mpath_head = drv->to_mpath_head(req);
	unsigned long flags;

	scsi_mpath_dev_clear_path(scsi_mpath_dev);

	spin_lock_irqsave(&mpath_head->requeue_lock, flags);
	bio_list_add_clone_master(&mpath_head->requeue_list, req->bio);
	spin_unlock_irqrestore(&mpath_head->requeue_lock, flags);
	req->bio = NULL;
	req->biotail = NULL;
	req->__data_len = 0;

	/* End old request with clone detached */
	scmd->result = 0;
	blk_mq_end_request(req, 0);

	mpath_schedule_requeue_work(mpath_head);
}

static inline bool scsi_is_mpath_error(struct scsi_cmnd *scmd)
{
	struct scsi_device *sdev = scmd->device;

	if (sdev->sdev_state == SDEV_TRANSPORT_OFFLINE)
		return true;
	return false;
}

int scsi_mpath_failover_disposition(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);

	if (is_mpath_request(req)) {
		if (scsi_is_mpath_error(scmd) ||
		    blk_queue_dying(req->q))
			return FAILOVER;
		return NEEDS_RETRY;
	} else {
		if (blk_queue_dying(req->q))
			return SUCCESS;
	}

	return SUCCESS;
}

int __init scsi_multipath_init(void)
{
	return class_register(&scsi_mpath_device_class);
}

void __exit scsi_multipath_exit(void)
{
	class_unregister(&scsi_mpath_device_class);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_multipath");
