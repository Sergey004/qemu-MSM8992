/*
 * Qualcomm PCIe wrapper / DM-core window of the MSM8992 (0xFF000000, 8 MB).
 *
 * The firmware's PCIExpressConfiguratorDxe (Lumia950XLPkg) programs this
 * window before anything else can boot:
 *
 *   - the PLR IATU registers (outbound/inbound windows) at +0x900,
 *   - the link control and status registers,
 *   - the config space windows at +0x100000.
 *
 * There is no PCIe device on the phone, so nothing behind the wrapper is
 * emulated: reads return zero and writes are dropped.  One status bit does
 * matter - the driver waits for the link training to report "complete"
 * (PCIE20_ELBI_SYS_STTS bit 10 at +0xf28) before it goes on, and the
 * stock firmware sees exactly that on the real device.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_PCIE "msm8992-pcie"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992PcieState, MSM8992_PCIE)

#define MSM8992_PCIE_SIZE   0x800000

/* PCIE20_ELBI_SYS_STTS */
#define PCIE_ELBI_SYS_STTS  0xF28
#define PCIE_LINK_TRAINED    0x00000400

typedef struct Msm8992PcieState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
} Msm8992PcieState;

static uint64_t msm8992_pcie_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t v = 0;

    switch (offset) {
    case PCIE_ELBI_SYS_STTS:
        /* the link training is done */
        v = PCIE_LINK_TRAINED;
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "msm8992-pcie: read  0x%05" HWADDR_PRIx
                  " = 0x%08" PRIx32 "\n", offset, v);
    return v;
}

static void msm8992_pcie_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "msm8992-pcie: write 0x%05" HWADDR_PRIx
                  " = 0x%08" PRIx64 "\n", offset, value);
}

static const MemoryRegionOps msm8992_pcie_ops = {
    .read = msm8992_pcie_read,
    .write = msm8992_pcie_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_pcie_realize(DeviceState *dev, Error **errp)
{
    Msm8992PcieState *s = MSM8992_PCIE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_pcie_ops, s,
                          TYPE_MSM8992_PCIE, MSM8992_PCIE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_pcie_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = msm8992_pcie_realize;
    dc->desc = "Qualcomm PCIe wrapper / DM core (MSM8992)";
}

static const TypeInfo msm8992_pcie_info = {
    .name          = TYPE_MSM8992_PCIE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992PcieState),
    .class_init    = msm8992_pcie_class_init,
};

static void msm8992_pcie_register_types(void)
{
    type_register_static(&msm8992_pcie_info);
}

type_init(msm8992_pcie_register_types)
