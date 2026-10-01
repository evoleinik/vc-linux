/* Linux-only native door containment. No environment variable disables it. */
#ifndef VC_DOOR_CONFINEMENT_H
#define VC_DOOR_CONFINEMENT_H

/* Called once, after H: is installed and before running any guest code. The
 * janitor must already have forked: it needs access to the session's parent.
 * Reports one clear diagnostic and returns -1 on failure. The test opt-out
 * tolerates an unavailable Landlock only; seccomp remains mandatory. */
int door_confine(int session_fd, int allow_unconfined);

#endif
