/*
 * Minimal Qualcomm MPM2 block (sleep timer + PS_HOLD power-off) for the
 * msm8992 machine. The stock UEFI polls MPM_SLEEP_TIMETICK_COUNT_VAL
 * (0xFC4A3000) for delays and writes MPM_PS_HOLD (0xFC4AB000) to power off.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/runstate.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "system/address-spaces.h"

/*
 * Stage-2 forensics: when the guest asks PS_HOLD (which stock firmware does
 * after the assert banner), dump CPU registers + the live stack to
 * /tmp/qemu-pshold-state.txt so the failing call's frame can be recovered.
 */
static void msm8992_mpm_forensic_dump(void)
{
    ARMCPU *armcpu = ARM_CPU(first_cpu);
    CPUState *cs = CPU(armcpu);
    CPUARMState *env = &armcpu->env;
    FILE *f = fopen("/tmp/qemu-pshold-state.txt", "w");

    if (!f)
        return;
    if (!env->aarch64) {
        int i;
        for (i = 0; i < 16; i++)
            fprintf(f, "r%-2d = 0x%08x\n", i, env->regs[i]);
        fprintf(f, "cpsr = 0x%08x\n", env->uncached_cpsr);
        /* stack: current SVC sp + 2 KiB */
        {
            uint32_t sp = env->regs[13];
            static uint8_t buf[2048];
            if (address_space_read(&address_space_memory, (hwaddr)sp,
                                   MEMTXATTRS_UNSPECIFIED, buf,
                                   sizeof(buf)) == MEMTX_OK)
                if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf))
                    fprintf(f, "stack write short\n");
        }
    } else {
        int i;
        for (i = 0; i < 32; i++)
            fprintf(f, "x%-2d = 0x%016lx\n", i, env->xregs[i]);
    }
    (void)cs;
    fclose(f);
    qemu_log("msm8992-mpm: PS_HOLD forensic state dumped to /tmp/qemu-pshold-state.txt\n");
}

#define TYPE_MSM8992_MPM "msm8992-mpm"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992MpmState, MSM8992_MPM)

#define MPM_SLEEP_TIMETICK_COUNT_VAL 0x2000   /* 0xFC4A3000 relative */
#define MPM_PS_HOLD                  0xA000   /* 0xFC4AB000 relative */
#define MPM_KRAIT_SLEEP_US_DISABLE   0x2210

#define MSM8992_MPM_SIZE  0x010000            /* through PS_HOLD window */

typedef struct Msm8992MpmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq vcto;      /* VCTO interrupt (not wired in stage 1) */
    QEMUTimer *tick_timer; /* placeholder, unused for counting */
} Msm8992MpmState;

static uint64_t msm8992_mpm_read(void *opaque, hwaddr offset, unsigned size)
{
    switch (offset) {
    case MPM_SLEEP_TIMETICK_COUNT_VAL: /* 32.768 kHz sleep counter */
        return (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000)
                          * 32768 / 1000000);
    case MPM_SLEEP_TIMETICK_COUNT_VAL + 4:  /* rollover/coarse */
        return 0;
    case MPM_PS_HOLD:
        return 0;
    default:
        return 0;
    }
}

static void msm8992_mpm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    switch (offset) {
    case MPM_PS_HOLD:
        qemu_log("msm8992-mpm: PS_HOLD request val=0x%" PRIx64
                 " (shutdown suppressed for forensics)\n", value);
        msm8992_mpm_forensic_dump();
        break;
    default:
        break;
    }
}

static const MemoryRegionOps msm8992_mpm_ops = {
    .read = msm8992_mpm_read,
    .write = msm8992_mpm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm8992_mpm_realize(DeviceState *dev, Error **errp)
{
    Msm8992MpmState *s = MSM8992_MPM(dev);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->vcto);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm8992_mpm_ops, s,
                          TYPE_MSM8992_MPM, MSM8992_MPM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm8992_mpm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_mpm_realize;
    dc->desc = "Qualcomm MPM2 (sleep counter + PS_HOLD)";
}

static const TypeInfo msm8992_mpm_info = {
    .name          = TYPE_MSM8992_MPM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992MpmState),
    .class_init    = msm8992_mpm_class_init,
};

static void msm8992_mpm_register_types(void)
{
    type_register_static(&msm8992_mpm_info);
}

type_init(msm8992_mpm_register_types)
