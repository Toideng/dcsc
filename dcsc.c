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

#define DCSC_MINORS 16 /* # of minor numbers per device */
#define MAXDEVICES 16
#define KERNEL_SECTOR_SIZE 512

static int dcsc_major = 0; /* allocate major number at runtime */
static int default_size = 128 * 1024 * 1024; /* twelvety-eight MiB in bytes */

/*
 * Chooses between requiring user to create all devices or creating one default
 * device at startup
 */
static int interactive_creation_allowed = 0;
module_param(interactive_creation_allowed, int, 0);

/* This bus is only needed for sysfs manipulations */
struct testbus_driver {
	char *name;
	struct driver_attribute createnewdevice_attr;
	struct device_driver driver;
} dcsc_driver = {"dcsc_driver"};

/* Main device 'descriptor', contains all needed info */
struct dcsc_dev {
	char *name;
	size_t size;                 /* Current device size in sectors */
	size_t size_cap;             /* Maximum device size in sectors,
	                              * i.e. allocated storage area for `data` */
	u8 *data;

	struct testbus_driver *driver;
	struct device_attribute access_attr;
	struct device_attribute size_attr;
	int access_mode;             /* 0 for read-write, other for read-only */

	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
	struct device dev;
};

/* An array of managed devices */
static struct {
	size_t n_devices;
	size_t cap_devices;
	struct dcsc_dev *Devices;
} Devices = {0, MAXDEVICES, NULL};



/*
 * These two functions are forward-declared here and defined later when all
 * other functions used by them are already defined
 */

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
 * I have created a bus of my own since this was the simplest way to test the
 * sysfs attributes.
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

/* Transfer a single bvec */

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
		printk(KERN_ALERT "%s: "
		       "kernel has requested transfer of a non-integer # of sectors.\xa",
		       dev->name);

	if (offset + len > dev->size * KERNEL_SECTOR_SIZE) {
		if (data_dir == WRITE)
			printk(KERN_WARNING "Beyond-end write (0x%016lx+0x%016lx)\xa",
			       offset, len);
		else
			printk(KERN_WARNING "Beyond-end read (0x%016lx+0x%016lx)\xa",
			       offset, len);
		return -EIO;
	}

	if (data_dir == WRITE) {
		if (dev->access_mode) {
			printk(KERN_ALERT "%s: "
			       "Acces denied: write on read-only device\xa",
			       dev->name);
			return -EACCES;
		}
		memcpy(indsk, inmem, len);
	} else {
		memcpy(inmem, indsk, len);
	}

	kunmap_atomic(buffer);

	return 0;
}

/* Transfer a full request */

static void dcsc_xfer_request(
	struct dcsc_dev *dev,
	struct request *req
	)
{
	struct bio_vec *bvec;
	struct req_iterator iter;

	if (!dev->data || !dev->name)
		return;// -EFAULT;

	/* Kernel code suggests using `rq_for_each_segment` instead, but how am
	 * i to get the `cur_sector` that way? */

	__rq_for_each_bio(iter.bio, req) {
		sector_t cur_sector = iter.bio->bi_sector;
		bio_for_each_segment(bvec, iter.bio, iter.i) {
			dcsc_xfer_bvec(dev, bvec, bio_data_dir(iter.bio), cur_sector);
			cur_sector += bvec->bv_len / KERNEL_SECTOR_SIZE;
		}
	}
	return;
}

/* Callback for serving queued requests */

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
 * Bus, device and driver management in sysfs. Adds required attributes
 */

/* Both 'release' functions are stubs since there's nothing to de-initialize */

void testbus_release(
	struct device *dev
	)
{
	return;
}

void testbus_dev_release(
	struct device *dev
	)
{
	return;
}

/*
 * 'size' attribute allows user to shrink/grow device size, yet does nothing to
 * reduce memory usage. Attribute allows specifying size by multiplying integer
 * numbers
 */

ssize_t show_size_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char *buf
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	return snprintf(buf, PAGE_SIZE, "%lu\xa", dev->size / (1024 / KERNEL_SECTOR_SIZE));
}

ssize_t store_size_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char const *buf,
	size_t count
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
	size_t size;
	size_t multiplier;
	size_t i;

	size = 1024;
	multiplier = 0;
	for (i = 0;  i < count;  i++) {
		if (buf[i] == 0xa)
			break;
		if (buf[i] == 0x20)
			continue;
		if (buf[i] == 0x2a) {
			size *= multiplier;
			multiplier = 0;
			continue;
		}
		if (buf[i] < '0' || buf[i] > '9')
			return -EINVAL;

		multiplier *= 10;
		multiplier += buf[i] - '0';
	}
	size *= multiplier;

	if (size == 0)
		return -EINVAL;

	printk(KERN_DEBUG "Requested size change to %08lx (max = %08lx)\xa",
	       size, dev->size_cap * KERNEL_SECTOR_SIZE);

	if (size > dev->size_cap * KERNEL_SECTOR_SIZE)
		return -EINVAL;
	if (size % KERNEL_SECTOR_SIZE)
		return -EINVAL;
	dev->size = size / KERNEL_SECTOR_SIZE;

	return count;
}

/* 'access' attribute allows user to change access mode for the device */

ssize_t show_access_attr(
	struct device *plaindev,
	struct device_attribute *attr,
	char *buf
	)
{
	struct dcsc_dev *dev = container_of(plaindev, struct dcsc_dev, dev);
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

	if (count < 1)
		return -EINVAL;
	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;

	printk(KERN_DEBUG "Requested access mode change to %d\xa", buf[0] - '0');

	dev->access_mode = buf[0] - '0';
	return count;
}

/* Registration of a device in sysfs for creating attributes */

int register_testbus_device(
	struct dcsc_dev *dev
	)
{
	int res;

	memset(&dev->dev, 0, sizeof dev->dev);
	dev->dev.bus = &testbus_type;
	dev->dev.parent = &testbus;
	dev->dev.release = testbus_dev_release;
	dev_set_name(&dev->dev, "%s", dev->name);

	res = device_register(&dev->dev);
	if (res)
		return res;

	memset(&dev->size_attr, 0, sizeof dev->size_attr);
	dev->size_attr.attr.name = "size";
	dev->size_attr.attr.mode = S_IRUGO | S_IWUGO;
	dev->size_attr.show = show_size_attr;
	dev->size_attr.store = store_size_attr;

	res = device_create_file(&dev->dev, &dev->size_attr);
	if (res)
		return res;

	memset(&dev->size_attr, 0, sizeof dev->size_attr);
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
	device_unregister(&dev->dev);
}

/*
 * Attribute 'createnewdevice' allows user, if the flag was checked at module's
 * load time, to create new devices of specified sizes.
 */

static ssize_t show_createnewdevice_attr(
	struct device_driver *driver,
	char *buf
	)
{
	return snprintf(buf, PAGE_SIZE, "Specify a device name and size in KiB, e.g. \"dcscb 12*1024\"\xa");
}

static ssize_t store_createnewdevice_attr(
	struct device_driver *driver,
	char const *buf,
	size_t count
	)
{
	int res;
	size_t i;
	char const *namestart;
	size_t namelen;
	size_t size;
	size_t multiplier;

	ssize_t ret_succ = count;

	/* Only care about date till the end of the first line, everything else
	 * is discarded */

	for (i = 0;  i < count;  i++) {
		if (buf[i] == 0xa) {
			count = i;
			break;
		}
	}

	/* "Name" is a run of alphanumeric characters till the first whitespace.
	 * A whitespace after name is required; failing to insert it is an
	 * error */

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
	if (!namelen)
		return -EINVAL;
	i++; /* skip whitespace */

	/* "Size" is the rest of the first line. It must only consist of
	 * characters [0-9*\x20]. It is a multiplication of several positive
	 * integers. */

	size = 1024; /* Size is specified by user in KiB */
	multiplier = 0;
	for (;  i < count;  i++) {
		if (buf[i] == 0x20)
			continue;
		if (buf[i] == 0x2a) {
			size *= multiplier;
			multiplier = 0;
			continue;
		}

		if (buf[i] >= '0' && buf[i] <= '9')
		{
			multiplier *= 10;
			multiplier += buf[i] - '0';
			continue;
		}

		return -EINVAL;
	}
	size *= multiplier;

	if (size == 0)
		return -EINVAL;
	if (size % KERNEL_SECTOR_SIZE)
		return -EINVAL;

	res = new_device(namestart, namelen, size);
	if (res)
		return res;

	return ret_succ;
}

/* Registration of the driver in sysfs for creating attributes */

int register_testbus_driver(
	struct testbus_driver *driver
	)
{
	int ret;

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
	driver_unregister(&driver->driver);
}

/*
 * Open and close. Both aren't of much use, but are required by the API.
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
 * By this point, everything needed for the setup of this structure has been
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

	printk(KERN_NOTICE "dcsc: Setting up a new device\xa");

	if (which >= Devices.cap_devices)
		return -EBADSLT;

	if (device_size % KERNEL_SECTOR_SIZE)
		return -EINVAL;

	/* convert `device_size` to sectors */
	device_size /= KERNEL_SECTOR_SIZE;

	if (name_len >= 32)
		return -EOVERFLOW;

	/*
	 * Setup storage management fields
	 */

	memset(dev, 0, sizeof *dev);
	dev->size = device_size;
	dev->size_cap = device_size;
	dev->data = vmalloc(dev->size * KERNEL_SECTOR_SIZE);
	if (!dev->data) {
		printk(KERN_ALERT "vmalloc failure.\xa");
		return -ENOMEM;
	}
	memset(dev->data, 0, dev->size * KERNEL_SECTOR_SIZE);

	/*
	 * Setup the I/O queue
	 */

	spin_lock_init(&dev->lock);
	dev->queue = blk_init_queue(dcsc_request, &dev->lock);
	if (!dev->queue) {
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

	dev->driver = &dcsc_driver;
	dev->access_mode = 0;

	res = register_testbus_device(dev);
	if (res)
	{
		printk(KERN_ALERT "register_testbus_device failure\xa");
		del_gendisk(dev->gd);
		put_disk(dev->gd);
		blk_cleanup_queue(dev->queue);
		vfree(dev->data);
		return res;
	}

	printk(KERN_NOTICE "dcsc: A new device has been successfully set up\xa");

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
	if (Devices.n_devices >= Devices.cap_devices)
		return -EBADSLT;

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

#define XX(s, ...)\
	printk(KERN_NOTICE "dcsc: " s "\xa", ##__VA_ARGS__);
#define XXA(s, ...)\
	printk(KERN_ALERT "dcsc: " s "\xa", ##__VA_ARGS__);

	XX("Initialize the module");

	XX("Create a simple bus");
	res = bus_register(&testbus_type);
	if (res) {
		XXA("failed to create a bus type (ret %d), stop", res);
		return res;
	}
	res = device_register(&testbus);
	if (res) {
		XXA("failed to create a bus (ret %d), stop", res);
		device_unregister(&testbus);
		return res;
	}

	XX("Register the driver on the bus");
	res = register_testbus_driver(&dcsc_driver);
	if (res) {
		XXA("failed to register the driver (ret %d), stop", res);
		device_unregister(&testbus);
		bus_unregister(&testbus_type);
		return res;
	}

	/*
	 * Get a major number.
	 */

	XX("Alloc a major number");
	dcsc_major = register_blkdev(dcsc_major, "dcsc");
	if (dcsc_major <= 0) {
		XXA("failed to get major number");
		XXA("failed to init");
		unregister_testbus_driver(&dcsc_driver);
		device_unregister(&testbus);
		bus_unregister(&testbus_type);
		return -EBUSY;
	}
	XX("got num: %d", dcsc_major);

	/*
	 * Allocate the device array, and initialize the first one if the
	 * dynamic device creation was not allowed at load time.
	 */

	XX("Initialize the devices array");
	Devices.Devices = kmalloc_array(Devices.cap_devices,
	                                sizeof(struct dcsc_dev),
	                                GFP_KERNEL);
	if (!Devices.Devices)
	{
		XXA("failed to alloc storage for device descriptors");
		XXA("failed to kmalloc");
		XXA("failed to init");
		unregister_blkdev(dcsc_major, "dcsc");
		unregister_testbus_driver(&dcsc_driver);
		device_unregister(&testbus);
		bus_unregister(&testbus_type);
		return -ENOMEM;
	}

	if (!interactive_creation_allowed)
	{
		XX("Initialize the default device");
		res = new_device("dcsca", strlen("dcsca"), default_size);
		if (res)
		{
			XXA("failed to init automatic device");
			XXA("failed to init");
			kfree(Devices.Devices);
			unregister_blkdev(dcsc_major, "dcsc");
			unregister_testbus_driver(&dcsc_driver);
			device_unregister(&testbus);
			bus_unregister(&testbus_type);
			return res;
		}
	}

	XX("Initialize complete and successfull");
	return 0;
#undef XXA
#undef XX
}

static void __exit dcsc_exit(void)
{
	size_t i;

#define XX(s, ...)\
	printk(KERN_NOTICE "dcsc: Finitialize" s "\xa", ##__VA_ARGS__);

	XX("the module");

	for (i = 0;  i < Devices.n_devices;  i++) {
		struct dcsc_dev *dev = Devices.Devices + i;

#define XXX(s)\
XX(" the device #%d (\"%s\")" s, (int)i, dev->name)

		XXX("")
		XXX(": dealloc disk")
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		XXX(": dealloc queue")
		if (dev->queue)
			blk_cleanup_queue(dev->queue);
		XXX(": dealloc storage")
		if (dev->data)
			vfree(dev->data);

		XXX(": unregister device")
		unregister_testbus_device(dev);

#undef XXX
	}
	XX(": unregister major num (%d)", dcsc_major)
	unregister_blkdev(dcsc_major, "dcsc");

	XX(": unregister driver")
	unregister_testbus_driver(&dcsc_driver);
	XX(": unregister bus")
	device_unregister(&testbus);
	bus_unregister(&testbus_type);

	XX(": free the Devices array")
	kfree(Devices.Devices);

	XX(" complete")
#undef XX

	return;
}

module_init(dcsc_init);
module_exit(dcsc_exit);
