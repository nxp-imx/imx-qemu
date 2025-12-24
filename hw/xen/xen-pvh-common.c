/*
 * QEMU Xen PVH machine - common code.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "system/tpm.h"
#include "system/tpm_backend.h"
#include "system/runstate.h"
#include "hw/xen/xen-pvh-common.h"
#include "trace.h"

static const MemoryListener xen_memory_listener = {
    .region_add = xen_region_add,
    .region_del = xen_region_del,
    .log_start = NULL,
    .log_stop = NULL,
    .log_sync = NULL,
    .log_global_start = NULL,
    .log_global_stop = NULL,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};

static void xen_pvh_init_ram(XenPVHMachineState *s,
                             MemoryRegion *sysmem)
{
    MachineState *ms = MACHINE(s);
    ram_addr_t block_len, ram_size[2];

    if (ms->ram_size <= s->cfg.ram_low.size) {
        ram_size[0] = ms->ram_size;
        ram_size[1] = 0;
        block_len = s->cfg.ram_low.base + ram_size[0];
    } else {
        ram_size[0] = s->cfg.ram_low.size;
        ram_size[1] = ms->ram_size - s->cfg.ram_low.size;
        block_len = s->cfg.ram_high.base + ram_size[1];
    }

    memory_region_init_ram(&xen_memory, NULL, "xen.ram", block_len,
                           &error_fatal);

    memory_region_init_alias(&s->ram.low, NULL, "xen.ram.lo", &xen_memory,
                             s->cfg.ram_low.base, ram_size[0]);
    memory_region_add_subregion(sysmem, s->cfg.ram_low.base, &s->ram.low);
    if (ram_size[1] > 0) {
        memory_region_init_alias(&s->ram.high, NULL, "xen.ram.hi", &xen_memory,
                                 s->cfg.ram_high.base, ram_size[1]);
        memory_region_add_subregion(sysmem, s->cfg.ram_high.base, &s->ram.high);
    }

    /* Setup support for grants.  */
    memory_region_init_ram(&xen_grants, NULL, "xen.grants", block_len,
                           &error_fatal);
    memory_region_add_subregion(sysmem, XEN_GRANT_ADDR_OFF, &xen_grants);
}

static void xen_set_irq(void *opaque, int irq, int level)
{
    if (xendevicemodel_set_irq_level(xen_dmod, xen_domid, irq, level)) {
        error_report("xendevicemodel_set_irq_level failed");
    }
}

static void xen_create_virtio_mmio_devices(XenPVHMachineState *s)
{
    int i;

    /*
     * We create the transports in reverse order. Since qbus_realize()
     * prepends (not appends) new child buses, the decrementing loop below will
     * create a list of virtio-mmio buses with increasing base addresses.
     *
     * When a -device option is processed from the command line,
     * qbus_find_recursive() picks the next free virtio-mmio bus in forwards
     * order.
     *
     * This is what the Xen tools expect.
     */
    for (i = s->cfg.virtio_mmio_num - 1; i >= 0; i--) {
        hwaddr base = s->cfg.virtio_mmio.base + i * s->cfg.virtio_mmio.size;
        qemu_irq irq = qemu_allocate_irq(xen_set_irq, NULL,
                                         s->cfg.virtio_mmio_irq_base + i);

        sysbus_create_simple("virtio-mmio", base, irq);

        trace_xen_create_virtio_mmio_devices(i,
                                             s->cfg.virtio_mmio_irq_base + i,
                                             base);
    }
}

#ifdef CONFIG_TPM
static void xen_enable_tpm(XenPVHMachineState *s)
{
    Error *errp = NULL;
    DeviceState *dev;
    SysBusDevice *busdev;

    TPMBackend *be = qemu_find_tpm_be("tpm0");
    if (be == NULL) {
        error_report("Couldn't find tmp0 backend");
        return;
    }
    dev = qdev_new(TYPE_TPM_TIS_SYSBUS);
    object_property_set_link(OBJECT(dev), "tpmdev", OBJECT(be), &errp);
    object_property_set_str(OBJECT(dev), "tpmdev", be->id, &errp);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, s->cfg.tpm.base);

    trace_xen_enable_tpm(s->cfg.tpm.base);
}
#endif

/*
 * We use the GPEX PCIe controller with its internal INTX PCI interrupt
 * swizzling. This swizzling is emulated in QEMU and routes all INTX
 * interrupts from endpoints down to only 4 INTX interrupts.
 * See include/hw/pci/pci.h : pci_swizzle()
 */
static inline void xenpvh_gpex_init(XenPVHMachineState *s,
                                    XenPVHMachineClass *xpc,
                                    MemoryRegion *sysmem)
{
    hwaddr base_ecam = s->cfg.pci_ecam.base;
    hwaddr size_ecam = s->cfg.pci_ecam.size;
    hwaddr base_mmio = s->cfg.pci_mmio.base;
    hwaddr size_mmio = s->cfg.pci_mmio.size;
    hwaddr base_mmio_high = s->cfg.pci_mmio_high.base;
    hwaddr size_mmio_high = s->cfg.pci_mmio_high.size;
    DeviceState *dev;
    MemoryRegion *mmio_alias, *mmio_alias_high, *mmio_reg;
    MemoryRegion *ecam_alias, *ecam_reg;
    int i;

    dev = qdev_new(TYPE_GPEX_HOST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* Map ECAM space */
    ecam_alias = g_new0(MemoryRegion, 1);
    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, size_ecam);
    memory_region_add_subregion(get_system_memory(), base_ecam, ecam_alias);

    /* Map the MMIO space */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, base_mmio, size_mmio);
    memory_region_add_subregion(get_system_memory(), base_mmio, mmio_alias);

    /* Map the MMIO_HIGH space */
    mmio_alias_high = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mmio_alias_high, OBJECT(dev), "pcie-mmio-high",
                             mmio_reg, base_mmio_high, size_mmio_high);
    memory_region_add_subregion(get_system_memory(), base_mmio_high,
                                mmio_alias_high);

    /* Legacy PCI interrupts (#INTA - #INTD) */
    for (i = 0; i < PCI_NUM_PINS; i++) {
        qemu_irq irq = qemu_allocate_irq(xpc->set_pci_intx_irq, s, i);

        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, irq);
        gpex_set_irq_num(GPEX_HOST(dev), i, s->cfg.pci_intx_irq_base + i);
    }

    DPRINTF("Created PCIe host bridge\n");
}

static int xen_get_pcie_params(XenPVHMachineState *s)
{
    char path[128], *value = NULL, **entries = NULL;
    unsigned int len, tmp;
    int rc = -1;

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host",
             xen_domid);
    DPRINTF("%s %s\n", __func__, path);
    entries = xs_directory(s->ioreq.xenstore, XBT_NULL, path, &len);
    DPRINTF("%s %s ccccccc\n", __func__, path);
    if (entries == NULL) {
        error_report("xenpv: 'virtio_pci_host' dir is not present");
        return -1;
    }
    DPRINTF("%s 1\n", __func__);
    free(entries);
    if (len != 9) {
        error_report("xenpv: Unexpected num of entries in 'virtio_pci_host' dir");
        goto out;
    }

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/id",
             xen_domid);
    DPRINTF("%s %s 2\n", __func__, path);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtoui(value, NULL, 10, &tmp)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/id' prop");
        goto out;
    }
    DPRINTF("%s %s 3\n", __func__, path);
    free(value);
    value = NULL;
    if (tmp > 0xffff) {
        error_report("xenpv: Wrong 'virtio_pci_host/id' value %u", tmp);
        goto out;
    }
    xen_pci_segment = tmp;
    DPRINTF("PCIe host bridge: id (xen_pci_segment) %u\n", xen_pci_segment);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/ecam_base",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_ecam.base)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/ecam_base' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: ecam_base 0x%lx\n", s->cfg.pci_ecam.base);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/ecam_size",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_ecam.size)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/ecam_size' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: ecam_size 0x%lx\n", s->cfg.pci_ecam.size);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/mem_base",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_mmio.base)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/mem_base' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: mem_base 0x%lx\n", s->cfg.pci_mmio.base);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/mem_size",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_mmio.size)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/mem_size' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: mem_size 0x%lx\n", s->cfg.pci_mmio.size);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/prefetch_mem_base",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_mmio_high.base)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/prefetch_mem_base' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: prefetch_mem_base 0x%lx\n", s->cfg.pci_mmio_high.base);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/prefetch_mem_size",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtou64(value, NULL, 16, &s->cfg.pci_mmio_high.size)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/prefetch_mem_size' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: prefetch_mem_size 0x%lx\n", s->cfg.pci_mmio_high.size);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/irq_first",
             xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtoui(value, NULL, 10, &s->cfg.pci_intx_irq_base)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/irq_first' prop");
        goto out;
    }
    free(value);
    value = NULL;
    DPRINTF("PCIe host bridge: irq_first %u\n", s->cfg.pci_intx_irq_base);

    snprintf(path, sizeof(path), "device-model/%d/virtio_pci_host/num_irqs", xen_domid);
    value = xs_read(s->ioreq.xenstore, XBT_NULL, path, &len);
    if (qemu_strtoui(value, NULL, 10, &tmp)) {
        error_report("xenpv: Failed to get 'virtio_pci_host/num_irqs' prop");
        goto out;
    }
    free(value);
    value = NULL;
    if (tmp != PCI_NUM_PINS) {
        error_report("xenpv: Wrong 'virtio_pci_host/num_irqs' value %u", tmp);
        goto out;
    }
    DPRINTF("PCIe host bridge: num_irqs %u\n", tmp);

    rc = 0;
out:
    if (value)
        free(value);

    return rc;
}
 
static void xen_pvh_init(MachineState *ms)
{
    XenPVHMachineState *s = XEN_PVH_MACHINE(ms);
    XenPVHMachineClass *xpc = XEN_PVH_MACHINE_GET_CLASS(s);
    MemoryRegion *sysmem = get_system_memory();

    if (ms->ram_size == 0) {
        warn_report("%s: ram size not specified. QEMU machine started"
                    " without IOREQ (no emulated devices including virtio)",
                    MACHINE_CLASS(object_get_class(OBJECT(ms)))->desc);
        return;
    }

    xen_pvh_init_ram(s, sysmem);
    xen_register_ioreq(&s->ioreq, ms->smp.max_cpus,
                       xpc->handle_bufioreq,
                       &xen_memory_listener);

    if (s->cfg.virtio_mmio_num) {
        xen_create_virtio_mmio_devices(s);
    }

    xen_get_pcie_params(s);

#ifdef CONFIG_TPM
    if (xpc->has_tpm) {
        if (s->cfg.tpm.base) {
            xen_enable_tpm(s);
        } else {
            warn_report("tpm-base-addr is not set. TPM will not be enabled");
        }
    }
#endif

    /* Non-zero pci-ecam-size enables PCI.  */
    if (s->cfg.pci_ecam.size) {
        if (s->cfg.pci_ecam.size != 256 * MiB) {
            error_report("pci-ecam-size only supports values 0 or 0x10000000");
            //exit(EXIT_FAILURE);
        }
        if (!s->cfg.pci_intx_irq_base) {
            error_report("PCI enabled but pci-intx-irq-base not set");
            exit(EXIT_FAILURE);
        }

        xenpvh_gpex_init(s, xpc, sysmem);
    }

    /* Call the implementation specific init.  */
    if (xpc->init) {
        xpc->init(ms);
    }
}

#define XEN_PVH_PROP_MEMMAP_SETTER(n, f)                                   \
static void xen_pvh_set_ ## n ## _ ## f(Object *obj, Visitor *v,           \
                                       const char *name, void *opaque,     \
                                       Error **errp)                       \
{                                                                          \
    XenPVHMachineState *xp = XEN_PVH_MACHINE(obj);                         \
    uint64_t value;                                                        \
                                                                           \
    if (!visit_type_size(v, name, &value, errp)) {                         \
        return;                                                            \
    }                                                                      \
    xp->cfg.n.f = value;                                                   \
}

#define XEN_PVH_PROP_MEMMAP_GETTER(n, f)                                   \
static void xen_pvh_get_ ## n ## _ ## f(Object *obj, Visitor *v,           \
                                       const char *name, void *opaque,     \
                                       Error **errp)                       \
{                                                                          \
    XenPVHMachineState *xp = XEN_PVH_MACHINE(obj);                         \
    uint64_t value = xp->cfg.n.f;                                          \
                                                                           \
    visit_type_uint64(v, name, &value, errp);                              \
}

#define XEN_PVH_PROP_MEMMAP_BASE(n)        \
    XEN_PVH_PROP_MEMMAP_SETTER(n, base)    \
    XEN_PVH_PROP_MEMMAP_GETTER(n, base)    \

#define XEN_PVH_PROP_MEMMAP_SIZE(n)        \
    XEN_PVH_PROP_MEMMAP_SETTER(n, size)    \
    XEN_PVH_PROP_MEMMAP_GETTER(n, size)

#define XEN_PVH_PROP_MEMMAP(n)             \
    XEN_PVH_PROP_MEMMAP_BASE(n)            \
    XEN_PVH_PROP_MEMMAP_SIZE(n)

XEN_PVH_PROP_MEMMAP(ram_low)
XEN_PVH_PROP_MEMMAP(ram_high)
/* TPM only has a base-addr option.  */
XEN_PVH_PROP_MEMMAP_BASE(tpm)
XEN_PVH_PROP_MEMMAP(virtio_mmio)
XEN_PVH_PROP_MEMMAP(pci_ecam)
XEN_PVH_PROP_MEMMAP(pci_mmio)
XEN_PVH_PROP_MEMMAP(pci_mmio_high)

static void xen_pvh_set_pci_intx_irq_base(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    XenPVHMachineState *xp = XEN_PVH_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    xp->cfg.pci_intx_irq_base = value;
}

static void xen_pvh_get_pci_intx_irq_base(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    XenPVHMachineState *xp = XEN_PVH_MACHINE(obj);
    uint32_t value = xp->cfg.pci_intx_irq_base;

    visit_type_uint32(v, name, &value, errp);
}

void xen_pvh_class_setup_common_props(XenPVHMachineClass *xpc)
{
    ObjectClass *oc = OBJECT_CLASS(xpc);
    MachineClass *mc = MACHINE_CLASS(xpc);

#define OC_MEMMAP_PROP_BASE(c, prop_name, name)                           \
do {                                                                      \
    object_class_property_add(c, prop_name "-base", "uint64_t",           \
                              xen_pvh_get_ ## name ## _base,              \
                              xen_pvh_set_ ## name ## _base, NULL, NULL); \
    object_class_property_set_description(oc, prop_name "-base",          \
                              "Set base address for " prop_name);         \
} while (0)

#define OC_MEMMAP_PROP_SIZE(c, prop_name, name)                           \
do {                                                                      \
    object_class_property_add(c, prop_name "-size", "uint64_t",           \
                              xen_pvh_get_ ## name ## _size,              \
                              xen_pvh_set_ ## name ## _size, NULL, NULL); \
    object_class_property_set_description(oc, prop_name "-size",          \
                              "Set memory range size for " prop_name);    \
} while (0)

#define OC_MEMMAP_PROP(c, prop_name, name)                                \
do {                                                                      \
        OC_MEMMAP_PROP_BASE(c, prop_name, name);                          \
        OC_MEMMAP_PROP_SIZE(c, prop_name, name);                          \
} while (0)

    /*
     * We provide memmap properties to allow Xen to move things to other
     * addresses for example when users need to accomodate the memory-map
     * for 1:1 mapped devices/memory.
     */
    OC_MEMMAP_PROP(oc, "ram-low", ram_low);
    OC_MEMMAP_PROP(oc, "ram-high", ram_high);

    if (xpc->has_virtio_mmio) {
        OC_MEMMAP_PROP(oc, "virtio-mmio", virtio_mmio);
    }

    if (xpc->has_pci) {
        OC_MEMMAP_PROP(oc, "pci-ecam", pci_ecam);
        OC_MEMMAP_PROP(oc, "pci-mmio", pci_mmio);
        OC_MEMMAP_PROP(oc, "pci-mmio-high", pci_mmio_high);

        object_class_property_add(oc, "pci-intx-irq-base", "uint32_t",
                                  xen_pvh_get_pci_intx_irq_base,
                                  xen_pvh_set_pci_intx_irq_base,
                                  NULL, NULL);
        object_class_property_set_description(oc, "pci-intx-irq-base",
                                  "Set PCI INTX interrupt base line.");
    }

#ifdef CONFIG_TPM
    if (xpc->has_tpm) {
        object_class_property_add(oc, "tpm-base-addr", "uint64_t",
                                  xen_pvh_get_tpm_base,
                                  xen_pvh_set_tpm_base,
                                  NULL, NULL);
        object_class_property_set_description(oc, "tpm-base-addr",
                                  "Set Base address for TPM device.");

        machine_class_allow_dynamic_sysbus_dev(mc, TYPE_TPM_TIS_SYSBUS);
    }
#endif
}

static void xen_pvh_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = xen_pvh_init;

    mc->desc = "Xen PVH machine";
    mc->max_cpus = GUEST_MAX_VCPUS;
    mc->default_machine_opts = "accel=xen";
    /* Set to zero to make sure that the real ram size is passed. */
    mc->default_ram_size = 0;
}

static const TypeInfo xen_pvh_info = {
    .name = TYPE_XEN_PVH_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(XenPVHMachineState),
    .class_size = sizeof(XenPVHMachineClass),
    .class_init = xen_pvh_class_init,
};

static void xen_pvh_register_types(void)
{
    type_register_static(&xen_pvh_info);
}

type_init(xen_pvh_register_types);
