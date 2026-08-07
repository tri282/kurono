#include "../include/main.h"

#include <stdio.h>
#include <string.h>

/*
 * Entry point. Handles --version by printing "kurono <version>" to
 * stdout and returning 0. Any other invocation (including no
 * arguments) prints a one-line "not implemented yet" notice to stderr
 * and returns 1 -- startup, wiring, and teardown land in later phases.
 * Takes the process argc/argv, returns the process exit status.
 */
int
main(int argc, char * argv[])
{
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("kurono %s\n", KURONO_VERSION);
    return 0;
  }

  fprintf(stderr, "kurono: not implemented yet\n");
  return 1;
} /* main */
