/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unity's web requests, whose Java side is com.unity3d.player.UnityWebRequest: a Runnable over HttpURLConnection that the engine
 * builds (with a java.util.HashMap of request headers), sets up, runs on a thread of its own, and which reports back through
 * native callbacks -- the status, each header, the length, the body in pieces, or an error. Here the request itself is the host's
 * (husk-tl-http.m), and the Java object is this: it keeps what the engine gave it and, on run(), does what the Java would have done,
 * in the same order, so the engine sees the same calls.
 *
 * Also the java.util.HashMap that carries the headers, since the engine reads nothing back from it but this code does.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"
#include "husk-tl-http.h"

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static void Noop(tl_jcall *c) { (void)c; }
static const char *S(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }

/* ------------------------------------------------------------------ HashMap */

typedef struct { jobj **k, **v; int n, cap; } hmap;
static hmap *HM(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(hmap)); return o->native; }

static void Map_init(tl_jcall *c) { (void)HM(c->self); }
static void Map_put(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(S(m->k[i]), key)) { jobj *old = m->v[i]; m->v[i] = c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL; c->ret = vl(old); return; }
    }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 8; m->k = realloc(m->k, (size_t)m->cap * sizeof(jobj *)); m->v = realloc(m->v, (size_t)m->cap * sizeof(jobj *)); }
    m->k[m->n] = c->args[0].l ? tl_jni_ref(c->args[0].l) : NULL;
    m->v[m->n] = c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL;
    m->n++;
    c->ret = vl(NULL);
}
static void Map_get(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) if (!strcmp(S(m->k[i]), key)) { c->ret = vl(m->v[i] ? tl_jni_ref(m->v[i]) : NULL); return; }
    c->ret = vl(NULL);
}
static void Map_containsKey(tl_jcall *c)
{
    hmap *m = HM(c->self);
    const char *key = S(c->args[0].l);
    for (int i = 0; i < m->n; i++) if (!strcmp(S(m->k[i]), key)) { c->ret = vz(1); return; }
    c->ret = vz(0);
}
static void Map_size(tl_jcall *c) { c->ret = vi(HM(c->self)->n); }
static void Map_isEmpty(tl_jcall *c) { c->ret = vz(HM(c->self)->n == 0); }
static void Map_entrySet(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("java/util/HashSet"))); }

/* ------------------------------------------------------------ UnityWebRequest */

#define CLS "com/unity3d/player/UnityWebRequest"

typedef struct { int64_t ptr; char *url, *method; jobj *headers; int timeout_ms; int64_t upload_len; bool expect, chunked; } uwr;
static uwr *U(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(uwr)); return o->native; }

static void UWR_init(tl_jcall *c)
{
    /* (long nativeRequest, String method, Map headers, String url, boolean certificateHandler, int timeoutMs) */
    uwr *u = U(c->self);
    u->ptr = c->args[0].j;
    u->method = strdup(S(c->args[1].l));
    u->headers = c->args[2].l ? tl_jni_ref(c->args[2].l) : NULL;
    u->url = strdup(S(c->args[3].l));
    u->timeout_ms = c->args[5].i;
}
static void UWR_setup(tl_jcall *c)
{
    uwr *u = U(c->self);
    u->upload_len = c->args[0].j; u->expect = c->args[1].z; u->chunked = c->args[2].z;
}

typedef int (*upload_fn)(void *env, void *cls, int64_t ptr, void *buf);
typedef uint8_t (*download_fn)(void *env, void *cls, int64_t ptr, void *buf, int n);
typedef void (*header_fn)(void *env, void *cls, int64_t ptr, void *name, void *value);
typedef void (*int_fn)(void *env, void *cls, int64_t ptr, int v);
typedef void (*error_fn)(void *env, void *cls, int64_t ptr, int code, void *msg);

static jobj *direct_buffer(void *mem, int64_t cap)
{
    jobj *o = tl_jni_new_object(tl_jni_class("java/nio/DirectByteBuffer"));
    jvalue a, n; a.j = 0; a.l = mem; n.j = cap;
    tl_jni_set_field(o, "address", "J", a);
    tl_jni_set_field(o, "capacity", "J", n);
    return o;
}

static void UWR_run(tl_jcall *c)
{
    uwr *u = U(c->self);
    void *env = tl_jni_env(), *cls = tl_jni_class_object(CLS);
    upload_fn upload = tl_jni_native(CLS, "uploadCallback", "(JLjava/nio/ByteBuffer;)I");
    download_fn download = tl_jni_native(CLS, "downloadCallback", "(JLjava/nio/ByteBuffer;I)Z");
    header_fn header = tl_jni_native(CLS, "headerCallback", "(JLjava/lang/String;Ljava/lang/String;)V");
    int_fn length = tl_jni_native(CLS, "contentLengthCallback", "(JI)V");
    int_fn status = tl_jni_native(CLS, "responseCodeCallback", "(JI)V");
    error_fn error = tl_jni_native(CLS, "errorCallback", "(JILjava/lang/String;)V");
    if (!upload || !download || !header || !length || !status || !error) return;

    /* the request body, if any: the engine says how much with a null buffer, then hands it over in pieces */
    const size_t CHUNK = 128 * 1024;
    uint8_t *chunk = malloc(CHUNK), *body = NULL; size_t body_len = 0;
    jobj *buf = direct_buffer(chunk, (int64_t)CHUNK);
    /* Each callback is a native call of its own, with its own locals: the strings made for it go with it. */
    int more;
    TL_JNI_NATIVE_CALL(more = upload(env, cls, u->ptr, NULL));
    if (more > 0) {
        for (;;) {
            int n;
            TL_JNI_NATIVE_CALL(n = upload(env, cls, u->ptr, buf));
            if (n <= 0) break;
            body = realloc(body, body_len + (size_t)n);
            memcpy(body + body_len, chunk, (size_t)n);
            body_len += (size_t)n;
        }
    }

    /* the headers it set */
    hmap *hm = u->headers ? HM(u->headers) : NULL;
    int nh = hm ? hm->n : 0;
    const char **names = calloc((size_t)nh + 1, sizeof(char *)), **values = calloc((size_t)nh + 1, sizeof(char *));
    for (int i = 0; i < nh; i++) { names[i] = S(hm->k[i]); values[i] = S(hm->v[i]); }

    tl_http_request rq = { .url = u->url, .method = u->method, .nheaders = nh, .header_names = names, .header_values = values,
                           .body = body, .body_len = body_len, .timeout_ms = u->timeout_ms };
    tl_http_response rs;
    tl_log_line("http: %s %s (%zu bytes sent)", u->method, u->url, body_len);
    if (!tl_http_perform(&rq, &rs)) {
        tl_log_line("http: %s failed: %s", u->url, rs.message);
        TL_JNI_NATIVE_CALL(error(env, cls, u->ptr, rs.error, tl_jni_local(tl_jni_new_string(rs.message))));
    } else {
        long content_length = -1;
        for (int i = 0; i < rs.nheaders; i++) {
            TL_JNI_NATIVE_CALL(header(env, cls, u->ptr, tl_jni_local(tl_jni_new_string(rs.header_names[i])), tl_jni_local(tl_jni_new_string(rs.header_values[i]))));
            if (!strcasecmp(rs.header_names[i], "content-length")) content_length = atol(rs.header_values[i]);
        }
        TL_JNI_NATIVE_CALL(length(env, cls, u->ptr, (int)(content_length >= 0 ? content_length : (long)rs.body_len)));
        TL_JNI_NATIVE_CALL(status(env, cls, u->ptr, rs.status));
        for (size_t off = 0; off < rs.body_len;) {
            size_t n = rs.body_len - off < CHUNK ? rs.body_len - off : CHUNK;
            memcpy(chunk, rs.body + off, n);
            uint8_t go;
            TL_JNI_NATIVE_CALL(go = download(env, cls, u->ptr, buf, (int)n));
            if (!go) break;
            off += n;
        }
        tl_log_line("http: %s -> %d (%zu bytes)", u->url, rs.status, rs.body_len);
        tl_http_response_free(&rs);
    }
    free(names); free(values); free(body);
    /* chunk stays: the buffer object may still be referenced by the engine */
}

#define M_(c, n, s, f) { c, n, s, f }
static const tl_jhle k_hle[] = {
    M_("java/util/HashMap", "<init>", "()V", Map_init), M_("java/util/HashMap", "<init>", "(I)V", Map_init),
    M_("java/util/HashMap", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", Map_put),
    M_("java/util/HashMap", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_get),
    M_("java/util/HashMap", "containsKey", "(Ljava/lang/Object;)Z", Map_containsKey),
    M_("java/util/HashMap", "size", "()I", Map_size), M_("java/util/HashMap", "isEmpty", "()Z", Map_isEmpty),
    M_("java/util/HashMap", "entrySet", "()Ljava/util/Set;", Map_entrySet),
    M_("java/util/Map", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", Map_put),
    M_("java/util/Map", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", Map_get),
    M_("java/util/Map", "containsKey", "(Ljava/lang/Object;)Z", Map_containsKey),
    M_("java/util/Map", "size", "()I", Map_size), M_("java/util/Map", "isEmpty", "()Z", Map_isEmpty),
    M_("java/util/Map", "entrySet", "()Ljava/util/Set;", Map_entrySet),
    M_(CLS, "<init>", "(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;ZI)V", UWR_init),
    M_(CLS, "setupTransferSettings", "(JZZ)V", UWR_setup),
    M_(CLS, "run", "()V", UWR_run),
    M_(CLS, "clearCookieCache", "(Ljava/lang/String;Ljava/lang/String;)V", Noop),
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------ connectivity */

static void CM_activeNetworkInfo(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/NetworkInfo"))); }
static void CM_activeNetwork(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/Network"))); }
static void CM_capabilities(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("android/net/NetworkCapabilities"))); }
static void True_(tl_jcall *c) { c->ret = vz(1); }
static void NI_type(tl_jcall *c) { c->ret = vi(1); }                          /* TYPE_WIFI */
static void NI_typeName(tl_jcall *c) { c->ret = vl(tl_jni_new_string("WIFI")); }
static void NI_subtype(tl_jcall *c) { c->ret = vi(0); }

/* The phone has its network, as far as a game asking Android about it can tell: one active connection, over Wi-Fi, that is up. */
static const tl_jhle k_net[] = {
    M_("android/net/ConnectivityManager", "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;", CM_activeNetworkInfo),
    M_("android/net/ConnectivityManager", "getNetworkInfo", "(I)Landroid/net/NetworkInfo;", CM_activeNetworkInfo),
    M_("android/net/ConnectivityManager", "getActiveNetwork", "()Landroid/net/Network;", CM_activeNetwork),
    M_("android/net/ConnectivityManager", "getNetworkCapabilities", "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;", CM_capabilities),
    M_("android/net/NetworkInfo", "isConnected", "()Z", True_), M_("android/net/NetworkInfo", "isConnectedOrConnecting", "()Z", True_),
    M_("android/net/NetworkInfo", "isAvailable", "()Z", True_), M_("android/net/NetworkInfo", "getType", "()I", NI_type),
    M_("android/net/NetworkInfo", "getTypeName", "()Ljava/lang/String;", NI_typeName), M_("android/net/NetworkInfo", "getSubtype", "()I", NI_subtype),
    M_("android/net/NetworkCapabilities", "hasCapability", "(I)Z", True_), M_("android/net/NetworkCapabilities", "hasTransport", "(I)Z", True_),
    { NULL, NULL, NULL, NULL }
};

void tl_http_install(void)
{
    tl_jni_declare("android/net/NetworkInfo", "java/lang/Object"); tl_jni_declare("android/net/Network", "java/lang/Object");
    tl_jni_declare("android/net/NetworkCapabilities", "java/lang/Object");
    tl_jni_register_hle(k_net);
    tl_jni_declare("java/util/HashMap", "java/lang/Object");
    tl_jni_declare(CLS, "java/lang/Object");
    tl_jni_register_hle(k_hle);
}
