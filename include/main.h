#ifndef KURONO_MAIN_H
#define KURONO_MAIN_H

/*
 * Version string reported by --version. DESIGN.md does not specify a
 * value (plan.md VII gap 11) -- plan.md phase 2 settles it here.
 */
#define KURONO_VERSION "0.1.0-dev"

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
