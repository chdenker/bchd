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

#include <linux/module.h>       /* Necessary for all modules */
#include <linux/init.h>         /* For module_init and module_exit */

#include <linux/kernel.h>       /* container_of */
#include <linux/types.h>        /* For dev_t */
#include <linux/kdev_t.h>       /* For MAJOR,MINOR,MKDEV */
#include <linux/fs.h>           /* For alloc_chrdev_region etc */
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/fcntl.h>        /* O_ACCMODE */
#include <linux/slab.h>         /* kmalloc, kfree */
#include <linux/uaccess.h>      /* copy_from_user, copy_to_user */
#include <linux/workqueue.h> 
#include <linux/jiffies.h>      /* HZ */

MODULE_AUTHOR("Christopher Denker");
MODULE_DESCRIPTION("Basic character device");
MODULE_LICENSE("GPL");

#ifndef BCHD_QUANTUM
#define BCHD_QUANTUM 4000
#endif

#ifndef BCHD_QSET
#define BCHD_QSET 1000
#endif

#ifndef BCHD_MAX_WORD_LEN
#define BCHD_MAX_WORD_LEN 20
#endif

int bchd_major = 0;    /* we use a dynamic major by default */
int bchd_minor = 0;
int bchd_quantum_size = BCHD_QUANTUM;
int bchd_qset_size = BCHD_QSET;
int bchd_max_word_len = BCHD_MAX_WORD_LEN;

/*
 * The data of a bchd device is represented using a linked list.
 * Each list item contains an array of pointers, called quantum set,
 * where each pointer points to a memory area, called a quantum.
 * The sizes of a quantum set and a quantum are attributes of the bchd_dev struct.
 */
struct bchd_qset {
    void **data;
    struct bchd_qset *next;
};

struct bchd_dev {
    struct bchd_qset *data;     /* Pointer to first quantum set */
    int quantum_size;           /* Amount of bytes per quantum */
    int qset_size;              /* Amount of pointers in a quantum set */
    unsigned long size;         /* Amount of data (in bytes) stored here */
 
    int max_word_len;           /* Max word length we write into the kernel log */
    struct workqueue_struct *wq_logger;
    struct delayed_work ws_logger;
    int log_pos;                /* Index used for logging data into the kernel log */

    struct mutex lock;          /* Mutual exclusion semaphore */
    struct cdev cdev;           /* Char device structure */
};

struct bchd_dev *bchd_dev; /* allocated in bchd_init */


/*
 * Empty out the bchd device.
 * Here, we walk through the entire list and free any quantum and quantum sets we find.
 *
 * NOTE:
 *  -- Device semaphore must be held
 *  -- We assume dev != NULL
 */
void bchd_trim(struct bchd_dev *dev)
{
    struct bchd_qset *next, *dptr;
    int qset_size = dev->qset_size;
    int i;

    /* Iterate over all list items and free them */
    for (dptr = dev->data; dptr != NULL; dptr = next) {
        if (dptr->data != NULL) {
            /* Free all quanta */
            for (i = 0; i < qset_size; i++) {
                kfree(dptr->data[i]);
            }
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }

    dev->size = 0;
    dev->quantum_size = bchd_quantum_size;
    dev->qset_size = bchd_qset_size;
    dev->log_pos = 0;
    dev->data = NULL;
}

int bchd_open(struct inode *inode, struct file *filp)
{
    struct bchd_dev *dev;

    /*
     * The i_cdev field of inode contains the cdev structure we set up before.
     * However, we want the bchd_dev struct that contains this cdev struct.
     */
    dev = container_of(inode->i_cdev, struct bchd_dev, cdev);

    /* We use this in bchd_read and bchd_write to obtain the bchd_dev struct. */
    filp->private_data = dev;

    /*
     * Trim the length of the device to 0 if open was write only.
     * We do this since overwriting a bchd device with a shorter file
     * results in a shorter device data area.
     * This does nothing if the device is opened for reading.
     */
    if ( (filp->f_flags & O_ACCMODE) == O_WRONLY) {
        if (mutex_lock_interruptible(&dev->lock)) {
            return -ERESTARTSYS;
        }
        bchd_trim(dev);
        mutex_unlock(&dev->lock);
    }

    return 0;
}

int bchd_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * Follow the list to the index n and return a pointer to the corresponding item.
 * This procedure creates new items if necessary.
 */
struct bchd_qset * bchd_follow(struct bchd_dev *dev, int n)
{
    struct bchd_qset *qs = dev->data;

    /* Allocate first qset if necessary */
    if (qs == NULL) {
        qs = dev->data = kmalloc(sizeof(*qs), GFP_KERNEL);
        if (qs == NULL) {
            return NULL;
        }
        memset(qs, 0, sizeof(*qs));
    }

    /* Then follow the list */
    while (n--) {
        if (qs->next == NULL) {
            qs->next = kmalloc(sizeof(*qs->next), GFP_KERNEL);
            if (qs->next == NULL) {
                return NULL;
            }
            memset(qs->next, 0, sizeof(*qs->next));
        }
        qs = qs->next;
    }

    return qs;
}

ssize_t bchd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct bchd_dev *dev = filp->private_data;
    struct bchd_qset *dptr;     /* first list item */
    int quantum_size = dev->quantum_size;
    int qset_size = dev->qset_size;
    int item_size = quantum_size * qset_size;
    int item, qset_pos, q_pos, rest;
    ssize_t retval = 0;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    if (*f_pos >= dev->size) {
        goto out;
    }
    if (*f_pos + count > dev->size) {
        count = dev->size - *f_pos;
    }

    /* Find list item, qset index and quantum index (i.e. offset in the quantum) */
    item = (long) *f_pos / item_size;
    rest = (long) *f_pos % item_size;
    qset_pos = rest / quantum_size;
    q_pos = rest % quantum_size;

    /* Follow the list up to the right position */
    dptr = bchd_follow(dev, item);

    if (dptr == NULL || dptr->data == NULL || dptr->data[qset_pos] == NULL) {
        goto out; /* We do not fill holes */
    }

    /* Read only up to the end of this quantum */
    if (count > quantum_size - q_pos) {
        count = quantum_size - q_pos;
    }

    if (copy_to_user(buf, dptr->data[qset_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t bchd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct bchd_dev *dev = filp->private_data;
    struct bchd_qset *dptr;     /* first list item */
    int quantum_size = dev->quantum_size;
    int qset_size = dev->qset_size;
    int item_size = quantum_size * qset_size;
    int item, qset_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;  /* value used in "goto out" statements */

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /* Find list item, qset index and quantum index (i.e. offset in the quantum) */
    item = (long) *f_pos / item_size;
    rest = (long) *f_pos % item_size;
    qset_pos = rest / quantum_size;
    q_pos = rest % quantum_size;

    /* Follow the list up to the right position */
    dptr = bchd_follow(dev, item);
    if (dptr == NULL) {
        goto out;
    }
    if (dptr->data == NULL) {
        dptr->data = kmalloc(qset_size * sizeof(char *), GFP_KERNEL);
        if (dptr->data == NULL) {
            goto out;
        }
        memset(dptr->data, 0, qset_size * sizeof(char *));
    }
    if (dptr->data[qset_pos] == NULL) {
        dptr->data[qset_pos] = kmalloc(quantum_size, GFP_KERNEL);
        if (dptr->data[qset_pos] == NULL) {
            goto out;
        }
    }

    /* Write only up to the end of this quantum */
    if (count > quantum_size - q_pos) {
        count = quantum_size - q_pos;
    }

    if (copy_from_user(dptr->data[qset_pos] + q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

    /* Update the size */
    if (dev->size < *f_pos) {
        dev->size = *f_pos;
    }
 
out:
    mutex_unlock(&dev->lock);
    return retval;
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

    if (bchd_dev->wq_logger != NULL) {
        cancel_delayed_work_sync(&bchd_dev->ws_logger);
        destroy_workqueue(bchd_dev->wq_logger);
    }

    /* get rid of char dev entry */
    if (bchd_dev != NULL) {
        bchd_trim(bchd_dev);
        cdev_del(&bchd_dev->cdev);
        kfree(bchd_dev);
    }

    /* bchd_cleanup is never called if registering failed */
    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO "bchd: MODULE EXIT\n");
}

/*
 * Read the next word starting from dev->log_pos from the device
 * and write it into the kernel log.
 * A word is a sequence of characters followed by ' ' or '\n'.
 * Only up to BCHD_MAX_WORD_LEN characters are examined.
 */
static void bchd_log_word(struct work_struct *ws)
{
    struct bchd_dev *dev = container_of(ws, struct bchd_dev, ws_logger.work);

    struct bchd_qset *dptr; /* first list item */
    int quantum_size = dev->quantum_size;
    int qset_size = dev->qset_size;
    int *log_pos = &dev->log_pos;
    int max_cnt = dev->max_word_len;
    int item_size = quantum_size * qset_size; /* how many bytes in the list item */
    int item, qset_pos, q_pos, rest;
    char word[BCHD_MAX_WORD_LEN];
    int w = 0;  /* index to the word string */
    int i;      /* index used for counting how many characters we already logged */
    unsigned long delay;
    
    if (mutex_lock_interruptible(&dev->lock)) {
        goto out;
    }
    if (dev->size == 0) {
        printk(KERN_INFO "bchd: no text stored in /dev/bchd\n");
        /* Reschedule work in the work queue */
        delay = HZ; /* One second */
        queue_delayed_work(dev->wq_logger, &dev->ws_logger, delay);
        goto out;
    }
    /*  
     * If we already logged all stored words, we start again.
     * We have +1 here since we read <= max_cnt - 1 characters due to storing '\0' in the 
     * string that we write into the kernel log later.
     */  
    if (*log_pos + 1 >= dev->size) {
        *log_pos = 0;
    }
    if (*log_pos + max_cnt > dev->size) {
        max_cnt = dev->size - *log_pos;
    }

    /* find list item, qset index and offset in the quantum */
    item = (long) *log_pos / item_size;
    rest = (long) *log_pos % item_size;
    qset_pos = rest / quantum_size;
    q_pos = rest % quantum_size;

    /* follow the list up to the right position */
    dptr = bchd_follow(dev, item);
    if (dptr == NULL || dptr->data == NULL || dptr->data[qset_pos] == NULL) {
        goto out;
    }

    /* Read only up to the end of this quantum */
    if (max_cnt > quantum_size - q_pos) {
        max_cnt = quantum_size - q_pos;
    }

    /* 
     * Read a word (i.e. until we encounter ' ' or '\n')
     * or until we have advanced max_cnt - 1 (keep '\0' in mind) positions.
     */
    for (i = 0; i < max_cnt - 1; i++) {
        int c = *((char *) dptr->data[qset_pos] + q_pos + i);
        if (c == ' ' || c == '\n') { /* end of word */
            word[w] = ' ';
            w++;
            (*log_pos)++;
            break;
        }
        /*
         * These are the ASCII values we accept as word characters.
         * ' ' is the integer 32 and '~' is the integer 126,
         * that is, we accept all ASCII values in between these two.
         * We ignore everything else.
         *
         * NOTE: This might not work on non-ASCII systems!
         */
        if (c >= ' ' || c <= '~') {
            word[w] = c;
            w++;
            (*log_pos)++;
        }
    }
    word[w] = '\0';

    /* Write the word string into the kernel log */
    printk(KERN_INFO "bchd: %s\n", word);

    /* Reschedule work in the work queue */
    delay = HZ; /* One second */
    queue_delayed_work(dev->wq_logger, &dev->ws_logger, delay);
out:
    mutex_unlock(&dev->lock);
}

static int __init bchd_init(void)
{
    int result;
    dev_t dev = 0;
    unsigned long delay;

    /* Obtain device number */    
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
    bchd_dev->quantum_size = bchd_quantum_size;
    bchd_dev->qset_size = bchd_qset_size;
    bchd_dev->max_word_len = bchd_max_word_len;
    bchd_dev->wq_logger = create_singlethread_workqueue("wq_logger");
    if (bchd_dev->wq_logger == NULL) {
        printk(KERN_WARNING "bchd: failed to create wq_logger\n");
        result = -ENOMEM;
        goto fail;
    }
    INIT_DELAYED_WORK(&bchd_dev->ws_logger, bchd_log_word); 
    bchd_dev->log_pos = 0;
    mutex_init(&bchd_dev->lock);
    bchd_setup_cdev(bchd_dev);

    /* Each second a word from the stored text data is written into the kernel log */
    delay = HZ; /* One second ... HZ denotes the jiffies per second*/
    queue_delayed_work(bchd_dev->wq_logger, &bchd_dev->ws_logger, delay);

    printk(KERN_INFO "bchd: MODULE INIT -- device major: %d; device minor: %d\n", MAJOR(dev), MINOR(dev));
    return 0;   /* success */

fail:
    bchd_cleanup();
    return result;
}

module_init(bchd_init);
module_exit(bchd_cleanup);
