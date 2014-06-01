/*
 * Name    : osurd.c
 * Project : Ram Disk Driver for Linux Kernel
 * Group   : Group 17
 * Authors : Jesse Wilson, Yuan Zhang, Daniel Tasato
 */

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
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	
#include <linux/bio.h>
#include <linux/crypto.h>

MODULE_LICENSE("Dual BSD/GPL");


/*
 * Global variables
 */
static char *key = "someweakkey";
struct crypto_cipher *tfm;
module_param(key, charp, 0000);

static int osurd_major = 0;
module_param(osurd_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
static int nsectors = 1024;
module_param(nsectors, int, 0);
static int ndevices = 4;
module_param(ndevices, int, 0);


/*
 * Possible request modes
 */
enum {
	RM_SIMPLE = 0,
	RM_FULL = 1,
	RM_NOQUEUE = 2,
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);


#define OSURD_MINORS 16
#define MINOR_SHIFT 4
#define DEVNUM(kdevnum) (MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT
#define OSU_CIPHER "aes"
#define KERNEL_SECTOR_SIZE 512
#define INVALIDATE_DELAY 30*HZ


/*
 * RAM disk device struct
 */
struct osurd_dev {
	int size;
	u8 *data;
	short users;
	short media_change;
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
	struct timer_list timer;
};
static struct osurd_dev *Devices = NULL;


//Itterates through and prints data.
static void hexdump(unsigned char *buf, unsigned int len)
{
	while(len--) {
		printk("%02x", *buf++);
	}
	
	printk("\n");
}


/*
 * Basic transfer function called by other functions for transfering
 * data from the RAM disk block. Calls the appropriate encrypt and
 * decrypt functions from crytpo. Also calls hexdump in order to
 * dump the entirety of the data to the kernel.
 */
static void osurd_transfer(struct osurd_dev *dev, unsigned long sector,
			   unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector *KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect *KERNEL_SECTOR_SIZE;
	int i;

	if((offset + nbytes) > dev->size) {
		printk(KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}

	crypto_cipher_clear_flags(tfm, ~0);
	crypto_cipher_setkey(tfm, key, strlen(key));

	if(write) {
		printk("Writing to RAM disk\n");
		printk("Pre-encrypted data: ");
		hexdump(buffer, nbytes);
		
		for(i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			memset(dev->data + offset +i, 0, crypto_cipher_blocksize(tfm));
			crypto_cipher_encrypt_one(tfm, dev->data + offset + i, buffer + i);
		}

		printk("Encrypted data: ");
		hexdump(dev->data + offset, nbytes);
	}
	else {
		printk("Reading from RAM disk\n");
		printk("Encrypted data: ");
		hexdump(dev->data + offset, nbytes);
	
		for(i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			crypto_ckpher_decrypt_one(tfm, buffer + i, dev->data + offset + i);
		}
		
		printk("Decrypted data: ");
		hexdump(buffer, nbytes);
	}
}


/*
 * Simply used for requesting a transfer (read or write) of 
 * data from the RAM disk.
 */
static void osurd_request(struct request_queue *q)
{
	struct request *req;
	req = blk_fetch_request(q);
	
	while(req != NULL) {
		struct osurd_dev *dev = req->rq_disk->private_data;
		if(req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		osurd_transfer(dev, blk_rq_pos(req), blk_rq_cur_sectors(req),
			       req->buffer, rq_data_dir(req)):
		
		if(!__blk_end_request_cur(req, 0)) {
			req = blk_fetch_request(q);
		}
	}
}


/*
 * Used for transfering a single bio to a sector. Calls 
 * osurd_tranfer for the actual tranfer to the RAM disk.
 */
static int osurd_xfer_bio(struct osurd_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
		osurd_transfer(dev, sector, bio_cur_bytes(bio) >> 9,
			       buffer, bio_data_dir(bio) == WRITE);
		sector += bio_cur_bytes(bio) >> 9;
		__bio_kunmap_atomic(bio, KM_USER0);
	}
	
	return 0;
}


/*
 * Calls osurd_xfer_bio on each bio in a request.
 */
static int osurd_xfer_request(struct osurd_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;

	__rq_for_each_bio(bio, req) {
		osurd_xfer_bio(dev, bio);
		nsect += bio->bi_size / KERNEL_SECTOR_SIZE;
	}
	
	return nsect;
}


/*
 * For handling clustering. 
 */
static void osurd_full_request(struct request_queue *q)
{
	struct request *req;
	int sectors_xferred;
	struct osurd_dev *dev = q->queuedata;
	req = blk_fetch_request(q);
	
	while(req != NULL) {
		if(req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}

		sectors_xferred = osurd_xfer_request(dev, req);
		if(!__blk_end_request_cur(req, 0)) {
			blk_fetch_reqest(q);
		}
	}
}


/*
 * This directly calls osurd_xfer_bio to make a direct request 
 * on the RAM disk. It also handles the returned status of the 
 * request.
 */
static int osurd_make_request(struct request_queue *q, struct bio *bio)
{
	struct osurd_dev *dev = q->queuedata;
	int status;
	
	status = osurd_xfer_bio(dev, bio);
	bio_endio(bio, status);
	
	return 0;
}


/*
 * Opens the RAM disk.
 * This simulates the RAM disk like removable media.
 */
static int osurd_open(struct block_device *device, fmode_t mode)
{
	struct osurd_dev *dev = device->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	spin_lock(&dev->lock);

	if(!dev->users) {
		check_disk_change(device);
	}
	
	dev->users++;
	spin_unlock(&dev->lock);

	return 0;
}


/*
 * Closes the RAM disk. Simulating removable media.
 */
static int osurd_release(struct gendisk *disk, fmode_t mode)
{
	struct osurd_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

	if(!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);

	return 0;
}


/*
 * Checks the status of a simulated media change.
 */
int osurd_media_changed(struct gendisk *gd)
{
	struct osurd_dev *dev = gd->private_data;
	return dev->media_change;
}


/*
 * Called after a media change. Lock is not used to
 * avoid deadlocking with open. Resets media_change
 * flag.
 */
int osurd_revalidate(struct gendisk *gd)
{
	struct osurd_dev *dev = gd->private_data;

	if(dev->media_change) {
		dev->media_change = 0;
		memset(dev->data, 0, dev->size);
	}

	return 0;
}


/*
 * Sets the media_change flag to simulate removal of media.
 */
void osurd_invalidate(unsigned long ldev)
{
	struct osurd_dev *dev = (struct osurd_dev *) ldev;

	spin_lock(&dev->lock);
	if(dev->users || !dev->data) {
		printk(KERN_WARNING "osurd: timer check failed\n");
	}
	else {
		dev->media_change = 1;
	}

	spin_unlock(&dev->lock);
}


/*
 * Checks geometric elements of a block device. 
 * Checks the size of the device and sets the appropriate
 * cylinders, heads, sectors, and start.
 */
static int osurd_getgeo(struct block_device *device, struct hd_geometry *geo)
{
	struct osurd_dev *dev = device->bd_disk->private_data;
	long size = (dev->size * (hardsect_size / KERNEL_SECTOR_SIZE));
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;

	return 0;
}


/*
 * Device operations struct. 
 */
static struct block_device_operations osurd_ops = {
	.owner = THIS_MODULE,
	.open = osurd_open,
	.release = osurd_release,
	.media_changed = osurd_media_changed,
	.revalidate_disk = osurd_revalidate,
	.getgeo = osurd_getgeo
};


/*
 * For setting up the RAM disk. This function sets the appropriate 
 * amount of memory and allocates it for the RAM disk. It then 
 * determines which request state the system is in and sets up
 * the RAM disk accordingly.
 */
static void setup_device(struct osurd_dev *dev, int which)
{
	memset(dev, 0, sizeof(struct osurd_dev));
	dev->size = nsectors * hardsect_size;
	dev->data = vmalloc(dev->size);

	if(dev->data == NULL) {
		printk(KERN_NOTICE "vmalloc failure.\n");
		return;
	}

	spin_lock_init(&dev->lock);

	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = osurd_invalidate;

	switch(request_mode) {
		case RM_NOQUEUE:
			dev->queue = blk_alloc_queue(GFP_KERNEL);
			if(dev->queue == NULL)
				goto out_vfree;
			blk_queue_make_request(dev->queue, osurd_make_request);
			break;

		case RM_FULL:
			dev->queue = blk_init_queue(osurd_full_request, &dev->lock);
			if(dev->queue == NULL)
				goto out_vfree;
			break;
		
		default:
			printk(KERN_NOTICE "Bad request mode %d, using simple\n", 
								request_mode);
	
		case RM_SIMPLE:
			dev->queue = blk_init_queue(osurd_request, &dev->lock);
			if(dev->queue == NULL)
				goto out_vfree;
			break;
	}

	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;

	dev->gd = alloc_disk(OSURD_MINORS);
	if(!dev->gd) {
		printk(KERN_NOTICE "alloc_disk failure\n");
		goto out_vfree;
	}

	dev->gd->major = osurd_major;
	dev->gd->first_minor = which * OSURD_MINORS;
	dev->gd->fops = &osurd_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;

	snprintf(dev->gd->disk_name, 32, "osurd%c", which + 'a');
	set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);

	return;

	out_vfree:
		if(dev->data) {
			vfree(dev->data);
		}
}


/*
 * For initializing the module. 
 */
static int __init osurd_init(void)
{
	int i;
	tfm = crypto_alloc_cipher(OSU_CIPHER, 0, 0);

	if(IS_ERR(tfm)) {
		printk(KERN_ERR "osurd: cipher allocation failed");
		return PTR_ERR(tfm);
	}

	osurd_major = register_blkdev(osurd_major, "osurd");
	if(osurd_major <= 0) {
		printk(KERN_WARNING "osurd: unable to get major number\n");
		return -EBUSY;
	}

	Devices = kmalloc(ndevices * sizeof(struct osurd_dev), GFP_KERNEL);
	if(Devices == NULL)
		goto out_unregister;
	
	for(i = 0; i < ndevices; i++) {
		setup_device(Devices + i, i);
	}

	return 0;
	
	out_unregister:
		unregister_blkdev(outrd_major, "osurd");
		return -ENOMEM;
}


/*
 * For exiting the module.
 */
static void osurd_exit(void)
{
	int i;
	for(i = 0; i < ndevices; i++) {
		struct osurd_dev *dev = Devices + i;

		del_timer_sync(&dev->timer);
		if(dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if(dev->queue) {
			blk_cleanup_queue(dev->queue);
		}
		if(dev->data) {
			vfree(dev->data);
		}
	}

	unregister_blkdev(osurd_major, "osurd");
	crypto_free_cipher(tfm);
	kfree(Devices);
}

/*
 * Module init and exit declarations
 */
module_init(osurd_init);
module_exit(osurd_exit);

