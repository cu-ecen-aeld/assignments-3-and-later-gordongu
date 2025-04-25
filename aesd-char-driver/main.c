/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Your Name Here"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */

    struct aesd_dev *dev;

    // Get container from inode and reserve file pointer private data to container
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    filp->f_pos = 0;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */

    // Release file pointer private data
    filp->private_data = NULL;

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */

    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;

    // Return with error if mutex cannot be obtained
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Get entry and entry_offset with given f_pos
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset);

    // Return with error if anything is NULL
    if (entry == NULL || entry->buffptr == NULL) {
        retval = 0;
        goto out_unlock;
    }

    // Read only amount requested or the remaining size of the buffer from the offset position (whichever is less)
    size_t bytes_to_copy = min(entry->size - entry_offset, count);
    
    // Copy read data to buffer in user space
    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy) != 0) {
        retval = -EFAULT;
        goto out_unlock;
    }

    // Adjust f_pos by amount of bytes read
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out_unlock:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */

    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry entry;
    ssize_t total_count = dev->leftover.size + count;
    void *buffer = kmalloc(total_count, GFP_KERNEL);
    void *newline_ptr = NULL;

    entry.buffptr = buffer;
    entry.size = total_count;

    // Return with error if buffer is NULL
    if (entry.buffptr == NULL) {
        goto out;
    }

    // Copy data from previous unfinished write
    if (dev->leftover.buffptr != NULL) {
        memcpy((void *)entry.buffptr, dev->leftover.buffptr, dev->leftover.size);
    }

    // Copy write data from buffer in user space
    if (copy_from_user((void *)entry.buffptr + dev->leftover.size, buf, count) != 0) {
        retval = -EFAULT;
        goto out;
    }

    // Free data from previous unfinished write
    if (dev->leftover.buffptr != NULL) {
        kfree((void *)dev->leftover.buffptr);
        dev->leftover.buffptr = NULL;
        dev->leftover.size = 0;
    }

    // Return with error if mutex cannot be obtained
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Find newline character occurrence
    newline_ptr = memchr(entry.buffptr, '\n', entry.size);

    while (entry.size > 0 && newline_ptr != NULL) {
        // Copy only the amount of bytes leading up to the newline character
        size_t bytes_to_copy = newline_ptr - (void *)entry.buffptr + 1;
        struct aesd_buffer_entry *duplicate = kmalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);

        // Duplicate memory to new temporary buffer
        duplicate->buffptr = kmemdup(entry.buffptr, bytes_to_copy, GFP_KERNEL);

        // Return with error if memory duplication failed
        if (duplicate->buffptr == NULL) {
            retval = -EINVAL;
            goto out_unlock;
        }

        duplicate->size = bytes_to_copy;

        // Clean up oldest buffer entry if buffer is full
        const char *oldptr = aesd_circular_buffer_add_entry(&dev->circular_buffer, duplicate);

        if (oldptr != NULL) {
            kfree((void *)oldptr);
        }

        // Adjust variables and repeat loop until there are no more newline occurrences
        entry.size -= bytes_to_copy;
        entry.buffptr += bytes_to_copy;
        newline_ptr = memchr(entry.buffptr, '\n', entry.size);
    }

    // Save remaining data after last newline occurrence to be written in the next write operation
    if (entry.size > 0) {
        dev->leftover.buffptr = kmemdup(entry.buffptr, entry.size, GFP_KERNEL);

        if (dev->leftover.buffptr == NULL) {
            retval = -ENOMEM;
            goto out_unlock;
        }

        dev->leftover.size = entry.size;
    }

    // Adjust f_pos by amount of bytes written
    *f_pos += count;
    retval = count;
  
out_unlock:
    mutex_unlock(&dev->lock);
out:
    if (buffer != NULL) {
        kfree((void *)buffer);
    }
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    
    // Initialize circular buffer and mutex
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

    // Free all entries in the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr != NULL) {
            kfree((void *)entry->buffptr);
            entry->buffptr = NULL;
        }
    }

    // Free data in the leftover buffer entry
    if (aesd_device.leftover.buffptr != NULL) {
        kfree((void *)aesd_device.leftover.buffptr);
        aesd_device.leftover.buffptr = NULL;
        aesd_device.leftover.size = 0;
    }

    // Free mutex
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
