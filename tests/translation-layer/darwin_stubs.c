/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-ins for the JIT-region symbols the device probe references when built
 * on macOS (it compiles its __APPLE__ paths there, unlike on Linux). The host
 * tests have no StikDebug region, so both report "none". */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "husk-tl-internal.h"

tl_dual_mapping *tl_find_stikdebug_prewarmed(void) { return NULL; }
bool tl_jit_carve(size_t bytes, uint8_t **rx, uint8_t **rw)
{
    (void)bytes; (void)rx; (void)rw;
    return false;
}
