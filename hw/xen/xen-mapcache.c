/*
 * Copyright (C) 2011       Citrix Ltd.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qemu/error-report.h"

#include <sys/resource.h>

#include "hw/xen/xen_native.h"
#include "hw/xen/xen_pvdev.h"
#include "qemu/bitmap.h"

#include "sysemu/runstate.h"
#include "sysemu/xen-mapcache.h"
#include "trace.h"


//#define MAPCACHE_DEBUG

#ifdef MAPCACHE_DEBUG
#  define DPRINTF(fmt, ...) do { \
    fprintf(stderr, "xen_mapcache: " fmt, ## __VA_ARGS__); \
} while (0)
#else
#  define DPRINTF(fmt, ...) do { } while (0)
#endif

#if HOST_LONG_BITS == 32
#  define MCACHE_BUCKET_SHIFT 16
#  define MCACHE_MAX_SIZE     (1UL<<31) /* 2GB Cap */
#else
#  define MCACHE_BUCKET_SHIFT 20
#  define MCACHE_MAX_SIZE     (1UL<<35) /* 32GB Cap */
#endif
#define MCACHE_BUCKET_SIZE (1UL << MCACHE_BUCKET_SHIFT)

/* This is the size of the virtual address space reserve to QEMU that will not
 * be use by MapCache.
 * From empirical tests I observed that qemu use 75MB more than the
 * max_mcache_size.
 */
#define NON_MCACHE_MEMORY_SIZE (80 * MiB)

typedef struct MapCacheEntry {
    hwaddr paddr_index;
    uint8_t *vaddr_base;
    unsigned long *valid_mapping;
    uint32_t lock;
#define XEN_MAPCACHE_ENTRY_DUMMY (1 << 0)
    uint8_t flags;
    hwaddr size;
    struct MapCacheEntry *next;
} MapCacheEntry;

typedef struct MapCacheRev {
    uint8_t *vaddr_req;
    hwaddr paddr_index;
    hwaddr size;
    QTAILQ_ENTRY(MapCacheRev) next;
    bool dma;
} MapCacheRev;

typedef struct MapCache {
    MapCacheEntry *entry;
    unsigned long nr_buckets;
    QTAILQ_HEAD(, MapCacheRev) locked_entries;

    /* For most cases (>99.9%), the page address is the same. */
    MapCacheEntry *last_entry;
    unsigned long max_mcache_size;
    unsigned int mcache_bucket_shift;

    phys_offset_to_gaddr_t phys_offset_to_gaddr;
    QemuMutex lock;
    void *opaque;
} MapCache;

static MapCache *mapcache;

static inline void mapcache_lock(void)
{
    qemu_mutex_lock(&mapcache->lock);
}

static inline void mapcache_unlock(void)
{
    qemu_mutex_unlock(&mapcache->lock);
}

static inline int test_bits(int nr, int size, const unsigned long *addr)
{
    unsigned long res = find_next_zero_bit(addr, size + nr, nr);
    if (res >= nr + size)
        return 1;
    else
        return 0;
}

void xen_map_cache_init(phys_offset_to_gaddr_t f, void *opaque)
{
    unsigned long size;
    struct rlimit rlimit_as;

    mapcache = g_new0(MapCache, 1);

    mapcache->phys_offset_to_gaddr = f;
    mapcache->opaque = opaque;
    qemu_mutex_init(&mapcache->lock);

    QTAILQ_INIT(&mapcache->locked_entries);

    if (geteuid() == 0) {
        rlimit_as.rlim_cur = RLIM_INFINITY;
        rlimit_as.rlim_max = RLIM_INFINITY;
        mapcache->max_mcache_size = MCACHE_MAX_SIZE;
    } else {
        getrlimit(RLIMIT_AS, &rlimit_as);
        rlimit_as.rlim_cur = rlimit_as.rlim_max;

        if (rlimit_as.rlim_max != RLIM_INFINITY) {
            warn_report("QEMU's maximum size of virtual"
                        " memory is not infinity");
        }
        if (rlimit_as.rlim_max < MCACHE_MAX_SIZE + NON_MCACHE_MEMORY_SIZE) {
            mapcache->max_mcache_size = rlimit_as.rlim_max -
                NON_MCACHE_MEMORY_SIZE;
        } else {
            mapcache->max_mcache_size = MCACHE_MAX_SIZE;
        }
    }

    setrlimit(RLIMIT_AS, &rlimit_as);

    mapcache->nr_buckets =
        (((mapcache->max_mcache_size >> XC_PAGE_SHIFT) +
          (1UL << (MCACHE_BUCKET_SHIFT - XC_PAGE_SHIFT)) - 1) >>
         (MCACHE_BUCKET_SHIFT - XC_PAGE_SHIFT));

    size = mapcache->nr_buckets * sizeof (MapCacheEntry);
    size = (size + XC_PAGE_SIZE - 1) & ~(XC_PAGE_SIZE - 1);
    DPRINTF("%s, nr_buckets = %lx size %lu\n", __func__,
            mapcache->nr_buckets, size);
    mapcache->entry = g_malloc0(size);
}

static void xen_remap_bucket(MapCacheEntry *entry,
                             void *vaddr,
                             hwaddr size,
                             hwaddr address_index,
                             bool dummy)
{
    uint8_t *vaddr_base;
    xen_pfn_t *pfns;
    int *err;
    unsigned int i;
    hwaddr nb_pfn = size >> XC_PAGE_SHIFT;

    trace_xen_remap_bucket(address_index);

    pfns = g_new0(xen_pfn_t, nb_pfn);
    err = g_new0(int, nb_pfn);

    if (entry->vaddr_base != NULL) {
        if (!(entry->flags & XEN_MAPCACHE_ENTRY_DUMMY)) {
            ram_block_notify_remove(entry->vaddr_base, entry->size,
                                    entry->size);
        }

        /*
         * If an entry is being replaced by another mapping and we're using
         * MAP_FIXED flag for it - there is possibility of a race for vaddr
         * address with another thread doing an mmap call itself
         * (see man 2 mmap). To avoid that we skip explicit unmapping here
         * and allow the kernel to destroy the previous mappings by replacing
         * them in mmap call later.
         *
         * Non-identical replacements are not allowed therefore.
         */
        assert(!vaddr || (entry->vaddr_base == vaddr && entry->size == size));

        if (!vaddr && munmap(entry->vaddr_base, entry->size) != 0) {
            perror("unmap fails");
            exit(-1);
        }
    }
    g_free(entry->valid_mapping);
    entry->valid_mapping = NULL;

    for (i = 0; i < nb_pfn; i++) {
        pfns[i] = (address_index << (MCACHE_BUCKET_SHIFT-XC_PAGE_SHIFT)) + i;
    }

    /*
     * If the caller has requested the mapping at a specific address use
     * MAP_FIXED to make sure it's honored.
     */
    if (!dummy) {
        vaddr_base = xenforeignmemory_map2(xen_fmem, xen_domid, vaddr,
                                           PROT_READ | PROT_WRITE,
                                           vaddr ? MAP_FIXED : 0,
                                           nb_pfn, pfns, err);
        if (vaddr_base == NULL) {
            perror("xenforeignmemory_map2");
            exit(-1);
        }
    } else {
        /*
         * We create dummy mappings where we are unable to create a foreign
         * mapping immediately due to certain circumstances (i.e. on resume now)
         */
        vaddr_base = mmap(vaddr, size, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_SHARED | (vaddr ? MAP_FIXED : 0),
                          -1, 0);
        if (vaddr_base == MAP_FAILED) {
            perror("mmap");
            exit(-1);
        }
    }

    if (!(entry->flags & XEN_MAPCACHE_ENTRY_DUMMY)) {
        ram_block_notify_add(vaddr_base, size, size);
    }

    entry->vaddr_base = vaddr_base;
    entry->paddr_index = address_index;
    entry->size = size;
    entry->valid_mapping = g_new0(unsigned long,
                                  BITS_TO_LONGS(size >> XC_PAGE_SHIFT));

    if (dummy) {
        entry->flags |= XEN_MAPCACHE_ENTRY_DUMMY;
    } else {
        entry->flags &= ~(XEN_MAPCACHE_ENTRY_DUMMY);
    }

    bitmap_zero(entry->valid_mapping, nb_pfn);
    for (i = 0; i < nb_pfn; i++) {
        if (!err[i]) {
            bitmap_set(entry->valid_mapping, i, 1);
        }
    }

    g_free(pfns);
    g_free(err);
}

static uint8_t *xen_map_cache_unlocked(hwaddr phys_addr, hwaddr size,
                                       uint8_t lock, bool dma)
{
    MapCacheEntry *entry, *pentry = NULL,
                  *free_entry = NULL, *free_pentry = NULL;
    hwaddr address_index;
    hwaddr address_offset;
    hwaddr cache_size = size;
    hwaddr test_bit_size;
    bool translated G_GNUC_UNUSED = false;
    bool dummy = false;

tryagain:
    address_index  = phys_addr >> MCACHE_BUCKET_SHIFT;
    address_offset = phys_addr & (MCACHE_BUCKET_SIZE - 1);

    trace_xen_map_cache(phys_addr);

    /* test_bit_size is always a multiple of XC_PAGE_SIZE */
    if (size) {
        test_bit_size = size + (phys_addr & (XC_PAGE_SIZE - 1));

        if (test_bit_size % XC_PAGE_SIZE) {
            test_bit_size += XC_PAGE_SIZE - (test_bit_size % XC_PAGE_SIZE);
        }
    } else {
        test_bit_size = XC_PAGE_SIZE;
    }

    if (mapcache->last_entry != NULL &&
        mapcache->last_entry->paddr_index == address_index &&
        !lock && !size &&
        test_bits(address_offset >> XC_PAGE_SHIFT,
                  test_bit_size >> XC_PAGE_SHIFT,
                  mapcache->last_entry->valid_mapping)) {
        trace_xen_map_cache_return(mapcache->last_entry->vaddr_base + address_offset);
        return mapcache->last_entry->vaddr_base + address_offset;
    }

    /* size is always a multiple of MCACHE_BUCKET_SIZE */
    if (size) {
        cache_size = size + address_offset;
        if (cache_size % MCACHE_BUCKET_SIZE) {
            cache_size += MCACHE_BUCKET_SIZE - (cache_size % MCACHE_BUCKET_SIZE);
        }
    } else {
        cache_size = MCACHE_BUCKET_SIZE;
    }

    entry = &mapcache->entry[address_index % mapcache->nr_buckets];

    while (entry && (lock || entry->lock) && entry->vaddr_base &&
            (entry->paddr_index != address_index || entry->size != cache_size ||
             !test_bits(address_offset >> XC_PAGE_SHIFT,
                 test_bit_size >> XC_PAGE_SHIFT,
                 entry->valid_mapping))) {
        if (!free_entry && !entry->lock) {
            free_entry = entry;
            free_pentry = pentry;
        }
        pentry = entry;
        entry = entry->next;
    }
    if (!entry && free_entry) {
        entry = free_entry;
        pentry = free_pentry;
    }
    if (!entry) {
        entry = g_new0(MapCacheEntry, 1);
        pentry->next = entry;
        xen_remap_bucket(entry, NULL, cache_size, address_index, dummy);
    } else if (!entry->lock) {
        if (!entry->vaddr_base || entry->paddr_index != address_index ||
                entry->size != cache_size ||
                !test_bits(address_offset >> XC_PAGE_SHIFT,
                    test_bit_size >> XC_PAGE_SHIFT,
                    entry->valid_mapping)) {
            xen_remap_bucket(entry, NULL, cache_size, address_index, dummy);
        }
    }

    if(!test_bits(address_offset >> XC_PAGE_SHIFT,
                test_bit_size >> XC_PAGE_SHIFT,
                entry->valid_mapping)) {
        mapcache->last_entry = NULL;
#ifdef XEN_COMPAT_PHYSMAP
        if (!translated && mapcache->phys_offset_to_gaddr) {
            phys_addr = mapcache->phys_offset_to_gaddr(phys_addr, size);
            translated = true;
            goto tryagain;
        }
#endif
        if (!dummy && runstate_check(RUN_STATE_INMIGRATE)) {
            dummy = true;
            goto tryagain;
        }
        trace_xen_map_cache_return(NULL);
        return NULL;
    }

    mapcache->last_entry = entry;
    if (lock) {
        MapCacheRev *reventry = g_new0(MapCacheRev, 1);
        entry->lock++;
        if (entry->lock == 0) {
            fprintf(stderr,
                    "mapcache entry lock overflow: "HWADDR_FMT_plx" -> %p\n",
                    entry->paddr_index, entry->vaddr_base);
            abort();
        }
        reventry->dma = dma;
        reventry->vaddr_req = mapcache->last_entry->vaddr_base + address_offset;
        reventry->paddr_index = mapcache->last_entry->paddr_index;
        reventry->size = entry->size;
        QTAILQ_INSERT_HEAD(&mapcache->locked_entries, reventry, next);
    }

    trace_xen_map_cache_return(mapcache->last_entry->vaddr_base + address_offset);
    return mapcache->last_entry->vaddr_base + address_offset;
}

uint8_t *xen_map_cache(hwaddr phys_addr, hwaddr size,
                       uint8_t lock, bool dma)
{
    uint8_t *p;

    mapcache_lock();
    p = xen_map_cache_unlocked(phys_addr, size, lock, dma);
    mapcache_unlock();
    return p;
}

static ram_addr_t xen_ram_addr_from_mapcache_try(void *ptr)
{
    MapCacheEntry *entry = NULL;
    MapCacheRev *reventry;
    hwaddr paddr_index;
    hwaddr size;
    ram_addr_t raddr;
    int found = 0;

    mapcache_lock();
    QTAILQ_FOREACH(reventry, &mapcache->locked_entries, next) {
        if (reventry->vaddr_req == ptr) {
            paddr_index = reventry->paddr_index;
            size = reventry->size;
            found = 1;
            break;
        }
    }
    if (!found) {
        mapcache_unlock();
        return ~0ULL;
    }

    entry = &mapcache->entry[paddr_index % mapcache->nr_buckets];
    while (entry && (entry->paddr_index != paddr_index || entry->size != size)) {
        entry = entry->next;
    }
    if (!entry) {
        raddr = ~0ULL;
    } else {
        raddr = (reventry->paddr_index << MCACHE_BUCKET_SHIFT) +
             ((unsigned long) ptr - (unsigned long) entry->vaddr_base);
    }
    mapcache_unlock();
    return raddr;
}

static void xen_invalidate_map_cache_entry_unlocked(uint8_t *buffer)
{
    MapCacheEntry *entry = NULL, *pentry = NULL;
    MapCacheRev *reventry;
    hwaddr paddr_index;
    hwaddr size;
    int found = 0;

    QTAILQ_FOREACH(reventry, &mapcache->locked_entries, next) {
        if (reventry->vaddr_req == buffer) {
            paddr_index = reventry->paddr_index;
            size = reventry->size;
            found = 1;
            break;
        }
    }
    if (!found) {
        DPRINTF("%s, could not find %p\n", __func__, buffer);
        QTAILQ_FOREACH(reventry, &mapcache->locked_entries, next) {
            DPRINTF("   "HWADDR_FMT_plx" -> %p is present\n", reventry->paddr_index, reventry->vaddr_req);
        }
        return;
    }
    QTAILQ_REMOVE(&mapcache->locked_entries, reventry, next);
    g_free(reventry);

    if (mapcache->last_entry != NULL &&
        mapcache->last_entry->paddr_index == paddr_index) {
        mapcache->last_entry = NULL;
    }

    entry = &mapcache->entry[paddr_index % mapcache->nr_buckets];
    while (entry && (entry->paddr_index != paddr_index || entry->size != size)) {
        pentry = entry;
        entry = entry->next;
    }
    if (!entry) {
        DPRINTF("Trying to unmap address %p that is not in the mapcache!\n", buffer);
        return;
    }
    entry->lock--;
    if (entry->lock > 0 || pentry == NULL) {
        return;
    }

    pentry->next = entry->next;
    ram_block_notify_remove(entry->vaddr_base, entry->size, entry->size);
    if (munmap(entry->vaddr_base, entry->size) != 0) {
        perror("unmap fails");
        exit(-1);
    }
    g_free(entry->valid_mapping);
    g_free(entry);
}

typedef struct XenMapCacheData {
    Coroutine *co;
    uint8_t *buffer;
} XenMapCacheData;

static void xen_invalidate_map_cache_entry_bh(void *opaque)
{
    XenMapCacheData *data = opaque;

    mapcache_lock();
    xen_invalidate_map_cache_entry_unlocked(data->buffer);
    mapcache_unlock();

    aio_co_wake(data->co);
}

void coroutine_mixed_fn xen_invalidate_map_cache_entry(uint8_t *buffer)
{
    if (qemu_in_coroutine()) {
        XenMapCacheData data = {
            .co = qemu_coroutine_self(),
            .buffer = buffer,
        };
        aio_bh_schedule_oneshot(qemu_get_current_aio_context(),
                                xen_invalidate_map_cache_entry_bh, &data);
        qemu_coroutine_yield();
    } else {
        mapcache_lock();
        xen_invalidate_map_cache_entry_unlocked(buffer);
        mapcache_unlock();
    }
}

void xen_invalidate_map_cache(void)
{
    unsigned long i;
    MapCacheRev *reventry;

    /* Flush pending AIO before destroying the mapcache */
    bdrv_drain_all();

    mapcache_lock();

    QTAILQ_FOREACH(reventry, &mapcache->locked_entries, next) {
        if (!reventry->dma) {
            continue;
        }
        fprintf(stderr, "Locked DMA mapping while invalidating mapcache!"
                " "HWADDR_FMT_plx" -> %p is present\n",
                reventry->paddr_index, reventry->vaddr_req);
    }

    for (i = 0; i < mapcache->nr_buckets; i++) {
        MapCacheEntry *entry = &mapcache->entry[i];

        if (entry->vaddr_base == NULL) {
            continue;
        }
        if (entry->lock > 0) {
            continue;
        }

        if (munmap(entry->vaddr_base, entry->size) != 0) {
            perror("unmap fails");
            exit(-1);
        }

        entry->paddr_index = 0;
        entry->vaddr_base = NULL;
        entry->size = 0;
        g_free(entry->valid_mapping);
        entry->valid_mapping = NULL;
    }

    mapcache->last_entry = NULL;

    mapcache_unlock();
}

static uint8_t *xen_replace_cache_entry_unlocked(hwaddr old_phys_addr,
                                                 hwaddr new_phys_addr,
                                                 hwaddr size)
{
    MapCacheEntry *entry;
    hwaddr address_index, address_offset;
    hwaddr test_bit_size, cache_size = size;

    address_index  = old_phys_addr >> MCACHE_BUCKET_SHIFT;
    address_offset = old_phys_addr & (MCACHE_BUCKET_SIZE - 1);

    assert(size);
    /* test_bit_size is always a multiple of XC_PAGE_SIZE */
    test_bit_size = size + (old_phys_addr & (XC_PAGE_SIZE - 1));
    if (test_bit_size % XC_PAGE_SIZE) {
        test_bit_size += XC_PAGE_SIZE - (test_bit_size % XC_PAGE_SIZE);
    }
    cache_size = size + address_offset;
    if (cache_size % MCACHE_BUCKET_SIZE) {
        cache_size += MCACHE_BUCKET_SIZE - (cache_size % MCACHE_BUCKET_SIZE);
    }

    entry = &mapcache->entry[address_index % mapcache->nr_buckets];
    while (entry && !(entry->paddr_index == address_index &&
                      entry->size == cache_size)) {
        entry = entry->next;
    }
    if (!entry) {
        DPRINTF("Trying to update an entry for "HWADDR_FMT_plx \
                "that is not in the mapcache!\n", old_phys_addr);
        return NULL;
    }

    address_index  = new_phys_addr >> MCACHE_BUCKET_SHIFT;
    address_offset = new_phys_addr & (MCACHE_BUCKET_SIZE - 1);

    fprintf(stderr, "Replacing a dummy mapcache entry for "HWADDR_FMT_plx \
            " with "HWADDR_FMT_plx"\n", old_phys_addr, new_phys_addr);

    xen_remap_bucket(entry, entry->vaddr_base,
                     cache_size, address_index, false);
    if (!test_bits(address_offset >> XC_PAGE_SHIFT,
                test_bit_size >> XC_PAGE_SHIFT,
                entry->valid_mapping)) {
        DPRINTF("Unable to update a mapcache entry for "HWADDR_FMT_plx"!\n",
                old_phys_addr);
        return NULL;
    }

    return entry->vaddr_base + address_offset;
}

uint8_t *xen_replace_cache_entry(hwaddr old_phys_addr,
                                 hwaddr new_phys_addr,
                                 hwaddr size)
{
    uint8_t *p;

    mapcache_lock();
    p = xen_replace_cache_entry_unlocked(old_phys_addr, new_phys_addr, size);
    mapcache_unlock();
    return p;
}

struct XENMappedGrantRegion {
    void *addr;
    unsigned int pages;
    unsigned int refs;
    unsigned int prot;
    unsigned int idx;
    QLIST_ENTRY(XENMappedGrantRegion) list;
    QLIST_ENTRY(XENMappedGrantRegion) bucket_list;
};

static MemoryRegion ram_grants;
static xengnttab_handle *xen_region_gnttabdev;
static QLIST_HEAD(GrantRegionList, XENMappedGrantRegion) *xen_region_mappings;
static QLIST_HEAD(, XENMappedGrantRegion) xen_grant_mappings =
    QLIST_HEAD_INITIALIZER(xen_grant_mappings);
static QemuMutex xen_map_mutex;

static void *xen_map_grant_dyn(MemoryRegion **mr, hwaddr addr, hwaddr *plen,
                               bool is_write, MemTxAttrs attrs)
{
    unsigned int page_off = addr & (XC_PAGE_SIZE - 1);
    unsigned int i;
    unsigned int nrefs = (page_off + *plen + XC_PAGE_SIZE - 1) >> XC_PAGE_SHIFT;
    uint32_t ref = (addr - XEN_GRANT_ADDR_OFF) >> XC_PAGE_SHIFT;
    uint32_t *refs;
    unsigned int prot = PROT_READ;
    struct XENMappedGrantRegion *mgr = NULL;

    if (is_write) {
        prot |= PROT_WRITE;
    }

    qemu_mutex_lock(&xen_map_mutex);

    QLIST_FOREACH(mgr, xen_region_mappings + ref, bucket_list) {
        if (mgr->pages == nrefs && (mgr->prot & prot) == prot) {
            break;
        }
    }
    if (!mgr) {
        mgr = g_new(struct XENMappedGrantRegion, 1);

        if (nrefs == 1) {
            refs = &ref;
        } else {
            refs = g_new(uint32_t, nrefs);
            for (i = 0; i < nrefs; i++) {
                refs[i] = ref + i;
            }
        }
        mgr->addr = xengnttab_map_domain_grant_refs(xen_region_gnttabdev, nrefs,
                                                    xen_domid, refs, prot);
        if (mgr->addr) {
            mgr->pages = nrefs;
            mgr->refs = 1;
            mgr->prot = prot;
            mgr->idx = ref;

            QLIST_INSERT_HEAD(xen_region_mappings + ref, mgr, bucket_list);
            QLIST_INSERT_HEAD(&xen_grant_mappings, mgr, list);
        } else {
            g_free(mgr);
            mgr = NULL;
        }
    } else {
        mgr->refs++;
    }

    qemu_mutex_unlock(&xen_map_mutex);

    if (nrefs > 1) {
        g_free(refs);
    }

    return mgr ? mgr->addr + page_off : NULL;
}

static void xen_unmap_grant_dyn(MemoryRegion *mr, void *buffer, ram_addr_t addr,
                                hwaddr len, bool is_write, hwaddr access_len)
{
    unsigned int page_off = (unsigned long)buffer & (XC_PAGE_SIZE - 1);
    unsigned int nrefs = (page_off + len + XC_PAGE_SIZE - 1) >> XC_PAGE_SHIFT;
    uint32_t ref = addr >> XC_PAGE_SHIFT;
    unsigned int prot = PROT_READ;
    struct XENMappedGrantRegion *mgr = NULL;

    if (is_write) {
        prot |= PROT_WRITE;
    }

    qemu_mutex_lock(&xen_map_mutex);

    QLIST_FOREACH(mgr, xen_region_mappings + ref, bucket_list) {
        if (mgr->addr == buffer - page_off) {
            break;
        }
    }
    if (mgr && mgr->pages == nrefs) {
        mgr->refs--;
        if (!mgr->refs) {
            xengnttab_unmap(xen_region_gnttabdev, mgr->addr, nrefs);

            QLIST_REMOVE(mgr, bucket_list);
            QLIST_REMOVE(mgr, list);
            g_free(mgr);
        }
    } else {
        error_report("xen_unmap_grant_dyn() trying to unmap unknown buffer");
    }

    qemu_mutex_unlock(&xen_map_mutex);
}

static ram_addr_t xen_ram_addr_from_grant_cache(void *ptr)
{
    unsigned int page_off = (unsigned long)ptr & (XC_PAGE_SIZE - 1);
    struct XENMappedGrantRegion *mgr = NULL;
    ram_addr_t raddr = ~0ULL;

    qemu_mutex_lock(&xen_map_mutex);

    QLIST_FOREACH(mgr, &xen_grant_mappings, list) {
        if (mgr->addr == ptr - page_off) {
            break;
        }
    }

    if (mgr) {
        raddr = (mgr->idx << XC_PAGE_SHIFT) + page_off + XEN_GRANT_ADDR_OFF;
    }

    qemu_mutex_unlock(&xen_map_mutex);

    return raddr;
}

ram_addr_t xen_ram_addr_from_mapcache(void *ptr)
{
    ram_addr_t raddr;

    raddr = xen_ram_addr_from_mapcache_try(ptr);
    if (raddr == ~0ULL) {
        raddr = xen_ram_addr_from_grant_cache(ptr);
    }

    return raddr;
}

static const struct MemoryRegionOps xen_grant_mr_ops = {
    .map = xen_map_grant_dyn,
    .unmap = xen_unmap_grant_dyn,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

MemoryRegion *xen_init_grant_ram(void)
{
    RAMBlock *block;
    unsigned int i;

    qemu_mutex_init(&xen_map_mutex);

    xen_region_gnttabdev = xengnttab_open(NULL, 0);
    if (xen_region_gnttabdev == NULL) {
        fprintf(stderr, "can't open gnttab device\n");
        return NULL;
    }
    xen_region_mappings = g_new(struct GrantRegionList, XEN_MAX_VIRTIO_GRANTS);
    for (i = 0; i < XEN_MAX_VIRTIO_GRANTS; i++) {
        QLIST_INIT(&xen_region_mappings[i]);
    }

    memory_region_init(&ram_grants, NULL, "xen.grants",
                       XEN_MAX_VIRTIO_GRANTS * XC_PAGE_SIZE);
    block = g_malloc0(sizeof(*block));
    block->mr = &ram_grants;
    block->used_length = XEN_MAX_VIRTIO_GRANTS * XC_PAGE_SIZE;
    block->max_length = XEN_MAX_VIRTIO_GRANTS * XC_PAGE_SIZE;
    block->fd = -1;
    block->page_size = XC_PAGE_SIZE;
    block->host = (void *)XEN_GRANT_ADDR_OFF;
    block->offset = XEN_GRANT_ADDR_OFF;
    block->flags = RAM_PREALLOC;
    ram_grants.ram_block = block;
    ram_grants.ram = true;
    ram_grants.terminates = true;
    ram_grants.ops = &xen_grant_mr_ops;
    ram_block_add_list(block);
    memory_region_add_subregion(get_system_memory(), XEN_GRANT_ADDR_OFF,
                                &ram_grants);

    return &ram_grants;
}
