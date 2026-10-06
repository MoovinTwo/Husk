/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest linker (src/translation-layer-next/husk-tl-ld.c), included whole with the platform it
 * expects stubbed out: the executable region is one plain mapping (both views the same), the bionic
 * shim and the APK reader answer nothing.
 *
 *   - dlsym of an IFUNC runs its resolver, as R_IRELATIVE does, once, and keeps the answer;
 *   - a library with more than sixteen loadable segments and executable sections is mapped whole;
 *   - a library that fails after taking executable memory says how much is lost;
 *   - (arm64 only, under qemu-user) a raw `svc #0` at either end of an image larger than a branch's
 *     range reaches a stub that calls the system-call handler and comes back, with the registers the
 *     kernel would keep kept -- where a site far from the stub pages used to answer ENOSYS.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/*
 * The linker finds the virtual x18's TSD slot by reading through TPIDRRO_EL0, which only Darwin points at a TSD
 * array. Failing the key makes it skip that, as it does on a phone that has no slot.
 */
static int no_keys(pthread_key_t *k, void (*d)(void *)) { (void)k; (void)d; return EAGAIN; }
#define pthread_key_create no_keys
#include "husk-tl-ld.c"
#undef pthread_key_create

static int failures;
#define CHECK(what, cond) do { bool c_ = (cond); printf("%s %s\n", c_ ? "ok  " : "FAIL", what); if (!c_) failures++; } while (0)

/* ------------------------------------------------------------- the platform */

static char g_last_log[512];
static int g_logs_lost;           /* lines that said executable memory was lost */
void tl_log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (strstr(g_last_log, "stays taken and unusable")) g_logs_lost++;
    printf("     | %s\n", g_last_log);
}

void *tl_bionic_find(const char *name) { (void)name; return NULL; }
bool tl_bionic_is_system_lib(const char *soname) { (void)soname; return false; }
unsigned long tl_bionic_auxval(unsigned long type) { return type == 16 ? 0x1234ul : type == 26 ? 0x5678ul : 0; }

bool tl_zip_open(tl_zip *z, const char *path, char *err, size_t errlen) { (void)z; (void)path; snprintf(err, errlen, "no"); return false; }
const tl_zip_entry *tl_zip_find(const tl_zip *z, const char *name) { (void)z; (void)name; return NULL; }
bool tl_zip_data(const tl_zip *z, const tl_zip_entry *e, size_t limit, const uint8_t **out, size_t *out_len, bool *owned,
                 char *err, size_t errlen) { (void)z; (void)e; (void)limit; (void)out; (void)out_len; (void)owned; (void)err; (void)errlen; return false; }

static uint8_t *g_region;
static size_t g_size, g_used;
bool tl_xmem_open(size_t host_bytes, char *err, size_t errlen)
{
    if (g_region) return true;
    int extra = 0;
#ifdef __APPLE__
    extra = MAP_JIT;    /* Apple Silicon refuses a writable+executable mapping without it */
#endif
    g_region = mmap(NULL, host_bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | extra, -1, 0);
    if (g_region == MAP_FAILED) { g_region = NULL; snprintf(err, errlen, "mmap: %s", strerror(errno)); return false; }
    g_size = host_bytes;
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0);   /* this thread writes the region; the tests run on it */
#endif
    return true;
}
bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw)
{
    size_t n = (bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    if (!g_region || n > g_size - g_used) return false;
    *rx = *rw = g_region + g_used;
    g_used += n;
    return true;
}
ptrdiff_t tl_xmem_delta(void) { return 0; }
bool tl_xmem_contains(const void *p) { return (const uint8_t *)p >= g_region && (const uint8_t *)p < g_region + g_size; }
bool tl_xmem_is_rx(const void *p) { return tl_xmem_contains(p); }
void tl_xmem_flush(const void *rx, size_t bytes) { __builtin___clear_cache((char *)rx, (char *)rx + bytes); }
size_t tl_xmem_used(void) { return g_used; }
size_t tl_xmem_size(void) { return g_size; }

/* The stubs' assembly calls Darwin's names for these. */
static long g_sys_calls, g_sys_nr, g_sys_a0;
long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    g_sys_calls++; g_sys_nr = nr; g_sys_a0 = a0;
#if defined(__aarch64__) && !defined(__APPLE__) /* Mach-O C symbols already carry the underscore */
    /* Clobber x9 as any C function may: the stub's caller relies on it surviving, as it would a real system call. */
    __asm__ volatile("mov x9, #0" ::: "x9");
#endif
    return nr * 1000 + a0;
}
#if defined(__aarch64__) && !defined(__APPLE__) /* Mach-O C symbols already carry the underscore */
__asm__(".globl _tl_linux_syscall\n_tl_linux_syscall: b tl_linux_syscall\n"
        ".globl _tl_unresolved_called_c\n_tl_unresolved_called_c: b tl_unresolved_called_c\n");
#endif

/* ------------------------------------------------------------- IFUNC */

static int g_resolved[20];
static uint64_t g_hwcap_seen, g_arg_hwcap2;
#define RESOLVER(i) static uint64_t resolver_##i(uint64_t hwcap, ifunc_arg *arg) \
    { g_resolved[i]++; g_hwcap_seen = hwcap; g_arg_hwcap2 = arg ? arg->hwcap2 : 0; return 0x10000u + i; }
RESOLVER(0) RESOLVER(1) RESOLVER(2) RESOLVER(3) RESOLVER(4) RESOLVER(5) RESOLVER(6) RESOLVER(7) RESOLVER(8) RESOLVER(9)
RESOLVER(10) RESOLVER(11) RESOLVER(12) RESOLVER(13) RESOLVER(14) RESOLVER(15) RESOLVER(16) RESOLVER(17) RESOLVER(18) RESOLVER(19)
static uint64_t (*const k_resolvers[20])(uint64_t, ifunc_arg *) = {
    resolver_0, resolver_1, resolver_2, resolver_3, resolver_4, resolver_5, resolver_6, resolver_7, resolver_8, resolver_9,
    resolver_10, resolver_11, resolver_12, resolver_13, resolver_14, resolver_15, resolver_16, resolver_17, resolver_18, resolver_19,
};

/*
 * A library made by hand: a symbol table, its strings and a SysV hash in one block (the writable view), and an
 * executable view placed at the lowest resolver so every IFUNC's value is a non-negative offset to its resolver.
 * Symbol 1 is a plain function, 2..21 IFUNCs ifunc0..ifunc19.
 */
static void test_ifunc(void)
{
    enum { NSYM = 22, SYMTAB = 64 };     /* not at vaddr 0: lib_find takes a zero table address for none */
    static uint8_t blob[4096];
    elf_sym *sym = (elf_sym *)(blob + SYMTAB);
    char *str = (char *)sym + NSYM * sizeof(elf_sym);
    size_t so = 1;
    uintptr_t lo = UINTPTR_MAX;
    for (int i = 0; i < 20; i++) if ((uintptr_t)k_resolvers[i] < lo) lo = (uintptr_t)k_resolvers[i];
    for (int i = 1; i < NSYM; i++) {
        char nm[24];
        if (i == 1) snprintf(nm, sizeof(nm), "plain");
        else snprintf(nm, sizeof(nm), "ifunc%d", i - 2);
        sym[i].st_name = (uint32_t)so;
        strcpy(str + so, nm); so += strlen(nm) + 1;
        sym[i].st_shndx = 1;
        sym[i].st_info = (uint8_t)((1 << 4) | (i == 1 ? 2 : STT_GNU_IFUNC_));
        sym[i].st_value = i == 1 ? 64 : (uint64_t)((uintptr_t)k_resolvers[i - 2] - lo);
    }
    uint32_t *hash = (uint32_t *)(str + ((so + 3) & ~(size_t)3));
    hash[0] = 1; hash[1] = NSYM; hash[2] = NSYM - 1;        /* one bucket: every symbol chained down from the last */
    for (uint32_t i = 0; i < NSYM; i++) hash[3 + i] = i ? i - 1 : 0;

    static tl_lib L;
    snprintf(L.name, sizeof(L.name), "libifunc.so");
    L.rw = blob; L.rx = (uint8_t *)lo; L.base_vaddr = 0;
    L.symtab = SYMTAB; L.strtab = (uint64_t)((uint8_t *)str - blob); L.sysv_hash = (uint64_t)((uint8_t *)hash - blob);
    L.nsyms = NSYM;
    L.state = 1;
    G.libs[G.nlibs++] = &L;

    CHECK("plain symbol resolves to its own address", tl_ld_sym(&L, "plain") == (void *)((uint8_t *)lo + 64));
    CHECK("IFUNC before relocation: refused, resolver not run", !tl_ld_sym(&L, "ifunc0") && g_resolved[0] == 0);
    L.state = 2;
    void *a = tl_ld_sym(&L, "ifunc0");
    CHECK("IFUNC: dlsym gives what the resolver picked", a == (void *)(uintptr_t)0x10000u);
    CHECK("IFUNC: resolver given hwcap | _IFUNC_ARG_HWCAP and the arg block",
          g_hwcap_seen == (0x1234u | IFUNC_ARG_HWCAP) && g_arg_hwcap2 == 0x5678u);
    void *b = tl_ld_sym(&L, "ifunc0"), *c = tl_ld_sym(NULL, "ifunc0");
    CHECK("IFUNC: answer kept, resolver run once", a == b && b == c && g_resolved[0] == 1);
    bool all = true;
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 20; i++) {
            char nm[24]; snprintf(nm, sizeof(nm), "ifunc%d", i);
            if (tl_ld_sym(&L, nm) != (void *)(uintptr_t)(0x10000u + (unsigned)i)) all = false;
        }
    }
    for (int i = 0; i < 20; i++) if (g_resolved[i] != 1) all = false;
    CHECK("IFUNC: twenty symbols, each resolved once and kept past the cache's first growth", all && L.nifuncs == 20);
    CHECK("missing symbol: NULL", !tl_ld_sym(&L, "nothing"));
    G.nlibs--;
    free(L.ifuncs);
}

/* ------------------------------------------------------------- mapping */

typedef struct { uint8_t *d; size_t n; } file;

/*
 * An ELF image of `nseg` one-page code segments (each starting with a nop and its index), a data segment holding
 * an empty dynamic table, and two executable sections per code segment. `bad_offset` puts the last code segment's
 * bytes past the end of the file, which is found only after memory is taken.
 */
static file make_many(int nseg, bool bad_offset)
{
    const size_t P = PAGE;
    int nload = nseg + 1, nsec = nseg * 2 + 1;
    size_t data_off = (size_t)nseg * P, shoff = data_off + P;
    file f = { calloc(1, shoff + (size_t)nsec * 64), shoff + (size_t)nsec * 64 };
    elf_ehdr *eh = (elf_ehdr *)f.d;
    memcpy(eh->e_ident, "\x7f" "ELF\x02\x01\x01", 7);
    eh->e_type = 3; eh->e_machine = EM_AARCH64_; eh->e_version = 1;
    eh->e_phoff = sizeof(elf_ehdr); eh->e_phentsize = sizeof(elf_phdr); eh->e_phnum = (uint16_t)(nload + 1);
    eh->e_shoff = shoff; eh->e_shentsize = 64; eh->e_shnum = (uint16_t)nsec;
    elf_phdr *ph = (elf_phdr *)(f.d + eh->e_phoff);
    for (int i = 0; i < nseg; i++) {
        ph[i] = (elf_phdr){ PT_LOAD_, PF_R_ | PF_X_, (uint64_t)i * P, (uint64_t)i * P, 0, P, P, P };
        uint32_t *w = (uint32_t *)(f.d + (size_t)i * P);
        if (i) { w[0] = 0xD503201Fu; w[1] = (uint32_t)i; }       /* page 0 holds the headers */
    }
    if (bad_offset) ph[nseg - 1].p_offset = f.n + P;
    ph[nseg] = (elf_phdr){ PT_LOAD_, PF_R_ | PF_W_, data_off, data_off, 0, P, P, P };
    ph[nseg + 1] = (elf_phdr){ PT_DYNAMIC_, PF_R_ | PF_W_, data_off, data_off, 0, 16, 16, 8 };
    for (int s = 1; s < nsec; s++) {
        uint8_t *sh = f.d + shoff + (size_t)s * 64;
        int seg = 1 + (s - 1) / 2;
        if (seg >= nseg) seg = nseg - 1;
        uint32_t type = 1; uint64_t flags = 6, addr = (uint64_t)seg * P + ((s - 1) % 2) * (P / 2), size = P / 2;
        memcpy(sh + 4, &type, 4); memcpy(sh + 8, &flags, 8); memcpy(sh + 16, &addr, 8); memcpy(sh + 24, &addr, 8); memcpy(sh + 32, &size, 8);
    }
    return f;
}

static void test_many_segments(void)
{
    char err[160];
    CHECK("region opens", tl_xmem_open((size_t)768 << 20, err, sizeof(err)));
    file f = make_many(20, false);
    tl_lib *L = map_library("libmany.so", f.d, f.n);
    CHECK("twenty loadable segments: mapped", L != NULL);
    if (L) {
        bool all = true;
        for (int i = 1; i < 20; i++) {
            const uint32_t *w = (const uint32_t *)(L->rw + (size_t)i * PAGE);
            if (w[1] != (uint32_t)i) all = false;
        }
        CHECK("every segment's bytes are in the image, the seventeenth on included", all);
        CHECK("every executable section is known (40 > 16)", L->ncode == 40);
        CHECK("stub pages follow the image, none before a small one", L->npre == 0 && L->stub_rx == L->rx + L->npages * PAGE);
    }
    free(f.d);

    size_t before = tl_xmem_used();
    int lost = g_logs_lost;
    f = make_many(20, true);
    L = map_library("libbroken.so", f.d, f.n);
    CHECK("a segment outside the file: refused after memory was taken", !L && tl_xmem_used() > before);
    CHECK("... and the lost executable memory is said", g_logs_lost == lost + 1);
    free(f.d);
}

/* ------------------------------------------------------------- far system calls */

#if defined(__aarch64__) && !defined(__APPLE__) /* Mach-O C symbols already carry the underscore */
/*
 * An image whose code is `code_mib` long, all `udf` but for system-call sites: each a function
 *     mov x8, #nr; mov x9, #77; mov x0, #a0; svc #0; add x0, x0, x9; ret
 * at the start, the middle and the end. Before the last, enough dead `svc` sites to take the stub pages after
 * the image past a megabyte, which is as far as an `ldr` literal reaches.
 */
static void test_far_svc(void)
{
    const size_t P = PAGE, code = (size_t)140 << 20;
    size_t data_off = code, n = data_off + P;
    uint8_t *d = calloc(1, n);
    elf_ehdr *eh = (elf_ehdr *)d;
    memcpy(eh->e_ident, "\x7f" "ELF\x02\x01\x01", 7);
    eh->e_type = 3; eh->e_machine = EM_AARCH64_; eh->e_version = 1;
    eh->e_phoff = sizeof(elf_ehdr); eh->e_phentsize = sizeof(elf_phdr); eh->e_phnum = 3;
    elf_phdr *ph = (elf_phdr *)(d + eh->e_phoff);
    ph[0] = (elf_phdr){ PT_LOAD_, PF_R_ | PF_X_, 0, 0, 0, code, code, P };
    ph[1] = (elf_phdr){ PT_LOAD_, PF_R_ | PF_W_, data_off, data_off, 0, P, P, P };
    ph[2] = (elf_phdr){ PT_DYNAMIC_, PF_R_ | PF_W_, data_off, data_off, 0, 16, 16, 8 };
    const size_t site_off[3] = { P, code / 2, code - 64 };
    const unsigned nr[3] = { 11, 22, 33 };
    for (int s = 0; s < 3; s++) {
        uint32_t *w = (uint32_t *)(d + site_off[s]);
        w[0] = 0xD2800008u | (nr[s] << 5);          /* mov x8, #nr */
        w[1] = 0xD2800009u | (77u << 5);            /* mov x9, #77 */
        w[2] = 0xD2800000u | ((unsigned)(s + 1) << 5);   /* mov x0, #s+1 */
        w[3] = 0xD4000001u;                         /* svc #0 */
        w[4] = 0x8B090000u;                         /* add x0, x0, x9 */
        w[5] = 0xD65F03C0u;                         /* ret */
    }
    uint32_t *dead = (uint32_t *)(d + code - 64 - 30000 * 4);
    for (int i = 0; i < 30000; i++) dead[i] = 0xD4000001u;

    tl_lib *L = map_library("libfar.so", d, n);
    free(d);
    CHECK("140 MiB image: mapped", L != NULL);
    if (!L) return;
    CHECK("... with stub pages before it", L->npre > 0 && L->pre_rx + L->npre * PAGE == L->rx);
    CHECK("... and the pages after it past an ldr literal's megabyte", L->stub_cap > ((size_t)1 << 20));
    L->state = 1;
    CHECK("relocated", relocate(L));
    CHECK("no site answered ENOSYS", L->n_svc_far == 0);
    CHECK("the first site's stub is before the image", L->pre_used > 0);
    for (int s = 0; s < 3; s++) {
        long (*fn)(void) = (long (*)(void))(void *)(L->rx + site_off[s]);
        g_sys_calls = 0;
        long r = fn();
        char what[120];
        snprintf(what, sizeof(what), "site at +%zu MiB: handler called with nr %u, x0 %d; x9 kept across it", site_off[s] >> 20, nr[s], s + 1);
        CHECK(what, g_sys_calls == 1 && g_sys_nr == (long)nr[s] && g_sys_a0 == s + 1 && r == (long)nr[s] * 1000 + s + 1 + 77);
    }
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    G.verbosity = 1;
    test_ifunc();
    test_many_segments();
#if defined(__aarch64__) && !defined(__APPLE__) /* Mach-O C symbols already carry the underscore */
    test_far_svc();
#else
    printf("skip far system calls: not arm64\n");
#endif
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
