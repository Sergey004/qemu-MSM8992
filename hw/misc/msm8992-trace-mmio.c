/*
 * msm8992 trace-mmio: RAM-backed MMIO window with deduplicated tracing.
 *
 * Debug aid for bring-up: models an unmapped device as plain memory that
 * latches writes and returns them, while logging every UNIQUE (offset,
 * direction) access exactly once.  Lets us discover which registers a
 * guest phase actually touches (ULPI PHY, SS PHY, BAM...) without flooding
 * the log on spin-polls.
 *
 * Copyright (c) 2026 lumia950 project
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"

#define TYPE_MSM8992_TRACE_MMIO "msm8992-trace-mmio"
OBJECT_DECLARE_SIMPLE_TYPE(TraceMmioState, MSM8992_TRACE_MMIO)

typedef struct TraceMmioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t *storage;

    char *name;
    uint64_t size;
    uint8_t *seen;          /* bit per (offset, write): seen[dir*size + off] */
    uint64_t log_mask;      /* 1 = trace reads too */
} TraceMmioState;

static const Property trace_mmio_props[] = {
    DEFINE_PROP_STRING("name", TraceMmioState, name),
    DEFINE_PROP_UINT64("size", TraceMmioState, size, 0x1000),
    DEFINE_PROP_UINT64("trace-reads", TraceMmioState, log_mask, 0),
};

static void trace_mmio_note(TraceMmioState *s, hwaddr off, uint64_t val,
                            unsigned size, bool is_write)
{
    uint64_t idx = (is_write ? 0 : s->size) + off;
    bool was;

    if (idx >= s->size * 2) {
        return;
    }
    was = s->seen[idx >> 3] & (1u << (idx & 7));
    if (was) {
        return;
    }
    s->seen[idx >> 3] |= 1u << (idx & 7);

    if (!is_write && !s->log_mask) {
        return;
    }
    qemu_log("TRACE-MMIO [%s] %s +0x%04llx = 0x%0*llx (%u bytes)\n",
             s->name, is_write ? "write" : "read",
             (unsigned long long)off,
             (int)(size * 2), (unsigned long long)val, size);
}

static uint64_t trace_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    TraceMmioState *s = MSM8992_TRACE_MMIO(opaque);
    uint64_t val = 0;

    if (off + size <= s->size) {
        memcpy(&val, s->storage + off, size);
    }
    trace_mmio_note(s, off, val, size, false);
    return val;
}

static void trace_mmio_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    TraceMmioState *s = MSM8992_TRACE_MMIO(opaque);

    if (off + size <= s->size) {
        memcpy(s->storage + off, &val, size);
    }
    trace_mmio_note(s, off, val, size, true);
}

static const MemoryRegionOps trace_mmio_ops = {
    .read = trace_mmio_read,
    .write = trace_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void trace_mmio_realize(DeviceState *dev, Error **errp)
{
    TraceMmioState *s = MSM8992_TRACE_MMIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->name) {
        s->name = g_strdup("mmio");
    }
    if (s->size == 0 || s->size > 0x10000000) {
        error_setg(errp, "msm8992-trace-mmio: bad size 0x%llx",
                   (unsigned long long)s->size);
        return;
    }

    s->storage = g_malloc0(s->size);
    /* 2 bits-per-byte tables: writes and reads, one bit per byte offset */
    s->seen = g_malloc0((s->size * 2 + 7) / 8);

    memory_region_init_io(&s->iomem, OBJECT(s), &trace_mmio_ops, s,
                          "msm8992-trace-mmio", s->size);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void trace_mmio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = trace_mmio_realize;
    device_class_set_props(dc, trace_mmio_props);
}

static const TypeInfo trace_mmio_info = {
    .name          = TYPE_MSM8992_TRACE_MMIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TraceMmioState),
    .class_init    = trace_mmio_class_init,
};

static void trace_mmio_register_types(void)
{
    type_register_static(&trace_mmio_info);
}

type_init(trace_mmio_register_types)
