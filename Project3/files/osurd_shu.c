#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>
#include <linux/crypto.h>	/* For crypto (Duh) */

MODULE_LICENSE("Dual BSD/GPL");

/* 
 *  * Global variables and stuff
 *   */
static char *key = "someweakkey"; //Crypto key
struct crypto_cipher *tfm; //Crypto cipher struct. Critical for crypto.

module_param(key, charp, 0000); //Allow user to set key as param in module
static int osurd_major = 0;
/* Passing 0 for the major number means the kernel will allocate a new number for it.
 * */
module_param(osurd_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
static int nsectors = 1024;	//Size of the drive
module_param(nsectors, int, 0);
static int ndevices = 2; //Number of RAM disks we want
module_param(ndevices, int, 0);

/*
 * * The different "request modes" we can use.
 * */
enum {
	RM_SIMPLE = 0,		/* The extra-simple request function */
	RM_FULL = 1,		/* The full-blown version */
	RM_NOQUEUE = 2,		/* Use make_request */
};
static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

/*
 * * Minor number and partition management.
 * */
#define OSURD_MINORS 16
#define MINOR_SHIFT 4
#define DEVNUM(kdevnum) (MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT
#define OSU_CIPHER "aes" //Cipher algorithm to use 

/*
 * * We can tweak our hardware sector size, but the kernel talks to us
 * * in terms of small sectors, always.
 * */
#define KERNEL_SECTOR_SIZE 512

/*
 * * After this much idle time, the driver will simulate a media change.
 * */
#define INVALIDATE_DELAY 30*HZ

/*
 * * The internal representation of our device.
 * */
struct osurd_dev {
	int size;		/* Device size in sectors */
	u8 *data;		/* The data array */
	short users;		/* How many users */
	short media_change;	/* Flag a media change? */
	spinlock_t lock;	/* For mutual exclusion */
	struct request_queue *queue;	/* The device request queue */
	struct gendisk *gd;	/* The gendisk structure */
	struct timer_list timer;	/* For simulated media changes */
};
static struct osurd_dev *Devices = NULL;

/*
 *  * For debugging crypto.
 *   */
static void hexdump(unsigned char *buf, unsigned int leng)
{
	while (leng--)
		printk("%02x", *buf++);

	printk("\n");
}

/*
 * * Handle an I/O request, in sectors.
 * */
static void
osurd_transfer(struct osurd_dev *dev, unsigned long sector,
	       unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector *KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect *KERNEL_SECTOR_SIZE;
	int i;
	if ((offset + nbytes) > dev->size) {
		printk(KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset,
		       nbytes);
		return;
	}

	crypto_cipher_clear_flags(tfm, ~0);
	crypto_cipher_setkey(tfm, key, strlen(key)); //Set the key to be our crypto key

	if (write){
		printk("Writing to RAMdisk\n");
		printk("Pre-encrypted data: ");
		hexdump(buffer, nbytes); //For debugging
		for (i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			memset(dev->data + offset + i, 0,
			       crypto_cipher_blocksize(tfm));
			crypto_cipher_encrypt_one(tfm, dev->data + offset + i,
						  buffer + i);
		}
		printk("Encrypted data:");
		hexdump(dev->data +offset, nbytes); //For debugging
	} else {
		printk("Reading from RAMdisk\n");
		printk("Encrypted data:");
		hexdump(dev->data +offset, nbytes); //For debugging
		for (i = 0; i < nbytes; i += crypto_cipher_blocksize(tfm)) {
			crypto_cipher_decrypt_one(tfm, buffer + i,
						  dev->data + offset + i);
		}
		printk("Decrypted data: ");
		hexdump(buffer, nbytes); //For debugging
	}
}

/*
 * * The simple form of the request function.
 * * Same.
 * */
static void
osurd_request(struct request_queue *q)
{
	struct request *req;

	req = blk_fetch_request(q);
	while (req != NULL) {
		struct osurd_dev *dev = req->rq_disk->private_data;
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		osurd_transfer(dev, blk_rq_pos(req),
			       blk_rq_cur_sectors(req), req->buffer,
			       rq_data_dir(req));
		/* end_request(req, 1); */
		if (!__blk_end_request_cur(req, 0)) {
			req = blk_fetch_request(q);
		}
	}
}

/*
 * * Transfer a single BIO.
 * * Also same.
 * */
static int
osurd_xfer_bio(struct osurd_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
		osurd_transfer(dev, sector, bio_cur_bytes(bio) >> 9	/* in sectors */
			       , buffer, bio_data_dir(bio) == WRITE);
		sector += bio_cur_bytes(bio) >> 9;	/* in sectors */
		__bio_kunmap_atomic(bio, KM_USER0);
	}
	return 0;		/* Always "succeed" */
}

/*
 *  * Transfer a full request.
 *   * Same
 *    */
static int
osurd_xfer_request(struct osurd_dev *dev, struct request *req)
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
 * * Smarter request function that "handles clustering".
 * * Same.
 * */
static void
osurd_full_request(struct request_queue *q)
{
	struct request *req;
	int sectors_xferred;
	struct osurd_dev *dev = q->queuedata;

	req = blk_fetch_request(q);
	while (req != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_NOTICE "Skip non-fs request\n");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		sectors_xferred = osurd_xfer_request(dev, req);
		if (!__blk_end_request_cur(req, 0)) {
			blk_fetch_request(q);
		}
	}
}

/*
 * * The direct make request version.
 * * Same.
 * */
static int
osurd_make_request(struct request_queue *q, struct bio *bio)
{
	struct osurd_dev *dev = q->queuedata;
	int status;

	status = osurd_xfer_bio(dev, bio);
	bio_endio(bio, status);
	return 0;
}

/*
 * * Open and close.
 * * Simulates removable media.
 * * Same.
 * */
static int
osurd_open(struct block_device *device, fmode_t mode)
{
	struct osurd_dev *dev = device->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	/* filp->private_data = dev; */
	spin_lock(&dev->lock);
	if (!dev->users)
		check_disk_change(device);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

/*
 *  * Same
 *   */
static int
osurd_release(struct gendisk *disk, fmode_t mode)
{
	struct osurd_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);

	return 0;
}

/*
 * * Look for a (simulated) media change.
 * * Same.
 * */
int
osurd_media_changed(struct gendisk *gd)
{
	struct osurd_dev *dev = gd->private_data;

	return dev->media_change;
}

/*
 * * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * * with open.  That needs to be reevaluated.
 * * This function is called after a media change.
 * * Same.
 * */
int
osurd_revalidate(struct gendisk *gd)
{
	struct osurd_dev *dev = gd->private_data;

	if (dev->media_change) {
		dev->media_change = 0;
		memset(dev->data, 0, dev->size);
	}
	return 0;
}

/*
 * * The "invalidate" function runs out of the device timer; it sets
 * * a flag to simulate the removal of the media.
 * * Same
 * */
void
osurd_invalidate(unsigned long ldev)
{
	struct osurd_dev *dev = (struct osurd_dev *) ldev;

	spin_lock(&dev->lock);
	if (dev->users || !dev->data)
		printk(KERN_WARNING "osurd: timer sanity check failed\n");
	else
		dev->media_change = 1;
	spin_unlock(&dev->lock);
}

/* 
 *  * Added getgeo() function 
 *  */
static int
osurd_getgeo(struct block_device *device, struct hd_geometry *geo)
{
	struct osurd_dev *dev = device->bd_disk->private_data;
	long size = dev->size * (hardsect_size / KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;		//Changed this to 0 from 4
	return 0;
}

/*
 * * The device operations structure.
 * */
static struct block_device_operations osurd_ops = {
	.owner = THIS_MODULE,
	.open = osurd_open,
	.release = osurd_release,
	.media_changed = osurd_media_changed,
	.revalidate_disk = osurd_revalidate,
	.getgeo = osurd_getgeo
};

/*
 * * Set up our internal device.
 * * Called by the init function.
 * * Same.
 * */
static void
setup_device(struct osurd_dev *dev, int which)
{
	/*
 * 	* Get some memory.
 * 		*/
	memset(dev, 0, sizeof (struct osurd_dev));
	dev->size = nsectors * hardsect_size; //Set the device size
	dev->data = vmalloc(dev->size); //Allocate virtually contiguous memory for the RAM disk
	if (dev->data == NULL) {
		printk(KERN_NOTICE "vmalloc failure.\n");
		return;
	}
	/* Allocate a spinlock for mutual exclusion */
	spin_lock_init(&dev->lock);

	/*
 * 	 * The timer which "invalidates" the device.
 * 	 	 * This is a 30-second timer used to simulate 
 * 	 	 	 * behavior of a removable device.
 * 	 	 	 	 */
	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = osurd_invalidate;

	/*
 * 	 * The I/O queue, depending on whether we are using our own
 * 	 	 * make_request function or not.
 * 	 	 	 */
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
		printk(KERN_NOTICE
		       "Bad request mode %d, using simple\n", request_mode);
		/* fall into.. */
	case RM_SIMPLE:
		dev->queue = blk_init_queue(osurd_request, &dev->lock);
		if(dev->queue == NULL)
			goto out_vfree;
		break;
	}
	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;

	/*
 * 	 * And the gendisk structure.
 * 	 	 * gendisk is the kernel's representation of an individual disk device.
 * 	 	 	 */
	dev->gd = alloc_disk(OSURD_MINORS);
	if (!dev->gd) {
		printk(KERN_NOTICE "alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = osurd_major;
	dev->gd->first_minor = which * OSURD_MINORS;
	dev->gd->fops = &osurd_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	/* 
 * 	 * Set the device names to osurda, osurdb, etc.
 * 	 	 */
	snprintf(dev->gd->disk_name, 32, "osurd%c", which + 'a');
	set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

      out_vfree:
	if (dev->data)
		vfree(dev->data);
}

/* 
 *  * Module initialization function.
 *   */
static int __init
osurd_init(void)
{
	int i;
	/* Initialize the cipher with AES encryption */
	tfm = crypto_alloc_cipher(OSU_CIPHER, 0, 0);
	/* Error checking for crypto */
	if (IS_ERR(tfm)) {
		printk(KERN_ERR "osurd: cipher allocation failed");
		return PTR_ERR(tfm);
	}

	/*
 * 	* Get registered.
 * 		* Register a block device called "osurd."
 * 			*/
	osurd_major = register_blkdev(osurd_major, "osurd");
	if (osurd_major <= 0) {
		printk(KERN_WARNING "osurd: unable to get major number\n");
		return -EBUSY;
	}

	/*
 * 	* Allocate the device array, and initialize each one.
 * 		*/
	Devices = kmalloc(ndevices * sizeof (struct osurd_dev), GFP_KERNEL);
	if (Devices == NULL)
		goto out_unregister;
	/* Setup each device */
	for (i = 0; i < ndevices; i++)
		setup_device(Devices + i, i);

	return 0;
      out_unregister:
		unregister_blkdev(osurd_major, "osurd");
		return -ENOMEM;
}

static void
osurd_exit(void)
{
	int i;
	for (i = 0; i < ndevices; i++) {
		struct osurd_dev *dev = Devices + i;

		del_timer_sync(&dev->timer);
		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue)
			/* Destroy the request queue by releasing the request_queue_t */
			blk_cleanup_queue(dev->queue);
		if (dev->data)
			/* Free the virtual memory allocated earlier for the disk */
			vfree(dev->data);
	}
	/* Unregister the osurd block device */
	unregister_blkdev(osurd_major, "osurd");
	/* Free the crypto cipher struct from memory */
	crypto_free_cipher(tfm);
	kfree(Devices);
}

module_init(osurd_init);
module_exit(osurd_exit);
