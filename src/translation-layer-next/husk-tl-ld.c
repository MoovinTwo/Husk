/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-ld.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "husk-tl-internal.h"
#include "husk-tl-a64.h"
#include "husk-tl-xmem.h"

/* The project's log sink, and the bionic shim's surface. */
void  tl_log_line(const char *fmt, ...);
void *tl_bionic_find(const char *name);
bool  tl_bionic_is_system_lib(const char *soname);
unsigned long tl_bionic_auxval(unsigned long type);

/* ------------------------------------------------------------------ ELF */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } elf_ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } elf_phdr;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } elf_sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } elf_rela;
typedef struct { int64_t d_tag; uint64_t d_val; } elf_dyn;

enum {
    PT_LOAD_ = 1, PT_DYNAMIC_ = 2, PT_TLS_ = 7, PT_GNU_RELRO_ = 0x6474e552,
    DT_NULL_ = 0, DT_NEEDED_ = 1, DT_PLTRELSZ_ = 2, DT_HASH_ = 4, DT_STRTAB_ = 5, DT_SYMTAB_ = 6,
    DT_RELA_ = 7, DT_RELASZ_ = 8, DT_STRSZ_ = 10, DT_INIT_ = 12, DT_SONAME_ = 14, DT_JMPREL_ = 23,
    DT_INIT_ARRAY_ = 25, DT_INIT_ARRAYSZ_ = 27, DT_FLAGS_ = 30, DT_RELRSZ_ = 35, DT_RELR_ = 36,
    DT_GNU_HASH_ = 0x6ffffef5, DT_ANDROID_RELA_ = 0x60000011, DT_ANDROID_RELASZ_ = 0x60000012,
    DT_ANDROID_RELR_ = 0x6fffe000, DT_ANDROID_RELRSZ_ = 0x6fffe001,
    R_NONE = 0, R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027,
    R_TLS_DTPMOD = 1028, R_TLS_DTPREL = 1029, R_TLS_TPREL = 1030, R_TLSDESC = 1031, R_IRELATIVE = 1032,
    STB_WEAK_ = 2, STT_TLS_ = 6, STT_GNU_IFUNC_ = 10, SHN_UNDEF_ = 0,
    PF_X_ = 1, PF_W_ = 2, PF_R_ = 4, EM_AARCH64_ = 183,
};

#define PAGE TL_XMEM_PAGE

/* ----------------------------------------------------------------- libs */

#define MAX_LIBS 96
#define MAX_DEPS 48

struct tl_lib {
    char name[96];                 /* as asked for */
    char soname[96];
    uint8_t *rx, *rw;              /* image: address of base_vaddr, in each view */
    uint64_t base_vaddr;
    size_t npages;
    uint8_t *pflags;               /* TL_PAGE_* per page */
    elf_phdr *phdr;                /* malloc'd copy, file addresses */
    unsigned phnum;

    /* dynamic section, as vaddrs */
    uint64_t strtab, strsz, symtab, gnu_hash, sysv_hash;
    uint64_t rela, relasz, jmprel, pltrelsz, arela, arelasz, relr, relrsz;
    uint64_t init, init_array, init_arraysz;
    uint32_t nsyms;
    uint64_t *symcache;            /* resolved import per symbol index; 0 = not yet */

    uint64_t needed[MAX_DEPS];
    int nneeded;
    struct tl_lib *deps[MAX_DEPS]; /* the closure, breadth-first, excluding self */
    int ndeps;
    bool deps_ready;

    uint8_t *stub_rx, *stub_rw;    /* pages after the image: stubs for rewritten `svc` and x18 sites */
    size_t stub_used, stub_cap, nstub;
    uint8_t *pre_rx, *pre_rw;      /* pages before the image: the stubs of sites too far from the pages after it */
    size_t pre_used, pre_cap, npre;
    uint8_t *isl_rx, *isl_rw;      /* a third pool inside the image: the dead tail of the relocation table, for sites out of range of both */
    size_t isl_used, isl_cap;
    size_t xmem_bytes;             /* executable memory taken for it, pools included: the region never takes it back */

    struct tl_code_range { uint64_t start, end; } *code;   /* executable sections, as vaddrs: the only places instructions are patched */
    int ncode;
    struct tl_ifunc { uint32_t sym; uint64_t value; } *ifuncs;   /* what dlsym's IFUNCs resolved to, by symbol index (under G.lock) */
    size_t nifuncs, capifuncs;
    size_t n_ctr;                  /* reads of CTR_EL0 replaced by a constant */
    size_t n_svc_far, n_adr_failed;   /* svc sites with no pool in branch range (answered ENOSYS), adr sites that could not be rewritten */
    size_t n_x18, n_x18_failed;    /* sites rewritten for the reserved register, and sites that could not be */
    size_t n_tpidr_shared;         /* reads of TPIDR_EL0 with no stub in range, given the shared thread block */
    /* ELF TLS: the PT_TLS segment, and where its block sits from every thread's TP; tls_id 0 = none */
    uint64_t tls_vaddr, tls_filesz, tls_memsz;
    size_t tls_off, tls_id;
    bool tls_ready;                /* relocated, so its image may be copied into thread blocks */
    int state;                     /* 0 mapped, 1 relocating, 2 relocated, 3 initialising, 4 initialised */
    uint32_t n_unresolved;
};

static struct {
    pthread_mutex_t lock;
    tl_lib *libs[MAX_LIBS];
    int nlibs;
    tl_zip apks[4];
    char apk_paths[4][1024];
    int napks;
    int verbosity;
    size_t unresolved;
    char **argv, **envp;
    bool recursive_init;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .verbosity = 1 };

static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;   /* serialises load/init */

void tl_ld_set_verbosity(int v) { G.verbosity = v; }
void tl_ld_set_environment(char **argv, char **envp) { G.argv = argv; G.envp = envp; }
size_t tl_ld_unresolved_count(void) { return G.unresolved; }

static inline const void *at(const tl_lib *L, uint64_t vaddr) { return L->rw + (vaddr - L->base_vaddr); }

/* ------------------------------------------------------------- the APKs */

bool tl_ld_add_apk(const char *path)
{
    if (G.napks >= 4) return false;
    char err[160];
    if (!tl_zip_open(&G.apks[G.napks], path, err, sizeof(err))) {
        tl_log_line("ld: cannot open %s: %s", path, err);
        return false;
    }
    snprintf(G.apk_paths[G.napks], sizeof(G.apk_paths[0]), "%s", path);
    G.napks++;
    return true;
}

const tl_zip *tl_ld_apk_at(int i) { return (i >= 0 && i < G.napks) ? &G.apks[i] : NULL; }
const char *tl_ld_apk_path(int i) { return (i >= 0 && i < G.napks) ? G.apk_paths[i] : NULL; }

static bool fetch_from_apks(const char *name, uint8_t **out, size_t *len)
{
    char path[160];
    snprintf(path, sizeof(path), "lib/arm64-v8a/%s", name);
    for (int i = 0; i < G.napks; i++) {
        const tl_zip_entry *e = tl_zip_find(&G.apks[i], path);
        if (!e) continue;
        const uint8_t *data; size_t n; bool owned; char err[160];
        if (!tl_zip_data(&G.apks[i], e, (size_t)1 << 30, &data, &n, &owned, err, sizeof(err))) {
            tl_log_line("ld: %s: %s", name, err);
            return false;
        }
        if (!owned) {
            uint8_t *copy = malloc(n);
            if (!copy) return false;
            memcpy(copy, data, n);
            data = copy;
        }
        *out = (uint8_t *)data;
        *len = n;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- hashing */

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = h * 33 + *p;
    return h;
}

static uint32_t sysv_hash(const char *s)
{
    uint32_t h = 0, g;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h = (h << 4) + *p;
        if ((g = h & 0xf0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static const char *sym_name(const tl_lib *L, const elf_sym *s)
{
    return (const char *)at(L, L->strtab) + s->st_name;
}

static const elf_sym *sym_at(const tl_lib *L, uint32_t i)
{
    return (const elf_sym *)at(L, L->symtab) + i;
}

static void *tls_sym_addr(const tl_lib *L, const elf_sym *s);

/* The address a defined symbol is known by: the writable view for data; a thread-local's, the calling thread's copy. */
static void *sym_value(const tl_lib *L, const elf_sym *s)
{
    if ((s->st_info & 0xf) == STT_TLS_) return tls_sym_addr(L, s);
    uint64_t off = s->st_value - L->base_vaddr;
    size_t page = (size_t)(off / PAGE);
    bool is_func = (s->st_info & 0xf) == 2;
    if (!is_func && page < L->npages && (L->pflags[page] & TL_PAGE_W)) return L->rw + off;
    return L->rx + off;
}

static const elf_sym *lib_find(const tl_lib *L, const char *name)
{
    if (!L->symtab || !L->strtab) return NULL;
    if (L->gnu_hash) {
        const uint8_t *g = at(L, L->gnu_hash);
        uint32_t nb, symoff, bloom_n, bloom_shift;
        memcpy(&nb, g, 4); memcpy(&symoff, g + 4, 4); memcpy(&bloom_n, g + 8, 4); memcpy(&bloom_shift, g + 12, 4);
        if (!nb) return NULL;
        const uint64_t *bloom = (const uint64_t *)(g + 16);
        const uint32_t *buckets = (const uint32_t *)(g + 16 + (size_t)bloom_n * 8);
        const uint32_t *chains = buckets + nb;
        uint32_t h = gnu_hash(name);
        if (bloom_n) {
            uint64_t w = bloom[(h / 64) % bloom_n];
            if (!((w >> (h % 64)) & 1) || !((w >> ((h >> bloom_shift) % 64)) & 1)) return NULL;
        }
        uint32_t b = buckets[h % nb];
        if (b < symoff) return NULL;
        for (uint32_t i = b;; i++) {
            uint32_t c = chains[i - symoff];
            if ((h | 1) == (c | 1)) {
                const elf_sym *s = sym_at(L, i);
                if (s->st_shndx != SHN_UNDEF_ && !strcmp(sym_name(L, s), name)) return s;
            }
            if (c & 1) break;
        }
        return NULL;
    }
    if (L->sysv_hash) {
        const uint32_t *t = at(L, L->sysv_hash);
        uint32_t nb = t[0], nc = t[1];
        if (!nb) return NULL;
        for (uint32_t i = t[2 + sysv_hash(name) % nb]; i && i < nc; i = t[2 + nb + i]) {
            const elf_sym *s = sym_at(L, i);
            if (s->st_shndx != SHN_UNDEF_ && !strcmp(sym_name(L, s), name)) return s;
        }
    }
    return NULL;
}

static uint32_t count_dynsyms(const tl_lib *L)
{
    if (L->sysv_hash) return ((const uint32_t *)at(L, L->sysv_hash))[1];
    if (!L->gnu_hash) return 0;
    const uint8_t *g = at(L, L->gnu_hash);
    uint32_t nb, symoff, bloom_n;
    memcpy(&nb, g, 4); memcpy(&symoff, g + 4, 4); memcpy(&bloom_n, g + 8, 4);
    const uint32_t *buckets = (const uint32_t *)(g + 16 + (size_t)bloom_n * 8);
    const uint32_t *chains = buckets + nb;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoff) return symoff;
    while (!(chains[last - symoff] & 1)) last++;
    return last + 1;
}

/* ----------------------------------------------------- lookup by scope */

static tl_lib *find_loaded(const char *name)
{
    for (int i = 0; i < G.nlibs; i++) {
        if (!strcmp(G.libs[i]->name, name) || !strcmp(G.libs[i]->soname, name)) return G.libs[i];
    }
    return NULL;
}

tl_lib *tl_ld_find_lib(const char *name) { return find_loaded(name); }

static void build_scope(tl_lib *L)
{
    if (L->deps_ready) return;
    L->ndeps = 0;
    tl_lib *queue[MAX_DEPS + 1];
    int qh = 0, qt = 0;
    queue[qt++] = L;
    while (qh < qt && L->ndeps < MAX_DEPS) {
        tl_lib *cur = queue[qh++];
        for (int i = 0; i < cur->nneeded; i++) {
            const char *n = (const char *)at(cur, cur->strtab) + cur->needed[i];
            tl_lib *d = find_loaded(n);
            if (!d || d == L) continue;
            bool seen = false;
            for (int k = 0; k < L->ndeps; k++) if (L->deps[k] == d) seen = true;
            if (seen) continue;
            L->deps[L->ndeps++] = d;
            if (qt < MAX_DEPS + 1) queue[qt++] = d;
        }
    }
    L->deps_ready = true;
}

/* Symbol lookup the way a library sees it: its own scope, then the system. */
static void *lookup_for(tl_lib *L, const char *name, bool *weak_hit)
{
    (void)weak_hit;
    build_scope(L);
    const elf_sym *s = lib_find(L, name);
    if (s && (s->st_info & 0xf) != STT_GNU_IFUNC_) return sym_value(L, s);
    for (int i = 0; i < L->ndeps; i++) {
        s = lib_find(L->deps[i], name);
        if (s && (s->st_info & 0xf) != STT_GNU_IFUNC_) return sym_value(L->deps[i], s);
    }
    return tl_bionic_find(name);
}

static uint64_t run_ifunc_resolver(const void *fn);

/*
 * A defined symbol's address as dlsym gives it. An IFUNC's is not its own value -- that is the resolver -- but what the
 * resolver picks, as bionic's dlsym answers (soinfo::resolve_symbol_address). The resolver runs the way R_IRELATIVE
 * runs it, once per symbol: the answer is kept, so a symbol asked for every frame does not run guest code every frame.
 * It runs without the lock, so a resolver that itself asks dlsym cannot deadlock; two threads asking at once may
 * both run it, and the first answer kept is the one both are given.
 */
static void *sym_export(tl_lib *L, const elf_sym *s)
{
    if ((s->st_info & 0xf) != STT_GNU_IFUNC_) return sym_value(L, s);
    if (L->state < 2) {
        tl_log_line("ld: %s: IFUNC %s asked for before the library is relocated; its resolver cannot run yet", L->name, sym_name(L, s));
        return NULL;
    }
    uint32_t idx = (uint32_t)(s - sym_at(L, 0));
    pthread_mutex_lock(&G.lock);
    for (size_t i = 0; i < L->nifuncs; i++) {
        if (L->ifuncs[i].sym == idx) { uint64_t v = L->ifuncs[i].value; pthread_mutex_unlock(&G.lock); return (void *)(uintptr_t)v; }
    }
    pthread_mutex_unlock(&G.lock);
    uint64_t v = run_ifunc_resolver(L->rx + (s->st_value - L->base_vaddr));
    pthread_mutex_lock(&G.lock);
    for (size_t i = 0; i < L->nifuncs; i++) {
        if (L->ifuncs[i].sym == idx) { v = L->ifuncs[i].value; pthread_mutex_unlock(&G.lock); return (void *)(uintptr_t)v; }
    }
    if (L->nifuncs == L->capifuncs) {
        size_t cap = L->capifuncs ? L->capifuncs * 2 : 8;
        struct tl_ifunc *n = realloc(L->ifuncs, cap * sizeof(*n));
        if (n) { L->ifuncs = n; L->capifuncs = cap; }
    }
    if (L->nifuncs < L->capifuncs) L->ifuncs[L->nifuncs++] = (struct tl_ifunc){ idx, v };   /* else not kept: asked again next time */
    pthread_mutex_unlock(&G.lock);
    return (void *)(uintptr_t)v;
}

void *tl_ld_sym(tl_lib *lib, const char *name)
{
    if (lib) {
        const elf_sym *s = lib_find(lib, name);
        return s ? sym_export(lib, s) : NULL;
    }
    for (int i = 0; i < G.nlibs; i++) {
        const elf_sym *s = lib_find(G.libs[i], name);
        if (s) return sym_export(G.libs[i], s);
    }
    return NULL;
}

tl_lib *tl_ld_lib_of(const void *addr)
{
    const uint8_t *a = addr;
    for (int i = 0; i < G.nlibs; i++) {
        tl_lib *L = G.libs[i];
        size_t span = L->npages * PAGE;
        if ((a >= L->rx && a < L->rx + span) || (a >= L->rw && a < L->rw + span)) return L;
    }
    return NULL;
}

const char *tl_ld_lib_name(const tl_lib *lib) { return lib ? lib->name : NULL; }

const char *tl_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr)
{
    tl_lib *L = tl_ld_lib_of(addr);
    if (!L) return NULL;
    if (lib_name) *lib_name = L->name;
    uint64_t off = (const uint8_t *)addr >= L->rx && (const uint8_t *)addr < L->rx + L->npages * PAGE
                 ? (uint64_t)((const uint8_t *)addr - L->rx) : (uint64_t)((const uint8_t *)addr - L->rw);
    const elf_sym *best = NULL;
    uint32_t n = L->nsyms;
    for (uint32_t i = 1; i < n; i++) {
        const elf_sym *s = sym_at(L, i);
        if (s->st_shndx == SHN_UNDEF_ || !s->st_value) continue;
        uint64_t so = s->st_value - L->base_vaddr;
        if (so <= off && (!best || so > best->st_value - L->base_vaddr)) best = s;
    }
    if (!best) return NULL;
    if (sym_addr) *sym_addr = L->rx + (best->st_value - L->base_vaddr);
    return sym_name(L, best);
}

int tl_ld_iterate(tl_ld_phdr_cb cb, void *user)
{
    int r = 0;
    for (int i = 0; i < G.nlibs && !r; i++) {
        tl_lib *L = G.libs[i];
        if (L->state < 2) continue;
        r = cb((uintptr_t)(L->rx - L->base_vaddr), L->name, L->phdr, L->phnum, user);
    }
    return r;
}

/* ------------------------------------------------- unresolved-import stubs */

/*
 * An import nothing provides is bound to a 32-byte stub in the executable region
 * that loads its own name and jumps to the logger, so the first call announces
 * exactly what was missing. Failing the whole load would hide every import after
 * the first one; most of these are never called.
 */
static void tl_unresolved_called(const char *name, void *lr)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[512];
    static int nseen;
    pthread_mutex_lock(&m);
    bool again = false;
    for (int i = 0; i < nseen; i++) if (seen[i] == name) again = true;
    if (!again && nseen < 512) seen[nseen++] = name;
    pthread_mutex_unlock(&m);
    if (!again) {
        const char *ln = NULL; const void *sa = NULL;
        const char *caller = tl_ld_symbol_at(lr, &ln, &sa);
        tl_log_line("ld: CALLED an unresolved import: %s (from %s %s+%#lx)", name, ln ? ln : "?",
                    caller ? caller : "?", sa ? (unsigned long)((const char *)lr - (const char *)sa) : 0ul);
    }
}

__attribute__((naked, used)) static void tl_unresolved_entry(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "mov x0, x17\n"
        "mov x1, x30\n"
        "stp x29, x30, [sp, #-16]!\n"
        "bl _tl_unresolved_called_c\n"
        "ldp x29, x30, [sp], #16\n"
        "mov x0, #0\n"
        "ret\n");
#endif
}
void tl_unresolved_called_c(const char *name, void *lr);
void tl_unresolved_called_c(const char *name, void *lr) { tl_unresolved_called(name, lr); }

static uint8_t *g_stub_rx, *g_stub_rw;
static size_t g_stub_left;

static void *make_stub(const char *name)
{
    if (g_stub_left < 32) {
        if (!tl_xmem_alloc(PAGE, &g_stub_rx, &g_stub_rw)) return NULL;
        g_stub_left = PAGE;
    }
    uint8_t *rw = g_stub_rw, *rx = g_stub_rx;
    uint32_t code[4] = {
        0x58000090u,        /* ldr x16, #16  (the entry) */
        0x580000B1u,        /* ldr x17, #20  (the name)  */
        0xD61F0200u,        /* br  x16                   */
        0xD503201Fu,        /* nop                       */
    };
    memcpy(rw, code, 16);
    uint64_t entry = (uint64_t)(uintptr_t)tl_unresolved_entry, nm = (uint64_t)(uintptr_t)name;
    memcpy(rw + 16, &entry, 8);
    memcpy(rw + 24, &nm, 8);
    tl_xmem_flush(rx, 32);
    g_stub_rx += 32; g_stub_rw += 32; g_stub_left -= 32;
    return rx;
}

/* ------------------------------------------------------- raw system calls */

/*
 * Some libraries make Linux system calls themselves: `mov x8, #nr; svc #0`. On Darwin
 * that traps into a different kernel's table. Each such site is rewritten to branch to
 * a small stub in the library's own stub page, which saves the two scratch registers
 * the host might clobber, calls tl_svc_common, and branches back to the instruction
 * after the site. tl_svc_common saves everything else the C handler may disturb --
 * the kernel preserves all registers but x0 across a system call, so code around a
 * raw `svc` relies on that -- and calls tl_linux_syscall with the arguments and number.
 */
long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr);

__attribute__((naked, used)) void tl_svc_common(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "sub sp, sp, #512\n"
        "stp x1, x2, [sp, #0]\n"
        "stp x3, x4, [sp, #16]\n"
        "stp x5, x6, [sp, #32]\n"
        "stp x7, x8, [sp, #48]\n"
        "stp x9, x10, [sp, #64]\n"
        "stp x11, x12, [sp, #80]\n"
        "stp x13, x14, [sp, #96]\n"
        "str x15, [sp, #112]\n"
        "stp q0, q1, [sp, #128]\n"
        "stp q2, q3, [sp, #160]\n"
        "stp q4, q5, [sp, #192]\n"
        "stp q6, q7, [sp, #224]\n"
        "stp q16, q17, [sp, #256]\n"
        "stp q18, q19, [sp, #288]\n"
        "stp q20, q21, [sp, #320]\n"
        "stp q22, q23, [sp, #352]\n"
        "stp q24, q25, [sp, #384]\n"
        "stp q26, q27, [sp, #416]\n"
        "stp q28, q29, [sp, #448]\n"
        "stp q30, q31, [sp, #480]\n"
        "mov x6, x8\n"
        "bl _tl_linux_syscall\n"
        "ldp x1, x2, [sp, #0]\n"
        "ldp x3, x4, [sp, #16]\n"
        "ldp x5, x6, [sp, #32]\n"
        "ldp x7, x8, [sp, #48]\n"
        "ldp x9, x10, [sp, #64]\n"
        "ldp x11, x12, [sp, #80]\n"
        "ldp x13, x14, [sp, #96]\n"
        "ldr x15, [sp, #112]\n"
        "ldp q0, q1, [sp, #128]\n"
        "ldp q2, q3, [sp, #160]\n"
        "ldp q4, q5, [sp, #192]\n"
        "ldp q6, q7, [sp, #224]\n"
        "ldp q16, q17, [sp, #256]\n"
        "ldp q18, q19, [sp, #288]\n"
        "ldp q20, q21, [sp, #320]\n"
        "ldp q22, q23, [sp, #352]\n"
        "ldp q24, q25, [sp, #384]\n"
        "ldp q26, q27, [sp, #416]\n"
        "ldp q28, q29, [sp, #448]\n"
        "ldp q30, q31, [sp, #480]\n"
        "mov sp, x29\n"
        "ldp x29, x30, [sp], #16\n"
        "ret\n");
#endif
}

/* ------------------------------------------------------------------ probes */

__attribute__((naked, used)) void tl_probe_common(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "sub sp, sp, #624\n"
        "stp x0, x1, [sp, #0]\n"   "stp x2, x3, [sp, #16]\n"   "stp x4, x5, [sp, #32]\n"   "stp x6, x7, [sp, #48]\n"
        "stp x8, x9, [sp, #64]\n"  "stp x10, x11, [sp, #80]\n" "stp x12, x13, [sp, #96]\n" "stp x14, x15, [sp, #112]\n"
        "stp x16, x17, [sp, #128]\n" "stp x18, x19, [sp, #144]\n" "stp x20, x21, [sp, #160]\n" "stp x22, x23, [sp, #176]\n"
        "stp x24, x25, [sp, #192]\n" "stp x26, x27, [sp, #208]\n" "str x28, [sp, #224]\n"
        "stp q0, q1, [sp, #240]\n"  "stp q2, q3, [sp, #272]\n"  "stp q4, q5, [sp, #304]\n"  "stp q6, q7, [sp, #336]\n"
        "stp q16, q17, [sp, #368]\n" "stp q18, q19, [sp, #400]\n" "stp q20, q21, [sp, #432]\n" "stp q22, q23, [sp, #464]\n"
        "stp q24, q25, [sp, #496]\n" "stp q26, q27, [sp, #528]\n" "stp q28, q29, [sp, #560]\n" "stp q30, q31, [sp, #592]\n"
        "mov x0, sp\n"
        "blr x17\n"
        "ldp x0, x1, [sp, #0]\n"   "ldp x2, x3, [sp, #16]\n"   "ldp x4, x5, [sp, #32]\n"   "ldp x6, x7, [sp, #48]\n"
        "ldp x8, x9, [sp, #64]\n"  "ldp x10, x11, [sp, #80]\n" "ldp x12, x13, [sp, #96]\n" "ldp x14, x15, [sp, #112]\n"
        "ldp q0, q1, [sp, #240]\n"  "ldp q2, q3, [sp, #272]\n"  "ldp q4, q5, [sp, #304]\n"  "ldp q6, q7, [sp, #336]\n"
        "ldp q16, q17, [sp, #368]\n" "ldp q18, q19, [sp, #400]\n" "ldp q20, q21, [sp, #432]\n" "ldp q22, q23, [sp, #464]\n"
        "ldp q24, q25, [sp, #496]\n" "ldp q26, q27, [sp, #528]\n" "ldp q28, q29, [sp, #560]\n" "ldp q30, q31, [sp, #592]\n"
        "mov sp, x29\n"
        "ldp x29, x30, [sp], #16\n"
        "ret\n");
#endif
}

bool tl_ld_probe(tl_lib *L, uint64_t vaddr, void (*cb)(uint64_t *regs))
{
    uint64_t off = vaddr - L->base_vaddr;
    if (off + 4 > L->npages * PAGE || L->stub_used + 64 > L->stub_cap) return false;
    uint32_t *site_rw = (uint32_t *)(L->rw + off);
    const uint8_t *site_rx = L->rx + off;
    uint8_t *rx = L->stub_rx + L->stub_used, *rw = L->stub_rw + L->stub_used;
    L->stub_used += 64;
    uint64_t common = (uint64_t)(uintptr_t)tl_probe_common, cbv = (uint64_t)(uintptr_t)cb;
    memcpy(rw + 40, &cbv, 8);
    memcpy(rw + 48, &common, 8);
    uint32_t ldr_common = 0x58000010u | ((uint32_t)(((48 - 8) / 4) & 0x7FFFF) << 5);   /* ldr x16, [stub+48] */
    uint32_t ldr_cb     = 0x58000011u | ((uint32_t)(((40 - 12) / 4) & 0x7FFFF) << 5);  /* ldr x17, [stub+40] */
    int64_t back = ((int64_t)(site_rx + 4) - (int64_t)(rx + 32)) / 4;
    uint32_t code[10] = {
        0xA9BF7BFDu, 0xA9BF47F0u, ldr_common, ldr_cb, 0xD63F0200u /* blr x16 */,
        0xA8C147F0u, 0xA8C17BFDu, *site_rw /* the original instruction */,
        0x14000000u | ((uint32_t)back & 0x3FFFFFFu), 0xD503201Fu,
    };
    memcpy(rw, code, 40);
    int64_t to = ((int64_t)rx - (int64_t)site_rx) / 4;
    if (to <= -(1 << 25) || to >= (1 << 25)) return false;
    *site_rw = 0x14000000u | ((uint32_t)to & 0x3FFFFFFu);
    tl_xmem_flush(rx, 64);
    tl_xmem_flush(site_rx, 4);
    return true;
}

static bool in_range_b(const uint8_t *from, const uint8_t *to)
{
    int64_t o = ((int64_t)to - (int64_t)from) / 4;
    return o > -(1 << 25) && o < (1 << 25);
}
static inline uint32_t e_b(const uint8_t *from, const uint8_t *to)
{
    return 0x14000000u | ((uint32_t)(((int64_t)to - (int64_t)from) / 4) & 0x3FFFFFFu);
}

static bool stub_in_range(const uint8_t *site, const uint8_t *stub)
{
    int64_t to = ((int64_t)stub - (int64_t)site) / 4;
    return to > -(1 << 25) && to < (1 << 25);
}

/*
 * Where the next `size` bytes of stub for the site at site_rx can go, without taking them: the first of the library's
 * pools with room whose next slot the site reaches with a branch, and from whose end the stub can branch back. A branch
 * reaches 128 MiB and Minecraft's code spans 220 MiB, so there are three pools: the pages after the image (any site
 * near its end), the pages before it (sized at map time for every site the pages after cannot reach), and the dead tail
 * of a large relocation table inside the image (see relocate) for whatever an image too big for both leaves between.
 * `*used` is the pool's counter, for the caller to advance once the stub is written.
 */
static bool stub_find(tl_lib *L, const uint8_t *site_rx, size_t size, uint8_t **rx, uint8_t **rw, size_t **used)
{
    struct { uint8_t *rx, *rw; size_t *used, cap; } pools[3] = {
        { L->stub_rx, L->stub_rw, &L->stub_used, L->stub_cap },
        { L->pre_rx, L->pre_rw, &L->pre_used, L->pre_cap },
        { L->isl_rx, L->isl_rw, &L->isl_used, L->isl_cap },
    };
    for (int i = 0; i < 3; i++) {
        size_t u = *pools[i].used;
        if (!pools[i].rx || u + size > pools[i].cap) continue;
        uint8_t *at_rx = pools[i].rx + u;
        if (!stub_in_range(site_rx, at_rx) || !stub_in_range(at_rx + size, site_rx + 4)) continue;
        *rx = at_rx; *rw = pools[i].rw + u; *used = pools[i].used;
        return true;
    }
    return false;
}

/* A slot for a site, as writable and executable addresses, taken from whichever pool stub_find picks. */
static bool stub_slot(tl_lib *L, const uint8_t *site_rx, size_t size, uint8_t **rx, uint8_t **rw)
{
    size_t *used;
    if (!stub_find(L, site_rx, size, rx, rw, &used)) return false;
    *used += size;
    return true;
}

/*
 * `adr Xd, label` where the label is writable data. adr reaches only a megabyte, and the writable view of the
 * image is somewhere else entirely, so the instruction is replaced by a branch to a stub that builds the writable
 * view's address in Xd (four moves) and branches back. Linkers turn adrp+add pairs into adr when the target is
 * close, which is why small libraries have these and Unity's did not.
 */
static bool adr_stub(tl_lib *L, const uint8_t *site_rx, uint32_t rd, uint64_t target, uint32_t *branch)
{
    uint8_t *rx, *rw;
    if (!stub_slot(L, site_rx, 32, &rx, &rw)) return false;
    int64_t to = ((int64_t)rx - (int64_t)site_rx) / 4;
    int64_t back = ((int64_t)(site_rx + 4) - (int64_t)(rx + 16)) / 4;
    uint32_t code[8] = {
        0xD2800000u | ((uint32_t)(target & 0xFFFF) << 5) | rd,                  /* movz Xd, #bits 0..15 */
        0xF2A00000u | ((uint32_t)((target >> 16) & 0xFFFF) << 5) | rd,          /* movk Xd, #bits 16..31, lsl 16 */
        0xF2C00000u | ((uint32_t)((target >> 32) & 0xFFFF) << 5) | rd,          /* movk Xd, #bits 32..47, lsl 32 */
        0xF2E00000u | ((uint32_t)((target >> 48) & 0xFFFF) << 5) | rd,          /* movk Xd, #bits 48..63, lsl 48 */
        0x14000000u | ((uint32_t)back & 0x3FFFFFFu),                            /* b site+4 */
        0xD503201Fu, 0xD503201Fu, 0xD503201Fu,
    };
    memcpy(rw, code, 32);
    *branch = 0x14000000u | ((uint32_t)to & 0x3FFFFFFu);
    return true;
}

/*
 * A stub for the `svc` at site_rx; returns its executable address, or NULL when no pool has room within range. The
 * handler's address is the stub's own literal: a pool can be megabytes long, and `ldr` (literal) reaches only one.
 */
#define SVC_STUB_BYTES 40
static uint8_t *svc_stub(tl_lib *L, const uint8_t *site_rx)
{
    uint8_t *rx, *rw;
    if (!stub_slot(L, site_rx, SVC_STUB_BYTES, &rx, &rw)) return NULL;
    uint32_t code[8] = {
        0xA9BF7BFDu,                    /* stp x29, x30, [sp, #-16]! */
        0xA9BF47F0u,                    /* stp x16, x17, [sp, #-16]! */
        0x580000D0u,                    /* ldr x16, +24  (the literal at +32) */
        0xD63F0200u,                    /* blr x16 */
        0xA8C147F0u,                    /* ldp x16, x17, [sp], #16 */
        0xA8C17BFDu,                    /* ldp x29, x30, [sp], #16 */
        e_b(rx + 24, site_rx + 4),      /* b site+4 */
        0xD503201Fu,                    /* nop */
    };
    uint64_t h = (uint64_t)(uintptr_t)tl_svc_common;
    memcpy(rw, code, 32);
    memcpy(rw + 32, &h, 8);
    return rx;
}

/* --------------------------------------------------------------------- x18 */

/*
 * Apple reserves x18, and the kernel zeroes it on every exception return -- every
 * page fault, every interrupt -- while Android compilers use it as one more scratch
 * register and keep values in it across instructions that can fault. Left alone,
 * guest code that does that computes with a zero at some random point and dies
 * with an address of 0xfffffffffffffff0.
 *
 * So no guest instruction ever holds a live value in the real x18. Each one that
 * names it is replaced by a branch to a small stub that works on a "virtual x18"
 * kept in the thread's own TSD array, which the kernel does not touch: the stub
 * loads the virtual value into a scratch register, runs the original instruction
 * with that register in place of x18, writes the (possibly changed) value back, and
 * branches to the following instruction. The scratch registers are saved in the
 * 128 bytes below sp that Apple's ABI keeps free of signal frames. Nothing the
 * kernel does can land between two instructions of a stub and be seen: only x18
 * is ever lost, and x18 is not used.
 *
 * Instructions that read their operand's value as part of control flow or that are
 * pc-relative cannot be copied into a stub unchanged, so they get their own
 * shapes: adrp/adr/ldr-literal into x18 become the constant they compute, cbz and
 * tbz become a test of the loaded value that branches on to the original target,
 * and `br x18` jumps through a scratch register.
 */
#define X18_STUB_BYTES 64

static int64_t g_vx18_off = -1;     /* byte offset of the virtual-x18 slot from the TSD base */

/*
 * Where a pthread key's value lives, as a byte offset from the TSD base that TPIDRRO_EL0 holds (low three bits
 * masked off), so generated code can read and write it with one load or store and no call. Found by storing a
 * sentinel and looking for it, because Darwin does not promise a key's slot index. -1 when it cannot be found, or
 * is beyond what a scaled 12-bit load offset reaches.
 */
static int64_t tsd_slot_of(pthread_key_t key)
{
#if defined(__aarch64__)
    const uintptr_t sentinel = (uintptr_t)0x5a5a1234deadbeefull;
    void *old = pthread_getspecific(key);
    pthread_setspecific(key, (void *)sentinel);
    uintptr_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    base &= ~(uintptr_t)7;
    const volatile uintptr_t *tsd = (const volatile uintptr_t *)base;
    int64_t off = -1;
    for (int i = 0; i < 520; i++) if (tsd[i] == sentinel) { off = (int64_t)i * 8; break; }
    pthread_setspecific(key, old);
    return off > 32760 ? -1 : off;
#else
    (void)key;
    return -1;
#endif
}

static bool vx18_init(void)
{
    if (g_vx18_off >= 0) return true;
    pthread_key_t key;
    if (pthread_key_create(&key, NULL)) return false;
    int64_t off = tsd_slot_of(key);
    if (off < 0) { pthread_key_delete(key); return false; }
    g_vx18_off = off;
    return true;
}

/* The value of the calling thread's virtual x18, for a signal handler to save and restore around guest handlers. */
uint64_t tl_vx18_get(void)
{
#if defined(__aarch64__)
    if (g_vx18_off < 0) return 0;
    uintptr_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    return *(const volatile uint64_t *)((base & ~(uintptr_t)7) + (uintptr_t)g_vx18_off);
#else
    return 0;
#endif
}
void tl_vx18_set(uint64_t v)
{
#if defined(__aarch64__)
    if (g_vx18_off < 0) return;
    uintptr_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    *(volatile uint64_t *)((base & ~(uintptr_t)7) + (uintptr_t)g_vx18_off) = v;
#else
    (void)v;
#endif
}

static inline uint32_t e_stur(unsigned rt, int imm)  { return 0xF8000000u | (((uint32_t)imm & 0x1FFu) << 12) | (31u << 5) | rt; }
static inline uint32_t e_ldur(unsigned rt, int imm)  { return 0xF8400000u | (((uint32_t)imm & 0x1FFu) << 12) | (31u << 5) | rt; }
static inline uint32_t e_mrs_tsd(unsigned rt)        { return 0xD53BD060u | rt; }
static inline uint32_t e_and_tsd(unsigned r)         { return 0x927DF000u | (r << 5) | r; }          /* and r, r, #~7 */
static inline uint32_t e_ldr_off(unsigned rt, unsigned rn, int64_t off) { return 0xF9400000u | ((uint32_t)(off / 8) << 10) | (rn << 5) | rt; }   /* ldr rt, [rn, #off] */
static inline uint32_t e_ldr_slot(unsigned rt, unsigned rn) { return e_ldr_off(rt, rn, g_vx18_off); }
static inline uint32_t e_str_slot(unsigned rt, unsigned rn) { return 0xF9000000u | ((uint32_t)(g_vx18_off / 8) << 10) | (rn << 5) | rt; }

static int e_mov64(uint32_t *out, unsigned rd, uint64_t v)
{
    int n = 0;
    out[n++] = 0xD2800000u | (uint32_t)((v & 0xFFFF) << 5) | rd;                       /* movz rd, #lo */
    for (int sh = 1; sh < 4; sh++) {
        uint64_t part = (v >> (16 * sh)) & 0xFFFF;
        if (part) out[n++] = 0xF2800000u | ((uint32_t)sh << 21) | ((uint32_t)part << 5) | rd;   /* movk */
    }
    return n;
}

static unsigned pick_scratch(uint32_t used, unsigned avoid)
{
    static const unsigned order[] = { 16, 17, 15, 14, 13, 12, 11, 10, 9 };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (!(used & (1u << order[i])) && order[i] != avoid) return order[i];
    return 0;
}

enum { X18_NONE = 0, X18_DONE = 1, X18_FAILED = 2 };

/* Rewrite the instruction at site_rw (executable address pc) if it names x18. */
static int x18_rewrite(tl_lib *L, uint32_t *site_rw, const uint8_t *pc, ptrdiff_t delta)
{
    uint32_t insn = *site_rw;
    if ((insn & 31u) != 18 && ((insn >> 5) & 31u) != 18 && ((insn >> 10) & 31u) != 18 && ((insn >> 16) & 31u) != 18)
        return X18_NONE;
    bool known;
    if (!a64_uses_gpr(insn, 18, &known)) return X18_NONE;
    if (!known) {
        if (G.verbosity >= 2) tl_log_line("ld: %s: unrecognised instruction %08x at +%#llx looks like it uses x18", L->name, insn, (unsigned long long)(pc - L->rx));
        return X18_FAILED;
    }
    uint8_t *rx, *rw; size_t *pool_used;
    if (g_vx18_off < 0 || !stub_find(L, pc, X18_STUB_BYTES, &rx, &rw, &pool_used)) return X18_FAILED;

    uint32_t c[X18_STUB_BYTES / 4];
    int n = 0;
    const uint8_t *back = pc + 4;
    uint32_t used = a64_gpr_mask(insn);
#define EMIT(word) do { uint32_t w_ = (word); c[n++] = w_; } while (0)
#define EMIT_B(to) do { uint32_t w_ = e_b(rx + n * 4, (to)); c[n++] = w_; } while (0)

    if ((insn & 0x9F000000u) == 0x90000000u || (insn & 0x9F000000u) == 0x10000000u || (insn & 0xBF000000u) == 0x18000000u) {
        /* pc-relative into x18: the value is a constant of the site, known now */
        uint64_t value;
        if ((insn & 0x9F000000u) == 0x90000000u || (insn & 0x9F000000u) == 0x10000000u) {
            bool page = (insn & 0x80000000u) != 0;
            int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
            if (imm & 0x100000) imm -= 0x200000;
            uintptr_t tp = page ? ((uintptr_t)pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm * 4096) : (uintptr_t)pc + (uintptr_t)imm;
            if (tp >= (uintptr_t)L->rx && tp < (uintptr_t)L->rx + L->npages * PAGE && (L->pflags[(tp - (uintptr_t)L->rx) / PAGE] & TL_PAGE_W))
                tp += (uintptr_t)delta;
            value = tp;
        } else {
            int64_t imm = (int64_t)((insn >> 5) & 0x7FFFFu);
            if (imm & 0x40000) imm -= 0x80000;
            const uint8_t *src = (const uint8_t *)((uintptr_t)pc + (uintptr_t)(imm * 4) + (uintptr_t)delta);
            value = (insn & 0x40000000u) ? *(const uint64_t *)src : (uint64_t)*(const uint32_t *)src;
        }
        unsigned S = 16, T = 17;
        EMIT(e_stur(S, -16)); EMIT(e_stur(T, -8));
        n += e_mov64(c + n, S, value);
        EMIT(e_mrs_tsd(T)); EMIT(e_and_tsd(T)); EMIT(e_str_slot(S, T));
        EMIT(e_ldur(T, -8)); EMIT(e_ldur(S, -16));
        EMIT_B(back);
    } else if ((insn & 0xFFFFFC1Fu) == 0xD61F0000u) {                       /* br x18 */
        EMIT(e_mrs_tsd(17)); EMIT(e_and_tsd(17)); EMIT(e_ldr_slot(17, 17));
        EMIT(0xD61F0000u | (17u << 5));
    } else if ((insn & 0x7E000000u) == 0x34000000u || (insn & 0x7E000000u) == 0x36000000u) {   /* cbz, cbnz, tbz, tbnz */
        bool is_tb = (insn & 0x7E000000u) == 0x36000000u;
        int64_t imm = is_tb ? (int64_t)((insn >> 5) & 0x3FFFu) : (int64_t)((insn >> 5) & 0x7FFFFu);
        int64_t sign = is_tb ? 0x2000 : 0x40000;
        if (imm & sign) imm -= sign * 2;
        const uint8_t *target = pc + imm * 4;
        unsigned S = 16;
        EMIT(e_stur(S, -16));
        EMIT(e_mrs_tsd(S)); EMIT(e_and_tsd(S)); EMIT(e_ldr_slot(S, S));
        /* the test at index 4 branches to index 7 when taken: three instructions on */
        uint32_t field_mask = is_tb ? (0x3FFFu << 5) : (0x7FFFFu << 5);
        EMIT((insn & ~(field_mask | 0x1Fu)) | (3u << 5) | S);
        EMIT(e_ldur(S, -16));
        EMIT_B(back);
        EMIT(e_ldur(S, -16));
        if (!in_range_b(rx + n * 4, target)) return X18_FAILED;
        EMIT_B(target);
    } else {
        /* a base register of sp with writeback would collide with the saved scratch registers */
        bool pair = (insn & 0x3A000000u) == 0x28000000u, single = (insn & 0x3B000000u) == 0x38000000u;
        unsigned mode = pair ? (insn >> 23) & 3u : (insn >> 10) & 3u;
        if ((pair || (single && !((insn >> 21) & 1u))) && ((insn >> 5) & 31u) == 31 && (mode == 1 || mode == 3)) {
            tl_log_line("ld: %s: x18 instruction %08x at +%#llx writes back sp", L->name, insn, (unsigned long long)(pc - L->rx));
            return X18_FAILED;
        }
        unsigned S = pick_scratch(used, 0), T = pick_scratch(used, S);
        if (!S || !T) return X18_FAILED;
        EMIT(e_stur(S, -16)); EMIT(e_stur(T, -8));
        EMIT(e_mrs_tsd(S)); EMIT(e_and_tsd(S)); EMIT(e_ldr_slot(S, S));
        EMIT(a64_subst_gpr(insn, 18, S));
        EMIT(e_mrs_tsd(T)); EMIT(e_and_tsd(T)); EMIT(e_str_slot(S, T));
        EMIT(e_ldur(T, -8)); EMIT(e_ldur(S, -16));
        EMIT_B(back);
    }
#undef EMIT
#undef EMIT_B
    memcpy(rw, c, (size_t)n * 4);
    for (int i = n; i < X18_STUB_BYTES / 4; i++) ((uint32_t *)rw)[i] = 0xD503201Fu;
    *pool_used += X18_STUB_BYTES;      /* taken only now: a site that fails above leaves the slot to the next */
    *site_rw = e_b(pc, rx);
    return X18_DONE;
}

/* ------------------------------------------------------------ thread blocks */

/*
 * bionic's arm64 thread pointer (TPIDR_EL0) points at its thread control block: eight 8-byte slots at TP+0..63,
 * of which compiled code reads TLS_SLOT_STACK_GUARD (TP+0x28) in every function built with a stack protector, plus
 * three slots at negative offsets that only bionic itself uses. Darwin keeps its own thread pointer in TPIDRRO_EL0
 * and gives TPIDR_EL0 no meaning a guest may rely on, so every `mrs Xt, tpidr_el0` is rewritten to produce the
 * thread pointer of a block this file owns.
 *
 * Each thread that runs guest code for long gets its own block (tl_ld_thread_attach; guest threads get theirs
 * from pthread_create's shim), its address kept as the value of a pthread key. The rewritten site branches to a
 * stub that reads that key's TSD slot directly -- the same way the virtual x18 is reached -- using nothing but Xt:
 *
 *      mrs  Xt, tpidrro_el0        TSD base, with the CPU number in the low bits
 *      and  Xt, Xt, #~7
 *      ldr  Xt, [Xt, #slot]        this thread's block, or 0
 *      cbnz Xt, 1f
 *      ldr  Xt, =fallback          a thread with no block of its own shares one
 *   1: b    <site + 4>
 *
 * Threads without a block (the main thread, short-lived host callbacks) share the fallback, which is what every
 * thread shared before. The fallback lives in the executable region's writable view so that a site with no stub
 * pool in branch range can still be given `adrp Xt, fallback`: blocks are plain data otherwise, mmap'd.
 *
 * ELF TLS lives in the same blocks, laid out as bionic lays out static TLS on arm64 (variant 1): after the eight
 * slots, each module's PT_TLS block at a TP offset fixed when the module is mapped -- aligned to its p_align, with
 * its p_vaddr % p_align skew kept, as StaticTlsLayout::reserve does -- in an area of a fixed size every block has
 * (TL_STATIC_TLS_KB, 256 KiB by default; a module that does not fit is refused). TP is 4096-aligned, which bounds
 * the p_align accepted. Bionic gives libraries dlopen'd after start-up dynamic TLS instead; here every module is
 * static, so initial-exec (R_TLS_TPREL) works for all of them, TLSDESC resolves to bionic's static resolver
 * (`ldr x0, [x0, #8]; ret`), and __tls_get_addr is TP + the module's offset + the offset asked for. A module's
 * .tdata/.tbss image is copied into every existing block once the module is relocated, and into each new block
 * as it is made. Threads sharing the fallback block share its thread-locals too.
 *
 * Every block carries the same stack-protector cookie, drawn at random once per process. It has to be the same
 * for a function's entry and exit only, which one per thread would also be, but a guest thread can outlive or
 * predate its block (key destructors run in some order; a callback can start on a thread before attaching), and a
 * function that sees two blocks must still see one cookie.
 */
#define TCB_PRE   4096u            /* before TP: the block's bookkeeping, and bionic's negative slots (all zero) */
#define TCB_SLOTS 64u              /* TP+0..63: bionic's slots 0..7 */
#define TLS_ALIGN_MAX 4096u        /* TP's alignment in every block, so the largest PT_TLS p_align that can be honoured */

typedef struct tcb_hdr { struct tcb_hdr *next, *prev; unsigned rounds; bool shared; } tcb_hdr;

static struct {
    pthread_mutex_t lock;          /* the list of blocks */
    pthread_once_t once;
    pthread_key_t key;             /* this thread's TP, or NULL */
    bool have_key;
    int64_t slot;                  /* the key's TSD slot offset, for the stubs; -1 if unknown */
    size_t span;                   /* bytes from a block's start to its end */
    uint64_t cookie;
    uint8_t *fallback;             /* TP of the shared block, in the writable view */
    tcb_hdr *live;                 /* every block, the fallback's included */
    size_t area;                   /* bytes from TP to a block's end: the slots, then the modules' static TLS */
    size_t cursor;                 /* the first TP offset no module has */
    tl_lib *mods[MAX_LIBS];        /* modules with a PT_TLS, by id - 1 */
    size_t nmods;
    const uint8_t *desc_static, *desc_weak;   /* TLSDESC resolvers, executable */
} T = { .lock = PTHREAD_MUTEX_INITIALIZER, .once = PTHREAD_ONCE_INIT, .slot = -1 };

static inline tcb_hdr *tcb_of(uint8_t *tp) { return (tcb_hdr *)(tp - TCB_PRE); }

/* A module's initial thread-local image into the block at tp: .tdata from the relocated image, .tbss zeroed. */
static void tls_fill(uint8_t *tp, const tl_lib *L)
{
    uint8_t *d = tp + L->tls_off;
    memcpy(d, at(L, L->tls_vaddr), (size_t)L->tls_filesz);
    memset(d + L->tls_filesz, 0, (size_t)(L->tls_memsz - L->tls_filesz));
}

/* Lay out a new block at `base` and put it on the list. Called with T.lock held. */
static uint8_t *tcb_setup(uint8_t *base, bool shared)
{
    uint8_t *tp = base + TCB_PRE;
    uint64_t *s = (uint64_t *)tp;
    s[0] = (uint64_t)(uintptr_t)tp;     /* self: what x86 code expects at slot 0, harmless where the DTV would be */
    s[1] = 1000; s[2] = 1000;
    s[5] = T.cookie;                    /* TLS_SLOT_STACK_GUARD */
    tcb_hdr *h = tcb_of(tp);
    h->shared = shared;
    h->prev = NULL;
    h->next = T.live;
    if (T.live) T.live->prev = h;
    T.live = h;
    for (size_t i = 0; i < T.nmods; i++) if (T.mods[i]->tls_ready) tls_fill(tp, T.mods[i]);
    return tp;
}

static void tcb_dtor(void *tp);

static void tcb_init_once(void)
{
    arc4random_buf(&T.cookie, sizeof(T.cookie));
    const char *kb = getenv("TL_STATIC_TLS_KB");
    long k = kb ? strtol(kb, NULL, 10) : 256;
    if (k < 4 || k > 65536) k = 256;
    T.area = (size_t)k << 10;
    T.cursor = TCB_SLOTS;
    T.span = TCB_PRE + T.area;
    T.span = (T.span + 4095u) & ~(size_t)4095u;
    if (pthread_key_create(&T.key, tcb_dtor)) return;
    T.have_key = true;
    T.slot = tsd_slot_of(T.key);
    if (T.slot < 0) tl_log_line("ld: no thread-specific slot for thread blocks; every thread will share one");
}

/* The calling thread's TP: its own block, or the shared one. */
static uint8_t *tcb_current(void)
{
    uint8_t *tp = T.have_key ? pthread_getspecific(T.key) : NULL;
    return tp ? tp : T.fallback;
}

static void tcb_release(uint8_t *tp)
{
    tcb_hdr *h = tcb_of(tp);
    pthread_mutex_lock(&T.lock);
    if (h->prev) h->prev->next = h->next; else T.live = h->next;
    if (h->next) h->next->prev = h->prev;
    pthread_mutex_unlock(&T.lock);
    munmap(tp - TCB_PRE, T.span);
}

/*
 * The key's destructor, at thread exit. Darwin clears the slot before calling it, after which the thread reads the
 * shared block. Guest code still runs here -- its own pthread keys' destructors, which bionic calls in the same
 * rounds -- so the block is put back and kept until the last round, PTHREAD_DESTRUCTOR_ITERATIONS, and only then
 * released; keys whose destructors run after ours in that last round see the shared block.
 */
static void tcb_dtor(void *v)
{
    uint8_t *tp = v;
    if (++tcb_of(tp)->rounds < PTHREAD_DESTRUCTOR_ITERATIONS && !pthread_setspecific(T.key, tp)) return;
    tcb_release(tp);
}

bool tl_ld_thread_attach(void)
{
    pthread_once(&T.once, tcb_init_once);
    if (!T.have_key) return false;
    if (pthread_getspecific(T.key)) return true;
    uint8_t *base = mmap(NULL, T.span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return false;
    pthread_mutex_lock(&T.lock);
    uint8_t *tp = tcb_setup(base, false);
    pthread_mutex_unlock(&T.lock);
    if (pthread_setspecific(T.key, tp)) { tcb_release(tp); return false; }
    return true;
}

void tl_ld_thread_detach(void)
{
    if (!T.have_key) return;
    uint8_t *tp = pthread_getspecific(T.key);
    if (!tp) return;
    pthread_setspecific(T.key, NULL);
    tcb_release(tp);
}

/* The shared block, in the executable region so that `adrp` reaches it from code; and the per-process state. */
static bool ensure_tcb(void)
{
    pthread_once(&T.once, tcb_init_once);
    if (T.fallback) return true;
    size_t n = (T.span + PAGE - 1) / PAGE * PAGE;
    uint8_t *rx, *rw;
    if (!tl_xmem_alloc(n, &rx, &rw)) return false;
    memset(rw, 0, n);
    pthread_mutex_lock(&T.lock);
    T.fallback = tcb_setup(rw, true);
    pthread_mutex_unlock(&T.lock);

    /*
     * TLSDESC resolvers. A descriptor is { resolver, argument }; code calls the resolver with x0 = the descriptor
     * and adds the result to TP, and the resolver may change nothing but x0. Every module has static TLS, so the
     * argument is already the TP offset (bionic's tlsdesc_resolver_static). An undefined weak thread-local must
     * come out as address 0 + addend, so its resolver returns the argument minus this thread's TP, found the way
     * the tpidr stubs find it (bionic's tlsdesc_resolver_unresolved_weak).
     */
    if (!tl_xmem_alloc(PAGE, &rx, &rw)) return false;
    uint32_t *c = (uint32_t *)rw;
    c[0] = 0xF9400400u;                             /* ldr  x0, [x0, #8] */
    c[1] = 0xD65F03C0u;                             /* ret */
    c[2] = c[3] = 0xD503201Fu;
    uint32_t *w = c + 4;                            /* at +16 */
    w[0] = 0xF81F0FE1u;                             /* str  x1, [sp, #-16]! */
    if (T.slot >= 0) {
        w[1] = e_mrs_tsd(1);                        /* mrs  x1, tpidrro_el0 */
        w[2] = e_and_tsd(1);                        /* and  x1, x1, #~7 */
        w[3] = e_ldr_off(1, 1, T.slot);             /* ldr  x1, [x1, #slot] */
        w[4] = 0xB5000041u;                         /* cbnz x1, +8 */
    } else {
        w[1] = w[2] = w[3] = w[4] = 0xD503201Fu;    /* no slot: every thread has the shared block */
    }
    w[5] = 0x580000E1u;                             /* ldr  x1, +28  (the literal) */
    w[6] = 0xF9400400u;                             /* ldr  x0, [x0, #8] */
    w[7] = 0xCB010000u;                             /* sub  x0, x0, x1 */
    w[8] = 0xF84107E1u;                             /* ldr  x1, [sp], #16 */
    w[9] = 0xD65F03C0u;                             /* ret */
    w[10] = w[11] = 0xD503201Fu;
    uint64_t fb = (uint64_t)(uintptr_t)T.fallback;
    memcpy(w + 12, &fb, 8);                         /* at +64 */
    tl_xmem_flush(rx, 80);
    T.desc_static = rx;
    T.desc_weak = rx + 16;
    return true;
}

/*
 * Bionic's static TLS placement (align_checked, then reserve): the cursor rounded up to the next offset that is
 * `skew` past a multiple of `align` (a power of two, skew < align), the module's block there, the cursor past it. False, with
 * nothing changed, when the block would run past `area`.
 */
static bool tls_place(size_t *cursor, size_t area, uint64_t memsz, uint64_t align, uint64_t skew, size_t *off)
{
    if (!align || (align & (align - 1)) || skew >= align || memsz > area) return false;
    uint64_t o = ((*cursor - skew + align - 1) & ~(align - 1)) + skew;
    if (o < *cursor || o > area - memsz) return false;
    *off = (size_t)o;
    *cursor = (size_t)(o + memsz);
    return true;
}

/* A PT_TLS segment's block in every thread's static TLS, logged and refused when it cannot have one. */
static bool tls_reserve(const char *name, const elf_phdr *p, size_t *off)
{
    uint64_t align = p->p_align ? p->p_align : 1;
    if (align & (align - 1)) { tl_log_line("ld: %s: its TLS segment's alignment %llu is not a power of two", name, (unsigned long long)align); return false; }
    if (align > TLS_ALIGN_MAX) { tl_log_line("ld: %s: its TLS segment wants %llu-byte alignment, more than the %u thread blocks have", name, (unsigned long long)align, TLS_ALIGN_MAX); return false; }
    if (p->p_filesz > p->p_memsz) { tl_log_line("ld: %s: its TLS segment's file size exceeds its memory size", name); return false; }
    pthread_mutex_lock(&T.lock);
    size_t before = T.cursor;
    bool ok = T.nmods < MAX_LIBS && tls_place(&T.cursor, T.area, p->p_memsz, align, p->p_vaddr % align, off);
    pthread_mutex_unlock(&T.lock);
    if (!ok) tl_log_line("ld: %s: its %llu bytes of thread-local storage do not fit in the static TLS area "
                         "(%zu of %zu bytes taken); raise TL_STATIC_TLS_KB", name, (unsigned long long)p->p_memsz, before, T.area);
    return ok;
}

/* A mapped module's TLS block becomes known: it gets its module id. Its image is not copied until it is relocated. */
static void tls_register(tl_lib *L)
{
    pthread_mutex_lock(&T.lock);
    T.mods[T.nmods] = L;
    L->tls_id = T.nmods + 1;
    T.nmods++;
    pthread_mutex_unlock(&T.lock);
}

/* A relocated module's initial image, into every block there is; blocks made later copy it themselves. */
static void tls_publish(tl_lib *L)
{
    if (!L->tls_id) return;
    pthread_mutex_lock(&T.lock);
    for (tcb_hdr *h = T.live; h; h = h->next) tls_fill((uint8_t *)h + TCB_PRE, L);
    L->tls_ready = true;
    pthread_mutex_unlock(&T.lock);
}

static void *tls_sym_addr(const tl_lib *L, const elf_sym *s)
{
    uint8_t *tp = tcb_current();
    return tp && L->tls_id ? tp + L->tls_off + s->st_value : NULL;
}

/* bionic's __tls_get_addr: every module is in static TLS, so it is one addition from the calling thread's TP. */
void *tl_ld_tls_get_addr(const tl_tls_index *ti)
{
    uint8_t *tp = tcb_current();
    if (!tp || !ti || !ti->module || ti->module > T.nmods) return NULL;
    return tp + T.mods[ti->module - 1]->tls_off + ti->offset;
}

/*
 * The stub a `mrs Xt, tpidr_el0` at site_rx branches to (see above): 32 bytes, the last 8 the fallback's address.
 * False when no TSD slot is known or no stub pool is in range; the caller then points Xt at the shared block.
 */
static bool tpidr_stub(tl_lib *L, const uint8_t *site_rx, unsigned rt, uint32_t *branch)
{
    uint8_t *rx, *rw;
    if (T.slot < 0 || !stub_slot(L, site_rx, 32, &rx, &rw)) return false;
    uint32_t code[6] = {
        e_mrs_tsd(rt),                  /* mrs  Xt, tpidrro_el0 */
        e_and_tsd(rt),                  /* and  Xt, Xt, #~7 */
        e_ldr_off(rt, rt, T.slot),      /* ldr  Xt, [Xt, #slot] */
        0xB5000040u | rt,               /* cbnz Xt, +8  (to the branch back) */
        0x58000040u | rt,               /* ldr  Xt, +8  (the literal) */
        e_b(rx + 20, site_rx + 4),      /* b    site+4 */
    };
    uint64_t fb = (uint64_t)(uintptr_t)T.fallback;
    memcpy(rw, code, sizeof(code));
    memcpy(rw + 24, &fb, 8);
    *branch = e_b(site_rx, rx);
    return true;
}

/* --------------------------------------------------------------- patching */

#if defined(__aarch64__)
static uint32_t encode_adrp(uint32_t rt, const void *pc, const void *target)
{
    int64_t delta = ((int64_t)((uintptr_t)target & ~(uintptr_t)0xFFF)
                   - (int64_t)((uintptr_t)pc & ~(uintptr_t)0xFFF)) >> 12;
    uint32_t imm = (uint32_t)delta & 0x1FFFFF;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rt & 0x1Fu);
}

/*
 * Rewrites in executable pages, all on the writable view; among them:
 *
 *  - `mrs Xt, tpidr_el0` becomes a branch to a stub that produces the calling
 *    thread's block (see "thread blocks"); where no stub is in branch range, an
 *    `adrp Xt` of the block threads without their own share.
 *  - an `adrp` that points into this image's writable pages is retargeted at the
 *    writable view, because code reaches its globals pc-relatively and the page
 *    the executable view shows is not writable.
 */
static void patch_image(tl_lib *L, size_t *n_tpidr, size_t *n_adrp, size_t *n_adr, size_t *n_svc)
{
    ptrdiff_t delta = L->rw - L->rx;
    *n_tpidr = *n_adrp = *n_adr = *n_svc = 0;
    /* Only instructions are patched: many libraries put .rodata and .eh_frame in the same
     * executable segment as .text, and a data word that happens to look like an adrp or a
     * load must be left alone. */
    for (int r = 0; r < L->ncode; r++) {
        uint32_t *w = (uint32_t *)(L->rw + (L->code[r].start - L->base_vaddr));
        const uint8_t *x = L->rx + (L->code[r].start - L->base_vaddr);
        size_t nwords = (size_t)((L->code[r].end - L->code[r].start) / 4);
        for (size_t i = 0; i < nwords; i++) {
            uint32_t insn = w[i];
            const uint8_t *pc = x + i * 4;
            if ((insn & 0xFFFFFFE0u) == 0xD53BD040u) {                  /* mrs Xt, tpidr_el0 */
                unsigned rt = insn & 0x1Fu;
                uint32_t branch;
                if (rt == 31) w[i] = 0xD503201Fu;                       /* into xzr: nothing to produce */
                else if (rt == 18) { L->n_x18_failed++; continue; }     /* Android's compilers never allocate x18 */
                else if (tpidr_stub(L, pc, rt, &branch)) w[i] = branch;
                else { w[i] = encode_adrp(rt, pc, T.fallback); L->n_tpidr_shared++; }
                (*n_tpidr)++;
                continue;
            }
            int xr = x18_rewrite(L, &w[i], pc, delta);
            if (xr == X18_DONE) { L->n_x18++; continue; }
            if (xr == X18_FAILED) { L->n_x18_failed++; continue; }
            if ((insn & 0xFFFFFFE0u) == 0xD53B0020u) {            /* mrs Xt, ctr_el0: privileged for user code on Apple silicon */
                /* The cache type register: the smallest cache lines the program may assume. Code that flushes the
                 * instruction cache (V8) reads it to step by line; 64 bytes is right for Apple's and safe for any. */
                uint32_t branch;
                if (adr_stub(L, pc, insn & 0x1Fu, 0x8444c004ull, &branch)) { w[i] = branch; L->n_ctr++; }
                else L->n_adr_failed++;
            } else if ((insn & 0x9F000000u) == 0x90000000u) {     /* adrp */
                int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
                if (imm & 0x100000) imm -= 0x200000;
                uintptr_t tp = ((uintptr_t)pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
                if (tp >= (uintptr_t)L->rx && tp < (uintptr_t)L->rx + L->npages * PAGE) {
                    size_t tpg = (tp - (uintptr_t)L->rx) / PAGE;
                    if (L->pflags[tpg] & TL_PAGE_W) {
                        w[i] = encode_adrp(insn & 0x1Fu, pc, (const void *)(tp + (uintptr_t)delta));
                        (*n_adrp)++;
                    }
                }
            } else if (insn == 0xD4000001u) {                       /* svc #0 */
                uint8_t *stub = svc_stub(L, pc);
                int64_t off = stub ? ((int64_t)stub - (int64_t)pc) / 4 : 0;
                if (stub && off > -(1 << 25) && off < (1 << 25)) {
                    w[i] = 0x14000000u | ((uint32_t)off & 0x3FFFFFFu);
                    (*n_svc)++;
                } else {
                    /* No pool within a branch has room: an image whose code spans more than two branch ranges with
                     * no relocation-table tail between them, or pools sized short. Such a site gets the answer a
                     * kernel without the call gives -- ENOSYS -- which is what the code around it (a crash reporter's
                     * raw-syscall wrappers, in Minecraft's case) is written to cope with; relocate says how many. */
                    w[i] = 0x92800000u | (37u << 5);                /* movn x0, #37  (x0 = -ENOSYS) */
                    L->n_svc_far++;
                }
            } else if ((insn & 0x9F000000u) == 0x10000000u) {     /* adr */
                int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
                if (imm & 0x100000) imm -= 0x200000;
                uintptr_t tp = (uintptr_t)pc + (uintptr_t)imm;
                if (tp >= (uintptr_t)L->rx && tp < (uintptr_t)L->rx + L->npages * PAGE
                    && (L->pflags[(tp - (uintptr_t)L->rx) / PAGE] & TL_PAGE_W)) {
                    uint32_t branch;
                    if (adr_stub(L, pc, insn & 0x1Fu, (uint64_t)(tp + (uintptr_t)delta), &branch)) { w[i] = branch; (*n_adr)++; }
                    else L->n_adr_failed++;
                }
            }
        }
    }
}
#else
static void patch_image(tl_lib *L, size_t *a, size_t *b, size_t *c, size_t *d) { (void)L; *a = *b = *c = *d = 0; }
#endif

/* -------------------------------------------------------------- relocation */

static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* An address inside the image as pointer data records it: writable view for data. */
static uint64_t image_addr(const tl_lib *L, uint64_t vaddr)
{
    uint64_t off = vaddr - L->base_vaddr;
    size_t page = (size_t)(off / PAGE);
    if (page < L->npages && (L->pflags[page] & TL_PAGE_W)) return (uint64_t)(uintptr_t)(L->rw + off);
    return (uint64_t)(uintptr_t)(L->rx + off);
}

static uint64_t bind_symbol(tl_lib *L, uint32_t symidx, bool *failed)
{
    if (L->symcache && L->symcache[symidx]) return L->symcache[symidx];
    const elf_sym *s = sym_at(L, symidx);
    const char *name = sym_name(L, s);
    uint64_t val = 0;
    if (s->st_shndx != SHN_UNDEF_ && (s->st_info >> 4) != STB_WEAK_) {
        /* Defined here. Search the scope anyway so an earlier library's definition
         * wins, as it does under ELF interposition... except that a library's own
         * definition is what Android's linker uses first for its own symbols. */
        if ((s->st_info & 0xf) == STT_GNU_IFUNC_) { *failed = true; return 0; }
        val = (uint64_t)(uintptr_t)sym_value(L, s);
    } else {
        void *a = lookup_for(L, name, NULL);
        if (a) {
            val = (uint64_t)(uintptr_t)a;
        } else if ((s->st_info >> 4) == STB_WEAK_) {
            val = 0;
        } else {
            void *stub = make_stub(name);
            if (!stub) { *failed = true; return 0; }
            val = (uint64_t)(uintptr_t)stub;
            L->n_unresolved++;
            G.unresolved++;
            if (G.verbosity >= 2) tl_log_line("ld: %s: unresolved import %s", L->name, name);
        }
    }
    if (L->symcache) L->symcache[symidx] = val ? val : 1;   /* 1 marks a resolved NULL */
    return val;
}

/*
 * An IFUNC resolver picks an implementation by what the CPU can do, and is called as Android's linker calls it on
 * arm64: resolver(hwcap | _IFUNC_ARG_HWCAP, &arg), the flag saying the second argument is there. A resolver called
 * with no arguments reads whatever is in x0 and x1 as the capabilities, and may pick code this CPU cannot run. The
 * values are the ones getauxval gives the guest, so that what a resolver picks matches what the code it picks checks.
 */
typedef struct { unsigned long size, hwcap, hwcap2; } ifunc_arg;   /* bionic's __ifunc_arg_t */
#define IFUNC_ARG_HWCAP (1ULL << 62)

static uint64_t run_ifunc_resolver(const void *fn)
{
    ifunc_arg arg = { sizeof(arg), tl_bionic_auxval(16 /* AT_HWCAP */), tl_bionic_auxval(26 /* AT_HWCAP2 */) };
    uint64_t (*resolver)(uint64_t, ifunc_arg *) = (uint64_t (*)(uint64_t, ifunc_arg *))(uintptr_t)fn;
    return resolver(arg.hwcap | IFUNC_ARG_HWCAP, &arg);
}

/*
 * The module a TLS relocation's symbol lives in, and the symbol's offset in that module's TLS block, found as
 * bionic finds it: the relocating library itself for symbol 0 and for its own non-weak definitions, otherwise its
 * scope. `*mod` is NULL for an undefined weak symbol, which bionic evaluates at offset 0.
 */
static bool tls_target(tl_lib *L, uint32_t symidx, tl_lib **mod, uint64_t *value)
{
    *mod = NULL; *value = 0;
    if (symidx == 0) {
        if (!L->tls_id) { tl_log_line("ld: %s: a TLS relocation with no symbol, and no TLS segment", L->name); return false; }
        *mod = L;
        return true;
    }
    const elf_sym *s = sym_at(L, symidx), *d = NULL;
    const char *name = sym_name(L, s);
    tl_lib *in = NULL;
    if (s->st_shndx != SHN_UNDEF_ && (s->st_info >> 4) != STB_WEAK_) { d = s; in = L; }
    else {
        build_scope(L);
        if ((d = lib_find(L, name))) in = L;
        for (int i = 0; !d && i < L->ndeps; i++) if ((d = lib_find(L->deps[i], name))) in = L->deps[i];
    }
    if (!d) {
        if ((s->st_info >> 4) == STB_WEAK_) return true;
        tl_log_line("ld: %s: thread-local %s is not defined by any library it can see", L->name, name);
        return false;
    }
    if ((d->st_info & 0xf) != STT_TLS_ || !in->tls_id) {
        tl_log_line("ld: %s: TLS relocation against %s, which %s does not define as thread-local", L->name, name, in->name);
        return false;
    }
    *mod = in;
    *value = d->st_value;
    return true;
}

static bool reloc_one(tl_lib *L, uint64_t r_offset, uint32_t type, uint32_t symidx, int64_t addend)
{
    uint64_t off = r_offset - L->base_vaddr;
    if (off + (type == R_TLSDESC ? 16 : 8) > L->npages * PAGE) return false;
    uint64_t *place = (uint64_t *)(L->rw + off);
    bool failed = false;
    switch (type) {
    case R_NONE:
        return true;
    case R_RELATIVE:
        *place = image_addr(L, (uint64_t)addend);
        return true;
    case R_ABS64: case R_GLOB_DAT: case R_JUMP_SLOT: {
        if (symidx == 0) { *place = image_addr(L, (uint64_t)addend); return true; }
        uint64_t v = bind_symbol(L, symidx, &failed);
        if (failed) return false;
        if (v == 1 && L->symcache) v = 0;
        *place = v + (type == R_ABS64 ? (uint64_t)addend : 0);
        return true;
    }
    case R_IRELATIVE:
        /* The resolver is guest code: run it, store what it returns. */
        *place = run_ifunc_resolver(L->rx + ((uint64_t)addend - L->base_vaddr));
        return true;
    case R_TLS_TPREL: case R_TLS_DTPMOD: case R_TLS_DTPREL: case R_TLSDESC: {
        /* Every module's block is in static TLS (see "thread blocks"), so each of these is a constant now. */
        tl_lib *m; uint64_t v;
        if (!tls_target(L, symidx, &m, &v)) return false;
        uint64_t tpoff = (m ? m->tls_off : 0) + v + (uint64_t)addend;
        if (type == R_TLS_TPREL) *place = tpoff;
        else if (type == R_TLS_DTPMOD) *place = m ? m->tls_id : 0;
        else if (type == R_TLS_DTPREL) *place = v + (uint64_t)addend;      /* arm64's TLS_DTV_OFFSET is 0 */
        else {
            place[0] = (uint64_t)(uintptr_t)(m ? T.desc_static : T.desc_weak);
            place[1] = m ? tpoff : (uint64_t)addend;
        }
        return true;
    }
    default:
        tl_log_line("ld: %s: unsupported relocation type %u", L->name, type);
        return false;
    }
}

static bool do_relas(tl_lib *L, uint64_t addr, uint64_t size, size_t *count)
{
    if (!addr || !size) return true;
    const elf_rela *r = at(L, addr);
    for (size_t i = 0; i < size / sizeof(elf_rela); i++) {
        if (!reloc_one(L, r[i].r_offset, (uint32_t)(r[i].r_info & 0xffffffffu),
                       (uint32_t)(r[i].r_info >> 32), r[i].r_addend)) return false;
        (*count)++;
    }
    return true;
}

typedef struct { const uint8_t *p, *end; bool bad; } sleb;
static int64_t rd_sleb(sleb *s)
{
    uint64_t v = 0; unsigned shift = 0; uint8_t b;
    do {
        if (s->p >= s->end || shift >= 64) { s->bad = true; return 0; }
        b = *s->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= ~0ull << shift;
    return (int64_t)v;
}

static bool do_packed(tl_lib *L, size_t *count)
{
    /*
     * Android's APS2 encoding, read the way bionic's linker reads it. Offsets and
     * addends are running totals, not absolute values: each entry adds a signed
     * delta to the previous one. Within a group the order is: offset delta (if the
     * group shares one), info (if shared), shared-addend delta, then per entry its
     * own offset delta, info and addend delta as the flags leave them unshared.
     */
    if (!L->arela || !L->arelasz) return true;
    const uint8_t *d = at(L, L->arela);
    if (memcmp(d, "APS2", 4) != 0) { tl_log_line("ld: %s: packed relocations lack APS2 magic", L->name); return false; }
    sleb s = { d + 4, d + L->arelasz, false };
    int64_t total = rd_sleb(&s);
    uint64_t r_offset = (uint64_t)rd_sleb(&s), r_info = 0;
    int64_t r_addend = 0;
    if (s.bad || total < 0) return false;
    enum { BY_INFO = 1, BY_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };
    for (int64_t idx = 0; idx < total;) {
        int64_t group = rd_sleb(&s), flags = rd_sleb(&s);
        if (s.bad || group <= 0 || idx + group > total) return false;
        uint64_t group_delta = 0;
        if (flags & BY_DELTA) group_delta = (uint64_t)rd_sleb(&s);
        if (flags & BY_INFO) r_info = (uint64_t)rd_sleb(&s);
        int addend_mode = (int)(flags & (HAS_ADDEND | BY_ADDEND));
        if (addend_mode == (HAS_ADDEND | BY_ADDEND)) r_addend += rd_sleb(&s);
        else if (addend_mode != HAS_ADDEND) r_addend = 0;
        for (int64_t i = 0; i < group; i++) {
            r_offset += (flags & BY_DELTA) ? group_delta : (uint64_t)rd_sleb(&s);
            if (!(flags & BY_INFO)) r_info = (uint64_t)rd_sleb(&s);
            if (addend_mode == HAS_ADDEND) r_addend += rd_sleb(&s);
            if (s.bad) return false;
            if (!reloc_one(L, r_offset, (uint32_t)(r_info & 0xffffffffu), (uint32_t)(r_info >> 32), r_addend)) return false;
            (*count)++;
        }
        idx += group;
    }
    return true;
}

static bool do_relr(tl_lib *L, size_t *count)
{
    if (!L->relr || !L->relrsz) return true;
    const uint8_t *w = at(L, L->relr);
    uint64_t where = 0;
    for (size_t i = 0; i < L->relrsz / 8; i++) {
        uint64_t word = rd64(w + i * 8);
        if (!(word & 1)) {
            if (!reloc_one(L, word, R_RELATIVE, 0, (int64_t)rd64(L->rw + (word - L->base_vaddr)))) return false;
            (*count)++;
            where = word + 8;
        } else {
            for (unsigned b = 1; b < 64; b++) {
                if (word & (1ull << b)) {
                    uint64_t a = where + (uint64_t)(b - 1) * 8;
                    if (!reloc_one(L, a, R_RELATIVE, 0, (int64_t)rd64(L->rw + (a - L->base_vaddr)))) return false;
                    (*count)++;
                }
            }
            where += 63 * 8;
        }
    }
    return true;
}

/* ----------------------------------------------------------------- mapping */

static void parse_dynamic(tl_lib *L, uint64_t dyn_vaddr, uint64_t dyn_size)
{
    const elf_dyn *d = at(L, dyn_vaddr);
    for (size_t i = 0; i < dyn_size / sizeof(elf_dyn); i++) {
        switch (d[i].d_tag) {
        case DT_NULL_: return;
        case DT_NEEDED_: if (L->nneeded < MAX_DEPS) L->needed[L->nneeded++] = d[i].d_val; break;
        case DT_STRTAB_: L->strtab = d[i].d_val; break;
        case DT_STRSZ_: L->strsz = d[i].d_val; break;
        case DT_SYMTAB_: L->symtab = d[i].d_val; break;
        case DT_GNU_HASH_: L->gnu_hash = d[i].d_val; break;
        case DT_HASH_: L->sysv_hash = d[i].d_val; break;
        case DT_RELA_: L->rela = d[i].d_val; break;
        case DT_RELASZ_: L->relasz = d[i].d_val; break;
        case DT_JMPREL_: L->jmprel = d[i].d_val; break;
        case DT_PLTRELSZ_: L->pltrelsz = d[i].d_val; break;
        case DT_ANDROID_RELA_: L->arela = d[i].d_val; break;
        case DT_ANDROID_RELASZ_: L->arelasz = d[i].d_val; break;
        case DT_RELR_: case DT_ANDROID_RELR_: L->relr = d[i].d_val; break;
        case DT_RELRSZ_: case DT_ANDROID_RELRSZ_: L->relrsz = d[i].d_val; break;
        case DT_INIT_: L->init = d[i].d_val; break;
        case DT_INIT_ARRAY_: L->init_array = d[i].d_val; break;
        case DT_INIT_ARRAYSZ_: L->init_arraysz = d[i].d_val; break;
        case DT_SONAME_: break;     /* read once the string table is known */
        default: break;
        }
    }
}

/* An executable section, or an executable segment taken whole, as the file has it. */
typedef struct { uint64_t vaddr, size, foff; } code_sec;

/*
 * The bytes of stub the instruction `v` at `va` will be given by patch_image, judged from the file before anything is
 * patched, in patch_image's order: a read of the thread pointer, then anything naming x18, then the rest.
 */
static size_t site_stub_bytes(uint32_t v, uint64_t va, uint64_t base_vaddr, size_t npages, const uint8_t *flags)
{
    if ((v & 0xFFFFFFE0u) == 0xD53BD040u) return 32;                                    /* mrs Xt, TPIDR_EL0 */
    if (((v & 31u) == 18 || ((v >> 5) & 31u) == 18 || ((v >> 10) & 31u) == 18 || ((v >> 16) & 31u) == 18) && a64_uses_gpr(v, 18, NULL))
        return X18_STUB_BYTES;
    if (v == 0xD4000001u) return SVC_STUB_BYTES;                                        /* svc #0 */
    if ((v & 0xFFFFFFE0u) == 0xD53B0020u) return 32;                                    /* mrs Xt, CTR_EL0 */
    if ((v & 0x9F000000u) == 0x10000000u) {                                             /* adr: a stub if it reaches writable data */
        int64_t imm = (int64_t)((((v >> 5) & 0x7FFFFu) << 2) | ((v >> 29) & 3u));
        if (imm & 0x100000) imm -= 0x200000;
        int64_t tv = (int64_t)va + imm - (int64_t)base_vaddr;
        if (tv >= 0 && (size_t)tv < npages * PAGE && (flags[(size_t)tv / PAGE] & TL_PAGE_W)) return 32;
    }
    return 0;
}

/* The stub bytes needed by the sites at image offsets [lo, hi). */
static size_t stub_bytes_in(const uint8_t *file, const code_sec *code, int ncode, uint64_t base_vaddr, size_t npages,
                            const uint8_t *flags, uint64_t lo, uint64_t hi)
{
    size_t n = 0;
    for (int r = 0; r < ncode; r++) {
        const uint32_t *wv = (const uint32_t *)(file + code[r].foff);
        uint64_t start = code[r].vaddr - base_vaddr;     /* map_library keeps only sections inside the image */
        size_t cnt = (size_t)(code[r].size / 4), k = 0;
        if (start + (uint64_t)cnt * 4 <= lo || start >= hi) continue;
        if (start < lo) k = (size_t)((lo - start) / 4);
        if (hi != UINT64_MAX && start + (uint64_t)cnt * 4 > hi) cnt = (size_t)((hi - start + 3) / 4);
        for (; k < cnt; k++) {
            uint64_t off = start + k * 4;
            if (off >= lo && off < hi) n += site_stub_bytes(wv[k], code[r].vaddr + k * 4, base_vaddr, npages, flags);
        }
    }
    return n;
}

/* How far a site may be from the far end of its stub's pool and still be reached, and reached back: a branch's 128 MiB, less a page to spare. */
#define BRANCH_REACH (((uint64_t)128 << 20) - PAGE)

/* Memory the region handed out that a library that then failed will never use. Said, because nothing else would. */
static void xmem_lost(const char *name, size_t bytes)
{
    tl_log_line("ld: %s: %zu KiB of executable memory stays taken and unusable (the region is a bump allocator and cannot "
                "take it back); %zu MiB left", name, bytes >> 10, (tl_xmem_size() - tl_xmem_used()) >> 20);
}

static tl_lib *map_library(const char *name, uint8_t *file, size_t flen)
{
    if (G.nlibs >= MAX_LIBS) { tl_log_line("ld: too many libraries"); return NULL; }
    const elf_ehdr *eh = (const elf_ehdr *)file;
    if (flen < sizeof(*eh) || memcmp(file, "\x7f""ELF", 4) != 0 || eh->e_ident[4] != 2 || eh->e_ident[5] != 1
        || eh->e_machine != EM_AARCH64_) {
        tl_log_line("ld: %s is not a 64-bit little-endian arm64 ELF image", name);
        return NULL;
    }
    if (eh->e_phentsize < sizeof(elf_phdr) || eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > flen) {
        tl_log_line("ld: %s: program headers run off the file", name);
        return NULL;
    }
    /* Everything below is sized from the file: a library may have any number of segments and sections. */
    size_t nph = eh->e_phnum ? eh->e_phnum : 1;
    bool have_sh = eh->e_shoff && eh->e_shentsize >= 64 && eh->e_shnum && eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize <= flen;
    size_t ncode_max = (have_sh && eh->e_shnum > nph) ? eh->e_shnum : nph;
    elf_phdr *phs = malloc(nph * sizeof(elf_phdr));
    tl_segment *loads = malloc(nph * sizeof(tl_segment));
    code_sec *code = malloc(ncode_max * sizeof(code_sec));
    uint8_t *flags = NULL, *base_rx = NULL, *base_rw = NULL;
    size_t total = 0;
    tl_lib *L = NULL;
    if (!phs || !loads || !code) { tl_log_line("ld: %s: out of memory for its headers", name); goto fail; }

    int nloads = 0; tl_segment relro = {0}; bool has_relro = false;
    uint64_t dyn_v = 0, dyn_n = 0;
    const elf_phdr *tls = NULL;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        memcpy(&phs[i], file + eh->e_phoff + (size_t)i * eh->e_phentsize, sizeof(elf_phdr));
        const elf_phdr *p = &phs[i];
        if (p->p_type == PT_LOAD_) {
            loads[nloads].vaddr = p->p_vaddr; loads[nloads].memsz = p->p_memsz; loads[nloads].flags = p->p_flags;
            nloads++;
        } else if (p->p_type == PT_GNU_RELRO_) {
            relro.vaddr = p->p_vaddr; relro.memsz = p->p_memsz; has_relro = true;
        } else if (p->p_type == PT_DYNAMIC_) {
            dyn_v = p->p_vaddr; dyn_n = p->p_filesz;
        } else if (p->p_type == PT_TLS_) {
            tls = p;
        }
    }
    if (!nloads || !dyn_n) { tl_log_line("ld: %s has no loadable or dynamic segments", name); goto fail; }

    uint64_t base_vaddr = 0;
    size_t npages = tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, NULL, 0, &base_vaddr);
    if (!npages) { tl_log_line("ld: %s: unusable page layout", name); goto fail; }
    if (!(flags = malloc(npages))) goto fail;
    tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, flags, npages, &base_vaddr);
    size_t tls_off = 0;
    if (tls && (tls->p_vaddr < base_vaddr || tls->p_vaddr - base_vaddr + tls->p_filesz > npages * PAGE)) {
        tl_log_line("ld: %s: its TLS segment is outside the image", name);
        goto fail;
    }
    if (tls && !tls_reserve(name, tls, &tls_off)) goto fail;

    /* The executable sections, from the section headers when the file has them (it almost always does);
     * otherwise whole executable segments, which is correct for a library with nothing but code in them. */
    int ncode = 0;
    if (have_sh) {
        for (unsigned i = 0; i < eh->e_shnum; i++) {
            const uint8_t *sh = file + eh->e_shoff + (size_t)i * eh->e_shentsize;
            uint32_t type; uint64_t sflags, addr, off, size;
            memcpy(&type, sh + 4, 4); memcpy(&sflags, sh + 8, 8); memcpy(&addr, sh + 16, 8); memcpy(&off, sh + 24, 8); memcpy(&size, sh + 32, 8);
            /* patch_image writes over these through the image: one outside it is ignored, not trusted */
            if (type == 1 /* PROGBITS */ && (sflags & 4 /* EXECINSTR */) && size && off <= flen && size <= flen - off
                && addr >= base_vaddr && addr - base_vaddr <= npages * PAGE && size <= npages * PAGE - (addr - base_vaddr)) {
                code[ncode].vaddr = addr; code[ncode].size = size; code[ncode].foff = off; ncode++;
            }
        }
    }
    if (!ncode) {
        tl_log_line("ld: %s has no section headers; treating every executable segment as code", name);
        for (unsigned i = 0; i < eh->e_phnum; i++) {
            const elf_phdr *p = &phs[i];
            if (p->p_type == PT_LOAD_ && (p->p_flags & PF_X_) && p->p_offset <= flen && p->p_filesz <= flen - p->p_offset
                && p->p_vaddr >= base_vaddr && p->p_vaddr - base_vaddr + p->p_filesz <= npages * PAGE) {
                code[ncode].vaddr = p->p_vaddr; code[ncode].size = p->p_filesz; code[ncode].foff = p->p_offset; ncode++;
            }
        }
    }

    /*
     * Stub pages after the image: one literal slot, a few probes, a stub for every raw system call, read of the thread
     * pointer and the like, and one for every instruction that names the reserved register x18. A site more than a
     * branch from the far end of those pages cannot use them, so when the image is that large, pages *before* it hold
     * the stubs of the sites near its start: with both, every site of an image whose code spans up to two branch ranges
     * (256 MiB) reaches a stub. The pages after are sized for every site, so a site that reaches them never runs short.
     */
    size_t stub_bytes = 16 + 8192 + stub_bytes_in(file, code, ncode, base_vaddr, npages, flags, 0, UINT64_MAX);
    size_t nstub = (stub_bytes + PAGE - 1) / PAGE, npre = 0;
    uint64_t span = (uint64_t)(npages + nstub) * PAGE;
    uint64_t post_lo = span > BRANCH_REACH ? span - BRANCH_REACH : 0;      /* sites below this offset may not reach the pages after */
    if (post_lo) {
        size_t pre_bytes = stub_bytes_in(file, code, ncode, base_vaddr, npages, flags, 0, post_lo);
        if (pre_bytes) {
            npre = (pre_bytes + 8192 + PAGE - 1) / PAGE;
            uint64_t pre_hi = (uint64_t)npre * PAGE < BRANCH_REACH ? BRANCH_REACH - (uint64_t)npre * PAGE : 0;
            size_t gap = pre_hi < post_lo ? stub_bytes_in(file, code, ncode, base_vaddr, npages, flags, pre_hi, post_lo) : 0;
            if (gap) tl_log_line("ld: %s: its code spans more than two branch ranges; sites needing %zu bytes of stubs between "
                                 "+%#llx and +%#llx reach neither stub pool and depend on the relocation table's dead tail",
                                 name, gap, (unsigned long long)pre_hi, (unsigned long long)post_lo);
        }
    }
    if (!vx18_init()) tl_log_line("ld: no thread-specific slot for the virtual x18; instructions using x18 will not be rewritten");

    total = (npre + npages + nstub) * PAGE;
    if (!tl_xmem_alloc(total, &base_rx, &base_rw)) {
        tl_log_line("ld: %s needs %zu MiB of executable memory and the region has %zu MiB left", name,
                    total >> 20, (tl_xmem_size() - tl_xmem_used()) >> 20);
        total = 0;
        goto fail;
    }
    uint8_t *rx = base_rx + npre * PAGE, *rw = base_rw + npre * PAGE;
    /* The region's pages are not guaranteed zero (StikDebug writes a byte into each),
     * and .bss has to be. */
    memset(base_rw, 0, total);
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const elf_phdr *p = &phs[i];
        if (p->p_type != PT_LOAD_ || !p->p_filesz) continue;
        if (p->p_offset > flen || p->p_filesz > flen - p->p_offset || p->p_vaddr < base_vaddr
            || p->p_vaddr - base_vaddr + p->p_filesz > npages * PAGE) {
            tl_log_line("ld: %s: segment %u is outside the file or the image", name, i);
            goto fail;
        }
        memcpy(rw + (p->p_vaddr - base_vaddr), file + p->p_offset, p->p_filesz);
    }

    L = calloc(1, sizeof(*L));
    if (!L || !(L->code = malloc((size_t)(ncode ? ncode : 1) * sizeof(*L->code)))) { tl_log_line("ld: %s: out of memory", name); goto fail; }
    snprintf(L->name, sizeof(L->name), "%s", name);
    L->rx = rx; L->rw = rw; L->base_vaddr = base_vaddr; L->npages = npages; L->pflags = flags;
    L->phdr = phs; L->phnum = eh->e_phnum;
    for (int r = 0; r < ncode; r++) { L->code[r].start = code[r].vaddr; L->code[r].end = code[r].vaddr + code[r].size; }
    L->ncode = ncode;
    L->stub_rx = rx + npages * PAGE; L->stub_rw = rw + npages * PAGE; L->stub_used = 16; L->stub_cap = nstub * PAGE; L->nstub = nstub;
    { uint64_t h = (uint64_t)(uintptr_t)tl_svc_common; memcpy(L->stub_rw, &h, 8); }
    if (npre) { L->pre_rx = base_rx; L->pre_rw = base_rw; L->pre_cap = npre * PAGE; L->npre = npre; }
    L->xmem_bytes = total;
    parse_dynamic(L, dyn_v, dyn_n);
    if (L->strtab) {
        const elf_dyn *d = at(L, dyn_v);
        for (size_t i = 0; i < dyn_n / sizeof(elf_dyn) && d[i].d_tag != DT_NULL_; i++) {
            if (d[i].d_tag == DT_SONAME_) snprintf(L->soname, sizeof(L->soname), "%s", (const char *)at(L, L->strtab) + d[i].d_val);
        }
    }
    if (!L->soname[0]) snprintf(L->soname, sizeof(L->soname), "%s", name);
    L->nsyms = count_dynsyms(L);
    if (L->nsyms) L->symcache = calloc(L->nsyms, sizeof(uint64_t));
    if (tls) {
        L->tls_vaddr = tls->p_vaddr; L->tls_filesz = tls->p_filesz; L->tls_memsz = tls->p_memsz; L->tls_off = tls_off;
        tls_register(L);
    }
    free(loads); free(code);
    G.libs[G.nlibs++] = L;
    return L;

fail:
    if (total) xmem_lost(name, total);
    if (L) free(L->code);
    free(L); free(flags); free(phs); free(loads); free(code);
    return NULL;
}

/* ------------------------------------------------------------------ loading */

static bool relocate(tl_lib *L)
{
    size_t count = 0;
    L->state = 1;
    bool ok = do_relas(L, L->rela, L->relasz, &count)
           && do_relas(L, L->jmprel, L->pltrelsz, &count)
           && do_packed(L, &count)
           && do_relr(L, &count);
    free(L->symcache);
    L->symcache = NULL;
    if (!ok) { tl_log_line("ld: %s: relocation failed", L->name); return false; }
    /* The relocation table is dead now that the image is relocated. Its tail becomes a second pool of stubs, for the
     * sites that are out of branch range of the stub pages after a very large image. */
    if (L->relasz >= (1u << 20) && L->rela >= L->base_vaddr) {
        uint64_t end = (L->rela + L->relasz) & ~(uint64_t)(PAGE - 1), cap = 256u << 10;
        if (end - L->rela > cap + PAGE) {
            size_t off = (size_t)(end - cap - L->base_vaddr);
            L->isl_rx = L->rx + off; L->isl_rw = L->rw + off; L->isl_cap = cap; L->isl_used = 0;
            memset(L->isl_rw, 0, cap);
        }
    }
    size_t t = 0, a = 0, ad = 0, sv = 0;
    patch_image(L, &t, &a, &ad, &sv);
    tl_xmem_flush(L->rx - L->npre * PAGE, (L->npre + L->npages + L->nstub) * PAGE);
    L->state = 2;
    if (G.verbosity >= 1) {
        tl_log_line("ld: %-36s %5.1f MiB  %7zu relocs, %4zu tpidr + %5zu adrp patched%s%s", L->name,
                    (double)(L->npages * PAGE) / 1048576.0, count, t, a,
                    L->n_unresolved ? ", unresolved imports: " : "", "");
        if (L->n_unresolved) tl_log_line("ld:   %s: %u imports bound to logging stubs", L->name, L->n_unresolved);
        if (ad) tl_log_line("ld:   %s: %zu 'adr' instructions that reach writable data rewritten", L->name, ad);
        if (L->n_adr_failed) tl_log_line("ld:   %s: %zu 'adr' instructions reach writable data and could not be rewritten", L->name, L->n_adr_failed);
        if (L->n_ctr) tl_log_line("ld:   %s: %zu reads of CTR_EL0 replaced by a constant", L->name, L->n_ctr);
        if (L->tls_id) tl_log_line("ld:   %s: %llu bytes of thread-local storage at TP+%#zx (module %zu)", L->name,
                                   (unsigned long long)L->tls_memsz, L->tls_off, L->tls_id);
        if (L->n_tpidr_shared) tl_log_line("ld:   %s: %zu reads of TPIDR_EL0 have no stub in range and see the shared thread block", L->name, L->n_tpidr_shared);
        if (sv) tl_log_line("ld:   %s: %zu raw system-call sites rewritten", L->name, sv);
        if (L->n_x18 || L->n_x18_failed) tl_log_line("ld:   %s: %zu instructions using x18 rewritten for the virtual register%s", L->name, L->n_x18,
                                                     L->n_x18_failed ? " (and some that could not be)" : "");
    }
    /* Not a summary line: a system call that silently fails is a bug report waiting to happen, so it is said at any verbosity. */
    if (L->n_svc_far) tl_log_line("ld: %s: %zu raw system-call sites have no stub in branch range and answer ENOSYS", L->name, L->n_svc_far);
    return true;
}

static tl_lib *load_locked(const char *name, int depth)
{
    tl_lib *L = find_loaded(name);
    if (L) return L;
    if (tl_bionic_is_system_lib(name)) return NULL;
    if (depth > 32) { tl_log_line("ld: dependency chain too deep at %s", name); return NULL; }

    uint8_t *file; size_t flen;
    if (!fetch_from_apks(name, &file, &flen)) {
        tl_log_line("ld: %s is not in the APK and is not a system library", name);
        return NULL;
    }
    { char e[160] = ""; if (!tl_xmem_open(768u << 20, e, sizeof(e))) { tl_log_line("ld: %s", e); free(file); return NULL; } }
    {   /* adrp, which the loader uses to retarget code at the writable view, reaches +-4 GiB. */
        ptrdiff_t d = tl_xmem_delta();
        if (d > ((ptrdiff_t)3 << 30) || d < -((ptrdiff_t)3 << 30)) { tl_log_line("ld: the writable and executable views are %td MiB apart: too far for adrp", d >> 20); free(file); return NULL; }
    }
    if (!ensure_tcb()) { free(file); return NULL; }
    L = map_library(name, file, flen);
    free(file);
    if (!L) return NULL;

    /* Dependencies first, so everything this library binds against exists. */
    for (int i = 0; i < L->nneeded; i++) {
        const char *dn = (const char *)at(L, L->strtab) + L->needed[i];
        if (find_loaded(dn) || tl_bionic_is_system_lib(dn)) continue;
        if (!load_locked(dn, depth + 1)) tl_log_line("ld: %s: needed library %s could not be loaded", name, dn);
    }
    build_scope(L);
    if (!relocate(L)) {
        /* It stays registered, half relocated, so nothing else maps over it; its memory is gone either way. */
        xmem_lost(L->name, L->xmem_bytes);
        return NULL;
    }
    tls_publish(L);
    return L;
}

tl_lib *tl_ld_load(const char *name)
{
    pthread_mutex_lock(&g_big);
    tl_lib *L = load_locked(name, 0);
    pthread_mutex_unlock(&g_big);
    return L;
}

static bool init_locked(tl_lib *L)
{
    if (!L || L->state >= 3) return true;
    if (L->state < 2) return false;
    L->state = 3;
    build_scope(L);
    /* Needed libraries initialise first, in the order the linker would run them: the
     * deepest dependency first. The BFS list is shallowest-first, so walk it backwards. */
    for (int i = L->ndeps - 1; i >= 0; i--) init_locked(L->deps[i]);

    char *fallback_argv[] = { (char *)"app_process64", NULL };
    char *fallback_envp[] = { NULL };
    char **argv = G.argv ? G.argv : fallback_argv, **envp = G.envp ? G.envp : fallback_envp;
    int argc = 1;
    typedef void (*init_fn)(int, char **, char **);
    if (L->init) ((init_fn)(L->rx + (L->init - L->base_vaddr)))(argc, argv, envp);
    if (L->init_array) {
        const uint64_t *fns = at(L, L->init_array);
        size_t n = L->init_arraysz / 8;
        for (size_t i = 0; i < n; i++) {
            uint64_t f = fns[i];
            if (f && f != ~0ull) ((init_fn)(uintptr_t)f)(argc, argv, envp);
        }
    }
    L->state = 4;
    if (G.verbosity >= 1) tl_log_line("ld: %-36s initialised (%s%zu constructors)", L->name, L->init ? "DT_INIT + " : "", (size_t)(L->init_arraysz / 8));
    return true;
}

bool tl_ld_init(tl_lib *lib)
{
    pthread_mutex_lock(&g_big);
    bool ok = init_locked(lib);
    pthread_mutex_unlock(&g_big);
    return ok;
}
