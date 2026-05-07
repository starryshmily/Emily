#ifndef LOG_STORE_H
#define LOG_STORE_H

#include <stdbool.h>
#include <stddef.h>

void log_store_init(void);
void log_store_enable(bool enabled);
bool log_store_is_enabled(void);
void log_store_clear(void);
size_t log_store_snapshot(char *out, size_t out_len);

#endif // LOG_STORE_H
