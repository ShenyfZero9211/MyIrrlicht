#include <irrlicht.h>
#include <iostream>
#include <vector>
#include <windows.h>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <direct.h> // for _mkdir

using namespace irr;
using namespace core;
using namespace scene;
using namespace video;
using namespace io;
using namespace gui;

// WASAPI Bridge API Functions - MATCHED TO updated wasapi_bridge.c
typedef int (*wasapi_init_fn)();
typedef void (*wasapi_stop_fn)();
typedef void (*wasapi_get_bands_fn)(float* low, float* mid, float* hi);
typedef int (*wasapi_get_recent_pcm_fn)(float* buffer, int max_samples);
typedef int (*wasapi_read_pcm_fn)(float* buffer, int max_samples); // NEW: Streaming API
typedef void (*wasapi_get_format_fn)(int* channels, int* bits, int* rate);

HINSTANCE hWasapiDll = NULL;
wasapi_init_fn wasapi_init_fn_ptr = NULL;
wasapi_stop_fn wasapi_stop_fn_ptr = NULL;
wasapi_get_bands_fn wasapi_get_bands_fn_ptr = NULL;
wasapi_get_recent_pcm_fn wasapi_get_recent_pcm_fn_ptr = NULL;
wasapi_read_pcm_fn wasapi_read_pcm_fn_ptr = NULL; // NEW
wasapi_get_format_fn wasapi_get_format_fn_ptr = NULL;

// Global State
bool isCapturing = false;
FILE* wavFile = NULL;
uint32_t dataLength = 0;

enum {
    GUI_ID_START = 101,
    GUI_ID_STOP
};

void writeWavHeader(FILE* f, int sampleRate, int channels) {
    if (!f) return;
    fwrite("RIFF", 1, 4, f);
    uint32_t fileSize = 0;
    fwrite(&fileSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtLen = 16;
    fwrite(&fmtLen, 4, 1, f);
    uint16_t fmtTag = 1; // PCM (Standard)
    fwrite(&fmtTag, 2, 1, f);
    uint16_t numChan = (uint16_t)channels;
    fwrite(&numChan, 2, 1, f);
    uint32_t sRate = (uint32_t)sampleRate;
    fwrite(&sRate, 4, 1, f);
    uint32_t byteRate = sRate * numChan * 2; // 2 bytes per sample
    fwrite(&byteRate, 4, 1, f);
    uint16_t blockAlign = numChan * 2;
    fwrite(&blockAlign, 2, 1, f);
    uint16_t bitsPerSample = 16;
    fwrite(&bitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t dataLen = 0;
    fwrite(&dataLen, 4, 1, f);
}

void finalizeWavHeader(FILE* f, uint32_t len) {
    if (!f) return;
    fseek(f, 4, SEEK_SET);
    uint32_t riffLen = len + 36;
    fwrite(&riffLen, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&len, 4, 1, f);
}

class MyEventReceiver : public IEventReceiver {
public:
    MyEventReceiver(IGUIEnvironment* env) : GuiEnv(env) {}
    virtual bool OnEvent(const SEvent& event) {
        if (event.EventType == irr::EET_GUI_EVENT) {
            s32 id = event.GUIEvent.Caller->getID();
            if (event.GUIEvent.EventType == irr::gui::EGET_BUTTON_CLICKED) {
                if (id == GUI_ID_START && !isCapturing) {
                    if (!wasapi_init_fn_ptr || !wasapi_get_format_fn_ptr) {
                        MessageBoxA(NULL, "DLL function pointers missing!", "Error", MB_OK | MB_ICONERROR);
                        return true;
                    }
                    if (wasapi_init_fn_ptr()) {
                        isCapturing = true;
                        IGUIElement* startBtn = GuiEnv->getRootGUIElement()->getElementFromId(GUI_ID_START, true);
                        if (startBtn) startBtn->setText(L"Capturing...");

                        char filename[256];
                        time_t t = time(0);
                        struct tm* timeinfo = localtime(&t);
                        strftime(filename, sizeof(filename), "captured_audio/record_%Y%m%d_%H%M%S.wav", timeinfo);
                        _mkdir("captured_audio");
                        wavFile = fopen(filename, "wb");
                        if (wavFile) {
                            int channels = 0, bits = 0, rate = 0;
                            wasapi_get_format_fn_ptr(&channels, &bits, &rate);
                            writeWavHeader(wavFile, rate, channels);
                            dataLength = 0;
                            std::cout << "Streaming Recording Started: " << filename << std::endl;
                        }
                    }
                } else if (id == GUI_ID_STOP && isCapturing) {
                    if (wasapi_stop_fn_ptr) {
                        wasapi_stop_fn_ptr();
                        isCapturing = false;
                        IGUIElement* startBtn = GuiEnv->getRootGUIElement()->getElementFromId(GUI_ID_START, true);
                        if (startBtn) startBtn->setText(L"Start Capture");
                        if (wavFile) {
                            finalizeWavHeader(wavFile, dataLength);
                            fclose(wavFile);
                            wavFile = NULL;
                            MessageBoxA(NULL, "Recording Saved (Stream Corrected).", "Info", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                }
            }
        }
        return false;
    }
private:
    IGUIEnvironment* GuiEnv;
};

SColor hsv2rgb(float h, float s, float v) {
    float r, g, b;
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }
    return SColor(255, (u32)(r * 255), (u32)(g * 255), (u32)(b * 255));
}

int main() {
    _mkdir("captured_audio");

    hWasapiDll = LoadLibraryA("wasapi_bridge.dll");
    if (hWasapiDll) {
        wasapi_init_fn_ptr = (wasapi_init_fn)GetProcAddress(hWasapiDll, "wasapi_init");
        wasapi_stop_fn_ptr = (wasapi_stop_fn)GetProcAddress(hWasapiDll, "wasapi_stop");
        wasapi_get_bands_fn_ptr = (wasapi_get_bands_fn)GetProcAddress(hWasapiDll, "wasapi_get_bands");
        wasapi_get_recent_pcm_fn_ptr = (wasapi_get_recent_pcm_fn)GetProcAddress(hWasapiDll, "wasapi_get_recent_pcm");
        wasapi_read_pcm_fn_ptr = (wasapi_read_pcm_fn)GetProcAddress(hWasapiDll, "wasapi_read_pcm");
        wasapi_get_format_fn_ptr = (wasapi_get_format_fn)GetProcAddress(hWasapiDll, "wasapi_get_format");
    } else {
        MessageBoxA(NULL, "wasapi_bridge.dll not found!", "Error", MB_OK | MB_ICONERROR);
    }

    IrrlichtDevice *device = createDevice(video::EDT_OPENGL, dimension2d<u32>(800, 600));
    if (!device) return 1;

    IGUIEnvironment* guienv = device->getGUIEnvironment();
    MyEventReceiver receiver(guienv);
    device->setEventReceiver(&receiver);
    device->setWindowCaption(L"Irrlicht Audio Visualization (Streaming Fix)");

    IVideoDriver* driver = device->getVideoDriver();
    guienv->addButton(rect<s32>(20, 20, 180, 60), 0, GUI_ID_START, L"Start Capture");
    guienv->addButton(rect<s32>(20, 80, 180, 120), 0, GUI_ID_STOP, L"Stop Capture");

    float curH = 0.5f, curS = 0.5f, curV = 0.5f;

    while(device->run()) {
        if (device->isWindowActive()) {
            SColor bgColor(255, 0, 0, 0);
            if (isCapturing) {
                // 1. Visualization with smoothing and range clamp [0.4, 0.7]
                if (wasapi_get_bands_fn_ptr) {
                    float low, mid, hi;
                    wasapi_get_bands_fn_ptr(&low, &mid, &hi);
                    
                    // Clamping to [0.4, 0.7] through linear mapping
                    float targetH = 0.4f + hi * 0.3f;
                    float targetS = 0.4f + mid * 0.3f;
                    float targetV = 0.4f + low * 0.3f;

                    // Smooth transition (lerp factor 0.05 for "fluid" feel)
                    curH += (targetH - curH) * 0.05f;
                    curS += (targetS - curS) * 0.05f;
                    curV += (targetV - curV) * 0.05f;

                    bgColor = hsv2rgb(curH, curS, curV);
                }

                // 2. Stream Capture (Drains the DLL buffer)
                if (wavFile && wasapi_read_pcm_fn_ptr) {
                    float pcmBuf[4096];
                    short pcm16Buf[4096];
                    int count;
                    // Drain the buffer until empty to ensure no lag/overlap
                    while ((count = wasapi_read_pcm_fn_ptr(pcmBuf, 4096)) > 0) {
                        for (int i = 0; i < count; i++) {
                            float s = pcmBuf[i];
                            // Clipping Protection (Clamp to [-1.0, 1.0])
                            if (s > 1.0f) s = 1.0f;
                            if (s < -1.0f) s = -1.0f;
                            // Quantize to 16-bit PCM
                            pcm16Buf[i] = (short)(s * 32767.0f);
                        }
                        fwrite(pcm16Buf, 2, count, wavFile);
                        dataLength += count * 2;
                    }
                }
            }
            driver->beginScene(true, true, bgColor);
            guienv->drawAll();
            driver->endScene();
        }
        device->yield();
    }

    if (isCapturing && wasapi_stop_fn_ptr) wasapi_stop_fn_ptr();
    if (wavFile) { finalizeWavHeader(wavFile, dataLength); fclose(wavFile); }
    if (hWasapiDll) FreeLibrary(hWasapiDll);
    device->drop();
    return 0;
}
