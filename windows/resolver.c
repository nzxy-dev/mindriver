#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "resolver.h"

#if defined(_MSC_VER)
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif

void resolver_sanitize_name(const char* input_path, char* out_id, size_t max_len) {
    if (!input_path || !out_id || max_len == 0) return;

    // extract name (base)
    const char* base = input_path;
    const char* p = input_path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
        p++;
    }

    //find last dot (fro file extensions)
    const char* last_dot = NULL;
    p = base;
    while (*p) {
        if (*p == '.') {
            last_dot = p;
        }
        p++;
    }

    //filter alphanumeric chars only
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

    //fallback
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

    char temp[MINDRIVER_MAX_PATH_LEN];
    size_t i = 0;
    for (; start[i] && i + 1 < sizeof(temp); i++) {
        if (start[i] == '\"' || start[i] == '\'') break;
        temp[i] = (start[i] == '/') ? '\\' : start[i];
    }
    temp[i] = '\0';
    while (i > 0 && temp[i - 1] == ' ') {
        temp[--i] = '\0';
    }

    if (temp[0] == '\0') return -1;

    DWORD ret = GetFullPathNameA(temp, (DWORD)max_len, out_full_path, NULL);
    if (ret == 0 || ret >= max_len) {
        strncpy(out_full_path, temp, max_len - 1);
        out_full_path[max_len - 1] = '\0';
    }

    DWORD attr = GetFileAttributesA(out_full_path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        DWORD attr_temp = GetFileAttributesA(temp);
        if (attr_temp != INVALID_FILE_ATTRIBUTES && !(attr_temp & FILE_ATTRIBUTE_DIRECTORY)) {
            strncpy(out_full_path, temp, max_len - 1);
            out_full_path[max_len - 1] = '\0';
            return 0;
        }
        return -1;
    }
    return 0;
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

static int load_wav_file(const char* full_path, AudioBuffer* out_buffer) {
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

    // converto32bits float (sterio)
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

        if (fmt.format_type == 1) { // pcm
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
        } else if (fmt.format_type == 3) { // IEEE Float 
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

#if defined(_MSC_VER)
static int load_wmf_file(const char* full_path, AudioBuffer* out_buffer) {
    HRESULT hr;
    IMFSourceReader* pReader = NULL;
    IMFMediaType* pPartialType = NULL;
    IMFMediaType* pUncompressedAudioType = NULL;

    wchar_t wpath[MINDRIVER_MAX_PATH_LEN];
    if (MultiByteToWideChar(CP_ACP, 0, full_path, -1, wpath, MINDRIVER_MAX_PATH_LEN) <= 0) {
        return -1;
    }

    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    hr = MFCreateSourceReaderFromURL(wpath, NULL, &pReader);
    if (FAILED(hr)) {
        MFShutdown();
        return -1;
    }
    hr = IMFSourceReader_SetStreamSelection(pReader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = IMFSourceReader_SetStreamSelection(pReader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    hr = MFCreateMediaType(&pPartialType);
    if (FAILED(hr)) {
        IMFSourceReader_Release(pReader);
        MFShutdown();
        return -1;
    }

    hr = IMFMediaType_SetGUID(pPartialType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    hr = IMFMediaType_SetGUID(pPartialType, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    hr = IMFSourceReader_SetCurrentMediaType(pReader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pPartialType);
    IMFMediaType_Release(pPartialType);

    if (FAILED(hr)) {
        IMFSourceReader_Release(pReader);
        MFShutdown();
        return -1;
    }

    hr = IMFSourceReader_GetCurrentMediaType(pReader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pUncompressedAudioType);
    if (FAILED(hr)) {
        IMFSourceReader_Release(pReader);
        MFShutdown();
        return -1;
    }

    UINT32 channels = 2;
    UINT32 sampleRate = 44100;
    IMFMediaType_GetUINT32(pUncompressedAudioType, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    IMFMediaType_GetUINT32(pUncompressedAudioType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    IMFMediaType_Release(pUncompressedAudioType);

    // read audio samples ALL
    size_t capacity = 1024 * 1024;
    size_t total_samples = 0;
    float* all_samples = (float*)malloc(capacity * sizeof(float));
    if (!all_samples) {
        IMFSourceReader_Release(pReader);
        MFShutdown();
        return -1;
    }

    while (1) {
        DWORD flags = 0;
        IMFSample* pSample = NULL;
        LONGLONG llTimestamp = 0;
        hr = IMFSourceReader_ReadSample(pReader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &llTimestamp, &pSample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (pSample) IMFSample_Release(pSample);
            break;
        }

        if (pSample) {
            IMFMediaBuffer* pMediaBuffer = NULL;
            hr = IMFSample_ConvertToContiguousBuffer(pSample, &pMediaBuffer);
            if (SUCCEEDED(hr)) {
                BYTE* pAudioData = NULL;
                DWORD cbBuffer = 0;
                hr = IMFMediaBuffer_Lock(pMediaBuffer, &pAudioData, NULL, &cbBuffer);
                if (SUCCEEDED(hr)) {
                    size_t float_count = cbBuffer / sizeof(float);
                    if (total_samples + float_count > capacity) {
                        capacity = (capacity + float_count) * 2;
                        float* resized = (float*)realloc(all_samples, capacity * sizeof(float));
                        if (resized) {
                            all_samples = resized;
                        }
                    }
                    if (all_samples && total_samples + float_count <= capacity) {
                        memcpy(all_samples + total_samples, pAudioData, float_count * sizeof(float));
                        total_samples += float_count;
                    }
                    IMFMediaBuffer_Unlock(pMediaBuffer);
                }
                IMFMediaBuffer_Release(pMediaBuffer);
            }
            IMFSample_Release(pSample);
        }
    }
    IMFSourceReader_Release(pReader);
    MFShutdown();
    if (total_samples == 0 || !all_samples) {
        if (all_samples) free(all_samples);
        return -1;
    }
    // convert mono to stereo
    if (channels == 1) {
        float* stereo = (float*)malloc(total_samples * 2 * sizeof(float));
        if (stereo) {
            for (size_t s = 0; s < total_samples; s++) {
                stereo[s * 2 + 0] = all_samples[s];
                stereo[s * 2 + 1] = all_samples[s];
            }
            free(all_samples);
            out_buffer->samples = stereo;
            out_buffer->sample_rate = sampleRate;
            out_buffer->channels = 2;
            out_buffer->total_frames = total_samples;
            return 0;
        }
    }

    out_buffer->samples = all_samples;
    out_buffer->sample_rate = sampleRate;
    out_buffer->channels = channels;
    out_buffer->total_frames = total_samples / channels;
    return 0;
}
#endif

int resolver_load_audio(const char* full_path, AudioBuffer* out_buffer) {
    if (!full_path || !out_buffer) return -1;
    memset(out_buffer, 0, sizeof(AudioBuffer));

    //try wav loader native
    if (load_wav_file(full_path, out_buffer) == 0) {
        return 0;
    }

#if defined(_MSC_VER)
    // try media foundry
    if (load_wmf_file(full_path, out_buffer) == 0) {
        return 0;
    }
#endif

    return -1;
}

void resolver_free_audio(AudioBuffer* buffer) {
    if (buffer && buffer->samples) {
        free(buffer->samples);
        buffer->samples = NULL;
        buffer->total_frames = 0;
    }
}
