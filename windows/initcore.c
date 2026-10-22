#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include "initcore.h"

//----------------------------- Define WASAPI COM GUIDs directly for linkers without mmdevapi.lib --------------------
const CLSID CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
const IID IID_IMMDeviceEnumerator   = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
const IID IID_IAudioClient          = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
const IID IID_IAudioRenderClient    = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

static int g_wasapi_initialized = 0;

int initcore_init(void) {
    HRESULT hr;
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    WAVEFORMATEX* pwfx = NULL;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        printf("initcore: CoInitializeEx failed (0x%08lX)\n", (unsigned long)hr);
        return -1;
    }
    //testcall
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) {
        printf("initcore: failed to create IMMDeviceEnumerator (0x%08lX)\n", (unsigned long)hr);
        CoUninitialize();
        return -1;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        printf("initcore: failed to get default audio endpoint (0x%08lX)\n", (unsigned long)hr);
        IMMDeviceEnumerator_Release(pEnumerator);
        CoUninitialize();
        return -1;
    }
    hr = IMMDevice_Activate(pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
    if (FAILED(hr)) {
        printf("initcore: failed to activate IAudioClient (0x%08lX)\n", (unsigned long)hr);
        IMMDevice_Release(pDevice);
        IMMDeviceEnumerator_Release(pEnumerator);
        CoUninitialize();
        return -1;
    }
    hr = IAudioClient_GetMixFormat(pAudioClient, &pwfx);
    if (FAILED(hr)) {
        printf("initcore: failed to retrieve mix format (0x%08lX)\n", (unsigned long)hr);
        IAudioClient_Release(pAudioClient);
        IMMDevice_Release(pDevice);
        IMMDeviceEnumerator_Release(pEnumerator);
        CoUninitialize();
        return -1;
    }
    printf("initcore: wasapi ready (mix format: %u Hz, %u channels, %u bits)\n",pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample);
    CoTaskMemFree(pwfx);
    IAudioClient_Release(pAudioClient);
    IMMDevice_Release(pDevice);
    IMMDeviceEnumerator_Release(pEnumerator);
    g_wasapi_initialized = 1;
    return 0;
}
void initcore_shutdown(void) {
    if (g_wasapi_initialized) {
        CoUninitialize();
        g_wasapi_initialized = 0;
        printf("initcore: wasapi core closed\n");
    }
}
