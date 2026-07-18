/* veh.h -- the fault-shield's vectored exception handler (raw AV -> recoverable Error(6)). */
#ifndef SHIELD_VEH_H
#define SHIELD_VEH_H

#include <windows.h>
#include <stddef.h>

int veh_install(void);   /* AddVectoredExceptionHandler(first=1); returns 1 on success. */

/* Walk a COPY of `ctx` with the OS unwinder into a compact "MOD+0xRVA <- ..." string. SEH-guarded per
 * frame (a wild frame ends the walk, never re-faults). Shared with the crash-record writer. */
void shield_capture_stack(const CONTEXT *ctx, char *out, size_t cap, int maxframes);

#endif /* SHIELD_VEH_H */
