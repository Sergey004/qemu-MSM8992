/*
 * Minimal Qualcomm SPMI PMIC Arbiter stub for the msm8992 machine.
 *
 * Retail SBL1 drives PMIC through the arbiter at 0xFC4C0000: it writes the
 * PPID->channel mapping tables (bulk 0xFF), programs the channel/arbiter
 * registers (0xA000/0xAF00 blocks), then polls CH0 IRQ_STATUS at +0xF808
 * for TRAN_DONE. Reads-as-zero never complete that poll and the failure
 * path is fatal ("SPMI Fatal: SPMI bus busy" -> shutdown). This stub
 * accepts every write, reports the transaction done immediately and keeps
 * the arbiter idle, so the fake PMIC always ACKs.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_SPMI "msm8992-spmi"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992SpmiState, MSM8992_SPMI)

#define SPMI_SIZE        0x10000
#define SPMI_CH0_IRQ_ST  0xF808   /* CH0 IRQ_STATUS: TRAN_DONE polled here */
#define SPMI_ARB_STATUS  0xAF20
#define SPMI_CH0_STATUS  0xA028
#define SPMI_CH0_STATUS2 0xA02C

typedef struct Msm8992SpmiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[SPMI_SIZE / 4];
    bool tran_done;     /* CH0 IRQ_STATUS TRAN_DONE, set by CMD writes */
} Msm8992SpmiState;

static uint64_t msm8992_spmi_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992SpmiState *s = MSM8992_SPMI(opaque);

    switch (offset) {
    case SPMI_CH0_IRQ_ST:
        return s->tran_done ? 1 : 0;
    case SPMI_ARB_STATUS:
    case SPMI_CH0_STATUS:
    case SPMI_CH0_STATUS2:
        return 0;                     /* arbiter idle, channel not busy */
    default:
        return s->regs[offset / 4];   /* mapping tables read back written values */
    }
}

static void msm8992_spmi_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Msm8992SpmiState *s = MSM8992_SPMI(opaque);

    s->regs[offset / 4] = (uint32_t)value;

    if (offset == SPMI_CH0_IRQ_ST || offset == SPMI_CH0_IRQ_ST + 4) {
        s->tran_done = false;         /* IRQ clear */
    } else if ((offset >= 0xA000 && offset <= 0xA004) ||
               (offset >= 0xF800 && offset <= 0xF807)) {
        s->tran_done = true;          /* command issued: complete it */
    }
}

static const MemoryRegionOps msm8992_spmi_ops = {
    .read = msm8992_spmi_read,
    .write = msm8992_spmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_spmi_realize(DeviceState *dev, Error **errp)
{
    Msm8992SpmiState *s = MSM8992_SPMI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_spmi_ops, s,
                          TYPE_MSM8992_SPMI, SPMI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_spmi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_spmi_realize;
    dc->desc = "Qualcomm SPMI PMIC Arbiter minimal (always-done)";
}

static const TypeInfo msm8992_spmi_info = {
    .name          = TYPE_MSM8992_SPMI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992SpmiState),
    .class_init    = msm8992_spmi_class_init,
};

static void msm8992_spmi_register_types(void)
{
    type_register_static(&msm8992_spmi_info);
}
type_init(msm8992_spmi_register_types)
