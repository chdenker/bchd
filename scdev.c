/*
 * scdev -- Simple character device
 *
 * Inspired by the "scull" driver as described in
 * "Linux Device Drivers Third Edition" by Corbet et al.
 *
 * This is a kernel module that takes user (text) input written to /dev/smplchdev
 * and stores it in a dynamic storage. Whenever a user reads from /dev/smplchdev,
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
MODULE_DESCRIPTION("Simple character device");
MODULE_LICENSE("GPL");

static int __init scdev_init(void)
{
    int result;
    result = 0;

    printk(KERN_INFO "scdev: init\n");
    return result;
}

static void __exit scdev_cleanup(void)
{
    printk(KERN_INFO "scdev: exit\n");
}

module_init(scdev_init);
module_exit(scdev_cleanup);
