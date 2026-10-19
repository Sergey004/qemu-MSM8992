/*
 * Minimal Qualcomm SPMI PMIC Arbiter (v2-layout) stub for the msm8992 machine.
 *
 * Retail SBL1 drives PMIC through the arbiter at 0xFC4C0000. Two phases:
 *
 *  1. Arbiter init: PPID->channel mapping tables (0xA000 block) are written,
 *     channel owner regs programmed, CHn IRQ_STATUS (+0xF808 + ch*0x80) polled
 *     for TRAN_DONE. Reads-as-zero never complete that poll, which used to be
 *     fatal ("SPMI Fatal: SPMI bus busy").
 *
 *  2. PMIC transactions (decoded from the retail SBL1 spmi driver):
 *     WDATA0/1 at arb + ch*0x80 + 0xF810/0xF814 hold outbound bytes,
 *     CMD word at +0xF800:
 *        bits [3:0]  byte count - 1
 *        bits [19:4] 16-bit PMIC address (periph<<8 | reg)
 *        bits [24:20] slave id
 *        bit  [26]   priority
 *        bit  [27]   opcode: 0 = EXT_WRITE, 1 = EXT_READ
 *     RDATA0/1 at +0xF818/+0xF81C, IRQ_STATUS at +0xF808
 *     (bit0 TRAN_DONE expected set, bit1 failed clear).
 *
 * The fake bus carries one primary PMIC on slave 0 emulating PM8994:
 *  - REVID peripheral 0x100: TYPE=0x51 'Q', SUBTYPE=9 (retail SBL1 maps
 *    model 9 to its /boot/pm8994 driver, the Lumia 950 primary PMIC)
 *  - PON peripheral 0x800: PERPH_TYPE=1 (PM_HW_MODULE_PON)
 *  - PBS peripheral 0x7000: PERPH_TYPE=0x16 (PM_HW_MODULE_PBS) plus a RAM
 *    echo model: writes to 0x7048 are buffered and read back through 0x704c
 *    (the retail PBS driver writes PBS RAM words and verifies the readback,
 *    failing with PM err 0x27 on mismatch).
 * All other slaves/peripherals read as 0, so detect loops treat them as
 * absent.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"

#define TYPE_MSM8992_SPMI "msm8992-spmi"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992SpmiState, MSM8992_SPMI)

#define SPMI_SIZE         0x10000
#define SPMI_CH_BASE      0xF800   /* channel blocks at 0xF800 + ch*0x80 */
#define SPMI_CH_STRIDE    0x80
#define SPMI_NUM_CH       8
#define CH_REG_CMD        0x00
#define CH_REG_CMD_ST     0x04
#define CH_REG_IRQ_ST     0x08
#define CH_REG_WDATA0     0x10
#define CH_REG_WDATA1     0x14
#define CH_REG_RDATA0     0x18
#define CH_REG_RDATA1     0x1C

/* fake PM8994 register map (slave 0) */
#define PMIC_SID_PRIMARY  0
#define MSM8992_PMIC_SLOTS 2       /* PM8994 (sid 0) + PMI8994 (sid 2) */
#define PMIC_REG_DIG_MINOR 0x100
#define PMIC_REG_DIG_MAJOR 0x101
#define PMIC_REG_ANA_MINOR 0x102
#define PMIC_REG_ANA_MAJOR 0x103
#define PMIC_REG_TYPE      0x104
#define PMIC_REG_SUBTYPE   0x105
#define PMIC_TYPE_QC       0x51
#define PMIC_MODEL_PM8994  0x09   /* retail SBL1 table: 9 -> /boot/pm8994 */
#define PMIC_REG_PON_DIGM  0x801   /* DIG_MAJOR checked for 0 (warm boot) */
#define PMIC_REG_PON_TYPE  0x804   /* PERPH_TYPE: PM_HW_MODULE_PON = 1 */
#define PMIC_REG_PON_SUB   0x805
#define PMIC_REG_PBS_TYPE  0x7004  /* PERPH_TYPE: PM_HW_MODULE_PBS = 0x16 */
#define PMIC_REG_PBS_SUB   0x7005
#define PMIC_REG_PBS_MODE  0x7041  /* RAM access mode (write dir<<6) */
#define PMIC_REG_PBS_ADDR  0x7042  /* RAM address, 2 bytes */
#define PMIC_REG_PBS_WDATA 0x7048  /* RAM write port */
#define PMIC_REG_PBS_RDATA 0x704C  /* RAM read port (echo of last write) */
/* PMI8994 status registers the OEM/charger config path polls while
 * waiting for the device to become ready (bit 0 = "ready"). */
#define PMIC_REG_CHG_STAT1 0x4410
#define PMIC_REG_CHG_STAT2 0x4440
/* Charger status: the retail battery driver reads this byte twice and
 * decodes it twice (bits 3..5 = "shutdown reason", bits 0..2 = "charger
 * state"), so both fields must be non-zero for the driver to continue
 * to UEFI instead of aborting with "No charger shut down". */
#define PMIC_REG_CHG_STAT3 0x130D

typedef struct Msm8992SpmiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[SPMI_SIZE / 4];
    bool tran_done[SPMI_NUM_CH];
    uint32_t rdata[SPMI_NUM_CH][2];
    uint32_t wdata[SPMI_NUM_CH][2];
    /* PBS RAM: addressed via 0x7042/0x7043, data via 0x7048 (write) and
     * 0x704c (read), both auto-incrementing a shared cursor. One RAM per
     * PMIC (index = pmic slot), because the PSI sequencer programs and
     * verifies PBS RAM on every present PMIC. */
    uint8_t pbs_ram[MSM8992_PMIC_SLOTS][0x10000];
    uint16_t pbs_addr[MSM8992_PMIC_SLOTS];
} Msm8992SpmiState;

/* PMIC slot index for an SPMI slave id (only the PMIC slaves we model) */
static int msm8992_pmic_slot(unsigned sid)
{
    if (sid == 0) {
        return 0;
    }
    if (sid == 2) {
        return 1;
    }
    return -1;
}

/* Peripheral page probing: retail SBL1 walks every 0x100-aligned page of a
 * PMIC and requires PERPH_TYPE (page+4) and SUBTYPE (page+5) to be nonzero
 * to consider the page present (FUN_fec4051c). The type also selects the
 * per-page init; type 0x11 (MPP) triggers the per-PMIC device registration
 * (FUN_fec267b0 -> FUN_fec3d1f4), so a synthetic MPP page guarantees the
 * device object exists for the railway/rail drivers. */
#define PMIC_REG_PERPH_TYPE  0x04
#define PMIC_REG_PERPH_SUBTYPE 0x05
#define PMIC_TYPE_REVID      0x51   /* 'Q' common/REVID page */
#define PMIC_TYPE_PON        0x01
#define PMIC_TYPE_PBS        0x16
#define PMIC_TYPE_MPP        0x11
#define PMIC_TYPE_ADC        0x08   /* generic peripheral, no registrar */
#define PMIC_TYPE_CHARGER    0x02   /* registers the per-PMIC rail device */
#define PMIC_TYPE_FG         0x0d   /* registers the per-PMIC charger/OEM device */

static uint8_t msm8992_pmic_page_type(unsigned sid, unsigned page)
{
    switch (page) {
    case 0x0100: return PMIC_TYPE_REVID;
    case 0x0800: return PMIC_TYPE_PON;
    case 0x1100: return PMIC_TYPE_MPP;   /* synthetic: device registrar */
    case 0x1200: return PMIC_TYPE_ADC;   /* synthetic: generic peripheral */
    case 0x1300:
        /* CHARGER only on the companion PMI8994 (slave 2): this is the path
         * that registers the per-PMIC railway/rail device (FUN_fec3ebe4). */
        return (sid == 2) ? PMIC_TYPE_CHARGER : 0;
    case 0x1400:
        /* Fuel-gauge class peripheral on the companion PMI8994: the retail
         * per-page dispatch routes type 0x0D to the registrar that creates
         * the per-PMIC device object used by the OEM/charger config path
         * (pm_driver_init -> FUN_fec24b40). */
        return (sid == 2) ? PMIC_TYPE_FG : 0;
    case 0x7000: return PMIC_TYPE_PBS;
    default:     return 0;
    }
}

/* PM8994 (slave 0) and its companion PMI8994 (slave 2) are present; see
 * msm8992_pmic_slot() for the slot mapping. */

static void msm8992_pmic_write_bytes(Msm8992SpmiState *s, unsigned sid,
                                     unsigned addr, const uint8_t *bytes,
                                     unsigned cnt)
{
    unsigned i;
    int slot = msm8992_pmic_slot(sid);

    if (slot < 0) {
        return;                     /* secondary PMICs absent */
    }
    for (i = 0; i < cnt; i++) {
        switch (addr + i) {
        case PMIC_REG_PBS_ADDR:
            s->pbs_addr[slot] = (s->pbs_addr[slot] & 0xff00) | bytes[i];
            break;
        case PMIC_REG_PBS_ADDR + 1:
            s->pbs_addr[slot] = (s->pbs_addr[slot] & 0x00ff) | (bytes[i] << 8);
            break;
        default:
            break;
        }
    }
    if (addr == PMIC_REG_PBS_WDATA) {
        for (i = 0; i < cnt; i++) {
            s->pbs_ram[slot][s->pbs_addr[slot]] = bytes[i];
            s->pbs_addr[slot]++;
        }
    }
    /* all other registers accept writes silently */
}

static uint8_t msm8992_pmic_read_byte(Msm8992SpmiState *s, unsigned sid,
                                      unsigned addr)
{
    unsigned page = addr & 0xFF00;
    unsigned reg = addr & 0xFF;
    uint8_t ptype;
    int slot = msm8992_pmic_slot(sid);

    if (slot < 0) {
        return 0;               /* secondary PMICs absent */
    }
    if (addr >= PMIC_REG_PBS_RDATA && addr < PMIC_REG_PBS_RDATA + 4) {
        /* PBS RAM read: auto-incrementing shared cursor */
        uint8_t b = s->pbs_ram[slot][s->pbs_addr[slot]];
        s->pbs_addr[slot]++;
        return b;
    }

    /* peripheral presence/type block */
    ptype = msm8992_pmic_page_type(sid, page);
    if (reg == PMIC_REG_PERPH_TYPE) {
        return ptype;
    }
    if (reg == PMIC_REG_PERPH_SUBTYPE) {
        if (ptype == PMIC_TYPE_REVID) {
            return PMIC_MODEL_PM8994;   /* 9 -> /boot/pm8994 driver name */
        }
        return ptype ? 0x01 : 0;
    }

    switch (addr) {
    case PMIC_REG_DIG_MINOR: return 0x01;
    case PMIC_REG_DIG_MAJOR: return 0x02;
    case PMIC_REG_ANA_MINOR: return 0x01;   /* metal revision */
    case PMIC_REG_ANA_MAJOR: return 0x02;   /* all-layer revision */
    case PMIC_REG_PON_DIGM:  return 0x00;
    case PMIC_REG_CHG_STAT1: return 0x01;   /* "ready" bit polled by the driver */
    case PMIC_REG_CHG_STAT2: return 0x01;
    case PMIC_REG_CHG_STAT3: return 0x09;
    case PMIC_REG_PON_SUB:   return 0x08;
    case PMIC_REG_PBS_SUB:   return 0x01;
    default:                 return 0;
    }
}

static void msm8992_pmic_exec_cmd(Msm8992SpmiState *s, unsigned ch,
                                  uint32_t cmd)
{
    unsigned sid = (cmd >> 20) & 0xF;
    unsigned addr = (cmd >> 4) & 0xFFFF;
    unsigned cnt = (cmd & 0xF) + 1;
    bool is_read = (cmd >> 27) & 1;
    uint8_t bytes[8];
    int i;

    for (i = 0; i < cnt; i++) {
        if (is_read) {
            bytes[i] = msm8992_pmic_read_byte(s, sid, addr + i);
        } else {
            /* outbound bytes live in WDATA0/1 of the issuing channel */
            uint32_t w = (i < 4) ? s->wdata[ch][0] : s->wdata[ch][1];
            bytes[i] = (w >> ((i & 3) * 8)) & 0xff;
        }
    }
    if (!is_read) {
        msm8992_pmic_write_bytes(s, sid, addr, bytes, cnt);
    }
    s->rdata[ch][0] = ldl_le_p((uint32_t *)bytes);
    s->rdata[ch][1] = cnt > 4 ? ldl_le_p((uint32_t *)(bytes + 4)) : 0;
    s->tran_done[ch] = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "spmi: ch%u op=%s sid=%u addr=0x%03x cnt=%u d0=0x%08x d1=0x%08x\n",
                  ch, is_read ? "RD" : "WR", sid, addr, cnt,
                  is_read ? s->rdata[ch][0] : s->wdata[ch][0],
                  is_read ? s->rdata[ch][1] : s->wdata[ch][1]);
}

static uint64_t msm8992_spmi_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992SpmiState *s = MSM8992_SPMI(opaque);

    if (offset >= SPMI_CH_BASE &&
        offset < SPMI_CH_BASE + SPMI_NUM_CH * SPMI_CH_STRIDE) {
        unsigned ch = (offset - SPMI_CH_BASE) / SPMI_CH_STRIDE;
        unsigned reg = (offset - SPMI_CH_BASE) % SPMI_CH_STRIDE;

        switch (reg) {
        case CH_REG_IRQ_ST:
            return s->tran_done[ch] ? 1 : 0;
        case CH_REG_RDATA0:
            return s->rdata[ch][0];
        case CH_REG_RDATA1:
            return s->rdata[ch][1];
        case CH_REG_CMD_ST:
            return 0;               /* command finished, no error */
        default:
            return 0;
        }
    }
    return s->regs[offset / 4];     /* mapping tables read back writes */
}

static void msm8992_spmi_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Msm8992SpmiState *s = MSM8992_SPMI(opaque);

    if (offset >= SPMI_CH_BASE &&
        offset < SPMI_CH_BASE + SPMI_NUM_CH * SPMI_CH_STRIDE) {
        unsigned ch = (offset - SPMI_CH_BASE) / SPMI_CH_STRIDE;
        unsigned reg = (offset - SPMI_CH_BASE) % SPMI_CH_STRIDE;

        switch (reg) {
        case CH_REG_CMD:
            msm8992_pmic_exec_cmd(s, ch, (uint32_t)value);
            break;
        case CH_REG_WDATA0:
            s->wdata[ch][0] = (uint32_t)value;
            break;
        case CH_REG_WDATA1:
            s->wdata[ch][1] = (uint32_t)value;
            break;
        case CH_REG_IRQ_ST:
            s->tran_done[ch] = false;   /* IRQ clear */
            break;
        default:
            break;
        }
        return;
    }

    s->regs[offset / 4] = (uint32_t)value;

    /* Legacy init path: channel-owner programming in the 0xA000 block used
     * to complete the polled transaction during arbiter bring-up */
    if (offset >= 0xA000 && offset <= 0xA004) {
        s->tran_done[0] = true;
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
    dc->desc = "Qualcomm SPMI PMIC Arbiter stub (fake PM8994 + PBS RAM echo)";
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
