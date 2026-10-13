/*
 * Qualcomm GCC (global clock controller, qcom,gcc-8992) minimal model for
 * msm8992 machine: register file holding last-written values; "polling"
 * patterns like writing a cmd/enable bit and then reading the status bit
 * get coherent answers, so boot firmware's clock-acknowledgement waits
 * complete. This is deliberately a "benevolent" model — whatever the
 * firmware programs simply stays set.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_GCC "msm8992-gcc"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992GccState, MSM8992_GCC)

#define MSM8992_GCC_SIZE 0x2000
#define GCC_REGS_WORDS (MSM8992_GCC_SIZE / 4)

typedef struct Msm8992GccState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[GCC_REGS_WORDS];
    uint64_t hot_bm[GCC_REGS_WORDS / 64 + 1];
} Msm8992GccState;

static uint64_t msm8992_gcc_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992GccState *s = MSM8992_GCC(opaque);
    uint32_t v;

    if (offset > MSM8992_GCC_SIZE - 4)
        return 0;
    v = s->regs[offset >> 2];

    /*
     * QC Global Clock Controller (RCG) semantics seen in SBL1:
     *   write UPDATE (bit 0) -> hardware applies the config -> STATUS reads
     *   bit31 = ACK/LOCKED set once done. QEMU apply is instantaneous: we
     *   always report ACK on reads (bit31 set, keep everything else as last
     *   written).
     */
    v |= 0x80000000u;
    return v;
}

static void msm8992_gcc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Msm8992GccState *s = MSM8992_GCC(opaque);
    if (offset > MSM8992_GCC_SIZE - 4)
        return;
    s->regs[offset >> 2] = value;
    s->hot_bm[(offset >> 2) / 64] |= 1ULL << ((offset >> 2) & 63);
}

static const MemoryRegionOps msm8992_gcc_ops = {
    .read = msm8992_gcc_read,
    .write = msm8992_gcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


static void msm8992_gcc_realize(DeviceState *dev, Error **errp)
{
    Msm8992GccState *s = MSM8992_GCC(dev);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_gcc_ops, s,
                          TYPE_MSM8992_GCC, MSM8992_GCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_gcc_reset(DeviceState *dev)
{
    Msm8992GccState *s = MSM8992_GCC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->hot_bm, 0, sizeof(s->hot_bm));
}

static void msm8992_gcc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_gcc_realize;
    device_class_set_legacy_reset(dc, msm8992_gcc_reset);
    dc->desc = "Qualcomm Global Clock Controller, benign register file";
}

static const TypeInfo msm8992_gcc_info = {
    .name          = TYPE_MSM8992_GCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992GccState),
    .class_init    = msm8992_gcc_class_init,
};

static void msm8992_gcc_register_types(void)
{
    type_register_static(&msm8992_gcc_info);
}

type_init(msm8992_gcc_register_types)
