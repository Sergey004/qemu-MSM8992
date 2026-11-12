/*
 * Copyright (C) 2014 - Linaro
 * Author: Rob Herring <rob.herring@linaro.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "helper.h"
#include "kvm-consts.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "system/runstate.h"
#include "internals.h"
#include "arm-powerctl.h"
#include "hw/arm/boot.h"
#include "target/arm/multiprocessing.h"
#include "target/arm/trace.h"

static ARMSmcVendorHandler arm_smc_vendor_handler;

void arm_register_smc_vendor_handler(ARMSmcVendorHandler handler)
{
    arm_smc_vendor_handler = handler;
}

/*
 * Board hook for PSCI CPU_ON.  Phones implement the TrustZone side of the
 * secondary-processor start themselves and Windows uses a 64-bit PSCI call
 * (0x8400000a) from an AArch32 kernel, passing only the target id - the
 * entry point is known to the secure world.  A board that wants to emulate
 * that (our msm8992 machine does) registers a handler here; when one is
 * present the generic CPU_ON path and the "64-bit call from a 32-bit
 * caller" restriction are bypassed.
 */
static ARMPsciCpuOnHandler arm_psci_cpu_on_handler;

/* the phone's secure world accepts the 64-bit PSCI convention from an
 * AArch32 caller (Windows on ARM32 uses it) */
static bool arm_psci_64bit_from_a32 = true;

void arm_register_psci_cpu_on_handler(ARMPsciCpuOnHandler handler)
{
    arm_psci_cpu_on_handler = handler;
}

/* PSCI function IDs always live in the 0x8400xxxx / 0xC400xxxx windows */
static bool arm_is_psci_fid(uint64_t fid)
{
    uint32_t hi = fid >> 16;
    return hi == 0x8400 || hi == 0xC400;
}

bool arm_is_psci_call(ARMCPU *cpu, int excp_type)
{
    /*
     * Return true if the exception type matches the configured PSCI conduit.
     * This is called before the SMC/HVC instruction is executed, to decide
     * whether we should treat it as a PSCI call or with the architecturally
     * defined behaviour for an SMC or HVC (which might be UNDEF or trap
     * to EL2 or to EL3).
     */

    switch (excp_type) {
    case EXCP_HVC:
        if (cpu->psci_conduit != QEMU_PSCI_CONDUIT_HVC &&
            !arm_smc_vendor_handler) {
            return false;
        }
        break;
    case EXCP_SMC:
        if (cpu->psci_conduit != QEMU_PSCI_CONDUIT_SMC &&
            !arm_smc_vendor_handler) {
            return false;
        }
        break;
    default:
        return false;
    }

    return true;
}

void arm_handle_psci_call(ARMCPU *cpu)
{
    /*
     * This function partially implements the logic for dispatching Power State
     * Coordination Interface (PSCI) calls (as described in ARM DEN 0022D.b),
     * to the extent required for bringing up and taking down secondary cores,
     * and for handling reset and poweroff requests.
     * Additional information about the calling convention used is available in
     * the document 'SMC Calling Convention' (ARM DEN 0028)
     */
    CPUARMState *env = &cpu->env;
    uint64_t param[4];
    uint64_t context_id, mpidr;
    uint64_t entry;
    int32_t ret = 0;
    int i;

    for (i = 0; i < 4; i++) {
        /*
         * All PSCI functions take explicit 32-bit or native int sized
         * arguments so we can simply zero-extend all arguments regardless
         * of which exact function we are about to call.
         */
        param[i] = is_a64(env) ? env->xregs[i] : env->regs[i];
    }
    trace_arm_psci_call(param[0], param[1], param[2], param[3],
                        arm_cpu_mp_affinity(cpu));

    qemu_log_mask(LOG_GUEST_ERROR,
                  "PSCI fid=0x%" PRIx64 " p1=0x%016" PRIx64 " p2=0x%016" PRIx64
                  " p3=0x%016" PRIx64 " pc=0x%08x lr=0x%08x sp=0x%08x\n",
                  param[0], param[1], param[2], param[3],
                  (uint32_t)env->regs[15], (uint32_t)env->regs[14],
                  (uint32_t)env->regs[13]);

    /* Non-PSCI SMC/HVC (e.g. Qualcomm SCM): hand to the board hook */
    if (arm_smc_vendor_handler && !arm_is_psci_fid(param[0])) {
        ret = arm_smc_vendor_handler(cpu, param[0], param[1], param[2],
                                     param[3]);
        goto err;
    }

    /*
     * The 64-bit PSCI calling convention is normally only for AArch64
     * callers, but the phone's TrustZone accepts it from the AArch32
     * Windows kernel as well (Windows uses the 64-bit CPU_ON_64 /
     * CPU_SUSPEND_64 / CPU_FN_INFO family).  Accept it.
     */
    if ((param[0] & QEMU_PSCI_0_2_64BIT) && !is_a64(env) &&
        !arm_psci_cpu_on_handler && !arm_psci_64bit_from_a32) {
        ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        goto err;
    }

    switch (param[0]) {
        CPUState *target_cpu_state;
        ARMCPU *target_cpu;

    case QEMU_PSCI_0_2_FN_PSCI_VERSION:
        ret = QEMU_PSCI_VERSION_1_1;
        break;
    case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
        ret = QEMU_PSCI_0_2_RET_TOS_MIGRATION_NOT_REQUIRED; /* No trusted OS */
        break;
    case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
        mpidr = param[1];

        switch (param[2]) {
        case 0:
            target_cpu_state = arm_get_cpu_by_id(mpidr);
            if (!target_cpu_state) {
                ret = QEMU_PSCI_RET_INVALID_PARAMS;
                break;
            }
            target_cpu = ARM_CPU(target_cpu_state);

            g_assert(bql_locked());
            ret = target_cpu->power_state;
            break;
        default:
            /* Everything above affinity level 0 is always on. */
            ret = 0;
        }
        break;
    case QEMU_PSCI_0_2_FN_SYSTEM_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        /* QEMU reset and shutdown are async requests, but PSCI
         * mandates that we never return from the reset/shutdown
         * call, so power the CPU off now so it doesn't execute
         * anything further.
         */
        goto cpu_off;
    case QEMU_PSCI_0_2_FN_SYSTEM_OFF:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        goto cpu_off;
    case QEMU_PSCI_0_1_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN64_CPU_ON:
    {
        /* The PSCI spec mandates that newly brought up CPUs start
         * in the highest exception level which exists and is enabled
         * on the calling CPU. Since the QEMU PSCI implementation is
         * acting as a "fake EL3" or "fake EL2" firmware, this for us
         * means that we want to start at the highest NS exception level
         * that we are providing to the guest.
         * The execution mode should be that which is currently in use
         * by the same exception level on the calling CPU.
         * The CPU should be started with the context_id value
         * in x0 (if AArch64) or r0 (if AArch32).
         */
        int target_el = arm_feature(env, ARM_FEATURE_EL2) ? 2 : 1;
        bool target_aarch64 = arm_el_is_aa64(env, target_el);

        mpidr = param[1];
        entry = param[2];
        context_id = param[3];
        if (arm_psci_cpu_on_handler) {
            ret = arm_psci_cpu_on_handler(mpidr, entry, context_id);
        } else {
            ret = arm_set_cpu_on(mpidr, entry, context_id,
                                 target_el, target_aarch64);
        }
        break;
    }
    case QEMU_PSCI_0_1_FN_CPU_OFF:
    case QEMU_PSCI_0_2_FN_CPU_OFF:
        goto cpu_off;
    case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
        /* Affinity levels are not supported in QEMU */
        if (param[1] & 0xfffe0000) {
            ret = QEMU_PSCI_RET_INVALID_PARAMS;
            break;
        }
        /*
         * Lumia/WOA quirk (only when a board vendor handler is registered):
         * the firmware's MpPark protocol uses CPU_SUSPEND(power_state=0,
         * entry, ctx) as "power-gate me and resume AT entry". Real TZ/HYP
         * restarts the core at `entry` with x0=ctx on wake; the stock WFI
         * emulation below parks the core at the SMC return site and never
         * matches that, deadlocking BootShim's "Waiting for all CPUs".
         * Emulate the QC virtualized powergate: jump to entry immediately.
         */
        if (arm_smc_vendor_handler && param[2]) {
            cpu_set_pc(CPU(cpu), param[2]);
            if (is_a64(env)) {
                env->xregs[0] = param[3];
            } else {
                env->regs[0] = param[3];
            }
            break;
        }
        /* Powerdown is not supported, we always go into WFI */
        if (is_a64(env)) {
            env->xregs[0] = 0;
        } else {
            env->regs[0] = 0;
        }
        helper_wfi(env, 4);
        break;
    case QEMU_PSCI_1_0_FN_PSCI_FEATURES: {
        /*
         * PSCI_FEATURES takes either a plain feature ID (0x00-0x3f) or a
         * function ID.  Windows on ARM32 asks with the function-ID form
         * (0x84000001, 0x8400000b, 0x8400000c, 0x8400000f, 0x84000010,
         * 0x84000011 - i.e. CPU_SUSPEND, *_64, SET_PWR_POLICY_64,
         * CPU_FN_INFO, CPU_FN_INFO_64) and falls back to a different
         * secondary-processor start path when the platform claims not to
         * support the 64-bit functions.  Answer with the feature ID so
         * that the phone's TrustZone-like behaviour is reproduced.
         */
        uint64_t raw = param[1];
        uint32_t id = raw;

        if ((raw & 0xffffff00ULL) == QEMU_PSCI_0_2_FN_BASE ||
            (raw & 0xffffff00ULL) == QEMU_PSCI_0_2_FN64_BASE) {
            id = raw & 0xff;
        }
        switch (id) {
        case 0x00: /* PSCI_VERSION */
        case 0x01: /* CPU_SUSPEND */
        case 0x02: /* CPU_OFF */
        case 0x03: /* CPU_ON */
        case 0x04: /* AFFINITY_INFO */
        case 0x06: /* MIGRATE_INFO_TYPE */
        case 0x08: /* SYSTEM_OFF */
        case 0x09: /* SYSTEM_RESET */
        case 0x0a: /* PSCI_FEATURES */
        case 0x0b: /* CPU_SUSPEND_64 */
        case 0x0c: /* CPU_OFF_64 */
        case 0x0d: /* CPU_ON_64 */
        case 0x0e: /* SET_PWR_POLICY */
        case 0x0f: /* SET_PWR_POLICY_64 */
        case 0x10: /* CPU_FN_INFO */
        case 0x11: /* CPU_FN_INFO_64 */
        case 0x12: /* SYSTEM_OFF_64 */
        case 0x13: /* SYSTEM_RESET_64 */
            ret = 0;
            break;
        default:
            ret = QEMU_PSCI_RET_NOT_SUPPORTED;
            break;
        }
        break;
    }
    case QEMU_PSCI_0_1_FN_MIGRATE:
    case QEMU_PSCI_0_2_FN_MIGRATE:
    default:
        ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        break;
    }

err:
    if (is_a64(env)) {
        env->xregs[0] = ret;
    } else {
        env->regs[0] = ret;
    }
    return;

cpu_off:
    ret = arm_set_cpu_off(arm_cpu_mp_affinity(cpu));
    /* notreached */
    /* sanity check in case something failed */
    assert(ret == QEMU_ARM_POWERCTL_RET_SUCCESS);
}
