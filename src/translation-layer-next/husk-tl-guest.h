/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Where a thread lands when the game ends under it.
 *
 * On Android a game that calls exit() or abort() ends its own process. Here the game shares a process with Husk,
 * so its end has to be confined to its own code: whatever guest frames are on the stack are abandoned, and the
 * thread goes back to the host code that called into the guest, as if that call had returned. Each place the host
 * calls guest code (a driver loop's native call, a callback, a constructor run at start-up) does so under a landing
 * pad: a sigjmp_buf on a per-thread stack. tl_guest_unwind, called by the app's exit hook, marks the game ended and
 * jumps to the outermost pad of the calling thread -- the host frame below which every frame is the guest's or the
 * runtime's on its behalf. Not the innermost: a pad nested inside guest code (a native the host runs from inside a
 * JNI call the guest made) would return into the guest frame that made the call, and the guest would carry on
 * running after its own exit.
 *
 * What the jump skips is never run: no destructor, no unlock. A guest that exits while holding one of its own
 * mutexes leaves it held, and its other threads block on it for the life of the process -- as harmless as anything
 * a dead game's threads do, since nothing of the host's waits on a guest lock. A runtime lock held across a call
 * into the guest is the real hazard (the linker's load lock is held while a library's constructors run, so a
 * constructor that exits leaves it held, and no further library can be loaded); host code that holds a lock of its
 * own across a guest call is not allowed for that reason, and the drivers release theirs before calling.
 *
 * Threads the guest started itself (pthread_create's shim) have no pad: below their outermost guest frame is only
 * the shim, so the exit hook ends them with pthread_exit, as before.
 *
 * Once the game has ended, a pad cannot be pushed: TL_GUEST_CALL and TL_JNI_NATIVE_CALL skip the call, and driver
 * loops test tl_guest_ended() to stop. A result the skipped call would have assigned keeps the value it had, so
 * callers initialise the variables such calls assign.
 */
#ifndef HUSK_TL_GUEST_H
#define HUSK_TL_GUEST_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tl_guest_pad {
    sigjmp_buf jb;
    struct tl_guest_pad *prev;              /* the pad this one is nested in, on the same thread */
    uint32_t jni_depth;                     /* the thread's JNI local frames when the pad was pushed */
    volatile int landed;                    /* set by tl_guest_unwind before it jumps here */
} tl_guest_pad;

bool tl_guest_ended(void);                  /* the game has exited: do not call into it again */
/* Push a pad for the calling thread; false (and nothing pushed) once the game has ended. */
bool tl_guest_pad_push(tl_guest_pad *p);
/* Pop it: after a landing, also releases the JNI locals made since the push. */
void tl_guest_pad_pop(tl_guest_pad *p);
/* Mark the game ended and, when the calling thread has a pad and is not one the guest started, jump to its outermost
 * pad. Returns only when there is none to jump to. */
void tl_guest_unwind(void);
/* Marks the calling thread as one the guest started (pthread_create's shim): pads on it are never landed on. */
void tl_guest_thread_mark(void);

/*
 * Run guest code under a landing pad. sigsetjmp has to be called from the frame that stays live, so this is a macro;
 * the signal mask is not saved, since nothing between the pad and the guest's exit changes it and saving it would
 * cost a system call per call into the guest.
 */
#define TL_GUEST_CALL(...) do { \
    tl_guest_pad tl_pad_; \
    if (tl_guest_pad_push(&tl_pad_)) { \
        if (!sigsetjmp(tl_pad_.jb, 0)) { __VA_ARGS__; } \
        tl_guest_pad_pop(&tl_pad_); \
    } \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_GUEST_H */
