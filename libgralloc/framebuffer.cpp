/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>

#include <dlfcn.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <linux/fb.h>
#include <linux/msm_mdp.h>

#include "gralloc_priv.h"
#include "gr.h"

#define MSMFB_UPDATE            _IOW(MSMFB_IOCTL_MAGIC, 3, void*)

/*****************************************************************************/

// numbers of buffers for page flipping
#define NUM_BUFFERS 2


enum {
    PAGE_FLIP = 0x00000001,
    LOCKED = 0x00000002
};

struct fb_context_t {
    framebuffer_device_t  device;
};

/*****************************************************************************/

static void
msm_copy_buffer(buffer_handle_t handle, int fd, int width, int height,
                int x, int y, int w, int h);

static int fb_setSwapInterval(struct framebuffer_device_t* dev,
            int interval)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;
    // FIXME: implement fb_setSwapInterval
    return 0;
}

static int fb_setUpdateRect(struct framebuffer_device_t* dev,
        int l, int t, int w, int h)
{
    if (((w|h) <= 0) || ((l|t)<0))
        return -EINVAL;
        
    fb_context_t* ctx = (fb_context_t*)dev;
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    m->info.reserved[0] = 0x54445055; // "UPDT";
    m->info.reserved[1] = (uint16_t)l | ((uint32_t)t << 16);
    m->info.reserved[2] = (uint16_t)(l+w) | ((uint32_t)(t+h) << 16);
    return 0;
}

static size_t last_offset=0;
static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
   // if (private_handle_t::validate(buffer) < 0)
   //     return -EINVAL;
    fb_context_t* ctx = (fb_context_t*)dev;

    //LOGE("FB_POST %d", (unsigned int)buffer);


    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(buffer);
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    
    if (hnd == 0 || (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        size_t offset = !last_offset;
	last_offset = offset;
	if(hnd)
	     offset = hnd->base - m->framebuffer->base;
        m->info.activate = FB_ACTIVATE_VBL;
        m->info.yoffset = offset;
	if(hnd) 
   	{
	    m->info.yoffset = (offset / m->finfo.line_length);//?1:0;
            if (ioctl(m->framebuffer->fd, FBIOPUT_VSCREENINFO, &m->info) == -1) {
               LOGE("FBIOPUT_VSCREENINFO failed");
	       if(buffer)
                  m->base.unlock(&m->base, buffer); 
               return -errno;
            }
     	}else{
		struct mdp_rect rect;
		rect.x = 0; rect.y = 0;
		rect.w = 480;
		rect.h = 800;
		ioctl(m->framebuffer->fd, MSMFB_UPDATE, &rect);
	}
	if(buffer)
            m->currentBuffer = buffer;
        
    } else {
        // If we can't do the page_flip, just copy the buffer to the front 
        // FIXME: use copybit HAL instead of memcpy

        LOGE("Used copybit fb post");
        
        void* fb_vaddr;
        void* buffer_vaddr;
        
        m->base.lock(&m->base, m->framebuffer, 
                GRALLOC_USAGE_SW_WRITE_RARELY, 
                0, 0, m->info.xres, m->info.yres,
                &fb_vaddr);

        m->base.lock(&m->base, buffer, 
                GRALLOC_USAGE_SW_READ_RARELY, 
                0, 0, m->info.xres, m->info.yres,
                &buffer_vaddr);

        //memcpy(fb_vaddr, buffer_vaddr, m->finfo.line_length * m->info.yres);

        msm_copy_buffer(m->framebuffer, m->framebuffer->fd,
                m->info.xres, m->info.yres,
                m->info.xoffset, m->info.yoffset,
                m->info.width, m->info.height);
        
        m->base.unlock(&m->base, buffer); 
        m->base.unlock(&m->base, m->framebuffer); 
    }
    
    return 0;
}

/*****************************************************************************/

int mapFrameBufferLocked(struct private_module_t* module, alloc_device_t *gralloc_device)
{
    // already initialized...
    if (module->framebuffer) {
        return 0;
    }
        
    char const * const device_template[] = {
            "/dev/graphics/fb%u",
            "/dev/fb%u",
            0 };

    int fd = -1;
    int i=0;
    char name[64];

    while ((fd==-1) && device_template[i]) {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);
        i++;
    }
    if (fd < 0)
        return -errno;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    struct fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_NOW;

    /*
     * Explicitly request 5/6/5
     */
    info.bits_per_pixel = 16;
    info.red.offset     = 11;
    info.red.length     = 5;
    info.green.offset   = 5;
    info.green.length   = 6;
    info.blue.offset    = 0;
    info.blue.length    = 5;
    info.transp.offset  = 0;
    info.transp.length  = 0;

    /*
     * Request NUM_BUFFERS screens (at lest 2 for page flipping)
     */
    info.yres_virtual = info.yres * NUM_BUFFERS;


    uint32_t flags = PAGE_FLIP;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1) {
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        LOGW("FBIOPUT_VSCREENINFO failed, page flipping not supported");
    }

    if (info.yres_virtual < info.yres * 2) {
        // we need at least 2 for page-flipping
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        LOGW("page flipping not supported (yres_virtual=%d, requested=%d)",
                info.yres_virtual, info.yres*2);
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    int refreshRate = 1000000000000000LLU /
    (
            uint64_t( info.upper_margin + info.lower_margin + info.yres )
            * ( info.left_margin  + info.right_margin + info.xres )
            * info.pixclock
    );

    if (refreshRate == 0) {
        // bleagh, bad info from the driver
        refreshRate = 60*1000;  // 60 Hz
    }

    if (int(info.width) <= 0 || int(info.height) <= 0) {
        // the driver doesn't return that information
        // default to 160 dpi
        info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
        info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
    }

    float xdpi = (info.xres * 25.4f) / info.width;
    float ydpi = (info.yres * 25.4f) / info.height;
    float fps  = refreshRate / 1000.0f;

    LOGI(   "using (fd=%d)\n"
            "id           = %s\n"
            "xres         = %d px\n"
            "yres         = %d px\n"
            "xres_virtual = %d px\n"
            "yres_virtual = %d px\n"
            "bpp          = %d\n"
            "r            = %2u:%u\n"
            "g            = %2u:%u\n"
            "b            = %2u:%u\n",
            fd,
            finfo.id,
            info.xres,
            info.yres,
            info.xres_virtual,
            info.yres_virtual,
            info.bits_per_pixel,
            info.red.offset, info.red.length,
            info.green.offset, info.green.length,
            info.blue.offset, info.blue.length
    );

    LOGI(   "width        = %d mm (%f dpi)\n"
            "height       = %d mm (%f dpi)\n"
            "refresh rate = %.2f Hz\n",
            info.width,  xdpi,
            info.height, ydpi,
            fps
    );


    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    if (finfo.smem_len <= 0)
        return -errno;


    module->flags = flags;
    module->info = info;
    module->finfo = finfo;
    module->xdpi = xdpi;
    module->ydpi = ydpi;
    module->fps = fps;

    /*
     * map the framebuffer
     */

    int err;
    size_t fbSize = roundUpToPageSize(finfo.line_length * info.yres_virtual);
    module->framebuffer = new private_handle_t(dup(fd), fbSize,
            private_handle_t::PRIV_FLAGS_USES_GPU);

    module->numBuffers = info.yres_virtual / info.yres;
    module->bufferMask = 0;

    void* vaddr = gralloc_alloc_gpu_framebuffer(gralloc_device, fbSize, fd,  module->framebuffer);

 /*   vaddr = mmap(0, fbSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (vaddr == MAP_FAILED) {
        LOGE("Error mapping the framebuffer (%s)", strerror(errno));
        return -errno;
    }*/
    module->framebuffer->base = intptr_t(vaddr);

    memset(vaddr, 0x55, fbSize);
    return 0;		
}

static int mapFrameBuffer(struct private_module_t* module, alloc_device_t *gralloc_device)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module,gralloc_device);
    pthread_mutex_unlock(&module->lock);
    return err;
}

/*****************************************************************************/

static int fb_close(struct hw_device_t *dev)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

int fb_device_open(hw_module_t const* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_FB0)) {
        alloc_device_t* gralloc_device;
        status = gralloc_open(module, &gralloc_device);
        if (status < 0)
            return status;

        /* initialize our state here */
        fb_context_t *dev = (fb_context_t*)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = fb_close;
        dev->device.setSwapInterval = fb_setSwapInterval;
        dev->device.post            = fb_post;
        dev->device.setUpdateRect = 0;

        private_module_t* m = (private_module_t*)module;
        status = mapFrameBuffer(m,gralloc_device);
        if (status >= 0) {
            int stride = m->finfo.line_length / (m->info.bits_per_pixel >> 3);
            const_cast<uint32_t&>(dev->device.flags) = 0;
            const_cast<uint32_t&>(dev->device.width) = m->info.xres;
            const_cast<uint32_t&>(dev->device.height) = m->info.yres;
            const_cast<int&>(dev->device.stride) = stride;
            const_cast<int&>(dev->device.format) = HAL_PIXEL_FORMAT_RGB_565;
            const_cast<float&>(dev->device.xdpi) = m->xdpi;
            const_cast<float&>(dev->device.ydpi) = m->ydpi;
            const_cast<float&>(dev->device.fps) = m->fps;
            const_cast<int&>(dev->device.minSwapInterval) = 1;
            const_cast<int&>(dev->device.maxSwapInterval) = 1;

            if (m->finfo.reserved[0] == 0x5444 &&
                    m->finfo.reserved[1] == 0x5055) {
                dev->device.setUpdateRect = fb_setUpdateRect;
                LOGD("UPDATE_ON_DEMAND supported");
            }

            *device = &dev->device.common;
        }
    }
    return status;
}

/* Copy a pmem buffer to the framebuffer */

static void
msm_copy_buffer(buffer_handle_t handle, int fd, int width, int height,
                int x, int y, int w, int h)
{
    struct {
        unsigned int count;
        mdp_blit_req req;
    } blit;
    private_handle_t *priv = (private_handle_t*) handle;

    memset(&blit, 0, sizeof(blit));
    blit.count = 1;

    blit.req.flags = 0;
    blit.req.alpha = 0xff;
    blit.req.transp_mask = 0xffffffff;

    blit.req.src.width = width;
    blit.req.src.height = height;
    blit.req.src.offset = 0;
    blit.req.src.memory_id = priv->fd;

    blit.req.dst.width = width;
    blit.req.dst.height = height;
    blit.req.dst.offset = 0;
    blit.req.dst.memory_id = fd; 
    blit.req.dst.format = MDP_RGB_565;

    blit.req.src_rect.x = blit.req.dst_rect.x = x;
    blit.req.src_rect.y = blit.req.dst_rect.y = y;
    blit.req.src_rect.w = blit.req.dst_rect.w = w;
    blit.req.src_rect.h = blit.req.dst_rect.h = h;

    if (ioctl(fd, MSMFB_BLIT, &blit))
        LOGE("MSMFB_BLIT failed = %d", -errno);
}
