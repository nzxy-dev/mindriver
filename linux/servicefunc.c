#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "servicefunc.h"
#include "resolver.h"
#include "initcore.h"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static AudioThread g_threads[MINDRIVER_MAX_THREADS];
static int g_initialized = 0;

static int16_t sample_to_s16(float sample) {
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    if (sample <= -1.0f) return INT16_MIN;
    return (int16_t)(sample * 32767.0f);
}

void servifunc_init(void) {
    pthread_mutex_lock(&g_mutex);
    memset(g_threads, 0, sizeof(g_threads));
    g_initialized = 1;
    pthread_mutex_unlock(&g_mutex);
}

static void* AudioPlaybackThreadProc(void* arg) {
    AudioThread* slot = (AudioThread*)arg;
    if (!slot) return NULL;

    snd_pcm_t* pcm = NULL;
    int result = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
        fprintf(stderr, "servicefunc: ALSA open failed: %s\n", snd_strerror(result));
        goto cleanup;
    }
    result = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, slot->audio.sample_rate, 1, 100000);
    if (result < 0) {
        fprintf(stderr, "servicefunc: ALSA setup failed: %s\n", snd_strerror(result));
        goto cleanup;
    }

    snd_pcm_uframes_t frames_per_chunk = (slot->audio.sample_rate * 20) / 1000;
    if (frames_per_chunk == 0) frames_per_chunk = 1;
    int16_t* output = (int16_t*)malloc((size_t)frames_per_chunk * 2 * sizeof(int16_t));
    if (!output) {
        fprintf(stderr, "servicefunc: failed to allocate ALSA output buffer\n");
        goto cleanup;
    }
    uint64_t current_frame = 0;
    int pcm_paused = 0;

    while (1) {
        pthread_mutex_lock(&g_mutex);
        int should_exit = slot->should_exit;
        int replay = slot->replay_requested;
        int playing = slot->playing;
        pthread_mutex_unlock(&g_mutex);

        if (should_exit) break;

        if (replay) {
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
            pcm_paused = 0;
            current_frame = 0;
            pthread_mutex_lock(&g_mutex);
            slot->current_frame = 0;
            slot->replay_requested = 0;
            slot->playing = 1;
            playing = 1;
            pthread_mutex_unlock(&g_mutex);
        }

        if (!playing) {
            if (!pcm_paused) {
                snd_pcm_drop(pcm);
                pcm_paused = 1;
            }
            usleep(20000);
            continue;
        }

        if (pcm_paused) {
            result = snd_pcm_prepare(pcm);
            if (result < 0) {
                fprintf(stderr, "servicefunc: ALSA resume failed: %s\n", snd_strerror(result));
                break;
            }
            pcm_paused = 0;
        }

        if (current_frame >= slot->audio.total_frames) {
            current_frame = 0;
        }
        uint64_t remaining = slot->audio.total_frames - current_frame;
        snd_pcm_uframes_t frames_to_write = remaining < frames_per_chunk
            ? (snd_pcm_uframes_t)remaining : frames_per_chunk;
        for (snd_pcm_uframes_t frame = 0; frame < frames_to_write; frame++) {
            size_t sample = (size_t)(current_frame + frame) * slot->audio.channels;
            output[frame * 2] = sample_to_s16(slot->audio.samples[sample]);
            output[frame * 2 + 1] = sample_to_s16(slot->audio.samples[sample + 1]);
        }

        snd_pcm_uframes_t written = 0;
        while (written < frames_to_write) {
            snd_pcm_sframes_t count = snd_pcm_writei(pcm, output + written * 2,
                                                      frames_to_write - written);
            if (count < 0) {
                result = snd_pcm_recover(pcm, (int)count, 1);
                if (result < 0) {
                    fprintf(stderr, "servicefunc: ALSA playback failed: %s\n", snd_strerror(result));
                    goto playback_done;
                }
                continue;
            }
            written += (snd_pcm_uframes_t)count;
        }
        current_frame += written;

        pthread_mutex_lock(&g_mutex);
        slot->current_frame = current_frame;
        int reached_end = (current_frame >= slot->audio.total_frames);
        int loop = slot->loop;
        EndAction end_act = slot->end_action;
        int delay_sec = slot->loop_delay_seconds;
        pthread_mutex_unlock(&g_mutex);

        if (reached_end) {
            if (loop) {
                current_frame = 0;
                pthread_mutex_lock(&g_mutex);
                slot->current_frame = 0;
                pthread_mutex_unlock(&g_mutex);
            } else if (end_act == END_ACTION_STOP_AT_END) {
                snd_pcm_drain(pcm);
                pthread_mutex_lock(&g_mutex);
                slot->playing = 0;
                pthread_mutex_unlock(&g_mutex);
            } else if (end_act == END_ACTION_DELETE_AT_END) {
                snd_pcm_drain(pcm);
                pthread_mutex_lock(&g_mutex);
                slot->playing = 0;
                slot->should_exit = 1;
                pthread_mutex_unlock(&g_mutex);
                break;
            } else if (delay_sec > 0) {
                snd_pcm_drain(pcm);
                pthread_mutex_lock(&g_mutex);
                slot->playing = 0;
                pthread_mutex_unlock(&g_mutex);

                int wait_steps = delay_sec * 10;
                for (int w = 0; w < wait_steps; w++) {
                    usleep(100000);
                    pthread_mutex_lock(&g_mutex);
                    int abort = slot->should_exit;
                    pthread_mutex_unlock(&g_mutex);
                    if (abort) break;
                }

                pthread_mutex_lock(&g_mutex);
                int abort = slot->should_exit;
                pthread_mutex_unlock(&g_mutex);
                if (abort) break;

                current_frame = 0;
                pthread_mutex_lock(&g_mutex);
                slot->loop = 1;
                slot->current_frame = 0;
                slot->playing = 1;
                pthread_mutex_unlock(&g_mutex);
            }
        }
    }

playback_done:
    free(output);
cleanup:
    if (pcm) snd_pcm_close(pcm);
    pthread_mutex_lock(&g_mutex);
    resolver_free_audio(&slot->audio);
    slot->active = 0;
    slot->playing = 0;
    slot->os_handle = NULL;
    pthread_mutex_unlock(&g_mutex);

    return NULL;
}

int servifunc_create(const char* id, const char* filepath, const AudioBuffer* buffer, int loop, EndAction end_action, int loop_delay_sec) {
    if (!id || !filepath || !buffer) return -1;
    if (!g_initialized) servifunc_init();

    pthread_mutex_lock(&g_mutex);

    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].loop = loop;
            g_threads[i].end_action = end_action;
            g_threads[i].loop_delay_seconds = loop_delay_sec;
            g_threads[i].replay_requested = 1;
            g_threads[i].playing = 1;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }

    int slot_idx = -1;
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (!g_threads[i].active) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx < 0) {
        pthread_mutex_unlock(&g_mutex);
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
        pthread_mutex_unlock(&g_mutex);
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

    pthread_attr_t thread_attr;
    if (pthread_attr_init(&thread_attr) != 0) {
        free(slot->audio.samples);
        slot->audio.samples = NULL;
        slot->active = 0;
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    int attr_result = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int thread_result = attr_result == 0
        ? pthread_create(&thread, &thread_attr, AudioPlaybackThreadProc, slot)
        : attr_result;
    pthread_attr_destroy(&thread_attr);
    if (thread_result != 0) {
        free(slot->audio.samples);
        slot->audio.samples = NULL;
        slot->active = 0;
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    slot->os_handle = NULL;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int servifunc_stop(const char* id) {
    if (!id || !g_initialized) return -1;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].playing = 0;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return -1;
}

int servifunc_replay(const char* id) {
    if (!id || !g_initialized) return -1;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].replay_requested = 1;
            g_threads[i].playing = 1;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return -1;
}

int servifunc_delete(const char* id) {
    if (!id || !g_initialized) return -1;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active && strcmp(g_threads[i].id, id) == 0) {
            g_threads[i].should_exit = 1;
            g_threads[i].playing = 0;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return -1;
}

int servifunc_status(const char* id, char* out_str, size_t max_len) {
    if (!id || !out_str || max_len == 0 || !g_initialized) return -1;
    pthread_mutex_lock(&g_mutex);
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

            const char* fname = g_threads[i].filepath;
            for (const char* p = g_threads[i].filepath; *p; p++) {
                if (*p == '/' || *p == '\\') fname = p + 1;
            }

            snprintf(out_str, max_len, "%s , %s , %s , playing=%s",
                     fname, loop_str, end_str, g_threads[i].playing ? "true" : "false");
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return -1;
}

int servifunc_list(char* out_str, size_t max_len) {
    if (!out_str || max_len == 0) return 0;
    out_str[0] = '\0';
    if (!g_initialized) return 0;

    pthread_mutex_lock(&g_mutex);
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
    pthread_mutex_unlock(&g_mutex);
    return count;
}

int servifunc_count_active(void) {
    if (!g_initialized) return 0;
    pthread_mutex_lock(&g_mutex);
    int count = 0;
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active) count++;
    }
    pthread_mutex_unlock(&g_mutex);
    return count;
}

void servifunc_stop_all(void) {
    if (!g_initialized) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MINDRIVER_MAX_THREADS; i++) {
        if (g_threads[i].active) {
            g_threads[i].should_exit = 1;
            g_threads[i].playing = 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);

    usleep(150000);
}
