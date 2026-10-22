#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "servicefunc.h"
#include "resolver.h"

static CRITICAL_SECTION g_cs;
static int g_cs_initialized = 0;
static AudioThread g_threads[MINDRIVER_MAX_THREADS];

void servifunc_init(void) {
    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_cs);
        g_cs_initialized = 1;
    }
    EnterCriticalSection(&g_cs);
    memset(g_threads, 0, sizeof(g_threads));
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI AudioPlaybackThreadProc(LPVOID lpParam) {
    AudioThread* slot = (AudioThread*)lpParam;
    if (!slot) return 1;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    IAudioRenderClient* pRenderClient = NULL;
    WAVEFORMATEX* pwfx = NULL;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) {
        printf("servicefunc: CoCreateInstance failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        printf("servicefunc: GetDefaultAudioEndpoint failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    hr = IMMDevice_Activate(pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
    if (FAILED(hr)) {
        printf("servicefunc: Activate failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    hr = IAudioClient_GetMixFormat(pAudioClient, &pwfx);
    if (FAILED(hr)) {
        printf("servicefunc: GetMixFormat failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    REFERENCE_TIME hnsDefaultPeriod = 0;
    IAudioClient_GetDevicePeriod(pAudioClient, &hnsDefaultPeriod, NULL);
    hr = IAudioClient_Initialize(pAudioClient, AUDCLNT_SHAREMODE_SHARED, 0, hnsDefaultPeriod, 0, pwfx, NULL);
    if (FAILED(hr)) {
        printf("servicefunc: Initialize failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    UINT32 bufferFrameCount = 0;
    hr = IAudioClient_GetBufferSize(pAudioClient, &bufferFrameCount);
    if (FAILED(hr)) {
        printf("servicefunc: GetBufferSize failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    hr = IAudioClient_GetService(pAudioClient, &IID_IAudioRenderClient, (void**)&pRenderClient);
    if (FAILED(hr)) {
        printf("servicefunc: GetService failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    hr = IAudioClient_Start(pAudioClient);
    if (FAILED(hr)) {
        printf("servicefunc: Start failed (0x%08lX)\n", (unsigned long)hr);
        goto cleanup_exit;
    }

    double sourceRate = (double)slot->audio.sample_rate;
    double targetRate = (double)pwfx->nSamplesPerSec;
    double rateRatio = (targetRate > 0) ? (sourceRate / targetRate) : 1.0;
    double currentPos = 0.0;
    int isFloat = (pwfx->wBitsPerSample == 32);

    while (1) {
        EnterCriticalSection(&g_cs);
        int should_exit = slot->should_exit;
        int replay = slot->replay_requested;
        int playing = slot->playing;
        LeaveCriticalSection(&g_cs);

        if (should_exit) break;

        if (replay) {
            currentPos = 0.0;
            EnterCriticalSection(&g_cs);
            slot->current_frame = 0;
            slot->replay_requested = 0;
            slot->playing = 1;
            playing = 1;
            LeaveCriticalSection(&g_cs);
        }

        if (!playing) {
            Sleep(20);
            continue;
        }

        UINT32 paddingFrames = 0;
        hr = IAudioClient_GetCurrentPadding(pAudioClient, &paddingFrames);
        if (FAILED(hr)) break;

        UINT32 framesAvailable = bufferFrameCount - paddingFrames;
        if (framesAvailable == 0) {
            Sleep(5);
            continue;
        }

        BYTE* pData = NULL;
        hr = IAudioRenderClient_GetBuffer(pRenderClient, framesAvailable, &pData);
        if (FAILED(hr)) break;

        UINT32 framesWritten = framesAvailable;
        int reached_end = 0;

        for (UINT32 f = 0; f < framesAvailable; f++) {
            uint64_t frameIdx = (uint64_t)currentPos;
            if (frameIdx >= slot->audio.total_frames) {
                reached_end = 1;
                framesWritten = f;
                break;
            }

            float left = 0.0f;
            float right = 0.0f;
            double frac = currentPos - (double)frameIdx;

            if (frameIdx + 1 < slot->audio.total_frames) {
                float l0 = slot->audio.samples[frameIdx * 2 + 0];
                float r0 = slot->audio.samples[frameIdx * 2 + 1];
                float l1 = slot->audio.samples[(frameIdx + 1) * 2 + 0];
                float r1 = slot->audio.samples[(frameIdx + 1) * 2 + 1];
                left = (float)(l0 + frac * (l1 - l0));
                right = (float)(r0 + frac * (r1 - r0));
            } else {
                left = slot->audio.samples[frameIdx * 2 + 0];
                right = slot->audio.samples[frameIdx * 2 + 1];
            }

            if (isFloat) {
                float* out_f = (float*)(pData + (f * pwfx->nBlockAlign));
                out_f[0] = left;
                if (pwfx->nChannels > 1) out_f[1] = right;
                for (UINT32 c = 2; c < pwfx->nChannels; c++) {
                    out_f[c] = 0.0f;
                }
            } else {
                int16_t* out_s = (int16_t*)(pData + (f * pwfx->nBlockAlign));
                int32_t il = (int32_t)(left * 32767.0f);
                int32_t ir = (int32_t)(right * 32767.0f);
                if (il > 32767) il = 32767; else if (il < -32768) il = -32768;
                if (ir > 32767) ir = 32767; else if (ir < -32768) ir = -32768;
                out_s[0] = (int16_t)il;
                if (pwfx->nChannels > 1) out_s[1] = (int16_t)ir;
                for (UINT32 c = 2; c < pwfx->nChannels; c++) {
                    out_s[c] = 0;
                }
            }

            currentPos += rateRatio;
        }

        if (framesWritten < framesAvailable) {
            memset(pData + (framesWritten * pwfx->nBlockAlign), 0, (framesAvailable - framesWritten) * pwfx->nBlockAlign);
        }

        IAudioRenderClient_ReleaseBuffer(pRenderClient, framesAvailable, 0);

        EnterCriticalSection(&g_cs);
        slot->current_frame = (uint64_t)currentPos;
        LeaveCriticalSection(&g_cs);

        if (reached_end) {
            EnterCriticalSection(&g_cs);
            int loop = slot->loop;
            EndAction end_act = slot->end_action;
            int delay_sec = slot->loop_delay_seconds;
            LeaveCriticalSection(&g_cs);

            if (loop) {
                currentPos = 0.0;
                EnterCriticalSection(&g_cs);
                slot->current_frame = 0;
                LeaveCriticalSection(&g_cs);
            } else if (end_act == END_ACTION_STOP_AT_END) {
                EnterCriticalSection(&g_cs);
                slot->playing = 0;
                LeaveCriticalSection(&g_cs);
            } else if (end_act == END_ACTION_DELETE_AT_END) {
                EnterCriticalSection(&g_cs);
                slot->playing = 0;
                slot->should_exit = 1;
                LeaveCriticalSection(&g_cs);
                break;
            } else if (delay_sec > 0) {
                // loopatend<N>: pause N seconds, then set loop=1
                EnterCriticalSection(&g_cs);
                slot->playing = 0;
                LeaveCriticalSection(&g_cs);

                int wait_steps = delay_sec * 10;
                for (int w = 0; w < wait_steps; w++) {
                    Sleep(100);
                    EnterCriticalSection(&g_cs);
                    int abort = slot->should_exit;
                    LeaveCriticalSection(&g_cs);
                    if (abort) break;
                }

                currentPos = 0.0;
                EnterCriticalSection(&g_cs);
                slot->loop = 1;
                slot->current_frame = 0;
                slot->playing = 1;
                LeaveCriticalSection(&g_cs);
            }
        }

        Sleep(10);
    }

cleanup_exit:
    if (pAudioClient) {
        IAudioClient_Stop(pAudioClient);
        IAudioClient_Release(pAudioClient);
    }
    if (pRenderClient) IAudioRenderClient_Release(pRenderClient);
    if (pDevice) IMMDevice_Release(pDevice);
    if (pEnumerator) IMMDeviceEnumerator_Release(pEnumerator);
    if (pwfx) CoTaskMemFree(pwfx);
    CoUninitialize();

    EnterCriticalSection(&g_cs);
    resolver_free_audio(&slot->audio);
    slot->active = 0;
    slot->playing = 0;
    slot->os_handle = NULL;
    LeaveCriticalSection(&g_cs);

    return 0;
}

int servifunc_create(const char* id, const char* filepath, const AudioBuffer* buffer, int loop, EndAction end_action, int loop_delay_sec) {
    if (!id || !filepath || !buffer) return -1;
    if (!g_cs_initialized) servifunc_init();

    EnterCriticalSection(&g_cs);

    // check exist
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].loop = loop;
            g_threads[i].end_action = end_action;
            g_threads[i].loop_delay_seconds = loop_delay_sec;
            g_threads[i].replay_requested = 1;
            g_threads[i].playing = 1;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
    }

    // find free slots
    int slot_idx = -1;
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (!g_threads[i].active) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx < 0) {
        LeaveCriticalSection(&g_cs);
        return -1;
    }

    AudioThread* slot = &g_threads[slot_idx];
    memset(slot, 0, sizeof(AudioThread));
    strncpy(slot->id, id, sizeof(slot->id) - 1);
    strncpy(slot->filepath, filepath, sizeof(slot->filepath) - 1);
    slot->audio.sample_rate = buffer->sample_rate;
    slot->audio.channels = buffer->channels;
    slot->audio.total_frames = buffer->total_frames;
    size_t sample_count = (size_t)buffer->total_frames * buffer->channels;
    slot->audio.samples = (float*)malloc(sample_count * sizeof(float));
    if (!slot->audio.samples) {
        LeaveCriticalSection(&g_cs);
        return -1;
    }
    memcpy(slot->audio.samples, buffer->samples, sample_count * sizeof(float));
    slot->loop = loop;
    slot->end_action = end_action;
    slot->loop_delay_seconds = loop_delay_sec;
    slot->playing = 1;
    slot->should_exit = 0;
    slot->replay_requested = 0;
    slot->current_frame = 0;
    slot->active = 1;
    DWORD threadId = 0;
    HANDLE hThread = CreateThread(NULL, 0, AudioPlaybackThreadProc, slot, 0, &threadId);
    if (!hThread) {
        free(slot->audio.samples);
        slot->audio.samples = NULL;
        slot->active = 0;
        LeaveCriticalSection(&g_cs);
        return -1;
    }
    slot->os_handle = (void*)hThread;
    LeaveCriticalSection(&g_cs);
    return 0;
}
int servifunc_stop(const char* id) {
    if (!id || !g_cs_initialized) return -1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].playing = 0;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
    }
    LeaveCriticalSection(&g_cs);
    return -1;
}

int servifunc_replay(const char* id) {
    if (!id || !g_cs_initialized) return -1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].replay_requested = 1;
            g_threads[i].playing = 1;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
    }
    LeaveCriticalSection(&g_cs);
    return -1;
}

int servifunc_delete(const char* id) {
    if (!id || !g_cs_initialized) return -1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].should_exit = 1;
            g_threads[i].playing = 0;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
    }
    LeaveCriticalSection(&g_cs);
    return -1;
}

int servifunc_status(const char* id, char* out_str, size_t max_len) {
    if (!id || !out_str || max_len == 0 || !g_cs_initialized) return -1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            const char* loop_str = g_threads[i].loop ? "loop" : "notloop";
            char end_str[32];
            if (g_threads[i].end_action == END_ACTION_STOP_AT_END) {
                strncpy(end_str, "stopatend", sizeof(end_str));
            } else if (g_threads[i].end_action == END_ACTION_DELETE_AT_END) {
                strncpy(end_str, "deleteatend", sizeof(end_str));
            } else if (g_threads[i].loop_delay_seconds > 0) {
                snprintf(end_str, sizeof(end_str), "loopatend%d", g_threads[i].loop_delay_seconds);
            } else {
                strncpy(end_str, "stopatend", sizeof(end_str));
            }

            //getfilename
            const char* fname = g_threads[i].filepath;
            for (const char* p = g_threads[i].filepath; *p; p++) {
                if (*p == '/' || *p == '\\') fname = p + 1;
            }

            snprintf(out_str, max_len, "%s , %s , %s , playing=%s",
                     fname, loop_str, end_str, g_threads[i].playing ? "true" : "false");
            LeaveCriticalSection(&g_cs);
            return 0;
        }
    }
    LeaveCriticalSection(&g_cs);
    return -1;
}

int servifunc_list(char* out_str, size_t max_len) {
    if (!out_str || max_len == 0) return 0;
    out_str[0] = '\0';
    if (!g_cs_initialized) return 0;

    EnterCriticalSection(&g_cs);
    size_t offset = 0;
    int count = 0;

    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active) {
            const char* loop_str = g_threads[i].loop ? "loop" : "notloop";
            char end_str[32];
            if (g_threads[i].end_action == END_ACTION_STOP_AT_END) {
                strncpy(end_str, "stopatend", sizeof(end_str));
            } else if (g_threads[i].end_action == END_ACTION_DELETE_AT_END) {
                strncpy(end_str, "deleteatend", sizeof(end_str));
            } else if (g_threads[i].loop_delay_seconds > 0) {
                snprintf(end_str, sizeof(end_str), "loopatend%d", g_threads[i].loop_delay_seconds);
            } else {
                strncpy(end_str, "stopatend", sizeof(end_str));
            }

            const char* fname = g_threads[i].filepath;
            for (const char* p = g_threads[i].filepath; *p; p++) {
                if (*p == '/' || *p == '\\') fname = p + 1;
            }

            char line[256];
            int written = snprintf(line, sizeof(line), "%s , %s , %s , playing=%s\n",
                                   fname, loop_str, end_str, g_threads[i].playing ? "true" : "false");
            if (written > 0 && offset + (size_t)written < max_len) {
                memcpy(out_str + offset, line, (size_t)written);
                offset += (size_t)written;
                out_str[offset] = '\0';
            }
            count++;
        }
    }
    LeaveCriticalSection(&g_cs);
    return count;
}

int servifunc_count_active(void) {
    if (!g_cs_initialized) return 0;
    EnterCriticalSection(&g_cs);
    int count = 0;
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active) count++;
    }
    LeaveCriticalSection(&g_cs);
    return count;
}

void servifunc_stop_all(void) {
    if (!g_cs_initialized) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active) {
            g_threads[i].should_exit = 1;
            g_threads[i].playing = 0;
        }
    }
    LeaveCriticalSection(&g_cs);

    Sleep(150);
}
