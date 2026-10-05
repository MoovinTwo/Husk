/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The table of anonymous mappings behind MADV_DONTNEED, on ranges worked out by
 * hand, and two threads mapping and unmapping at once for TSan to watch.
 *
 * The table's own file is included, so a test can reset it and make realloc fail.
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_fail_realloc;            /* the next realloc the table makes returns NULL */
static void *anon_test_realloc(void *p, size_t n)
{
    if (g_fail_realloc) { g_fail_realloc = 0; return NULL; }
    return realloc(p, n);
}
#define realloc anon_test_realloc
#include "husk-tl-anon.c"
#undef realloc

static int failures;
static uintptr_t P;                   /* the host's page */

static void reset(void) { free(g_anon); g_anon = NULL; g_nanon = g_capanon = 0; }

/* The table as "[a,e) [a,e) ...", in pages from 0x100000 and sorted, so a case can say what it expects. */
static void show(char *out, size_t cap)
{
    uintptr_t at = 0; out[0] = 0;
    for (;;) {
        uintptr_t best = UINTPTR_MAX, len = 0;
        for (int i = 0; i < g_nanon; i++)
            if (g_anon[i].addr >= at && g_anon[i].addr < best) { best = g_anon[i].addr; len = g_anon[i].len; }
        if (best == UINTPTR_MAX) break;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s[%lu,%lu)", n ? " " : "", (unsigned long)((best - 0x100000) / P),
                 (unsigned long)((best + len - 0x100000) / P));
        at = best + 1;
    }
}

static void expect(const char *what, const char *want)
{
    char got[512]; show(got, sizeof(got));
    if (strcmp(got, want)) { printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); failures++; }
    else printf("ok   %s: %s\n", what, got);
}
static void check(const char *what, int cond)
{
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else printf("ok   %s\n", what);
}

static void *stress(void *arg);

#define PG(n) ((uintptr_t)0x100000 + (uintptr_t)(n) * P)

int main(void)
{
    P = (uintptr_t)getpagesize();
    tl_anon_lock();

    reset();
    tl_anon_add_locked(PG(0), 10 * P);
    tl_anon_remove_locked(PG(3), 2 * P);
    expect("split: unmapping the middle keeps both sides", "[0,3) [5,10)");

    reset();
    tl_anon_add_locked(PG(0), 10 * P);
    tl_anon_remove_locked(PG(0), 4 * P);
    tl_anon_remove_locked(PG(8), 4 * P);
    expect("trim: unmapping either end", "[4,8)");

    reset();
    tl_anon_add_locked(PG(0), 4 * P);
    tl_anon_add_locked(PG(4), 4 * P);
    tl_anon_remove_locked(PG(4) - 1, 1);
    expect("round: one byte takes its whole page and no more", "[0,3) [4,8)");

    reset();
    tl_anon_add_locked(PG(0), 4 * P);
    tl_anon_add_locked(PG(4), 4 * P);
    tl_anon_remove_locked(PG(0), 4 * P);
    expect("round: a page-sized range leaves its neighbour alone", "[4,8)");

    reset();
    tl_anon_add_locked(PG(0), 10 * P);
    tl_anon_add_locked(PG(4), 2 * P);
    expect("overlap: MAP_FIXED inside a region keeps the region around it", "[0,4) [4,6) [6,10)");
    uintptr_t a, e; int i = 0, n = 0; size_t covered = 0;
    while (tl_anon_next_locked(&i, PG(2), PG(8), &a, &e)) { n++; covered += e - a; }
    check("overlap: MADV_DONTNEED around it still finds all of it", n == 3 && covered == 6 * P);

    reset();
    tl_anon_add_locked(PG(2), 2 * P);
    tl_anon_add_locked(PG(6), 2 * P);
    tl_anon_add_locked(PG(1), 8 * P);
    expect("overlap: a mapping over several replaces them", "[1,9)");

    reset();
    i = 0;
    check("lookup: nothing recorded, nothing found", !tl_anon_next_locked(&i, 0, UINTPTR_MAX, &a, &e));

    reset();
    int ok = 1;
    for (int k = 0; k < 10000; k++) ok &= tl_anon_add_locked(PG(2 * k), P);
    check("growth: 10k entries are all recorded", ok && g_nanon == 10000);
    for (int k = 0; k < 10000; k += 2) tl_anon_remove_locked(PG(2 * k), P);
    i = 0; n = 0;
    while (tl_anon_next_locked(&i, 0, UINTPTR_MAX, &a, &e)) n++;
    check("growth: half removed, half remain", n == 5000);

    reset();
    g_fail_realloc = 1;
    check("realloc fails: the first add says so", !tl_anon_add_locked(PG(0), P) && g_nanon == 0);
    check("realloc fails: the next add carries on", tl_anon_add_locked(PG(0), P) && g_nanon == 1);

    reset();
    for (int k = 0; k < 1024; k++) tl_anon_add_locked(PG(4 * k), 3 * P);
    g_fail_realloc = 1;
    check("realloc fails: a split that cannot grow says so", !tl_anon_remove_locked(PG(1), P));
    check("realloc fails: and keeps the left part", g_nanon == 1024 && g_anon[0].addr == PG(0) && g_anon[0].len == P);

    reset();
    tl_anon_unlock();

    /* Two threads, each mapping, recording, checking its entry is there, then unmapping and forgetting -- the munmap
     * and the table update held together, as b_munmap holds them. Each thread's addresses are the other's soon after. */
    pthread_t t[2]; int lost[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) pthread_create(&t[k], NULL, stress, &lost[k]);
    for (int k = 0; k < 2; k++) pthread_join(t[k], NULL);
    check("stress: no thread lost an entry to the other's unmapping", lost[0] == 0 && lost[1] == 0);
    tl_anon_lock(); check("stress: the table ends empty", g_nanon == 0); reset(); tl_anon_unlock();

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

static void *stress(void *arg)
{
    int *lost = arg;
    size_t len = 4 * P;
    for (int k = 0; k < 20000; k++) {
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        tl_anon_lock(); tl_anon_add_locked((uintptr_t)p, len); tl_anon_unlock();
        sched_yield();                                       /* give the other thread a chance to unmap in between */
        tl_anon_lock();
        int i = 0; uintptr_t a, e; size_t covered = 0;
        while (tl_anon_next_locked(&i, (uintptr_t)p, (uintptr_t)p + len, &a, &e)) covered += e - a;
        if (covered != len) (*lost)++;
        tl_anon_unlock();
        tl_anon_lock();
        if (munmap(p, len) == 0) tl_anon_remove_locked((uintptr_t)p, len);
        tl_anon_unlock();
    }
    return NULL;
}
