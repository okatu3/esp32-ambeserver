#pragma once
/* A test M17 reflector inside the program (127.0.0.1 only, no internet traffic): answers CONN with ACKN, PINGs every
 * 3 s, relays streams to the other linked clients of the same module and, like a parrot, sends each stream back to
 * its sender 1 s after it ended (same 40 ms pace, a new stream id). Runs in its own thread. */
#include <stdbool.h>
#include <stdint.h>

#define PARROT_PORT 17000

typedef void (*parrot_log_fn)(const char *line);

bool parrot_start(uint16_t port, parrot_log_fn log);   /* false if the port is taken */
void parrot_stop(void);
bool parrot_running(void);
