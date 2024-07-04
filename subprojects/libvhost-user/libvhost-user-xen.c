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
	fprintf(stderr, "xenforeignmemory_open failure\n");
        return NULL;
    }

    ramptr = xenforeignmemory_map2(vu_foreignmap_handle, domainid, 0,
                                   PROT_READ | PROT_WRITE, 0, nb_pfn, pfns, err);
    if (!ramptr) {
        fprintf(stderr, "xenforeignmemory_map2 fail\n");
	return NULL;
    }

    DPRINT("xen foreign map domain[%d]: gpa %016"PRIx64" size %016"PRIx64" va addr %016"PRIx64"\n",
           domainid, addr, size, ramptr);

    return ramptr;
}
