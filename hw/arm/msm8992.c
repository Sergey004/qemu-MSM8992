/*
 * Qualcomm MSM8992 (Snapdragon 808) machine — Lumia 950 "Talkman" (RM-1104)
 *
 * Stage-1 skeleton for booting the original Windows 10 Mobile stack
 * (see /research: RESEARCH.md §8). Memory map follows
 * Lumia950XLPkg:Include/Configuration/DeviceMemoryMap.h and
 * kernel_msm:arch/arm64/boot/dts/qcom/msm8992.dtsi (bullhead branch),
 * both verified against the retail FFU firmware ELF headers.
 *
 * Composition:
 *   - 2 clusters: 2x Cortex-A57 + 4x Cortex-A53 (big.LITTLE hexa-core)
 *   - GIC-400 (qcom,msm-qgic2 derivative) @0xF9000000/0xF9002000
 *   - 3 GiB DRAM @0x0 (0x0..0xBFFFFFFF); framebuffer is plain RAM @0x400000
 *   - DBG2 console: PL011 placeholder at the blsp1_uart2 window 0xF991E000
 *     (real hardware has qcom,msm-lsuart-v14; replace with an lsuart model
 *     in Stage 2+)
 *   - SDHCI SDC1 (eMMC) @0xF9824900, SPI 123 (= ACPI GSIV 155, QCOM24BF)
 *   - unimplemented-device stubs for Tier-1 blocks the firmware touches:
 *     GCC clocks, TLMM GPIO, SPMI/PMIC arbiter, RPM MSGRAM, QTimer mem frame,
 *     plus Tier-2 placeholders (KGSL MDP5/SMMU/OCMEM/DWC3) so stray accesses
 *     are logged instead of aborting the machine.
 *
 * PSCI: emulated conduit (SMC by default; -machine secure=on / virt=on add
 * EL3/EL2). Secondary CPUs start powered off, like on real hardware.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/core/boards.h"
#include "hw/cpu/cluster.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/intc/arm_gic.h"
#include "hw/misc/unimp.h"
#include "system/system.h"
#include "hw/sd/sdhci.h"
#include "hw/timer/qct-qtimer.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"

#define TYPE_MSM8992_MACHINE MACHINE_TYPE_NAME("msm8992")
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992MachineState, MSM8992_MACHINE)

/* ---- CPU topology: DT + MADT agree — A53 cluster is first ----
 * msm8992.dtsi: cpu@0..3 = cortex-a53 (MPIDR 0..3), cpu@4/5 = cortex-a57
 * (MPIDR 0x100/0x101). MADT.asl (Lumia950XLPkg) lists the same MPIDRs.
 * Boot CPU is cpu0 = A53. */
#define MSM8992_NUM_LITTLE  4
#define MSM8992_NUM_BIG     2
#define MSM8992_NUM_CPUS    (MSM8992_NUM_LITTLE + MSM8992_NUM_BIG)
#define MSM8992_BOOT_CPU    0          /* cpu0 = first A53 */

/* ---- Memory map (DeviceMemoryMap.h + msm8992.dtsi, verified) ---- */
#define MSM8992_DRAM_BASE   0x00000000ULL
#define MSM8992_DRAM_SIZE   (3ULL * GiB)        /* 0x0 .. 0xBFFFFFFF */
#define MSM8992_FB_BASE     0x00400000ULL       /* 1080x1920x32 window in DRAM */
#define MSM8992_SMEM_BASE   0x06A00000ULL       /* inside DRAM, 0x200000 bytes */
#define MSM8992_SMEM_SIZE   0x200000ULL
#define MSM8992_TARG_INFO   0xFE805000ULL       /* smem_targ_info_imem page (DT) */
#define MSM8992_IMEM_BASE   0xFE800000ULL       /* system IMEM (iomap.h) */
#define MSM8992_IMEM_SIZE   0x00100000ULL       /* covers cookie/target areas */

#define MSM8992_GIC_DIST    0xF9000000ULL
#define MSM8992_GIC_CPU     0xF9002000ULL
#define MSM8992_GIC_NUM_SPI 256                 /* qgic2: 256 SPI + 32 = 288 */

#define MSM8992_QTIMER_F0   0xF9021000ULL       /* armv7-timer-mem frame 0 */
#define MSM8992_GCC_BASE    0xFC400000ULL       /* qcom,gcc-8992, len 0x2000 */
#define MSM8992_TLMM_BASE   0xFD510000ULL       /* qcom,tlmm, len 0x4000 */
#define MSM8992_SPMI_BASE   0xFC4C0000ULL       /* arbiter; core window +0xF000 */
#define MSM8992_RPM_MSGRAM  0xFC428000ULL       /* len 0x4000 */
#define MSM8992_SDC1_CONF   0xF9824000ULL       /* vendor core cfg, len 0x800 */
#define MSM8992_SDC1_BASE   0xF9824900ULL       /* SDHCI regs, len 0x1A0 */
#define MSM8992_SDC2_BASE   0xF98A4900ULL       /* microSD (Tier 3, stubbed) */
#define MSM8992_UART2_BASE  0xF991E000ULL       /* blsp1_uart2 / DBG2 */
#define MSM8992_KGSL_BASE   0xFDB00000ULL       /* Adreno 418 KGSL core */
#define MSM8992_MDP5_BASE   0xFD900000ULL       /* display pipe, len 0xB4000 */
#define MSM8992_SMMU_MMU0   0xFD9B4000ULL       /* msm-smmu-v1, mdp_iommu */
#define MSM8992_OCMEM_BASE  0xFDD00000ULL       /* ocmem ctrl regs */
#define MSM8992_DWC3_BASE   0xF9200000ULL       /* ssusb/dwc3, len 0xFC000 */

#define MSM8992_UART2_IRQ   108                 /* DT SPI */
#define MSM8992_SDC1_IRQ    123                 /* DT SPI 123 == ACPI GSIV 155 */
#define MSM8992_SDC2_IRQ    125                 /* per bullhead DT sdhc_2 */

/* GIC PPI interrupt numbers (banked per-CPU) */
#define MSM8992_PHYS_TIMER_PPI  14
#define MSM8992_VIRT_TIMER_PPI  11
#define MSM8992_HYP_TIMER_PPI   10
#define MSM8992_SEC_TIMER_PPI   13
#define GIC_MAINTENANCE_PPI     9

struct Msm8992MachineState {
    MachineState parent_obj;

    ARMCPU cpu[MSM8992_NUM_CPUS];
    CPUClusterState little_cluster;   /* cluster 0: 4x A53 */
    CPUClusterState big_cluster;      /* cluster 1: 2x A57 */
    DeviceState *gic;
    DeviceState *sdc1;
    DeviceState *sdc2;
    struct arm_boot_info bootinfo;

    bool secure;                    /* provide EL3 (stock-TZ experiments) */
    bool virt;                      /* provide EL2 */
    bool fullchain;                 /* PBL-stub->SBL1->TZ->UEFI chain mode */
};

/* GIC PPI inputs sit after the SPI pins: index = num_spi + cpu*32 + INTID,
 * where INTID = 16 + ppi (arm_gic.c gic_set_irq input layout). */
static inline int msm8992_gic_ppi_line(int cpu_nr, int ppi)
{
    return MSM8992_GIC_NUM_SPI + cpu_nr * GIC_INTERNAL + GIC_NR_SGIS + ppi;
}

static void msm8992_create_cpus(Msm8992MachineState *s, MachineState *ms)
{
    unsigned int i;

    /* Only instantiate a cluster when at least one of its CPUs is enabled,
     * otherwise the realize-time assert in cpu-cluster fires. */
    if (ms->smp.cpus > 0) {
        object_initialize_child(OBJECT(s), "little-cluster",
                                &s->little_cluster, TYPE_CPU_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&s->little_cluster), "cluster-id", 0);
    }
    if (ms->smp.cpus > MSM8992_NUM_LITTLE) {
        object_initialize_child(OBJECT(s), "big-cluster", &s->big_cluster,
                                TYPE_CPU_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&s->big_cluster), "cluster-id", 1);
    }

    for (i = 0; i < ms->smp.cpus; i++) {
        Object *parent  = i < MSM8992_NUM_LITTLE
                          ? OBJECT(&s->little_cluster) : OBJECT(&s->big_cluster);
        const char *type = i < MSM8992_NUM_LITTLE
                          ? ARM_CPU_TYPE_NAME("cortex-a53")
                          : ARM_CPU_TYPE_NAME("cortex-a57");
        unsigned int cluster = i < MSM8992_NUM_LITTLE ? 0 : 1;
        unsigned int pos     = i < MSM8992_NUM_LITTLE ? i
                                                      : i - MSM8992_NUM_LITTLE;

        object_initialize_child(parent, "cpu[*]", &s->cpu[i], type);

        if (i != MSM8992_BOOT_CPU && false) {
            object_property_set_bool(OBJECT(&s->cpu[i]),
                                     "start-powered-off", true, &error_abort);
        }
        object_property_set_int(OBJECT(&s->cpu[i]), "mp-affinity",
                                (cluster << 8) | pos, &error_abort);
        object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                MSM8992_GIC_CPU, &error_abort);
        /* QC QTimer runs at 19.2 MHz on MSM8992 */
        object_property_set_uint(OBJECT(&s->cpu[i]), "cntfrq",
                                 19200000, &error_abort);
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3",
                                 s->secure, &error_abort);
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                 s->virt, &error_abort);
        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, &error_fatal)) {
            g_assert_not_reached();
        }
    }

    if (ms->smp.cpus > 0) {
        qdev_realize(DEVICE(&s->little_cluster), NULL, &error_fatal);
    }
    if (ms->smp.cpus > MSM8992_NUM_LITTLE) {
        qdev_realize(DEVICE(&s->big_cluster), NULL, &error_fatal);
    }
}

static void msm8992_create_gic(Msm8992MachineState *s, MachineState *ms)
{
    SysBusDevice *gicbusdev;
    unsigned int i;

    s->gic = qdev_new(gic_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 2);
    qdev_prop_set_uint32(s->gic, "num-cpu", ms->smp.cpus);
    /* num-irq counts the mandatory 32 internal lines as well */
    qdev_prop_set_uint32(s->gic, "num-irq", MSM8992_GIC_NUM_SPI + 32);
    qdev_prop_set_bit(s->gic, "has-security-extensions", s->secure);
    qdev_prop_set_bit(s->gic, "has-virtualization-extensions", s->virt);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, MSM8992_GIC_DIST);
    sysbus_mmio_map(gicbusdev, 1, MSM8992_GIC_CPU);
    if (s->virt) {
        sysbus_mmio_map(gicbusdev, 2, MSM8992_GIC_DIST + 0x8000); /* GICH */
        sysbus_mmio_map(gicbusdev, 3, MSM8992_GIC_DIST + 0xA000); /* GICV */
    }

    for (i = 0; i < ms->smp.cpus; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        qemu_irq irq;
        int n = ms->smp.cpus;

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));

        irq = qdev_get_gpio_in(s->gic,
                msm8992_gic_ppi_line(i, MSM8992_PHYS_TIMER_PPI));
        qdev_connect_gpio_out(cpudev, GTIMER_PHYS, irq);
        irq = qdev_get_gpio_in(s->gic,
                msm8992_gic_ppi_line(i, MSM8992_VIRT_TIMER_PPI));
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT, irq);
        irq = qdev_get_gpio_in(s->gic,
                msm8992_gic_ppi_line(i, MSM8992_HYP_TIMER_PPI));
        qdev_connect_gpio_out(cpudev, GTIMER_HYP, irq);
        irq = qdev_get_gpio_in(s->gic,
                msm8992_gic_ppi_line(i, MSM8992_SEC_TIMER_PPI));
        qdev_connect_gpio_out(cpudev, GTIMER_SEC, irq);

        if (s->virt) {
            irq = qdev_get_gpio_in(s->gic,
                    msm8992_gic_ppi_line(i, GIC_MAINTENANCE_PPI));
            sysbus_connect_irq(gicbusdev, i + 4 * n, irq);
        }
    }
}

/*
 * RPM clock-request state words.  The stock ClockLib writes an enable
 * request (bit1) and waits for the RPM to acknowledge it by setting
 * bit31; a finished request (bit0 set) is not acknowledged again.
 */
static uint64_t msm8992_rpm_clk_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t v = regs[offset >> 2];

    if ((offset == 0x1000 || offset == 0x5000) &&
        (v & 0x2) && !(v & 0x1)) {
        v |= 0x80000000u;
    }
    return v;
}

static void msm8992_rpm_clk_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    uint32_t *regs = opaque;

    regs[offset >> 2] = value;
}

static const MemoryRegionOps msm8992_rpm_clk_ops = {
    .read = msm8992_rpm_clk_read,
    .write = msm8992_rpm_clk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_create_uart(Msm8992MachineState *s, qemu_irq *gic_spi)
{
    /* DBG2 console: UARTDM (qcom,msm-lsuart-v14), minimal model in
     * hw/char/msm-uartdm.c (TX instant to chardev, RX FIFO, SR/ISR). */
    DeviceState *dev = qdev_new("msm-uartdm");
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, MSM8992_UART2_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, gic_spi[MSM8992_UART2_IRQ]);
}

static void msm8992_create_sdc1(Msm8992MachineState *s, qemu_irq *gic_spi,
                                bool fullchain)
{
    SysBusDevice *sbd;
    DeviceState *sdc1_host;

    /* eMMC boot device; attach the pre-flashed GPT image via -drive if=sd.
     * Two register banks on the same controller: legacy MCI (PL181-like)
     * @0xF9824000 (retail SBL1's path) and SDHCI @0xF9824900 (UEFI's path).
     * Only one host is instantiated per lane since they share the card. */
    if (fullchain) {
        sdc1_host = qdev_new("pl181");
        sbd = SYS_BUS_DEVICE(sdc1_host);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, MSM8992_SDC1_CONF);
        sysbus_connect_irq(sbd, 0, gic_spi[MSM8992_SDC1_IRQ]);
    } else {
        sdc1_host = s->sdc1 = qdev_new(TYPE_SYSBUS_SDHCI);
        sbd = SYS_BUS_DEVICE(sdc1_host);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, MSM8992_SDC1_BASE);
        sysbus_connect_irq(sbd, 0, gic_spi[MSM8992_SDC1_IRQ]);
        /* QC's SDHCI vendor registers extend past the generic 0x100 window */
        create_unimplemented_device("sdc1-pad", MSM8992_SDC1_BASE, 0x1000);
        create_unimplemented_device("sdhc1-vendor-cfg", MSM8992_SDC1_CONF,
                                    0x800);
    }
    create_unimplemented_device("sdhc2-vendor-cfg", 0xF98A4000ULL, 0x900);

    /* SDC1 endpoint is the on-board eMMC */
    {
        DriveInfo *dinfo = drive_get(IF_SD, 0, 0);

        if (dinfo) {
            DeviceState *card = qdev_new(TYPE_EMMC);

            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                    &error_fatal);
            if (!qdev_realize(DEVICE(card),
                              BUS(qdev_get_child_bus(sdc1_host, "sd-bus")),
                              &error_fatal))
                g_assert_not_reached();
        }
    }

    /* microSD slot (SDC2): real SDHCI — the user-visible SD card of the phone */
    {
        DriveInfo *dinfo;
        SysBusDevice *sbd2;

        s->sdc2 = qdev_new(TYPE_SYSBUS_SDHCI);
        sbd2 = SYS_BUS_DEVICE(s->sdc2);
        sysbus_realize_and_unref(sbd2, &error_fatal);
        sysbus_mmio_map(sbd2, 0, MSM8992_SDC2_BASE);
        sysbus_connect_irq(sbd2, 0, gic_spi[MSM8992_SDC2_IRQ]);
        create_unimplemented_device("sdc2-pad", MSM8992_SDC2_BASE, 0x1000);

        dinfo = drive_get(IF_SD, 0, 1);
        if (dinfo) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                    &error_fatal);
            if (!qdev_realize(DEVICE(card),
                              BUS(qdev_get_child_bus(s->sdc2, "sd-bus")),
                              &error_fatal))
                g_assert_not_reached();
        }
    }
}

#define MSM8992_MPM_BASE 0xFC4A1000ULL

/*
 * Low-memory scratch window (0x0..0x10000). Retail SBL1 derives the SPMI
 * arbiter MMIO base from devcfg, which our boot does not provide yet; with
 * a NULL base its channel writes land at 0xF810+ch*0x80 and abort. This
 * window catches them (logged) so PMIC init proceeds until devcfg support
 * lands. Nothing the machine loads occupies this range (stub is at
 * 0x10000, PBL shared data at 0x80000).
 */
static uint64_t msm8992_low_scratch_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "low-scratch: read  (size %d, offset 0x%04llx)\n",
                  size, (unsigned long long)addr);
    return 0;
}

static void msm8992_low_scratch_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "low-scratch: write (size %d, offset 0x%04llx, value 0x%08llx)\n",
                  size, (unsigned long long)addr, (unsigned long long)val);
}

static void msm8992_create_stubs(MachineState *ms)
{
    /* MPM2: sleep-wake timer + PS_HOLD (stock firmware polls it for delays) */
    DeviceState *mpm = qdev_new("msm8992-mpm");

    sysbus_realize_and_unref(SYS_BUS_DEVICE(mpm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mpm), 0, MSM8992_MPM_BASE);

    /* APCS doorbell pad — stock SmdLib kicks 0xF900D008 on the 8994-family
     * fallback path when its HWIO protocol can't resolve "APCS_KPSS_GLB". */
    DeviceState *apcs = qdev_new("msm8992-apcs");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(apcs), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(apcs), 0, 0xF900D000);



    /*
     * Tier-1 register blocks the firmware touches on every boot.
     * LOG_UNIMP traces from these feeds the §9 gap registry (RESEARCH.md):
     * every unimplemented access the stock firmware makes is exactly the
     * behavior contract the real models must implement.
     */
    {
        DeviceState *g;
        g = qdev_new("msm8992-gcc");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(g), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(g), 0, MSM8992_GCC_BASE);
    }
    /*
     * Carpet-stub the whole QGIC/APCS chipset window (DeviceMemoryMap.h
     * "TERMINATOR" 0xF9000000+0x113000): the real GIC dist/cpu, QTimer and
     * the APCS doorbell pad are mapped at priority 0 and win over this
     * -1000-priority filler; everything else (clock blocks etc.) logs-and-
     * reads-as-zero instead of faulting.
     */
    create_unimplemented_device("qgic-adjacent", 0xF9000000ULL, 0x113000);

    {
        static const MemoryRegionOps low_scratch_ops = {
            .read = msm8992_low_scratch_read,
            .write = msm8992_low_scratch_write,
            .endianness = DEVICE_LITTLE_ENDIAN,
            .impl.min_access_size = 1,
            .impl.max_access_size = 8,
        };
        MemoryRegion *ls = g_new(MemoryRegion, 1);
        memory_region_init_io(ls, NULL, &low_scratch_ops, NULL,
                              "msm8992.low-scratch", 0x10000);
        memory_region_add_subregion(get_system_memory(), 0x0ULL, ls);
    }

    create_unimplemented_device("tlmm",      MSM8992_TLMM_BASE,  0x4000);
    /* WDT0 kick/bite regs @0xFD41A000 (SBL1 boot-mark writes land at
     * +0x400; unmodeled WDT would abort the pet path mid-boot) */
    create_unimplemented_device("wdt0",      0xFD41A000ULL,      0x1000);
    /* CRYPTO0/1 engines (SBL1 initializes them for secboot) */
    create_unimplemented_device("crypto0",   0xFD404000ULL,      0x1C000);
    create_unimplemented_device("crypto1",   0xFD444000ULL,      0x1C000);
    {
        DeviceState *sp;
        sp = qdev_new("msm8992-spmi");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sp), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sp), 0, MSM8992_SPMI_BASE);
    }
    {
        DeviceState *pr;
        pr = qdev_new("msm8992-prng");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(pr), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(pr), 0, 0xF9BFF000ULL);
    }
    create_unimplemented_device("sec-trust-ctrl", 0xFC4B8000, 0x7000);
    create_unimplemented_device("rpm-msgram", MSM8992_RPM_MSGRAM, 0x4000);
    /* SBL1 clears the RPM IPC status/ack words at +0x20/+0x24 of this
     * block; without a mapping the write faults (data abort, 302a). */
    create_unimplemented_device("rpm-ipc", 0xFC44F000ULL, 0x1000);
    {
        /*
         * Qualcomm Cortex Timer (QCT-QTIMER, the model from the official
         * Qualcomm QEMU tree). Two MMIO windows: the access-control block
         * (CNTFRQ/CNTSR/CNTTID/CNTACR/VERSION) and the timer frames
         * (CNTPCT/CNTP_CVAL/CNTP_TVAL/CNTP_CTL). SBL1 polls the frame-0
         * physical counter at 0xF9021000, so the frame window starts there
         * and the AC block sits right below it.
         */
        DeviceState *qt;
        qt = qdev_new(TYPE_QCT_QTIMER);
        qdev_prop_set_uint32(qt, "freq-hz", 19200000);
        qdev_prop_set_uint32(qt, "freq-scale", 1);
        qdev_prop_set_uint32(qt, "nr_frames", 8);
        qdev_prop_set_uint32(qt, "nr_views", 1);
        qdev_prop_set_uint32(qt, "frame_stride", 0x1000);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(qt), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(qt), 0, MSM8992_QTIMER_F0 - 0x1000);
        sysbus_mmio_map(SYS_BUS_DEVICE(qt), 1, MSM8992_QTIMER_F0);
    }
    {
        DeviceState *fb;
        fb = qdev_new("msm8992-fb");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(fb), &error_fatal);
    }

    /* Tier-2/3 placeholders */
    create_unimplemented_device("mmss-lo",   0xFD800000ULL, 0xC0000);
    /* BIMC / DDR PHY window (msm8992 dtsi: bimc@fc380000): SBL1's DDR
     * driver runs its LPDDR3 training against these registers. Reads-as-0
     * for now; every access is logged for the DDR contract extraction. */
    create_unimplemented_device("bimc",      0xFC380000ULL, 0x20000);
    create_unimplemented_device("vbif",      0xFD8C0000ULL, 0x40000);
    create_unimplemented_device("kgsl-3d0",  MSM8992_KGSL_BASE, 0x10000);
    create_unimplemented_device("mdp5",      MSM8992_MDP5_BASE, 0xB4000);
    create_unimplemented_device("smmu-mmu0", MSM8992_SMMU_MMU0, 0xB000);
    create_unimplemented_device("ocmem",     MSM8992_OCMEM_BASE, 0x2000);
    create_unimplemented_device("dwc3",      MSM8992_DWC3_BASE, 0xFC000);
    {
        DeviceState *t;
        t = qdev_new("msm8992-tcsr");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(t), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(t), 0, 0xFD4A0000);
    }
}

/*
 * Pre-populate SMEM (the legacy LK-style layout that both stock firmware and
 * Lumia950XLPkg SmemDxe read — Driver/SmemDxe/smem.c, Include/Chipset/smem.h
 * "struct smem"). On real hardware SBL1 does this; since we skip SBL1, the
 * machine has to plant the tables itself, or UEFI dies in board/platform
 * detection.
 *
 * Layout @0x06A00000:
 *   0x00  proc_comm[4]          (64 B, zero)
 *   0x40  version_info[32]      (each = SMEM_VERSION(0xB) << 16)
 *   0xC0  heap_info             {initialized, free_offset, heap_remaining}
 *   0xD0  alloc_info[419]       {allocated, offset, size, base_ext}
 *
 * Allocated items we plant:
 *   #9   SMEM_AARM_PARTITION_TABLE @+0x4000 — valid magic, zero parts
 *   #137 SMEM_BOARD_INFO_LOCATION  @+0x5000 — board_info_v8, MSM8992/MTP
 * Plus the 16-byte smem_addr_info at 0xFE805FF0 (smem_targ_info_imem).
 */
#define SMEM32_OFF_VERSIONS   0x40
#define SMEM32_OFF_HEAP       0xC0
#define SMEM32_OFF_ALLOC      0xD0
#define SMEM_ALLOC_COUNT      419
#define SMEM_HEAP_FREE_OFF    (SMEM32_OFF_ALLOC + SMEM_ALLOC_COUNT * 16)
#define SMEM_ITEM_PTABLE      9
#define SMEM_ITEM_BOARD_INFO  137
#define SMEM_ITEM_RAM_PTABLE  402   /* SMEM_USABLE_RAM_PARTITION_TABLE */
#define SMEM_ITEM_CHAN_ALLOC  13    /* SMEM_CHANNEL_ALLOC_TBL */
#define SMEM_ITEM_SMD_BASE    14    /* SMEM_SMD_BASE_ID: ctrl per cid */
#define SMEM_ITEM_SMD_FIFO    338   /* SMEM_SMD_FIFO_BASE_ID: fifo per cid */
#define SMEM_PTABLE_OFF       0x4000
#define SMEM_BOARD_INFO_OFF   0x5000
#define SMEM_RAMPT_OFF        0x6000
#define SMEM_CHAN_TBL_OFF     0x30000
#define SMEM_SMD_CTRL_OFF     0x31000   /* cid 0: rpm_requests ctrl (88 B) */
#define SMEM_SMD_FIFO_OFF     0x32000   /* cid 0: fifo (2 KiB) */

static const struct { char name[16]; uint32_t start, size, attr; } msm8992_flash_ptn[] = {
  {"DPP", 0x00001000, 0x00004000},
  {"MODEM_FSG", 0x00005000, 0x00000c00},
  {"MODEM_FS1", 0x00006000, 0x00000c00},
  {"MODEM_FS2", 0x00007000, 0x00000c00},
  {"MODEM_FSC", 0x00008000, 0x00000020},
  {"DDR", 0x00009000, 0x00000800},
  {"SEC", 0x0000a000, 0x00000100},
  {"APDP", 0x0000b000, 0x00000200},
  {"MSADP", 0x0000c000, 0x00000200},
  {"DPO", 0x0000d000, 0x00000020},
  {"LogFS", 0x0000e000, 0x00008000},
  {"SSD", 0x00016000, 0x00000020},
  {"UEFI_BS_NV", 0x00017000, 0x00000200},
  {"UEFI_RT_NV", 0x00018000, 0x00000200},
  {"LIMITS", 0x00019000, 0x00000020},
  {"SBL1", 0x0001a000, 0x00000800},
  {"PMIC", 0x0001b000, 0x00000400},
  {"DBI", 0x0001c000, 0x00000060},
  {"UEFI", 0x0001d000, 0x00001000},
  {"RPM", 0x0001e000, 0x000003e8},
  {"TZ", 0x0001f000, 0x00000800},
  {"HYP", 0x00020000, 0x000003e8},
  {"WINSECAPP", 0x00021000, 0x00000400},
  {"TZAPPS", 0x00022000, 0x00008000},
  {"BACKUP_SBL1", 0x0002a000, 0x00000800},
  {"BACKUP_PMIC", 0x0002b000, 0x00000400},
  {"BACKUP_DBI", 0x0002c000, 0x00000060},
  {"BACKUP_UEFI", 0x0002d000, 0x00001000},
  {"BACKUP_RPM", 0x0002e000, 0x000003e8},
  {"BACKUP_TZ", 0x0002f000, 0x00000800},
  {"BACKUP_HYP", 0x00030000, 0x000003e8},
  {"BACKUP_WINSECAP", 0x00031000, 0x00000400},
  {"BACKUP_TZAPPS", 0x00032000, 0x00008000},
  {"PLAT", 0x0003a000, 0x00004000},
  {"EFIESP", 0x0003e000, 0x00010000},
  {"MMOS", 0x0004e000, 0x00003000},
  {"MainOS", 0x00054000, 0x0057a800},
  {"Data", 0x005d0400, 0x03a2ab1e},
};

static void msm8992_smem_store(uint8_t *ram, hwaddr off, uint32_t val)
{
    stl_le_p(ram + off, val);
}

static MemoryRegion *msm8992_imem_mr;

static void msm8992_populate_smem(MachineState *ms)
{
    uint8_t *ram = memory_region_get_ram_ptr(ms->ram);
    uint8_t *base = ram + MSM8992_SMEM_BASE;
    MemoryRegion *targ;
    int i;

    /* version_info[]: SMEM_VERSION 0xB in the major halfword */
    for (i = 0; i < 32; i++) {
        msm8992_smem_store(base, SMEM32_OFF_VERSIONS + i * 4, 0x000B0000);
    }
    /* heap_info */
    msm8992_smem_store(base, SMEM32_OFF_HEAP + 0, 1);                /* initialized */
    msm8992_smem_store(base, SMEM32_OFF_HEAP + 4, SMEM_HEAP_FREE_OFF);
    msm8992_smem_store(base, SMEM32_OFF_HEAP + 8,
                       MSM8992_SMEM_SIZE - SMEM_HEAP_FREE_OFF);

    /* alloc_info[9]: RAM partition table (header-only, no partitions yet) */
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 4,
                       SMEM_PTABLE_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 8, 16);
    msm8992_smem_store(base, SMEM_PTABLE_OFF + 0, 0x55ee73aa);   /* magic1 */
    msm8992_smem_store(base, SMEM_PTABLE_OFF + 4, 0xe35ebddb);   /* magic2 */
    msm8992_smem_store(base, SMEM_PTABLE_OFF + 8, 2);            /* version */
    msm8992_smem_store(base, SMEM_PTABLE_OFF + 12, 0);           /* len */

    /* alloc_info[137]: board_info_v8 — makes BoardDxe identify MSM8992/MTP */
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_BOARD_INFO * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_BOARD_INFO * 16 + 4,
                       SMEM_BOARD_INFO_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_BOARD_INFO * 16 + 8,
                       100);                  /* sizeof(smem_board_info_v8) */
    {
        hwaddr b = SMEM_BOARD_INFO_OFF;
        static const char build_id[32] = "msm8992-qemu";

        msm8992_smem_store(base, b +  0, 8);          /* format: v8 */
        msm8992_smem_store(base, b +  4, 251);        /* MSM8992 */
        msm8992_smem_store(base, b +  8, 0x00010000); /* msm_version 1.0 */
        memcpy(base + b + 12, build_id, sizeof(build_id));
        msm8992_smem_store(base, b + 44, 251);        /* raw_msm_id */
        msm8992_smem_store(base, b + 48, 0x00010000); /* raw_msm_version */
        msm8992_smem_store(base, b + 52, 8);          /* HW_PLATFORM_MTP */
        msm8992_smem_store(base, b + 56, 0x00010002); /* platform_version 1.2 */
        /* fused_chip / platform_subtype / pmic_info[] / align stay 0 */
    }

        /* item #402: usable-RAM partition table — BOOT.XF ram_partition.h
     * "usable_ram_partition_table_v1" (version 1):
     *   header 24B: {magic1, magic2, version=1, reserved1, num_partitions,
     *                reserved2(align)}
     *   entries 64B { name[16], start u64, length u64, attr u32,
     *                 category u32=14(SDRAM), domain u32=1(APPS),
     *                 type u32=1(SYS_MEMORY), num_partitions u32, hw_info u32,
     *                 reserved4 u32, reserved5 u32 }
     * Sec.c:ValidateFdRegion requires the FD base (0x200000) to fall inside
     * some SYS_MEMORY/SDRAM entry. We therefore cover the full 3 GiB. */
    {
        static const struct {
            const char *name;
            uint32_t start_lo, start_hi;
            uint32_t len_lo,  len_hi;
        } rows[] = {
            { "EBI0 CS0", 0x00000000, 0, 0xC0000000, 0 },
            { "EBI1 CS0", 0x80000000, 0, 0x30000000, 0 },
            { "EBI0 CS1", 0xB0001000, 0, 0x2FFFF000, 0 },
        };
        const unsigned int n = 3;
        unsigned int k;
        hwaddr p = SMEM_RAMPT_OFF;

        msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_RAM_PTABLE * 16 + 0, 1);
        msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_RAM_PTABLE * 16 + 4,
                           SMEM_RAMPT_OFF);
        msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_RAM_PTABLE * 16 + 8,
                           24 + 64 * 3);

        msm8992_smem_store(base, p + 0x00, 0x9DA5E0A8);
        msm8992_smem_store(base, p + 0x04, 0xAF9EC4E2);
        msm8992_smem_store(base, p + 0x08, 1);               /* version 1 */
        msm8992_smem_store(base, p + 0x10, n);              /* num_partitions */
        for (k = 0; k < n; k++) {
            hwaddr e = p + 0x18 + k * 64;
            memset(base + e, 0, 64);
            memcpy(base + e + 0x00, rows[k].name, strlen(rows[k].name));
            msm8992_smem_store(base, e + 0x10, rows[k].start_lo);
            msm8992_smem_store(base, e + 0x14, rows[k].start_hi);
            msm8992_smem_store(base, e + 0x18, rows[k].len_lo);
            msm8992_smem_store(base, e + 0x1C, rows[k].len_hi);
            msm8992_smem_store(base, e + 0x20, 1);          /* READWRITE */
            msm8992_smem_store(base, e + 0x24, 0xE);        /* SDRAM */
            msm8992_smem_store(base, e + 0x28, 1);          /* APPS_DOMAIN */
            msm8992_smem_store(base, e + 0x2C, 1);          /* SYS_MEMORY */
            msm8992_smem_store(base, e + 0x30, n);          /* num_partitions */
        }
    }

    /* item #9: eMMC partition table (smem_ptable, magic 0x55ee73aa/e35ebddb) */
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 4,
                       SMEM_PTABLE_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_PTABLE * 16 + 8,
                       16 + (uint32_t)(sizeof(msm8992_flash_ptn)
                                       / sizeof(msm8992_flash_ptn[0])) * 28);
    {
        hwaddr p = SMEM_PTABLE_OFF;
        unsigned int n = sizeof(msm8992_flash_ptn) / sizeof(msm8992_flash_ptn[0]);
        unsigned int k;
        msm8992_smem_store(base, p + 0, 0x55ee73aa);
        msm8992_smem_store(base, p + 4, 0xe35ebddb);
        msm8992_smem_store(base, p + 8, 1);            /* version 1 */
        msm8992_smem_store(base, p + 12, n);           /* len */
        for (k = 0; k < n; k++) {
            memcpy(base + p + 16 + k * 28, msm8992_flash_ptn[k].name, 16);
            msm8992_smem_store(base, p + 16 + k * 28 + 16,
                               msm8992_flash_ptn[k].start);
            msm8992_smem_store(base, p + 16 + k * 28 + 20,
                               msm8992_flash_ptn[k].size);
            msm8992_smem_store(base, p + 16 + k * 28 + 24, 0);
        }
    }

    /* SMD ghost: a pre-opened APPS<->RPM channel named "rpm_requests".
     * Without an RPM firmware image, ClockDxe/RpmLib would otherwise spin
     * forever in smd_is_port_open("rpm_requests", SMD_APPS_RPM).  We plant
     * cid 0 with both half-channels OPENED but CD=0: RpmLib then runs in
     * "rpm_is_up == no" autocomplete mode and never blocks on wire ACKs.
     *
     * #13 SMEM_CHANNEL_ALLOC_TBL: 64 x {name[20], cid, prop, ref_count}
     *     entry 0 = { "rpm_requests", 0, ctype=SMD_APPS_RPM(0x0F), 1 }
     * #14 SMEM_SMD_BASE_ID+0: smd_port_ctrl_info {ch0,ch1}, each half
     *     11 x u32 { state=2(OPENED), DTR_DSR=1, CTS_RTS=1, CD=0, ... }
     * #338 SMEM_SMD_FIFO_BASE_ID+0: 2 KiB fifo (tx|rx halves) */
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_CHAN_ALLOC * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_CHAN_ALLOC * 16 + 4,
                       SMEM_CHAN_TBL_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_CHAN_ALLOC * 16 + 8,
                       64 * 32);
    {
        hwaddr t = SMEM_CHAN_TBL_OFF;
        memcpy(base + t + 0, "rpm_requests", 12);
        msm8992_smem_store(base, t + 20, 0);        /* cid 0 */
        msm8992_smem_store(base, t + 24, 0x0000000F); /* prop: ctype APPS_RPM */
        msm8992_smem_store(base, t + 28, 1);        /* ref_count */

        hwaddr c = SMEM_SMD_CTRL_OFF;
        int half;
        for (half = 0; half < 2; half++) {
            msm8992_smem_store(base, c + half * 44 + 0, 2); /* SMD_SS_OPENED */
            msm8992_smem_store(base, c + half * 44 + 4, 1); /* DTR_DSR */
            msm8992_smem_store(base, c + half * 44 + 8, 1); /* CTS_RTS */
            /* CD/RI/indices stay 0 */
        }
    }
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_BASE * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_BASE * 16 + 4,
                       SMEM_SMD_CTRL_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_BASE * 16 + 8, 88);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_FIFO * 16 + 0, 1);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_FIFO * 16 + 4,
                       SMEM_SMD_FIFO_OFF);
    msm8992_smem_store(base, SMEM32_OFF_ALLOC + SMEM_ITEM_SMD_FIFO * 16 + 8,
                       0x2000);

    /* smem_addr_info beacon for SmemDxe (struct smem_addr_info @ 0xFE805FF0),
     * and the whole IMEM window — stock firmware reads cookies/target info
     * all over 0xFE80xxxx (msm-imem@fe80f000, tz-log@fe80f720, ...). */
    targ = g_new(MemoryRegion, 1);
    memory_region_init_ram(targ, NULL, "msm8992.imem", MSM8992_IMEM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), MSM8992_IMEM_BASE, targ);
    msm8992_imem_mr = targ;
    {
        uint8_t *ti = memory_region_get_ram_ptr(targ);
        /* boot_flash_dev_if: PBL's IMEM flash-type cookie. Retail SBL1
         * reads *(int*)0xFE80F838 (sbl_shared->+0x40 -> here) as
         * boot_flash_type to dispatch boot_flash_dev_sdcc registration;
         * 0 = NO_FLASH skips it and lock_singleton later asserts 300d. */
        stl_le_p(ti + 0xF838, 4);                     /* SDC_FLASH */
        stl_le_p(ti + 0x5FF0 + 0, 0x49494953);        /* "SIII" identifier */
        stl_le_p(ti + 0x5FF0 + 4, MSM8992_SMEM_SIZE);
        stl_le_p(ti + 0x5FF0 + 8, (uint32_t)MSM8992_SMEM_BASE);
    }
}

typedef struct { uint8_t *ptr; hwaddr size; } Msm8992SmemTraceCtx;
static Msm8992SmemTraceCtx msm8992_smem_ctx;

static uint64_t msm8992_smem_trace_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t v = 0;
    if (offset < msm8992_smem_ctx.size - 4)
        memcpy(&v, msm8992_smem_ctx.ptr + offset, size);
    qemu_log_mask(LOG_UNIMP, "smem READ +0x%05" HWADDR_PRIx " = 0x%08x\n",
                  offset, v);
    return v;
}

static void msm8992_smem_trace_write(void *opaque, hwaddr offset, uint64_t v,
                                     unsigned size)
{
    if (offset < msm8992_smem_ctx.size - 4)
        memcpy(msm8992_smem_ctx.ptr + offset, &v, size);
    qemu_log_mask(LOG_UNIMP, "smem WRITE +0x%05" HWADDR_PRIx
                  " = 0x%08" PRIx64 "\n", offset, v);
}

static const MemoryRegionOps msm8992_smem_trace_ops = {
    .read = msm8992_smem_trace_read,
    .write = msm8992_smem_trace_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool msm8992_smem_trace;

static void msm8992_smem_trace_prepare(MachineState *ms)
{
    MemoryRegion *io;
    if (!msm8992_smem_trace)
        return;
    msm8992_smem_ctx.ptr = memory_region_get_ram_ptr(ms->ram) + MSM8992_SMEM_BASE;
    msm8992_smem_ctx.size = MSM8992_SMEM_SIZE;
    io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, NULL, &msm8992_smem_trace_ops, NULL,
                          "msm8992-smem-trace", MSM8992_SMEM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        MSM8992_SMEM_BASE, io, 100);
}

static bool msm8992_get_smem_trace(Object *obj, Error **errp)
{
    return msm8992_smem_trace;
}

static void msm8992_set_smem_trace(Object *obj, bool value, Error **errp)
{
    msm8992_smem_trace = value;
}

static bool msm8992_smem_enabled;

/*
 * Qualcomm SCM (secure-channel monitor) stub.
 *
 * Built-in QEMU PSCI answers only PSCI function IDs; the stock firmware
 * chain (SBL1 traces notwithstanding) and W10M drivers (qcscm8994.sys) issue
 * SMCs with QC service IDs. Route them here via arm_register_smc_vendor_handler
 * and answer "success" after logging — the log is the trace that tells us the
 * real SCM surface (RESEARCH.md §9-#3). Returning 0 keeps UEFI/kernel going;
 * anything that matters later (PIL auth, TZ app mgmt) gets promoted to a real
 * reply as the traces demand.
 */
/*
 * Full-chain mode staging: a minimal PBL shared-data page at 0x80000 so the
 * real SBL1's early reads land on something sane. Field order follows
 * XBLLoaderLib/pbl_sbl_shared.h (boot_pbl_shared_data_type, A64: pointers are
 * 8 bytes). Everything we can afford starts zeroed; only the fields known to
 * gate its early behaviour get real values (auth/hash checks off).
 *
 * Stage iteratively: every DFAR an SBL1 run reports against this map is the
 * next field to populate for real.
 */
#define MSM8992_PBL_SHARED   0x80000ULL
static void msm8992_stage_pbl_shared(MachineState *ms)
{
    uint8_t *ram = memory_region_get_ram_ptr(ms->ram);
    uint8_t *p = ram + MSM8992_PBL_SHARED;

    memset(p, 0, 0x1000);
    stl_le_p(p + 0x00, 0x00010001); /* shared_data_version (v2) */
    /* +0x08 (qword): retail SBL1 main_ctl reads [pbl_shared+8] as the
     * boot_mmu_enable argument — the L1 identity page-table base. */
    stq_le_p(p + 0x08, MSM8992_PBL_SHARED + 0x10000);
    stl_le_p(p + 0x10, 0);          /* bl_loader_auth_enabled */
    stl_le_p(p + 0x14, 1);          /* hash_integrity_check_disabled */
    stl_le_p(p + 0x18, 1);          /* bl_sec_boot_flow_supported */
    stl_le_p(p + 0x1C, 1);          /* bl_sec_auth_disabled */
    stq_le_p(p + 0x20, MSM8992_PBL_SHARED + 0x4000); /* boot_stack_base */
    stl_le_p(p + 0x28, 0x4000);     /* boot_stack_size */
    /* Retail SBL1 contract (Ghidra FUN_fec06408 @0xFEC06408): struct is
     * 0x68 bytes; +0x40 -> 0x58B sub-struct, +0x48 -> flash shared
     * (asserted non-NULL; head {q,q,u32,0x2C} rebuilt at 0xFECae588),
     * +0x50 clock, +0x58 elf_img, +0x60 secboot. */
    stq_le_p(p + 0x40, MSM8992_PBL_SHARED + 0xD000);  /* flash shared (retail: type-first) */
    stq_le_p(p + 0x48, MSM8992_PBL_SHARED + 0xE000);  /* qsee iface head copy */
    stq_le_p(p + 0x50, MSM8992_PBL_SHARED + 0xA000);  /* pbl_clock_shared_data */
    stq_le_p(p + 0x58, MSM8992_PBL_SHARED + 0xB000);  /* elf_img_shared_info */
    stq_le_p(p + 0x60, MSM8992_PBL_SHARED + 0xC000);  /* secboot_shared_data */

    /* Retail boot_flash_dev_if contract: pbl_shared+0x40 struct starts with
     * boot_flash_type (boot_flash_init reads *(int*)it to dispatch SDCC
     * registration; the 3.3-header version-first layout makes the dispatcher
     * silently skip and lock_singleton later asserts 300d). */
    stl_le_p(p + 0xD000, 5);                          /* type = MMC_FLASH (eMMC on SDCC1) */
    stl_le_p(p + 0xD004, 0xF9824000);                 /* CS_base: SDCC1 */
    stl_le_p(p + 0xD008, 8);                          /* data_width */
    stl_le_p(p + 0xD00C, 0);                          /* ctrl_type */
    /* dev_info.sdc */
    stl_le_p(p + 0xD010, 0);          /* mem */
    stl_le_p(p + 0xD014, 4);          /* card_type */
    stl_le_p(p + 0xD018, 2);          /* card_state: standby */
    stl_le_p(p + 0xD01C, 2);          /* rca */
    stl_le_p(p + 0xD020, 200000000);  /* mclk */
    stl_le_p(p + 0xD024, 0);          /* port: SDC1 */
    stl_le_p(p + 0xD028, 0);          /* boot_cfg */
    stl_le_p(p + 0xD02C, 2);          /* data_width: 8-bit */
    stl_le_p(p + 0xD030, 1);          /* is_gpt_parti */
    stl_le_p(p + 0xD034, 1);          /* is_primary_table */
    stq_le_p(p + 0xD038, 0x0001A000); /* starting_lba: SBL1 */
    stq_le_p(p + 0xD040, 0x0001A7FF); /* ending_lba */

    /* pbl_clock_shared_data (0xA000 page): {nVersionInfo, nProcSpeedMHz} */
    stl_le_p(p + 0xA000, 0x00010000);
    stl_le_p(p + 0xA004, 1248); /* MHz, A53 cluster reset speed */

    /* pbl_elf_img_share_info_type (0xB000 page): version + meta ptr */
    stl_le_p(p + 0xB000, 0x00010000);                 /* shared_data_version */
    stq_le_p(p + 0xB008, MSM8992_PBL_SHARED + 0xC800);/* elf_img_meta_info */
    /* num_of_bytes_read_from_elf stays 0 */

    /* sec_img_auth_elf_info_type （meta page itself）: ρεal hdr/phdr pointers
     * into a copy of SBL1's ELF header we stage at 0x80000+0xC900 */
    stq_le_p(p + 0xC800, MSM8992_PBL_SHARED + 0xC900); /* elf_hdr */
    stq_le_p(p + 0xC808, MSM8992_PBL_SHARED + 0xCA00); /* prog_hdr */
    stq_le_p(p + 0xC810, 0);                           /* hash_seg_hdr */

    /* cache_mmu_share_info (0xF000 page): version, page_tbl_base, size.
     * Retail SBL1 reads *page_tbl_base as its TTBR0_EL3 and rewrites the
     * four 1GB identity block entries itself, but boot_mmu_map_region then
     * swaps L1[3] for an L2 table @IMEM+0x1000 (0xFE801000) and writes into
     * it through the MMU — so PBL must hand over an already-populated map
     * (real PBL does): L1 blocks for RAM + L2 covering 0xC0000000+ with
     * 2MB descriptors (attr4, same shape SBL1 builds: PA|0x711). */
    stl_le_p(p + 0xF000, 0x00010001);
    stq_le_p(p + 0xF008, MSM8992_PBL_SHARED + 0x10000); /* L1 table */
    stl_le_p(p + 0xF00C, 0x1000);                       /* table size */
    {
        uint64_t *l1 = (uint64_t *)(p + 0x10000);
        uint64_t *l2;
        int i;

        memset(p + 0x10000, 0, 0x1000);
        /* RAM blocks: 0x00000000-0xBFFFFFFF (3GB DRAM, device-less RW) */
        l1[0] = 0x0000000000000411ULL;
        l1[1] = 0x0000000040000411ULL;
        l1[2] = 0x0000000080000411ULL;
        /* 0xC0000000-0xFFFFFFFF: L2 table at IMEM+0x1000 (SBL1's own spot) */
        l1[3] = 0x00000000FE801003ULL;

        /* L2 (in IMEM ram, 512 x 2MB blocks over 0xC0000000-0xFFFFFFFF) */
        l2 = (uint64_t *)memory_region_get_ram_ptr(msm8992_imem_mr)
             + 0x1000 / sizeof(uint64_t);
        for (i = 0; i < 512; i++) {
             l2[i] = (0xC0000000ULL + ((uint64_t)i << 21)) | 0x711ULL;
        }
        /* SBL1 splits the IMEM 2MB (L2[0x1F4], VA 0xFE800000+) into 4KB
         * pages via an L3 table @IMEM+0x2000; writes into it go through the
         * MMU, so both levels must pre-exist (retail PBL fills them too). */
        l2[0x1F4] = 0xFE802003ULL;               /* table -> L3 */
        {
            uint64_t *l3 = (uint64_t *)memory_region_get_ram_ptr(
                               msm8992_imem_mr) + 0x2000 / sizeof(uint64_t);
            for (i = 0; i < 512; i++) {
                l3[i] = (0xFE800000ULL + ((uint64_t)i << 12)) | 0x713ULL;
            }
        }
    }

    /* boot_flash_shared_dev_info_type (0xE000 page):
     * the phone boots from eMMC on SDC1; PBL records its probe result here. */
    stl_le_p(p + 0xE000, 0x00010000); /* shared_data_version */
    stl_le_p(p + 0xE004, 4);          /* type = SDC_FLASH */
    stl_le_p(p + 0xE008, 0xF9824000); /* CS_base: SDCC1 core config */
    stl_le_p(p + 0xE00C, 8);          /* data_width */
    /* ctrl_type = 0 (SDHC/QC default) */
    /* union dev_info.sdc @ +0x14: */
    stl_le_p(p + 0xE014, 0);          /* mem (enum 0: unknown) */
    stl_le_p(p + 0xE018, 4);          /* card_type (MMC-like) */
    stl_le_p(p + 0xE01C, 2);          /* card_state: standby */
    stl_le_p(p + 0xE020, 2);          /* rca (QEMU emmc default) */
    stl_le_p(p + 0xE024, 200000000);  /* mclk */
    stl_le_p(p + 0xE028, 0);          /* port: SDC1 */
    stl_le_p(p + 0xE02C, 0);          /* boot_cfg */
    stl_le_p(p + 0xE030, 2);          /* data_width: 8-bit */
    stl_le_p(p + 0xE034, 1);          /* is_gpt_parti */
    stl_le_p(p + 0xE038, 1);          /* is_primary_table */
    stl_le_p(p + 0xE03C, 0x0001A000); /* starting_lba: SBL1 */
    stl_le_p(p + 0xE044, 0x0001A7FF); /* ending_lba */
}

static int32_t msm8992_scm_call(ARMCPU *cpu, uint64_t id, uint64_t a1,
                                uint64_t a2, uint64_t a3)
{
    qemu_log_mask(LOG_UNIMP,
                  "msm8992-scm: id=0x%" PRIx64 " args=(0x%" PRIx64
                  ",0x%" PRIx64 ",0x%" PRIx64 ") -> success (stub)\n",
                  id, a1, a2, a3);
    return 0;
}

static void msm8992_init(MachineState *machine)
{
    Msm8992MachineState *s = MSM8992_MACHINE(machine);

    qemu_irq *gic_spi;
    unsigned int i;

    if (machine->ram_size != MSM8992_DRAM_SIZE &&
        machine->ram_size != MSM8992_DRAM_SIZE + 0x20000000ULL) {
        error_report("msm8992: Talkman DRAM is 3GiB (usable to 0xE0000000; use -m 3G or -m 3584M)");
        exit(1);
    }

    /* Main DRAM: the full 3 GiB window at 0x0 (SMS scenario: SMEM and the
     * framebuffer live inside it, holes like TZ/HYP are just RAM contents). */
    memory_region_add_subregion(get_system_memory(), MSM8992_DRAM_BASE,
                                machine->ram);
    if (msm8992_smem_enabled) {
        msm8992_populate_smem(machine);
    }
    msm8992_smem_trace_prepare(machine);

    msm8992_create_cpus(s, machine);
    msm8992_create_gic(s, machine);

    /* SPI lines pass 1:1 to the GIC's external inputs */
    gic_spi = g_new(qemu_irq, MSM8992_GIC_NUM_SPI);
    for (i = 0; i < MSM8992_GIC_NUM_SPI; i++) {
        gic_spi[i] = qdev_get_gpio_in(s->gic, i);
    }

    /* SMEM lives inside the DRAM window; no aliasing needed. */
    /*
     * Peripheral-core memory windows that must behave as RAM for the
     * firmware chain (SBL1/TZ/HYP/RPM get loaded there by the loader or
     * read their own code from these locations by AP):
     *   - RPM code RAM 0xFC100000..0xFC110000 (RPM.image vaddr 0xFC100090)
     *   - SBL workspace / boot RAM 0xFEC00000..0xFEC80000
     */
    {
        MemoryRegion *r = g_new(MemoryRegion, 1);
        memory_region_init_ram(r, NULL, "msm8992.rpm-coderam", 0x100000,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0xFC100000, r);

        /*
         * PMIC PSI (Power Sequence Information) header in RPM code RAM at
         * SCL_PMIC_CONFIG_BUF_BASE. Retail SBL1's pm_device_init walks this
         * table (magic "PSI_" + table pointer, 20-byte entries, type 4 =
         * END). On hardware the PMIC image's PDM data supplies it; the
         * sequence itself only programs regulator registers, which the fake
         * bus ignores, so a terminator-only table keeps the walk happy.
         */
        {
            uint8_t *rp = memory_region_get_ram_ptr(r);
            uint32_t psi_base = 0xA080;   /* 0xFC10A080 */
            stl_le_p(rp + psi_base, 0x5F495350);            /* "PSI_" magic */
            stl_le_p(rp + psi_base + 8, 0xFC10A0A0);        /* entry table */
            stl_le_p(rp + psi_base + 0xC, 0xFC10A0C0);      /* reg-off table */
            stl_le_p(rp + psi_base + 0x28, 4);              /* entry type END */
        }
    }
    {
        MemoryRegion *r = g_new(MemoryRegion, 1);
        memory_region_init_ram(r, NULL, "msm8992.sbl-work", 0x400000,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0xFEC00000, r);
    }
    /*
     * Backfill the DSC-declared DRAM footprint [ram_size .. 0xE0000000):
     * DeviceMemoryMap.h/SMEM ram-ptable both advertise the phone as
     * fragments summing to a 0xE0000000 bytes top address, so any
     * page the allocator layers above ram_size must still resolve.
     */
    if (machine->ram_size < 0xE0000000ULL) {
        MemoryRegion *r = g_new(MemoryRegion, 1);
        memory_region_init_ram(r, NULL, "msm8992.hole-ce",
                               0xE0000000ULL - machine->ram_size,
                               &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            machine->ram_size, r, 0);
    }
    {
        MemoryRegion *r = g_new(MemoryRegion, 1);
        memory_region_init_ram(r, NULL, "msm8992.hole-ef", 0x0F000000,
                               &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            0xE0000000, r, 0);
    }

    /*
     * RPM shared RAM windows the stock ClockLib touches during
     * boot_pre_ddr_clock_init() (sbl1_hw.c: "Pre_DDR_clock_init"):
     *
     *  - 0xF9011000 / 0xF9015000: the "clock request" structures the
     *    SBL1 hands to the RPM.  Word 0 is the request/state word: the
     *    driver writes the enable sequence (2 -> 6 -> 7) and then waits
     *    for the RPM firmware to set bit31 ("the switch has completed").
     *    The stock ClockLib also refuses to configure a clock whose
     *    state word still has bit31 set or bits[2:0] == 7, so the RPM
     *    side is emulated here: a pending request is acknowledged
     *    immediately, and the acknowledgement disappears once the driver
     *    writes the final "on" value.
     *
     *  - 0xF9112000: the DSB word the RPM clock request is written to.
     */
    {
        static const uint32_t clock_domain[] = {
            0x00000000,   /* +0x00 request/state word                  */
            0x00000000,   /* +0x04                                    */
            0x00000000,   /* +0x08                                    */
            0x00000000,   /* +0x0C                                    */
            0x00000000,   /* +0x10                                    */
            0x00000000,   /* +0x14                                    */
            0x00000000,   /* +0x18                                    */
            0x00000000,   /* +0x1C                                    */
            0x00000000,   /* +0x20                                    */
            0x00000000,   /* +0x24                                    */
            0x00000000,   /* +0x28                                    */
            0x00000000,   /* +0x2C                                    */
        };
        MemoryRegion *r = g_new(MemoryRegion, 1);
        uint32_t *regs = g_new0(uint32_t, 0x10000 / 4);

        memory_region_init_io(r, NULL, &msm8992_rpm_clk_ops,
                              regs, "msm8992.rpm-clk-ram", 0x10000);
        memory_region_add_subregion(get_system_memory(), 0xF9010000, r);
        for (unsigned j = 0; j < ARRAY_SIZE(clock_domain); j++) {
            regs[(0x1000 >> 2) + j] = clock_domain[j];
            regs[(0x5000 >> 2) + j] = clock_domain[j];
        }
    }
    {
        MemoryRegion *r = g_new(MemoryRegion, 1);
        memory_region_init_ram(r, NULL, "msm8992.rpm-dsb", 0x1000,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0xF9112000, r);
    }

    msm8992_create_uart(s, gic_spi);
    msm8992_create_sdc1(s, gic_spi, s->fullchain);

    msm8992_create_stubs(machine);

    /* SMD ghost coprocessor: emulates the RPM side of the rpm_requests
     * channel on SMEM; raises GIC INTID 200 (PcdSmdIrq used by SmdDxe). */
    {
        DeviceState *gh = qdev_new("msm8992-smd-ghost");
        SysBusDevice *ghsbd;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gh), &error_fatal);
        ghsbd = SYS_BUS_DEVICE(gh);
        sysbus_connect_irq(ghsbd, 0, gic_spi[168]);  /* SPI 168 = INTID 200 */
    }

    /*
     * fullchain=on: boot the real chain (pblstub -> SBL1 -> TZ -> UEFI).
     * The synthetic SCM vendor hook must NOT be registered then: SMCs have
     * to reach the guest's own EL3 monitor once TZ installs it. Also use
     * the PSCI "disabled" conduit so QEMU's internal PSCI shim doesn't eat
     * vendor SMCs either (mirrors virt's firmware_loaded behaviour).
     */
    if (!s->fullchain) {
        arm_register_smc_vendor_handler(msm8992_scm_call);
    }

    if (machine->kernel_filename) {
        s->bootinfo.ram_size       = machine->ram_size;
        s->bootinfo.loader_start   = MSM8992_DRAM_BASE;
        s->bootinfo.kernel_filename  = machine->kernel_filename;
        s->bootinfo.kernel_cmdline   = machine->kernel_cmdline;
        s->bootinfo.initrd_filename  = machine->initrd_filename;
        s->bootinfo.skip_dtb_autoload = true; /* Windows uses ACPI, not DTB */
        /* Always SMC: the vendor hook below also catches HVC-carried PSCI. */
        s->bootinfo.psci_conduit   = s->fullchain ? QEMU_PSCI_CONDUIT_DISABLED
                                                  : QEMU_PSCI_CONDUIT_SMC;
        s->bootinfo.gic_cpu_if_addr = MSM8992_GIC_CPU;
        s->bootinfo.board_id        = -1;
        if (s->fullchain) {
            msm8992_stage_pbl_shared(machine);
        } else {
            /* plain -kernel UEFI lane: we'd have TZ/SBL1 do this for real;
             * emulate the drop to NS EL1 so UEFI doesn't hit its EL3 assert */
            s->bootinfo.target_el = 1;
        }
        arm_load_kernel(ARM_CPU(&s->cpu[MSM8992_BOOT_CPU]), machine,
                        &s->bootinfo);
        /*
         * Without an EL3/EL2 firmware ROM, hardware starts at the EL the
         * image was linked for; also honor -device loader images.
         */
    }

    g_free(gic_spi);
}

static bool msm8992_get_secure(Object *obj, Error **errp)
{
    return MSM8992_MACHINE(obj)->secure;
}

static void msm8992_set_secure(Object *obj, bool value, Error **errp)
{
    MSM8992_MACHINE(obj)->secure = value;
}

static bool msm8992_get_virt(Object *obj, Error **errp)
{
    return MSM8992_MACHINE(obj)->virt;
}

static void msm8992_set_virt(Object *obj, bool value, Error **errp)
{
    MSM8992_MACHINE(obj)->virt = value;
}

static bool msm8992_get_smem(Object *obj, Error **errp)
{
    return msm8992_smem_enabled;
}

static bool msm8992_get_fullchain(Object *obj, Error **errp)
{
    return MSM8992_MACHINE(obj)->fullchain;
}

static void msm8992_set_fullchain(Object *obj, bool value, Error **errp)
{
    MSM8992_MACHINE(obj)->fullchain = value;
}

static void msm8992_set_smem(Object *obj, bool value, Error **errp)
{
    msm8992_smem_enabled = value;
}

static const CPUArchIdList *msm8992_possible_cpu_arch_ids(MachineState *ms)
{
    int n;

    if (ms->possible_cpus) {
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * MSM8992_NUM_CPUS);
    ms->possible_cpus->len = MSM8992_NUM_CPUS;
    {
        for (n = 0; n < MSM8992_NUM_CPUS; n++) {
            ms->possible_cpus->cpus[n].type =
                (n < MSM8992_NUM_LITTLE) ? ARM_CPU_TYPE_NAME("cortex-a53")
                                         : ARM_CPU_TYPE_NAME("cortex-a57");
            ms->possible_cpus->cpus[n].arch_id =
                (n < MSM8992_NUM_LITTLE) ? n : 0x100 + (n - MSM8992_NUM_LITTLE);
            ms->possible_cpus->cpus[n].props.has_thread_id = true;
            ms->possible_cpus->cpus[n].props.thread_id = n;
        }
    }
    return ms->possible_cpus;
}

static void msm8992_instance_init(Object *obj)
{
    Msm8992MachineState *s = MSM8992_MACHINE(obj);

    s->secure = true;    /* EL3 resident like the real chain */
    s->virt   = false;
    msm8992_smem_enabled = true;   /* SBL1's SMEM tables emulated by default */
}

static void msm8992_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Qualcomm MSM8992 / Snapdragon 808 (Lumia 950 'Talkman')";
    mc->alias = "lumia-950";
    mc->init = msm8992_init;
    mc->default_cpus = MSM8992_NUM_CPUS;
    mc->min_cpus = 1;
    mc->max_cpus = MSM8992_NUM_CPUS;
    mc->default_ram_size = MSM8992_DRAM_SIZE;
    mc->default_ram_id = "msm8992.dram";
    mc->auto_create_sdcard = true;
    mc->possible_cpu_arch_ids = msm8992_possible_cpu_arch_ids;

    object_class_property_add_bool(oc, "secure", msm8992_get_secure,
                                   msm8992_set_secure);
    object_class_property_set_description(oc, "secure",
        "Provide EL3 (needed to run the stock TZ image)");
    object_class_property_add_bool(oc, "virtualization", msm8992_get_virt,
                                   msm8992_set_virt);
    object_class_property_set_description(oc, "virtualization",
        "Provide EL2");
    object_class_property_add_bool(oc, "fullchain", msm8992_get_fullchain,
                                   msm8992_set_fullchain);
    object_class_property_set_description(oc, "fullchain",
        "Boot the real chain: -kernel=pblstub64 + SBL1 staged at 0xFEC04000 "
        "(via -device loader), TZ resident at EL3; synthetic SCM vendor hook "
        "and QEMU PSCI are disabled so guest EL3 receives SMCs");
    object_class_property_add_bool(oc, "smem", msm8992_get_smem,
                                   msm8992_set_smem);
    object_class_property_add_bool(oc, "smem-trace",
                                   msm8992_get_smem_trace,
                                   msm8992_set_smem_trace);
    object_class_property_set_description(oc, "smem",
        "Pre-populate SMEM tables as SBL1 would (default: on)");
}

static const TypeInfo msm8992_machine_info = {
    .name          = TYPE_MSM8992_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_init = msm8992_instance_init,
    .instance_size = sizeof(Msm8992MachineState),
    .class_init    = msm8992_machine_class_init,
};

static void msm8992_machine_register_types(void)
{
    type_register_static(&msm8992_machine_info);
}

type_init(msm8992_machine_register_types)
