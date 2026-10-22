#ifndef MINDRIVER_WINDOWS_RESOLVER_H
#define MINDRIVER_WINDOWS_RESOLVER_H
#include "../common.h"
void resolver_sanitize_name(const char* input_path, char* out_id, size_t max_len);
int resolver_resolve_path(const char* input_path, char* out_full_path, size_t max_len);
int resolver_load_audio(const char* full_path, AudioBuffer* out_buffer);
void resolver_free_audio(AudioBuffer* buffer);
#endif
