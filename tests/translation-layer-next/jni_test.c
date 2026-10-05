/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The JNI layer's references and fields, driven through the JNIEnv table the
 * way guest code drives it. Built with the sanitizers by run.sh: ASan and LSan
 * see a reference released once too often or never, TSan sees two threads on
 * one field or array element.
 *
 * husk-tl-jni.c is included rather than linked: its variadic entry points are
 * arm64 assembly for Darwin, which this host cannot assemble, and the tests
 * call only the A forms anyway. What else it links against is stubbed below,
 * with a DEX that defines two classes.
 */
#include "husk-tl-va.h"
#undef TL_VA_STUB
#define TL_VA_STUB(sym, impl) void sym(void) { abort(); }
#include "husk-tl-jni.c"

#include <stdio.h>

/* ------------------------------------------------------------------ stubs */

void tl_log_line(const char *fmt, ...)
{
    if (!getenv("JNI_TEST_LOG")) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

/* app/B extends app/A; A declares x, B declares y. */
static const struct { const char *cls, *name, *sig; } k_dex_fields[] = {
    { "app/A", "x", "Ljava/lang/Object;" },
    { "app/B", "y", "I" },
};
bool tl_dexidx_has_class(const char *name) { return !strcmp(name, "app/A") || !strcmp(name, "app/B"); }
const char *tl_dexidx_super(const char *name, char *buf, size_t n)
{
    if (strcmp(name, "app/B")) return NULL;
    snprintf(buf, n, "app/A");
    return buf;
}
bool tl_dexidx_declares_method(const char *cls, const char *name, const char *sig, bool *is_static) { (void)cls; (void)name; (void)sig; (void)is_static; return false; }
bool tl_dexidx_declares_field(const char *cls, const char *name, const char *sig, bool *is_static)
{
    (void)is_static;
    for (size_t i = 0; i < sizeof(k_dex_fields) / sizeof(*k_dex_fields); i++)
        if (!strcmp(k_dex_fields[i].cls, cls) && !strcmp(k_dex_fields[i].name, name) && (!*sig || !strcmp(k_dex_fields[i].sig, sig))) return true;
    return false;
}
bool tl_dexidx_field_sig(const char *cls, const char *name, char *out, size_t n)
{
    for (size_t i = 0; i < sizeof(k_dex_fields) / sizeof(*k_dex_fields); i++)
        if (!strcmp(k_dex_fields[i].cls, cls) && !strcmp(k_dex_fields[i].name, name)) { snprintf(out, n, "%s", k_dex_fields[i].sig); return true; }
    return false;
}
bool tl_dexidx_method_named(const char *cls, const char *name) { (void)cls; (void)name; return false; }
bool tl_dexidx_find_method_lenient(const char *cls, const char *name, const char *want, char *out, size_t n, bool *is_static)
{
    (void)cls; (void)name; (void)want; (void)out; (void)n; (void)is_static;
    return false;
}

/* ------------------------------------------------------------ the JNIEnv */

static void *E;                                 /* the JNIEnv, as guest code holds it */
#define FN(i) (((void *const *)*(void **)E)[i])
#define J(i, ret, ...) ((ret (*)(void *, __VA_ARGS__))FN(i))
#define FindClass(n)               J(6, jo, const char *)(E, n)
#define ExceptionOccurred()        ((jo (*)(void *))FN(15))(E)
#define ExceptionClear()           ((void (*)(void *))FN(17))(E)
#define PushLocalFrame(n)          J(19, int32_t, int32_t)(E, n)
#define PopLocalFrame(r)           J(20, jo, jo)(E, r)
#define NewGlobalRef(o)            J(21, jo, jo)(E, o)
#define DeleteGlobalRef(o)         J(22, void, jo)(E, o)
#define DeleteLocalRef(o)          J(23, void, jo)(E, o)
#define AllocObject(c)             J(27, jo, jo)(E, c)
#define NewObjectA(c, m, a)        J(30, jo, jo, void *, const jvalue *)(E, c, m, a)
#define GetMethodID(c, n, s)       J(33, void *, jo, const char *, const char *)(E, c, n, s)
#define CallVoidMethodA(o, m, a)   J(63, void, jo, void *, const jvalue *)(E, o, m, a)
#define GetFieldID(c, n, s)        J(94, void *, jo, const char *, const char *)(E, c, n, s)
#define GetObjectField(o, f)       J(95, jo, jo, void *)(E, o, f)
#define GetIntField(o, f)          J(100, int32_t, jo, void *)(E, o, f)
#define SetObjectField(o, f, v)    J(104, void, jo, void *, jo)(E, o, f, v)
#define SetIntField(o, f, v)       J(109, void, jo, void *, int32_t)(E, o, f, v)
#define GetStaticFieldID(c, n, s)  J(144, void *, jo, const char *, const char *)(E, c, n, s)
#define GetStaticObjectField(c, f) J(145, jo, jo, void *)(E, c, f)
#define SetStaticObjectField(c, f, v) J(154, void, jo, void *, jo)(E, c, f, v)
#define NewStringUTF(s)            J(167, jo, const char *)(E, s)
#define NewObjectArray(n, c, i)    J(172, jo, int32_t, jo, jo)(E, n, c, i)
#define GetObjectArrayElement(a, i) J(173, jo, jo, int32_t)(E, a, i)
#define SetObjectArrayElement(a, i, v) J(174, void, jo, int32_t, jo)(E, a, i, v)
#define ExceptionCheck()           ((uint8_t (*)(void *))FN(228))(E)

static void *vm_fn(int i) { return ((void *const *)*(void **)tl_jni_vm())[i]; }
static void attach(void) { void *env; ((int32_t (*)(void *, void **, void *))vm_fn(4))(tl_jni_vm(), &env, NULL); }
static void detach(void) { ((int32_t (*)(void *))vm_fn(5))(tl_jni_vm()); }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); failures++; } } while (0)

/* --------------------------------------------------------- what is implemented */

static void Box_init(tl_jcall *c) { jvalue v; v.j = 0; v.i = c->args[0].i; tl_jni_set_field(c->self, "v", "I", v); }
static void Box_boom(tl_jcall *c) { (void)c; tl_jni_throw("java/lang/IllegalStateException", "boom"); }
static const tl_jhle k_hle[] = {
    { "t/Box", "<init>", "(I)V", Box_init },
    { "t/Box", "boom", "()V", Box_boom },
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------------------------------ cases */

static void frames(void)
{
    tl_jni_local_push();
    jo s = NewStringUTF("s");
    CHECK(s->refs == 1);
    jo g = NewGlobalRef(s);
    CHECK(g == s && s->refs == 2);
    tl_jni_local_pop();
    CHECK(s->refs == 1);
    DeleteGlobalRef(g);
}

static void pop_local_frame_promotes(void)
{
    tl_jni_local_push();
    CHECK(PushLocalFrame(16) == 0);
    jo s = NewStringUTF("kept");
    jo other = NewStringUTF("dropped");
    jo g = NewGlobalRef(s), go = NewGlobalRef(other);
    jo r = PopLocalFrame(s);
    CHECK(r == s && s->refs == 2);              /* the global, and a local in the frame outside */
    CHECK(other->refs == 1);
    CHECK(PopLocalFrame(NULL) == NULL);         /* none of the guest's left: ignored */
    CHECK(s->refs == 2);
    tl_jni_local_pop();
    CHECK(s->refs == 1);
    DeleteGlobalRef(g);
    DeleteGlobalRef(go);
}

static void delete_local_ref(void)
{
    tl_jni_local_push();
    jo s = NewStringUTF("local");
    jo g = NewGlobalRef(s);
    DeleteLocalRef(s);
    CHECK(s->refs == 1);
    DeleteLocalRef(s);                          /* only the global is left: not a local, so ignored */
    CHECK(s->refs == 1);
    jo h = tl_jni_new_string("host's");
    DeleteLocalRef(h);
    CHECK(h->refs == 1);
    tl_jni_local_pop();
    CHECK(s->refs == 1 && h->refs == 1);
    DeleteGlobalRef(g);
    tl_jni_unref(h);
}

static jo g_kept;
static void *nested_body(void *arg)
{
    (void)arg;
    attach();
    for (int i = 0; i < 20000; i++) NewStringUTF("base");
    g_kept = NewGlobalRef(NewStringUTF("kept"));
    for (int d = 0; d < 3; d++) { PushLocalFrame(4); for (int i = 0; i < 100; i++) NewStringUTF("nested"); }
    detach();
    return NULL;
}
static void nested_frames_and_detach(void)
{
    pthread_t t;
    pthread_create(&t, NULL, nested_body, NULL);
    pthread_join(t, NULL);
    CHECK(g_kept->refs == 1);
    DeleteGlobalRef(g_kept);
}

static void field_and_element_ownership(void)
{
    tl_jni_local_push();
    jo cls = FindClass("t/Holder");
    void *f = GetFieldID(cls, "s", "Ljava/lang/String;");
    jo o = AllocObject(cls);
    jo s = NewStringUTF("s"), t = NewStringUTF("t");
    SetObjectField(o, f, s);
    CHECK(s->refs == 2);
    jo r = GetObjectField(o, f);
    CHECK(r == s && s->refs == 3);
    SetObjectField(o, f, t);
    CHECK(s->refs == 2 && t->refs == 2);
    DeleteLocalRef(o);                          /* the object goes and releases its field */
    CHECK(t->refs == 1);

    jo a = NewObjectArray(2, FindClass("java/lang/String"), NULL);
    jo h = tl_jni_new_string("held by the array alone");
    SetObjectArrayElement(a, 0, h);
    CHECK(h->refs == 2);
    tl_jni_unref(h);
    SetObjectArrayElement(a, 0, h);             /* the same object again: kept, not released first */
    CHECK(h->refs == 1);
    jo e = GetObjectArrayElement(a, 0);
    CHECK(e == h && h->refs == 2 && !strcmp(tl_jni_string(e), "held by the array alone"));
    CHECK(GetObjectArrayElement(a, 2) == NULL);
    tl_jni_local_pop();
}

static void exceptions_and_new_object(void)
{
    tl_jni_local_push();
    CHECK(FindClass("com/nowhere/Missing") == NULL);
    CHECK(ExceptionCheck());
    jo e = ExceptionOccurred();
    CHECK(e && !strcmp(tl_jni_class_name(e), "java/lang/NoClassDefFoundError"));
    ExceptionClear();
    CHECK(!ExceptionCheck());

    jo box = FindClass("t/Box");
    void *init = GetMethodID(box, "<init>", "(I)V");
    jvalue a; a.j = 0; a.i = 42;
    jo o = NewObjectA(box, init, &a);
    CHECK(o && o->refs == 1);
    CHECK(GetIntField(o, GetFieldID(box, "v", "I")) == 42);
    CallVoidMethodA(o, GetMethodID(box, "boom", "()V"), NULL);
    CHECK(ExceptionCheck());
    ExceptionClear();
    tl_jni_local_pop();
}

static void inherited_field(void)
{
    tl_jni_local_push();
    /* The DEX says A declares x: asked of B first, it is still A's. */
    jo A = FindClass("app/A"), B = FindClass("app/B");
    void *bx = GetFieldID(B, "x", "Ljava/lang/Object;");
    void *ax = GetFieldID(A, "x", "Ljava/lang/Object;");
    CHECK(bx && bx == ax);
    CHECK(GetFieldID(A, "y", "I") == NULL);     /* B's field is not A's */
    ExceptionClear();
    /* A framework class's fields are not known: one asked of the superclass is found from the subclass. */
    jo P = tl_jni_declare("t/P", "java/lang/Object")->mirror, Q = tl_jni_declare("t/Q", "t/P")->mirror;
    void *px = GetFieldID(P, "x", "I");
    CHECK(GetFieldID(Q, "x", "I") == px);
    jo q = AllocObject(Q);
    SetIntField(q, px, 7);
    CHECK(GetIntField(q, GetFieldID(Q, "x", "I")) == 7);
    tl_jni_local_pop();
}

static void subclass_field_does_not_alias(void)
{
    tl_jni_local_push();
    jo A = FindClass("app/A"), B = FindClass("app/B");
    void *ax = GetFieldID(A, "x", "Ljava/lang/Object;"), *by = GetFieldID(B, "y", "I");
    jo o = AllocObject(B), s = NewStringUTF("x"), g = NewGlobalRef(s);
    SetObjectField(o, ax, s);
    SetIntField(o, by, 5);
    CHECK(GetObjectField(o, ax) == s);
    CHECK(GetIntField(o, by) == 5);
    SetIntField(o, ax, 9);                      /* the wrong kind for x: ignored */
    CHECK(GetObjectField(o, ax) == s);
    CHECK(GetObjectField(o, by) == NULL);
    tl_jni_local_pop();
    CHECK(s->refs == 1);                        /* o went, and released x */
    DeleteGlobalRef(g);
}

static void many_fields_released(void)
{
    tl_jni_local_push();
    jo cls = FindClass("t/Wide");
    jo o = AllocObject(cls);
    jo g[70];
    for (int i = 0; i < 70; i++) {
        char n[16]; snprintf(n, sizeof(n), "f%d", i);
        jo s = NewStringUTF(n);
        SetObjectField(o, GetFieldID(cls, n, "Ljava/lang/String;"), s);
        g[i] = NewGlobalRef(s);
    }
    tl_jni_local_pop();                         /* o goes, and every field with it */
    for (int i = 0; i < 70; i++) { CHECK(g[i]->refs == 1); DeleteGlobalRef(g[i]); }
}

static int g_finalized;
static void free_native(jobj *o) { free(o->native); g_finalized++; }
static void native_state_freed(void)
{
    tl_jni_local_push();
    jo o = AllocObject(FindClass("t/Holder"));
    o->native = malloc(64);
    o->finalize = free_native;
    NewGlobalRef(o);
    DeleteGlobalRef(o);
    CHECK(g_finalized == 0);
    tl_jni_local_pop();
    CHECK(g_finalized == 1);
}

static void *exit_body(void *arg)
{
    (void)arg;
    attach();
    NewStringUTF("in the base frame");
    TL_JNI_NATIVE_CALL({
        g_kept = NewGlobalRef(NewStringUTF("kept"));
        PushLocalFrame(4);
        for (int i = 0; i < 100; i++) NewStringUTF("guest's");
        pthread_exit(NULL);                     /* as a guest's exit does, from inside a native call */
    });
    return NULL;
}
static void thread_exit_without_detach(void)
{
    pthread_t t;
    pthread_create(&t, NULL, exit_body, NULL);
    pthread_join(t, NULL);
    CHECK(g_kept->refs == 1);
    DeleteGlobalRef(g_kept);
}

enum { STRESS = 20000 };
static jo g_scls, g_arr, g_obj;
static void *g_sfid, *g_ofid[2];
static void *stress_writer(void *arg)
{
    int k = (int)(intptr_t)arg;
    for (int i = 0; i < STRESS; i++) TL_JNI_NATIVE_CALL({
        jo s = NewStringUTF("written");
        SetStaticObjectField(g_scls, g_sfid, s);
        SetObjectArrayElement(g_arr, 0, s);
        SetObjectField(g_obj, g_ofid[k], s);
    });
    return NULL;
}
static void *stress_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < STRESS; i++) TL_JNI_NATIVE_CALL({
        jo s = GetStaticObjectField(g_scls, g_sfid);
        jo e = GetObjectArrayElement(g_arr, 0);
        if (s && strcmp(tl_jni_string(s), "written")) failures++;
        if (e && strcmp(tl_jni_string(e), "written")) failures++;
        DeleteLocalRef(s);
    });
    return NULL;
}
static void two_threads_on_one_field(void)
{
    g_scls = FindClass("t/Shared");
    g_sfid = GetStaticFieldID(g_scls, "s", "Ljava/lang/String;");
    g_ofid[0] = GetFieldID(g_scls, "a", "Ljava/lang/String;");
    g_ofid[1] = GetFieldID(g_scls, "b", "Ljava/lang/String;");
    g_arr = tl_jni_new_obj_array(NULL, 1);
    g_obj = tl_jni_new_object(g_scls->klass.jc);
    pthread_t w[2], r;
    pthread_create(&w[0], NULL, stress_writer, (void *)0);
    pthread_create(&w[1], NULL, stress_writer, (void *)1);
    pthread_create(&r, NULL, stress_reader, NULL);
    pthread_join(w[0], NULL); pthread_join(w[1], NULL); pthread_join(r, NULL);
    tl_jni_unref(g_arr);
    tl_jni_unref(g_obj);
    jvalue z; z.j = 0;
    tl_jni_set_static("t/Shared", "s", "Ljava/lang/String;", z);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    E = tl_jni_env();
    tl_jni_register_hle(k_hle);
    /* Classes the host declares, as the HLE files do; FindClass finds nothing else outside the DEX and the framework. */
    const char *host[] = { "t/Holder", "t/Box", "t/Wide", "t/Shared" };
    for (size_t i = 0; i < sizeof(host) / sizeof(*host); i++) tl_jni_declare(host[i], "java/lang/Object");
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "push and pop", frames },
        { "PopLocalFrame promotes its result", pop_local_frame_promotes },
        { "DeleteLocalRef of a local and of a global", delete_local_ref },
        { "nested frames, 20k locals, detach", nested_frames_and_detach },
        { "fields and array elements own their values", field_and_element_ownership },
        { "exceptions and NewObject", exceptions_and_new_object },
        { "an inherited field through the subclass", inherited_field },
        { "a subclass's field is not its superclass's", subclass_field_does_not_alias },
        { "70 object fields released with their object", many_fields_released },
        { "an object's native state goes with it", native_state_freed },
        { "a thread that exits without detaching", thread_exit_without_detach },
        { "two threads on one field and element", two_threads_on_one_field },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        int before = failures;
        cases[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", cases[i].name);
    }
    return failures != 0;
}
