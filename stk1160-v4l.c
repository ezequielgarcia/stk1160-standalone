/*
 * STK1160 driver
 *
 * Copyright (C) 2012 Ezequiel Garcia
 * <elezegarcia--a.t--gmail.com>
 *
 * Based on Easycap driver by R.M. Thomas
 *	Copyright (C) 2010 R.M. Thomas
 *	<rmthomas--a.t--sciolus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-chip-ident.h>
#include <media/videobuf2-vmalloc.h>

#include <media/saa7115.h>

#include "stk1160.h"
#include "stk1160-reg.h"

static unsigned int vidioc_debug;
module_param(vidioc_debug, int, 0644);
MODULE_PARM_DESC(vidioc_debug, "enable debug messages [vidioc]");

static bool keep_buffers;
module_param(keep_buffers, bool, 0644);
MODULE_PARM_DESC(keep_buffers, "don't release buffers upon stop streaming");

/* supported video standards */
static struct stk1160_fmt format[] = {
	{
		.name     = "16 bpp YUY2, 4:2:2, packed",
		.fourcc   = V4L2_PIX_FMT_UYVY,
		.depth    = 16,
	}
};

static void stk1160_set_std(struct stk1160 *dev)
{
	int i;

	static struct regval std525[] = {

		/* 720x480 */

		/* Frame start */
		{STK116_CFSPO_STX_L, 0x0000},
		{STK116_CFSPO_STX_H, 0x0000},
		{STK116_CFSPO_STY_L, 0x0003},
		{STK116_CFSPO_STY_H, 0x0000},

		/* Frame end */
		{STK116_CFEPO_ENX_L, 0x05a0},
		{STK116_CFEPO_ENX_H, 0x0005},
		{STK116_CFEPO_ENY_L, 0x00f3},
		{STK116_CFEPO_ENY_H, 0x0000},

		{0xffff, 0xffff}
	};

	static struct regval std625[] = {

		/* 720x576 */

		/* TODO: Each line of frame has some junk at the end */
		/* Frame start */
		{STK116_CFSPO,   0x0000},
		{STK116_CFSPO+1, 0x0000},
		{STK116_CFSPO+2, 0x0001},
		{STK116_CFSPO+3, 0x0000},

		/* Frame end */
		{STK116_CFEPO,   0x05a0},
		{STK116_CFEPO+1, 0x0005},
		{STK116_CFEPO+2, 0x0121},
		{STK116_CFEPO+3, 0x0001},

		{0xffff, 0xffff}
	};

	if (dev->norm & V4L2_STD_525_60) {
		stk1160_dbg("registers to NTSC like standard\n");
		for (i = 0; std525[i].reg != 0xffff; i++)
			stk1160_write_reg(dev, std525[i].reg, std525[i].val);
	} else if (dev->norm & V4L2_STD_625_50) {
		stk1160_dbg("registers to PAL like standard\n");
		for (i = 0; std625[i].reg != 0xffff; i++)
			stk1160_write_reg(dev, std625[i].reg, std625[i].val);
	} else {
		BUG();
	}

}

/*
 * Set a new alternate setting.
 * Returns true is dev->max_pkt_size has changed, false otherwise.
 */
static bool stk1160_set_alternate(struct stk1160 *dev)
{
	int i, prev_alt = dev->alt;
	unsigned int min_pkt_size;
	bool new_pkt_size;

	/*
	 * If we don't set right alternate,
	 * then we will get a green screen with junk.
	 */
	min_pkt_size = STK1160_MIN_PKT_SIZE;

	for (i = 0; i < dev->num_alt; i++) {
		/* stop when the selected alt setting offers enough bandwidth */
		if (dev->alt_max_pkt_size[i] >= min_pkt_size) {
			dev->alt = i;
			break;
		/*
		 * otherwise make sure that we end up with the maximum bandwidth
		 * because the min_pkt_size equation might be wrong...
		 */
		} else if (dev->alt_max_pkt_size[i] >
			   dev->alt_max_pkt_size[dev->alt])
			dev->alt = i;
	}

	stk1160_info("setting alternate %d\n", dev->alt);

	if (dev->alt != prev_alt) {
		stk1160_dbg("minimum isoc packet size: %u (alt=%d)\n",
				min_pkt_size, dev->alt);
		stk1160_dbg("setting alt %d with wMaxPacketSize=%u\n",
			       dev->alt, dev->alt_max_pkt_size[dev->alt]);
		usb_set_interface(dev->udev, 0, dev->alt);
	}

	new_pkt_size = dev->max_pkt_size != dev->alt_max_pkt_size[dev->alt];
	dev->max_pkt_size = dev->alt_max_pkt_size[dev->alt];

	return new_pkt_size;
}

static bool stk1160_acquire_owner(struct stk1160 *dev, struct file *file)
{
	/* If there is an owner and it's not this filehandle */
	if (dev->fh_owner != NULL && dev->fh_owner != file)
		return false;

	/* We are the owner of this queue and queue operations */
	dev->fh_owner = file;

	return true;
}

static void stk1160_drop_owner(struct stk1160 *dev)
{
	dev->fh_owner = NULL;
}

static bool stk1160_is_owner(struct stk1160 *dev, struct file *file)
{
	return dev->fh_owner == file;
}

static int stk1160_start_streaming(struct stk1160 *dev)
{
	int i, rc;
	bool new_pkt_size;

	/* Check device presence */
	if (!dev->udev)
		return -ENODEV;

	/*
	 * For some reason it is mandatory to set alternate *first*
	 * and only *then* initialize isoc urbs.
	 * Someone please explain me why ;)
	 */
	new_pkt_size = stk1160_set_alternate(dev);

	/*
	 * We (re)allocate isoc urbs if:
	 * there is no allocated isoc urbs, OR
	 * a new dev->max_pkt_size is detected
	 */
	if (!dev->isoc_ctl.num_bufs || new_pkt_size) {
		rc = stk1160_alloc_isoc(dev);
		if (rc < 0)
			return rc;
	}

	/* submit urbs and enables IRQ */
	for (i = 0; i < dev->isoc_ctl.num_bufs; i++) {
		rc = usb_submit_urb(dev->isoc_ctl.urb[i], GFP_KERNEL);
		if (rc) {
			stk1160_err("cannot submit urb[%d] (%d)\n", i, rc);
			stk1160_uninit_isoc(dev);
			return rc;
		}
	}

	/* Start saa711x */
	v4l2_device_call_all(&dev->v4l2_dev, 0, video, s_stream, 1);

	/* Start stk1160 */
	stk1160_write_reg(dev, STK1160_DCTRL, 0xb3);
	stk1160_write_reg(dev, STK1160_DCTRL+3, 0x00);

	stk1160_dbg("streaming started\n");

	return 0;
}

int stk1160_stop_streaming(struct stk1160 *dev, bool connected)
{
	struct stk1160_buffer *buf;
	unsigned long flags = 0;

	stk1160_cancel_isoc(dev);

	/*
	 * It is possible to keep buffers around using a module parameter.
	 * This is intended to avoid memory fragmentation.
	 */
	if (!keep_buffers)
		stk1160_free_isoc(dev);

	/* If the device is physically plugged */
	if (connected && dev->udev) {

		/* set alternate 0 */
		dev->alt = 0;
		stk1160_info("setting alternate %d\n", dev->alt);
		usb_set_interface(dev->udev, 0, 0);

		/* Stop stk1160 */
		stk1160_write_reg(dev, STK1160_DCTRL, 0x00);
		stk1160_write_reg(dev, STK1160_DCTRL+3, 0x00);

		/* Stop saa711x */
		v4l2_device_call_all(&dev->v4l2_dev, 0, video, s_stream, 0);
	}

	/* Release all active buffers */
	spin_lock_irqsave(&dev->buf_lock, flags);
	while (!list_empty(&dev->avail_bufs)) {
		buf = list_first_entry(&dev->avail_bufs,
			struct stk1160_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
		stk1160_info("buffer [%p/%d] aborted\n",
				buf, buf->vb.v4l2_buf.index);
	}
	/* It's important to clear current buffer */
	dev->isoc_ctl.buf = NULL;
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	stk1160_dbg("streaming stopped\n");
	return 0;
}

/* fops */
static ssize_t stk1160_read(struct file *file,
	char __user *data, size_t count, loff_t *ppos)
{
	struct stk1160 *dev = video_drvdata(file);
	int rc;

	/*
	 * Read operation is emulated by videobuf2.
	 * When vb2 calls reqbufs it acquires ownership of queue.
	 * When the transfer is done, vb2 calls reqbufs with zero count,
	 * dropping ownership.
	 */
	rc = vb2_read(&dev->vb_vidq, data, count, ppos,
			file->f_flags & O_NONBLOCK);

	return rc;
}

static unsigned int
stk1160_poll(struct file *file, struct poll_table_struct *wait)
{
	struct stk1160 *dev = video_drvdata(file);
	int rc;

	rc = vb2_poll(&dev->vb_vidq, file, wait);

	return rc;
}

static int stk1160_close(struct file *file)
{
	struct stk1160 *dev = video_drvdata(file);

	/*
	 * If this is the owner handle we stop
	 * streaming to free/dequeue all buffers.
	 * Also, we drop ownership.
	 */
	if (stk1160_is_owner(dev, file)) {
		vb2_queue_release(&dev->vb_vidq);
		stk1160_drop_owner(dev);
	}

	return v4l2_fh_release(file);
}

static int stk1160_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct stk1160 *dev = video_drvdata(file);
	int rc;

	stk1160_dbg("vma=0x%08lx\n", (unsigned long)vma);

	/* TODO: Lock or trylock? */
	rc = vb2_mmap(&dev->vb_vidq, vma);

	stk1160_dbg("vma start=0x%08lx, size=%ld (%d)\n",
		(unsigned long)vma->vm_start,
		(unsigned long)vma->vm_end - (unsigned long)vma->vm_start,
		rc);
	return rc;
}

static struct v4l2_file_operations stk1160_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = stk1160_close,
	.read = stk1160_read,
	.poll = stk1160_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = stk1160_mmap,
};

/*
 * vb2 ioctls
 */
static int vidioc_reqbufs(struct file *file, void *priv,
			  struct v4l2_requestbuffers *p)
{
	struct stk1160 *dev = video_drvdata(file);
	int rc;

	if (!stk1160_acquire_owner(dev, file))
		return -EBUSY;

	rc = vb2_reqbufs(&dev->vb_vidq, p);

	/*
	 * If reqbufs has been called with count == 0
	 * it means the owner is releasing the queue,
	 * thus dropping ownership.
	 */
	if (p->count == 0)
		stk1160_drop_owner(dev);

	return rc;
}

static int vidioc_querybuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_is_owner(dev, file))
		return -EBUSY;

	return vb2_querybuf(&dev->vb_vidq, p);
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_is_owner(dev, file))
		return -EBUSY;

	return vb2_qbuf(&dev->vb_vidq, p);
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_is_owner(dev, file))
		return -EBUSY;

	return vb2_dqbuf(&dev->vb_vidq, p, file->f_flags & O_NONBLOCK);
}

static int vidioc_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_is_owner(dev, file))
		return -EBUSY;

	return vb2_streamon(&dev->vb_vidq, i);
}

static int vidioc_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_is_owner(dev, file))
		return -EBUSY;

	return vb2_streamoff(&dev->vb_vidq, i);
}

/*
 * vidioc ioctls
 */
static int vidioc_querycap(struct file *file,
		void *priv, struct v4l2_capability *cap)
{
	struct stk1160 *dev = video_drvdata(file);

	strcpy(cap->driver, "stk1160");
	strcpy(cap->card, "stk1160");
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	cap->capabilities =
		V4L2_CAP_VIDEO_CAPTURE |
		V4L2_CAP_STREAMING |
		V4L2_CAP_READWRITE;
	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void  *priv,
		struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	strlcpy(f->description, format[f->index].name, sizeof(f->description));
	f->pixelformat = format[f->index].fourcc;
	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct stk1160 *dev = video_drvdata(file);

	f->fmt.pix.width = dev->width;
	f->fmt.pix.height = dev->height;
	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
	f->fmt.pix.pixelformat = dev->fmt->fourcc;
	f->fmt.pix.bytesperline = dev->width * 2;
	f->fmt.pix.sizeimage = dev->height * f->fmt.pix.bytesperline;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
			struct v4l2_format *f)
{
	struct stk1160 *dev = video_drvdata(file);

	if (f->fmt.pix.pixelformat != format[0].fourcc) {
		stk1160_err("fourcc format 0x%08x invalid\n",
			f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	/*
	 * User can't choose size at his own will,
	 * so we just return him the current size chosen
	 * at standard selection.
	 * TODO: Implement frame scaling?
	 */

	f->fmt.pix.width = dev->width;
	f->fmt.pix.height = dev->height;
	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
	f->fmt.pix.bytesperline = dev->width * 2;
	f->fmt.pix.sizeimage = dev->height * f->fmt.pix.bytesperline;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct stk1160 *dev = video_drvdata(file);
	struct vb2_queue *q = &dev->vb_vidq;
	int rc;

	if (!stk1160_acquire_owner(dev, file))
		return -EBUSY;

	rc = vidioc_try_fmt_vid_cap(file, priv, f);
	if (rc < 0)
		return rc;

	if (vb2_is_streaming(q)) {
		stk1160_err("device busy\n");
		return -EBUSY;
	}

	return 0;
}

static int vidioc_querystd(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct stk1160 *dev = video_drvdata(file);
	v4l2_device_call_all(&dev->v4l2_dev, 0, video, querystd, norm);
	return 0;
}

static int vidioc_g_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct stk1160 *dev = video_drvdata(file);

	*norm = dev->norm;
	return 0;
}

static int vidioc_s_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct stk1160 *dev = video_drvdata(file);
	struct vb2_queue *q = &dev->vb_vidq;

	if (!stk1160_acquire_owner(dev, file))
		return -EBUSY;

	if (vb2_is_streaming(q)) {
		stk1160_err("device busy\n");
		return -EBUSY;
	}

	/* Check device presence */
	if (!dev->udev)
		return -ENODEV;

	/* This is taken from saa7115 video decoder */
	if (dev->norm & V4L2_STD_525_60) {
		dev->width = 720;
		dev->height = 480;
	} else if (dev->norm & V4L2_STD_625_50) {
		dev->width = 720;
		dev->height = 576;
	} else {
		stk1160_err("invalid standard\n");
		return -EINVAL;
	}

	/* We need to set this now, before we call stk1160_set_std */
	dev->norm = *norm;

	stk1160_set_std(dev);

	v4l2_device_call_all(&dev->v4l2_dev, 0, core, s_std,
			dev->norm);

	return 0;
}

/* FIXME: Extend support for other inputs */
static int vidioc_enum_input(struct file *file, void *priv,
				struct v4l2_input *i)
{
	struct stk1160 *dev = video_drvdata(file);

	if (i->index > STK1160_MAX_INPUT)
		return -EINVAL;

	sprintf(i->name, "Composite%d", i->index);
	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->std = dev->vdev.tvnorms;
	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct stk1160 *dev = video_drvdata(file);
	*i = dev->ctl_input;
	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	struct stk1160 *dev = video_drvdata(file);

	if (!stk1160_acquire_owner(dev, file))
		return -EBUSY;

	if (i > STK1160_MAX_INPUT)
		return -EINVAL;

	dev->ctl_input = i;

	stk1160_select_input(dev);

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				 struct v4l2_frmsizeenum *fsize)
{
	/* TODO: Is this needed? */
	return -EINVAL;
}

static int vidioc_enum_frameintervals(struct file *file, void *fh,
				  struct v4l2_frmivalenum *fival)
{
	/* TODO: Is this needed? */
	return -EINVAL;
}

static int vidioc_g_chip_ident(struct file *file, void *priv,
	       struct v4l2_dbg_chip_ident *chip)
{
	switch (chip->match.type) {
	case V4L2_CHIP_MATCH_HOST:
		chip->ident = V4L2_IDENT_NONE;
		chip->revision = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int vidioc_g_register(struct file *file, void *priv,
			     struct v4l2_dbg_register *reg)
{
	struct stk1160 *dev = video_drvdata(file);
	int rc;
	u8 val;

	switch (reg->match.type) {
	case V4L2_CHIP_MATCH_AC97:
		/* TODO: Support me please :-( */
		return -EINVAL;
	case V4L2_CHIP_MATCH_I2C_DRIVER:
		v4l2_device_call_all(&dev->v4l2_dev, 0, core, g_register, reg);
		return 0;
	case V4L2_CHIP_MATCH_I2C_ADDR:
		/* TODO: is this correct? */
		v4l2_device_call_all(&dev->v4l2_dev, 0, core, g_register, reg);
		return 0;
	default:
		if (!v4l2_chip_match_host(&reg->match))
			return -EINVAL;
	}

	/* Match host */
	rc = stk1160_read_reg(dev, reg->reg, &val);
	reg->val = val;
	reg->size = 1;

	return rc;
}

static int vidioc_s_register(struct file *file, void *priv,
			     struct v4l2_dbg_register *reg)
{
	struct stk1160 *dev = video_drvdata(file);

	switch (reg->match.type) {
	case V4L2_CHIP_MATCH_AC97:
		return -EINVAL;
	case V4L2_CHIP_MATCH_I2C_DRIVER:
		v4l2_device_call_all(&dev->v4l2_dev, 0, core, s_register, reg);
		return 0;
	case V4L2_CHIP_MATCH_I2C_ADDR:
		/* TODO: is this correct? */
		v4l2_device_call_all(&dev->v4l2_dev, 0, core, s_register, reg);
		return 0;
	default:
		if (!v4l2_chip_match_host(&reg->match))
			return -EINVAL;
	}

	/* Match host */
	return stk1160_write_reg(dev, reg->reg, cpu_to_le16(reg->val));
}
#endif

static const struct v4l2_ioctl_ops stk1160_ioctl_ops = {
	.vidioc_querycap      = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap  = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap     = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = vidioc_s_fmt_vid_cap,
	.vidioc_querystd      = vidioc_querystd,
	.vidioc_g_std         = vidioc_g_std,
	.vidioc_s_std         = vidioc_s_std,
	.vidioc_enum_input    = vidioc_enum_input,
	.vidioc_g_input       = vidioc_g_input,
	.vidioc_s_input       = vidioc_s_input,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = vidioc_enum_frameintervals,

	/* vb2 takes care of these */
	.vidioc_reqbufs       = vidioc_reqbufs,
	.vidioc_querybuf      = vidioc_querybuf,
	.vidioc_qbuf          = vidioc_qbuf,
	.vidioc_dqbuf         = vidioc_dqbuf,
	.vidioc_streamon      = vidioc_streamon,
	.vidioc_streamoff     = vidioc_streamoff,

	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_g_chip_ident = vidioc_g_chip_ident,

#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register = vidioc_g_register,
	.vidioc_s_register = vidioc_s_register,
#endif
};

/********************************************************************/

/*
 * Videobuf2 operations
 */
static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *v4l_fmt,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct stk1160 *dev = vb2_get_drv_priv(vq);
	unsigned long size;

	size = dev->width * dev->height * 2;

	/*
	 * Here we can change the number of buffers being requested.
	 * So, we set a minimum and a maximum like this:
	 */
	*nbuffers = clamp_t(unsigned int, *nbuffers,
			STK1160_MIN_VIDEO_BUFFERS, STK1160_MAX_VIDEO_BUFFERS);

	/* This means a packed colorformat */
	*nplanes = 1;

	sizes[0] = size;

	stk1160_info("%s: buffer count %d, each %ld bytes\n",
			__func__, *nbuffers, size);

	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	unsigned long flags;
	struct stk1160 *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct stk1160_buffer *buf =
		container_of(vb, struct stk1160_buffer, vb);

	spin_lock_irqsave(&dev->buf_lock, flags);
	if (!dev->udev) {
		/*
		 * If the device is disconnected return the buffer to userspace
		 * directly. The next QBUF call will fail with -ENODEV.
		 */
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	} else {

		buf->mem = vb2_plane_vaddr(vb, 0);
		buf->length = vb2_plane_size(vb, 0);
		buf->bytesused = 0;
		buf->pos = 0;

		/*
		 * If buffer length is less from expected then we return
		 * the buffer to userspace directly.
		 */
		if (buf->length < dev->width * dev->height * 2)
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
		else
			list_add_tail(&buf->list, &dev->avail_bufs);

	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct stk1160 *dev = vb2_get_drv_priv(vq);
	return stk1160_start_streaming(dev);
}

/* abort streaming and wait for last buffer */
static int stop_streaming(struct vb2_queue *vq)
{
	struct stk1160 *dev = vb2_get_drv_priv(vq);
	return stk1160_stop_streaming(dev, true);
}

static void stk1160_lock(struct vb2_queue *vq)
{
	struct stk1160 *dev = vb2_get_drv_priv(vq);
	mutex_lock(&dev->v4l_lock);
}

static void stk1160_unlock(struct vb2_queue *vq)
{
	struct stk1160 *dev = vb2_get_drv_priv(vq);
	mutex_unlock(&dev->v4l_lock);
}

static struct vb2_ops stk1160_video_qops = {
	.queue_setup		= queue_setup,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
	.wait_prepare		= stk1160_unlock,
	.wait_finish		= stk1160_lock,
};

static struct video_device v4l_template = {
	.name = "stk1160",
	.tvnorms = V4L2_STD_525_60 | V4L2_STD_625_50,
	.fops = &stk1160_fops,
	.ioctl_ops = &stk1160_ioctl_ops,
	.release = video_device_release_empty,
};

/********************************************************************/

int stk1160_vb2_setup(struct stk1160 *dev)
{
	int rc;
	struct vb2_queue *q;

	q = &dev->vb_vidq;
	memset(q, 0, sizeof(dev->vb_vidq));
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_READ | VB2_MMAP | VB2_USERPTR;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct stk1160_buffer);
	q->ops = &stk1160_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;

	rc = vb2_queue_init(q);
	if (rc < 0)
		return rc;

	/* initialize video dma queue */
	INIT_LIST_HEAD(&dev->avail_bufs);

	return 0;
}

int stk1160_video_register(struct stk1160 *dev)
{
	int rc;

	/* Initialize video_device with a template structure */
	dev->vdev = v4l_template;
	dev->vdev.debug = vidioc_debug;

	/*
	 * Provide a mutex to v4l2 core.
	 * In kernel 3.2 it will be used to protect *every* v4l2 ioctls.
	 */
	dev->vdev.lock = &dev->v4l_lock;

	/* This will be used to set video_device parent */
	dev->vdev.v4l2_dev = &dev->v4l2_dev;
	set_bit(V4L2_FL_USE_FH_PRIO, &dev->vdev.flags);

	/* NTSC is default */
	dev->norm = V4L2_STD_NTSC_M;
	dev->width = 720;
	dev->height = 480;

	/* set default format */
	dev->fmt = &format[0];
	stk1160_set_std(dev);

	stk1160_ac97_register(dev);

	v4l2_device_call_all(&dev->v4l2_dev, 0, core, s_std,
			dev->norm);

	video_set_drvdata(&dev->vdev, dev);
	rc = video_register_device(&dev->vdev, VFL_TYPE_GRABBER, -1);
	if (rc < 0) {
		stk1160_err("video_register_device failed (%d)\n", rc);
		return rc;
	}

	v4l2_info(&dev->v4l2_dev, "V4L2 device registered as %s\n",
		  video_device_node_name(&dev->vdev));

	return 0;
}
