#ifndef KURONO_MAIN_H
#define KURONO_MAIN_H

/*
 * Version string reported by --version. plan.md VII gap 11 left the
 * value unspecified and phase 2 parked "0.1.0-dev" here; phase 18
 * closed the gap at 1.0.0, on the grounds that v1 is what M1 plus M2
 * was defined to be and both are complete. A -dev suffix on a finished
 * v1 would be the version string lying about the milestone.
 */
#define KURONO_VERSION "1.0.0"

/*
 * kurono_die: restore the terminal if it was claimed, print msg to
 * stderr, and exit with status 1. Never returns. Safe to call before
 * tb_init. For recoverable fatal startup/runtime errors only (alloc
 * failure, an unopenable database, a newer schema_version) -- it is not
 * a crash handler and must never be installed as a signal handler
 * (DESIGN.md IV.9.c).
 */
_Noreturn void kurono_die(const char * fmt, ...);

#endif // KURONO_MAIN_H
