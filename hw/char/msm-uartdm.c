/*
 * Minimal Qualcomm UARTDM (BLSP UART, qcom,msm-lsuart-v14) model.
 *
 * Register layout per kernel_msm drivers/tty/serial/msm_serial_hs_hwreg.h
 * (bullhead branch — the exact IP revision shipped in MSM8992).
 *
 * Scope: enough for firmware/earlycon to talk on the DBG2 console:
 *   - TX: 32-bit writes to UARTDM_TF push bytes straight to the chardev,
 *     SR always reports TX_EMPTY|TX_READY (instant transmit);
 *   - RX: chardev input lands in a 64-byte FIFO, SR/ISR report RXLEV;
 *   - CR commands (reset rx/tx, enable/disable) and the mode/DMA registers
 *     are accepted and stored; their effects (framing, DMA) are ignored.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "qemu/fifo8.h"

#define TYPE_MSM_UARTDM "msm-uartdm"
OBJECT_DECLARE_SIMPLE_TYPE(MsmUartDmState, MSM_UARTDM)

/* Register offsets (msm_serial_hs_hwreg.h) */
#define UARTDM_MR1        0x00
#define UARTDM_MR2        0x04
#define UARTDM_CSR_SR     0x08    /* write: CSR (baud), read: SR */
/* UART_DM later register layout ("msm_serial_hs" / v1.4+ MSM flavor):
 * stock Lumia UEFI (qcboot) uses this map, not the classic one. */
#define UARTDM2_IMR       0xB0
#define UARTDM2_SR        0xA4
#define UARTDM2_CR        0xA8
#define UARTDM2_CSR       0xA0
#define UARTDM2_ISR       0xB4
#define UARTDM2_MISR      0xAC
#define UARTDM2_TF        0x100
#define UARTDM2_TF_END    0x10C
#define UARTDM2_RF        0x140
#define UARTDM_RF_TF      0x70    /* read: RF, write: TF (32-bit windows) */
#define UARTDM_RF_TF1     0x74
#define UARTDM_RF_TF2     0x78
#define UARTDM_RF_TF3     0x7C
#define UARTDM_MISR_CR    0x10    /* write: CR, read: MISR */
#define UARTDM_IMR_ISR    0x14    /* write: IMR, read: ISR */
#define UARTDM_IPR        0x18
#define UARTDM_TFWR       0x1C
#define UARTDM_RFWR       0x20
#define UARTDM_HCR        0x24
#define UARTDM_DMRX       0x34
#define UARTDM_RXTSNAP    0x38
#define UARTDM_DMEN       0x3C
#define UARTDM_NCF_TX     0x40
#define UARTDM_BADR       0x44
#define UARTDM_TXFS       0x4C
#define UARTDM_RXFS       0x50
#define UARTDM_SIM_CFG    0x80
#define UARTDM_BCR        0xC8
#define UARTDM_RXTCTRL    0xCC

/* SR bits (msm_serial.h) */
#define SR_HUNT_CHAR      (1 << 7)
#define SR_RX_BREAK       (1 << 6)
#define SR_PAR_FRAME_ERR  (1 << 5)
#define SR_OVERRUN        (1 << 4)
#define SR_TX_EMPTY       (1 << 3)
#define SR_TX_READY       (1 << 2)
#define SR_RX_FULL        (1 << 1)
#define SR_RX_READY       (1 << 0)

/* ISR bits */
#define ISR_TX_READY      (1 << 7)
#define ISR_TXLEV         (1 << 0)
#define ISR_CURRENT_CTS   (1 << 6)
#define ISR_DELTA_CTS     (1 << 5)
#define ISR_RXSTALE       (1 << 3)
#define ISR_RXLEV         (1 << 0)

/* IMR bits we honor */
#define IMR_TXLEV         (1 << 0)
#define IMR_RXSTALE       (1 << 3)
#define IMR_RXLEV         (1 << 4)

/* CR fields (msm_serial.h) */
#define CR_CMD_MASK       (0xFF << 4)
#define CR_CMD_RESET_RX   (1 << 4)
#define CR_CMD_RESET_TX   (2 << 4)
#define CR_CMD_RESET_ERR  (3 << 4)
#define CR_CMD_RESET_STALE (8 << 4)
#define CR_TX_DISABLE     (1 << 3)
#define CR_TX_ENABLE      (1 << 2)
#define CR_RX_DISABLE     (1 << 1)
#define CR_RX_ENABLE      (1 << 0)

struct MsmUartDmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;

    uint32_t mr1, mr2, csr, imr;
    uint32_t dmrx, dmen, ncf_tx, badr, ipr, tfwr, rfwr, hcr;
    uint32_t bcr, rxtctrl, sim_cfg;
    bool tx_enabled, rx_enabled;

    Fifo8 rx_fifo;                 /* backed by the chardev */
};

static void msm_uartdm_update_irq(MsmUartDmState *s)
{
    bool level = false;

    if ((s->imr & IMR_RXLEV) && !fifo8_is_empty(&s->rx_fifo)) {
        level = true;                             /* RXLEV */
    }
    if (s->imr & IMR_TXLEV) {
        level = true;                             /* TX FIFO is always ready */
    }
    qemu_set_irq(s->irq, level);
}

static uint64_t msm_uartdm_read(void *opaque, hwaddr offset, unsigned size)
{
    MsmUartDmState *s = MSM_UARTDM(opaque);
    uint32_t v = 0;

    switch (offset) {
    case UARTDM_MR1:     return s->mr1;
    case UARTDM_MR2:     return s->mr2;
    case UARTDM_CSR_SR:
        v = SR_TX_EMPTY | (s->tx_enabled ? SR_TX_READY : 0);
        if (!fifo8_is_empty(&s->rx_fifo)) {
            v |= SR_RX_READY;
        }
        return v;
    case UARTDM_IMR_ISR:
    case UARTDM2_ISR:
        return ISR_TX_READY | ISR_TXLEV | ISR_CURRENT_CTS |
               (fifo8_is_empty(&s->rx_fifo) ? 0 : ISR_RXLEV);
    case UARTDM2_SR:
        v = SR_TX_EMPTY | (s->tx_enabled ? SR_TX_READY : 0);
        if (!fifo8_is_empty(&s->rx_fifo)) {
            v |= SR_RX_READY;
        }
        return v;
    case UARTDM2_MISR:
        return s->imr;                       /* masked ISR shadow */
    case UARTDM_RF_TF:
    case UARTDM_RF_TF1:
    case UARTDM_RF_TF2:
    case UARTDM_RF_TF3:
    case UARTDM2_RF:
    case 0x144:
    case 0x148:
    case 0x14C: {
        /* pack up to 4 bytes from the rx FIFO */
        int i;
        for (i = 0; i < 4 && !fifo8_is_empty(&s->rx_fifo); i++) {
            v |= fifo8_pop(&s->rx_fifo) << (i * 8);
        }
        msm_uartdm_update_irq(s);
        return v;
    }
    case UARTDM_IPR:     return s->ipr;
    case UARTDM_TFWR:    return s->tfwr;
    case UARTDM_RFWR:    return s->rfwr;
    case UARTDM_HCR:     return s->hcr;
    case UARTDM_DMRX:    return s->dmrx;
    case UARTDM_RXTSNAP: return fifo8_num_used(&s->rx_fifo);
    case UARTDM_DMEN:    return s->dmen;
    case UARTDM_NCF_TX:  return s->ncf_tx;
    case UARTDM_BADR:    return s->badr;
    case UARTDM_TXFS:    return 0;               /* TX FIFO empty */
    case UARTDM_RXFS:    return fifo8_num_used(&s->rx_fifo);
    case UARTDM_SIM_CFG: return s->sim_cfg;
    case UARTDM_BCR:     return s->bcr;
    case UARTDM_RXTCTRL: return s->rxtctrl;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-uartdm: read 0x%02" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void msm_uartdm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    MsmUartDmState *s = MSM_UARTDM(opaque);

    switch (offset) {
    case UARTDM_MR1: s->mr1 = value; break;
    case UARTDM_MR2: s->mr2 = value; break;
    case UARTDM_CSR_SR: s->csr = value; break;
    case UARTDM_MISR_CR:
    case UARTDM2_CR: {
        uint32_t cmd = value & CR_CMD_MASK;
        if (cmd == CR_CMD_RESET_RX) {
            fifo8_reset(&s->rx_fifo);
        }
        if (cmd == CR_CMD_RESET_TX) {
            /* instant TX: nothing to clear */
        }
        if (value & CR_TX_ENABLE)  s->tx_enabled = true;
        if (value & CR_TX_DISABLE) s->tx_enabled = false;
        if (value & CR_RX_ENABLE)  s->rx_enabled = true;
        if (value & CR_RX_DISABLE) s->rx_enabled = false;
        break;
    }
    case UARTDM_IMR_ISR:
    case UARTDM2_IMR:
        s->imr = value;
        msm_uartdm_update_irq(s);
        break;
    case UARTDM2_CSR:
        s->csr = value;
        break;
    case UARTDM_RF_TF:
    case UARTDM_RF_TF1:
    case UARTDM_RF_TF2:
    case UARTDM_RF_TF3:
    case UARTDM2_TF:
    case 0x104:
    case 0x108:
    case 0x10C: {
        if (s->tx_enabled && qemu_chr_fe_backend_connected(&s->chr)) {
            /*
             * Real FIFO words are 4 bytes; polling writers (msm earlycon /
             * UEFI debug print) push one character per word, batched ASCII
             * writers fill the whole word. Emit up to the last non-NUL byte
             * of each word - NULs are padding in every known driver style.
             */
            uint8_t ch[4];
            int i, n = 3;
            for (i = 0; i < 4; i++) {
                ch[i] = (value >> (i * 8)) & 0xFF;
            }
            while (n > 0 && ch[n] == 0) {
                n--;
            }
            for (i = 0; i <= n && ch[i] != 0; i++) {
                qemu_chr_fe_write(&s->chr, &ch[i], 1);
            }
        }
        break;
    }
    case UARTDM_IPR:     s->ipr = value; break;
    case UARTDM_TFWR:    s->tfwr = value; break;
    case UARTDM_RFWR:    s->rfwr = value; break;
    case UARTDM_HCR:     s->hcr = value; break;
    case UARTDM_DMRX:    s->dmrx = value; break;
    case UARTDM_DMEN:    s->dmen = value; break;
    case UARTDM_NCF_TX:  s->ncf_tx = value; break;
    case UARTDM_BADR:    s->badr = value; break;
    case UARTDM_SIM_CFG: s->sim_cfg = value; break;
    case UARTDM_BCR:     s->bcr = value; break;
    case UARTDM_RXTCTRL: s->rxtctrl = value; break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-uartdm: write 0x%02" HWADDR_PRIx
                      " = 0x%08" PRIx64 "\n", offset, value);
        break;
    }
}

static const MemoryRegionOps msm_uartdm_ops = {
    .read = msm_uartdm_read,
    .write = msm_uartdm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static int msm_uartdm_can_receive(void *opaque)
{
    MsmUartDmState *s = MSM_UARTDM(opaque);
    return fifo8_num_free(&s->rx_fifo);
}

static void msm_uartdm_receive(void *opaque, const uint8_t *buf, int size)
{
    MsmUartDmState *s = MSM_UARTDM(opaque);
    if (s->rx_enabled) {
        fifo8_push_all(&s->rx_fifo, buf, size);
    }
    msm_uartdm_update_irq(s);
}

static void msm_uartdm_reset(DeviceState *dev)
{
    MsmUartDmState *s = MSM_UARTDM(dev);
    s->mr1 = s->mr2 = s->csr = s->imr = 0;
    s->dmrx = s->dmen = s->ncf_tx = s->badr = 0;
    s->ipr = s->tfwr = s->rfwr = s->hcr = 0;
    s->bcr = s->rxtctrl = s->sim_cfg = 0;
    s->tx_enabled = false;
    s->rx_enabled = false;
    fifo8_reset(&s->rx_fifo);
}

static void msm_uartdm_realize(DeviceState *dev, Error **errp)
{
    MsmUartDmState *s = MSM_UARTDM(dev);

    qemu_chr_fe_set_handlers(&s->chr, msm_uartdm_can_receive,
                             msm_uartdm_receive, NULL, NULL, s, NULL, true);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(dev), &msm_uartdm_ops, s,
                          TYPE_MSM_UARTDM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msm_uartdm_init(Object *obj)
{
    MsmUartDmState *s = MSM_UARTDM(obj);
    fifo8_create(&s->rx_fifo, 64);
}

static void msm_uartdm_finalize(Object *obj)
{
    MsmUartDmState *s = MSM_UARTDM(obj);
    fifo8_destroy(&s->rx_fifo);
}

static const Property msm_uartdm_properties[] = {
    DEFINE_PROP_CHR("chardev", MsmUartDmState, chr),
};

static void msm_uartdm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, msm_uartdm_properties);
    device_class_set_legacy_reset(dc, msm_uartdm_reset);
    dc->realize = msm_uartdm_realize;
    dc->desc = "Qualcomm UARTDM (BLSP, msm-lsuart-v14)";
}

static const TypeInfo msm_uartdm_info = {
    .name          = TYPE_MSM_UARTDM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmUartDmState),
    .instance_init = msm_uartdm_init,
    .instance_finalize = msm_uartdm_finalize,
    .class_init    = msm_uartdm_class_init,
};

static void msm_uartdm_register_types(void)
{
    type_register_static(&msm_uartdm_info);
}

type_init(msm_uartdm_register_types)
