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
    uint64_t upd_bm[GCC_REGS_WORDS / 64 + 1]; /* last write had bit0=1 */
} Msm8992GccState;

/*
 * Clock-control blocks driven by the stock (pre-8974) ClockLib that the
 * retail SBL1 uses.  Their +0x00 register is the "branch control" word:
 *   bit[2:0]  on/off request (the driver writes 2 -> 6 -> 7)
 *   bit30     locked  - set by the hardware, the driver polls for it
 *   bit31     busy    - set by the hardware while a switch is in flight
 *                       (the driver polls for it to fall)
 * The RCG-style model below (bit31 = lock) is the *newer* ClockLib's view
 * and does not match what this SBL1 expects, so the control blocks get
 * their own status semantics.
 */
static const uint32_t msm8992_gcc_ctrl_blocks[] = {
    0x000, 0x040, 0x080, 0x0C0, 0x1000, 0x1DC0,
};

/*
 * Request registers: the firmware writes the request bit and then polls
 * for the hardware to clear it again.  Writes with bit0 set are already
 * covered by the RCG rule below (bit0 reads back as 0); these need an
 * explicit mask.
 */
static const struct {
    uint32_t off;
    uint32_t mask;
} msm8992_gcc_request_regs[] = {
    { 0x0F10, 0x00000001 },   /* DFS mux request, bit 0      */
    { 0x116C, 0x00000080 },   /* branch enable request, bit 7 */
    { 0x1190, 0x00000001 },   /* branch enable request, bit 0 */
};

static bool msm8992_gcc_is_ctrl_block(uint32_t off)
{
    for (size_t i = 0; i < ARRAY_SIZE(msm8992_gcc_ctrl_blocks); i++) {
        if (off >= msm8992_gcc_ctrl_blocks[i] &&
            off < msm8992_gcc_ctrl_blocks[i] + 0x20) {
            return true;
        }
    }
    return false;
}

static uint32_t msm8992_gcc_ctrl_base(uint32_t off)
{
    for (size_t i = 0; i < ARRAY_SIZE(msm8992_gcc_ctrl_blocks); i++) {
        if (off >= msm8992_gcc_ctrl_blocks[i] &&
            off < msm8992_gcc_ctrl_blocks[i] + 0x20) {
            return msm8992_gcc_ctrl_blocks[i];
        }
    }
    return off;
}

static uint64_t msm8992_gcc_read(void *opaque, hwaddr offset, unsigned size)
{
    Msm8992GccState *s = MSM8992_GCC(opaque);
    uint32_t v;

    if (offset > MSM8992_GCC_SIZE - 4)
        return 0;
    {
        size_t word = offset >> 2;

        v = s->regs[word];
        /*
         * Clock-control blocks: report the hardware status the stock
         * ClockLib polls - "locked" (bit30) set, "busy" (bit31) clear.
         */
        if (msm8992_gcc_is_ctrl_block((uint32_t)offset)) {
            uint32_t base = msm8992_gcc_ctrl_base((uint32_t)offset);
            uint32_t r;

            /*
             * +0x00 status:
             *   bit30 - locked: set by the hardware, the driver polls
             *           for it after enabling a clock.
             *   bit31 - the enable request is running: the driver writes
             *           2/6 and then waits for this bit to rise.  The
             *           hardware raises it once the switch started, so
             *           it is set as soon as the driver asked for the
             *           clock (bit1 of the request word).
             */
            if (offset == base) {
                r = v | 0x40000000u;
                if (v & 0x2) {
                    r |= 0x80000000u;
                }
            } else if (offset == base + 0x04) {
                /* divider: plain storage, the driver compares it back */
                r = v;
            } else {
                /* UPDATE (bit0) is write-only, busy (bit31) falls */
                r = v & ~0x80000001u;
            }
            qemu_log_mask(LOG_UNIMP, "gcc: read 0x%04" PRIx32 " = 0x%08"
                          PRIx32 " (ctrl)\n", (uint32_t)offset, r);
            return r;
        }
        /*
         * Request bits are cleared by the hardware once the switch is
         * done; the firmware waits for them to fall.
         */
        for (size_t i = 0; i < ARRAY_SIZE(msm8992_gcc_request_regs); i++) {
            if (offset == msm8992_gcc_request_regs[i].off) {
                v &= ~msm8992_gcc_request_regs[i].mask;
                break;
            }
        }
        /*
         * QC RCG semantics seen in SBL1's clock bring-up:
         *  - CMD_RCGR-class: software writes UPDATE=1(bit0), then polls
         *    `tbnz bit31` for the busy flag to FALL — the busy bit is
         *    never set by our model, so the poll completes at once.
         *  - PLL_STATUS/GPLL-class: init sets config with bit0=0 and the
         *    firmware then polls for bit31=1 (LOCK). Our static model sets
         *    the lock on the first write and reports it always.
         * In untouched state a register reads as "locked" (0x80000000) —
         * matches a quiescent, working clock tree.
         *
         * Note: a write never changes what a register reads back, except
         * for the explicit request bits above.  The stock ClockLib compares
         * the divider register (clock control block + 0x4) against the value
         * it programmed, so clearing bit0 on read would break the match.
         */
        if (s->upd_bm[word / 64] & (1ULL << (word & 63))) {
            return v & ~0x80000001u;
        }
        if (!(s->hot_bm[word / 64] & (1ULL << (word & 63)))) {
            return v | 0x80000000u;
        }
        return v;
    }
}

static void msm8992_gcc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Msm8992GccState *s = MSM8992_GCC(opaque);
    size_t word;
    if (offset > MSM8992_GCC_SIZE - 4)
        return;
    word = offset >> 2;
    s->regs[word] = value;
    s->hot_bm[word / 64] |= 1ULL << (word & 63);
    qemu_log_mask(LOG_UNIMP, "gcc: write 0x%04" HWADDR_PRIx " = 0x%08"
                  PRIx64 "\n", offset, value);
    if (value & 1) {
        s->upd_bm[word / 64] |= 1ULL << (word & 63);
    } else {
        s->upd_bm[word / 64] &= ~(1ULL << (word & 63));
    }
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
    memset(s->upd_bm, 0, sizeof(s->upd_bm));
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
