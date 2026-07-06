/* recovery.h -- the bad-LOAD recovery router (survive -> steer the editor back to the My-Maps browser). */
#ifndef SHIELD_RECOVERY_H
#define SHIELD_RECOVERY_H

int  recovery_install(void);   /* detour the (collision-free) frame pump; returns 1 on success */
void recovery_arm(void);       /* the VEH calls this after surviving a bad-LOAD fault */
void notice_request(void);     /* the VEH calls this on a Class-A revert -> frame-hook raises the generic toast */
void notice_request_msg(void); /* Class-B/Error(6): toast carries the engine's harvested error text if present */
void log_engine_error_text(void); /* the VEH's C++-throw path calls this to record a masked FatalError's verbatim text */

#endif /* SHIELD_RECOVERY_H */
