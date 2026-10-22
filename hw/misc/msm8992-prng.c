/*
 * Minimal Qualcomm QRNG (PRNG) block for the msm8992 machine.
 *
 * DeviceMemoryMap "PERIPH_SS_PRNG" @ 0xF9BFF000 (4 KB). Retail SBL1 pokes
 * registers here during crypto/secboot bring-up; without a model the write
 * aborts (FAR 0xF9BFF100) and SBL1 takes the shutdown path. Writes are
 * accepted and read back; the data registers hand out a moving value so
 * callers waiting on entropy do not spin.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_PRNG "msm8992-prng"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992PrngState, MSM8992_PRNG)

#define PRNG_SIZE 0x1000

typedef struct Msm8992PrngState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[PRNG_SIZE / 4];
    uint32_t seed;
} Msm8992PrngState;

static uint64_t msm8992_prng_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992PrngState *s = MSM8992_PRNG(opaque);

    s->seed = s->seed * 1103515245u + 12345u +
              (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    switch (offset) {
    case 0x0:                      /* PRNG_DATA */
        return s->seed | 1;        /* never zero: rader cbz-loops on it */
    case 0x4:                      /* PRNG_STATUS: bit0 = data available */
        return 1;
    default:
        return s->regs[offset / 4];
    }
}

static void msm8992_prng_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Msm8992PrngState *s = MSM8992_PRNG(opaque);

    s->regs[offset / 4] = (uint32_t)value;
}

static const MemoryRegionOps msm8992_prng_ops = {
    .read = msm8992_prng_read,
    .write = msm8992_prng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_prng_realize(DeviceState *dev, Error **errp)
{
    Msm8992PrngState *s = MSM8992_PRNG(dev);

    s->seed = 0x12345678u;
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_prng_ops, s,
                          TYPE_MSM8992_PRNG, PRNG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_prng_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_prng_realize;
    dc->desc = "Qualcomm QRNG minimal";
}

static const TypeInfo msm8992_prng_info = {
    .name          = TYPE_MSM8992_PRNG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992PrngState),
    .class_init    = msm8992_prng_class_init,
};

static void msm8992_prng_register_types(void)
{
    type_register_static(&msm8992_prng_info);
}
type_init(msm8992_prng_register_types)
