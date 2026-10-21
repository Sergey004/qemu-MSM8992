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

/* Defaults: Snapdragon 808 - A53 core 0 at 1.5 GHz, LPDDR3-1866 (933 MHz) */
#define MSM8992_GCC_PROP_CPU0_FREQ_KHZ  1500000
#define MSM8992_GCC_PROP_DDR_FREQ_KHZ    933000

typedef struct Msm8992GccState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[GCC_REGS_WORDS];
    uint64_t hot_bm[GCC_REGS_WORDS / 64 + 1];
    uint64_t upd_bm[GCC_REGS_WORDS / 64 + 1]; /* last write had bit0=1 */
    uint16_t read_cnt[GCC_REGS_WORDS];       /* for the repeated-read trace */
    uint32_t core0_freq_khz;                /* rate of CPU core 0 */
    uint32_t ddr_freq_khz;                  /* DRAM clock rate */
} Msm8992GccState;

/*
 * Rate registers.  The clock controller is the block that knows how fast the
 * clocks it produces actually run, so the two rates the firmware asks about
 * live here:
 *
 *   +0x1F00  CORE0_FREQ_KHZ  rate of CPU core 0 in kHz (read-only)
 *   +0x1F04  DDR_FREQ_KHZ    DRAM clock rate in kHz (read-only)
 *
 * Snapdragon 808 (MSM8992) values, as used by the retail firmware:
 *   - core 0 is a Cortex-A53 (cpu0..3 are A53, cpu4..5 are A57); the A53
 *     cluster runs at up to 1.5 GHz;
 *   - the DRAM is LPDDR3-1866, i.e. a 933 MHz clock.
 *
 * On real hardware the firmware learns these numbers from the RPM/AOP (over
 * DAL) and from the DRAM device itself; here the machine simply states them.
 */
#define GCC_CORE0_FREQ_KHZ  0x1F00
#define GCC_DDR_FREQ_KHZ    0x1F04

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

/*
 * Bring-up status registers.  The TrustZone firmware polls 0x0D00 until
 * bit0 is set (and 0x0D04 until it turns negative) before it continues
 * the boot; on real hardware the RPM reports "up" there.  The RPM itself
 * is not modelled, so the flag is simply reported as set.
 */
static const struct {
    uint32_t off;
    uint32_t value;
} msm8992_gcc_status_regs[] = {
    { 0x0D00, 0x00000001 },
    { 0x0D04, 0x00000000 },
};

/*
 * Branch control registers (CBCR).  The UEFI's clock library enables a
 * clock and then polls the CBCR until its branch state (bits 31:28) reads
 * "on" (0) or "on, no FSM" (2).  A generic register that was never written
 * reads back as 0x80000000 here, which would be state 8 and the poll would
 * never end, so the CBCRs report their branch state as off/on.
 */
static const uint32_t msm8992_gcc_branch_regs[] = {
    0x0280, 0x0284, 0x03C8, 0x03CC, 0x03D0, 0x03FC, 0x0484, 0x0488,
    0x04AC, 0x04C4, 0x04C8, 0x0504, 0x0508, 0x0544, 0x0548, 0x0584,
    0x0588, 0x05C4, 0x0644, 0x0648, 0x0684, 0x06C4, 0x06C8, 0x0704,
    0x0744, 0x0748, 0x0784, 0x07C4, 0x07C8, 0x0804, 0x0844, 0x0848,
    0x0884, 0x08C4, 0x08C8, 0x0904, 0x0944, 0x0984, 0x0988, 0x09C4,
    0x0A04, 0x0A08, 0x0A44, 0x0A84, 0x0A88, 0x0AC4, 0x0B04, 0x0B08,
    0x0B44, 0x0B84, 0x0B88, 0x0BC4, 0x0C04, 0x0C08, 0x0C44, 0x0CC4,
    0x0CCC, 0x0D04, 0x0D44, 0x0D84, 0x0D88, 0x0E04, 0x10C8, 0x11C0,
    0x1408, 0x140C, 0x1900, 0x1940, 0x1980, 0x1A84, 0x1AC8, 0x1ACC,
    0x1AD0, 0x1AD4, 0x1AD8, 0x1B48, 0x1B4C, 0x1B50, 0x1B54, 0x1B58,
    0x1D48, 0x1D4C, 0x1D50, 0x1D54, 0x1D58, 0x1D5C, 0x1D60, 0x1D64,
    0x1D7C,
};

static bool msm8992_gcc_is_branch_reg(uint32_t off)
{
    for (size_t i = 0; i < ARRAY_SIZE(msm8992_gcc_branch_regs); i++) {
        if (off == msm8992_gcc_branch_regs[i]) {
            return true;
        }
    }
    return false;
}

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

        /*
         * A firmware poll loop reads the same register over and over; report
         * an offset the third time it is read so the poll is visible in the
         * log without drowning it in single reads.
         */
        if (s->read_cnt[word] < 0xffff) {
            s->read_cnt[word]++;
            if (s->read_cnt[word] == 3) {
                qemu_log_mask(LOG_UNIMP, "gcc: repeated read 0x%04" HWADDR_PRIx
                              " = 0x%08" PRIx32 "\n", offset, s->regs[word]);
            }
        }

        v = s->regs[word];
        /*
         * The rate registers are read-only and always report the rate the
         * machine was configured with.
         */
        if (offset == GCC_CORE0_FREQ_KHZ) {
            return s->core0_freq_khz;
        }
        if (offset == GCC_DDR_FREQ_KHZ) {
            return s->ddr_freq_khz;
        }
        for (size_t i = 0; i < ARRAY_SIZE(msm8992_gcc_status_regs); i++) {
            if (offset == msm8992_gcc_status_regs[i].off) {
                return msm8992_gcc_status_regs[i].value;
            }
        }
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
         * Branch control: the branch state (bits 31:28) of a CBCR reads
         * "on" - the clock is running.
         */
        if (msm8992_gcc_is_branch_reg((uint32_t)offset)) {
            v &= ~0xF0000000u;
            qemu_log_mask(LOG_UNIMP, "gcc: read 0x%04" PRIx32 " = 0x%08"
                          PRIx32 " (branch)\n", (uint32_t)offset, v);
            return v;
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
    memset(s->read_cnt, 0, sizeof(s->read_cnt));
}

static void msm8992_gcc_instance_init(Object *obj)
{
    Msm8992GccState *s = MSM8992_GCC(obj);
    s->core0_freq_khz = MSM8992_GCC_PROP_CPU0_FREQ_KHZ;
    s->ddr_freq_khz   = MSM8992_GCC_PROP_DDR_FREQ_KHZ;
}

static void msm8992_gcc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_gcc_realize;
    device_class_set_legacy_reset(dc, msm8992_gcc_reset);
    dc->desc = "Qualcomm Global Clock Controller, benign register file";

    object_class_property_add_uint32_ptr(oc, "cpu0-freq-khz",
            offsetof(Msm8992GccState, core0_freq_khz),
            OBJ_PROP_FLAG_READWRITE);
    object_class_property_set_description(oc, "cpu0-freq-khz",
        "Rate of CPU core 0 in kHz (Snapdragon 808 A53: 1500000)");
    object_class_property_add_uint32_ptr(oc, "ddr-freq-khz",
            offsetof(Msm8992GccState, ddr_freq_khz),
            OBJ_PROP_FLAG_READWRITE);
    object_class_property_set_description(oc, "ddr-freq-khz",
        "DRAM clock rate in kHz (LPDDR3-1866: 933000)");
}

static const TypeInfo msm8992_gcc_info = {
    .name          = TYPE_MSM8992_GCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992GccState),
    .instance_init = msm8992_gcc_instance_init,
    .class_init    = msm8992_gcc_class_init,
};

static void msm8992_gcc_register_types(void)
{
    type_register_static(&msm8992_gcc_info);
}

type_init(msm8992_gcc_register_types)
