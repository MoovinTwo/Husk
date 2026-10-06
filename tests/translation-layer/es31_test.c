/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The GLES 3.1-on-3.0 shim's binding tables (src/translation-layer-next/husk-tl-egl-es31.inc), included whole with
 * GL stubbed: what each shader drops is applied at link to exactly the shaders the program has attached, keyed by
 * their real names (two names that agree modulo 1024 used to share a slot), with no limit on how many, and an
 * entry goes when GL's object does.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tl_log_line(const char *fmt, ...);
void tl_log_line(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("     | "); vprintf(fmt, ap); printf("\n"); va_end(ap); }

static void *(*a_eglGetProcAddress)(const char *);
static void (*a_glGetIntegerv)(unsigned, int *);

#include "husk-tl-egl-es31.inc"

static int failures;
#define CHECK(what, cond) do { bool c_ = (cond); printf("%s %s\n", c_ ? "ok  " : "FAIL", what); if (!c_) failures++; } while (0)

/* ------------------------------------------------------------- GL, recorded */

static char *g_src;                         /* the last source the driver was given */
static void gl_shader_source(unsigned sh, int count, const char *const *str, const int *len)
{
    (void)sh;
    free(g_src);
    size_t n = 0;
    for (int i = 0; i < count; i++) n += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    g_src = malloc(n + 1); n = 0;
    for (int i = 0; i < count; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(g_src + n, str[i], l); n += l; }
    g_src[n] = 0;
}
static void gl_nop2(unsigned a, unsigned b) { (void)a; (void)b; }
static void gl_nop1(unsigned a) { (void)a; }

/* Uniform "texN" is at location 100 + N, "texB" at 9000; blocks "Blk" and "Data" are 0 and 1. */
static int gl_get_uniform_location(unsigned prog, const char *name)
{
    (void)prog;
    if (!strcmp(name, "texB")) return 9000;
    if (!strncmp(name, "tex", 3)) return 100 + atoi(name + 3);
    return -1;
}
static unsigned gl_get_uniform_block_index(unsigned prog, const char *name)
{
    (void)prog;
    return !strcmp(name, "Blk") ? 0 : !strcmp(name, "Data") ? 1 : 0xFFFFFFFFu;
}
static int g_uni[10000];                    /* by location: binding + 1, 0 = not set */
static int g_nuni;
static void gl_uniform1i(int loc, int v) { if (loc >= 0 && loc < 10000) { g_uni[loc] = v + 1; g_nuni++; } }
static unsigned g_blk[2]; static int g_nblk;
static void gl_uniform_block_binding(unsigned prog, unsigned idx, unsigned b) { (void)prog; if (idx < 2) g_blk[idx] = b + 1; g_nblk++; }
static unsigned g_current = 42;
static void gl_use_program(unsigned p) { g_current = p; }
static void gl_get_programiv(unsigned p, unsigned pname, int *v) { (void)p; *v = pname == 0x8B82; }
static void gl_get_integerv(unsigned pname, int *v) { *v = pname == 0x8B8D ? (int)g_current : 0; }

static void *proc(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        { "glGetUniformLocation", (void *)gl_get_uniform_location }, { "glGetUniformBlockIndex", (void *)gl_get_uniform_block_index },
        { "glUniformBlockBinding", (void *)gl_uniform_block_binding }, { "glUniform1i", (void *)gl_uniform1i },
        { "glUseProgram", (void *)gl_use_program }, { "glGetProgramiv", (void *)gl_get_programiv },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(t[i].n, name)) return t[i].f;
    return NULL;
}

static void reset_calls(void) { memset(g_uni, 0, sizeof(g_uni)); g_nuni = 0; memset(g_blk, 0, sizeof(g_blk)); g_nblk = 0; }
static void source(unsigned sh, const char *text) { const char *one = text; w_glShaderSource(sh, 1, &one, NULL); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    a_eglGetProcAddress = proc; a_glGetIntegerv = gl_get_integerv;
    r_glShaderSource = gl_shader_source; r_glAttachShader = gl_nop2; r_glDetachShader = gl_nop2;
    r_glDeleteShader = gl_nop1; r_glDeleteProgram = gl_nop1; r_glLinkProgram = gl_nop1;

    /* Shim off: passed through, nothing kept. */
    source(7, "#version 310 es\nlayout(binding = 1) uniform sampler2D tex1;\n");
    CHECK("shim off: source passed through as it came", g_src && strstr(g_src, "#version 310 es") && strstr(g_src, "binding"));
    w_glAttachShader(1, 7);
    CHECK("shim off: nothing recorded", g_es31_nsh == 0 && g_es31_npr == 0);

    tl_egl_es31_shim(true);

    /* One shader with twenty sampler bindings (more than the twelve there used to be room for), a uniform block and
     * a read-only storage block. */
    char big[4096]; size_t o = 0;
    o += (size_t)snprintf(big + o, sizeof(big) - o, "#version 310 es\nprecision mediump float;\n");
    for (int i = 0; i < 20; i++) o += (size_t)snprintf(big + o, sizeof(big) - o, "layout(binding = %d) uniform sampler2D tex%d;\n", i + 1, i);
    o += (size_t)snprintf(big + o, sizeof(big) - o, "layout(std140, binding = 3) uniform Blk { vec4 a; };\n");
    o += (size_t)snprintf(big + o, sizeof(big) - o, "layout(std430, binding = 7) readonly buffer Data { float v[]; };\nvoid main() {}\n");
    source(5, big);
    CHECK("310 es written down to 300 es, bindings dropped",
          g_src && strstr(g_src, "#version 300 es") && !strstr(g_src, "binding") && strstr(g_src, "layout(std140) uniform Blk"));
    CHECK("the storage block became a uniform block of 1024", g_src && strstr(g_src, "layout(std140) uniform Data") && strstr(g_src, "v[1024]"));
    CHECK("all twenty-two bindings kept", es31_shader_get(5, false) && es31_shader_get(5, false)->n == 22);

    /* 1029 is 5 modulo 1024: the old table kept one of the two. */
    source(1029, "#version 310 es\nlayout(binding = 9) uniform sampler2D texB;\nvoid main() {}\n");
    w_glAttachShader(1, 5); w_glAttachShader(1, 1029); w_glAttachShader(1, 1029);
    reset_calls();
    w_glLinkProgram(1);
    bool all = true;
    for (int i = 0; i < 20; i++) if (g_uni[100 + i] != i + 2) all = false;
    CHECK("link: every sampler of shader 5 bound", all);
    CHECK("link: shader 1029's sampler bound too, though 1029 = 5 mod 1024", g_uni[9000] == 10);
    CHECK("link: uniform block and storage block bound", g_blk[0] == 4 && g_blk[1] == 8);
    CHECK("link: each binding applied once (attaching twice is one attachment)", g_nuni == 21 && g_nblk == 2);
    CHECK("link: the current program put back", g_current == 42);

    /* Deleted while attached: GL keeps it until detached, so the bindings still apply on relink. */
    w_glDeleteShader(5);
    reset_calls(); w_glLinkProgram(1);
    CHECK("shader deleted while attached: still applied on relink", g_uni[100] == 2 && g_uni[9000] == 10);
    w_glDetachShader(1, 5);
    CHECK("... and gone once detached", !es31_shader_get(5, false));
    reset_calls(); w_glLinkProgram(1);
    CHECK("relink after a detach: only what is attached now", g_uni[100] == 0 && g_uni[9000] == 10 && g_nuni == 1 && g_nblk == 0);

    /* The name comes back as a new shader with other bindings: none of the old ones leak in. */
    source(5, "#version 310 es\nlayout(binding = 4) uniform sampler2D tex0;\nvoid main() {}\n");
    w_glAttachShader(1, 5);
    reset_calls(); w_glLinkProgram(1);
    CHECK("a reused shader name carries only its new bindings", g_uni[100] == 5 && g_uni[101] == 0 && g_nuni == 2);

    /* Deleting the program detaches its shaders; a shader not deleted stays, one deleted goes. */
    w_glDeleteShader(1029);
    CHECK("1029 deleted while attached: kept", es31_shader_get(1029, false) != NULL);
    w_glDeleteProgram(1);
    CHECK("program deleted: its entry gone, its deleted shader with it, the live one kept",
          !es31_prog_get(1, false) && !es31_shader_get(1029, false) && es31_shader_get(5, false));
    w_glDeleteShader(5);
    CHECK("an unattached shader deleted: gone at once", g_es31_nsh == 0 && g_es31_npr == 0);

    /* Thousands of each, in a scattered order: the tables grow and every lookup is exact. */
    enum { N = 3000 };
    for (unsigned k = 0; k < N; k++) {
        unsigned i = (k * 7919u) % N;
        char s[160];
        snprintf(s, sizeof(s), "#version 310 es\nlayout(binding = %u) uniform sampler2D tex%u;\nvoid main() {}\n", i % 50, i % 900);
        source(10000 + i, s);
        w_glAttachShader(20000 + i, 10000 + i);
    }
    CHECK("3000 shaders and programs kept", g_es31_nsh == N && g_es31_npr == N);
    all = true;
    for (unsigned i = 0; i < N; i += 37) {
        reset_calls(); w_glLinkProgram(20000 + i);
        if (g_nuni != 1 || g_uni[100 + i % 900] != (int)(i % 50) + 1) all = false;
    }
    CHECK("each program gets its own shader's binding", all);
    for (unsigned i = 0; i < N; i++) { w_glDeleteProgram(20000 + i); w_glDeleteShader(10000 + i); }
    CHECK("all deleted: tables empty", g_es31_nsh == 0 && g_es31_npr == 0);

    free(g_src); free(g_es31_sh); free(g_es31_pr);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
