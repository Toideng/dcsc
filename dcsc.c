#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/string.h>

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

MODULE_LICENSE("Dual BSD/GPL");

#define DCSC_MINORS 16
#define MAXDEVICES 16
#define KERNEL_SECTOR_SIZE 512

#if 1
#define MARKENTER(f) printk(KERN_DEBUG "dcsc: enter " #f "\xa");
#define MARKLEAVE(f) printk(KERN_DEBUG "dcsc: leave " #f "\xa");
#else
#define MARKENTER(f)
#define MARKLEAVE(f)
#endif

static int dcsc_major = 0; // allocate major number at runtime
static int default_size = 128 * 1024 * 1024; // twelvety-eight MiB in bytes

// Chooses between requiring user to create all devices or creating one default
// device at startup
static int interactive_creation_allowed = 0;
module_param(interactive_creation_allowed, int, 0);

// Only needed for sysfs manipulations
struct testbus_driver {
	char *name;
	struct driver_attribute createnewdevice_attr;
	struct device_driver driver;
} dcsc_driver = {"dcsc_driver"};

// Main device 'descriptor', contains all needed info
struct dcsc_dev {
	char *name;
	int access_mode;             /* 0 for read-write, other for read-only */
	struct device_attribute access_attr;
	struct testbus_driver *driver;
	struct device_attribute size_attr;
	size_t size;                 /* Current device size in sectors */
	size_t size_cap;             /* Maximum device size in sectors,
	                              * i.e. allocated storage area for `data` */
	u8 *data;                    /* The data array */
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
	struct device dev;
};

// An array of managed devices
static struct {
	size_t n_devices;
	size_t cap_devices;
	struct dcsc_dev *Devices;
} Devices = {0, MAXDEVICES, NULL};



// Forward-declarations for including in structures and functions
static int setup_device(
	struct dcsc_dev *dev,
	int which,
	char const *name,
	size_t name_len,
	size_t device_size
);

int new_device(
	char const *name,
	size_t name_len,
	size_t device_size
);

void testbus_release(
	struct device *dev
);

/*
 * I have created a bus of my own since i couldn't find a method to create a
 * virtual device on a real bus like pci or usb.
 */
static struct device testbus = {
	.init_name   = "testbus",
	.release  = testbus_release
};

static struct bus_type testbus_type = {
	.name = "testbus",
};










/*
 * Functions for request management and data transfers
 */



// Transfer a single bvec
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

	MARKENTER(dcsc_xfer_bvec);

	if (bvec->bv_len % KERNEL_SECTOR_SIZE)
		printk(KERN_WARNING "%s: "
		       "kernel has requested transfer of a non-integer # of sectors.\xa",
		       dev->name);

	if ((offset + len) > dev->size * KERNEL_SECTOR_SIZE) {
		printk(KERN_WARNING "Beyond-end write (0x%016lx+0x%016lx)\n",
		       offset, len);
		return -EFAULT;
	}

	if (data_dir == WRITE)
		memcpy(indsk, inmem, len);
	else
		memcpy(inmem, indsk, len);

	kunmap_atomic(buffer);
	MARKLEAVE(dcsc_xfer_bvec);

	return 0;
}



// Transfer a full request.
static void dcsc_xfer_request(
	struct dcsc_dev *dev,
	struct request *req
	)
{
	struct bio_vec *bvec;
	struct req_iterator iter;

	MARKENTER(dcsc_xfer_request);

	// Kernel code suggests using `rq_for_each_segment` instead, but how am
	// i to get the `cur_sector` that way?
	__rq_for_each_bio(iter.bio, req) {
		sector_t cur_sector = iter.bio->bi_sector;
		bio_for_each_segment(bvec, iter.bio, iter.i) {
			dcsc_xfer_bvec(dev, bvec, bio_data_dir(iter.bio), cur_sector);
			cur_sector += bvec->bv_len / KERNEL_SECTOR_SIZE;
		}
	}
	MARKLEAVE(dcsc_xfer_request);
	return;
}



// Callback for serving queued requests
static void dcsc_request(
	struct request_queue *q
	)
{
	struct request *req;
	struct dcsc_dev *dev = q->queuedata;
	int ret;

	MARKENTER(dcsc_request);

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
	MARKLEAVE(dcsc_request);
}










/*
 * Bus, device and driver management in sysfs. Adds required attributes
 */



void testbus_release(
	struct device *dev
	)
{
	MARKENTER(testbus_release);
	return;
}

void testbus_dev_release(
	struct device *dev
	)
{
	MARKENTER(testbus_dev_release);
	return;
}

// 'Size' attribute allows user to shrink/grow device size, yet does nothing to
// reduce memory usage

ssize_t show_size_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char *buf
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	MARKENTER(show_size_attr);
	return snprintf(buf, PAGE_SIZE, "%lu\xa", dev->size);
}

ssize_t store_size_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char const *buf,
	size_t count
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	size_t size = 0;
	size_t i;

	MARKENTER(store_size_attr);

	for (i = 0;  i < count;  i++)
	{
		if (buf[i] == 0xa)
			break;
		if (buf[i] < '0' || buf[i] > '9')
			return -EINVAL;
		size *= 10;
		size += buf[i] - '0';
	}

	if (size > dev->size_cap)
		return -EINVAL;
	if (size % KERNEL_SECTOR_SIZE)
		return -EINVAL;
	dev->size = size / KERNEL_SECTOR_SIZE;

	return 0;
}

// 'Access' attribute allows user to change access mode for the device

ssize_t show_access_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char *buf
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	MARKENTER(show_access_attr);
	return snprintf(buf, PAGE_SIZE, "%d\xa", !!dev->access_mode);
}

ssize_t store_access_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char const *buf,
	size_t count
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	MARKENTER(store_access_attr);

	if (count < 1)
		return -EINVAL;
	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;

	dev->access_mode = buf[0] - '0';
	return 0;
}

// Registration of a device in sysfs for creating attributes

int register_testbus_device(
	struct dcsc_dev *dev
	)
{
	int res;

	MARKENTER(register_testbus_device);

	dev->dev.bus = &testbus_type;
	dev->dev.parent = &testbus;
	dev->dev.release = testbus_dev_release;
	dev_set_name(&dev->dev, "%s", dev->name);

	res = device_register(&dev->dev);
	if (res)
		return res;

	dev->size_attr.attr.name = "size";
	dev->size_attr.attr.mode = S_IRUGO | S_IWUGO;
	dev->size_attr.show = show_size_attr;
	dev->size_attr.store = store_size_attr;

	res = device_create_file(&dev->dev, &dev->size_attr);
	if (res)
		return res;

	dev->access_attr.attr.name = "access";
	dev->access_attr.attr.mode = S_IRUGO | S_IWUGO;
	dev->access_attr.show = show_access_attr;
	dev->access_attr.store = store_access_attr;

	res = device_create_file(&dev->dev, &dev->access_attr);
	if (res)
		return res;

	return 0;
}

void unregister_testbus_device(
	struct dcsc_dev *dev
	)
{
	MARKENTER(unregister_testbus_device);
	device_unregister(&dev->dev);
}

// Attribute 'createnewdevice' allows user, if the flag was checked at module's
// load time, to create new devices of specified sizes.

static ssize_t show_createnewdevice_attr(
	struct device_driver *driver,
	char *buf
	)
{
	MARKENTER(show_createnewdevice_attr);
	return snprintf(buf, PAGE_SIZE, "Specify a device name and size in KiB, e.g. \"dcscb 12*1024\"\xa");
}

static ssize_t store_createnewdevice_attr(
	struct device_driver *driver,
	char const *buf,
	size_t count
	)
{
	size_t i;
	char const *namestart;
	char const *sizestart;
	size_t namelen;
	size_t sizelen;
	size_t size;
	size_t multiplier;

	MARKENTER(store_createnewdevice_attr);

	// check format for being "[[:alnum:]]+\x20[0-9*\x20]+"
	namestart = buf;
	for (i = 0;  i < count;  i++) {
		if (buf[i] == 0x20)
			break;
		if ((buf[i] >= '0' && buf[i] <= '9')
		 || (buf[i] >= 'a' && buf[i] <= 'z')
		 || (buf[i] >= 'A' && buf[i] <= 'Z'))
		{
			continue;
		}

		return -EINVAL;
	}
	if (i == count)
		return -EINVAL;
	namelen = i;

	i++;
	sizestart = buf + i;
	for (;  i < count;  i++) {
		if (buf[i] == 0x20
		 || buf[i] == 0x2a
		 || (buf[i] >= '0' && buf[i] <= '9'))
		{
			continue;
		}

		return -EINVAL;
	}
	sizelen = (buf + i) - sizestart;

	if (!namelen || !sizelen)
		return -EINVAL;

	size = 1024;
	multiplier = 0;
	for (i = 0;  i < sizelen;  i++) {
		if (sizestart[i] == 0x20)
			continue;
		if (sizestart[i] == 0x2a) {
			size *= multiplier;
			multiplier = 0;
			continue;
		}

		multiplier *= 10;
		multiplier += sizestart[i] - '0';
	}
	size *= multiplier;

	if (size == 0)
		return -EINVAL;

	// at this point, `size` has numerical size and
	// `namestart[0..namelen-1]` has text name for the new device

	return new_device(namestart, namelen, size);
}



// Registration of the driver in sysfs for creating attributes

int register_testbus_driver(
	struct testbus_driver *driver
	)
{
	int ret;

	MARKENTER(register_testbus_driver);

	driver->driver.name = driver->name;
	driver->driver.bus = &testbus_type;

	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	if (interactive_creation_allowed)
	{
		driver->createnewdevice_attr.attr.name = "createnewdevice";
		driver->createnewdevice_attr.attr.mode = S_IRUGO | S_IWUGO;
		driver->createnewdevice_attr.show = show_createnewdevice_attr;
		driver->createnewdevice_attr.store = store_createnewdevice_attr;

		ret = driver_create_file(&driver->driver, &driver->createnewdevice_attr);
		if (ret)
			return ret;
	}

	return 0;
}

void unregister_testbus_driver(
	struct testbus_driver *driver
	)
{
	MARKENTER(unregister_testbus_driver);
	driver_unregister(&driver->driver);
}










/*
 * Open and close. Both don't do much useful, but are required by the API.
 */

static int dcsc_open(
	struct block_device *bdev,
	fmode_t mode
	)
{
	MARKENTER(dcsc_open);
	return 0;
}



static void dcsc_release(
	struct gendisk *disk,
	fmode_t mode
	)
{
	MARKENTER(dcsc_release);
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
	MARKENTER(dcsc_ioctl);
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
		long size = dev->size;
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
 * At this point, everything needed for the setup of this structure has been
 * defined.
 */

static struct block_device_operations dcsc_ops = {
	.owner           = THIS_MODULE,
	.open            = dcsc_open,
	.release         = dcsc_release,
	.ioctl           = dcsc_ioctl,
};










/*
 * Setup function, does some sanity checks and then creates a device. May fail,
 * check return value.
 */

static int setup_device(
	struct dcsc_dev *dev,
	int which,
	char const *name,
	size_t name_len,
	size_t device_size
	)
{
	int res;

	MARKENTER(setup_device);

	if (which >= Devices.cap_devices)
		return -EBADSLT;

	if (device_size % KERNEL_SECTOR_SIZE)
		return -EINVAL;
	device_size /= KERNEL_SECTOR_SIZE;

	if (name_len >= 32)
		return -EOVERFLOW;

	/*
	 * Get some memory.
	 */

	memset(dev, 0, sizeof *dev);
	dev->size = device_size;
	dev->size_cap = device_size;
	dev->data = vmalloc(dev->size * KERNEL_SECTOR_SIZE);
	if (!dev->data) {
		printk(KERN_ALERT "vmalloc failure.\xa");
		return -ENOMEM;
	}
	memset(dev->data, 0, dev->size);
	dev->driver = &dcsc_driver;

	spin_lock_init(&dev->lock);

	/*
	 * Setup the I/O queue
	 */

	dev->queue = blk_init_queue(dcsc_request, &dev->lock);
	if (!dev->queue)
	{
		vfree(dev->data);
		return -ENOMEM;
	}

	blk_queue_logical_block_size(dev->queue, KERNEL_SECTOR_SIZE);
	dev->queue->queuedata = dev;

	/*
	 * Setup the gendisk structure.
	 */

	dev->gd = alloc_disk(DCSC_MINORS);
	if (!dev->gd) {
		printk(KERN_ALERT "alloc_disk failure\xa");
		blk_cleanup_queue(dev->queue);
		vfree(dev->data);
		return -ENOMEM;
	}
	dev->gd->major = dcsc_major;
	dev->gd->first_minor = which * DCSC_MINORS;
	dev->gd->fops = &dcsc_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;

	snprintf(dev->gd->disk_name, ((32 < name_len) ? 32 : name_len), "%s", name);
	dev->name = dev->gd->disk_name;

	set_capacity(dev->gd, device_size);
	add_disk(dev->gd);

	res = register_testbus_device(dev);
	if (res)
	{
		printk(KERN_ALERT "register_testbus_device failure\xa");
		blk_cleanup_queue(dev->queue);
		del_gendisk(dev->gd);
		put_disk(dev->gd);
		vfree(dev->data);
		return res;
	}

	return 0;
}

/* A wrapper for a setup function, appends a new device */

int new_device(
	char const *name,
	size_t name_len,
	size_t device_size
	)
{
	int res;
	MARKENTER(new_device);
	res = setup_device(Devices.Devices + Devices.n_devices,
	                   Devices.n_devices,
	                   name,
	                   name_len,
	                   device_size);
	if (res)
		return res;

	Devices.n_devices++;
	return 0;
}










/*
 * Initialiaztion and finitialization functions
 */



static int __init dcsc_init(void)
{
	int res;

	MARKENTER(dcsc_init);

	printk(KERN_NOTICE "dcsc: Initialize the module\xa");

	printk(KERN_NOTICE "dcsc: Create a simple bus\xa");
	res = bus_register(&testbus_type);
	if (res) {
		printk(KERN_ALERT "dcsc: failed to create a bus type (ret %d), stop\xa", res);
		return res;
	}
	res = device_register(&testbus);
	if (res) {
		printk(KERN_ALERT "dcsc: failed to create a bus (ret %d), stop\xa", res);
		return res;
	}

	printk(KERN_NOTICE "dcsc: Register the driver on the bus\xa");
	res = register_testbus_driver(&dcsc_driver);
	if (res) {
		printk(KERN_ALERT "dcsc: failed to register the driver (ret %d), stop\xa", res);
		return res;
	}

	/*
	 * Get registered.
	 */

	printk(KERN_NOTICE "dcsc: Alloc a major number\xa");
	dcsc_major = register_blkdev(dcsc_major, "dcsc");
	if (dcsc_major <= 0) {
		printk(KERN_ALERT "dcsc: failed to get major number\xa");
		printk(KERN_ALERT "dcsc: failed to init\xa");
		return -EBUSY;
	}
	printk(KERN_NOTICE "dcsc: got num: %d\xa", dcsc_major);

	/*
	 * Allocate the device array, and initialize each one.
	 */

	printk(KERN_NOTICE "dcsc: Initialize the devices\xa");
	Devices.Devices = kmalloc(Devices.cap_devices * sizeof(struct dcsc_dev),
	                          GFP_KERNEL);
	if (!Devices.Devices)
	{
		printk(KERN_ALERT "dcsc: failed to alloc storage for device descriptors\xa");
		printk(KERN_ALERT "dcsc: failed to kmalloc\xa");
		printk(KERN_ALERT "dcsc: failed to init\xa");
		unregister_blkdev(dcsc_major, "dcsc");
		return -ENOMEM;
	}

	if (!interactive_creation_allowed)
	{
		res = new_device("dcsca", 5, default_size);
		if (res)
		{
			printk(KERN_ALERT "dcsc: failed to init automatic device\xa");
			printk(KERN_ALERT "dcsc: failed to init\xa");
			unregister_blkdev(dcsc_major, "dcsc");
			return res;
		}
	}

	printk(KERN_NOTICE "dcsc: Initialize complete and successfull\xa");
	return 0;
}



static void __exit dcsc_exit(void)
{
	size_t i;
	MARKENTER(dcsc_exit);
	printk(KERN_NOTICE "dcsc: Finitialize the module\xa");

	for (i = 0;  i < Devices.n_devices;  i++) {
		struct dcsc_dev *dev = Devices.Devices + i;
		printk(KERN_NOTICE "dcsc: Finitialize the device %s\xa", dev->name);

		printk(KERN_NOTICE "dcsc: Finitialize the device %s: dealloc disk\xa", dev->name);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		printk(KERN_NOTICE "dcsc: Finitialize the device %s: dealloc queue\xa", dev->name);
		if (dev->queue)
			blk_cleanup_queue(dev->queue);
		printk(KERN_NOTICE "dcsc: Finitialize the device %s: dealloc storage\xa", dev->name);
		if (dev->data)
			vfree(dev->data);

		printk(KERN_NOTICE "dcsc: Finitialize the device %s: unregister device\xa", dev->name);
		unregister_testbus_device(dev);
	}
	printk(KERN_NOTICE "dcsc: Finitialize: unregister major num (%d)\xa", dcsc_major);
	unregister_blkdev(dcsc_major, "dcsc");
	unregister_testbus_driver(&dcsc_driver);
	device_unregister(&testbus);
	bus_unregister(&testbus_type);
	kfree(Devices.Devices);

	printk(KERN_NOTICE "dcsc: Finitialize complete\xa");
}



module_init(dcsc_init);
module_exit(dcsc_exit);
