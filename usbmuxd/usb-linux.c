/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <libusb.h>

#include "usb.h"
#include "log.h"
#include "device.h"

// interval for device connection/disconnection polling, in milliseconds
// we need this because there is currently no asynchronous device discovery mechanism in libusb
#define DEVICE_POLL_TIME 1000

struct usb_device {
	libusb_device_handle *dev;
	uint8_t bus, address;
	uint16_t vid, pid;
	char serial[256];
	int alive;
	struct libusb_transfer *rx_xfer;
	struct collection tx_xfers;
};

static struct collection device_list;

static struct timeval next_dev_poll_time;

static int devlist_failures;

static void usb_disconnect(struct usb_device *dev)
{
	if(!dev->dev) {
		return;
	}
	
	// kill the rx xfer and tx xfers and try to make sure the callbacks get called before we free the device
	if(dev->rx_xfer) {
		usbmuxd_log(LL_DEBUG, "usb_disconnect: cancelling RX xfer");
		libusb_cancel_transfer(dev->rx_xfer);
	}
	FOREACH(struct libusb_transfer *xfer, &dev->tx_xfers) {
		usbmuxd_log(LL_DEBUG, "usb_disconnect: cancelling TX xfer %p", xfer);
		libusb_cancel_transfer(xfer);
	} ENDFOREACH

	while(dev->rx_xfer || collection_count(&dev->tx_xfers)) {
		struct timeval tv;
		int res;
		
		tv.tv_sec = 0;
		tv.tv_usec = 1000;
		if((res = libusb_handle_events_timeout(NULL, &tv)) < 0) {
			usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout for usb_disconnect failed: %d", res);
			break;
		}
	}
	collection_free(&dev->tx_xfers);
	libusb_release_interface(dev->dev, USB_INTERFACE);
	libusb_close(dev->dev);
	dev->dev = NULL;
	collection_remove(&device_list, dev);
	free(dev);
}

static void tx_callback(struct libusb_transfer *xfer)
{
	struct usb_device *dev = xfer->user_data;
	usbmuxd_log(LL_SPEW, "TX callback dev %d-%d len %d -> %d status %d", dev->bus, dev->address, xfer->length, xfer->actual_length, xfer->status);
	if(xfer->status != LIBUSB_TRANSFER_COMPLETED) {
		switch(xfer->status) {
			case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
			case LIBUSB_TRANSFER_ERROR:
				// funny, this happens when we disconnect the device while waiting for a transfer, sometimes
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to error or disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_TIMED_OUT:
				usbmuxd_log(LL_ERROR, "TX transfer timed out for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_CANCELLED:
				usbmuxd_log(LL_ERROR, "TX transfer cancelled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_STALL:
				usbmuxd_log(LL_ERROR, "TX transfer stalled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_NO_DEVICE:
				// other times, this happens, and also even when we abort the transfer after device removal
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_OVERFLOW:
				usbmuxd_log(LL_ERROR, "TX transfer overflow for device %d-%d", dev->bus, dev->address);
				break;
			// and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
		}
		// we can't usb_disconnect here due to a deadlock, so instead mark it as dead and reap it after processing events
		// we'll do device_remove there too
		dev->alive = 0;
	}
	if(xfer->buffer)
		free(xfer->buffer);
	collection_remove(&dev->tx_xfers, xfer);
	libusb_free_transfer(xfer);
}

int usb_send(struct usb_device *dev, const unsigned char *buf, int length)
{
	int res;
	struct libusb_transfer *xfer = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(xfer, dev->dev, BULK_OUT, (void*)buf, length, tx_callback, dev, 0);
	xfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;
	if((res = libusb_submit_transfer(xfer)) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit TX transfer %p len %d to device %d-%d: %d", buf, length, dev->bus, dev->address, res);
		libusb_free_transfer(xfer);
		return res;
	}
	collection_add(&dev->tx_xfers, xfer);/*
	if((length % 512) == 0) {
		usbmuxd_log(LL_DEBUG, "Send ZLP");
		// Send Zero Length Packet
		xfer = libusb_alloc_transfer(0);
		void *buffer = malloc(1);
		libusb_fill_bulk_transfer(xfer, dev->dev, BULK_OUT, buffer, 0, tx_callback, dev, 0);
		xfer->flags = LIBUSB_TRANSFER_SHORT_NOT_OK;
		if((res = libusb_submit_transfer(xfer)) < 0) {
			usbmuxd_log(LL_ERROR, "Failed to submit TX ZLP transfer to device %d-%d: %d", dev->bus, dev->address, res);
			libusb_free_transfer(xfer);
			return res;
		}
		collection_add(&dev->tx_xfers, xfer);
	}*/
	return 0;
}

static void rx_callback(struct libusb_transfer *xfer)
{
	struct usb_device *dev = xfer->user_data;
	usbmuxd_log(LL_SPEW, "RX callback dev %d-%d len %d status %d", dev->bus, dev->address, xfer->actual_length, xfer->status);
	if(xfer->status == LIBUSB_TRANSFER_COMPLETED) {
		device_data_input(dev, xfer->buffer, xfer->actual_length);
		libusb_submit_transfer(xfer);
	} else {
		switch(xfer->status) {
			case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
			case LIBUSB_TRANSFER_ERROR:
				// funny, this happens when we disconnect the device while waiting for a transfer, sometimes
				usbmuxd_log(LL_INFO, "Device %d-%d RX aborted due to error or disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_TIMED_OUT:
				usbmuxd_log(LL_ERROR, "RX transfer timed out for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_CANCELLED:
				usbmuxd_log(LL_ERROR, "RX transfer cancelled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_STALL:
				usbmuxd_log(LL_ERROR, "RX transfer stalled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_NO_DEVICE:
				// other times, this happens, and also even when we abort the transfer after device removal
				usbmuxd_log(LL_INFO, "Device %d-%d RX aborted due to disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_OVERFLOW:
				usbmuxd_log(LL_ERROR, "RX transfer overflow for device %d-%d", dev->bus, dev->address);
				break;
			// and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
		}
		free(xfer->buffer);
		dev->rx_xfer = NULL;
		libusb_free_transfer(xfer);
		// we can't usb_disconnect here due to a deadlock, so instead mark it as dead and reap it after processing events
		// we'll do device_remove there too
		dev->alive = 0;
	}
}

static int start_rx(struct usb_device *dev)
{
	int res;
	void *buf;
	dev->rx_xfer = libusb_alloc_transfer(0);
	buf = malloc(USB_MRU);
	libusb_fill_bulk_transfer(dev->rx_xfer, dev->dev, BULK_IN, buf, USB_MRU, rx_callback, dev, 0);
	if((res = libusb_submit_transfer(dev->rx_xfer)) != 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit RX transfer to device %d-%d: %d", dev->bus, dev->address, res);
		libusb_free_transfer(dev->rx_xfer);
		dev->rx_xfer = NULL;
		return res;
	}
	return 0;
}

static int usb_discover(void)
{
	int cnt, i, res;
	int valid_count = 0;
	libusb_device **devs;
	
	cnt = libusb_get_device_list(NULL, &devs);
	if(cnt < 0) {
		usbmuxd_log(LL_WARNING, "Could not get device list: %d", cnt);
		devlist_failures++;
		// sometimes libusb fails getting the device list if you've just removed something
		if(devlist_failures > 5) {
			usbmuxd_log(LL_FATAL, "Too many errors getting device list\n");
			return cnt;
		} else {
			gettimeofday(&next_dev_poll_time, NULL);
			next_dev_poll_time.tv_usec += DEVICE_POLL_TIME * 1000;
			next_dev_poll_time.tv_sec += next_dev_poll_time.tv_usec / 1000000;
			next_dev_poll_time.tv_usec = next_dev_poll_time.tv_usec % 1000000;
			return 0;
		}
	}
	devlist_failures = 0;

	usbmuxd_log(LL_SPEW, "usb_discover: scanning %d devices", cnt);

	FOREACH(struct usb_device *usbdev, &device_list) {
		usbdev->alive = 0;
	} ENDFOREACH

	for(i=0; i<cnt; i++) {
		// the following are non-blocking operations on the device list
		libusb_device *dev = devs[i];
		uint8_t bus = libusb_get_bus_number(dev);
		uint8_t address = libusb_get_device_address(dev);
		struct libusb_device_descriptor devdesc;
		int found = 0;
		FOREACH(struct usb_device *usbdev, &device_list) {
			if(usbdev->bus == bus && usbdev->address == address) {
				valid_count++;
				usbdev->alive = 1;
				found = 1;
				break;
			}
		} ENDFOREACH
		if(found)
			continue; //device already found
		if((res = libusb_get_device_descriptor(dev, &devdesc)) != 0) {
			usbmuxd_log(LL_WARNING, "Could not get device descriptor for device %d-%d: %d", bus, address, res);
			continue;
		}
		if(devdesc.idVendor != VID_APPLE)
			continue;
		if( (devdesc.idProduct != PID_IPHONE2G) &&
			(devdesc.idProduct != PID_ITOUCH1G) &&
			(devdesc.idProduct != PID_IPHONE3G))
			continue;
		libusb_device_handle *handle;
		usbmuxd_log(LL_INFO, "Found new device with v/p %04x:%04x at %d-%d", devdesc.idVendor, devdesc.idProduct, bus, address);
		// potentially blocking operations follow; they will only run when new devices are detected, which is acceptable
		if((res = libusb_open(dev, &handle)) != 0) {
			usbmuxd_log(LL_WARNING, "Could not open device %d-%d: %d", bus, address, res);
			continue;
		}
		if((res = libusb_set_configuration(handle, USB_CONFIGURATION)) != 0) {
			usbmuxd_log(LL_WARNING, "Could not set configuration %d for device %d-%d: %d", USB_CONFIGURATION, bus, address, res);
			libusb_close(handle);
			continue;
		}
		if((res = libusb_claim_interface(handle, USB_INTERFACE)) != 0) {
			usbmuxd_log(LL_WARNING, "Could not claim interface %d for device %d-%d: %d", USB_INTERFACE, bus, address, res);
			libusb_close(handle);
			continue;
		}
		struct usb_device *usbdev;
		usbdev = malloc(sizeof(struct usb_device));

		if((res = libusb_get_string_descriptor_ascii(handle, devdesc.iSerialNumber, (uint8_t *)usbdev->serial, 256)) <= 0) {
			usbmuxd_log(LL_WARNING, "Could not get serial number for device %d-%d: %d", bus, address, res);
			libusb_release_interface(handle, USB_INTERFACE);
			libusb_close(handle);
			free(usbdev);
			continue;
		}
		usbdev->serial[res] = 0;
		usbdev->bus = bus;
		usbdev->address = address;
		usbdev->vid = devdesc.idVendor;
		usbdev->pid = devdesc.idProduct;
		usbdev->dev = handle;
		usbdev->alive = 1;
		collection_init(&usbdev->tx_xfers);

		collection_add(&device_list, usbdev);
		
		if(device_add(usbdev) < 0) {
			usb_disconnect(usbdev);
			continue;
		}
		if(start_rx(usbdev) < 0) {
			device_remove(usbdev);
			usb_disconnect(usbdev);
			continue;
		}
		valid_count++;
	}
	FOREACH(struct usb_device *usbdev, &device_list) {
		if(!usbdev->alive) {
			device_remove(usbdev);
			usb_disconnect(usbdev);
		}
	} ENDFOREACH
	
	libusb_free_device_list(devs, 1);
	
	gettimeofday(&next_dev_poll_time, NULL);
	next_dev_poll_time.tv_usec += DEVICE_POLL_TIME * 1000;
	next_dev_poll_time.tv_sec += next_dev_poll_time.tv_usec / 1000000;
	next_dev_poll_time.tv_usec = next_dev_poll_time.tv_usec % 1000000;
	
	return valid_count;
}

const char *usb_get_serial(struct usb_device *dev)
{
	if(!dev->dev) {
		return NULL;
	}
	return dev->serial;
}

uint32_t usb_get_location(struct usb_device *dev)
{
	if(!dev->dev) {
		return 0;
	}
	return (dev->bus << 16) | dev->address;
}

uint16_t usb_get_pid(struct usb_device *dev)
{
	if(!dev->dev) {
		return 0;
	}
	return dev->pid;
}

void usb_get_fds(struct fdlist *list)
{
	const struct libusb_pollfd **usbfds;
	const struct libusb_pollfd **p;
	usbfds = libusb_get_pollfds(NULL);
	if(!usbfds) {
		usbmuxd_log(LL_ERROR, "libusb_get_pollfds failed");
		return;
	}
	p = usbfds;
	while(*p) {
		fdlist_add(list, FD_USB, (*p)->fd, (*p)->events);
		p++;
	}
	free(usbfds);
}

static int dev_poll_remain_ms(void)
{
	int msecs;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	msecs = (next_dev_poll_time.tv_sec - tv.tv_sec) * 1000;
	msecs += (next_dev_poll_time.tv_usec - tv.tv_usec) / 1000;
	if(msecs < 0)
		return 0;
	return msecs;
}

int usb_get_timeout(void)
{
	struct timeval tv;
	int msec;
	int res;
	int pollrem;
	pollrem = dev_poll_remain_ms();
	res = libusb_get_next_timeout(NULL, &tv);
	if(res == 0)
		return pollrem;
	if(res < 0) {
		usbmuxd_log(LL_ERROR, "libusb_get_next_timeout failed: %d", res);
		return pollrem;
	}
	msec = tv.tv_sec * 1000;
	msec += tv.tv_usec / 1000;
	if(msec > pollrem)
		return pollrem;
	return msec;
}

int usb_process(void)
{
	int res;
	struct timeval tv;
	tv.tv_sec = tv.tv_usec = 0;
	res = libusb_handle_events_timeout(NULL, &tv);
	if(res < 0) {
		usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout failed: %d", res);
		return res;
	}
	// reap devices marked dead due to an RX error
	FOREACH(struct usb_device *usbdev, &device_list) {
		if(!usbdev->alive) {
			device_remove(usbdev);
			usb_disconnect(usbdev);
		}
	} ENDFOREACH

	if(dev_poll_remain_ms() <= 0) {
		res = usb_discover();
		if(res < 0) {
			usbmuxd_log(LL_ERROR, "usb_discover failed: %d", res);
			return res;
		}
	}
	return 0;
}

int usb_process_timeout(int msec)
{
	int res;
	struct timeval tleft, tcur, tfin;
	gettimeofday(&tcur, NULL);
	tfin.tv_sec = tcur.tv_sec + (msec / 1000);
	tfin.tv_usec = tcur.tv_usec + (msec % 1000) * 1000;
	tfin.tv_sec += tfin.tv_usec / 1000000;
	tfin.tv_usec %= 1000000;
	while((tfin.tv_sec > tcur.tv_sec) || ((tfin.tv_sec == tcur.tv_sec) && (tfin.tv_usec > tcur.tv_usec))) {
		tleft.tv_sec = tfin.tv_sec - tcur.tv_sec;
		tleft.tv_usec = tfin.tv_usec - tcur.tv_usec;
		if(tleft.tv_usec < 0) {
			tleft.tv_usec += 1000000;
			tleft.tv_sec -= 1;
		}
		res = libusb_handle_events_timeout(NULL, &tleft);
		if(res < 0) {
			usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout failed: %d", res);
			return res;
		}
		// reap devices marked dead due to an RX error
		FOREACH(struct usb_device *usbdev, &device_list) {
			if(!usbdev->alive) {
				device_remove(usbdev);
				usb_disconnect(usbdev);
			}
		} ENDFOREACH
	gettimeofday(&tcur, NULL);
	}
	return 0;
}

int usb_init(void)
{
	int res;
	usbmuxd_log(LL_DEBUG, "usb_init for linux / libusb 1.0");
	
	devlist_failures = 0;
	res = libusb_init(NULL);
	//libusb_set_debug(NULL, 3);
	if(res != 0) {
		usbmuxd_log(LL_FATAL, "libusb_init failed: %d", res);
		return -1;
	}
	
	collection_init(&device_list);
	
	return usb_discover();
}

void usb_shutdown(void)
{
	usbmuxd_log(LL_DEBUG, "usb_shutdown");
	FOREACH(struct usb_device *usbdev, &device_list) {
		device_remove(usbdev);
		usb_disconnect(usbdev);
	} ENDFOREACH
	collection_free(&device_list);
	libusb_exit(NULL);
}