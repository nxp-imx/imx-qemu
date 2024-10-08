/*
 * Vhost User library
 *
 * Copyright 2024 NXP
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifndef LIBVHOST_USER_XEN_H
#define LIBVHOST_USER_XEN_H

#include "libvhost-user.h"

#if CONFIG_XEN_LIBVHOST_USER
bool vu_xen_add_mem_reg(VuDev *dev, VhostUserMsg *vmsg);
void *vu_xen_foreignmap(uint32_t domainid, uint64_t addr, uint64_t size);
void* vu_open_grantdev(void);
void vu_release_grantdev(void* handler);
int vu_export_grant_resource_pages(void* handler, uint32_t domid, uint32_t count,uint32_t* refs);
int vu_release_grant_resource_pages(void* handler, int fd);
#else
static inline bool vu_xen_add_mem_reg(VuDev *dev, VhostUserMsg *vmsg)
{
    return false;
}
static inline
void *vu_xen_foreignmap(uint32_t domainid, uint64_t addr, uint64_t size)
{
    return NULL;
}
static inline void* vu_open_grantdev(void)
{
    return 0;
}
static inline void vu_release_grantdev(void* handler)
{
    return;
}
static inline
int vu_export_grant_resource_pages(void* handler, uint32_t domid, uint32_t count,uint32_t* refs)
{
    return -1;
}
static inline
int vu_release_grant_resource_pages(void* handler, int fd)
{
    return 0;
}
#endif

#endif /* LIBVHOST_USER_XEN_H */
