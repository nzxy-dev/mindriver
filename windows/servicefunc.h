#ifndef MINDRIVER_WINDOWS_SERVICEFUNC_H
#define MINDRIVER_WINDOWS_SERVICEFUNC_H

#include "../common.h"

void servifunc_init(void);
int servifunc_create(const char* id, const char* filepath, const AudioBuffer* buffer, int loop, EndAction end_action, int loop_delay_sec);
int servifunc_stop(const char* id);
int servifunc_replay(const char* id);
int servifunc_delete(const char* id);
int servifunc_list(char* out_str, size_t max_len);
int servifunc_status(const char* id, char* out_str, size_t max_len);
int servifunc_count_active(void);
void servifunc_stop_all(void);

#endif
