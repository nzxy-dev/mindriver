#ifndef MINDRIVER_LINUX_INITCORE_H
#define MINDRIVER_LINUX_INITCORE_H

#include "../common.h"

typedef enum {
    LINUX_AUDIO_NONE = 0,
    LINUX_AUDIO_PIPEWIRE = 1,
    LINUX_AUDIO_PULSEAUDIO = 2,
    LINUX_AUDIO_ALSA = 3
} LinuxAudioBackend;

int initcore_init(void);
void initcore_shutdown(void);
LinuxAudioBackend initcore_get_active_backend(void);
const char* initcore_get_backend_name(void);
void initcore_show_missing_alert(void);

#endif
