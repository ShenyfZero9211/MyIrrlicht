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
#define FFT_SIZE 2048

typedef struct { float r, i; } Complex;
static void fft(Complex *v, int n, Complex *tmp);
int wasapi_get_recent_pcm(float* out_samples, int max_samples);
int wasapi_read_pcm(float* out_samples, int max_samples);
int wasapi_get_recent_pcm_rate();
static float g_fft_smoothing[128] = {0};
static float g_current_volume = 0;
static float g_tilt_base = 4.5f;
static float g_tilt_power = 1.2f;

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

// Forward FFt
static void fft(Complex *v, int n, Complex *tmp) {
    if (n > 1) {
        int k, m = n >> 1;
        for (k = 0; k < m; k++) {
            tmp[k] = v[k << 1];
            tmp[k + m] = v[(k << 1) + 1];
        }
        fft(tmp, m, v);
        fft(tmp + m, m, v);
        for (k = 0; k < m; k++) {
            float arg = -2.0f * 3.14159265f * (float)k / (float)n;
            Complex w = { cosf(arg), sinf(arg) };
            Complex e = tmp[k];
            Complex o = tmp[k + m];
            Complex wo = { w.r * o.r - w.i * o.i, w.r * o.i + w.i * o.r };
            v[k].r = e.r + wo.r;
            v[k].i = e.i + wo.i;
            v[k + m].r = e.r - wo.r;
            v[k + m].i = e.i - wo.i;
        }
    }
}

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

__declspec(dllexport) float wasapi_get_volume() {
    float v = g_current_volume;
    g_current_volume *= 0.9f; 
    return (v > 1.0f) ? 1.0f : v;
}

__declspec(dllexport) void wasapi_get_fft(float* bins, int nBoxes) {
    if (!bins || nBoxes <= 0 || nBoxes > 128) return;
    
    float pcm[FFT_SIZE];
    int count = wasapi_get_recent_pcm(pcm, FFT_SIZE);
    if (count < FFT_SIZE) return;

    // RMS Volume calculation
    float sumSq = 0;
    for(int i=0; i<FFT_SIZE; i++) sumSq += pcm[i]*pcm[i];
    float rms = sqrtf(sumSq / FFT_SIZE) * 5.0f; // Scale up for visibility
    g_current_volume = g_current_volume * 0.5f + rms * 0.5f;

    Complex buf[FFT_SIZE], tmp[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        float window = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_SIZE - 1)));
        buf[i].r = pcm[i] * window;
        buf[i].i = 0;
    }

    fft(buf, FFT_SIZE, tmp);

    // Logarithmic Banding Overhaul
    float sample_rate = (float)wasapi_get_recent_pcm_rate();
    if (sample_rate <= 0) sample_rate = 44100.0f;

    // OCTAVE-UNIFORM DISTRIBUTION (MUSICAL SCALE)
    float min_freq = 60.0f; // Raised to filter sub-bass mud
    float max_freq = 18000.0f;
    if (max_freq > sample_rate / 2.0f) max_freq = sample_rate / 2.1f;
    float num_octaves = log2f(max_freq / min_freq);

    for (int i = 0; i < nBoxes; i++) {
        // Equal musical interval steps (Octaves)
        float f_low = min_freq * powf(2.0f, (float)i * num_octaves / (float)nBoxes);
        float f_high = min_freq * powf(2.0f, (float)(i + 1) * num_octaves / (float)nBoxes);
        
        float k_low = f_low * (float)FFT_SIZE / sample_rate;
        float k_high = f_high * (float)FFT_SIZE / sample_rate;
        if (k_high <= k_low) k_high = k_low + 1.0f;

        float mag = 0;
        float total_weight = 0;

        int k_start = (int)k_low;
        int k_end = (int)k_high;

        for (int k = k_start; k <= k_end && k < FFT_SIZE / 2; k++) {
            float w = 1.0f;
            if (k == k_start) w *= (1.0f - (k_low - (float)k_start));
            if (k == k_end) w *= (k_high - (float)k_end);
            if (k_start == k_end) w = (k_high - k_low);

            mag += sqrtf(buf[k].r * buf[k].r + buf[k].i * buf[k].i) * w;
            total_weight += w;
        }

        if (total_weight > 0) mag /= total_weight;

        // Apply spectral tilt for visual balance
        float center_freq = (f_low + f_high) * 0.5f;
        float tilt_boost = powf(center_freq / 200.0f, 0.95f); // Slightly steeper
        mag *= tilt_boost;
        
        mag /= (float)FFT_SIZE; 
        
        // --- dB Scaling ---
        float db = 20.0f * log10f(mag + 1e-7f);
        
        float freq_factor = (float)i / (float)nBoxes;
        float db_floor = -60.0f - freq_factor * 10.0f; // Tighter floor
        float db_ceil = -5.0f; // Added some headroom
        
        mag = (db - db_floor) / (db_ceil - db_floor);
        if (mag < 0) mag = 0;
        if (mag > 1.0f) mag = 1.0f;

        // SENSITIVITY TILT: Balanced boost for high frequencies (Now dynamic)
        float tilt = 1.0f + powf(freq_factor, g_tilt_power) * g_tilt_base; 
        mag *= tilt; 

        // Temporal smoothing
        g_fft_smoothing[i] = g_fft_smoothing[i] * 0.35f + mag * 0.65f;
    }

    // NEW: SPATIAL SMOOTHING (Horizontal Filter)
    for (int i = 0; i < nBoxes; i++) {
        float prev = (i > 0) ? g_fft_smoothing[i - 1] : g_fft_smoothing[i];
        float next = (i < nBoxes - 1) ? g_fft_smoothing[i + 1] : g_fft_smoothing[i];
        
        // 3-tap weighted average for smoother transitions
        float smooth_mag = prev * 0.25f + g_fft_smoothing[i] * 0.5f + next * 0.25f;
        bins[i] = (smooth_mag > 1.0f) ? 1.0f : smooth_mag;
    }
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
        // [SHUTDOWN LOCK] 增加等待时间从 50ms 至 200ms，确保线程有足够时间捕获退出信号
        WaitForSingleObject(g_state.hThread, 200); 
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

__declspec(dllexport) void wasapi_set_config_v2(float base, float power) {
    g_tilt_base = base;
    g_tilt_power = power;
}
