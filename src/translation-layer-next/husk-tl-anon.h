/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The table of anonymous mappings the guest made.
 *
 * madvise(MADV_DONTNEED) on Linux throws anonymous pages away so the next touch
 * finds zeros, and allocators lean on that. Darwin's keeps the contents, so the
 * bionic shim replaces the pages itself -- and for that it has to know which
 * ranges are anonymous. This is that record: what mmap made, less what munmap,
 * mremap and later mappings have taken back.
 *
 * Only the bookkeeping lives here, with no Mach in it, so it can be tested on any
 * host. Replacing the pages is husk-tl-bionic-io.c's.
 *
 * Every *_locked call is made between tl_anon_lock and tl_anon_unlock. A caller
 * that unmaps or maps over memory holds the lock across that and the table update
 * together; otherwise another thread can map the same addresses in between, record
 * them, and have its new entry forgotten.
 */
#ifndef HUSK_TL_ANON_H
#define HUSK_TL_ANON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tl_anon_lock(void);
void tl_anon_unlock(void);

/*
 * Record [addr, addr+len) as anonymous. It replaces only what it covers: one placed
 * inside a bigger region leaves the rest of that region recorded. Returns false when
 * the table cannot grow and something went unrecorded; MADV_DONTNEED on that keeps
 * its contents, as Darwin's does.
 */
bool tl_anon_add_locked(uintptr_t addr, size_t len);

/*
 * Forget [addr, addr+len) once it is unmapped, rounded out to whole host pages as
 * munmap rounds it. An entry it cuts through keeps the part on either side. Returns
 * false when the table cannot grow to hold both parts, and one went unrecorded.
 */
bool tl_anon_remove_locked(uintptr_t addr, size_t len);

/*
 * The recorded ranges that overlap [addr, end), one at a time and clipped to it.
 * Start with *i = 0; each call that returns true sets [*a, *e) and moves *i on.
 */
bool tl_anon_next_locked(int *i, uintptr_t addr, uintptr_t end, uintptr_t *a, uintptr_t *e);

#ifdef __cplusplus
}
#endif

#endif
