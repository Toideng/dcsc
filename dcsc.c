#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>

MODULE_LICENSE("GPL");

#define DCSC_MINORS 16
#define KERNEL_SECTOR_SIZE 512

static int dcsc_major = 0;
static int hardsect_size = KERNEL_SECTOR_SIZE;
static int nsectors = (128 * 1024 * 1024) / KERNEL_SECTOR_SIZE; // twelvety-eight MiB
static int ndevices = 1;

struct dcsc_dev {
	size_t size;                 /* Device size in sectors */
	u8 *data;                    /* The data array */
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
};

static struct dcsc_dev *Devices = NULL;










static int dcsc_xfer_bvec(
	struct dcsc_dev *dev,
	struct bio_vec *bvec,
	size_t data_dir,
	sector_t cur_sector)
{
	char *buffer = kmap_atomic(bvec->bv_page);
	size_t offset = cur_sector * KERNEL_SECTOR_SIZE;
	size_t len = (bvec->bv_len / KERNEL_SECTOR_SIZE) * KERNEL_SECTOR_SIZE;

	char *inmem = buffer + bvec->bv_offset;
	char *indsk = dev->data + offset;

	if (bvec->bv_len % KERNEL_SECTOR_SIZE)
		printk(KERN_WARNING "%s: "
		       "kernel has requested transfer of a non-integer # of sectors.\xa",
		       dev->gd->disk_name);

	if ((offset + len) > dev->size) {
		printk(KERN_WARNING "Beyond-end write (0x%016lx+0x%016lx)\n",
		       offset, len);
		return -EFAULT;
	}

	if (data_dir == WRITE)
		memcpy(indsk, inmem, len);
	else
		memcpy(inmem, indsk, len);

	kunmap_atomic(buffer);
	return 0;
}



/*
 * Transfer a full request.
 */

static void dcsc_xfer_request(
	struct dcsc_dev *dev,
	struct request *req
	)
{
	struct bio_vec *bvec;
	struct req_iterator iter;

	// Kernel code suggests using `rq_for_each_segment` instead, but how am
	// i to get the `cur_sector` that way?
	__rq_for_each_bio(iter.bio, req) {
		sector_t cur_sector = iter.bio->bi_sector;
		bio_for_each_segment(bvec, iter.bio, iter.i) {
			dcsc_xfer_bvec(dev, bvec, bio_data_dir(iter.bio), cur_sector);
			cur_sector += bvec->bv_len / KERNEL_SECTOR_SIZE;
		}
	}
	return;
}










/*
 * Callback for serving queued requests
 */
static void dcsc_request(
	struct request_queue *q
	)
{
	struct request *req;
	struct dcsc_dev *dev = q->queuedata;
	int ret;

	req = blk_fetch_request(q);
	while (req) {
		if (req->cmd_type != REQ_TYPE_FS) {
			printk (KERN_NOTICE "Skip non-fs request\xa");
			ret = -EIO;
			goto done;
		}
		dcsc_xfer_request(dev, req);
		ret = 0;
done:
		if(!__blk_end_request_cur(req, ret)){
			req = blk_fetch_request(q);
		}
	}
}










/*
 * Open and close.
 */

static int dcsc_open(
	struct block_device *bdev,
	fmode_t mode
	)
{
	return 0;
}



static void dcsc_release(
	struct gendisk *disk,
	fmode_t mode
	)
{
	return;
}










/*
 * The ioctl() implementation
 */

int dcsc_ioctl(
	struct block_device *bdev,
	fmode_t mode,
	unsigned int cmd,
	unsigned long arg
	)
{
	switch(cmd) {
	case HDIO_GETGEO:
	{
		/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.  We set the
		 * start of data at sector four.
		 */
		struct dcsc_dev *dev = bdev->bd_disk->private_data;
		struct hd_geometry geo;
		long size = dev->size * (hardsect_size / KERNEL_SECTOR_SIZE);
		geo.cylinders = size >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof geo))
			return -EFAULT;
		return 0;
	} break;
	}

	return -ENOTTY;
}










/*
 * The device operations structure.
 */

static struct block_device_operations dcsc_ops = {
	.owner           = THIS_MODULE,
	.open            = dcsc_open,
	.release         = dcsc_release,
	.ioctl           = dcsc_ioctl,
};



/*
 * Set up our internal device.
 */

static void setup_device(
	struct dcsc_dev *dev,
	int which
	)
{
	/*
	 * Get some memory.
	 */

	memset(dev, 0, sizeof *dev);
	dev->size = nsectors * hardsect_size;
//	dev->data = kmalloc(dev->size, GFP_KERNEL);
	dev->data = vmalloc(dev->size);
	if (!dev->data) {
//		printk(KERN_NOTICE "kmalloc failure.\xa");
		printk(KERN_NOTICE "vmalloc failure.\xa");
		return;
	}
	memset(dev->data, 0, dev->size);
	spin_lock_init(&dev->lock);

	/*
	 * Setup the I/O queue
	 */

	dev->queue = blk_init_queue(dcsc_request, &dev->lock);
	if (!dev->queue)
		goto out_kfree;

	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;

	/*
	 * Setup the gendisk structure.
	 */

	dev->gd = alloc_disk(DCSC_MINORS);
	if (!dev->gd) {
		printk(KERN_NOTICE "alloc_disk failure\xa");
		goto out_kfree;
	}
	dev->gd->major = dcsc_major;
	dev->gd->first_minor = which * DCSC_MINORS;
	dev->gd->fops = &dcsc_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 6, "dcsc%c", which + 'a');

	set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

out_kfree:
	vfree(dev->data);
	return;
}










static int __init dcsc_init(void)
{
	size_t i;
	printk(KERN_ALERT "dcsc: Initialize the module\xa");

	/*
	 * Get registered.
	 */

	printk(KERN_ALERT "dcsc: Alloc a major number\xa");
	dcsc_major = register_blkdev(dcsc_major, "dcsc");
	if (dcsc_major <= 0) {
		printk(KERN_ALERT "dcsc: failed to get major number\xa");
		printk(KERN_ALERT "dcsc: failed to init\xa");
		return -EBUSY;
	}
	printk(KERN_ALERT "dcsc: got num: %d\xa", dcsc_major);

	/*
	 * Allocate the device array, and initialize each one.
	 */

	printk(KERN_ALERT "dcsc: Initialize the devices\xa");
	Devices = kmalloc(ndevices * sizeof(struct dcsc_dev), GFP_KERNEL);
	if (!Devices)
	{
		printk(KERN_ALERT "dcsc: failed to alloc storage for device descriptors\xa");
		printk(KERN_ALERT "dcsc: failed to kmalloc\xa");
		printk(KERN_ALERT "dcsc: failed to init\xa");
		goto out_unregister;
	}
	for (i = 0;  i < ndevices;  i++) 
		setup_device(Devices + i, i);

	printk(KERN_ALERT "dcsc: Initialize complete and successfull\xa");
	return 0;

out_unregister:
	unregister_blkdev(dcsc_major, "dcsc");
	return -ENOMEM;
}



static void __exit dcsc_exit(void)
{
	size_t i;
	printk(KERN_ALERT "dcsc: Finitialize the module\xa");

	for (i = 0; i < ndevices; i++) {
		struct dcsc_dev *dev = Devices + i;
		printk(KERN_ALERT "dcsc: Finitialize the device %s\xa", dev->gd->disk_name);

		printk(KERN_ALERT "dcsc: Finitialize the device %s: dealloc disk\xa", dev->gd->disk_name);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		printk(KERN_ALERT "dcsc: Finitialize the device %s: dealloc queue\xa", dev->gd->disk_name);
		if (dev->queue)
			blk_cleanup_queue(dev->queue);
		printk(KERN_ALERT "dcsc: Finitialize the device %s: dealloc storage\xa", dev->gd->disk_name);
		if (dev->data)
			vfree(dev->data);
	}
	printk(KERN_ALERT "dcsc: Finitialize: unregister major num (%d)\xa", dcsc_major);
	unregister_blkdev(dcsc_major, "dcsc");
	kfree(Devices);

	printk(KERN_ALERT "dcsc: Finitialize complete\xa");
}



module_init(dcsc_init);
module_exit(dcsc_exit);
