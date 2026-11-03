/*
 * msm8992-fb: Lumia 950 framebuffer scanout for the QEMU msm8992 machine.
 * The WB EG2 framebuffer lives at 0x400000 in DRAM (1080x1920 BGRX per
 * DeviceMemoryMap.h). Map that slice, publish it as a QemuConsole and
 * blit it into the display surface on every gfx_update - the UI backend
 * (GTK/VNC/SDL) polls consoles at ~30 fps, so the guest's writes to
 * DRAM appear on screen without any guest cooperation.
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
#include "ui/pixel_ops.h"

#define TYPE_MSM8992_FB "msm8992-fb"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992FbState, MSM8992_FB)

typedef struct Msm8992FbState {
    SysBusDevice parent_obj;
    QemuConsole *console;
    void *ram_ptr;
    hwaddr mapsize;
    bool surface_ready;
} Msm8992FbState;

#define MSM8992_FB_ADDR  0x400000ULL
#define MSM8992_FB_W     1080
#define MSM8992_FB_H     1920
#define MSM8992_FB_STRIDE (MSM8992_FB_W * 4)
#define MSM8992_FB_SIZE  (MSM8992_FB_STRIDE * MSM8992_FB_H)

/*
 * The UI backend calls this at its own refresh rate (GTK/VNC ~30 fps).
 * The first call swaps the placeholder surface for a real 1080x1920 one,
 * every call copies the guest framebuffer into it and marks the whole
 * area dirty.  A straight memcpy is enough: at 1080x1920x4 = 8.3 MB per
 * frame the copy costs well under a millisecond.
 */
static void msm8992_fb_blit(Msm8992FbState *s)
{
    DisplaySurface *surface;

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
    s->surface_ready = false;
}

static void msm8992_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm8992_fb_realize;
    dc->unrealize = msm8992_fb_unrealize;
    dc->desc = "Lumia 950 framebuffer scanout (DRAM @ 0x400000)";
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