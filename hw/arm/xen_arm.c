/*
 * QEMU ARM Xen PVH Machine
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "sysemu/tpm_backend.h"
#include "sysemu/sysemu.h"
#include "hw/xen/xen-hvm-common.h"
#include "sysemu/tpm.h"
#include "hw/xen/arch_hvm.h"
#include "hw/pci-host/gpex.h"
#include "hw/virtio/virtio-pci.h"

#define TYPE_XEN_ARM  MACHINE_TYPE_NAME("xenpvh")
OBJECT_DECLARE_SIMPLE_TYPE(XenArmState, XEN_ARM)

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

struct XenArmState {
    /*< private >*/
    MachineState parent;

    XenIOState *state;

    struct {
        uint64_t tpm_base_addr;
    } cfg;

    const MemMapEntry *memmap;
    const int *irqmap;
};

static MemoryRegion ram_lo, ram_hi;

/*
 * VIRTIO_MMIO_DEV_SIZE is imported from tools/libs/light/libxl_arm.c under Xen
 * repository.
 *
 * Origin: git://xenbits.xen.org/xen.git 2128143c114c
 */
#define VIRTIO_MMIO_DEV_SIZE   0x200

#define VIRTIO_MMIO_IDX	       0
#define VIRT_PCIE              1
#define VIRT_PCIE_MMIO         2
#define VIRT_PCIE_ECAM         3
#define VIRT_PCIE_MMIO_HIGH    4

#define NR_VIRTIO_MMIO_DEVICES   \
   (GUEST_VIRTIO_MMIO_SPI_LAST - GUEST_VIRTIO_MMIO_SPI_FIRST)

static const MemMapEntry xen_memmap[] = {
    [VIRTIO_MMIO_IDX]     = { GUEST_VIRTIO_MMIO_BASE, VIRTIO_MMIO_DEV_SIZE },
    [VIRT_PCIE_MMIO]      = { GUEST_VPCI_MEM_ADDR, GUEST_VPCI_MEM_SIZE },
    [VIRT_PCIE_ECAM]      = { GUEST_VPCI_ECAM_BASE, GUEST_VPCI_ECAM_SIZE },
    [VIRT_PCIE_MMIO_HIGH] = { GUEST_VPCI_PREFETCH_MEM_ADDR, GUEST_VPCI_PREFETCH_MEM_SIZE },
};

static const int xen_irqmap[] = {
    [VIRTIO_MMIO_IDX] = GUEST_VIRTIO_MMIO_SPI_FIRST, /* ...to GUEST_VIRTIO_MMIO_SPI_LAST - 1 */
    [VIRT_PCIE]       = GUEST_VIRTIO_PCI_SPI_FIRST,  /* ...to GUEST_VIRTIO_PCI_SPI_LAST - 1 */
};

static void xen_set_irq(void *opaque, int irq, int level)
{
    if (xendevicemodel_set_irq_level(xen_dmod, xen_domid, irq, level)) {
        error_report("xendevicemodel_set_irq_level failed");
    }
}

static void xen_create_virtio_mmio_devices(XenArmState *xam)
{
    int i;

    for (i = 0; i < NR_VIRTIO_MMIO_DEVICES; i++) {
        hwaddr base = GUEST_VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_DEV_SIZE;
        qemu_irq irq = qemu_allocate_irq(xen_set_irq, NULL,
                                         GUEST_VIRTIO_MMIO_SPI_FIRST + i);

        sysbus_create_simple("virtio-mmio", base, irq);

        DPRINTF("Created virtio-mmio device %d: irq %d base 0x%lx\n",
                i, GUEST_VIRTIO_MMIO_SPI_FIRST + i, base);
    }
}

static void xen_init_ram(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    ram_addr_t block_len, ram_size[GUEST_RAM_BANKS];

    if (machine->ram_size <= GUEST_RAM0_SIZE) {
        ram_size[0] = machine->ram_size;
        ram_size[1] = 0;
        block_len = GUEST_RAM0_BASE + ram_size[0];
    } else {
        ram_size[0] = GUEST_RAM0_SIZE;
        ram_size[1] = machine->ram_size - GUEST_RAM0_SIZE;
        block_len = GUEST_RAM1_BASE + ram_size[1];
    }

    memory_region_init_ram(&ram_memory, NULL, "xen.ram", block_len,
                           &error_fatal);

    memory_region_init_alias(&ram_lo, NULL, "xen.ram.lo", &ram_memory,
                             GUEST_RAM0_BASE, ram_size[0]);
    memory_region_add_subregion(sysmem, GUEST_RAM0_BASE, &ram_lo);
    DPRINTF("Initialized region xen.ram.lo: base 0x%llx size 0x%lx\n",
            GUEST_RAM0_BASE, ram_size[0]);

    if (ram_size[1] > 0) {
        memory_region_init_alias(&ram_hi, NULL, "xen.ram.hi", &ram_memory,
                                 GUEST_RAM1_BASE, ram_size[1]);
        memory_region_add_subregion(sysmem, GUEST_RAM1_BASE, &ram_hi);
        DPRINTF("Initialized region xen.ram.hi: base 0x%llx size 0x%lx\n",
                GUEST_RAM1_BASE, ram_size[1]);
    }

    /* Add grant mappings as a pseudo RAM region. */
    ram_grants = xen_init_grant_ram();
}

static void xen_create_pcie(XenArmState *xam)
{
    hwaddr base_ecam = xam->memmap[VIRT_PCIE_ECAM].base;
    hwaddr size_ecam = xam->memmap[VIRT_PCIE_ECAM].size;
    hwaddr base_mmio = xam->memmap[VIRT_PCIE_MMIO].base;
    hwaddr size_mmio = xam->memmap[VIRT_PCIE_MMIO].size;
    hwaddr base_mmio_high = xam->memmap[VIRT_PCIE_MMIO_HIGH].base;
    hwaddr size_mmio_high = xam->memmap[VIRT_PCIE_MMIO_HIGH].size;
    MemoryRegion *mmio_alias, *mmio_alias_high, *mmio_reg;
    MemoryRegion *ecam_alias, *ecam_reg;
    DeviceState *dev;
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
    for (i = 0; i < GPEX_NUM_IRQS; i++) {
        qemu_irq irq = qemu_allocate_irq(xen_set_irq, NULL,
                                         xam->irqmap[VIRT_PCIE] + i);

        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, irq);
        gpex_set_irq_num(GPEX_HOST(dev), i, xam->irqmap[VIRT_PCIE] + i);
    }

    DPRINTF("Created PCIe host bridge\n");
}

void arch_handle_ioreq(XenIOState *state, ioreq_t *req)
{
    hw_error("Invalid ioreq type 0x%x\n", req->type);

    return;
}

void arch_xen_set_memory(XenIOState *state, MemoryRegionSection *section,
                         bool add)
{
}

void xen_hvm_modified_memory(ram_addr_t start, ram_addr_t length)
{
}

void qmp_xen_set_global_dirty_log(bool enable, Error **errp)
{
}

#ifdef CONFIG_TPM
static void xen_enable_tpm(XenArmState *xam)
{
    Error *errp = NULL;
    DeviceState *dev;
    SysBusDevice *busdev;

    TPMBackend *be = qemu_find_tpm_be("tpm0");
    if (be == NULL) {
        DPRINTF("Couldn't fine the backend for tpm0\n");
        return;
    }
    dev = qdev_new(TYPE_TPM_TIS_SYSBUS);
    object_property_set_link(OBJECT(dev), "tpmdev", OBJECT(be), &errp);
    object_property_set_str(OBJECT(dev), "tpmdev", be->id, &errp);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, xam->cfg.tpm_base_addr);

    DPRINTF("Connected tpmdev at address 0x%lx\n", xam->cfg.tpm_base_addr);
}
#endif

static void xen_arm_init(MachineState *machine)
{
    XenArmState *xam = XEN_ARM(machine);

    xam->state =  g_new0(XenIOState, 1);
    xam->memmap = xen_memmap;
    xam->irqmap = xen_irqmap;

    if (machine->ram_size == 0) {
        DPRINTF("ram_size not specified. QEMU machine started without IOREQ"
                "(no emulated devices including Virtio)\n");
        return;
    }

    xen_init_ram(machine);

    xen_register_ioreq(xam->state, machine->smp.cpus, &xen_memory_listener);

    xen_create_virtio_mmio_devices(xam);
    xen_create_pcie(xam);

#ifdef CONFIG_TPM
    if (xam->cfg.tpm_base_addr) {
        xen_enable_tpm(xam);
    } else {
        DPRINTF("tpm-base-addr is not provided. TPM will not be enabled\n");
    }
#endif
}

#ifdef CONFIG_TPM
static void xen_arm_get_tpm_base_addr(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    XenArmState *xam = XEN_ARM(obj);
    uint64_t value = xam->cfg.tpm_base_addr;

    visit_type_uint64(v, name, &value, errp);
}

static void xen_arm_set_tpm_base_addr(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    XenArmState *xam = XEN_ARM(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    xam->cfg.tpm_base_addr = value;
}
#endif

static void xen_arm_machine_class_init(ObjectClass *oc, void *data)
{

    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Xen Para-virtualized PC";
    mc->init = xen_arm_init;
    mc->max_cpus = GUEST_MAX_VCPUS;
    mc->default_machine_opts = "accel=xen";
    /* Set explicitly here to make sure that real ram_size is passed */
    mc->default_ram_size = 0;

#ifdef CONFIG_TPM
    object_class_property_add(oc, "tpm-base-addr", "uint64_t",
                              xen_arm_get_tpm_base_addr,
                              xen_arm_set_tpm_base_addr,
                              NULL, NULL);
    object_class_property_set_description(oc, "tpm-base-addr",
                                          "Set Base address for TPM device.");

    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_TPM_TIS_SYSBUS);
#endif
}

static const TypeInfo xen_arm_machine_type = {
    .name = TYPE_XEN_ARM,
    .parent = TYPE_MACHINE,
    .class_init = xen_arm_machine_class_init,
    .instance_size = sizeof(XenArmState),
};

static void xen_arm_machine_register_types(void)
{
    type_register_static(&xen_arm_machine_type);
}

type_init(xen_arm_machine_register_types)
