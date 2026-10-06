/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Where guest code may reach the filesystem.
 *
 * An Android game runs as native code inside Husk's own process, so a path it hands to open() or stat() would
 * otherwise reach anything Husk's sandbox can: the guest images and other apps' data in Documents, the debugger
 * pairing, Husk's preferences. Every path-taking wrapper asks tl_path_confine first. The path is made absolute
 * against the guest's working directory, collapsed lexically (".", "..", "//", never above "/"), Android's own
 * locations are moved into this app's data directory, and what is left must lie under one of the roots:
 *
 *   read-write  the app's data directory
 *   read-only   the APKs the loader opened, and the app bundle
 *   exactly     /dev/null and /dev/zero (read-write); /dev/random and /dev/urandom (read-only)
 *
 * The /proc and /sys files that are made up (husk-tl-bionic-io.c) are answered by their wrappers before a path gets
 * here. A write under a read-only root fails with EACCES; anything outside the roots fails with ENOENT, as though it
 * were not there. Looking only at metadata (stat, access, realpath, chdir) is also allowed for the directories above
 * a root, so that code which walks down from "/" making each directory finds them.
 *
 * Roots are compared a whole component at a time, so a sibling "data-x" is not inside "data". A path inside a root can
 * still leave it through a symbolic link, so the deepest part of it that exists is resolved with realpath and must
 * land inside a root as well; a dangling link is refused rather than followed somewhere unknown. That is a check
 * followed by a use, and a guest that swaps a directory for a link between the two wins the race.
 *
 * This is defence in depth, not a sandbox: native code in this process can read Husk's memory and make system calls
 * of its own. It keeps an ordinary game, or a careless one, inside its own directory.
 *
 * TL_PATH_UNCONFINED=1 turns it off -- Android's locations are still moved, nothing is refused -- for host
 * harnesses that need the guest to reach arbitrary files.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linux errno values, as the guest reads them. */
#define G_ENOENT       2
#define G_EBADF        9
#define G_ENOMEM       12
#define G_EACCES       13
#define G_EFAULT       14
#define G_ENOTDIR      20
#define G_EINVAL       22
#define G_ERANGE       34
#define G_ENAMETOOLONG 36

#define G_AT_FDCWD (-100)

/* ------------------------------------------------------------ the data directory */

/* Wherever the host set aside for this app. Changing it changes the roots, so they are rebuilt on the next check. */
static char g_data_dir[PATH_MAX];
static atomic_uint g_data_gen;
void tl_set_data_dir(const char *dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir ? dir : "");
    atomic_fetch_add(&g_data_gen, 1);
}
const char *tl_data_dir(void) { return g_data_dir[0] ? g_data_dir : "/tmp"; }

static bool unconfined(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("TL_PATH_UNCONFINED"); on = e && !strcmp(e, "1"); }
    return on;
}

/* ------------------------------------------------------------ path arithmetic */

/*
 * `in`, absolute, with ".", ".." and repeated slashes collapsed and no trailing slash, into `out`. ".." at the top
 * stays there, as the kernel's does. It is lexical: "a/link/.." is "a" whatever the link points at, and that is the
 * path the system call then gets, so the two agree. False when the result does not fit.
 */
static bool normalize(const char *in, char *out, size_t n)
{
    if (n < 2 || in[0] != '/') return false;
    size_t len = 0;
    out[len++] = '/';
    for (const char *p = in; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - s);
        if (cl == 1 && s[0] == '.') continue;
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 1 && out[len - 1] != '/') len--;     /* "/a/b" -> "/a/" */
            if (len > 1) len--;                                /* -> "/a"; "/" stays "/" */
            continue;
        }
        if (len + (len > 1) + cl + 1 > n) return false;
        if (len > 1) out[len++] = '/';
        memcpy(out + len, s, cl);
        len += cl;
    }
    out[len] = 0;
    return true;
}

/* What follows `pre` in `p` when `p` is `pre` or lies under it, a whole component at a time; NULL otherwise. */
static const char *under(const char *p, const char *pre)
{
    size_t l = strlen(pre);
    if (l == 0 || strncmp(p, pre, l)) return NULL;
    if (pre[l - 1] == '/') return p + l;                       /* "/" itself: everything is under it */
    return (p[l] == 0 || p[l] == '/') ? p + l : NULL;
}

/*
 * Android's locations for this app's own files, moved into the data directory: /data/data/<pkg> and /data/user/0/<pkg>
 * are the directory itself, and /sdcard (or /storage/emulated/0) is a directory inside it. 1 when `p` was one of them
 * and `out` holds where it went, 0 when it was not, -1 when the result does not fit.
 */
static int android_move(const char *p, char *out, size_t n)
{
    const char *rest = NULL, *sub = "";
    const char *r;
    if ((r = under(p, "/data/data")) && r[0] == '/') rest = r + 1;
    else if ((r = under(p, "/data/user/0")) && r[0] == '/') rest = r + 1;
    if (rest) {
        rest = strchr(rest, '/');                              /* past the package name */
        if (!rest) rest = "";
    } else if ((r = under(p, "/sdcard")) || (r = under(p, "/storage/emulated/0"))) {
        rest = r; sub = "/sdcard";
    } else {
        return 0;
    }
    int w = snprintf(out, n, "%s%s%s", tl_data_dir(), sub, rest);
    return (w < 0 || (size_t)w >= n) ? -1 : 1;
}

/* ------------------------------------------------------------ the guest's working directory */

/*
 * The guest's own, kept here rather than in the process: a chdir from a game must not move the host's, and relative
 * paths have to be made absolute before they can be checked. It starts as the process's, which is "/" for an iOS app
 * and wherever a harness was started on a Mac.
 */
static char g_cwd[PATH_MAX];
static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;

static void guest_cwd(char *out, size_t n)
{
    pthread_mutex_lock(&g_cwd_lock);
    if (!g_cwd[0]) {
        char real[PATH_MAX];
        if (!getcwd(real, sizeof(real)) || !normalize(real, g_cwd, sizeof(g_cwd))) snprintf(g_cwd, sizeof(g_cwd), "/");
    }
    snprintf(out, n, "%s", g_cwd);
    pthread_mutex_unlock(&g_cwd_lock);
}

/* ------------------------------------------------------------ the roots */

typedef struct {
    char *lex;           /* as the host named it, normalised */
    char *canon;         /* through realpath; the same as lex while it does not exist */
    bool rw;
    bool exact;          /* a single file (the devices): nothing under it */
    bool resolved;
} root;

typedef struct {
    unsigned data_gen;
    int napks;
    int n;
    root r[12];
} root_table;

static root_table *g_roots;
static pthread_rwlock_t g_roots_lock = PTHREAD_RWLOCK_INITIALIZER;

static int apk_count(void)
{
    int n = 0;
    while (tl_ld_apk_path(n)) n++;
    return n;
}

/* The .app directory this code was loaded from, or "" when it was not loaded from one (a host harness). */
static void bundle_dir(char *out, size_t n)
{
    Dl_info info;
    out[0] = 0;
    if (!dladdr((void *)&tl_path_confine, &info) || !info.dli_fname) return;
    const char *f = info.dli_fname, *s = strstr(f, ".app/");
    if (s) snprintf(out, n, "%.*s", (int)(s + 4 - f), f);
}

static void add_root(root_table *t, const char *path, bool rw, bool exact)
{
    if (!path || !path[0] || t->n >= (int)(sizeof(t->r) / sizeof(t->r[0]))) return;
    char abs[2 * PATH_MAX], lex[PATH_MAX], canon[PATH_MAX];
    if (path[0] == '/') snprintf(abs, sizeof(abs), "%s", path);
    else {                                                     /* a harness given a relative APK: the host's own cwd */
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return;
        snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
    }
    if (!normalize(abs, lex, sizeof(lex)) || !strcmp(lex, "/")) return;   /* "/" would let everything in */
    root *r = &t->r[t->n++];
    r->resolved = exact || realpath(lex, canon);
    r->lex = strdup(lex);
    r->canon = strdup(r->resolved && !exact ? canon : lex);
    r->rw = rw; r->exact = exact;
    if (!r->lex || !r->canon) { free(r->lex); free(r->canon); t->n--; }
}

static void free_roots(root_table *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) { free(t->r[i].lex); free(t->r[i].canon); }
    free(t);
}

/* Whether the table no longer matches the launch: a new data directory, another APK, or a root that has since appeared. */
static bool stale(const root_table *t)
{
    if (!t || t->data_gen != atomic_load(&g_data_gen) || t->napks != apk_count()) return true;
    for (int i = 0; i < t->n; i++) if (!t->r[i].resolved) return true;
    return false;
}

/* Takes the read lock on the roots, rebuilding them first if they are stale. */
static const root_table *roots_acquire(void)
{
    pthread_rwlock_rdlock(&g_roots_lock);
    if (!stale(g_roots)) return g_roots;
    pthread_rwlock_unlock(&g_roots_lock);
    pthread_rwlock_wrlock(&g_roots_lock);
    if (stale(g_roots)) {
        root_table *t = calloc(1, sizeof(*t));
        if (t) {
            t->data_gen = atomic_load(&g_data_gen);
            t->napks = apk_count();
            add_root(t, tl_data_dir(), true, false);
            for (int i = 0; i < t->napks; i++) add_root(t, tl_ld_apk_path(i), false, false);
            char bundle[PATH_MAX];
            bundle_dir(bundle, sizeof(bundle));
            add_root(t, bundle, false, false);
            add_root(t, "/dev/null", true, true);
            add_root(t, "/dev/zero", true, true);
            add_root(t, "/dev/random", false, true);
            add_root(t, "/dev/urandom", false, true);
            free_roots(g_roots);
            g_roots = t;
        }
    }
    pthread_rwlock_unlock(&g_roots_lock);
    pthread_rwlock_rdlock(&g_roots_lock);
    return g_roots;
}

/* The root `p` lies under, by either of its names, or NULL. Writable roots win, so a read-only one inside never narrows them. */
static const root *root_of(const root_table *t, const char *p)
{
    const root *best = NULL;
    for (int i = 0; t && i < t->n; i++) {
        const root *r = &t->r[i];
        bool in = r->exact ? (!strcmp(p, r->lex) || !strcmp(p, r->canon)) : (under(p, r->lex) || under(p, r->canon));
        if (in && (!best || (r->rw && !best->rw))) best = r;
    }
    return best;
}

/* Whether `p` is a directory above some root (by either of its names), whose metadata may be looked at. */
static bool above_root(const root_table *t, const char *p)
{
    for (int i = 0; t && i < t->n; i++) if (under(t->r[i].lex, p) || under(t->r[i].canon, p)) return true;
    return false;
}

/* 0 when `p` (absolute, normalised) may be used the way `how` says, else the guest errno to fail with. */
static int check(const root_table *t, const char *p, int how)
{
    int want = how & 3;
    const root *r = root_of(t, p);
    if (!r) return (want == TL_PATH_META && above_root(t, p)) ? 0 : G_ENOENT;
    if (want == TL_PATH_WRITE && !r->rw) return G_EACCES;
    if (r->exact) return 0;

    /*
     * Where the path really leads. The last component is left out when the call does not follow it (lstat, unlink,
     * rename): a link there is the thing being acted on, not a way out. Below the deepest part that exists nothing
     * can be a link, since nothing is there at all.
     */
    char probe[PATH_MAX], real[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s", p);
    if ((how & TL_PATH_NOFOLLOW) && strcmp(p, r->lex) && strcmp(p, r->canon)) {
        char *s = strrchr(probe, '/');
        if (s == probe) s[1] = 0; else *s = 0;
    }
    struct stat st;
    while (lstat(probe, &st) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) return G_ENOENT;
        char *s = strrchr(probe, '/');
        if (s == probe) { if (!probe[1]) return G_ENOENT; s[1] = 0; }
        else *s = 0;
    }
    if (!realpath(probe, real)) return G_ENOENT;              /* a dangling link or a loop */
    const root *cr = root_of(t, real);
    if (!cr) return (want == TL_PATH_META && above_root(t, real)) ? 0 : G_ENOENT;
    if (want == TL_PATH_WRITE && !cr->rw) return G_EACCES;
    return 0;
}

/* Said once per refused area (its first few components), so a game probing for files does not flood the log. */
static void note_refused(const char *p, int e)
{
    size_t i = 1;
    for (int comps = 0; p[i]; i++) if (p[i] == '/' && ++comps == 3) break;
    char note[200];
    snprintf(note, sizeof(note), "path: refused %s %.*s%s", e == G_EACCES ? "a write to" : "access to",
             (int)(i < 120 ? i : 120), p, p[i] ? "/..." : "");
    tl_note_once(note);
}

static const char *fail(int e) { tl_set_guest_errno(e); return NULL; }

const char *tl_path_confine(const char *path, int how, char *buf, size_t n)
{
    if (!path) return fail(G_EFAULT);
    if (!path[0]) return fail(G_ENOENT);
    char abs[2 * PATH_MAX], norm[PATH_MAX], moved[PATH_MAX];

    if (unconfined()) {
        /* The old behaviour: the kernel sees the guest's path, with only Android's locations moved. */
        int m = path[0] == '/' ? android_move(path, moved, sizeof(moved)) : 0;
        if (m < 0) return fail(G_ENAMETOOLONG);
        const char *p = m ? moved : path;
        if (strlen(p) >= n) return fail(G_ENAMETOOLONG);
        memcpy(buf, p, strlen(p) + 1);
        return buf;
    }

    char cwd[PATH_MAX] = "";
    if (path[0] != '/') guest_cwd(cwd, sizeof(cwd));
    int w = snprintf(abs, sizeof(abs), "%s/%s", cwd, path);    /* "/" + an absolute path is the same path */
    if (w < 0 || (size_t)w >= sizeof(abs) || !normalize(abs, norm, sizeof(norm))) return fail(G_ENAMETOOLONG);
    int m = android_move(norm, abs, sizeof(abs));
    if (m < 0 || (m && !normalize(abs, moved, sizeof(moved)))) return fail(G_ENAMETOOLONG);
    const char *p = m ? moved : norm;
    if (strlen(p) >= n) return fail(G_ENAMETOOLONG);

    const root_table *t = roots_acquire();
    int e = check(t, p, how);
    pthread_rwlock_unlock(&g_roots_lock);
    if (e) { note_refused(p, e); return fail(e); }
    memcpy(buf, p, strlen(p) + 1);
    return buf;
}

const char *tl_path_at(int dirfd, const char *path, char *buf, size_t n)
{
    if (!path) return fail(G_EFAULT);
    if (dirfd == G_AT_FDCWD || path[0] == '/') return path;
    if (!path[0]) return fail(G_ENOENT);
#ifdef F_GETPATH
    /* The directory the descriptor names, by path, so the result is checked like any other. */
    char dir[PATH_MAX];
    if (fcntl(dirfd, F_GETPATH, dir) < 0) return fail(G_EBADF);
    int w = snprintf(buf, n, "%s/%s", dir, path);
    if (w < 0 || (size_t)w >= n) return fail(G_ENAMETOOLONG);
    return buf;
#else
    /* No way to learn where the descriptor points, so no way to check what is under it. */
    (void)buf; (void)n;
    tl_note_once("path: a directory descriptor's path cannot be found here; refused");
    return fail(G_EBADF);
#endif
}

int tl_path_chdir(const char *path)
{
    char p[PATH_MAX];
    if (!tl_path_confine(path, TL_PATH_META, p, sizeof(p))) return -1;
    if (unconfined()) {
        errno = 0;
        int r = chdir(p);
        if (r) tl_set_guest_errno(tl_errno_to_guest(errno));
        return r;
    }
    struct stat st;
    if (stat(p, &st) != 0) { tl_set_guest_errno(tl_errno_to_guest(errno)); return -1; }
    if (!S_ISDIR(st.st_mode)) { tl_set_guest_errno(G_ENOTDIR); return -1; }
    pthread_mutex_lock(&g_cwd_lock);
    snprintf(g_cwd, sizeof(g_cwd), "%s", p);
    pthread_mutex_unlock(&g_cwd_lock);
    return 0;
}

char *tl_path_getcwd(char *buf, size_t n)
{
    if (unconfined()) {
        errno = 0;
        char *r = getcwd(buf, n);
        if (!r) tl_set_guest_errno(tl_errno_to_guest(errno));
        return r;
    }
    char cwd[PATH_MAX];
    guest_cwd(cwd, sizeof(cwd));
    size_t len = strlen(cwd);
    if (!buf) {                                                /* bionic allocates, at least `n` bytes when n is given */
        buf = malloc(n > len ? n : len + 1);
        if (!buf) { tl_set_guest_errno(G_ENOMEM); return NULL; }
    } else if (n == 0) {
        tl_set_guest_errno(G_EINVAL); return NULL;
    } else if (n <= len) {
        tl_set_guest_errno(G_ERANGE); return NULL;
    }
    memcpy(buf, cwd, len + 1);
    return buf;
}
