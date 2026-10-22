#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <alsa/asoundlib.h>
#include "initcore.h"

static LinuxAudioBackend g_active_backend = LINUX_AUDIO_NONE;

int initcore_init(void) {
    snd_pcm_t* pcm = NULL;
    int result = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
        fprintf(stderr, "initcore [linux]: ALSA playback unavailable: %s\n", snd_strerror(result));
        g_active_backend = LINUX_AUDIO_NONE;
        return -1;
    }

    snd_pcm_close(pcm);
    g_active_backend = LINUX_AUDIO_ALSA;
    printf("initcore [linux]: active backend -> alsa\n");
    return 0;
}

void initcore_shutdown(void) {
    g_active_backend = LINUX_AUDIO_NONE;
    printf("initcore [linux]: audio core closed\n");
}

LinuxAudioBackend initcore_get_active_backend(void) {
    return g_active_backend;
}

void initcore_show_missing_alert(void) {
    typedef int (*GtkInitCheck)(int*, char***);
    typedef void* (*GtkMessageDialogNew)(void*, int, int, int, const char*, ...);
    typedef int (*GtkDialogRun)(void*);
    typedef void (*GtkWidgetDestroy)(void*);
    typedef void (*GtkWindowSetTitle)(void*, const char*);

    void* gtk = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (gtk) {
        GtkInitCheck gtk_init_check = (GtkInitCheck)dlsym(gtk, "gtk_init_check");
        GtkMessageDialogNew gtk_message_dialog_new =
            (GtkMessageDialogNew)dlsym(gtk, "gtk_message_dialog_new");
        GtkDialogRun gtk_dialog_run = (GtkDialogRun)dlsym(gtk, "gtk_dialog_run");
        GtkWidgetDestroy gtk_widget_destroy = (GtkWidgetDestroy)dlsym(gtk, "gtk_widget_destroy");
        GtkWindowSetTitle gtk_window_set_title =
            (GtkWindowSetTitle)dlsym(gtk, "gtk_window_set_title");

        if (gtk_init_check && gtk_message_dialog_new && gtk_dialog_run &&
            gtk_widget_destroy && gtk_window_set_title && gtk_init_check(NULL, NULL)) {
            void* dialog = gtk_message_dialog_new(NULL, 0, 3, 2, "%s",
                                                   "Alsa not found install it");
            if (dialog) {
                gtk_window_set_title(dialog, "mindriver");
                gtk_dialog_run(dialog);
                gtk_widget_destroy(dialog);
                dlclose(gtk);
                return;
            }
        }
        dlclose(gtk);
    }

    fprintf(stderr, "Alsa not found install it\n");
}

const char* initcore_get_backend_name(void) {
    switch (g_active_backend) {
        case LINUX_AUDIO_PIPEWIRE:   return "pipewire";
        case LINUX_AUDIO_PULSEAUDIO: return "pulseaudio";
        case LINUX_AUDIO_ALSA:       return "alsa";
        default:                     return "none";
    }
}
