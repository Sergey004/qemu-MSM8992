/*
 * Minimal QC APCS/SMD doorbell scratch register window (stage-2 bringup).
 * Stock UEFI writes interprocessor interrupt kicks here (e.g. the SMD
 * "rpm_requests" doorbell at 0xF900D008 on the 8994-family fallback path).
 * We model it as a 4 KiB store/readback pad so those writes stop aborting;
 * the actual peer (RPM) is emulated by the SMEM ghost in msm8992.c.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_APCS "msm8992-apcs"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992ApcsState, MSM8992_APCS)

struct Msm8992ApcsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x1000 / 4];
};

static uint64_t msm8992_apcs_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992ApcsState *s = MSM8992_APCS(opaque);
    if (offset >= sizeof(s->regs))
        return 0;
    return s->regs[offset >> 2];
}

static void msm8992_apcs_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Msm8992ApcsState *s = MSM8992_APCS(opaque);
    if (offset < sizeof(s->regs))
        s->regs[offset >> 2] = value;
}

static const MemoryRegionOps msm8992_apcs_ops = {
    .read = msm8992_apcs_read,
    .write = msm8992_apcs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_apcs_realize(DeviceState *dev, Error **errp)
{
    Msm8992ApcsState *s = MSM8992_APCS(dev);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_apcs_ops, s,
                          TYPE_MSM8992_APCS, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_apcs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_apcs_realize;
    dc->desc = "QC APCS/SMD doorbell scratch window";
}

static const TypeInfo msm8992_apcs_info = {
    .name          = TYPE_MSM8992_APCS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992ApcsState),
    .class_init    = msm8992_apcs_class_init,
};

static void msm8992_apcs_register_types(void)
{
    type_register_static(&msm8992_apcs_info);
}
type_init(msm8992_apcs_register_types)
