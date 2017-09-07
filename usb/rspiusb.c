/*
 * rspiusb.c
 *
 * Copyright (C) 2005, 2006 Princeton Instruments
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation version 2 of the License
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#if	LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
#include <asm/scatterlist.h>
#else
#include <linux/scatterlist.h>
#endif
#include <linux/mm.h>
#include <linux/pci.h> //for scatterlist macros
#include <linux/pagemap.h>
#include "rspiusb.h"

#ifdef CONFIG_USB_DEBUG
  static int debug = 1;
#else
  static int debug;
#endif
/* Use our own dbg macro */
#undef dbg
#define dbg(format, arg...) do { if (debug) printk(KERN_DEBUG "rspiusb: " format "\n" , ## arg); } while (0)

// Reenable to use the userspace <-> kernel DMA mapping (that currently doesn't work)
//#define USE_DMA_MAPPING

/* Version Information */
#define DRIVER_VERSION "V1.0.3"
#define DRIVER_DESC "PI USB2.0 Device Driver for Linux"

static struct usb_driver piusb_driver;

static int lastErr;
static int errCnt;

/**
 *  piusb_write_bulk_callback
 *  called when the urb submitted by piusb_write_bulk is done writing.
 */
static void piusb_write_bulk_callback(struct urb *urb)
{
	struct device_extension *pdx = urb->context;
	int status = urb->status;

	/* sync/async unlink faults aren't errors */
	if (status) {
		if (status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN) {
			dev_dbg(&urb->dev->dev,
				"%s - nonzero write bulk early end, status: %d",
				__func__, -status);
		} else {
			dev_dbg(&urb->dev->dev,
				"%s - nonzero write bulk status received: %d",
				__func__, -status);
		}
	}
	pdx->pendingWrite = 0;
	kfree(urb->transfer_buffer);
}

/**
 * Called from user-space (via the IOCTL) to send some data to one of the output
 * bulk endpoints (eg: on the PIXIS 1 or 8). It asynchronous.
 * Returns the number of bytes written (sent).
 */
static int piusb_write_bulk(ioctl_struct *io, unsigned char *uBuf, int len, struct device_extension *pdx)
{
	struct urb *urb = NULL;
	int retval = 0;
	unsigned char *kbuf = NULL;

	urb = usb_alloc_urb( 0, GFP_KERNEL );
	if (urb == NULL) {
		retval = -ENOMEM;
		goto done;
	}

	kbuf = kmalloc(len, GFP_KERNEL);
	if (kbuf == NULL) {
		dev_err(&pdx->udev->dev, "buffer_alloc failed\n");
		retval = -ENOMEM;
		goto done;
	}
	if (copy_from_user(kbuf, uBuf, len)) {
		dev_err(&pdx->udev->dev, "copy_from_user failed\n");
		retval = -EFAULT;
		goto done;
	}

	usb_fill_bulk_urb( urb, pdx->udev, pdx->hEP[io->endpoint], kbuf, len, piusb_write_bulk_callback, pdx );

	retval = usb_submit_urb(urb, GFP_KERNEL);
	if (retval) {
		dev_err(&pdx->udev->dev, "WRITE ERROR: submit urb error = %d\n", retval);
		goto done;
	}
	dbg("sending %d bytes to pipe %d\n", len, io->endpoint);
	pdx->pendingWrite = 1;
	retval = len;

done:
	usb_free_urb(urb);
	return retval;
}


#ifdef USE_DMA_MAPPING
static void piusb_read_pixel_callback ( struct urb *urb )
{
	struct device_extension *pdx = urb->context;
	int status = urb->status;

	if (status &&
	    // for these 3 errors -> we might have still received something
	    !(status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)) {
		dbg("%s - nonzero read bulk status received: %d", __func__, urb->status);
		dbg( "Error in read EP2 callback" );
		dbg( "FrameIndex = %d", pdx->frameIdx );
		dbg( "Bytes received before problem occurred = %d", pdx->bulk_in_byte_trk );
		dbg( "Urb Idx = %d", pdx->urbIdx );
		pdx->pendedPixelUrbs[pdx->frameIdx][pdx->urbIdx] = 0;
		return;
	}

	pdx->bulk_in_byte_trk += urb->actual_length;

	pdx->urbIdx++;  //point to next URB when we callback
	if( pdx->bulk_in_byte_trk >= pdx->frameSize ) {
		pdx->bulk_in_size_returned = pdx->bulk_in_byte_trk;
		pdx->bulk_in_byte_trk = 0;
		pdx->gotPixelData = 1;
		pdx->frameIdx = ( ( pdx->frameIdx + 1 ) % pdx->num_frames );
		pdx->urbIdx = 0;
	}

	// The user interface expects us to keep listening to the
	// camera until the buffer is unmapped. So resubmit the same URB to
	// keep filling the cyclic buffer. (Unless it has been trying to stop)
	// eg urb->status == -ENOENT means UnMapBuffer has been called (and the urb
	// was killed)
	if (!urb->status) {
		int err=0;
		err = usb_submit_urb( urb, GFP_ATOMIC ); //resubmit the URB
		if( err && err != -EPERM ) {
			errCnt++;
			if( err != lastErr ) {
				dbg("submit urb in callback failed with error code %d", -err );
				lastErr = err;
			}
			return;
		} else if (err == -EPERM)
			dbg("submit urb in callback failed, due to shutdown" );
	}
}

/**
 * Unmap the user buffer from the DMA, and also stop receiving data from the
 * camera (by killing the URB, which will prevent the callback from resubmitting
 * it).
 */
static int UnMapUserBuffer( struct device_extension *pdx )
{
	int i, k;
	unsigned int epAddr;

	if (!pdx->PixelUrb)
		return -EINVAL; // not initialized yet

	for( k = 0; k < pdx->num_frames; k++ ) {
		dbg("Killing Urbs for Frame %d", k );
		for( i = 0; i < pdx->sgEntries[k]; i++ ) {
			usb_kill_urb( pdx->PixelUrb[k][i] );
			usb_free_urb( pdx->PixelUrb[k][i] );
			pdx->pendedPixelUrbs[k][i] = 0;
		}
		dbg( "Urb error count = %d", errCnt );
		errCnt = 0;
		dbg( "Urbs free'd and Killed for Frame %d", k );
	}

	for( k = 0; k < pdx->num_frames; k++ ) {
		if( pdx->iama == PIXIS_PID ) { //if so, which EP should we map this frame to
			if( k % 2 )//check to see if this should use EP4(PONG)
				epAddr = pdx->hEP[3];//PONG, odd frames
			else
				epAddr = pdx->hEP[2];//PING, even frames and zero
		} else //ST133 only has 1 endpoint for Pixel data transfer
			epAddr = pdx->hEP[0];

		//dma_unmap_sg( pdx->udev->bus->controller, pdx->sgl[k], pdx->maplist_numPagesMapped[k], DMA_FROM_DEVICE);
		for( i = 0; i < pdx->maplist_numPagesMapped[k]; i++ )
			page_cache_release( sg_page(&(pdx->sgl[k][i])) );
		kfree( pdx->sgl[k] );
		kfree( pdx->PixelUrb[k] );
		kfree( pdx->pendedPixelUrbs[k] );
		pdx->sgl[k] = NULL;
		pdx->PixelUrb[k] = NULL;
		pdx->pendedPixelUrbs[k] = NULL;
	}
	kfree( pdx->sgEntries );
	vfree( pdx->maplist_numPagesMapped );
	pdx->sgEntries = NULL;
	pdx->maplist_numPagesMapped = NULL;
	kfree( pdx->sgl );
	kfree( pdx->pendedPixelUrbs );
	kfree( pdx->PixelUrb );
	pdx->sgl = NULL;
	pdx->pendedPixelUrbs = NULL;
	pdx->PixelUrb = NULL;
	return 0;
}
/* MapUserBuffer(
  inputs:
  ioctl_struct *io - structure containing user address, frame #, and size
  struct device_extension *pdx - the PIUSB device extension
  returns:
  int - status of the task
  Notes:
  MapUserBuffer maps a buffer passed down through an ioctl.  The user buffer is Page Aligned by the app
  and then passed down.  The function get_free_pages(...) does the actual mapping of the buffer from user space to 
  kernel space.  From there a scatterlist is created from all the pages.  The next function called is to usb_buffer_map_sg
  which allocated DMA addresses for each page, even coalescing them if possible.  The DMA address is placed in the scatterlist
  structure.  The function returns the number of DMA addresses.  This may or may not be equal to the number of pages that 
  the user buffer uses.  We then build an URB for each DMA address and then submit them.
*/
//int MapUserBuffer( unsigned long uaddr, unsigned long numbytes, unsigned long frameInfo, struct device_extension *pdx )
static int MapUserBuffer(ioctl_struct *io, struct device_extension *pdx )
{
	unsigned long uaddr;
	unsigned long numbytes;
	int frameInfo; //which frame we're mapping
	unsigned int epAddr = 0;
	int i = 0;
	int k = 0;
	int err = 0;
	int ret;
	struct page **maplist_p;
	int num_pages;
	frameInfo = io->numFrames;
	uaddr = (unsigned long) io->pData;
	numbytes = io->numbytes;

	if (!pdx->PixelUrb)
		return -EINVAL; // not initialized yet
 
	if( pdx->iama == PIXIS_PID ) { //if so, which EP should we map this frame to
		if( frameInfo % 2 )//check to see if this should use EP4(PONG)
			epAddr = pdx->hEP[3];//PONG, odd frames
		else
			epAddr = pdx->hEP[2];//PING, even frames and zero
		dbg("Pixis Frame #%d: EP=%d",frameInfo, (epAddr==pdx->hEP[2]) ? 2 : 4 );
	} else { //ST133 only has 1 endpoint for Pixel data transfer
		epAddr = pdx->hEP[0];
		dbg("ST133 Frame #%d: EP=2",frameInfo );
	}
	dbg("UserAddress = 0x%08lX", uaddr );
	dbg("numbytes = %d", (int)numbytes );
	//number of pages to map the entire user space DMA buffer
	num_pages = ((uaddr & ~PAGE_MASK) + numbytes + ~PAGE_MASK) >> PAGE_SHIFT;
	dbg("Number of pages needed = %d", num_pages );
	maplist_p = vmalloc( num_pages * sizeof(struct page*));
	if (!maplist_p) {
		dbg( "Can't Allocate Memory for maplist_p" );
		return -ENOMEM;
	}
	// Note: this is similar to videobuf2-dma-contig.c vb2_dc_get_userptr()
	//map the user buffer to kernel memory
	down_write( &current->mm->mmap_sem );
	ret = get_user_pages(current, current->mm, (uaddr & PAGE_MASK),
						 num_pages, WRITE, 0, //Don't Force
						 maplist_p, NULL);
	up_write(&current->mm->mmap_sem );
	if (num_pages != ret) {
		dbg( "get_user_pages() failed with %d", ret);
		vfree( maplist_p );
		// TODO: put_page
		return -ENOMEM;
	}
	pdx->maplist_numPagesMapped[frameInfo] = num_pages;
	dbg( "Number of pages mapped = %d", pdx->maplist_numPagesMapped[frameInfo] );
	for( i=0; i < num_pages; i++ )
		flush_dcache_page(maplist_p[i]);
	//need to create a scatterlist that spans each frame that can fit into the mapped buffer
	pdx->sgl[frameInfo] = kmalloc( ( num_pages * sizeof( struct scatterlist ) ), GFP_ATOMIC );
	if (!pdx->sgl[frameInfo]) {
		vfree( maplist_p );
		dbg("can't allocate mem for sgl");
		return -ENOMEM;
	}

	sg_init_table( pdx->sgl[frameInfo], num_pages );
	sg_assign_page(&(pdx->sgl[frameInfo][0]), maplist_p[0]);
	pdx->sgl[frameInfo][0].offset = uaddr & ~PAGE_MASK;
	if (num_pages > 1) {
		unsigned long count = numbytes;
		pdx->sgl[frameInfo][0].length = PAGE_SIZE - pdx->sgl[frameInfo][0].offset;
		count -= pdx->sgl[frameInfo][0].length;
		for (k=1; k < num_pages ; k++) {
			sg_set_page(&(pdx->sgl[frameInfo][k]), maplist_p[k],
				    min(count, PAGE_SIZE), 0);
			count -= pdx->sgl[frameInfo][k].length;
		}
	} else {
		pdx->sgl[frameInfo][0].length = numbytes;
	}
	/*
	if (!pdx->udev->bus->controller->dma_mask){
	vfree(maplist_p);
	pr_info( "usb controller doesn't support DMA" );
	return -EINVAL;
	}
	dbg("DMA mask = %p", pdx->udev->bus->controller->dma_mask);
	ret = dma_map_sg(pdx->udev->bus->controller, pdx->sgl[frameInfo],
			 num_pages, DMA_FROM_DEVICE);
	if (ret == 0) {
		vfree(maplist_p);
		pr_info( "dma_map_sg failed" );
		return -EINVAL;
	}

	pdx->sgEntries[frameInfo] = ret;
	dbg("Number of sgEntries = %d", pdx->sgEntries[frameInfo] );
	pdx->userBufMapped = 1;
	vfree( maplist_p );

	// This looks awfully like usb_sg_init()! => use it? (but then there is no callback, just usb_sg_wait in READPIPE, which is different behaviour)
	//Create and Send the URB's for each s/g entry
	pdx->PixelUrb[frameInfo] = kmalloc( pdx->sgEntries[frameInfo] * sizeof( struct urb *), GFP_KERNEL);
	if( !pdx->PixelUrb[frameInfo] )
	{
		dbg( "Can't Allocate Memory for Urb" );
		return -ENOMEM;
	}
	for( i = 0; i < pdx->sgEntries[frameInfo]; i++ )
	{
		pdx->PixelUrb[frameInfo][i] = usb_alloc_urb( 0, GFP_KERNEL );//0 because we're using BULK transfers
		usb_fill_bulk_urb( pdx->PixelUrb[frameInfo][i],
					pdx->udev,
					epAddr,
					(void*) (unsigned long) sg_dma_address( &pdx->sgl[frameInfo][i] ),
					sg_dma_len( &pdx->sgl[frameInfo][i] ),
					piusb_read_pixel_callback,
					(void *)pdx );
		pdx->PixelUrb[frameInfo][i]->transfer_dma = sg_dma_address( &pdx->sgl[frameInfo][i] );
		pdx->PixelUrb[frameInfo][i]->transfer_flags = URB_NO_TRANSFER_DMA_MAP; // | URB_NO_INTERRUPT;
	}
	// TODO: check whether it should send an interrupt only for the last URB or
	// not. It seems the callback expects to be called after every URB (increases
	// the size of the data received).
	//pdx->PixelUrb[frameInfo][i-1]->transfer_flags &= ~URB_NO_INTERRUPT;  //only interrupt when last URB completes
	pdx->pendedPixelUrbs[frameInfo] = kmalloc( ( pdx->sgEntries[frameInfo] * sizeof( char ) ), GFP_KERNEL );
	if( !pdx->pendedPixelUrbs[frameInfo] ) {
		dbg( "Can't allocate Memory for pendedPixelUrbs" );
		return -ENOMEM;
	}
	for( i = 0; i < pdx->sgEntries[frameInfo]; i++ )
	{
		//err = usb_submit_urb( pdx->PixelUrb[frameInfo][i], GFP_ATOMIC );
		err = usb_submit_urb( pdx->PixelUrb[frameInfo][i], GFP_KERNEL );
		if( err )
		{
			dbg( "submit urb for entry %d error = %d\n", i, err);
			pdx->pendedPixelUrbs[frameInfo][i] = 0;
			return err;
		}
		else
			pdx->pendedPixelUrbs[frameInfo][i] = 1;
	}
	*/
	// all the above can be simplified by letting the usb do it, passing it the sgl
	pdx->sgEntries[frameInfo] = 1;
	pdx->PixelUrb[frameInfo] = kmalloc( sizeof( struct urb *), GFP_KERNEL);
	if( !pdx->PixelUrb[frameInfo] ) {
		dbg( "Can't Allocate Memory for Urb" );
		return -ENOMEM;
	}
	pdx->pendedPixelUrbs[frameInfo] = kmalloc( sizeof( char ) , GFP_KERNEL );
	if( !pdx->pendedPixelUrbs[frameInfo] ) {
		dbg( "Can't allocate Memory for pendedPixelUrbs" );
		return -ENOMEM;
	}
	pdx->PixelUrb[frameInfo][0] = usb_alloc_urb( 0, GFP_KERNEL );
	usb_fill_bulk_urb(pdx->PixelUrb[frameInfo][0], pdx->udev, epAddr, NULL,
			  numbytes, piusb_read_pixel_callback, pdx);
	pdx->PixelUrb[frameInfo][0]->num_sgs = num_pages;
	pdx->PixelUrb[frameInfo][0]->sg = pdx->sgl[frameInfo];
	err = usb_submit_urb( pdx->PixelUrb[frameInfo][0], GFP_KERNEL );
	if( err ) {
		dbg( "submit urb for entry %d error = %d\n", 0, err);
		pdx->pendedPixelUrbs[frameInfo][0] = 0;
		return err;
	} else
			pdx->pendedPixelUrbs[frameInfo][0] = 1;
	return 0;
}

static int get_pixel_data(struct device_extension *pdx)
{
	int i;
	unsigned long numbytes;

	if (!pdx->gotPixelData)
		return 0;

	pdx->gotPixelData = 0;
	numbytes = pdx->bulk_in_size_returned;
	pdx->bulk_in_size_returned -= pdx->frameSize;

	for (i = 0; i < pdx->maplist_numPagesMapped[pdx->active_frame]; i++)
		SetPageDirty(sg_page(&pdx->sgl[pdx->active_frame][i]));

	pdx->active_frame = (pdx->active_frame + 1) % pdx->num_frames;

	return numbytes;
}
#else
static void piusb_read_pixel_callback ( struct urb *urb )
{
	struct device_extension *pdx = urb->context;
	int status = urb->status;

	if (status &&
	    // for these 3 errors -> we might have still received something
	    !(status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)) {
		dbg("%s - nonzero read bulk status received: %d", __func__, urb->status);
		dbg( "Error in read EP2 callback" );
		dbg( "FrameIndex = %d", pdx->frameIdx );
		dbg( "Bytes received before problem occurred = %d", pdx->bulk_in_byte_trk );
		dbg( "Urb Idx = %d", pdx->urbIdx );
		pdx->pendedPixelUrbs[pdx->frameIdx][pdx->urbIdx] = 0;
		pdx->gotPixelData = -EPIPE; // tell there is no hope
		return;
	}

	pdx->bulk_in_byte_trk += urb->actual_length;

	pdx->urbIdx++;  //point to next URB when we callback
	if( pdx->bulk_in_byte_trk >= pdx->frameSize ) {
		pdx->bulk_in_size_returned = pdx->bulk_in_byte_trk;
		pdx->bulk_in_byte_trk = 0;
		pdx->gotPixelData = 1;
		pdx->frameIdx = ( ( pdx->frameIdx + 1 ) % pdx->num_frames );
		pdx->urbIdx = 0;
	}

	// Without DMA mapping, it's not possible to resubmit the URB here, because
	// the data hasn't been copied yet to the user. => we'll do it in get_pixel_data()
}

/**
 * it's called UnMapUserBuffer, but actually this implementation just frees the
 * kernel buffer.
 */
static int UnMapUserBuffer( struct device_extension *pdx )
{
	int i = 0;
	int k = 0;
	unsigned int epAddr;

	if (!pdx->PixelUrb)
		return -EINVAL; // not initialized yet

	for( k = 0; k < pdx->num_frames; k++ ) {
		for (i = 0; i < pdx->sgEntries[k]; i++) {
//			dbg("Killing Urbs %d for Frame %d", i, k );
			usb_kill_urb( pdx->PixelUrb[k][i] );
			usb_free_coherent(pdx->udev, pdx->PixelUrb[k][i]->transfer_buffer_length,
					pdx->PixelUrb[k][i]->transfer_buffer, pdx->PixelUrb[k][i]->transfer_dma);
			usb_free_urb( pdx->PixelUrb[k][i] );
			pdx->pendedPixelUrbs[k][i] = 0;
		}
		dbg( "Urb error count = %d", errCnt );
		errCnt = 0;
		dbg( "Urbs free'd and Killed for Frame %d", k );
	}

	for( k = 0; k < pdx->num_frames; k++ ) {
		if( pdx->iama == PIXIS_PID ) { //if so, which EP should we map this frame to
			if( k % 2 )//check to see if this should use EP4(PONG)
				epAddr = pdx->hEP[3];//PONG, odd frames
			else
				epAddr = pdx->hEP[2];//PING, even frames and zero
		} else //ST133 only has 1 endpoint for Pixel data transfer
			epAddr = pdx->hEP[0];
		kfree( pdx->PixelUrb[k] );
		kfree( pdx->pendedPixelUrbs[k] );
		pdx->PixelUrb[k] = NULL;
		pdx->pendedPixelUrbs[k] = NULL;
	}
	kfree(pdx->user_buffer);
	pdx->user_buffer = NULL;

	kfree( pdx->sgEntries );
	vfree( pdx->maplist_numPagesMapped );
	pdx->sgEntries = NULL;
	pdx->maplist_numPagesMapped = NULL;
	kfree( pdx->pendedPixelUrbs );
	kfree( pdx->PixelUrb );
	pdx->pendedPixelUrbs = NULL;
	pdx->PixelUrb = NULL;
	return 0;
}

/*
 * Maximum size for each allocation block, if it's too big, it might have some
 * failure being allocated. So we use 100 Kb. 1Mb seemed to work fine too, but at
 * least we are sure to test multiple URBs.
 */
#define MAX_BUFFER_SIZE (102400)
/**
 * Actually doesn't map the user buffer to DMA, but just write down the address,
 * and allocates kernel memory of the same size to receive the camera data. It
 * also starts requesting data from the camera by setting up URBs.
 */
static int MapUserBuffer(ioctl_struct *io, struct device_extension *pdx )
{
	unsigned long numbytes = io->numbytes; // length of the buffer
	int f = io->numFrames; // which frame we're mapping
	unsigned int epAddr;
	int i;
	int retval = 0;
	struct urb *urb = NULL;
	void *buf = NULL;
	unsigned int buf_size, size_last;
	int numurb;

	if (!pdx->PixelUrb)
		return -EINVAL; // not initialized yet

	pdx->user_buffer[f] = io->pData; // address of the user buffer, to copy it back

	if( pdx->iama == PIXIS_PID ) { //if so, which EP should we map this frame to
		if( f % 2 )//check to see if this should use EP4(PONG)
			epAddr = pdx->hEP[3];//PONG, odd frames
		else
			epAddr = pdx->hEP[2];//PING, even frames and zero
		dbg("Pixis Frame #%d: EP=%d",f, (epAddr==pdx->hEP[2]) ? 2 : 4 );
	} else { //ST133 only has 1 endpoint for Pixel data transfer
		epAddr = pdx->hEP[0];
		dbg("ST133 Frame #%d: EP=2",f );
	}
	dbg("UserAddress = %p", io->pData );

	buf_size = min((int)numbytes, MAX_BUFFER_SIZE);
	numurb = numbytes / buf_size;
	size_last = numbytes % buf_size;
	if (size_last)
		numurb++;
	dbg("numbytes = %lu => %d urbs of %d bytes", numbytes, numurb, buf_size);
	pdx->sgEntries[f] = numurb;

	pdx->PixelUrb[f] = kzalloc(numurb * sizeof(struct urb *), GFP_KERNEL);
	if (!pdx->PixelUrb[f]) {
		dbg( "Can't Allocate Memory for Urb" );
		return -ENOMEM;
	}

	pdx->pendedPixelUrbs[f] = kzalloc(numurb * sizeof(char), GFP_KERNEL);
	if (!pdx->pendedPixelUrbs[f]) {
		dbg( "Can't allocate Memory for pendedPixelUrbs" );
		retval = -ENOMEM;
		goto error;
	}

	for (i = 0; i < numurb; i++) {
		int size = buf_size;
		if (size_last && (i == (numurb-1)))
			size = size_last;

		urb = usb_alloc_urb( 0, GFP_KERNEL );
		if (!urb) {
			retval = -ENOMEM;
			goto error;
		}
		pdx->PixelUrb[f][i] = urb;

		buf = usb_alloc_coherent(pdx->udev, size, GFP_KERNEL, &urb->transfer_dma);
		if (!buf) {
			retval = -ENOMEM;
			goto error;
		}

		usb_fill_bulk_urb(urb, pdx->udev, epAddr, buf, size,
				  piusb_read_pixel_callback, (void *)pdx);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
	}

	for (i = 0; i < numurb; i++) {
		retval = usb_submit_urb( pdx->PixelUrb[f][i], GFP_KERNEL );
		if (retval) {
			dbg( "submit urb for entry %d error = %d", i, retval);
			pdx->pendedPixelUrbs[f][i] = 0;
			goto error_kill;
		}

		pdx->pendedPixelUrbs[f][i] = 1;
	}
	return 0;

error_kill:
	for (i = 0; i < numurb; i++) {
		if (pdx->pendedPixelUrbs[f][i])
			usb_kill_urb(pdx->PixelUrb[f][i]);
	}
error:
	for (i = 0; i < numurb; i++) {
		urb = pdx->PixelUrb[f][i];
		if (urb)
			usb_free_coherent(pdx->udev, urb->transfer_buffer_length,
					urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);

	}
	kfree(pdx->pendedPixelUrbs[f]);
	pdx->pendedPixelUrbs[f] = NULL;
	kfree(pdx->PixelUrb[f]);
	pdx->PixelUrb[f] = NULL;
	return retval;
}

static int get_pixel_data(struct device_extension *pdx)
{
	struct urb **urbs = pdx->PixelUrb[pdx->active_frame];
	unsigned char *to_buf = pdx->user_buffer[pdx->active_frame];
	unsigned long numbytes;
	int i, err;

	if (!pdx->gotPixelData)
		return 0; /* not yet */
	else if (pdx->gotPixelData < 0) {
		err = pdx->gotPixelData;
		pdx->gotPixelData = 0;
		// We should return the error number, but it seems the libpvcam
		// thinks it's just a negative length to read. So instead claim
		// we got all
		//return err; /* error */
		numbytes = pdx->frameSize;
		dbg("pretending to return %lu bytes of data after err %d", numbytes, err);
		return numbytes;
	}

	pdx->gotPixelData = 0;
	numbytes = pdx->bulk_in_size_returned;
	pdx->bulk_in_size_returned -= pdx->frameSize;

	for (i=0; i<pdx->sgEntries[pdx->active_frame]; i++) {
		u16 *buf = (urbs[i]->transfer_buffer);
		unsigned int length = urbs[i]->actual_length;

		if (!access_ok(VERIFY_WRITE, (void __user *)to_buf, length))
			return -EFAULT;

		dbg("Got pixel data of urb %d = %x", i, buf[length/(i+1)]);
		if (copy_to_user(to_buf, buf, length))
			dbg("failed to copy pixel data of urb %d to user", i);
		to_buf += length;

		/* try to resubmitting the urb (will fail if buffer is unmapped) */
		err = usb_submit_urb(urbs[i], GFP_KERNEL);
		if (err && err != -EPERM) {
			errCnt++;
			if(err != lastErr) {
				dbg("submit urb failed with error code %d", -err);
				lastErr = err;
			}
		} else if (err == -EPERM)
			dbg("submit urb cancelled");
	}

	pdx->active_frame = (pdx->active_frame + 1) % pdx->num_frames;
	dbg("return %lu bytes of data", numbytes);
	return numbytes;
}
#endif

static int piusb_read_io(ioctl_struct *ctrl, struct device_extension *pdx,
		ioctl_struct *arg)
{
	unsigned char *uBuf;
	int numbytes;
	int ret;

	uBuf = kmalloc(ctrl->numbytes, GFP_KERNEL);
	if (!uBuf) {
		dbg("Alloc for uBuf failed");
		return -ENOMEM;
	}
	numbytes = (int) ctrl->numbytes;
	dbg("numbytes to read = %d", numbytes);

	// FIXME: why reading this data? is it sent? left-over from piusb_write_bulk()?
	if (copy_from_user(uBuf, ctrl->pData, numbytes)) {
		dbg("copying ctrl->pData to uBuf failed");
		kfree(uBuf);
		return -EFAULT;
	}
	ret = usb_bulk_msg(pdx->udev, pdx->hEP[ctrl->endpoint],
					   uBuf, numbytes, &numbytes, HZ * 10);
	if (ret) {
		// FIXME: that's pretty strange message, knowing that uBuf has not been sent??
		dbg("CMD = %s, Address = 0x%02X",
			((uBuf[3] == 0x02) ? "WRITE" : "READ"),
			uBuf[1]);
		dbg("Number of bytes Attempted to read = %d", numbytes);
		dbg("Blocking ReadI/O Failed with status %d", ret);
		kfree(uBuf);
		return ret;
	}
	dbg("EP Read %d bytes", numbytes);

	memcpy(ctrl->pData, uBuf, numbytes);
	dbg("Total Bytes Read from EP[%d] = %d", ctrl->endpoint, numbytes);
	ctrl->numbytes = numbytes;

	if (copy_to_user(arg, ctrl, sizeof(ioctl_struct))) {
		dbg("copy_to_user failed in IORB");
		kfree(uBuf);
		return -EFAULT;
	}

	kfree(uBuf);
	return ctrl->numbytes;
}


// TODO: add a piusb_compat_ioctl to support 32 bits calls on 64 bits. It's
// mainly about reading/writing the ioctl_struct in 32 bits.
static long piusb_ioctl (struct file *file, unsigned int cmd, unsigned long arg)
{
	struct device_extension *pdx;
	char dummyCtlBuf[] = {0,0,0,0,0,0,0,0};
	u16 devRB=0;
	int err = 0;
	long retval = 0;
	ioctl_struct ctrl;
	unsigned short controlData;

	pdx = (struct device_extension *)file->private_data;
	mutex_lock(&pdx->mutex);
	/* verify that the device wasn't unplugged */
	if (!pdx->present) {
		dbg( "No Device Present\n" );
		retval = -ENODEV;
		if (cmd == PIUSB_READPIPE)
			retval = 0; // libpvcam will crash if we report an error
		goto done;
	}

	/* check the ioctl struct can be read/written */
	if(_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if( err ) {
		dev_err(&pdx->udev->dev, "fail to access ioctl data. error = %d\n", err);
		retval = -EFAULT;
		goto done;
	}

	switch (cmd) {
	case PIUSB_GETVNDCMD:
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		dbg("Get Vendor Command = %x, pData = %p", ctrl.cmd, ctrl.pData);
		if (ctrl.numbytes != sizeof(devRB)) {
			dev_err(&pdx->udev->dev, "GETVNDCMD numbytes should be 2, but is %lu\n", ctrl.numbytes);
			retval = -EINVAL;
			goto done;
		}
		retval = usb_control_msg(pdx->udev, usb_rcvctrlpipe(pdx->udev, 0),
					ctrl.cmd, USB_DIR_IN, 0, 0, &devRB, ctrl.numbytes, HZ*10);
		if (ctrl.cmd == 0xF1)
			dbg( "FW Version returned from HW = %d.%d", (devRB>>8), (devRB&0xFF) );
		// FIXME: the user-space lib doesn't seem much happy with the value
		// returned for the FW version (states it's unsupported). Maybe it's
		// a sign that it's not behaving well. Should it return 0 and copy
		// the return value in ctrl.pData (it seems it's a valid pointer)?
		retval = devRB;
		break;

	case PIUSB_SETVNDCMD:
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		controlData = (ctrl.pData[1] << 8) | ctrl.pData[0];
		dbg( "Set Vendor Command = %x -> %d", ctrl.cmd, controlData);

		// TODO: not clear whether ctrl.numbytes is supposed to be the size of
		// ctrl.pData or the amount of extra (null) data to send. My guess
		// is that it's related to ctrl.pData (and it's possible to send no
		// data at all, ie, *data=NULL) but for safety, keep sending null data.
		if (ctrl.numbytes > ARRAY_SIZE(dummyCtlBuf)) {
			dev_err(&pdx->udev->dev, "SETVNDCMD numbytes bigger than possible: %lu\n", ctrl.numbytes);
			retval = -EINVAL;
			goto done;
		}

		retval = usb_control_msg(pdx->udev, usb_sndctrlpipe(pdx->udev, 0),
						ctrl.cmd,
						(USB_DIR_OUT | USB_TYPE_VENDOR ),/* | USB_RECIP_ENDPOINT), */
						controlData, 0, dummyCtlBuf, ctrl.numbytes, HZ*10);
		dbg( "control msg returned %ld", retval);
		break;

	case PIUSB_ISHIGHSPEED:
		retval = (pdx->udev->speed == USB_SPEED_HIGH) ? 1 : 0;
		break;

	case PIUSB_WRITEPIPE:
		dbg("WRITEPIPE");
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		if( !access_ok(VERIFY_READ, ctrl.pData, ctrl.numbytes)) {
			dbg("can't access pData" );
			retval = -EFAULT;
			goto done;
		}
		// TODO: shall we care about pendingWrite?
		retval = piusb_write_bulk(&ctrl, ctrl.pData, ctrl.numbytes, pdx);
		break;

	case PIUSB_USERBUFFER:
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		retval = MapUserBuffer(&ctrl, pdx);
		break;

	case PIUSB_UNMAP_USERBUFFER:
		dbg("unmapping buffer");
		retval = UnMapUserBuffer(pdx);
		break;

	case PIUSB_READPIPE:
		/* Called to receive data from the camera */
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		dbg("READPIPE %d", ctrl.endpoint);

		/* Depending on the camera, endpoints have different meanings */
		if (pdx->iama == PIXIS_PID) {
			switch(ctrl.endpoint) {
			case 0: // PIXIS IO EP0
			case 4: // PIXIS IO EP4
				retval = piusb_read_io(&ctrl, pdx, (ioctl_struct *)arg);
				break;
			case 2://PIXIS Ping
			case 3://PIXIS Pong
				retval = get_pixel_data(pdx);
				break;
			default:
				retval = -EINVAL;
				break;
			}
		} else { /* ST133 */
			switch(ctrl.endpoint) {
			case 0://ST133 Pixel Data
				retval = get_pixel_data(pdx);
				break;
			case 1://ST133 IO
				retval = piusb_read_io(&ctrl, pdx, (ioctl_struct *)arg);
				break;
			default:
				retval = -EINVAL;
				break;
			}
		}
		break;

	case PIUSB_WHATCAMERA:
		retval = pdx->iama;
		break;

	case PIUSB_SETFRAMESIZE:
		if (copy_from_user(&ctrl, (void __user*)arg, sizeof(ioctl_struct))) {
			pr_info("copy_from_user failed\n");
			retval = -EFAULT;
			goto done;
		}
		/* don't allow to change it after it has already been allocated */
		if (pdx->PixelUrb) {
			dev_err(&pdx->udev->dev, "SETFRAMESIZE called while buffer is still mapped\n");
			retval = -EINVAL;
			goto done;
		}
		dbg("SETFRAMESIZE to %dx%lu", ctrl.numFrames, ctrl.numbytes);

		if ((pdx->iama == PIXIS_PID) && (ctrl.numFrames % 2)) {
			/*
			 * The PIXIS uses a ping-pong scheme, which means we
			 * need to have a even number of buffer (or we would
			 * need to change the endpoint number every time we
			 * resubmit the URBs).
			 */
			// TODO: allow odd number, and update pipe when resubmitting URB
			// might need to look out for Set Vendor Command = f0.
			dev_warn(&pdx->udev->dev, "PIXIS needs an even number of "
				 "frame buffers, it will not work past %d frames\n",
				 ctrl.numFrames);
		}

		pdx->frameSize = ctrl.numbytes;
		pdx->num_frames = ctrl.numFrames;
		pdx->active_frame = 0;

		/* the checks shouldn't be necessary, but it makes sure there is no leak */
		if( !pdx->sgl )
			pdx->sgl = kmalloc( sizeof ( struct scatterlist *) * pdx->num_frames, GFP_KERNEL );
		if( !pdx->sgEntries )
			pdx->sgEntries = kmalloc( sizeof( unsigned int ) * pdx->num_frames, GFP_KERNEL );
		if( !pdx->PixelUrb )
			pdx->PixelUrb = kmalloc( sizeof( struct urb **) * pdx->num_frames, GFP_KERNEL );
		if( !pdx->maplist_numPagesMapped )
			pdx->maplist_numPagesMapped = vmalloc( sizeof( unsigned int ) * pdx->num_frames );
		if( !pdx->pendedPixelUrbs )
			pdx->pendedPixelUrbs = kmalloc( sizeof( char *) * pdx->num_frames, GFP_KERNEL );
		if( !pdx->user_buffer)
			pdx->user_buffer = kmalloc( sizeof(unsigned char *) * pdx->num_frames, GFP_KERNEL );
		break;

	default:
		/* return that we did not understand this ioctl call */
		dbg( "%s\n", "No IOCTL found" );
		retval = -ENOTTY;
		break;
	}

done:
	mutex_unlock(&pdx->mutex);
	return retval;
}

static void piusb_delete(struct kref *kref)
{
	struct device_extension *pdx = to_pi_dev(kref);

	dev_dbg(&pdx->udev->dev, "%s\n", __func__);
	usb_put_dev(pdx->udev);
	kfree(pdx);
}

static int piusb_open(struct inode *inode, struct file *file)
{
	struct device_extension *pdx = NULL;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	dbg( "Piusb_Open()" );
	subminor = iminor(inode);
	interface = usb_find_interface (&piusb_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d", __func__, subminor);
		retval = -ENODEV;
		goto exit_no_device;
	}

	pdx = usb_get_intfdata(interface);
	if (!pdx) {
		retval = -ENODEV;
		goto exit_no_device;
	}
	dbg( "Alternate Setting = %d", interface->num_altsetting );

	pdx->frameIdx = pdx->urbIdx = 0;
	pdx->gotPixelData = 0;
	pdx->pendingWrite = 0; // FIXME: never read
	pdx->frameSize = 0;
	pdx->num_frames = 0;
	pdx->active_frame = 0;
	pdx->bulk_in_byte_trk = 0;
	pdx->userBufMapped = 0; // FIXME: never read
	pdx->pendedPixelUrbs = NULL;
	pdx->sgEntries = NULL;
	pdx->sgl = NULL;
	pdx->maplist_numPagesMapped = NULL;
	pdx->PixelUrb = NULL;
	pdx->bulk_in_size_returned = 0;
	/* increment our usage count for the device */
	kref_get(&pdx->kref);
	/* save our object in the file's private structure */
	file->private_data = pdx;
exit_no_device:
	return retval;
}

/**
 *  piusb_release
 */
static int piusb_release(struct inode *inode, struct file *file)
{
	struct device_extension *pdx;
	int retval = 0;

	dbg( "Piusb_Release()" );
	pdx = (struct device_extension *)file->private_data;
	if (pdx == NULL) {
		dbg ("%s - object is NULL", __func__);
		return -ENODEV;
	}
  /* decrement the count on our device */
	kref_put(&pdx->kref, piusb_delete);
	return retval;
}

/*
 * File operations needed when we register this driver.
 * This assumes that this driver NEEDS file operations,
 * of course, which means that the driver is expected
 * to have a node in the /dev directory. This is for the
 * IOCTL interface.
 */
static struct file_operations piusb_fops = {
	/*
	 * The owner field is part of the module-locking
	 * mechanism. The idea is that the kernel knows
	 * which module to increment the use-counter of
	 * BEFORE it calls the device's open() function.
	 * This also means that the kernel can decrement
	 * the use-counter again before calling release()
	 * or should the open() function fail.
	 */
	.owner =	THIS_MODULE,
	.unlocked_ioctl = piusb_ioctl,
	.open =		piusb_open,
	.release =	piusb_release,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with devfs and the driver core
 */
static struct usb_class_driver piusb_class = {
	.name =		"usb/rspiusb%d",
	.fops =		&piusb_fops,
	.minor_base =	PIUSB_MINOR_BASE,
};

/* table of devices that work with this driver */
static struct usb_device_id pi_device_table [] = {
	{ USB_DEVICE( APA_VID, ST133_PID ) },
	{ USB_DEVICE( APA_VID, PIXIS_PID ) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE (usb, pi_device_table);

/**
 *  piusb_probe
 *
 *  Called by the usb core when a new device is connected that it thinks
 *  this driver might be interested in.
 */
static int piusb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct device_extension *pdx = NULL;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int i;
	int retval = -ENOMEM;

	dev_dbg(&interface->dev, "%s - Looking for PI USB Hardware", __func__);

	pdx = kzalloc( sizeof( struct device_extension ), GFP_KERNEL );
	if (pdx == NULL ) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error;
	}
	kref_init( &pdx->kref );
	mutex_init(&pdx->mutex);
	pdx->udev = usb_get_dev( interface_to_usbdev(interface));
	pdx->interface = interface;
	iface_desc = interface->cur_altsetting;

	/* See if the device offered us matches what we can accept */
	if ((pdx->udev->descriptor.idVendor != APA_VID) ||
		((pdx->udev->descriptor.idProduct != PIXIS_PID) &&
		 (pdx->udev->descriptor.idProduct != ST133_PID )))
		return -ENODEV;

	pdx->iama = pdx->udev->descriptor.idProduct;

	if( debug ) {
		if( pdx->udev->descriptor.idProduct == PIXIS_PID )
			dbg("Pixis Camera Found" );
		else
			dbg("ST133 USB Controller Found" );
		if( pdx->udev->speed  == USB_SPEED_HIGH )
			dbg("Highspeed(USB2.0) Device Attached" );
		else
			dbg("Lowspeed (USB1.1) Device Attached" );

		dbg( "NumEndpoints in Configuration: %d", iface_desc->desc.bNumEndpoints );
	}
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;
		if( debug ) {
			dbg( "Endpoint[%d]->bDescriptorType = %d", i, endpoint->bDescriptorType );
			dbg( "Endpoint[%d]->bEndpointAddress = 0x%02X", i, endpoint->bEndpointAddress );
			dbg( "Endpoint[%d]->bbmAttributes = %d", i, endpoint->bmAttributes );
			dbg( "Endpoint[%d]->MaxPacketSize = %d\n", i, endpoint->wMaxPacketSize );
		}
		if (usb_endpoint_xfer_bulk(endpoint)) {
			if(usb_endpoint_dir_in(endpoint))
				pdx->hEP[i] = usb_rcvbulkpipe( pdx->udev, endpoint->bEndpointAddress );
			else
				pdx->hEP[i] = usb_sndbulkpipe( pdx->udev, endpoint->bEndpointAddress );
		}
	}
	usb_set_intfdata( interface, pdx );
	retval = usb_register_dev( interface, &piusb_class );
	if (retval) {
		pr_err( "Not able to get a minor for this device." );
		usb_set_intfdata( interface, NULL );
		goto error;
	}
	pdx->present = 1;

	/* we can register the device now, as it is ready */
	pdx->minor = interface->minor;
	/* let the user know what node this device is now attached to */
	dbg ("PI USB2.0 device now attached to piusb-%d", pdx->minor);
	return 0;

error:
	if( pdx )
		kref_put( &pdx->kref, piusb_delete );
	return retval;
}

/**
 *  piusb_disconnect
 *
 *  Called by the usb core when the device is removed from the system.
 *
 *  This routine guarantees that the driver will not submit any more urbs
 *  by clearing pdx->udev.  It is also supposed to terminate any currently
 *  active urbs.  Unfortunately, usb_bulk_msg(), used in piusb_read(), does
 *  not provide any way to do this.  But at least we can cancel an active
 *  write.
 */
static void piusb_disconnect(struct usb_interface *interface)
{
	struct device_extension *pdx;
	int minor = interface->minor;

	pdx = usb_get_intfdata (interface);
	mutex_lock(&pdx->mutex);
	usb_set_intfdata (interface, NULL);
	/* give back our minor */
	usb_deregister_dev (interface, &piusb_class);
	/* prevent device read, write and ioctl */
	pdx->present = 0;
	mutex_unlock(&pdx->mutex);

	kref_put(&pdx->kref, piusb_delete);
	dbg("PI USB2.0 device #%d now disconnected\n", minor);
}

static struct usb_driver piusb_driver = {
	.name =			"rspiusb",
	.probe =		piusb_probe,
	.disconnect =		piusb_disconnect,
	.id_table =		pi_device_table,
};


static int __init piusb_init(void)
{
	int result;

	lastErr = 0;
	errCnt = 0;

	/* register this driver with the USB subsystem */
	result = usb_register(&piusb_driver);
	if (result)
		printk(KERN_ERR KBUILD_MODNAME
			": usb_register failed. Error number %d\n",
			result);
	else
		printk(KERN_INFO KBUILD_MODNAME ": %s %s\n", DRIVER_DESC, DRIVER_VERSION);
	return result;
}

static void __exit piusb_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&piusb_driver);
}

module_init(piusb_init);
module_exit(piusb_exit);

/* Module parameters */
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Log debug information");

MODULE_AUTHOR("Princeton Instruments");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");
