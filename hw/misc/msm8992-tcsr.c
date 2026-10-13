/*
 * TCSR window (0xFD4A0000) for msm8992: answers the SoC-ID register that
 * paves a clear road for bootloaders. Nobody-owned alias gets zero.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_TCSR "msm8992-tcsr"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992TcsrState, MSM8992_TCSR)

typedef struct Msm8992TcsrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
} Msm8992TcsrState;

/* SBL1 reads an 8-byte ID at offset 0x8000 (per DT cpuss@fd4a8000).
 * Return the SMEM soc id 251 embedded */
#define MSM8992_TCSR_SIZE    0x10000
#define MSM8992_TCSR_SOCVER  0x8000

static uint64_t msm8992_tcsr_read(void *opaque, hwaddr offset, unsigned size)
{
    switch (offset) {
    case MSM8992_TCSR_SOCVER:
        return 0xFB;             /* socinfo id 251 = MSM8992 */
    default:
        return 0;
    }
}

static void msm8992_tcsr_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
}

static const MemoryRegionOps msm8992_tcsr_ops = {
    .read = msm8992_tcsr_read,
    .write = msm8992_tcsr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void msm8992_tcsr_realize(DeviceState *dev, Error **errp)
{
    Msm8992TcsrState *s = MSM8992_TCSR(dev);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_tcsr_ops, s,
                          TYPE_MSM8992_TCSR, MSM8992_TCSR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_tcsr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_tcsr_realize;
    dc->desc = "Qualcomm TCSR window with SoC id";
}

static const TypeInfo msm8992_tcsr_info = {
    .name          = TYPE_MSM8992_TCSR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992TcsrState),
    .class_init    = msm8992_tcsr_class_init,
};

static void msm8992_tcsr_register_types(void)
{
    type_register_static(&msm8992_tcsr_info);
}

type_init(msm8992_tcsr_register_types)
