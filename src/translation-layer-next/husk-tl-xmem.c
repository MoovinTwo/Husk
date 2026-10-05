/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-xmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
#endif
#endif

#include "husk-tl-internal.h"

static struct {
    uint8_t *rx, *rw;
    size_t size, used;
    bool open;
    pthread_mutex_t lock;
} g_x = { .lock = PTHREAD_MUTEX_INITIALIZER };

#if defined(__APPLE__) && !TARGET_OS_OSX
#define TL_XMEM_SHARED 1
/* The phone: StikDebug's region. Found through the allocator that asked for it.
 * Only its bounds are kept here, for the range checks below: QEMU and the old
 * loader take slices of the same region, so every allocation is carved from the
 * JIT library's shared allocator rather than from a bump pointer of our own. */
static bool open_platform(size_t host_bytes, char *err, size_t errlen)
{
    (void)host_bytes;
    tl_dual_mapping *m = tl_find_stikdebug_prewarmed();
    if (!m || !m->rw_addr || !m->rx_addr) {
        snprintf(err, errlen, "no StikDebug JIT region: executable memory is the one "
                              "thing guest code cannot run without");
        return false;
    }
    g_x.rx = m->rx_addr;
    g_x.rw = m->rw_addr;
    g_x.size = m->size;
    return true;
}
#else
/* A Mac (or a plain host): two views of ordinary memory. */
static bool open_platform(size_t host_bytes, char *err, size_t errlen)
{
    /* TL_XMEM_MIB: size the region as a phone's would be, to see whether a game fits there */
    if (getenv("TL_XMEM_MIB")) host_bytes = (size_t)atoi(getenv("TL_XMEM_MIB")) << 20;
    size_t size = (host_bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    /* Reserve twice the span and put the two views side by side. adrp reaches +-4 GiB, and
     * the loader retargets code at the writable view with it, so the views must stay close:
     * left to place the alias anywhere, the kernel can put it more than 3 GiB away. */
    uint8_t *base = mmap(NULL, size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        snprintf(err, errlen, "mmap of %zu MiB failed: %s", (size * 2) >> 20, strerror(errno));
        return false;
    }
    uint8_t *rw = base;
    if (mprotect(rw, size, PROT_READ | PROT_WRITE) != 0) {
        snprintf(err, errlen, "mprotect(RW) failed: %s", strerror(errno));
        munmap(base, size * 2);
        return false;
    }
#if defined(__APPLE__)
    mach_vm_address_t alias = (mach_vm_address_t)(uintptr_t)(base + size);
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &alias, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)rw, FALSE,
                                     &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        snprintf(err, errlen, "mach_vm_remap failed (%d)", kr);
        munmap(base, size * 2);
        return false;
    }
    if (mprotect((void *)(uintptr_t)alias, size, PROT_READ | PROT_EXEC) != 0) {
        snprintf(err, errlen, "mprotect(R|X) on the alias failed: %s", strerror(errno));
        munmap(base, size * 2);
        return false;
    }
    g_x.rx = (uint8_t *)(uintptr_t)alias;
#else
    snprintf(err, errlen, "no dual mapping on this host");
    munmap(base, size * 2);
    return false;
#endif
    g_x.rw = rw;
    g_x.size = size;
    return true;
}
#endif

bool tl_xmem_open(size_t host_bytes, char *err, size_t errlen)
{
    bool ok = true;
    pthread_mutex_lock(&g_x.lock);
    if (!g_x.open) {
        char local[160] = "";
        ok = open_platform(host_bytes, local, sizeof(local));
        if (ok) {
            g_x.open = true;
        } else if (err && errlen) {
            snprintf(err, errlen, "%s", local);
        }
    }
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw)
{
    size_t n = (bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    bool ok = false;
    pthread_mutex_lock(&g_x.lock);
#ifdef TL_XMEM_SHARED
    if (g_x.open && tl_jit_carve(n, rx, rw)) {
        g_x.used += n;
        ok = true;
    }
#else
    if (g_x.open && n <= g_x.size - g_x.used) {
        if (rx) *rx = g_x.rx + g_x.used;
        if (rw) *rw = g_x.rw + g_x.used;
        g_x.used += n;
        ok = true;
    }
#endif
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

ptrdiff_t tl_xmem_delta(void) { return g_x.rw - g_x.rx; }

bool tl_xmem_contains(const void *p)
{
    const uint8_t *b = p;
    return g_x.open && ((b >= g_x.rx && b < g_x.rx + g_x.size)
                     || (b >= g_x.rw && b < g_x.rw + g_x.size));
}

bool tl_xmem_is_rx(const void *p)
{
    const uint8_t *b = p;
    return g_x.open && b >= g_x.rx && b < g_x.rx + g_x.size;
}

void tl_xmem_flush(const void *rx, size_t bytes)
{
#if defined(__APPLE__)
    sys_icache_invalidate((void *)rx, bytes);
#else
    __builtin___clear_cache((char *)rx, (char *)rx + bytes);
#endif
}

size_t tl_xmem_used(void) { return g_x.used; }
size_t tl_xmem_size(void)  { return g_x.size; }
