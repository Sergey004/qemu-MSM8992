/*
 * Qualcomm SDCC glue for the msm8992 machine.
 *
 * The Lumia 950 UEFI (Lumia950XLPkg/Driver/SdhciMMCHSDxe) drives the SD
 * controller of the phone through two register banks:
 *
 *   - the "SDCC core" bank at 0xF9824000 - the very window the retail SBL1
 *     uses as a legacy MCI.  On top of the legacy registers the UEFI expects
 *     MCI_VERSION (0x50), SDCC_MCI_HC_MODE (0x78) and the power control
 *     quad PWRCTL status/mask/clear/ctl (0xdc..0xe8);
 *
 *   - the "SDHCI" bank at 0xF9824900 - the standard SDHC registers (modelled
 *     by the generic SDHCI device) plus Qualcomm's vendor registers at
 *     0x100..0x1b8 and 0xa1c.
 *
 * This device provides the SDCC core registers and the SDHCI vendor
 * registers, and the power control state machine behind them.  The guest
 * arms the power interrupt, switches the bus on and then waits in
 * gBS->WaitForEvent() for the interrupt that tells it the bus came up; the
 * SDCC interrupt line (INTID 170) carries both the card interrupt of the
 * SDHCI block and this power event.
 *
 * The power event is raised when the guest enables the power interrupt in
 * PWRCTL_MASK (the guest does that in sdhci_msm_init() right before it
 * turns the bus power on and waits for the event).  The real chip raises it
 * from the SDCC core when the SDHCI block enables the bus power; the
 * generic SDHCI device does not expose that, so the trigger is taken from
 * the mask write instead.  Everything else - which bits are pending, how
 * the guest acknowledges them and how the interrupt is deasserted - follows
 * the guest's expectations.
 *
 * Free Software Foundation; licensed GPL-2.0-only.
 */
#include "qemu/osdep.h"
#include <stdarg.h>
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_MSM8992_SDCC "msm8992-sdcc"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992SdccState, MSM8992_SDCC)

/* SDCC core registers, offset from the core base */
#define SDCC_MCI_VERSION   0x50
#define SDCC_MCI_HC_MODE   0x78
#define SDCC_PWRCTL_STATUS 0xdc
#define SDCC_PWRCTL_MASK   0xe0
#define SDCC_PWRCTL_CLEAR  0xe4
#define SDCC_PWRCTL_CTL    0xe8

/* power control bits, as the guest sees them (sdhci_msm.h) */
#define SDCC_PWR_BUS_ON          (1u << 0)
#define SDCC_PWR_BUS_OFF         (1u << 1)
#define SDCC_PWR_IO_SIG_LOW      (1u << 2)
#define SDCC_PWR_IO_SIG_HIGH     (1u << 3)
#define SDCC_PWR_BUS_ON_OFF_SUCC (1u << 0)
#define SDCC_PWR_IO_SIG_SUCC     (1u << 2)
#define SDCC_PWR_CTRL_INT        0xfu

/*
 * The SDHCI vendor registers live at offset 0x100 of the SDHCI bank; the
 * region therefore starts there and reaches past the vendor function
 * register at 0xa1c.
 */
#define SDHCI_VENDOR_BASE  0x100
#define SDHCI_VENDOR_SIZE  0x600

/* Bound the debug log: the guest touches these registers a lot. */
#define SDCC_TRACE_MAX 256

struct Msm8992SdccState {
    SysBusDevice parent_obj;

    MemoryRegion vendor;       /* SDHCI vendor registers */
    MemoryRegion core_version; /* MCI_VERSION */
    MemoryRegion core_mode;    /* SDCC_MCI_HC_MODE */
    MemoryRegion core_status;  /* PWRCTL status */
    MemoryRegion core_mask;    /* PWRCTL mask */
    MemoryRegion core_clear;   /* PWRCTL clear */
    MemoryRegion core_ctl;     /* PWRCTL ctl */

    uint32_t vendor_regs[SDHCI_VENDOR_SIZE / 4];

    uint32_t hc_mode;
    uint32_t pwrctl_status;
    uint32_t pwrctl_mask;
    uint32_t pwrctl_ctl;
    unsigned  trace_count;

    qemu_irq irq;
};

static void msm8992_sdcc_trace(Msm8992SdccState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void msm8992_sdcc_trace(Msm8992SdccState *s, const char *fmt, ...)
{
    va_list ap;
    char buf[128];

    if (s->trace_count >= SDCC_TRACE_MAX) {
        return;
    }
    s->trace_count++;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_GUEST_ERROR, "msm8992-sdcc: %s", buf);
}

static void msm8992_sdcc_update_irq(Msm8992SdccState *s)
{
    qemu_set_irq(s->irq, (s->pwrctl_status & s->pwrctl_mask) != 0);
}

static uint64_t msm8992_sdcc_vendor_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    Msm8992SdccState *s = opaque;

    if (offset + size > SDHCI_VENDOR_SIZE) {
        return 0;
    }
    return s->vendor_regs[offset / 4];
}

static void msm8992_sdcc_vendor_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    Msm8992SdccState *s = opaque;

    if (offset + size > SDHCI_VENDOR_SIZE) {
        return;
    }
    msm8992_sdcc_trace(s, "vendor[0x%x] = 0x%x\n",
                        (unsigned)(offset + SDHCI_VENDOR_BASE), (unsigned)value);
    s->vendor_regs[offset / 4] = value;
}

static const MemoryRegionOps msm8992_sdcc_vendor_ops = {
    .read = msm8992_sdcc_vendor_read,
    .write = msm8992_sdcc_vendor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t msm8992_sdcc_core_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Msm8992SdccState *s = opaque;

    msm8992_sdcc_trace(s, "core read 0x%x\n", (unsigned)offset);
    switch (offset) {
    case SDCC_MCI_VERSION:
        /*
         * Version 0 keeps the guest on the generic SDHCI code paths: it
         * does not override the host capabilities with the Qualcomm vendor
         * ones and does not use the "SDCC core 0x42" DLL sequence.
         */
        return 0;
    case SDCC_MCI_HC_MODE:
        return s->hc_mode;
    case SDCC_PWRCTL_STATUS:
        return s->pwrctl_status;
    case SDCC_PWRCTL_MASK:
        /*
         * The guest's interrupt handler reads this register to find out
         * which power events are pending, so it has to return the pending
         * status (and not just the enabled mask).
         */
        return s->pwrctl_status & s->pwrctl_mask;
    case SDCC_PWRCTL_CLEAR:
        return 0;
    case SDCC_PWRCTL_CTL:
        return s->pwrctl_ctl;
    default:
        return 0;
    }
}

static void msm8992_sdcc_core_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Msm8992SdccState *s = opaque;

    msm8992_sdcc_trace(s, "core write 0x%x = 0x%x\n",
                        (unsigned)offset, (unsigned)value);
    switch (offset) {
    case SDCC_MCI_VERSION:
        break;
    case SDCC_MCI_HC_MODE:
        msm8992_sdcc_trace(s, "HC_MODE = 0x%x\n", (unsigned)value);
        s->hc_mode = value;
        break;
    case SDCC_PWRCTL_STATUS:
        break;
    case SDCC_PWRCTL_MASK:
        msm8992_sdcc_trace(s, "PWRCTL_MASK = 0x%x\n", (unsigned)value);
        s->pwrctl_mask = value;
        if (value & SDCC_PWR_CTRL_INT) {
            /* the guest armed the power interrupt: the bus comes up */
            s->pwrctl_status |= SDCC_PWR_BUS_ON;
            msm8992_sdcc_update_irq(s);
        }
        break;
    case SDCC_PWRCTL_CLEAR:
        s->pwrctl_status &= ~value;
        msm8992_sdcc_update_irq(s);
        break;
    case SDCC_PWRCTL_CTL:
        s->pwrctl_ctl = value;
        if (value & SDCC_PWR_BUS_ON_OFF_SUCC) {
            s->pwrctl_status &= ~(SDCC_PWR_BUS_ON | SDCC_PWR_BUS_OFF);
        }
        if (value & SDCC_PWR_IO_SIG_SUCC) {
            s->pwrctl_status &= ~(SDCC_PWR_IO_SIG_LOW | SDCC_PWR_IO_SIG_HIGH);
        }
        msm8992_sdcc_update_irq(s);
        break;
    default:
        break;
    }
}

/*
 * One ops vector per register: the regions are a single dword each, so the
 * offset inside the region is always zero and only the base tells which
 * register was touched.
 */
#define SDCC_CORE_OPS(name, base)                                            \
    static uint64_t name##_read(void *opaque, hwaddr offset,               \
                                 unsigned size)                             \
    {                                                                       \
        return msm8992_sdcc_core_read(opaque, (base) + offset, size);       \
    }                                                                       \
    static void name##_write(void *opaque, hwaddr offset, uint64_t value,  \
                             unsigned size)                                 \
    {                                                                       \
        msm8992_sdcc_core_write(opaque, (base) + offset, value, size);     \
    }                                                                       \
    static const MemoryRegionOps name##_ops = {                             \
        .read = name##_read,                                                \
        .write = name##_write,                                              \
        .endianness = DEVICE_LITTLE_ENDIAN,                                 \
        .valid.min_access_size = 4,                                         \
        .valid.max_access_size = 4,                                         \
    }

SDCC_CORE_OPS(msm8992_sdcc_version, SDCC_MCI_VERSION);
SDCC_CORE_OPS(msm8992_sdcc_mode, SDCC_MCI_HC_MODE);
SDCC_CORE_OPS(msm8992_sdcc_status, SDCC_PWRCTL_STATUS);
SDCC_CORE_OPS(msm8992_sdcc_mask, SDCC_PWRCTL_MASK);
SDCC_CORE_OPS(msm8992_sdcc_clear, SDCC_PWRCTL_CLEAR);
SDCC_CORE_OPS(msm8992_sdcc_ctl, SDCC_PWRCTL_CTL);

static void msm8992_sdcc_reset_hold(Object *obj, ResetType type)
{
    Msm8992SdccState *s = MSM8992_SDCC(obj);

    memset(s->vendor_regs, 0, sizeof(s->vendor_regs));
    s->hc_mode = 0;
    s->pwrctl_status = 0;
    s->pwrctl_mask = 0;
    s->pwrctl_ctl = 0;
    msm8992_sdcc_update_irq(s);
}

static void msm8992_sdcc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Msm8992SdccState *s = MSM8992_SDCC(dev);

    sysbus_init_irq(sbd, &s->irq);

    /*
     * The SDCC core registers share their window with the legacy MCI model
     * of the retail SBL1, so they are mapped as single dwords: only those
     * four bytes per register shadow the MCI.  The machine maps the MCI
     * first, and a region added later wins over an earlier one for the
     * bytes they overlap.
     */
    memory_region_init_io(&s->core_version, OBJECT(s), &msm8992_sdcc_version_ops,
                          s, "sdcc-mci-version", 4);
    memory_region_init_io(&s->core_mode, OBJECT(s), &msm8992_sdcc_mode_ops, s,
                          "sdcc-mci-hc-mode", 4);
    memory_region_init_io(&s->core_status, OBJECT(s), &msm8992_sdcc_status_ops, s,
                          "sdcc-pwrctl-status", 4);
    memory_region_init_io(&s->core_mask, OBJECT(s), &msm8992_sdcc_mask_ops, s,
                          "sdcc-pwrctl-mask", 4);
    memory_region_init_io(&s->core_clear, OBJECT(s), &msm8992_sdcc_clear_ops, s,
                          "sdcc-pwrctl-clear", 4);
    memory_region_init_io(&s->core_ctl, OBJECT(s), &msm8992_sdcc_ctl_ops, s,
                          "sdcc-pwrctl-ctl", 4);

    sysbus_init_mmio(sbd, &s->core_version);
    sysbus_init_mmio(sbd, &s->core_mode);
    sysbus_init_mmio(sbd, &s->core_status);
    sysbus_init_mmio(sbd, &s->core_mask);
    sysbus_init_mmio(sbd, &s->core_clear);
    sysbus_init_mmio(sbd, &s->core_ctl);

    /* the SDHCI vendor registers, right behind the standard SDHC ones */
    memory_region_init_io(&s->vendor, OBJECT(s), &msm8992_sdcc_vendor_ops, s,
                          "sdhci-vendor", SDHCI_VENDOR_SIZE);
    sysbus_init_mmio(sbd, &s->vendor);
}

static void msm8992_sdcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Qualcomm SDCC core and SDHCI vendor registers";
    dc->realize = msm8992_sdcc_realize;
    rc->phases.hold = msm8992_sdcc_reset_hold;
}

static const TypeInfo msm8992_sdcc_types[] = {
    {
        .name           = TYPE_MSM8992_SDCC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msm8992SdccState),
        .class_init     = msm8992_sdcc_class_init,
    },
};

DEFINE_TYPES(msm8992_sdcc_types)
