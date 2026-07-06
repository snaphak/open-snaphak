/* palette_guard.h -- see palette_guard.c.
 *
 * Prevents a reload crash: on a full map-load the editor's entity palette is sorted by name, and after a
 * heavy edit session one palette entry can carry a stale (already-freed) name string. Copying that entry
 * during the sort dereferences the freed pointer and crashes. The guard sanitizes any such stale string in
 * place before the sort runs. Install once at startup after the module base is known. */
#ifndef B2_PALETTE_GUARD_H
#define B2_PALETTE_GUARD_H

#include <stdint.h>

/* Install the reload-crash guard: detours the palette-migration entry so any dangling name string in the
 * entity palette is reset to empty BEFORE the entry-time name sort copies it. module_base = DOOMx64vk image
 * base. Idempotent (one-shot latch). Returns 1 if armed, 0 on failure. */
int sh_palette_guard_install(const uint8_t *module_base);

#endif /* B2_PALETTE_GUARD_H */
