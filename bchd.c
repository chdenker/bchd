/*
 * bchd -- Basic character device
 *
 * Inspired by the "scull" driver as described in
 * "Linux Device Drivers Third Edition" by Corbet et al.
 *
 * This is a kernel module that takes user (text) input written to /dev/bchd
 * and stores it in a dynamic storage. Whenever a user reads from /dev/bchd,
 * the text data is transferred back to the user.
 * Furthermore, this module periodically (1 word per sec) writes the stored text data
 * into the kernel log.
 * If there is no data stored, the character ' ' is written.
 */

#include <linux/module.h>   /* Necessary for all modules */
#include <linux/init.h>     /* For module_init and module_exit */

#include <linux/kernel.h>   /* container_of */
#include <linux/types.h>    /* For dev_t */
#include <linux/kdev_t.h>   /* For MAJOR,MINOR,MKDEV */
#include <linux/fs.h>       /* For alloc_chrdev_region etc */
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/fcntl.h>    /* O_ACCMODE */
#include <linux/slab.h>     /* kmalloc, kfree */
#include <linux/uaccess.h>  /* copy_from_user, copy_to_user */

MODULE_AUTHOR("Christopher Denker");
MODULE_DESCRIPTION("Basic character device");
MODULE_LICENSE("GPL");

int bchd_major = 0;    /* we use a dynamic major by default */
int bchd_minor = 0;

struct bchd_dev {
    struct cdev cdev;   /* Char device structure */
};

struct bchd_dev *bchd_dev; /* allocated in bchd_init */

int bchd_open(struct inode *inode, struct file *filp)
{
    return 0;
}

int bchd_release(struct inode *inode, struct file *filp)
{
    return 0;
}

ssize_t bchd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    return 0;
}

ssize_t bchd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    return 0;
}

struct file_operations bchd_fops = {
    .owner = THIS_MODULE, /* used to prevent module from being unloaded while in use */
    .read = bchd_read,
    .write = bchd_write,
    .open = bchd_open,
    .release = bchd_release,
};

/* Set up char device structure for this device */
static void bchd_setup_cdev(struct bchd_dev *dev)
{
    int err;
    dev_t devno = MKDEV(bchd_major, bchd_minor);

    cdev_init(&dev->cdev, &bchd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &bchd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_NOTICE "Error %d adding bchd device", err);
    }
}

static void bchd_cleanup(void)
{
    dev_t dev = MKDEV(bchd_major, bchd_minor);

    /* get rid of char dev entry */
    if (bchd_dev != NULL) {
        cdev_del(&bchd_dev->cdev);
        kfree(bchd_dev);
    }

    /* bchd_cleanup is never called if registering failed */
    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO "bchd: exit\n");
}

static int __init bchd_init(void)
{
    int result;
    dev_t dev = 0;
    
    result = alloc_chrdev_region(&dev, bchd_minor, 1, "bchd");
    bchd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "bchd: can't get major %d\n", bchd_major);
        return result;
    }

    /* Allocate the device */
    bchd_dev = kmalloc(sizeof(*bchd_dev), GFP_KERNEL);
    if (bchd_dev == NULL) {
        result = -ENOMEM;
        goto fail;
    }
    memset(bchd_dev, 0, sizeof(*bchd_dev));

    /* Initialize the device */
    bchd_setup_cdev(bchd_dev);

    printk(KERN_INFO "bchd: initialized -- device major: %d; device minor: %d\n", MAJOR(dev), MINOR(dev));
    return 0;   /* success */

fail:
    bchd_cleanup();
    return result;
}

module_init(bchd_init);
module_exit(bchd_cleanup);
