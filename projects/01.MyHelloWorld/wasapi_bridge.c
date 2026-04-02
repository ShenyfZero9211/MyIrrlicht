#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// [SharpEye] WASAPI Bridge Driver v1.07
// 极致加速退出响应，将关闭延迟从 500ms 压缩至 50ms

#define REFTIMES_PER_SEC  10000000
#define PCM_RING_SECONDS 16

typedef struct {
    IMMDeviceEnumerator *pEnumerator;
    IMMDevice *pDevice;
    IAudioClient *pAudioClient;
    IAudioCaptureClient *pCaptureClient;
    WAVEFORMATEX *pwfx;
    float current_low;
    float current_mid;
    float current_hi;
    BOOL active;
    HANDLE hThread;
    float *pcmRing;
    int ringCapacity;
    int ringWritePos;
    int ringReadPos;
    int ringCount;
    int unreadCount;
    double captureTimeSec;
    CRITICAL_SECTION ringLock;
    BOOL ringLockReady;
} WASAPI_State;

static WASAPI_State g_state = {0};

static void ring_push_sample(float sample) {
    if (!g_state.pcmRing || g_state.ringCapacity <= 0 || !g_state.ringLockReady) return;

    EnterCriticalSection(&g_state.ringLock);
    g_state.pcmRing[g_state.ringWritePos] = sample;
    g_state.ringWritePos = (g_state.ringWritePos + 1) % g_state.ringCapacity;
    if (g_state.ringCount < g_state.ringCapacity) {
        g_state.ringCount++;
    }
    if (g_state.unreadCount < g_state.ringCapacity) {
        g_state.unreadCount++;
    }
    LeaveCriticalSection(&g_state.ringLock);
}

static float get_sample_value(BYTE* pData, int index, WAVEFORMATEX* wfx) {
    int bits = wfx->wBitsPerSample;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (bits == 32 && wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)) {
        return ((float*)pData)[index];
    } else if (bits == 16) {
        return (float)((short*)pData)[index] / 32768.0f;
    } else if (bits == 24) {
        BYTE* b = &pData[index * 3];
        int val = (int)((b[2] << 16) | (b[1] << 8) | b[0]);
        if (val & 0x800000) val |= 0xFF000000;
        return (float)val / 8388608.0f;
    }
    return 0;
}

DWORD WINAPI CaptureThread(LPVOID lpParam) {
    HRESULT hr;
    UINT32 packetLength = 0;
    BYTE *pData;
    UINT32 numFramesAvailable;
    DWORD flags;

    while (g_state.active) {
        Sleep(1); 
        hr = g_state.pCaptureClient->lpVtbl->GetNextPacketSize(g_state.pCaptureClient, &packetLength);
        if (FAILED(hr)) continue;

        while (packetLength != 0 && g_state.active) {
            hr = g_state.pCaptureClient->lpVtbl->GetBuffer(g_state.pCaptureClient, &pData, &numFramesAvailable, &flags, NULL, NULL);
            if (SUCCEEDED(hr)) {
                int channels = g_state.pwfx->nChannels;
                float lAcc = 0, mAcc = 0, hAcc = 0;

                for (UINT32 i = 0; i < numFramesAvailable; i++) {
                    float frameMixed = 0;
                    for (int c = 0; c < channels; c++) {
                        float s = get_sample_value(pData, i*channels + c, g_state.pwfx);
                        s *= 2.0f; // GAIN BOOST 2.0x
                        ring_push_sample(s); // Store interleaved Stereo
                        frameMixed += fabsf(s);
                    }
                    frameMixed /= (float)channels;

                    lAcc += frameMixed * 12.0f; 
                    mAcc += frameMixed * 6.0f; 
                    hAcc += frameMixed * 4.0f;
                }

                float weight = 10.0f / (numFramesAvailable + 1); 
                g_state.current_low = g_state.current_low * 0.7f + (lAcc * weight) * 0.3f;
                g_state.current_mid = g_state.current_mid * 0.72f + (mAcc * weight) * 0.28f;
                g_state.current_hi = g_state.current_hi * 0.75f + (hAcc * weight) * 0.25f;
                g_state.captureTimeSec = (double)GetTickCount64() / 1000.0;

                g_state.pCaptureClient->lpVtbl->ReleaseBuffer(g_state.pCaptureClient, numFramesAvailable);
            }
            g_state.pCaptureClient->lpVtbl->GetNextPacketSize(g_state.pCaptureClient, &packetLength);
        }
    }
    return 0;
}

__declspec(dllexport) int wasapi_init() {
    if (g_state.active) return 1;
    HRESULT hr;
    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&g_state.pEnumerator);
    if (FAILED(hr)) return 0;
    hr = g_state.pEnumerator->lpVtbl->GetDefaultAudioEndpoint(g_state.pEnumerator, eRender, eConsole, &g_state.pDevice);
    if (FAILED(hr)) return 0;
    hr = g_state.pDevice->lpVtbl->Activate(g_state.pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_state.pAudioClient);
    if (FAILED(hr)) return 0;
    hr = g_state.pAudioClient->lpVtbl->GetMixFormat(g_state.pAudioClient, &g_state.pwfx);
    if (FAILED(hr)) return 0;
    if (!g_state.ringLockReady) {
        InitializeCriticalSection(&g_state.ringLock);
        g_state.ringLockReady = TRUE;
    }
    g_state.ringCapacity = (int)g_state.pwfx->nSamplesPerSec * g_state.pwfx->nChannels * PCM_RING_SECONDS;
    g_state.pcmRing = (float*)calloc((size_t)g_state.ringCapacity, sizeof(float));
    if (!g_state.pcmRing) return 0;
    g_state.ringWritePos = 0;
    g_state.ringReadPos = 0;
    g_state.ringCount = 0;
    g_state.unreadCount = 0;
    g_state.captureTimeSec = 0.0;
    hr = g_state.pAudioClient->lpVtbl->Initialize(g_state.pAudioClient, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, REFTIMES_PER_SEC, 0, g_state.pwfx, NULL);
    if (FAILED(hr)) return 0;
    hr = g_state.pAudioClient->lpVtbl->GetService(g_state.pAudioClient, &IID_IAudioCaptureClient, (void**)&g_state.pCaptureClient);
    if (FAILED(hr)) return 0;
    hr = g_state.pAudioClient->lpVtbl->Start(g_state.pAudioClient);
    if (FAILED(hr)) return 0;
    g_state.active = TRUE;
    g_state.hThread = CreateThread(NULL, 0, CaptureThread, NULL, 0, NULL);
    return 1;
}

__declspec(dllexport) void wasapi_get_format(int* channels, int* bits, int* rate) {
    if (g_state.pwfx) {
        if (channels) *channels = g_state.pwfx->nChannels;
        if (bits) *bits = g_state.pwfx->wBitsPerSample;
        if (rate) *rate = (int)g_state.pwfx->nSamplesPerSec;
    }
}

__declspec(dllexport) void wasapi_get_bands(float* l, float* m, float* h) {
    if (l) *l = (g_state.current_low > 1.5f) ? 1.0f : (g_state.current_low / 1.5f);
    if (m) *m = (g_state.current_mid > 1.5f) ? 1.0f : (g_state.current_mid / 1.5f);
    if (h) *h = (g_state.current_hi > 1.5f) ? 1.0f : (g_state.current_hi / 1.5f);
    g_state.current_low *= 0.88f;
    g_state.current_mid *= 0.88f;
    g_state.current_hi *= 0.88f;
}

__declspec(dllexport) int wasapi_read_pcm(float* out_samples, int max_samples) {
    int count, firstChunk, secondChunk;
    if (!out_samples || max_samples <= 0 || !g_state.pcmRing || !g_state.ringLockReady) {
        return 0;
    }

    EnterCriticalSection(&g_state.ringLock);
    count = (g_state.unreadCount < max_samples) ? g_state.unreadCount : max_samples;
    if (count <= 0) {
        LeaveCriticalSection(&g_state.ringLock);
        return 0;
    }

    firstChunk = g_state.ringCapacity - g_state.ringReadPos;
    if (firstChunk > count) firstChunk = count;
    memcpy(out_samples, g_state.pcmRing + g_state.ringReadPos, (size_t)firstChunk * sizeof(float));

    secondChunk = count - firstChunk;
    if (secondChunk > 0) {
        memcpy(out_samples + firstChunk, g_state.pcmRing, (size_t)secondChunk * sizeof(float));
    }

    g_state.ringReadPos = (g_state.ringReadPos + count) % g_state.ringCapacity;
    g_state.unreadCount -= count;
    LeaveCriticalSection(&g_state.ringLock);

    return count;
}

__declspec(dllexport) int wasapi_get_recent_pcm(float* out_samples, int max_samples) {
    int count, start, firstChunk, secondChunk;
    if (!out_samples || max_samples <= 0 || !g_state.pcmRing || !g_state.ringLockReady) {
        return 0;
    }

    EnterCriticalSection(&g_state.ringLock);
    count = (g_state.ringCount < max_samples) ? g_state.ringCount : max_samples;
    start = g_state.ringWritePos - count;
    if (start < 0) start += g_state.ringCapacity;

    firstChunk = g_state.ringCapacity - start;
    if (firstChunk > count) firstChunk = count;
    memcpy(out_samples, g_state.pcmRing + start, (size_t)firstChunk * sizeof(float));

    secondChunk = count - firstChunk;
    if (secondChunk > 0) {
        memcpy(out_samples + firstChunk, g_state.pcmRing, (size_t)secondChunk * sizeof(float));
    }
    LeaveCriticalSection(&g_state.ringLock);

    return count;
}

__declspec(dllexport) int wasapi_get_recent_pcm_rate() {
    return g_state.pwfx ? (int)g_state.pwfx->nSamplesPerSec : 0;
}

__declspec(dllexport) double wasapi_get_capture_time() {
    return g_state.captureTimeSec;
}

__declspec(dllexport) void wasapi_stop() {
    if (!g_state.active) return;
    g_state.active = FALSE;
    if (g_state.hThread) {
        // [SPEED OPTIMIZATION] 大幅缩减等待时间：由 500ms 压缩至 50ms
        WaitForSingleObject(g_state.hThread, 50); 
        CloseHandle(g_state.hThread);
        g_state.hThread = NULL;
    }
    if (g_state.pAudioClient) {
        g_state.pAudioClient->lpVtbl->Stop(g_state.pAudioClient);
    }
    // 注意：在 DLL 卸载时 CoUninitialize 可能导致卡顿，确保仅在初始化成功后调用
    if (g_state.pCaptureClient) {
        g_state.pCaptureClient->lpVtbl->Release(g_state.pCaptureClient);
        g_state.pCaptureClient = NULL;
    }
    if (g_state.pAudioClient) {
        g_state.pAudioClient->lpVtbl->Release(g_state.pAudioClient);
        g_state.pAudioClient = NULL;
    }
    if (g_state.pDevice) {
        g_state.pDevice->lpVtbl->Release(g_state.pDevice);
        g_state.pDevice = NULL;
    }
    if (g_state.pEnumerator) {
        g_state.pEnumerator->lpVtbl->Release(g_state.pEnumerator);
        g_state.pEnumerator = NULL;
    }
    if (g_state.pwfx) {
        CoTaskMemFree(g_state.pwfx);
        g_state.pwfx = NULL;
    }
    if (g_state.pcmRing) {
        free(g_state.pcmRing);
        g_state.pcmRing = NULL;
    }
    g_state.ringCapacity = 0;
    g_state.ringWritePos = 0;
    g_state.ringCount = 0;
    g_state.captureTimeSec = 0.0;
    if (g_state.ringLockReady) {
        DeleteCriticalSection(&g_state.ringLock);
        g_state.ringLockReady = FALSE;
    }
    CoUninitialize();
}
