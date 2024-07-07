/*
 * Vhost User library
 *
 * Copyright 2024 NXP
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <endian.h>
#include "libvhost-user.h"

#include <xenctrl.h>
#include <xenforeignmemory.h>

void *vu_xen_foreignmap(uint32_t domainid, uint64_t addr, uint64_t size)
{
    xenforeignmemory_handle * vu_foreignmap_handle = NULL;
    void* ramptr;
    int nb_pfn = size >> XC_PAGE_SHIFT;
    xen_pfn_t *pfns;
    xen_pfn_t address_index = addr >> XC_PAGE_SHIFT;
    int *err;
    int i;

    pfns = g_new0(xen_pfn_t, nb_pfn);
    err = g_new0(int, nb_pfn);

    for (i = 0; i < nb_pfn; i++) {
        pfns[i] = address_index + i;
    }

    vu_foreignmap_handle = xenforeignmemory_open(NULL,0);
    if (!vu_foreignmap_handle) {
        DPRINT("xenforeignmemory_open failure\n");
        return NULL;
    }

    ramptr = xenforeignmemory_map2(vu_foreignmap_handle, domainid, 0,
                                   PROT_READ | PROT_WRITE, 0, nb_pfn, pfns, err);
    if (!ramptr) {
        DPRINT("xenforeignmemory_map2 fail\n");
        return NULL;
    }

    DPRINT("xen foreign map domain[%d]: gpa %016"PRIx64" size %016"PRIx64" va addr %p\n",
           domainid, addr, size, ramptr);

    return ramptr;
}

bool vu_xen_add_mem_reg(VuDev *dev, VhostUserMsg *vmsg)
{
    int i;
    uint32_t domid = vmsg->payload.xenmemreg.xen_domainid;
    VhostUserMemoryRegion m = vmsg->payload.xenmemreg.region, *msg_region = &m;
    VuDevRegion *dev_region = &dev->regions[dev->nregions];

    if (vmsg->size < VHOST_USER_MEM_REG_SIZE) {
        DPRINT("VHOST_USER_ADD_MEM_REG requires a message size of at "
               "least %zu bytes and only %d bytes were received",
               VHOST_USER_MEM_REG_SIZE, vmsg->size);
        return false;
    }

    if (dev->nregions == VHOST_USER_MAX_RAM_SLOTS) {
        DPRINT("failing attempt to hot add memory via "
               "VHOST_USER_ADD_MEM_REG message because the backend has "
               "no free ram slots available");
        return false;
    }


    DPRINT("Adding region: %u %d %x\n", dev->nregions, vmsg->request, vmsg->flags);
    DPRINT("    guest_phys_addr: 0x%016"PRIx64"\n",
           msg_region->guest_phys_addr);
    DPRINT("    memory_size:     0x%016"PRIx64"\n",
           msg_region->memory_size);
    DPRINT("    userspace_addr   0x%016"PRIx64"\n",
           msg_region->userspace_addr);
    DPRINT("    mmap_offset      0x%016"PRIx64"\n",
           msg_region->mmap_offset);

    dev->xen_domainid = domid;
    dev_region->gpa = msg_region->guest_phys_addr;
    dev_region->size = msg_region->memory_size;
    dev_region->qva = msg_region->userspace_addr;
    dev_region->mmap_offset = msg_region->mmap_offset;
    dev_region->mmap_addr = vu_xen_foreignmap(domid, dev_region->gpa, dev_region->size); 
    if (!dev_region->mmap_addr) {
        DPRINT("foreignmap dom[%d] gpa %"PRId64" %"PRId64"\n",
               domid, dev_region->gpa, dev_region->size);
        return false;
    }

    DPRINT("dev->max_queues %d\n", dev->max_queues);
    for (i = 0; i < dev->max_queues; i++) {
        if (dev->vq[i].vring.desc) {
            if (map_ring(dev, &dev->vq[i])) {
                DPRINT("remapping queue %d for new memory region", i);
            }
        }
    }

    DPRINT("Successfully added new region\n");
    dev->nregions++;
    /* false means no reply, true means reply. TODO: update to reply*/
    return false;
}
