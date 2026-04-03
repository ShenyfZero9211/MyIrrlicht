#include <irrlicht.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <math.h>
#include <windows.h>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <direct.h> // for _mkdir
#include <fstream>
#include <string>
#include <sstream>

using namespace irr;
using namespace core;
using namespace scene;
using namespace video;
using namespace io;
using namespace gui;

// WASAPI Bridge API Functions
typedef int (*wasapi_init_fn)();
typedef void (*wasapi_stop_fn)();
typedef void (*wasapi_get_bands_fn)(float* low, float* mid, float* hi);
typedef void (*wasapi_get_fft_fn)(float* bins, int nBoxes);
typedef void (*wasapi_get_fft_fn)(float* bins, int nBoxes);
typedef int (*wasapi_get_recent_pcm_fn)(float* buffer, int max_samples);
typedef int (*wasapi_read_pcm_fn)(float* buffer, int max_samples);
typedef void (*wasapi_get_format_fn)(int* channels, int* bits, int* rate);
typedef void (*wasapi_set_config_v2_fn)(float base, float power);

HINSTANCE hWasapiDll = NULL;
wasapi_init_fn wasapi_init_fn_ptr = NULL;
wasapi_stop_fn wasapi_stop_fn_ptr = NULL;
wasapi_get_bands_fn wasapi_get_bands_fn_ptr = NULL;
wasapi_get_recent_pcm_fn wasapi_get_recent_pcm_fn_ptr = NULL;
wasapi_read_pcm_fn wasapi_read_pcm_fn_ptr = NULL;
wasapi_get_fft_fn wasapi_get_fft_fn_ptr = NULL;
wasapi_get_format_fn wasapi_get_format_fn_ptr = NULL;
wasapi_set_config_v2_fn wasapi_set_config_v2_fn_ptr = NULL;

// Global State
bool isRecording = false;
FILE* wavFile = NULL;
uint32_t dataLength = 0;

// Dynamic Settings
float g_VisualMultiplier = 45.0f;
float g_Smoothness = 0.15f;
float g_Gain = 2.5f;
int g_BarCount = 64;
float g_HF_TiltBase = 4.5f;
float g_HF_TiltPower = 1.2f;
float g_PeakGravity = 0.22f; // Gravity for Peak fall
float g_MaxHeight = 40.0f;  // Maximum allowed visual height

std::string g_ConfigPath;
FILETIME g_lastConfigWriteTime = {0, 0};

// Forward Declarations
SColor hsv2rgb(float h, float s, float v);

std::string getAppDir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string path(buffer);
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) return path.substr(0, pos);
    return ".";
}

void loadSettings(const char* filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "Could not open " << filename << ", using defaults." << std::endl;
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t\r\n"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);

        if (key == "VisualMultiplier") g_VisualMultiplier = std::stof(val);
        else if (key == "Smoothness") g_Smoothness = std::stof(val);
        else if (key == "Gain") g_Gain = std::stof(val);
        else if (key == "BarCount") {
            int newCount = std::stoi(val);
            if (newCount > 0 && newCount <= 128) g_BarCount = newCount;
        }
        else if (key == "HF_TiltBase") g_HF_TiltBase = std::stof(val);
        else if (key == "HF_TiltPower") g_HF_TiltPower = std::stof(val);
        else if (key == "PeakGravity") g_PeakGravity = std::stof(val);
        else if (key == "MaxHeight") g_MaxHeight = std::stof(val);
    }
    std::cout << "Config Loaded: Mult=" << g_VisualMultiplier << ", Smooth=" << g_Smoothness << ", Gain=" << g_Gain 
              << ", Bars=" << g_BarCount << ", MaxH=" << g_MaxHeight << ", PeakG=" << g_PeakGravity << std::endl;
}

bool checkConfigUpdate() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(g_ConfigPath.c_str(), GetFileExInfoStandard, &data)) {
        if (CompareFileTime(&data.ftLastWriteTime, &g_lastConfigWriteTime) > 0) {
            int oldCount = g_BarCount;
            loadSettings(g_ConfigPath.c_str());
            g_lastConfigWriteTime = data.ftLastWriteTime;
            return (g_BarCount != oldCount);
        }
    }
    return false;
}

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
    uint16_t fmtTag = 1;
    fwrite(&fmtTag, 2, 1, f);
    uint16_t numChan = (uint16_t)channels;
    fwrite(&numChan, 2, 1, f);
    uint32_t sRate = (uint32_t)sampleRate;
    fwrite(&sRate, 4, 1, f);
    uint32_t byteRate = sRate * numChan * 2;
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
                if (id == GUI_ID_START && !isRecording) {
                    isRecording = true;
                    IGUIElement* startBtn = GuiEnv->getRootGUIElement()->getElementFromId(GUI_ID_START, true);
                    if (startBtn) startBtn->setText(L"Recording...");
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
                    }
                } else if (id == GUI_ID_STOP && isRecording) {
                    isRecording = false;
                    IGUIElement* startBtn = GuiEnv->getRootGUIElement()->getElementFromId(GUI_ID_START, true);
                    if (startBtn) startBtn->setText(L"Start Recording");
                    if (wavFile) {
                        finalizeWavHeader(wavFile, dataLength);
                        fclose(wavFile);
                        wavFile = NULL;
                    }
                }
            }
        }
        return false;
    }
private:
    IGUIEnvironment* GuiEnv;
};

struct BarData {
    ISceneNode* node;
    ISceneNode* peak;
    float height;
    float peakHeight;
    float peakVel;
    SColor color;
};

std::vector<BarData> g_Bars;

void initSpectrum(ISceneManager* smgr) {
    for (size_t i = 0; i < g_Bars.size(); ++i) {
        if (g_Bars[i].node) g_Bars[i].node->remove();
        if (g_Bars[i].peak) g_Bars[i].peak->remove();
    }
    g_Bars.clear();

    float totalWidth = 110.0f;
    float spacing = totalWidth / (float)g_BarCount;
    float barWidth = spacing * 0.75f;
    float startX = -(totalWidth / 2.0f);

    for (int i = 0; i < g_BarCount; ++i) {
        BarData b;
        b.node = smgr->addCubeSceneNode(2.0f);
        b.peak = smgr->addCubeSceneNode(2.0f);
        b.height = 1.0f;
        b.peakHeight = 1.1f;
        b.peakVel = 0.0f;

        if (b.node) {
            b.node->setPosition(vector3df(startX + i * spacing, 0, 0)); // Move closer to Z=0
            b.node->setScale(vector3df(barWidth * 0.37f, 0.5f, 2.0f));
            // Default Diffuse: Deep Studio Blue
            b.color = hsv2rgb(0.61f, 1.0f, 0.35f); 
            b.node->getMaterial(0).AmbientColor = SColor(255, 30, 30, 35); 
            b.node->getMaterial(0).DiffuseColor = b.color;
            b.node->getMaterial(0).EmissiveColor = SColor(0, 0, 0, 0); // No self-illumination
            b.node->getMaterial(0).Lighting = true; 
        }
        if (b.peak) {
            b.peak->setPosition(vector3df(startX + i * spacing, 2, 0));
            b.peak->setScale(vector3df(barWidth * 0.37f, 0.15f, 2.0f));
            b.peak->getMaterial(0).DiffuseColor = SColor(255, 220, 240, 255); 
            b.peak->getMaterial(0).Lighting = true;
        }
        g_Bars.push_back(b);
    }
}

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
    g_ConfigPath = getAppDir() + "\\settings.cfg";
    loadSettings(g_ConfigPath.c_str());
    
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(g_ConfigPath.c_str(), GetFileExInfoStandard, &data)) {
        g_lastConfigWriteTime = data.ftLastWriteTime;
    }

    hWasapiDll = LoadLibraryA("wasapi_bridge.dll");
    if (hWasapiDll) {
        wasapi_init_fn_ptr = (wasapi_init_fn)GetProcAddress(hWasapiDll, "wasapi_init");
        wasapi_stop_fn_ptr = (wasapi_stop_fn)GetProcAddress(hWasapiDll, "wasapi_stop");
        wasapi_read_pcm_fn_ptr = (wasapi_read_pcm_fn)GetProcAddress(hWasapiDll, "wasapi_read_pcm");
        wasapi_get_format_fn_ptr = (wasapi_get_format_fn)GetProcAddress(hWasapiDll, "wasapi_get_format");
        wasapi_get_fft_fn_ptr = (wasapi_get_fft_fn)GetProcAddress(hWasapiDll, "wasapi_get_fft");
        wasapi_set_config_v2_fn_ptr = (wasapi_set_config_v2_fn)GetProcAddress(hWasapiDll, "wasapi_set_config_v2");
        if (wasapi_set_config_v2_fn_ptr) wasapi_set_config_v2_fn_ptr(g_HF_TiltBase, g_HF_TiltPower);
        if (wasapi_init_fn_ptr) wasapi_init_fn_ptr();
    }

    IrrlichtDevice *device = createDevice(video::EDT_OPENGL, dimension2d<u32>(800, 600), 16, false, false, false, 0);
    if (!device) return 1;
    
    device->setResizable(true); // Allow maximization and manual resizing

    IGUIEnvironment* guienv = device->getGUIEnvironment();
    IVideoDriver* driver = device->getVideoDriver();
    ISceneManager* smgr = device->getSceneManager();

    MyEventReceiver receiver(guienv);
    device->setEventReceiver(&receiver);
    device->setWindowCaption(L"Irrlicht Audio Visualization - Physical Studio v0.0.6");

    IGUIElement* startBtn = guienv->addButton(rect<s32>(20, 20, 180, 60), 0, GUI_ID_START, L"Start Recording");
    IGUIElement* stopBtn = guienv->addButton(rect<s32>(20, 80, 180, 120), 0, GUI_ID_STOP, L"Stop Recording");
    if (startBtn) startBtn->setVisible(false); 
    if (stopBtn) stopBtn->setVisible(false);

    // PHYSICAL STUDIO LIGHTS
    smgr->addLightSceneNode(0, vector3df(0, 150, -85), SColorf(1.0f, 1.0f, 1.0f, 1.0f), 800.0f);
    smgr->setAmbientLight(SColorf(0.12f, 0.12f, 0.18f, 1.0f));

    // CAMERA: Close-up Studio View
    smgr->addCameraSceneNode(0, vector3df(0, 45, -75), vector3df(0, 15, 0));

    initSpectrum(smgr);

    u32 lastCheckTime = 0;
    SColor bgColor = SColor(255, 10, 10, 20);

    while(device->run()) {
        u32 now = device->getTimer()->getTime();
        if (now - lastCheckTime > 500) {
            if (checkConfigUpdate()) {
                initSpectrum(smgr);
                if (wasapi_set_config_v2_fn_ptr) wasapi_set_config_v2_fn_ptr(g_HF_TiltBase, g_HF_TiltPower);
            }
            lastCheckTime = now;
        }

        // Background Running Enabled (Always update logic)
        if (wasapi_get_fft_fn_ptr) {
            std::vector<float> fftBins(g_BarCount);
            wasapi_get_fft_fn_ptr(fftBins.data(), g_BarCount);
            bgColor = SColor(255, 5, 5, 8); // Very dark navy for maximum neon contrast

            for (int i = 0; i < g_BarCount; ++i) {
                float rawHeight = fftBins[i] * g_VisualMultiplier * g_Gain;
                // RE-INTRODUCE HF_TILT: Apply exponential boost/attenuation to High Frequencies
                float tilt = powf(g_HF_TiltBase, (float)i / g_BarCount * g_HF_TiltPower);
                float tiltedHeight = rawHeight * tilt;

                // NEW FORMULA with TILT: MaxHeight is the master visual scale
                float targetHeight = 1.0f + (g_MaxHeight * tanhf(tiltedHeight / 15.0f));
                
                if (targetHeight > g_Bars[i].height) g_Bars[i].height = targetHeight;
                else g_Bars[i].height = g_Bars[i].height * (1.0f - g_Smoothness) + targetHeight * g_Smoothness;

                // Position Peak based on CALCULATED peakHeight
                g_Bars[i].node->setScale(vector3df(g_Bars[i].node->getScale().X, g_Bars[i].height / 2.0f, 2.0f));
                g_Bars[i].node->setPosition(vector3df(g_Bars[i].node->getPosition().X, g_Bars[i].height / 2.0f, 0));
                
                // DYNAMIC COLOR: Synchronized with current MaxHeight
                float factor = g_Bars[i].height / g_MaxHeight; 
                if (factor > 1.0f) factor = 1.0f;
                float s = 1.0f - factor * 0.95f;
                float v = 0.35f + factor * 0.65f;
                SColor dynamicColor = hsv2rgb(0.61f, s, v);
                
                g_Bars[i].node->getMaterial(0).DiffuseColor = dynamicColor;
                g_Bars[i].node->getMaterial(0).AmbientColor = dynamicColor;

                // Sync Peak Color 
                g_Bars[i].peak->getMaterial(0).DiffuseColor = SColor(255, 230, 245, 255);

                // Peak Physics: Gravity & Decay
                if (g_Bars[i].height > g_Bars[i].peakHeight) {
                    g_Bars[i].peakHeight = g_Bars[i].height;
                    g_Bars[i].peakVel = 0.0f;
                } else {
                    g_Bars[i].peakVel += g_PeakGravity; // Use dynamic gravity
                    g_Bars[i].peakHeight -= g_Bars[i].peakVel;
                    if (g_Bars[i].peakHeight < g_Bars[i].height) {
                        g_Bars[i].peakHeight = g_Bars[i].height;
                        g_Bars[i].peakVel = 0.0f;
                    }
                }
                
                // Position Peak based on CALCULATED peakHeight
                g_Bars[i].peak->setPosition(vector3df(g_Bars[i].node->getPosition().X, g_Bars[i].peakHeight + 1.25f, 0));
            }
        }

        if (isRecording && wavFile && wasapi_read_pcm_fn_ptr) {
            float pcmBuf[4096]; short pcm16Buf[4096]; int count;
            while ((count = wasapi_read_pcm_fn_ptr(pcmBuf, 4096)) > 0) {
                for (int i = 0; i < count; i++) {
                    float s = (pcmBuf[i] > 1.0f) ? 1.0f : (pcmBuf[i] < -1.0f ? -1.0f : pcmBuf[i]);
                    pcm16Buf[i] = (short)(s * 32767.0f);
                }
                fwrite(pcm16Buf, 2, count, wavFile);
                dataLength += count * 2;
            }
        }

        // Only draw if window is active to save GPU, but logic runs always
        if (device->isWindowActive()) {
            driver->beginScene(true, true, bgColor);
            smgr->drawAll();
            guienv->drawAll();
            driver->endScene();
        } else {
            device->yield(); // Be nice to CPU in background
        }
        device->yield();
    }

    if (wasapi_stop_fn_ptr) wasapi_stop_fn_ptr();
    if (wavFile) { finalizeWavHeader(wavFile, dataLength); fclose(wavFile); }
    if (hWasapiDll) FreeLibrary(hWasapiDll);
    device->drop();
    ExitProcess(0);
    return 0;
}
