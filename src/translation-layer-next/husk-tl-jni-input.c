/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Touch input, as Unity expects to receive it: a Java android.view.MotionEvent handed to
 * UnityPlayer.nativeInjectEvent, which the engine reads back through JNI one getter at a time.
 *
 * An event here is a small C record behind a MotionEvent object; the getters read the
 * record. The caller describes touches in screen pixels with y down, which is what
 * Android reports, so there is nothing to convert.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_POINTERS 10

enum { ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6, SOURCE_TOUCHSCREEN = 0x1002 };

typedef struct motion {
    int action, count;
    int ids[MAX_POINTERS];
    float x[MAX_POINTERS], y[MAX_POINTERS];
    int64_t down_ms, event_ms;
} motion;

static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vj(int64_t j) { jvalue v; v.j = j; return v; }
static jvalue vf(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static const motion *M_(const tl_jcall *c) { return c->self ? c->self->native : NULL; }
static void motion_free(jobj *o) { free(o->native); }       /* each event has its own copy */

static int pidx(const motion *m, int i) { return i >= 0 && i < m->count ? i : 0; }

static void ME_getAction(tl_jcall *c)       { const motion *m = M_(c); c->ret = vi(m ? m->action : 0); }
static void ME_getActionMasked(tl_jcall *c) { const motion *m = M_(c); c->ret = vi(m ? m->action & 0xFF : 0); }
static void ME_getActionIndex(tl_jcall *c)  { const motion *m = M_(c); c->ret = vi(m ? (m->action >> 8) & 0xFF : 0); }
static void ME_getPointerCount(tl_jcall *c) { const motion *m = M_(c); c->ret = vi(m ? m->count : 0); }
static void ME_getPointerId(tl_jcall *c)    { const motion *m = M_(c); c->ret = vi(m ? m->ids[pidx(m, c->args[0].i)] : 0); }
static void ME_findPointerIndex(tl_jcall *c)
{
    const motion *m = M_(c);
    int r = -1;
    for (int i = 0; m && i < m->count; i++) if (m->ids[i] == c->args[0].i) r = i;
    c->ret = vi(r);
}
static void ME_getXi(tl_jcall *c)  { const motion *m = M_(c); c->ret = vf(m ? m->x[pidx(m, c->args[0].i)] : 0); }
static void ME_getYi(tl_jcall *c)  { const motion *m = M_(c); c->ret = vf(m ? m->y[pidx(m, c->args[0].i)] : 0); }
static void ME_getX(tl_jcall *c)   { const motion *m = M_(c); c->ret = vf(m ? m->x[0] : 0); }
static void ME_getY(tl_jcall *c)   { const motion *m = M_(c); c->ret = vf(m ? m->y[0] : 0); }
static void ME_getPressure(tl_jcall *c) { c->ret = vf(1.0f); }
static void ME_getSize(tl_jcall *c)     { c->ret = vf(0.1f); }
static void ME_zeroF(tl_jcall *c)       { c->ret = vf(0.0f); }
static void ME_getToolType(tl_jcall *c) { c->ret = vi(1); }                       /* TOOL_TYPE_FINGER */
static void ME_getEventTime(tl_jcall *c){ const motion *m = M_(c); c->ret = vj(m ? m->event_ms : 0); }
static void ME_getDownTime(tl_jcall *c) { const motion *m = M_(c); c->ret = vj(m ? m->down_ms : 0); }
static void ME_getSource(tl_jcall *c)   { c->ret = vi(SOURCE_TOUCHSCREEN); }
static void ME_zeroI(tl_jcall *c)       { c->ret = vi(0); }
static void ME_getAxisValue(tl_jcall *c)
{
    const motion *m = M_(c);
    int axis = c->args[0].i, i = c->args[1].i;
    float v = 0;
    if (m && axis == 0) v = m->x[pidx(m, i)];
    else if (m && axis == 1) v = m->y[pidx(m, i)];
    else if (axis == 2) v = 1.0f;
    else if (axis == 3) v = 0.1f;
    c->ret = vf(v);
}
static void ME_getAxisValue1(tl_jcall *c)
{
    const motion *m = M_(c);
    int axis = c->args[0].i;
    c->ret = vf(m && axis == 0 ? m->x[0] : m && axis == 1 ? m->y[0] : 0);
}
static void ME_obtainCopy(tl_jcall *c)
{
    const motion *m = c->args[0].l ? ((const jobj *)c->args[0].l)->native : NULL;
    if (!m) return;
    jobj *o = tl_jni_new_object(tl_jni_class("android/view/MotionEvent"));
    motion *copy = malloc(sizeof(*copy));
    memcpy(copy, m, sizeof(*copy));
    o->native = copy;
    o->finalize = motion_free;
    c->ret.l = o;
}
static void ME_recycle(tl_jcall *c) { (void)c; }
static void ME_isTrue(tl_jcall *c)      { c->ret = vz(1); }

/* The event for the pointers currently down; `action` already carries any pointer index. */
jobj *tl_input_motion_event(int action, int count, const int *ids, const float *xs, const float *ys, int64_t down_ms, int64_t event_ms)
{
    motion *m = calloc(1, sizeof(*m));
    m->action = action;
    m->count = count > MAX_POINTERS ? MAX_POINTERS : count;
    for (int i = 0; i < m->count; i++) { m->ids[i] = ids[i]; m->x[i] = xs[i]; m->y[i] = ys[i]; }
    m->down_ms = down_ms;
    m->event_ms = event_ms;
    jobj *o = tl_jni_new_object(tl_jni_class("android/view/MotionEvent"));
    o->native = m;
    o->finalize = motion_free;
    return o;
}

#define K(c, n, s, f) { c, n, s, f }
static const tl_jhle k_input_hle[] = {
    K("android/view/MotionEvent", "getAction", "()I", ME_getAction),
    K("android/view/MotionEvent", "getActionMasked", "()I", ME_getActionMasked),
    K("android/view/MotionEvent", "getActionIndex", "()I", ME_getActionIndex),
    K("android/view/MotionEvent", "getPointerCount", "()I", ME_getPointerCount),
    K("android/view/MotionEvent", "getPointerId", "(I)I", ME_getPointerId),
    K("android/view/MotionEvent", "findPointerIndex", "(I)I", ME_findPointerIndex),
    K("android/view/MotionEvent", "getX", "(I)F", ME_getXi), K("android/view/MotionEvent", "getY", "(I)F", ME_getYi),
    K("android/view/MotionEvent", "getX", "()F", ME_getX), K("android/view/MotionEvent", "getY", "()F", ME_getY),
    K("android/view/MotionEvent", "getRawX", "()F", ME_getX), K("android/view/MotionEvent", "getRawY", "()F", ME_getY),
    K("android/view/MotionEvent", "getRawX", "(I)F", ME_getXi), K("android/view/MotionEvent", "getRawY", "(I)F", ME_getYi),
    K("android/view/MotionEvent", "getPressure", "(I)F", ME_getPressure), K("android/view/MotionEvent", "getPressure", "()F", ME_getPressure),
    K("android/view/MotionEvent", "getSize", "(I)F", ME_getSize), K("android/view/MotionEvent", "getSize", "()F", ME_getSize),
    K("android/view/MotionEvent", "getTouchMajor", "(I)F", ME_zeroF), K("android/view/MotionEvent", "getTouchMinor", "(I)F", ME_zeroF),
    K("android/view/MotionEvent", "getOrientation", "(I)F", ME_zeroF),
    K("android/view/MotionEvent", "getToolType", "(I)I", ME_getToolType),
    K("android/view/MotionEvent", "getEventTime", "()J", ME_getEventTime),
    K("android/view/MotionEvent", "getDownTime", "()J", ME_getDownTime),
    K("android/view/MotionEvent", "getSource", "()I", ME_getSource),
    K("android/view/InputEvent", "getSource", "()I", ME_getSource),
    K("android/view/MotionEvent", "getDeviceId", "()I", ME_zeroI), K("android/view/InputEvent", "getDeviceId", "()I", ME_zeroI),
    K("android/view/MotionEvent", "getFlags", "()I", ME_zeroI), K("android/view/MotionEvent", "getMetaState", "()I", ME_zeroI),
    K("android/view/MotionEvent", "getButtonState", "()I", ME_zeroI), K("android/view/MotionEvent", "getEdgeFlags", "()I", ME_zeroI),
    K("android/view/MotionEvent", "getHistorySize", "()I", ME_zeroI),
    K("android/view/MotionEvent", "getAxisValue", "(II)F", ME_getAxisValue), K("android/view/MotionEvent", "getAxisValue", "(I)F", ME_getAxisValue1),
    K("android/view/MotionEvent", "isFromSource", "(I)Z", ME_isTrue),
    K("android/view/MotionEvent", "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", ME_obtainCopy),
    K("android/view/MotionEvent", "recycle", "()V", ME_recycle),
    { NULL, NULL, NULL, NULL }
};

void tl_input_install(void)
{
    tl_jni_declare("android/view/InputEvent", "java/lang/Object");
    tl_jni_declare("android/view/MotionEvent", "android/view/InputEvent");
    tl_jni_declare("android/view/KeyEvent", "android/view/InputEvent");
    tl_jni_register_hle(k_input_hle);
}
