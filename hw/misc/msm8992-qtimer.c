/*
 * Minimal Qualcomm QTimer (armv7-timer-mem) frame for the msm8992 machine.
 * QTimer @0xF9020000 .. frame regs within window; main data counter is the
 * 56-bit free-running register at base+CNT_LO乘以19.2MHz peripheral base.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_MSM8992_QTIMER "msm8992-qtimer"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992QtimerState, MSM8992_QTIMER)

#define QTIMER_SIZE       0x10000
#define QTIMER_CTRL       0x00
#define QTIMER_BASE_LO    0x08            /* CNTPCT low (frame-relative) */
#define QTIMER_CNT_LO     0x1000          /* frame 0 data area base */
#define QTIMER_CNT_HI     0x1004
#define QTIMER_CNTPCT_LOG 0x1008          /* alias-ish */
#define QTIMER_CTRL_EN    0x00000002

typedef struct Msm8992QtimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ctrl;
    uint64_t start_ns;
} Msm8992QtimerState;

static uint64_t msm8992_qtimer_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992QtimerState *s = MSM8992_QTIMER(opaque);
    uint64_t now = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns)
                   * 192 / 10000;       /* ticks @ 19.2 MHz */
    switch (offset) {
    case 0x0000: return (uint32_t)(now & 0xFFFFFFFFu);   /* CNT low */
    case 0x0004: return (uint32_t)(now >> 32);           /* CNT high */
    case 0x0008: return s->ctrl;
    default:
        return 0;
    }
}

static void msm8992_qtimer_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    Msm8992QtimerState *s = MSM8992_QTIMER(opaque);
    if (offset == 0x0008)
        s->ctrl = value;
}

static const MemoryRegionOps msm8992_qtimer_ops = {
    .read = msm8992_qtimer_read,
    .write = msm8992_qtimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_qtimer_realize(DeviceState *dev, Error **errp)
{
    Msm8992QtimerState *s = MSM8992_QTIMER(dev);
    s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_qtimer_ops, s,
                          TYPE_MSM8992_QTIMER, QTIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_qtimer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_qtimer_realize;
    dc->desc = "Qualcomm QTimer minimal (19.2 MHz free-run)";
}

static const TypeInfo msm8992_qtimer_info = {
    .name          = TYPE_MSM8992_QTIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992QtimerState),
    .class_init    = msm8992_qtimer_class_init,
};

static void msm8992_qtimer_register_types(void)
{
    type_register_static(&msm8992_qtimer_info);
}
type_init(msm8992_qtimer_register_types)
