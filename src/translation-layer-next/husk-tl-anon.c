/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-anon.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct { uintptr_t addr; size_t len; } anon_range;
static anon_range *g_anon;         /* grows: an allocator maps and unmaps all through a game's life */
static int g_nanon, g_capanon;
static uintptr_t g_anon_page_mask; /* the host's page less one: 16 KiB on a phone, 4 KiB on an Intel Mac */
static pthread_mutex_t g_anon_lock = PTHREAD_MUTEX_INITIALIZER;

void tl_anon_lock(void) { pthread_mutex_lock(&g_anon_lock); }
void tl_anon_unlock(void) { pthread_mutex_unlock(&g_anon_lock); }

/* Room for one more entry. When there is none to be had the range goes unrecorded, and the table carries on. */
static bool anon_room(void)
{
    if (g_nanon < g_capanon) return true;
    int cap = g_capanon ? g_capanon * 2 : 1024;
    anon_range *n = realloc(g_anon, (size_t)cap * sizeof(anon_range));
    if (!n) return false;
    g_anon = n; g_capanon = cap;
    return true;
}

/* Take [addr, end) out of the table. An entry it cuts through keeps the part on either side. */
static bool anon_cut_locked(uintptr_t addr, uintptr_t end)
{
    bool ok = true;
    for (int i = 0; i < g_nanon; i++) {
        uintptr_t a = g_anon[i].addr, e = a + g_anon[i].len;
        if (addr >= e || end <= a) continue;
        if (a < addr && e > end) {                           /* cut out of the middle: two pieces remain */
            g_anon[i].len = addr - a;
            if (anon_room()) { g_anon[g_nanon].addr = end; g_anon[g_nanon].len = e - end; g_nanon++; }
            else ok = false;
        } else if (a < addr) {
            g_anon[i].len = addr - a;
        } else if (e > end) {
            g_anon[i].addr = end; g_anon[i].len = e - end;
        } else {
            g_anon[i] = g_anon[--g_nanon]; i--;
        }
    }
    return ok;
}

bool tl_anon_add_locked(uintptr_t addr, size_t len)
{
    uintptr_t end = addr + len < addr ? UINTPTR_MAX : addr + len;
    bool ok = anon_cut_locked(addr, end);
    if (!anon_room()) return false;
    g_anon[g_nanon].addr = addr; g_anon[g_nanon].len = end - addr; g_nanon++;
    return ok;
}

bool tl_anon_remove_locked(uintptr_t addr, size_t len)
{
    if (!g_anon_page_mask) g_anon_page_mask = (uintptr_t)getpagesize() - 1;
    uintptr_t m = g_anon_page_mask;
    uintptr_t end = addr + len;
    end = end < addr ? UINTPTR_MAX : (end + m < end ? UINTPTR_MAX : (end + m) & ~m);
    addr &= ~m;
    return anon_cut_locked(addr, end);
}

bool tl_anon_next_locked(int *i, uintptr_t addr, uintptr_t end, uintptr_t *a, uintptr_t *e)
{
    for (; *i < g_nanon; (*i)++) {
        uintptr_t ra = g_anon[*i].addr, re = ra + g_anon[*i].len;
        *a = ra > addr ? ra : addr;
        *e = re < end ? re : end;
        if (*a < *e) { (*i)++; return true; }
    }
    return false;
}
