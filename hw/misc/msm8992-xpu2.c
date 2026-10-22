/*
 * Qualcomm MSM8992 "secure peripheral" window (0xFC4B8000..0xFC4BEFFF).
 *
 * The TrustZone firmware owns a table of the peripherals it protects with
 * the XPU2 firewall.  Each entry carries the normal-world address
 * (0xFC4B80xx) and its secure alias (0xFC4BC0xx); the TZ programs a
 * range descriptor into the peripheral and then reads the block back.
 *
 * The hardware is not modelled beyond that: the window keeps whatever was
 * written and returns it on a read, so the firmware's readback check
 * succeeds.  Without it the TZ never finishes protecting its resources
 * and never hands over to the HLOS.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define TYPE_MSM8992_XPU2 "msm8992-xpu2"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992Xpu2State, MSM8992_XPU2)

#define MSM8992_XPU2_SIZE  0x7000
#define MSM8992_XPU2_TRACE_MAX 4096

struct Msm8992Xpu2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[MSM8992_XPU2_SIZE / 4];
    uint32_t trace_count;
};

static uint64_t msm8992_xpu2_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992Xpu2State *s = MSM8992_XPU2(opaque);
    uint32_t v = 0;

    if (offset + size <= sizeof(s->regs)) {
        memcpy(&v, (uint8_t *)s->regs + offset, size);
    }

    if (s->trace_count < MSM8992_XPU2_TRACE_MAX) {
        s->trace_count++;
        qemu_log_mask(LOG_UNIMP,
                      "xpu2: read  0x%05" HWADDR_PRIx " = 0x%08" PRIx64 "\n",
                      (hwaddr)offset, (uint64_t)v);
    }
    return v;
}

static void msm8992_xpu2_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Msm8992Xpu2State *s = MSM8992_XPU2(opaque);

    if (offset + size <= sizeof(s->regs)) {
        memcpy((uint8_t *)s->regs + offset, &value, size);
    }

    if (s->trace_count < MSM8992_XPU2_TRACE_MAX) {
        s->trace_count++;
        qemu_log_mask(LOG_UNIMP,
                      "xpu2: write 0x%05" HWADDR_PRIx " = 0x%08" PRIx64 "\n",
                      (hwaddr)offset, value);
    }
}

static const MemoryRegionOps msm8992_xpu2_ops = {
    .read = msm8992_xpu2_read,
    .write = msm8992_xpu2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_xpu2_realize(DeviceState *dev, Error **errp)
{
    Msm8992Xpu2State *s = MSM8992_XPU2(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_xpu2_ops, s,
                          TYPE_MSM8992_XPU2, MSM8992_XPU2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_xpu2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_xpu2_realize;
    dc->desc = "Qualcomm MSM8992 secure peripheral (XPU2) window";
}

static const TypeInfo msm8992_xpu2_info = {
    .name          = TYPE_MSM8992_XPU2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992Xpu2State),
    .class_init    = msm8992_xpu2_class_init,
};

static void msm8992_xpu2_register_types(void)
{
    type_register_static(&msm8992_xpu2_info);
}

type_init(msm8992_xpu2_register_types)
