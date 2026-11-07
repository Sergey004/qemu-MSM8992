/*
 * msm8992-fb: Lumia 950 framebuffer scanout for the QEMU msm8992 machine.
 *
 * Two sources of pixels:
 *   1. The boot pipeline (UEFI/winload) draws into the WB EG2
 *      framebuffer at 0x400000, 1080x1920 BGRX (DeviceMemoryMap.h) -
 *      this is the default scanout.
 *   2. The MainOS display driver programs MDP5 (0xFD900000) with its
 *      own pipe configuration - typically a switch to the native
 *      1440x2560 panel mode right after the boot logo disappears.  The
 *      MDP5 register window below captures the SSPP source address /
 *      stride / size writes, and the scanout follows: the same console
 *      shows the boot FB first, the MDP framebuffer afterwards.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "system/physmem.h"
#include "qemu/log.h"
#include "ui/pixel_ops.h"

#define TYPE_MSM8992_FB "msm8992-fb"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992FbState, MSM8992_FB)

#define MSM8992_FB_ADDR  0x400000ULL
#define MSM8992_FB_W     1080
#define MSM8992_FB_H     1920
#define MSM8992_FB_STRIDE (MSM8992_FB_W * 4)
#define MSM8992_FB_SIZE  (MSM8992_FB_STRIDE * MSM8992_FB_H)

/* MDP5 (0xFD900000, 0xB4000): SSPP pipe blocks live at 0x1000..0x2FFF,
 * every block uses the same register strip: SRC_SIZE +0x100 (h<<16|w),
 * SRC0_ADDR +0x114, SRC_YSTRIDE +0x124 (bytes), SRC_FORMAT +0x130. */
#define MSM8992_MDP5_BASE 0xFD900000ULL
#define MSM8992_MDP5_SIZE 0xB4000ULL
#define MDP_SSPP_LO       0x1000
#define MDP_SSPP_HI       0x32FF
#define MDP_OFF_SIZE      0x100
#define MDP_OFF_ADDR0     0x114
#define MDP_OFF_STRIDE    0x124
#define MDP_OFF_FORMAT    0x130
#define MDP_OFF_MASK      0x3FF

typedef struct Msm8992FbState {
    SysBusDevice parent_obj;
    QemuConsole *console;
    void *ram_ptr;             /* boot FB mapping (0x400000) */
    hwaddr mapsize;
    bool surface_ready;

    /* MDP5 register file */
    uint32_t *regs;
    MemoryRegion iomem;

    /* captured live scanout config */
    bool     mdp_active;
    hwaddr   mdp_fb_addr;
    void    *mdp_map;
    hwaddr   mdp_map_len;
    uint32_t mdp_w, mdp_h, mdp_stride;
    uint32_t mdp_format;
} Msm8992FbState;

static void msm8992_fb_remap(Msm8992FbState *s, hwaddr addr,
                             uint32_t w, uint32_t h, uint32_t stride)
{
    uint64_t needed = (uint64_t)stride * h;

    if (s->mdp_map) {
        physical_memory_unmap(s->mdp_map, s->mdp_map_len, 0, 0);
        s->mdp_map = NULL;
    }
    s->mdp_map_len = needed;
    s->mdp_map = physical_memory_map(addr, &s->mdp_map_len, false);
    if (s->mdp_map_len != needed) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "msm8992-fb: MDP fb 0x%llx+%llx crosses RAM end\n",
                      (unsigned long long)addr,
                      (unsigned long long)needed);
        if (s->mdp_map) {
            physical_memory_unmap(s->mdp_map, s->mdp_map_len, 0, 0);
            s->mdp_map = NULL;
        }
        s->mdp_active = false;
        return;
    }
    s->mdp_fb_addr = addr;
    s->mdp_w = w;
    s->mdp_h = h;
    s->mdp_stride = stride;
    s->mdp_active = true;
    s->surface_ready = false;   /* force console resize */
    qemu_log_mask(LOG_UNIMP,
                  "msm8992-fb: MDP5 scanout -> 0x%llx %ux%u stride %u\n",
                  (unsigned long long)addr, w, h, stride);
}

static uint64_t msm8992_fb_mdp_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992FbState *s = opaque;

    if (offset >= MSM8992_MDP5_SIZE) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void msm8992_fb_mdp_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Msm8992FbState *s = opaque;

    if (offset >= MSM8992_MDP5_SIZE) {
        return;
    }
    s->regs[offset >> 2] = value;

    if (offset >= MDP_SSPP_LO && offset <= MDP_SSPP_HI) {
        hwaddr pipe = offset & ~((hwaddr)MDP_OFF_MASK);
        uint32_t off = offset & MDP_OFF_MASK;
        uint32_t w, h, stride, addr;

        if (off == MDP_OFF_ADDR0) {
            addr = value;
            w    = s->regs[(pipe + MDP_OFF_SIZE) >> 2] & 0xffff;
            h    = s->regs[(pipe + MDP_OFF_SIZE) >> 2] >> 16;
            stride = s->regs[(pipe + MDP_OFF_STRIDE) >> 2];
            s->mdp_format = s->regs[(pipe + MDP_OFF_FORMAT) >> 2];
            if (addr != 0 && w != 0 && h != 0 && stride != 0) {
                msm8992_fb_remap(s, addr, w, h, stride);
            }
        }
    }
}

static const MemoryRegionOps msm8992_fb_mdp_ops = {
    .read = msm8992_fb_mdp_read,
    .write = msm8992_fb_mdp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * The UI backend calls this at its own refresh rate (GTK/VNC ~30 fps).
 * Scanout follows the active source: the boot FB until MDP5 takes over,
 * the MDP pipe framebuffer afterwards.
 */
static void msm8992_fb_blit(Msm8992FbState *s)
{
    DisplaySurface *surface;

    if (s->mdp_active) {
        if (!s->surface_ready) {
            qemu_console_resize(s->console, s->mdp_w, s->mdp_h);
            s->surface_ready = true;
        }
        surface = qemu_console_surface(s->console);
        if (!surface || surface_is_placeholder(surface)) {
            return;
        }
        {
            uint8_t *src = s->mdp_map;
            uint8_t *dst = surface_data(surface);
            uint32_t row;

            for (row = 0; row < s->mdp_h; row++) {
                memcpy(dst + row * s->mdp_w * 4,
                       src + row * s->mdp_stride,
                       MIN(s->mdp_w * 4, s->mdp_stride));
            }
        }
        qemu_console_update_full(s->console);
        return;
    }

    if (!s->surface_ready) {
        qemu_console_resize(s->console, MSM8992_FB_W, MSM8992_FB_H);
        s->surface_ready = true;
    }
    surface = qemu_console_surface(s->console);
    if (!surface || surface_is_placeholder(surface)) {
        return;
    }
    memcpy(surface_data(surface), s->ram_ptr, MSM8992_FB_SIZE);
    qemu_console_update_full(s->console);
}

static bool msm8992_fb_update(void *opaque)
{
    msm8992_fb_blit(MSM8992_FB(opaque));
    return true;
}

static void msm8992_fb_invalidate(void *opaque)
{
    msm8992_fb_blit(MSM8992_FB(opaque));
}

static const GraphicHwOps msm8992_fb_ops = {
    .gfx_update  = msm8992_fb_update,
    .invalidate  = msm8992_fb_invalidate,
};

static void msm8992_fb_realize(DeviceState *dev, Error **errp)
{
    Msm8992FbState *s = MSM8992_FB(dev);
    /* Map guest FB directly — reads ride through the DRAM alias */
    hwaddr mapsize = MSM8992_FB_SIZE;

    s->ram_ptr = physical_memory_map(MSM8992_FB_ADDR, &mapsize, false);
    if (mapsize != MSM8992_FB_SIZE) {
        physical_memory_unmap(s->ram_ptr, mapsize, 0, 0);
        error_setg(errp, "msm8992-fb: cannot map framebuffer");
        return;
    }

    /* MDP5 register window */
    s->regs = g_new0(uint32_t, MSM8992_MDP5_SIZE / 4);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_fb_mdp_ops, s,
                          "msm8992-mdp5", MSM8992_MDP5_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    s->console = qemu_graphic_console_create(dev, 0, &msm8992_fb_ops, dev);
}

static void msm8992_fb_unrealize(DeviceState *dev)
{
    Msm8992FbState *s = MSM8992_FB(dev);

    if (s->console) {
        qemu_graphic_console_close(s->console);
        s->console = NULL;
    }
    if (s->ram_ptr) {
        physical_memory_unmap(s->ram_ptr, MSM8992_FB_SIZE, 0, 0);
        s->ram_ptr = NULL;
    }
    if (s->mdp_map) {
        physical_memory_unmap(s->mdp_map, s->mdp_map_len, 0, 0);
        s->mdp_map = NULL;
    }
    g_free(s->regs);
    s->surface_ready = false;
}

static void msm8992_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm8992_fb_realize;
    dc->unrealize = msm8992_fb_unrealize;
    dc->desc = "Lumia 950 display: boot FB scanout + MDP5 pipe capture";
}

static const TypeInfo msm8992_fb_info = {
    .name          = TYPE_MSM8992_FB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992FbState),
    .class_init    = msm8992_fb_class_init,
};

static void msm8992_fb_register_types(void)
{
    type_register_static(&msm8992_fb_info);
}
type_init(msm8992_fb_register_types)