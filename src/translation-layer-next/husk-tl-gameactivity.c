/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-gameactivity.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);
jobj *tl_hle_assets(void);
jobj *tl_hle_config(void);
void tl_set_data_dir(const char *dir);
void tl_nwindow_configure(int w, int h, void *layer);
void tl_mc_hle_install(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_mc_set_activity(jobj *activity);
void tl_security_install(void);
void tl_fmod_install(void);

#define CLS_GA "com/google/androidgamesdk/GameActivity"
#define CLS_MAIN "com/mojang/minecraftpe/MainActivity"

static struct {
    tl_ga_config cfg;
    char apk[1024], data[512], pkg[128], frame_dir[512], angle_egl[600], angle_gles[600];
    jobj *activity, *surface;
    int64_t handle;
    atomic_bool paused;
} G;

static void load_library(const char *name)
{
    jvalue a; a.j = 0; a.l = tl_jni_new_string(name);
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
}

bool tl_ga_start(const tl_ga_config *cfg)
{
    G.cfg = *cfg;
    snprintf(G.apk, sizeof(G.apk), "%s", cfg->apk_path);
    snprintf(G.data, sizeof(G.data), "%s", cfg->data_dir);
    snprintf(G.pkg, sizeof(G.pkg), "%s", cfg->package_name);
    G.cfg.apk_path = G.apk; G.cfg.data_dir = G.data; G.cfg.package_name = G.pkg;
    if (cfg->frame_dir) { snprintf(G.frame_dir, sizeof(G.frame_dir), "%s", cfg->frame_dir); G.cfg.frame_dir = G.frame_dir; }
    if (cfg->angle_egl) { snprintf(G.angle_egl, sizeof(G.angle_egl), "%s", cfg->angle_egl); G.cfg.angle_egl = G.angle_egl; }
    if (cfg->angle_gles) { snprintf(G.angle_gles, sizeof(G.angle_gles), "%s", cfg->angle_gles); G.cfg.angle_gles = G.angle_gles; }

    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("minecraft: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    tl_egl_es31_shim(true);
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();
    tl_mc_hle_install(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_security_install();
    tl_fmod_install();

    G.activity = tl_jni_new_object(tl_jni_class(CLS_MAIN));
    tl_hle_set_activity(G.activity);
    tl_mc_set_activity(G.activity);

    /* MainActivity.<clinit>: the libraries it loads, each running its JNI_OnLoad. */
    static const char *const libs[] = { "fmod", "MediaDecoders_Android", "minecraftpe", NULL };
    for (int i = 0; libs[i]; i++) {
        load_library(libs[i]);
        if (tl_jni_pending()) { tl_log_line("minecraft: loading lib%s.so failed", libs[i]); return false; }
    }
    /*
     * The game embeds V8 (its UI and scripting run on it). V8 normally generates machine code at run time, which needs
     * memory that is writable and executable and which an iPhone does not give an app. Its interpreter and built-in
     * code need none of that, so it is set to run without a JIT before anything starts it.
     */
    {
        tl_lib *mc = tl_ld_find_lib("libminecraftpe.so");
        void (*set_flags)(const char *) = mc ? (void (*)(const char *))tl_ld_sym(mc, "_ZN2v82V818SetFlagsFromStringEPKc") : NULL;
        if (set_flags) { set_flags("--jitless"); tl_log_line("minecraft: V8 set to run without a JIT"); }
        else tl_log_line("minecraft: V8's flag setter is not exported; the game's JavaScript may need executable memory");
    }
    tl_log_line("minecraft: libraries loaded (%zu of %zu MiB of executable memory used)", tl_xmem_used() >> 20, tl_xmem_size() >> 20);
    return true;
}

/* The native a Java method would have bound: registered by the library, or found by its JNI name. */
static void *native_of(const char *cls, const char *name, const char *sig, const char *mangled)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) {
        tl_lib *lib = tl_ld_find_lib("libminecraftpe.so");
        if (lib) fn = tl_ld_sym(lib, mangled);
    }
    if (!fn) tl_log_line("minecraft: native %s.%s%s is not provided by the library", cls, name, sig);
    return fn;
}

/* GameActivity's natives are registered by the library when it initialises; each takes the handle first. */
#define GA_NATIVE(name, sig) native_of(CLS_GA, name, sig, "Java_com_google_androidgamesdk_GameActivity_" name)
#define MAIN_NATIVE(name, sig) native_of(CLS_MAIN, name, sig, "Java_com_mojang_minecraftpe_MainActivity_" name)

/* ------------------------------------------------------------- the UI thread */

/*
 * Android runs an activity's callbacks on one thread with a message loop, and the game posts work to it
 * (runOnUiThread). This is that: a queue of jobs, served in order by the thread that also ran the lifecycle.
 */
typedef struct ui_job { void (*fn)(void *); void *arg; struct ui_job *next; } ui_job;
static struct { pthread_mutex_t mu; pthread_cond_t cv; ui_job *head, *tail; pthread_t thread; bool started; void *looper; } UI = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

void tl_ga_post(void (*fn)(void *), void *arg)
{
    ui_job *j = calloc(1, sizeof(*j));
    j->fn = fn; j->arg = arg;
    pthread_mutex_lock(&UI.mu);
    if (UI.tail) UI.tail->next = j; else UI.head = j;
    UI.tail = j;
    pthread_cond_signal(&UI.cv);
    void *looper = UI.looper;
    pthread_mutex_unlock(&UI.mu);
    if (looper) ((void (*)(void *))tl_bionic_find("ALooper_wake"))(looper);
}

static void *ui_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UiThread");
    tl_ld_thread_attach();          /* guest code runs here for the whole game: its own thread block */
    /* Android's main thread already has a looper; the game asks for it (ALooper_forThread) while it initialises. */
    UI.looper = ((void *(*)(int))tl_bionic_find("ALooper_prepare"))(0);
    void *env = tl_jni_env();
    jobj *activity = G.activity;
    jobj *surface = tl_jni_new_object(tl_jni_class("android/view/Surface"));
    G.surface = surface;

    /* GameActivity.onCreate: the library's own initialiser, with the directories and the asset manager. */
    typedef int64_t (*init_fn)(void *env, void *self, void *internal, void *obb, void *ext, void *assets, void *saved, void *config);
    init_fn init = (init_fn)native_of(CLS_GA, "initializeNativeCode",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J",
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
    if (!init) return NULL;
    char files[600], obb[600], ext[700];
    snprintf(files, sizeof(files), "%s/files", G.data);
    snprintf(obb, sizeof(obb), "%s/obb", G.data);
    snprintf(ext, sizeof(ext), "%s/sdcard/Android/data/%s/files", G.data, G.pkg);
    tl_log_line("minecraft: initializeNativeCode");
    TL_JNI_NATIVE_CALL(G.handle = init(env, activity, tl_jni_local(tl_jni_new_string(files)), tl_jni_local(tl_jni_new_string(obb)), tl_jni_local(tl_jni_new_string(ext)),
                                       tl_hle_assets(), NULL, tl_hle_config()));
    tl_log_line("minecraft: initializeNativeCode -> %#llx", (unsigned long long)G.handle);
    if (tl_guest_ended() || !G.handle || tl_jni_pending()) { tl_log_line("minecraft: the game's native code did not initialise"); return NULL; }

    typedef void (*set_input_fn)(void *env, void *self, int64_t h, void *conn);
    set_input_fn set_input = (set_input_fn)GA_NATIVE("setInputConnectionNative", "(JLcom/google/androidgamesdk/gametextinput/InputConnection;)V");
    if (set_input) TL_JNI_NATIVE_CALL(set_input(env, activity, G.handle, tl_jni_local(tl_jni_new_object(tl_jni_class("com/google/androidgamesdk/gametextinput/InputConnection")))));

    /* MainActivity.onCreate, after super.onCreate: the Java crash manager is set up by the game's own thread,
     * and the UI thread waits for it to say so. */
    typedef void (*wait_fn)(void *env, void *cls);
    wait_fn wait = (wait_fn)MAIN_NATIVE("nativeWaitCrashManagementSetupComplete", "()V");
    if (wait) { tl_log_line("minecraft: waiting for the crash manager setup"); TL_JNI_NATIVE_CALL(wait(env, tl_jni_class_object(CLS_MAIN))); tl_log_line("minecraft: crash manager setup complete"); }

    typedef void (*life_fn)(void *env, void *self, int64_t h);
    life_fn on_start = (life_fn)GA_NATIVE("onStartNative", "(J)V");
    life_fn on_resume = (life_fn)GA_NATIVE("onResumeNative", "(J)V");
    if (on_start) TL_JNI_NATIVE_CALL(on_start(env, activity, G.handle));
    tl_log_line("minecraft: onStart done");
    if (on_resume) TL_JNI_NATIVE_CALL(on_resume(env, activity, G.handle));
    tl_log_line("minecraft: onResume done");

    typedef void (*surf_fn)(void *env, void *self, int64_t h, void *surface);
    typedef void (*surf_changed_fn)(void *env, void *self, int64_t h, void *surface, int format, int w, int h2);
    typedef void (*focus_fn)(void *env, void *self, int64_t h, uint8_t focused);
    surf_fn created = (surf_fn)GA_NATIVE("onSurfaceCreatedNative", "(JLandroid/view/Surface;)V");
    surf_changed_fn changed = (surf_changed_fn)GA_NATIVE("onSurfaceChangedNative", "(JLandroid/view/Surface;III)V");
    focus_fn focus = (focus_fn)GA_NATIVE("onWindowFocusChangedNative", "(JZ)V");
    if (created) TL_JNI_NATIVE_CALL(created(env, activity, G.handle, surface));
    tl_log_line("minecraft: surface created");
    if (changed) TL_JNI_NATIVE_CALL(changed(env, activity, G.handle, surface, 1 /* PixelFormat.RGBA_8888 */, G.cfg.width, G.cfg.height));
    tl_log_line("minecraft: surface changed %dx%d", G.cfg.width, G.cfg.height);
    if (focus) TL_JNI_NATIVE_CALL(focus(env, activity, G.handle, 1));
    tl_log_line("minecraft: focus gained");

    /* The message loop: the looper serves what the game's native glue registered on it (its main-work pipe), and
     * the jobs posted with tl_ga_post run in between. */
    int (*poll_once)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollOnce");
    while (!tl_guest_ended()) {
        /* On Android this is MessageQueue.nativePollOnce, a native method: what the callbacks it runs leave as locals goes when it returns. */
        TL_JNI_NATIVE_CALL(poll_once(50, NULL, NULL, NULL));
        for (;;) {
            pthread_mutex_lock(&UI.mu);
            ui_job *j = UI.head;
            if (j) { UI.head = j->next; if (!UI.head) UI.tail = NULL; }
            pthread_mutex_unlock(&UI.mu);
            if (!j) break;
            j->fn(j->arg);
            free(j);
            if (tl_jni_pending()) tl_jni_clear();
        }
    }
    return NULL;
}

bool tl_ga_is_ui_thread(void) { return UI.started && pthread_equal(pthread_self(), UI.thread); }

bool tl_ga_run(void)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    if (pthread_create(&UI.thread, &a, ui_main, NULL) != 0) return false;
    UI.started = true;
    return true;
}

/* ----------------------------------------------------------------- touch */

extern jobj *tl_input_motion_event(int action, int count, const int *ids, const float *xs, const float *ys, int64_t down_ms, int64_t event_ms);

typedef struct { jobj *ev; int pointers, action; int64_t down_ms, event_ms; } touch_job;

/* GameActivity.onTouchEventNative: the Java side hands over the event with its header fields already read out. The arguments past the
 * eighth go on the stack, one eight-byte slot each on the guest's ABI, so they are declared eight bytes wide here. */
static void touch_run(void *arg)
{
    touch_job *t = arg;
    typedef uint8_t (*touch_fn)(void *env, void *self, int64_t h, void *ev, int pointers, int history, int device, int source,
                                uint64_t action, int64_t event_time, int64_t down_time, uint64_t flags, uint64_t meta,
                                uint64_t action_button, uint64_t button_state, uint64_t classification, uint64_t edge_flags,
                                float precision_x, float precision_y);
    touch_fn fn = (touch_fn)GA_NATIVE("onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z");
    if (fn && G.handle) TL_JNI_NATIVE_CALL(fn(tl_jni_env(), G.activity, G.handle, t->ev, t->pointers, 0, 0, 0x1002 /* SOURCE_TOUCHSCREEN */, (uint64_t)t->action,
                                              t->event_ms, t->down_ms, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f));
    tl_jni_unref(t->ev);
    free(t);
}

void tl_ga_touch(int phase, int id, float x, float y)
{
    static struct { pthread_mutex_t lock; int n, ids[10]; float x[10], y[10]; int64_t down_ms; } T = { .lock = PTHREAD_MUTEX_INITIALIZER };
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    pthread_mutex_lock(&T.lock);
    int idx = -1;
    for (int i = 0; i < T.n; i++) if (T.ids[i] == id) idx = i;
    int action;
    if (phase == 0) {
        if (idx < 0 && T.n < 10) { idx = T.n++; T.ids[idx] = id; }
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        if (T.n == 1) T.down_ms = now;
        action = T.n == 1 ? 0 /* ACTION_DOWN */ : (5 /* ACTION_POINTER_DOWN */ | (idx << 8));
    } else if (phase == 1) {
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        action = 2; /* ACTION_MOVE */
    } else if (phase == 3) {
        action = 3; /* ACTION_CANCEL */
    } else {
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        action = T.n == 1 ? 1 /* ACTION_UP */ : (6 /* ACTION_POINTER_UP */ | (idx << 8));
    }
    touch_job *t = calloc(1, sizeof(*t));
    t->ev = tl_input_motion_event(action, T.n, T.ids, T.x, T.y, T.down_ms, now);
    t->pointers = T.n; t->action = action; t->down_ms = T.down_ms; t->event_ms = now;
    /* An ended touch leaves the set after the event that reports it. */
    if (phase == 2 && idx >= 0) {
        float lx[10], ly[10]; int li[10];
        memcpy(lx, T.x, sizeof(lx)); memcpy(ly, T.y, sizeof(ly)); memcpy(li, T.ids, sizeof(li));
        int n = 0;
        for (int i = 0; i < T.n; i++) if (i != idx) { T.ids[n] = li[i]; T.x[n] = lx[i]; T.y[n] = ly[i]; n++; }
        T.n = n;
    } else if (phase == 3) {
        T.n = 0;
    }
    pthread_mutex_unlock(&T.lock);
    tl_ga_post(touch_run, t);
}
/* Leaving the screen is Activity.onPause/onStop with the focus gone; coming back is onStart/onResume with it returned. The game stops
 * updating and drawing on the pause, which is what keeps it off the GPU while the app is in the background. */
static void pause_run(void *arg)
{
    bool pause = arg != NULL;
    typedef void (*life_fn)(void *env, void *self, int64_t h);
    typedef void (*focus_fn)(void *env, void *self, int64_t h, uint8_t focused);
    life_fn on_pause = (life_fn)GA_NATIVE("onPauseNative", "(J)V"), on_stop = (life_fn)GA_NATIVE("onStopNative", "(J)V");
    life_fn on_start = (life_fn)GA_NATIVE("onStartNative", "(J)V"), on_resume = (life_fn)GA_NATIVE("onResumeNative", "(J)V");
    focus_fn focus = (focus_fn)GA_NATIVE("onWindowFocusChangedNative", "(JZ)V");
    void *env = tl_jni_env();
    if (!G.handle) return;
    if (pause) {
        if (focus) TL_JNI_NATIVE_CALL(focus(env, G.activity, G.handle, 0));
        if (on_pause) TL_JNI_NATIVE_CALL(on_pause(env, G.activity, G.handle));
        if (on_stop) TL_JNI_NATIVE_CALL(on_stop(env, G.activity, G.handle));
        tl_log_line("minecraft: paused");
    } else {
        if (on_start) TL_JNI_NATIVE_CALL(on_start(env, G.activity, G.handle));
        if (on_resume) TL_JNI_NATIVE_CALL(on_resume(env, G.activity, G.handle));
        if (focus) TL_JNI_NATIVE_CALL(focus(env, G.activity, G.handle, 1));
        tl_log_line("minecraft: resumed");
    }
}

void tl_ga_set_paused(bool paused)
{
    if (atomic_exchange(&G.paused, paused) == paused || !G.handle) return;
    tl_ga_post(pause_run, paused ? (void *)1 : NULL);
}
unsigned long tl_ga_frames(void) { return tl_egl_frames_presented(); }
