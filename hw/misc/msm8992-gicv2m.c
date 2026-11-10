/*
 * msm8992 GICv2M MSI frame (the "GICM" block the W10M kernel's GTDT
 * platform-timer walk maps and probes).
 *
 * The stock GTDT carries a type-0xD subtable whose base address points at
 * one v2M frame (observed: 0xF9007000).  During early kernel init ntoskrnl
 * maps the frame and reads MSI_TYPER (+0x8) to learn the MSI SPI window:
 * bits [25:16] = number of SPIs, bits [9:0] = first SPI.  With nothing
 * answering there, HalInitSystem ends up with an empty MSI provider list
 * and bugchecks 0x5C (HAL_INITIALIZATION_FAILED) with arg 0x110.
 *
 * Values follow the Android DT description of the same hardware
 * (qcom,msi-gicm-base = 0x180): MSI SPIs start at SPI 0x180 (INTID 416),
 * 64 SPIs wide.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/dma.h"
#include "hw/core/cpu.h"

#define TYPE_MSM8992_GICV2M "msm8992-gicv2m"
OBJECT_DECLARE_SIMPLE_TYPE(MsmGicv2mState, MSM8992_GICV2M)

typedef struct MsmGicv2mState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq *spi;          /* one output per MSI SPI window entry */

    uint32_t first_spi;     /* first SPI of the MSI window */
    uint32_t num_spis;      /* window size in SPIs */
    uint32_t setspi_ns;     /* last value written to MSI_SETSPI_NS */
    uint8_t seen[16];       /* dedup bitmap for debug logging (per offset) */
} MsmGicv2mState;

static const Property gicv2m_props[] = {
    DEFINE_PROP_UINT32("first-spi", MsmGicv2mState, first_spi, 0x180),
    DEFINE_PROP_UINT32("num-spis", MsmGicv2mState, num_spis, 0x40),
};

static void gicv2m_inject(MsmGicv2mState *s, uint32_t spi)
{
    if (spi < s->first_spi || spi >= s->first_spi + s->num_spis) {
        qemu_log_mask(LOG_GUEST_ERROR, "msm8992-gicv2m: SETSPI SPI %u out of "
                      "window [%u..%u)\n", spi, s->first_spi,
                      s->first_spi + s->num_spis);
        return;
    }
    /* level pulse */
    qemu_set_irq(s->spi[spi - s->first_spi], 1);
    qemu_set_irq(s->spi[spi - s->first_spi], 0);
}

static uint64_t gicv2m_read(void *opaque, hwaddr offset, unsigned size)
{
    MsmGicv2mState *s = MSM8992_GICV2M(opaque);
    uint64_t val = 0;

    switch (offset) {
    case 0x00:              /* MSG_TYPER (RO) */
        val = 0;
        break;
    case 0x08:              /* MSI_TYPER (RO): [25:16]=num, [9:0]=first */
        val = ((s->num_spis & 0x3ff) << 16) | (s->first_spi & 0x3ff);
        break;
    case 0x40:              /* MSI_SETSPI_NS */
        val = s->setspi_ns;
        break;
    default:
        val = 0;
        break;
    }
    if (offset < 0x80 && !(s->seen[offset / 8] & (1u << (offset % 8)))) {
        s->seen[offset / 8] |= 1u << (offset % 8);
        qemu_log("msm8992-gicv2m: read +0x%02llx -> 0x%0*llx (guest_pc=0x%016llx)\n",
                 (unsigned long long)offset, (int)(size * 2),
                 (unsigned long long)val,
                 (unsigned long long)(current_cpu && current_cpu->cc &&
                                      current_cpu->cc->get_pc
                                      ? current_cpu->cc->get_pc(current_cpu) : 0));
    }
    return val;
}

static void gicv2m_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    MsmGicv2mState *s = MSM8992_GICV2M(opaque);

    switch (offset) {
    case 0x40:              /* MSI_SETSPI_NS: inject the SPI */
        s->setspi_ns = (uint32_t)value;
        gicv2m_inject(s, (uint32_t)(value & 0x3ff));
        break;
    default:
        break;
    }
}

static const MemoryRegionOps gicv2m_ops = {
    .read = gicv2m_read,
    .write = gicv2m_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void gicv2m_realize(DeviceState *dev, Error **errp)
{
    MsmGicv2mState *s = MSM8992_GICV2M(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->num_spis == 0 || s->num_spis > 1024) {
        error_setg(errp, "msm8992-gicv2m: bad num-spis %u", s->num_spis);
        return;
    }

    s->spi = g_new0(qemu_irq, s->num_spis);
    qdev_init_gpio_out(DEVICE(s), s->spi, s->num_spis);

    memory_region_init_io(&s->iomem, OBJECT(s), &gicv2m_ops, s,
                          "msm8992-gicv2m", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void gicv2m_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gicv2m_realize;
    device_class_set_props(dc, gicv2m_props);
}

static const TypeInfo gicv2m_info = {
    .name          = TYPE_MSM8992_GICV2M,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmGicv2mState),
    .class_init    = gicv2m_class_init,
};

static void gicv2m_register_types(void)
{
    type_register_static(&gicv2m_info);
}

type_init(gicv2m_register_types)
