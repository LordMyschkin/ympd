#ifndef YMPD_ASHUFFLE_CONTROL_H
#define YMPD_ASHUFFLE_CONTROL_H

#include <stddef.h>

void ashuffle_init(const char *ympd_executable);
void ashuffle_tick(void);
void ashuffle_restart(void);
void ashuffle_shutdown(void);
int ashuffle_enabled(void);
size_t ashuffle_status_json(char *out, size_t size);
int ashuffle_apply_query(const char *query, char *error, size_t error_size);

#endif
