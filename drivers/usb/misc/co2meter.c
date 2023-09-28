// SPDX-License-Identifier: GPL-2.0+
/* co2meter driver v0.1

Copyright (C) 2023 by Ben Chaney <chaneybenjamini@gmail.com>

Derived from the USB Skeleton driver 1.1,
Copyright (C) 2003 Greg Kroah-Hartman (greg@kroah.com)
        
*/

#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#define ID_HOLTEK 0x04d9
#define ID_CO2MINI 0xa052

#define DRIVER_AUTHOR "Ben Chaney <chaneybenjamini@gmail.com>"
#define DRIVER_DESC "CO2Meter sensor driver"

static const struct usb_device_id id_co2_table[] = {
	{USB_DEVICE(ID_HOLTEK, ID_CO2MINI)},
	{}
};

MODULE_DEVICE_TABLE(usb, id_co2_table);

//TODO: get minor number
#define CO2MON_MINOR_BASE 192

struct usb_co2meter {
	struct mutex lock;
	struct work_struct work_data;
	struct usb_device *active_device;
	struct usb_endpoint_descriptor *read_endpoint;
	struct usb_interface *active_interface;
	u32 co2_level;
	int ready;
};

static int co2meter_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void co2meter_disconnect(struct usb_interface *interface);
static int co2meter_open(struct inode *inode, struct file *file);
static ssize_t co2meter_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos);
static int co2meter_release(struct inode *inode, struct file *file);
static void co2meter_delete(struct usb_co2meter *dev);
static void co2meter_handle_input (struct work_struct *dev);

//TODO: handle suspension
static struct usb_driver co2meter_driver = {
	.name = "co2meter",
	.probe = co2meter_probe,
	.disconnect = co2meter_disconnect,
	.id_table = id_co2_table,
};

static const struct file_operations co2meter_fops_co2 = {
	.owner = THIS_MODULE,
	.open = co2meter_open,
	.release = co2meter_release,
	.read = co2meter_read,
};

//TODO: add temperature?
struct usb_class_driver co2meter_class_co2 = {
	.name = "co2meter%d",
	.fops = &co2meter_fops_co2,
	.minor_base = CO2MON_MINOR_BASE
};

#define USB_BUF_SIZE 16
static void co2meter_handle_input (struct work_struct *work)
{
	struct usb_co2meter *dev = container_of(work, struct usb_co2meter, work_data);
	mutex_lock(&dev->lock);

	u8 *buf = kzalloc(sizeof(u8) * USB_BUF_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_err(&dev->active_interface->dev, "Could not allocate buffer for transfer\n");
		schedule_work(&dev->work_data);
		return;
	}

	int transfer_size;
	
	struct usb_device *udev = dev->active_device;
	unsigned int ep_addr = dev->read_endpoint->bEndpointAddress;

	mutex_unlock(&dev->lock);
	int retval = usb_bulk_msg(udev,
				  usb_rcvbulkpipe(udev, ep_addr),
				  buf,
				  USB_BUF_SIZE,
				  &transfer_size,
				  5000);
	if (!retval &&
	    transfer_size >= 5 &&
	    buf[0] == 0x50 &&
	    buf[0] + buf[1] + buf[2] == buf[3] && 
	    buf[4] == 0x0d) {
		mutex_lock(&dev->lock);
		dev->ready = 1;
		dev->co2_level = (buf[1] * 256) + buf[2];
		mutex_unlock(&dev->lock);
	}
	kfree(buf);
	schedule_work(&dev->work_data);
}

static int co2meter_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_co2meter *dev;

	dev = kzalloc(sizeof (*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	
	mutex_init(&dev->lock);
	dev->ready = 0;
	
	dev->active_interface = interface;
	dev->active_device = interface_to_usbdev(interface);

	int retval = usb_find_common_endpoints(interface->cur_altsetting,
					       NULL, NULL, &dev->read_endpoint, NULL);

	if (retval) {
		dev_err(&interface->dev, "Could not find bulk-in endpoint %d\n", retval);
		co2meter_delete(dev);
		return retval;
	}
	
	int result = usb_register_dev(interface, &co2meter_class_co2);
	if (result) {
		//device regionstration failed
		dev_err(&interface->dev, "Unable to allocate minor number %d\n", result);
		co2meter_delete(dev);
		return result;
	}
	

	usb_set_intfdata(interface, dev);
	INIT_WORK(&dev->work_data, co2meter_handle_input);

	schedule_work(&dev->work_data);
	//pmac_backlight_key_workerqueue_work(dev, co2meter_handle_input);

	return 0;
}

static void co2meter_delete(struct usb_co2meter *dev)
{
	kfree(dev);
}

static void co2meter_disconnect(struct usb_interface *interface)
{
	struct usb_co2meter *dev = usb_get_intfdata(interface);

	cancel_work_sync(&dev->work_data);

	mutex_lock(&dev->lock);

	usb_deregister_dev(interface, &co2meter_class_co2);

	dev = usb_get_intfdata(interface);
	co2meter_delete(dev);
}

#define CO2METER_DATA_SIZE 32
static struct co2meter_file_data {
	char buf[CO2METER_DATA_SIZE];
	size_t buf_size;
};

static int co2meter_open(struct inode *inode, struct file *file)
{
	struct usb_co2meter *dev;
	struct usb_interface *interface;

	interface = usb_find_interface(&co2meter_driver, iminor(inode));
	if(!interface)
		return -ENODEV;
	
	dev = usb_get_intfdata(interface);
	mutex_lock(&dev->lock);

	if(!dev->ready){
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}

	struct co2meter_file_data *file_data = kzalloc(sizeof(struct co2meter_file_data), GFP_KERNEL);
	file_data->buf_size = scnprintf(file_data->buf, CO2METER_DATA_SIZE, "%u\n", dev->co2_level);
	file->private_data = file_data;
	mutex_unlock(&dev->lock);
	return 0;
}

static ssize_t co2meter_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	if (!file->private_data)
		return -ENODEV;

	struct co2meter_file_data *file_data = file->private_data;

	return simple_read_from_buffer(buffer, count, ppos, file_data->buf, file_data->buf_size);

}

static int co2meter_release(struct inode *inode, struct file *file)
{
	if(file->private_data)
		kfree(file->private_data);

	return 0;
}


module_usb_driver(co2meter_driver);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
