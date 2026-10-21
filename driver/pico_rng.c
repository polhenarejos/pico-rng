/**
 * Copyright (c) 2020 Mickey Malone.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/usb.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mickey Malone");
MODULE_DESCRIPTION("Random number generator using a Raspberry Pi Pico");
MODULE_VERSION("1.0");

/**
 * USB Device Macros
 **/
#define VENDOR_ID             0x0
#define PRODUCT_ID            0x4

/**
 * Logger Macros
 **/
#define LOGGER_INFO(fmt, args ...) printk( KERN_INFO "[info]  %s(%d): " fmt, __FUNCTION__, __LINE__, ## args)
#define LOGGER_ERR(fmt, args ...) printk( KERN_ERR "[err]  %s(%d): " fmt, __FUNCTION__, __LINE__, ## args)
#define LOGGER_WARN(fmt, args ...) printk( KERN_ERR "[warn]  %s(%d): " fmt, __FUNCTION__, __LINE__, ## args)
#define LOGGER_DEBUG(fmt, args ...) if (debug == 1) { printk( KERN_DEBUG "[debug]  %s(%d): " fmt, __FUNCTION__, __LINE__, ## args); }

/**
 * Enable module LOGGER_DEBUG macro. Defaults to 0 or false.
 **/
static int debug = 0;
module_param(debug, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(debug, "Set the log level to debug");

/**
 * Lever that will set the usb timeout in milliseconds. Defaults to 500.
 **/
static int timeout = 100;
module_param(timeout, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(timeout, "Set the read timeout in milliseconds for the pico rng usb device. Defaults to 100.");

/**
 * The main data structure for this module.
 **/
struct pico_rng_data {
	struct usb_device                      *dev;
	struct usb_interface                   *interface;
	struct usb_endpoint_descriptor         *endpoint;
	int                                    pipe;
	struct task_struct                     *rng_task;
} module_data;

/**
 * Prototype USB Functions
 **/
static int pico_rng_usb_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void pico_rng_usb_disconnect(struct usb_interface *interface);

/**
 * Prototype File Operation Functions
 **/ 
static int pico_rng_open(struct inode *inode, struct file *file);
static ssize_t pico_rng_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset);

/**
 * Prototype RNG Kthread Functions
 **/ 
static int pico_rng_kthread(void *data);
void pico_rng_kthread_start(void);
void pico_rng_kthread_stop(void);

/**
 * Prototype module Functions
 **/
static int pico_rng_read_data(void *buffer, int count);
static int __init pico_rng_driver_init(void);
static void __exit pico_rng_driver_exit(void);
module_init(pico_rng_driver_init);
module_exit(pico_rng_driver_exit); 

/**
 * Data structure of the USB vid:pid device that we will support
 **/
static struct usb_device_id pico_rng_usb_table [] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ }
};

/**
 * USB driver data structure
 **/
static struct usb_driver pico_rng_usb_driver = {
	.name           = "pico_rng",
	.id_table       = pico_rng_usb_table,
	.probe          = pico_rng_usb_probe,
	.disconnect     = pico_rng_usb_disconnect,
};

/**
 * File operations data structure for the character device
 * that will be implemented in this module
 **/
static struct file_operations pico_rng_fops = {
	.owner          = THIS_MODULE,
	.read           = pico_rng_read,
	.open           = pico_rng_open,
};

/**
 * USB class data structure
 **/
 struct usb_class_driver pico_rng_usb_class = {
	.name           = "pico_rng",
	.fops           = &pico_rng_fops,
};

/**
 * USB: Probe
 * This method will be called if the device we plug in matches the vid:pid we are listening for
 **/
static int pico_rng_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int retval = -ENODEV;

	module_data.dev = interface_to_usbdev(interface);
	if(!module_data.dev)
	{
		LOGGER_ERR("Unable to locate usb device\n");
		return retval;
	}

	module_data.interface = interface;
	if(!module_data.interface)
	{
		LOGGER_ERR("Invalid interface\n");
		return retval;
	}

	retval = usb_find_bulk_in_endpoint(module_data.interface->cur_altsetting, &(module_data.endpoint));
	if(retval)
	{
        LOGGER_ERR("Unable to find bulk endpoint %d\n", retval);
		return(retval);
    }

	module_data.pipe = usb_rcvbulkpipe(module_data.dev, module_data.endpoint->bEndpointAddress);

	LOGGER_DEBUG("endpoint found %p with pipe %d\n", module_data.endpoint, module_data.pipe);

	retval = usb_register_dev(module_data.interface, &pico_rng_usb_class);
	if(retval)
	{
		LOGGER_ERR("not able to get a minor for this device\n");
		return -1;
	}

	pico_rng_kthread_start();

	return retval;
}

/**
 * USB:disconnect
 * This method is used to cleanup everything after the device is disconnected
 **/
static void pico_rng_usb_disconnect(struct usb_interface *interface)
{
	LOGGER_INFO("pico rng usb device disconnected\n");
	pico_rng_kthread_stop();
	usb_deregister_dev(module_data.interface, &pico_rng_usb_class);
	module_data.dev = NULL;
	module_data.interface = NULL;
	module_data.pipe = 0;
}


/**
 * File:open
 * Does nothing, just here as a place holder
 **/
static int pico_rng_open(struct inode *inode, struct file *file)
{
	LOGGER_DEBUG("inside pico_rng_open with inode %p file %p\n", inode, file);
	return 0;
}

/**
 * File:read
 * Calls pico_rng_read_data() and returns module_data.endpoint->wMaxPacketSize bytes of data back to the user
 **/
static ssize_t pico_rng_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
    int bytes_read = 0;
	void *buffer = NULL;

	LOGGER_DEBUG("inside pico_rng_read with file %p, user_buffer %p, size %ld, offset %lld\n", file, user_buffer, size, *offset);

    buffer = kmalloc(module_data.endpoint->wMaxPacketSize, GFP_KERNEL);
	if(!buffer)
	{
		LOGGER_ERR("Failed to allocate buffer\n");
		return -EFAULT;
	}

	bytes_read = pico_rng_read_data(buffer, module_data.endpoint->wMaxPacketSize);
	if(bytes_read <= 0)
	{
		LOGGER_ERR("Failed to read data\n");
		kfree(buffer);
		return -EFAULT;
	}

    LOGGER_DEBUG("Copying %d bytest of random data to userspace with offset %lld\n", bytes_read, *offset);
	if(copy_to_user(user_buffer, buffer, bytes_read))
	{
		kfree(buffer);
        return -EFAULT;
	}
	
	kfree(buffer);
    return bytes_read;
}


/*
 * Pico rng thread that periodically adds hardware randomness
 */
static int pico_rng_kthread(void *data)
{
    int bytes_read;
	void *buffer = NULL;

	buffer = kmalloc(module_data.endpoint->wMaxPacketSize, GFP_KERNEL);
	if(!buffer)
	{
		LOGGER_ERR("RNG kthread failed to allocate buffer\n");
		return -ENOMEM;
	}

	while (!kthread_should_stop())
	{
		bytes_read = pico_rng_read_data(buffer, module_data.endpoint->wMaxPacketSize);
		if(bytes_read <= 0)
		{
			if (bytes_read == -ENODEV || bytes_read == -ESHUTDOWN)
			{
				LOGGER_INFO("Device disconnected, stopping kthread polling loop\n");
				break;
			}

			LOGGER_ERR("Failed to read data\n");

			// sleep for at least a second, max 2 seconds
			usleep_range(1000000, 2000000);

			continue;
		}

        LOGGER_DEBUG("Adding hardware randomness\n");
		// I would not exactly call this rng as trusted, so it will not add entropy, only random bits to the pool
		// A trusted device would call add_hwgenerator_randomness and credit the entropy pool. For now the credit is 0 while still adding random bits.
		add_hwgenerator_randomness(buffer, bytes_read, 0, false);
		LOGGER_DEBUG("Randomness added\n");
	}

	kfree(buffer);

	/*
	 * If loop ended due to USB unplug, park thread until kthread_stop()
	 * is explicitly called in pico_rng_usb_disconnect to prevent double free.
	 */
	while (!kthread_should_stop())
	{
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	set_current_state(TASK_RUNNING);

    return 0;
}

/**
 * Start the rng thread
 **/
void pico_rng_kthread_start()
{
    module_data.rng_task = kthread_run(pico_rng_kthread, &module_data, "pico_rng_thread");
	if(IS_ERR(module_data.rng_task))
	{
        module_data.rng_task = NULL;
		LOGGER_ERR("Failed to launch the pico rng task\n");
	}
}

/**
 * Stop the rng thread
 **/
void pico_rng_kthread_stop()
{
	if(module_data.rng_task)
	{
		kthread_stop(module_data.rng_task);
		module_data.rng_task = NULL;
	}
}

/**
 * Read data from the pico rng. 
 * Fills the buffer and returns the number of bytes filled.
 * Count  is the maximum number of bytes the buffer can hold.
 */
static int pico_rng_read_data(void *buffer, int count)
{
    int retval = 0;
	int actual_length = 0;

    // int usb_bulk_msg(struct usb_device *usb_dev, unsigned int pipe, void *data, int len, int *actual_length, int timeout)
	LOGGER_DEBUG("Calling usb_bulk_msg dev %p, pipe %u, buffer %p, size %d, and timeout %d", \
	        module_data.dev, module_data.pipe, buffer, count, timeout);

	if (!module_data.dev)
		return -ENODEV;

    retval = usb_bulk_msg(module_data.dev, 
	                      module_data.pipe,
						  buffer,
						  count,
						  &actual_length,
						  timeout);
	
	if(retval)
	{
		if (retval == -ENODEV || retval == -ESHUTDOWN)
			return retval;
		return -EFAULT;
	}

    return actual_length;
}

/**
 * Module:init
 **/
static int __init pico_rng_driver_init(void)
{
	int retval = 0;
   	LOGGER_INFO("pico rng driver debut\n");
	
	retval = usb_register(&pico_rng_usb_driver);
	if(retval)
	{
		LOGGER_ERR("registering pico rng driver failed\n");
		return retval;
	}
	else
	{
		LOGGER_INFO("pico rng driver registered successfully\n");
	}

	return 0;
}

/**
 * Module:exit
 **/
static void __exit pico_rng_driver_exit(void)
{
	usb_deregister(&pico_rng_usb_driver);
	return;
}
