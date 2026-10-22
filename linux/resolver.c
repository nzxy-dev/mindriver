#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include "resolver.h"

void resolver_sanitize_name(const char* input_path, char* out_id, size_t max_len) {
    if (!input_path || !out_id || max_len == 0) return;

    const char* base = input_path;
    const char* p = input_path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
        p++;
    }

    const char* last_dot = NULL;
    p = base;
    while (*p) {
        if (*p == '.') {
            last_dot = p;
        }
        p++;
    }

    size_t out_idx = 0;
    p = base;
    while (*p && (last_dot == NULL || p < last_dot)) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (out_idx + 1 < max_len) {
                out_id[out_idx++] = (char)tolower(c);
            }
        }
        p++;
    }

    if (out_idx == 0) {
        const char* def_name = "audio";
        while (*def_name && out_idx + 1 < max_len) {
            out_id[out_idx++] = *def_name++;
        }
    }
    out_id[out_idx] = '\0';
}

int resolver_resolve_path(const char* input_path, char* out_full_path, size_t max_len) {
    if (!input_path || !out_full_path || max_len == 0) return -1;

    const char* start = input_path;
    while (*start == ' ' || *start == '\"' || *start == '\'') start++;

    char temp[PATH_MAX];
    size_t i = 0;
    for (; start[i] && i + 1 < sizeof(temp); i++) {
        if (start[i] == '\"' || start[i] == '\'') break;
        temp[i] = start[i];
    }
    temp[i] = '\0';
    while (i > 0 && temp[i - 1] == ' ') {
        temp[--i] = '\0';
    }

    if (temp[0] == '\0') return -1;

    char resolved[PATH_MAX];
    if (realpath(temp, resolved) != NULL) {
        strncpy(out_full_path, resolved, max_len - 1);
        out_full_path[max_len - 1] = '\0';
        return 0;
    }

    if (access(temp, R_OK) == 0) {
        strncpy(out_full_path, temp, max_len - 1);
        out_full_path[max_len - 1] = '\0';
        return 0;
    }

    return -1;
}

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
} WavHeader;

typedef struct {
    char chunk_id[4];
    uint32_t chunk_size;
} WavChunkHeader;

typedef struct {
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} WavFmtChunk;
#pragma pack(pop)

int resolver_load_audio(const char* full_path, AudioBuffer* out_buffer) {
    if (!full_path || !out_buffer) return -1;
    memset(out_buffer, 0, sizeof(AudioBuffer));

    FILE* f = fopen(full_path, "rb");
    if (!f) return -1;

    WavHeader header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }

    if (memcmp(header.riff, "RIFF", 4) != 0 || memcmp(header.wave, "WAVE", 4) != 0) {
        fclose(f);
        return -1;
    }

    WavFmtChunk fmt;
    memset(&fmt, 0, sizeof(fmt));
    int has_fmt = 0;
    uint8_t* raw_data = NULL;
    uint32_t data_bytes = 0;

    WavChunkHeader chunk;
    while (fread(&chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
        if (memcmp(chunk.chunk_id, "fmt ", 4) == 0) {
            uint32_t read_sz = chunk.chunk_size < sizeof(fmt) ? chunk.chunk_size : sizeof(fmt);
            if (fread(&fmt, 1, read_sz, f) != read_sz) {
                fclose(f);
                return -1;
            }
            if (chunk.chunk_size > read_sz) {
                fseek(f, chunk.chunk_size - read_sz, SEEK_CUR);
            }
            has_fmt = 1;
        } else if (memcmp(chunk.chunk_id, "data", 4) == 0) {
            data_bytes = chunk.chunk_size;
            raw_data = (uint8_t*)malloc(data_bytes);
            if (!raw_data) {
                fclose(f);
                return -1;
            }
            if (fread(raw_data, 1, data_bytes, f) != data_bytes) {
                free(raw_data);
                fclose(f);
                return -1;
            }
            break;
        } else {
            fseek(f, chunk.chunk_size, SEEK_CUR);
        }
    }
    fclose(f);

    if (!has_fmt || !raw_data || data_bytes == 0) {
        if (raw_data) free(raw_data);
        return -1;
    }

    uint32_t bytes_per_sample = fmt.bits_per_sample / 8;
    if (bytes_per_sample == 0) bytes_per_sample = 2;
    uint32_t frame_size = fmt.channels * bytes_per_sample;
    if (frame_size == 0) {
        free(raw_data);
        return -1;
    }

    uint64_t total_frames = data_bytes / frame_size;
    float* stereo_samples = (float*)malloc((size_t)total_frames * 2 * sizeof(float));
    if (!stereo_samples) {
        free(raw_data);
        return -1;
    }

    for (uint64_t i = 0; i < total_frames; i++) {
        float left = 0.0f;
        float right = 0.0f;
        uint8_t* frame_ptr = raw_data + (i * frame_size);

        if (fmt.format_type == 1) {
            if (fmt.bits_per_sample == 16) {
                int16_t* s16 = (int16_t*)frame_ptr;
                left = s16[0] / 32768.0f;
                right = (fmt.channels > 1) ? s16[1] / 32768.0f : left;
            } else if (fmt.bits_per_sample == 8) {
                left = (frame_ptr[0] - 128) / 128.0f;
                right = (fmt.channels > 1) ? (frame_ptr[1] - 128) / 128.0f : left;
            } else if (fmt.bits_per_sample == 24) {
                int32_t val = (frame_ptr[0] | (frame_ptr[1] << 8) | (frame_ptr[2] << 16));
                if (val & 0x800000) val |= 0xFF000000;
                left = val / 8388608.0f;
                if (fmt.channels > 1) {
                    uint8_t* rptr = frame_ptr + 3;
                    int32_t rval = (rptr[0] | (rptr[1] << 8) | (rptr[2] << 16));
                    if (rval & 0x800000) rval |= 0xFF000000;
                    right = rval / 8388608.0f;
                } else {
                    right = left;
                }
            } else if (fmt.bits_per_sample == 32) {
                int32_t* s32 = (int32_t*)frame_ptr;
                left = s32[0] / 2147483648.0f;
                right = (fmt.channels > 1) ? s32[1] / 2147483648.0f : left;
            }
        } else if (fmt.format_type == 3) {
            float* sf = (float*)frame_ptr;
            left = sf[0];
            right = (fmt.channels > 1) ? sf[1] : left;
        }

        stereo_samples[i * 2 + 0] = left;
        stereo_samples[i * 2 + 1] = right;
    }

    free(raw_data);
    out_buffer->samples = stereo_samples;
    out_buffer->sample_rate = fmt.sample_rate;
    out_buffer->channels = 2;
    out_buffer->total_frames = total_frames;
    return 0;
}

void resolver_free_audio(AudioBuffer* buffer) {
    if (buffer && buffer->samples) {
        free(buffer->samples);
        buffer->samples = NULL;
        buffer->total_frames = 0;
    }
}
