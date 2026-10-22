/*
 * Qualcomm QUP block (BLSP) in SPI mode - the MSM8992 BLSP2 QUP4 SPI
 * instance at 0xF9966000, which the firmware drives through
 * Lumia950XLPkg/GPLDriver/QupSpiDxe.
 *
 * The QUP is a DMA/IO engine; nothing behind it is emulated.  What the
 * driver needs from the hardware at init time:
 *
 *   +0x004 QUP_STATE      bit 2 (VALID) must be set, otherwise the driver
 *                         gives up with "cannot set RESET state";
 *   +0x008 QUP_IO_M_MODES the block/fifo sizes (0 selects the defaults);
 *   +0x030 QUP_HW_VERSION the block revision.
 *
 * Everything else is a plain register file: writes are kept, reads return
 * what was written.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_QUP "msm8992-qup"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992QupState, MSM8992_QUP)

#define MSM8992_QUP_SIZE 0x1000
#define QUP_REGS_WORDS   (MSM8992_QUP_SIZE / 4)

/* register offsets (see QupSpi.h of the firmware) */
#define QUP_STATE_OFF       0x004
#define QUP_IO_M_MODES_OFF  0x008
#define QUP_HW_VERSION_OFF  0x030

#define QUP_STATE_VALID   (1 << 2)
#define QUP_STATE_MASK    0x3
#define QUP_HW_VERSION_21 0x20010001

typedef struct Msm8992QupState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[QUP_REGS_WORDS];
} Msm8992QupState;

static uint64_t msm8992_qup_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992QupState *s = MSM8992_QUP(opaque);
    uint32_t v;

    if (offset > MSM8992_QUP_SIZE - 4) {
        return 0;
    }

    switch (offset) {
    case QUP_STATE_OFF:
        /* the state machine always reports a valid state */
        v = (s->regs[offset >> 2] & QUP_STATE_MASK) | QUP_STATE_VALID;
        break;
    case QUP_HW_VERSION_OFF:
        v = QUP_HW_VERSION_21;
        break;
    case QUP_IO_M_MODES_OFF:
        v = s->regs[offset >> 2];
        break;
    default:
        v = s->regs[offset >> 2];
        break;
    }
    return v;
}

static void msm8992_qup_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Msm8992QupState *s = MSM8992_QUP(opaque);

    if (offset > MSM8992_QUP_SIZE - 4) {
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps msm8992_qup_ops = {
    .read = msm8992_qup_read,
    .write = msm8992_qup_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_qup_realize(DeviceState *dev, Error **errp)
{
    Msm8992QupState *s = MSM8992_QUP(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_qup_ops, s,
                          TYPE_MSM8992_QUP, MSM8992_QUP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_qup_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm8992_qup_realize;
    dc->desc = "Qualcomm QUP block (BLSP) in SPI mode";
}

static const TypeInfo msm8992_qup_info = {
    .name          = TYPE_MSM8992_QUP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992QupState),
    .class_init    = msm8992_qup_class_init,
};

static void msm8992_qup_register_types(void)
{
    type_register_static(&msm8992_qup_info);
}

type_init(msm8992_qup_register_types)
