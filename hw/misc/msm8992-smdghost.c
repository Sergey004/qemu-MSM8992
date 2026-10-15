/*
 * SMD ghost coprocessor for the msm8992 machine: a periodic timer that plays
 * the role of the RPM firmware against the legacy-SMD "rpm_requests" channel
 * that lives in SMEM (both sides of the protocol documented in QcomPkg's
 * Library/RpmLib and Lumia950XLPkg/Driver/SmdRpmDxe/smd.c).
 *
 * Layout (all offsets relative to the SMEM base we prepopulate in msm8992.c):
 *   ctrl @SMEM+0x31000: two halves (APPS ch0 @+0, RPM ch1 @+0x2c), each a
 *       smd_shared_stream_info_type { state, DTR_DSR, CTS_RTS, CD, RI,
 *       data_written, data_read, state_updated, mask_recv_intr,
 *       read_index, write_index } = 11 x u32.
 *   fifo @SMEM+0x32000: 2 KiB; APPS->RPM in first 1 KiB half, RPM->APPS
 *       replies in the second half (matches smd.c's fifo_buf_size/=2).
 *
 * Reply payload format (RPM wire from QcomPkg RpmLib client_smd/rpm_wire.c):
 *   { uint32 msg_id, set, resource, resource_id, data_len } then kvp payload.
 *   We echo msg_id back with data_len=0 and set=0 — precisely what an
 *   RPM-side acknowledgement looks like for the NPA client.
 *
 * The guest polls ch1.{write_index,read_index} (our recv half's counters) —
 * meaning it waits for "RPM consumed my write or replied".  This model closes
 * that loop: each tick we advance ch0.read_index to ch0.write_index (claimed
 * consumption) and if a full header was consumed we also push a reply into
 * the RPM->APPS fifo, bumping ch1's write_index and pulsing the SMD IRQ.
 *
 * Free Software Foundation; licensed GPL-2.0-only.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/system.h"
#include "system/address-spaces.h"

#define TYPE_MSM8992_SMD_GHOST "msm8992-smd-ghost"
OBJECT_DECLARE_SIMPLE_TYPE(Msm8992SmdGhostState, MSM8992_SMD_GHOST)

#define SMD_GHOST_CTRL_OFF  0x31000
#define SMD_GHOST_FIFO_OFF  0x32000
#define SMD_GHOST_FIFO_HALF 0x1000

struct Msm8992SmdGhostState {
    SysBusDevice parent_obj;
    QEMUTimer   *tick;
    qemu_irq     irq;
    uint32_t     last_ch0_wr;
    uint64_t     reply_serial;
};

static inline uint32_t smd_rd32(hwaddr off)
{
    uint32_t v;
    address_space_read(&address_space_memory,
                       (hwaddr)(0x06A00000ULL) + off,
                       MEMTXATTRS_UNSPECIFIED, &v, 4);
    return le32_to_cpu(v);
}

static inline void smd_wr32(hwaddr off, uint32_t v)
{
    v = cpu_to_le32(v);
    address_space_write(&address_space_memory,
                        (hwaddr)(0x06A00000ULL) + off,
                        MEMTXATTRS_UNSPECIFIED, &v, 4);
}

static void msm8992_smd_ghost_tick(void *opaque)
{
    Msm8992SmdGhostState *s = MSM8992_SMD_GHOST(opaque);

    /* ch0 = APPS->RPM (write side), ch1 = RPM->APPS (reply side) */
    uint32_t ch0_wr = smd_rd32(SMD_GHOST_CTRL_OFF + 0x28);
    uint32_t ch0_rd = smd_rd32(SMD_GHOST_CTRL_OFF + 0x24);

    if (ch0_wr != ch0_rd) {
        /* new outbound packet(s): consume the whole outstanding range, then
         * ACK it back on the reply half. */
        smd_wr32(SMD_GHOST_CTRL_OFF + 0x24, ch0_wr);

        {

            /* the difference here is what we just consumed; we cannot know
             * msg_id w/o parsing stream u8 data - peek at the fifo head. */
            uint8_t hdr[20];
            address_space_read(&address_space_memory,
                               (hwaddr)(0x06A00000ULL) + SMD_GHOST_FIFO_OFF +
                               (ch0_rd % SMD_GHOST_FIFO_HALF),
                               MEMTXATTRS_UNSPECIFIED, hdr, sizeof(hdr));
            /* Reply in the wire format UEFI SmdRpmDxe expects
             * (rpm-smd.c): {rpm_gen_hdr = {type=0x00716572 'req',
             * len=0x0C ACK_MSG_LENGTH}, then the msg id echoed}. */
            uint32_t reply[5] = { 0x00716572u, 0x0Cu,
                                  le32_to_cpu(*(uint32_t *)hdr), 0, 0 };
            uint32_t ch1_wr = smd_rd32(SMD_GHOST_CTRL_OFF + 0x28 + 0x2c);
            address_space_write(&address_space_memory,
                                (hwaddr)(0x06A00000ULL) + SMD_GHOST_FIFO_OFF +
                                SMD_GHOST_FIFO_HALF + (ch1_wr % SMD_GHOST_FIFO_HALF),
                                MEMTXATTRS_UNSPECIFIED, reply, sizeof(reply));
            ch1_wr += 20;
            smd_wr32(SMD_GHOST_CTRL_OFF + 0x24 + 0x2c, ch1_wr);
            smd_wr32(SMD_GHOST_CTRL_OFF + 0x28 + 0x2c, ch1_wr);
            /* signal + state_updated bits helped some readers */
            smd_wr32(SMD_GHOST_CTRL_OFF + 0x1c + 0x2c, 1);
            qemu_set_irq(s->irq, 1);
            qemu_set_irq(s->irq, 0);
        }
    }

    timer_mod(s->tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
}

static void msm8992_smd_ghost_realize(DeviceState *dev, Error **errp)
{
    Msm8992SmdGhostState *s = MSM8992_SMD_GHOST(dev);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->tick = timer_new_ms(QEMU_CLOCK_VIRTUAL, msm8992_smd_ghost_tick, s);
    timer_mod(s->tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

static void msm8992_smd_ghost_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = msm8992_smd_ghost_realize;
    dc->desc = "QC SMD ghost coprocessor (RPM reply channel)";
}

static const TypeInfo msm8992_smd_ghost_info = {
    .name          = TYPE_MSM8992_SMD_GHOST,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Msm8992SmdGhostState),
    .class_init    = msm8992_smd_ghost_class_init,
};

static void msm8992_smd_ghost_register_types(void)
{
    type_register_static(&msm8992_smd_ghost_info);
}
type_init(msm8992_smd_ghost_register_types)
