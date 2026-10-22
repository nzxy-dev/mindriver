#ifndef MINDRIVER_COMMON_H
#define MINDRIVER_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define MINDRIVER_VERSION "1.0.0"
#define MINDRIVER_PUBLISHER "nzxy project"
#define MINDRIVER_MAX_THREADS 64
#define MINDRIVER_MAX_PATH_LEN 512
#define MINDRIVER_MAX_ID_LEN 64
#define MINDRIVER_PIPE_NAME "\\\\.\\pipe\\mindriver_ipc"
#define MINDRIVER_SOCKET_PATH "/tmp/mindriver.sock"
#define MINDRIVER_IPC_BUFFER_SIZE 1024

typedef enum {
    END_ACTION_STOP_AT_END = 0,
    END_ACTION_DELETE_AT_END = 1,
    END_ACTION_LOOP_AT_END_BASE = 100
} EndAction;

typedef struct {
    float* samples;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t total_frames;
} AudioBuffer;

typedef struct {
    char id[MINDRIVER_MAX_ID_LEN];
    char filepath[MINDRIVER_MAX_PATH_LEN];
    AudioBuffer audio;
    int loop;
    EndAction end_action;
    int loop_delay_seconds;
    int playing;
    int should_exit;
    int replay_requested;
    int active;
    uint64_t current_frame;
    void* os_handle;
} AudioThread;

#endif
