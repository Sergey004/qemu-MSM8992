/*
 * Qualcomm SDCC glue for the msm8992 machine.
 *
 * The Lumia 950 UEFI (Lumia950XLPkg/Driver/SdhciMMCHSDxe) drives the SD
 * controller of the phone through a single register bank at 0xF9824900:
 * standard SDHC registers (0x00-0xFF) plus Qualcomm vendor registers
 * at 0x100..0x1b8 and 0xa1c.
 *
 * This device is a container (like aspeed_sdhci) that provides a single
 * 0xC00-byte register window containing:
 *   - generic-sdhci child at offset 0x000 (standard SDHC registers)
 *   - vendor registers at offset 0x100..0xBFF (Qualcomm extensions)
 *
 * It also provides the SDCC core registers at 0xF9824000 (MCI_VERSION,
 * SDCC_MCI_HC_MODE, PWRCTL status/mask/clear/ctl) and the power control
 * state machine. The interrupt line (INTID 170) carries both the card
 * interrupt of the SDHCI block and the power event.
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
#include "hw/sd/sdhci.h"

#define TYPE_MSM8992_SDCC "msm8992-sdcc"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992SdccState, MSM8992_SDCC)

/* SDCC core registers, offset from the core base (0xF9824000) */
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
 * Qualcomm SDHCI vendor register offsets (from SDHCI base + 0x100)
 * Based on sdhci_msm.h
 */
#define SDCC_DLL_CONFIG_REG          0x100
#define SDCC_VENDOR_SPECIFIC_FUNC    0x10C
#define SDCC_REG_DLL_STATUS          0x108
#define SDCC_CDC_DDR200_CFG          0x184
#define SDCC_VENDOR_SPEC_CSR_CDC_CFG 0x178
#define SDCC_CSR_CDC_CTRL_CFG0       0x130
#define SDCC_CSR_CDC_CTRL_CFG1       0x134
#define SDCC_CSR_CDC_CAL_TIMER_CFG0  0x138
#define SDCC_CSR_CDC_CAL_TIMER_CFG1  0x13C
#define SDCC_CSR_CDC_REFCOUNT_CFG    0x140
#define SDCC_CSR_CDC_COARSE_CAL_CFG  0x144
#define SDCC_CSR_CDC_DELAY_CFG       0x150
#define SDCC_CDC_OFFSET_CFG          0x14C
#define SDCC_CDC_SLAVE_DDA_CFG       0x160
#define SDCC_CSR_CDC_STATUS0         0x164

#define SDCC_HC_VENDOR_SPECIFIC_FUNC3 0x1B0
#define SDCC_HC_REG_DLL_CONFIG_2     0x1B4
#define SDCC_HC_REG_DDR_CONFIG       0x1B8

#define SDCC_HC_VENDOR_SPECIFIC_CAPABILITIES0 0x11C

/* DLL bits */
#define SDCC_DLL_LOCK_STAT           (1u << 7)

/* VENDOR_SPECIFIC_FUNC bits */
#define HC_IO_PAD_PWR_SWITCH_EN      (1u << 15)
#define HC_IO_PAD_PWR_SWITCH         (1u << 16)

/* DLL_CONFIG bits */
#define SDCC_DLL_PWR_SAVE_EN         (1u << 1)
#define SDCC_DLL_EN                  (1u << 16)
#define SDCC_DLL_CDR_EN              (1u << 17)
#define SDCC_DLL_CLK_OUT_EN          (1u << 18)
#define SDCC_DLL_CDR_EXT_EN          (1u << 19)
#define SDCC_DLL_CLOCK_DISABLE       (1u << 21)
#define SDCC_DLL_PDN_EN              (1u << 29)
#define SDCC_DLL_RESET_EN            (1u << 30)

/* DLL_CONFIG_2 bits */
#define SDCC_FLL_CYCLE_CNT           (1u << 18)
#define DDR_CAL_EN                   (1u << 0)
#define DDR_DLL_LOCK_JDR             (1u << 11)
#define PWRSAVE_DLL                  (1u << 3)

/* DDR_CONFIG_VAL from sdhci_msm.c */
#define DDR_CONFIG_VAL               0x80040853

/*
 * The vendor register window at offset 0x100 of the SDHCI bank.
 * Covers 0x100..0xBFF (0xC00 total from SDHCI base).
 */
#define SDHCI_VENDOR_BASE  0x100
#define SDHCI_VENDOR_SIZE  0xC00

/* Bound the debug log: the guest touches these registers a lot. */
#define SDCC_TRACE_MAX 256

struct Msm8992SdccState {
    SysBusDevice parent_obj;

    /* Embedded generic-sdhci child (initialized with object_initialize_child) */
    SDHCIState sdhci;

    MemoryRegion container;      /* combined SDHCI + vendor window (0xD00 bytes) */
    MemoryRegion vendor;         /* SDHCI vendor registers */
    MemoryRegion core_version;   /* MCI_VERSION at 0xF9824050 */
    MemoryRegion core_mode;      /* SDCC_MCI_HC_MODE at 0xF9824078 */
    MemoryRegion core_status;    /* PWRCTL status at 0xF98240DC */
    MemoryRegion core_mask;      /* PWRCTL mask at 0xF98240E0 */
    MemoryRegion core_clear;     /* PWRCTL clear at 0xF98240E4 */
    MemoryRegion core_ctl;       /* PWRCTL ctl at 0xF98240E8 */

    uint32_t vendor_regs[SDHCI_VENDOR_SIZE / 4];

    uint32_t hc_mode;
    uint32_t pwrctl_status;
    uint32_t pwrctl_mask;
    uint32_t pwrctl_ctl;
    unsigned  trace_count;

    qemu_irq irq;
};

/* Forward declarations with __attribute__((used)) */
static void msm8992_sdcc_realize(DeviceState *dev, Error **errp) __attribute__((used));
static void msm8992_sdcc_update_irq(Msm8992SdccState *s) __attribute__((used));
static void msm8992_sdcc_sdhci_irq(void *opaque, int n, int level) __attribute__((used));
static void msm8992_sdcc_trace(Msm8992SdccState *s, const char *fmt, ...) __attribute__((used))
    G_GNUC_PRINTF(2, 3);
static uint64_t msm8992_sdcc_vendor_read(void *opaque, hwaddr offset,
                                         unsigned size) __attribute__((used));
static void msm8992_sdcc_vendor_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size) __attribute__((used));
static uint64_t msm8992_sdcc_core_read(void *opaque, hwaddr offset,
                                       unsigned size) __attribute__((used));
static void msm8992_sdcc_core_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size) __attribute__((used));

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
    /* Combine SDHCI card interrupt (from child) and power event. */
    bool sdhci_irq = (s->sdhci.norintsts & s->sdhci.norintsigen) ||
                     (s->sdhci.errintsts & s->sdhci.errintsigen);
    qemu_set_irq(s->irq, sdhci_irq || ((s->pwrctl_status & s->pwrctl_mask) != 0));
}

static void msm8992_sdcc_sdhci_irq(void *opaque, int n, int level)
{
    Msm8992SdccState *s = opaque;
    msm8992_sdcc_update_irq(s);
}

static uint64_t msm8992_sdcc_vendor_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    Msm8992SdccState *s = opaque;

    if (offset + size > SDHCI_VENDOR_SIZE) {
        return 0;
    }

    uint32_t reg_offset = offset + SDHCI_VENDOR_BASE;

    /* Handle special registers that need specific behavior */
    switch (reg_offset) {
    case SDCC_REG_DLL_STATUS:
        /* DLL_STATUS: return DLL_LOCK_STAT (bit 7) set after DLL initialization */
        return s->vendor_regs[offset / 4] | SDCC_DLL_LOCK_STAT;

    case SDCC_HC_VENDOR_SPECIFIC_CAPABILITIES0:
        /* Return capabilities with 1.8V and 3.0V support */
        return s->vendor_regs[offset / 4];

    default:
        return s->vendor_regs[offset / 4];
    }
}

static void msm8992_sdcc_vendor_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    Msm8992SdccState *s = opaque;

    if (offset + size > SDHCI_VENDOR_SIZE) {
        return;
    }

    uint32_t reg_offset = offset + SDHCI_VENDOR_BASE;

    msm8992_sdcc_trace(s, "vendor[0x%x] = 0x%x\n",
                        (unsigned)reg_offset, (unsigned)value);

    /* Handle special registers */
    switch (reg_offset) {
    case SDCC_DLL_CONFIG_REG:
        /* DLL_CONFIG: when DLL_EN (bit 16) is set, mark DLL as locked */
        s->vendor_regs[offset / 4] = value;
        if (value & SDCC_DLL_EN) {
            s->vendor_regs[SDCC_REG_DLL_STATUS / 4] |= SDCC_DLL_LOCK_STAT;
        }
        break;

    case SDCC_VENDOR_SPECIFIC_FUNC:
        s->vendor_regs[offset / 4] = value;
        break;

    case SDCC_HC_REG_DLL_CONFIG_2:
        s->vendor_regs[offset / 4] = value;
        /* When DDR_CAL_EN is set, set DDR_DLL_LOCK_JDR in DLL_STATUS */
        if (value & DDR_CAL_EN) {
            s->vendor_regs[SDCC_REG_DLL_STATUS / 4] |= DDR_DLL_LOCK_JDR;
        }
        break;

    case SDCC_HC_REG_DDR_CONFIG:
        s->vendor_regs[offset / 4] = value;
        break;

    case SDCC_HC_VENDOR_SPECIFIC_FUNC3:
        s->vendor_regs[offset / 4] = value;
        break;

    case SDCC_HC_VENDOR_SPECIFIC_CAPABILITIES0:
        s->vendor_regs[offset / 4] = value;
        break;

    default:
        s->vendor_regs[offset / 4] = value;
        break;
    }
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

static void msm8992_sdcc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Msm8992SdccState *s = MSM8992_SDCC(dev);

    /* Create our container memory region (0xD00 bytes: SDHCI 0x100 + vendor 0xC00) */
    memory_region_init_io(&s->container, OBJECT(dev), NULL, s,
                          "msm8992-sdcc-container", SDHCI_VENDOR_SIZE + 0x100);

    /* Initialize the embedded generic-sdhci child (aspeed_sdhci pattern) */
    object_initialize_child(OBJECT(dev), "sdhci", &s->sdhci, TYPE_SYSBUS_SDHCI);
    object_property_set_int(OBJECT(&s->sdhci), "sd-spec-version", 2, errp);
    if (*errp) {
        return;
    }

    /* Realize the embedded SDHCI (aspeed_sdhci pattern) */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdhci), errp)) {
        return;
    }

    /* Connect the SDHCI child's IRQ to our internal handler */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdhci), 0,
                       qemu_allocate_irqs(msm8992_sdcc_sdhci_irq, s, 1)[0]);

    /* Add the SDHCI child's iomem at offset 0 of our container */
    memory_region_add_subregion(&s->container, 0, &s->sdhci.iomem);

    /* Create vendor registers region and add at offset 0x100 */
    memory_region_init_io(&s->vendor, OBJECT(dev), &msm8992_sdcc_vendor_ops, s,
                          "sdhci-vendor", SDHCI_VENDOR_SIZE);
    memory_region_add_subregion(&s->container, SDHCI_VENDOR_BASE, &s->vendor);

    /* Map our combined container at the SDHCI base address (0xF9824900) */
    sysbus_init_mmio(sbd, &s->container);

    /* SDCC core registers (single dwords, overlap with legacy MCI at 0xF9824000) */
    memory_region_init_io(&s->core_version, OBJECT(dev), &msm8992_sdcc_version_ops,
                          s, "sdcc-mci-version", 4);
    memory_region_init_io(&s->core_mode, OBJECT(dev), &msm8992_sdcc_mode_ops, s,
                          "sdcc-mci-hc-mode", 4);
    memory_region_init_io(&s->core_status, OBJECT(dev), &msm8992_sdcc_status_ops, s,
                          "sdcc-pwrctl-status", 4);
    memory_region_init_io(&s->core_mask, OBJECT(dev), &msm8992_sdcc_mask_ops, s,
                          "sdcc-pwrctl-mask", 4);
    memory_region_init_io(&s->core_clear, OBJECT(dev), &msm8992_sdcc_clear_ops, s,
                          "sdcc-pwrctl-clear", 4);
    memory_region_init_io(&s->core_ctl, OBJECT(dev), &msm8992_sdcc_ctl_ops, s,
                          "sdcc-pwrctl-ctl", 4);

    sysbus_init_mmio(sbd, &s->core_version);
    sysbus_init_mmio(sbd, &s->core_mode);
    sysbus_init_mmio(sbd, &s->core_status);
    sysbus_init_mmio(sbd, &s->core_mask);
    sysbus_init_mmio(sbd, &s->core_clear);
    sysbus_init_mmio(sbd, &s->core_ctl);

    /* Initialize vendor registers with proper defaults */
    s->vendor_regs[SDCC_VENDOR_SPECIFIC_FUNC / 4] = 0xA1C;
    s->vendor_regs[SDCC_REG_DLL_STATUS / 4] = 0;
    s->vendor_regs[SDCC_HC_VENDOR_SPECIFIC_CAPABILITIES0 / 4] = 0x40000000;

    /* Our single IRQ output combines SDHCI card IRQ and power event */
    sysbus_init_irq(sbd, &s->irq);
}

static void msm8992_sdcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Qualcomm SDCC core, SDHCI, and vendor registers";
    dc->realize = msm8992_sdcc_realize;
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