#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>   /* Windows 11 SDK */
#include <psapi.h>                         /* EnumProcesses, GetModuleBaseName */
#include <propvarutil.h>

#include "imgui_internal.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "GLFW/glfw3.h"
#include "nfd.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

struct Wav
{
    /* 
        [Master RIFF chunk]
            FileTypeBlocID  (4 bytes) : Identifier « RIFF »  (0x52, 0x49, 0x46, 0x46)
            FileSize        (4 bytes) : Overall file size minus 8 bytes
            FileFormatID    (4 bytes) : Format = « WAVE »  (0x57, 0x41, 0x56, 0x45)

        [Chunk describing the data format]
            FormatBlocID    (4 bytes) : Identifier « fmt␣ »  (0x66, 0x6D, 0x74, 0x20)
            BlocSize        (4 bytes) : Chunk size minus 8 bytes, which is 16 bytes here  (0x10)
            AudioFormat     (2 bytes) : Audio format (1: PCM integer, 3: IEEE 754 float)
            NbrChannels     (2 bytes) : Number of channels
            Frequency       (4 bytes) : Sample rate (in hertz)
            BytePerSec      (4 bytes) : Number of bytes to read per second (Frequency * BytePerBloc).
            BytePerBloc     (2 bytes) : Number of bytes per block (NbrChannels * BitsPerSample / 8).
            BitsPerSample   (2 bytes) : Number of bits per sample

        [Chunk containing the sampled data]
            DataBlocID      (4 bytes) : Identifier « data »  (0x64, 0x61, 0x74, 0x61)
            DataSize        (4 bytes) : SampledData size
            SampledData
    */
    #pragma pack(push, 1)
    struct WavHeader {
        char  riff[4];
        DWORD fileSize;
        char  wave[4];
        char  fmt_[4];
        DWORD fmtSize;
        WORD  audioFormat;
        WORD  channels;
        DWORD sampleRate;
        DWORD byteRate;
        WORD  blockAlign;
        WORD  bitsPerSample;
        char  data[4];
        DWORD dataSize;
    };
    #pragma pack(pop)

    static bool ExportRange(const std::string           &path,
                            const std::vector<uint8_t>  &pcm,
                            size_t                      byteStart,
                            size_t                      byteEnd,
                            const WAVEFORMATEX          &wfx)
    {
        if (byteStart >= byteEnd || byteEnd > pcm.size()){
            return false;
        }

        FILE *f = fopen(path.c_str(), "wb");
        if (!f) return false;

        size_t dataSize = byteEnd - byteStart;

        WavHeader h{};
        memcpy(h.riff,  "RIFF", 4);
        h.fileSize      = (DWORD)(36 + dataSize);
        memcpy(h.wave,  "WAVE", 4);
        memcpy(h.fmt_,  "fmt ", 4);
        h.fmtSize       = 16;
        h.audioFormat   = 1;
        h.channels      = wfx.nChannels;
        h.sampleRate    = wfx.nSamplesPerSec;
        h.byteRate      = wfx.nAvgBytesPerSec;
        h.blockAlign    = wfx.nBlockAlign;
        h.bitsPerSample = wfx.wBitsPerSample;
        memcpy(h.data,  "data", 4);
        h.dataSize      = (DWORD)dataSize;

        fwrite(&h, sizeof(h), 1, f);
        fwrite(pcm.data() + byteStart, 1, dataSize, f);
        fclose(f);
        return true;
    }
};

struct ProcEntry 
{
    DWORD       pid;
    std::string name;
    std::string label;

    ProcEntry(DWORD p, const char* n)
    : pid(p), name(n)
    {
        char buf[MAX_PATH + 32];
        snprintf(buf, sizeof(buf), "%s  (%lu)", n, (unsigned long)p);
        label = buf;
    }
    ProcEntry(DWORD p, const char* n, const char* l)
    : pid(p), name(n), label(l)
    {
    }
};

/*
    https://learn.microsoft.com/en-us/windows/win32/psapi/enumerating-all-processes 
 */
static std::vector<ProcEntry> EnumProcesses()
{
    std::vector<ProcEntry> out;
    out.push_back({ 0, "(System / All)", "(System / All)" });

    // Get the list of process identifiers
    DWORD pids[2048];
    DWORD cb = 0;
    DWORD count = 0;
    if (!EnumProcesses(pids, sizeof(pids), &cb)) {
        return out;
    }

    // calculate how many returned
    count = cb / sizeof(DWORD);

    out.reserve(count);

    for (DWORD i = 0; i < count; ++i)
    {
        if (!pids[i]){
            continue;
        }

        // Get a handle to the process
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | 
                               PROCESS_VM_READ,
                               FALSE, pids[i]);
        if (!h) {   
            continue;
        }

        HMODULE hMod;
        DWORD cbNeeded;
        // Get a handle to the module
        if (EnumProcessModules(h, &hMod, sizeof(hMod), &cbNeeded) )
        {
            char buf[MAX_PATH] = {};
            if (GetModuleBaseNameA(h, hMod, buf, MAX_PATH) && buf[0])
            {
                out.emplace_back(pids[i], buf);
            }
        }
        CloseHandle(h);
    }

    // keep the all processes first always!
    std::sort(out.begin() + 1, out.end(),
    [](const ProcEntry &a, const ProcEntry &b)
    {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    return out;
}

struct FgInfo { DWORD pid; char name[MAX_PATH]; };

static FgInfo GetForegroundProcessInfo()
{
    FgInfo info{};
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return info;
    GetWindowThreadProcessId(hwnd, &info.pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, info.pid);
    if (h) {
        GetModuleBaseNameA(h, nullptr, info.name, MAX_PATH);
        CloseHandle(h);
    }
    return info;
}

/*
    https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/
    https://github.com/microsoft/windows-classic-samples/tree/main/Samples/ApplicationLoopback

*/
class ActivationHandler
    : public IActivateAudioInterfaceCompletionHandler
    , public IAgileObject
{
public:
    HANDLE          hDone    = nullptr;
    HRESULT         hrResult = E_PENDING;
    IAudioClient   *pClient  = nullptr;

    ActivationHandler() 
    { 
        hDone = CreateEventW(nullptr, FALSE, FALSE, nullptr); 
    }
    ~ActivationHandler() 
    { 
        if (hDone) 
            CloseHandle(hDone); 
    }

    ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override
    {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject))
        { 
            *pp = this; return S_OK; 
        }
        *pp = nullptr; 
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation *op) override
    {
        HRESULT hrAct = E_FAIL;
        IUnknown *pUnk = nullptr;
        op->GetActivateResult(&hrAct, &pUnk);
        if (SUCCEEDED(hrAct) && pUnk)
            pUnk->QueryInterface(__uuidof(IAudioClient), (void**)&pClient);
        if (pUnk) pUnk->Release();
        hrResult = hrAct;
        SetEvent(hDone);
        return S_OK;
    }
};

enum class RecState { Idle, Recording, Paused,
                      ReadyToExport  
                    };

static const WAVEFORMATEX kProcFmt = {
    WAVE_FORMAT_PCM, 2, 44100, 44100*2*2, 4, 16, 0
};

static bool NormaliseMixFormat(WAVEFORMATEX *pwfx, WAVEFORMATEX &out)
{
    if (!pwfx) return false;
    out = {};
    out.wFormatTag      = WAVE_FORMAT_PCM;
    out.nChannels       = pwfx->nChannels;
    out.nSamplesPerSec  = pwfx->nSamplesPerSec;
    out.wBitsPerSample  = 16;
    out.nBlockAlign     = out.nChannels * 2;
    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
    out.cbSize          = 0;
    return true;
}

/*
    The Windows Audio Session API (WASAPI) enables client applications to manage the flow of audio data
    between the application and an audio endpoint device.
 */

struct Recorder
{
    /* WASAPI */
    /*
        Enables a client to create and initialize an audio stream 
        between an audio application and the audio engine or 
        the hardware buffer of an audio endpoint device.
    */
    /*
        Enables a client to read input data from a capture endpoint buffer. 
    */
    IAudioClient        *pClient  = nullptr;
    IAudioCaptureClient *pCapture = nullptr;

    std::thread          thread;
    std::atomic<bool>    stopReq{false};

    std::atomic<RecState> state{RecState::Idle};

    std::vector<uint8_t> pcm;
    std::mutex           pcmMtx;

    WAVEFORMATEX         capFmt{};

    std::string statusMsg;
    float       recordSecs  = 0.f;
    DWORD       startTick   = 0;
    float       pausedSecs  = 0.f;

    DWORD  targetPid = 0;

    int64_t cropFrameStart = 0;
    int64_t cropFrameEnd   = 0;   
    int64_t totalFrames    = 0;  

    /* 
        https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
        https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream
    */
    bool Start(DWORD pid)
    {
        if (state != RecState::Idle) 
            return false;

        targetPid = pid;

        HRESULT hr;
        ActivationHandler handler;

        if (pid == 0)
        {
            /* 
                The IMMDevice interface encapsulates 
                the generic features of a multimedia device resource. 

            */
            IMMDeviceEnumerator *pEnum = nullptr;
            IMMDevice           *pDev  = nullptr;

            // The application thread that uses this interface must be initialized for COM.
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnum);
            if (FAILED(hr)) 
            { 
                statusMsg = "CoCreateInstance failed"; 
                return false; 
            }

            hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
            pEnum->Release();

            if (FAILED(hr)) 
            { 
                statusMsg = "No default playback device";
                return false; 
            }

            /*
                To access the WASAPI interfaces, a client first obtains a reference to the IAudioClient interface 
                of an audio endpoint device by calling the IMMDevice::Activate method with parameter 
                iid set to REFIID IID_IAudioClient.
             */
            hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                nullptr, (void**)&pClient);
            pDev->Release();

            if (FAILED(hr)) 
            { 
                statusMsg = "Activate failed"; 
                return false; 
            }

            WAVEFORMATEX *pwfxMix = nullptr;
            hr = pClient->GetMixFormat(&pwfxMix);

            if (FAILED(hr)) 
            { 
                statusMsg = "GetMixFormat failed"; 
                Cleanup(); return false;     
            }

            if (!NormaliseMixFormat(pwfxMix, capFmt))
            {
                CoTaskMemFree(pwfxMix);
                statusMsg = "Bad mix format";
                Cleanup(); return false;
            }
            CoTaskMemFree(pwfxMix);

            /*
                The client calls the IAudioClient::Initialize 
                method to initialize a stream on an endpoint device.  
             */
            hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     10000000, 0, &capFmt, nullptr);
        }
        else
        {
            capFmt = kProcFmt;

            AUDIOCLIENT_ACTIVATION_PARAMS p = {};
            p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            p.ProcessLoopbackParams.TargetProcessId  = pid;
            p.ProcessLoopbackParams.ProcessLoopbackMode =
                PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

            PROPVARIANT pv = {};
            pv.vt             = VT_BLOB;
            pv.blob.cbSize    = sizeof(p);
            pv.blob.pBlobData = (BYTE*)&p;

            IActivateAudioInterfaceAsyncOperation *asyncOp = nullptr;
            hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &pv, &handler, &asyncOp);
            if (FAILED(hr)) { statusMsg = "ActivateAudioInterfaceAsync failed"; return false; }

            WaitForSingleObject(handler.hDone, 5000);
            if (asyncOp) asyncOp->Release();

            if (FAILED(handler.hrResult) || !handler.pClient)
            {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "Activation failed (0x%08X). Is the process still running?",
                         (unsigned)handler.hrResult);
                statusMsg = buf;
                return false;
            }
            pClient = handler.pClient;

            hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     10000000, 0, &capFmt, nullptr);
        }

        if (FAILED(hr)) 
        {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "IAudioClient::Initialize failed (0x%08X)", (unsigned)hr);
            statusMsg = buf;
            Cleanup(); return false;
        }

        /*
            After initializing a stream, the client can obtain references to the other WASAPI 
            interfaces by calling the IAudioClient::GetService method.
         */
        hr = pClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCapture);

        if (FAILED(hr)) 
        { 
            statusMsg = "GetService failed"; 
            Cleanup(); 
            return false; 
        }

        {
            std::lock_guard<std::mutex> lk(pcmMtx);
            pcm.clear();
        }

        statusMsg.clear();
        recordSecs  = 0.f;
        pausedSecs  = 0.f;
        startTick   = GetTickCount();

        // start recording
        pClient->Start();
        stopReq = false;
        state   = RecState::Recording;
        thread  = std::thread([this](){ ThreadFunc(); });
        return true;
    }

    void TogglePause()
    {
        if (state == RecState::Recording)
        {
            pausedSecs += (GetTickCount() - startTick) / 1000.f;
            pClient->Stop();
            state = RecState::Paused;
            statusMsg = "Paused";
        }
        else if (state == RecState::Paused)
        {
            startTick = GetTickCount();
            pClient->Start();
            state = RecState::Recording;
            statusMsg.clear();
        }
    }

    void Stop()
    {
        RecState cur = state.load();
        if (cur == RecState::Idle || cur == RecState::ReadyToExport) 
            return;

        stopReq = true;
        if (cur == RecState::Paused) 
            pClient->Start(); /* let thread drain & exit */

        if (thread.joinable()) thread.join();

        Cleanup();

        {
            std::lock_guard<std::mutex> lk(pcmMtx);
            int frameBytes = (int)(capFmt.nBlockAlign);
            totalFrames    = (frameBytes > 0)
                             ? (int64_t)(pcm.size() / (size_t)frameBytes)
                             : 0;
            cropFrameStart = 0;
            cropFrameEnd   = totalFrames;
        }

        if (totalFrames > 0)
        {
            state     = RecState::ReadyToExport;
            statusMsg = "Adjust crop region, then press Save.";
        }
        else
        {
            state     = RecState::Idle;
            statusMsg = "Nothing captured - play audio while recording.";
        }
    }

    void Discard()
    {
        if (state != RecState::ReadyToExport) return;
        { std::lock_guard<std::mutex> lk(pcmMtx); pcm.clear(); }
        totalFrames    = 0;
        cropFrameStart = 0;
        cropFrameEnd   = 0;
        statusMsg.clear();
        state = RecState::Idle;
    }

    void ThreadFunc()
    {
        while (!stopReq)
        {
            Sleep(10);
            if (state == RecState::Paused) continue;

            UINT32 pktSize = 0;
            if (FAILED(pCapture->GetNextPacketSize(&pktSize))) 
                break;

            while (pktSize > 0)
            {
                BYTE *pData; 
                UINT32 nFrames; 
                DWORD flags;

                if (FAILED(pCapture->GetBuffer(&pData, &nFrames, &flags, nullptr, nullptr))) 
                    break;

                DWORD bytes = nFrames * capFmt.nBlockAlign;
                {
                    std::lock_guard<std::mutex> lk(pcmMtx);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        pcm.insert(pcm.end(), bytes, 0);
                    else
                        pcm.insert(pcm.end(), pData, pData + bytes);
                }
                pCapture->ReleaseBuffer(nFrames);
                if (FAILED(pCapture->GetNextPacketSize(&pktSize))) pktSize = 0;
            }
        }
    }

    void Cleanup()
    {
        if (pClient)  { pClient->Stop(); pClient->Release();  pClient  = nullptr; }
        if (pCapture) { pCapture->Release(); pCapture = nullptr; }
    }

    ~Recorder() {
        RecState cur = state.load();
        if (cur == RecState::Recording || cur == RecState::Paused)
        {
            stopReq = true;
            if (cur == RecState::Paused && pClient) pClient->Start();
            if (thread.joinable()) thread.join();
            Cleanup();
        }
    }
};

static Recorder               g_rec;
static std::vector<ProcEntry> g_procs;
static int                    g_selProc = 0;
static char                   g_procFilter[128] = {};

static void FrameToTimeStr(int64_t frame, uint32_t sampleRate, char *buf, size_t bufsz)
{
    if (sampleRate == 0) { snprintf(buf, bufsz, "0:00.000"); return; }
    double secs  = (double)frame / (double)sampleRate;
    int    m     = (int)(secs / 60.0);
    double s     = secs - m * 60.0;
    snprintf(buf, bufsz, "%d:%06.3f", m, s);
}

static void DrawWaveform(ImDrawList        *dl,
                         ImVec2             canvasPos,
                         float              canvasW,
                         float              canvasH,
                         const std::vector<uint8_t> &snap,
                         const WAVEFORMATEX &fmt,
                         RecState           rs,
                         int64_t            *selF0,   
                         int64_t            *selF1)  
{
    dl->AddRectFilled(canvasPos,
                      ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                      IM_COL32(18, 18, 22, 255));

    float cy = canvasPos.y + canvasH * 0.5f;


    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiID id = window->GetID("waveform_crop");
    ImGuiContext& g = *GImGui;

    ImRect canvas_bb(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH));
    ImGui::ItemSize(canvas_bb);
    if (!ImGui::ItemAdd(canvas_bb, id))
        return;

    bool hovered = ImGui::ItemHoverable(canvas_bb, id, g.LastItemData.ItemFlags);

    dl->AddLine(ImVec2(canvasPos.x, cy),
                ImVec2(canvasPos.x + canvasW, cy),
                IM_COL32(45, 45, 55, 255));

    const int channels     = (fmt.nChannels     > 0) ? (int)fmt.nChannels     : 2;
    const int bytesPerSamp = (fmt.wBitsPerSample > 0) ? (int)(fmt.wBitsPerSample / 8) : 2;
    const int frameBytes   = channels * bytesPerSamp;

    size_t snapFrames = (frameBytes > 0) ? snap.size() / (size_t)frameBytes : 0;

    if (snapFrames < 2 || canvasW < 2.f)
    {
        dl->AddLine(ImVec2(canvasPos.x + canvasW * 0.3f, cy),
                    ImVec2(canvasPos.x + canvasW * 0.7f, cy),
                    IM_COL32(60, 60, 70, 180), 1.f);
        return;
    }

    int cols = (int)canvasW;

    ImU32 waveColSel  =
        (rs == RecState::Recording)    ? IM_COL32(220,  60,  60, 230) :
        (rs == RecState::Paused)       ? IM_COL32(220, 180,  40, 200) :
        (rs == RecState::ReadyToExport)? IM_COL32( 80, 200, 120, 230) :
                                         IM_COL32(100, 160, 200, 180);
    ImU32 waveColDim  = IM_COL32(60, 60, 70, 130);

    if (selF1 > selF0 && (int64_t)snapFrames > 0)
    {
        float x0sel = canvasPos.x + (float)canvasW * ((float)*selF0 / (float)snapFrames);
        float x1sel = canvasPos.x + (float)canvasW * ((float)*selF1 / (float)snapFrames);
        x0sel = x0sel < canvasPos.x ? canvasPos.x : x0sel;
        x1sel = x1sel > canvasPos.x + canvasW ? canvasPos.x + canvasW : x1sel;
        dl->AddRectFilled(ImVec2(x0sel, canvasPos.y),
                          ImVec2(x1sel, canvasPos.y + canvasH),
                          IM_COL32(80, 200, 120, 18));
    }

    for (int col = 0; col < cols; ++col)
    {
        size_t f0 = (size_t)((double)col       / cols * snapFrames);
        size_t f1 = (size_t)((double)(col + 1) / cols * snapFrames);
        if (f1 <= f0) f1 = f0 + 1;
        if (f1 > snapFrames) f1 = snapFrames;

        float peak_min =  1.f;
        float peak_max = -1.f;

        for (size_t f = f0; f < f1; ++f)
        {
            float mixed = 0.f;
            for (int ch = 0; ch < channels; ++ch)
            {
                size_t byteOff = f * (size_t)frameBytes + (size_t)(ch * bytesPerSamp);
                float s = 0.f;
                if (bytesPerSamp == 2)
                {
                    int16_t raw;
                    memcpy(&raw, snap.data() + byteOff, 2);
                    s = raw / 32768.f;
                }
                else if (bytesPerSamp == 4)
                {
                    float raw;
                    memcpy(&raw, snap.data() + byteOff, 4);
                    s = raw;
                }
                mixed += s;
            }
            mixed /= (float)channels;
            if (mixed < peak_min) peak_min = mixed;
            if (mixed > peak_max) peak_max = mixed;
        }

        peak_min = peak_min < -1.f ? -1.f : (peak_min > 1.f ? 1.f : peak_min);
        peak_max = peak_max < -1.f ? -1.f : (peak_max > 1.f ? 1.f : peak_max);

        float x  = canvasPos.x + (float)col;
        float y0 = canvasPos.y + (1.f - peak_max) * 0.5f * canvasH;
        float y1 = canvasPos.y + (1.f - peak_min) * 0.5f * canvasH;
        if (y1 - y0 < 1.f) { float mid = (y0 + y1) * 0.5f; y0 = mid - 0.5f; y1 = mid + 0.5f; }

        bool inSel = true;
        if (selF1 > selF0 && (int64_t)snapFrames > 0)
        {
            int64_t colFrame = (int64_t)f0; /* representative frame for this col */
            inSel = (colFrame >= *selF0 && colFrame < *selF1);
        }

        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), inSel ? waveColSel : waveColDim, 1.f);
    }
    float xStart = canvasPos.x + (float)canvasW * ((float)(*selF0) / (float)snapFrames);
    float xEnd   = canvasPos.x + (float)canvasW * ((float)(*selF1) / (float)snapFrames);

    const float grabRadius = 6.0f;
    enum DragTarget { None, Start, End };
    static DragTarget active = None;

    if (hovered && ImGui::IsMouseClicked(0))
    {
        float mx = ImGui::GetIO().MousePos.x;

        float dStart = fabsf(mx - xStart);
        float dEnd   = fabsf(mx - xEnd);

        active = (dStart < dEnd) ? Start : End;

        ImGui::SetActiveID(id, window);
        ImGui::FocusWindow(window);
    }
    if (ImGui::GetActiveID() == id && ImGui::IsMouseDown(0))
    {
        float mx = ImGui::GetIO().MousePos.x;
        float t = (mx - canvasPos.x) / canvasW;
        t = (t < 0.f) ? 0.f : (t > 1.f ? 1.f : t);

        int64_t frame = (int64_t)(t * snapFrames);

        if (active == Start)
        {
            *selF0 = frame;
            if (*selF0 > *selF1) *selF0 = *selF1;
        }
        else if (active == End)
        {
            *selF1 = frame;
            if (*selF1 < *selF0) *selF1 = *selF0;
        }
    }
    else if (!ImGui::IsMouseDown(0))
    {
        active = None;
        if (ImGui::GetActiveID() == id)
            ImGui::ClearActiveID();
    }
    if (rs == RecState::ReadyToExport && *selF1 > *selF0 && (int64_t)snapFrames > 0)
    {
        auto drawHandle = [&](int64_t frame, ImU32 col)
        {
            float x = canvasPos.x + (float)canvasW * ((float)frame / (float)snapFrames);

            dl->AddLine(ImVec2(x, canvasPos.y),
                        ImVec2(x, canvasPos.y + canvasH),
                        col, 2.f);

            dl->AddCircleFilled(ImVec2(x, cy), 3.5f, col);
        };
        drawHandle(*selF0, IM_COL32(80, 220, 120, 220));   /* green start */
        drawHandle(*selF1, IM_COL32(220, 100, 60, 220));   /* orange end  */
    }
}

void RecorderWindow()
{
    RecState rs = g_rec.state.load();

    if (rs == RecState::Recording)
        g_rec.recordSecs = g_rec.pausedSecs +
                           (GetTickCount() - g_rec.startTick) / 1000.f;

    /* ------------------------------------------------------------------ */
    ImGui::Begin("Audio Recorder");
    {
        DWORD s  = (DWORD)g_rec.recordSecs;
        DWORD h  = s / 3600; s %= 3600;
        DWORD m  = s / 60;   s %= 60;
        char buf[32];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
        ImGui::SetWindowFontScale(8.0f);
        float w = ImGui::CalcTextSize(buf).x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextUnformatted(buf);
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::Spacing();

    if (rs == RecState::Recording) {
        float alpha = (sinf((float)GetTickCount() / 500.f * 3.14159f) > 0.f) ? 1.f : 0.3f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.2f, 0.2f, alpha));
        float w = ImGui::CalcTextSize("● RECORDING").x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextUnformatted("● RECORDING");
        ImGui::PopStyleColor();
    } else if (rs == RecState::Paused) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.f, 1.f));
        float w = ImGui::CalcTextSize("⏸ PAUSED").x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextUnformatted("⏸ PAUSED");
        ImGui::PopStyleColor();
    } else if (rs == RecState::ReadyToExport) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.5f, 1.f));
        float w = ImGui::CalcTextSize("✂ CROP & SAVE").x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextUnformatted("✂ CROP & SAVE");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.f));
        float w = ImGui::CalcTextSize("◼ IDLE").x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextUnformatted("◼ IDLE");
        ImGui::PopStyleColor();
    }
  
    bool isIdle          = (rs == RecState::Idle);
    bool isReadyToExport = (rs == RecState::ReadyToExport);

    float avail = ImGui::GetContentRegionAvail().x;
    float sp    = ImGui::GetStyle().ItemSpacing.x;
    float btnW3 = (avail - sp * 2) / 3.f;

    if (!isReadyToExport)
    {
        ImGui::BeginDisabled(!isIdle || g_procs.empty());
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.25f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.70f, 0.30f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.40f, 0.18f, 1.f));
        if (ImGui::Button("▶  Start", ImVec2(btnW3, 40)))
        {
            DWORD pid = g_procs.empty() ? 0 : g_procs[g_selProc].pid;
            if (!g_rec.Start(pid))
                g_rec.statusMsg = "Failed to start. " + g_rec.statusMsg;
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::SameLine();

        /* Pause / Resume */
        bool canPause = (rs == RecState::Recording || rs == RecState::Paused);
        ImGui::BeginDisabled(!canPause);
        const char *pauseLbl = (rs == RecState::Paused) ? "▶  Resume" : "⏸  Pause";
        ImVec4 pauseCol  = (rs == RecState::Paused)
                           ? ImVec4(0.15f, 0.45f, 0.75f, 1.f)
                           : ImVec4(0.65f, 0.55f, 0.05f, 1.f);
        ImVec4 pauseHov  = (rs == RecState::Paused)
                           ? ImVec4(0.20f, 0.60f, 0.95f, 1.f)
                           : ImVec4(0.85f, 0.72f, 0.07f, 1.f);
        ImVec4 pauseAct  = (rs == RecState::Paused)
                           ? ImVec4(0.10f, 0.35f, 0.55f, 1.f)
                           : ImVec4(0.50f, 0.42f, 0.04f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Button,        pauseCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pauseHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  pauseAct);
        if (ImGui::Button(pauseLbl, ImVec2(btnW3, 40)))
            g_rec.TogglePause();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::SameLine();

        /* Stop */
        ImGui::BeginDisabled(isIdle);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.10f, 0.10f, 1.f));
        if (ImGui::Button("■  Stop", ImVec2(btnW3, 40)))
            g_rec.Stop();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        /* PCM size + format indicator */
        if (!isIdle)
        {
            size_t bytes;
            { std::lock_guard<std::mutex> lk(g_rec.pcmMtx); bytes = g_rec.pcm.size(); }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.5f, 1.f));
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Captured: %.1f MB   |   %u Hz / %u-bit / %uch",
                     bytes / 1048576.0,
                     (unsigned)g_rec.capFmt.nSamplesPerSec,
                     (unsigned)g_rec.capFmt.wBitsPerSample,
                     (unsigned)g_rec.capFmt.nChannels);
            ImGui::TextUnformatted(buf);
            ImGui::PopStyleColor();
        }
    }
    else
    {
        static std::vector<uint8_t> s_snap;
        static RecState             s_lastRs = RecState::Idle;

        if (s_lastRs != RecState::ReadyToExport)
        {
            std::lock_guard<std::mutex> lk(g_rec.pcmMtx);
            s_snap = g_rec.pcm;
        }
        s_lastRs = rs;

        /* Save + Discard buttons */
        float btnW2 = (avail - sp) / 2.f;

        /* Save */
        bool hasRange = (g_rec.cropFrameEnd > g_rec.cropFrameStart);
        ImGui::BeginDisabled(!hasRange);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.25f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.70f, 0.30f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.40f, 0.18f, 1.f));
        if (ImGui::Button("Save WAV", ImVec2(btnW2, 40)))
        {
            nfdchar_t  *outPath = nullptr;
            nfdfilteritem_t filters[] = {{ "WAV Audio", "wav" }};
            nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1,
                                                nullptr, "recording.wav");
            if (result == NFD_OKAY && outPath)
            {
                std::string path = outPath;
                if (path.size() < 4 || path.substr(path.size()-4) != ".wav")
                    path += ".wav";

                /* Convert frame indices to byte offsets.
                   nBlockAlign is the number of bytes per frame. */
                size_t frameBytes  = (size_t)g_rec.capFmt.nBlockAlign;
                size_t byteStart   = (size_t)g_rec.cropFrameStart * frameBytes;
                size_t byteEnd     = (size_t)g_rec.cropFrameEnd   * frameBytes;

                /* Clamp to actual pcm size (defensive) */
                if (byteEnd > s_snap.size()) byteEnd = s_snap.size();

                if (Wav::ExportRange(path, s_snap, byteStart, byteEnd, g_rec.capFmt))
                {
                    g_rec.statusMsg = "Saved: " + path;
                    /* Clear and return to Idle */
                    s_snap.clear();
                    s_lastRs = RecState::Idle;
                    g_rec.Discard();
                }
                else
                {
                    g_rec.statusMsg = "Save failed!";
                }
                NFD_FreePath(outPath);
            }
            else
            {
                g_rec.statusMsg = "Save cancelled.";
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::SameLine();

        /* Discard */
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.10f, 0.10f, 1.f));
        if (ImGui::Button("Discard", ImVec2(btnW2, 40)))
        {
            s_snap.clear();
            s_lastRs = RecState::Idle;
            g_rec.Discard();
        }
        ImGui::PopStyleColor(3);
    }

    /* ---- Status message (always visible) ---- */
    if (!g_rec.statusMsg.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.5f, 1.f));
        ImGui::TextWrapped("%s", g_rec.statusMsg.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();

    /* ---- Waveform ---- */
    {
        const float WAVE_H = 100.0f;
        const float panelW = ImGui::GetContentRegionAvail().x;

        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        std::vector<uint8_t> displaySnap;

        int64_t selF0 = 0, selF1 = 0;

        if (isReadyToExport)
        {
            std::lock_guard<std::mutex> lk(g_rec.pcmMtx);
            displaySnap = g_rec.pcm;
            selF0 = g_rec.cropFrameStart;
            selF1 = g_rec.cropFrameEnd;
        }
        else
        {
            std::lock_guard<std::mutex> lk(g_rec.pcmMtx);
            displaySnap = g_rec.pcm;
            /* No crop highlight during recording */
            selF0 = 0;
            selF1 = 0;
        }

        DrawWaveform(dl, canvas_pos, panelW, WAVE_H,
                     displaySnap, g_rec.capFmt, rs, &g_rec.cropFrameStart, &g_rec.cropFrameEnd);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!isReadyToExport)
    {
        static FgInfo fg{};
        static DWORD  lastPollTick = 0;
        DWORD now = GetTickCount();
        if (now - lastPollTick > 500) { fg = GetForegroundProcessInfo(); lastPollTick = now; }

        bool isSelf = (fg.pid == GetCurrentProcessId());

        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            isSelf ? ImVec4(0.18f, 0.18f, 0.18f, 1.f)
                   : ImVec4(0.10f, 0.22f, 0.35f, 1.f));
        ImGui::BeginChild("##fgbar", ImVec2(0, 36), false,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);

        ImGui::PushStyleColor(ImGuiCol_Text,
            isSelf ? ImVec4(0.45f, 0.45f, 0.45f, 1.f)
                   : ImVec4(0.55f, 0.85f, 1.00f, 1.f));

        char fgBuf[MAX_PATH + 64];
        if (isSelf || !fg.pid)
            snprintf(fgBuf, sizeof(fgBuf), "Active window:  (this recorder)");
        else
            snprintf(fgBuf, sizeof(fgBuf),
                     "Active window:  %s   [PID %lu]",
                     fg.name[0] ? fg.name : "?", (unsigned long)fg.pid);
        ImGui::TextUnformatted(fgBuf);
        ImGui::PopStyleColor();

        if (!isSelf && fg.pid && isIdle)
        {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize("Select").x - 16.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f,0.40f,0.60f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.55f,0.80f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f,0.28f,0.45f,1.f));
            if (ImGui::Button("Select"))
            {
                auto findPid = [&]() -> bool {
                    for (int i = 0; i < (int)g_procs.size(); ++i)
                        if (g_procs[i].pid == fg.pid) { g_selProc = i; return true; }
                    return false;
                };
                if (!findPid()) {
                    g_procs = EnumProcesses();
                    g_procFilter[0] = '\0';
                    findPid();
                }
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!isReadyToExport)
    {
        ImGui::BeginDisabled(!isIdle);

        if (ImGui::Button("↺ Refresh processes"))
        {
            g_procs    = EnumProcesses();
            g_selProc  = 0;
            g_procFilter[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##filter", "Filter...", g_procFilter, sizeof(g_procFilter));

        std::vector<int> filtered;
        for (int i = 0; i < (int)g_procs.size(); ++i)
        {
            if (!g_procFilter[0] ||
                strstr(g_procs[i].label.c_str(), g_procFilter) != nullptr)
                filtered.push_back(i);
        }

        bool selVisible = false;
        for (int fi : filtered) if (fi == g_selProc) { selVisible = true; break; }
        if (!selVisible && !filtered.empty()) g_selProc = filtered[0];

        const char *preview = g_procs.empty() ? "(none)" : g_procs[g_selProc].label.c_str();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##proc", preview))
        {
            for (int fi : filtered)
            {
                bool sel = (fi == g_selProc);
                if (ImGui::Selectable(g_procs[fi].label.c_str(), sel))
                    g_selProc = fi;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    ImGui::End();
}

const char *glsl_version = "#version 130";

static void glfw_error_callback(int error, const char *description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char **)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    g_procs   = EnumProcesses();
    g_selProc = 0;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow *window = glfwCreateWindow((int)(820 * scale), (int)(480 * scale),
                                          "Loopback Recorder", nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts     = true;
    io.ConfigDpiScaleViewports = true;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.f;
        style.Colors[ImGuiCol_WindowBg].w = 1.f;
    }

    static const ImWchar ranges[] = {
        0x0020, 0x00FF,
        0x2300, 0x23FF,  
        0x2580, 0x259F,
        0x25A0, 0x25FF,
        0,
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
                                  16.0f * scale, &cfg, ranges);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    NFD_Init();

    ImVec4 clear_color{0.12f, 0.12f, 0.14f, 1.f};

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10); continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
            ImGui::Begin("##root", nullptr,
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
            ImGui::PopStyleVar(3);
            ImGui::DockSpace(ImGui::GetID("root_ds"), ImVec2(0,0),
                             ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::End();
        }

        RecorderWindow();

        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow *bk = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(bk);
        }
        glfwSwapBuffers(window);
    }

    {
        RecState cur = g_rec.state.load();
        if (cur == RecState::Recording || cur == RecState::Paused)
        {
            g_rec.stopReq = true;
            if (cur == RecState::Paused && g_rec.pClient) g_rec.pClient->Start();
            if (g_rec.thread.joinable()) g_rec.thread.join();
            g_rec.Cleanup();
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    NFD_Quit();
    glfwDestroyWindow(window);
    glfwTerminate();
    CoUninitialize();
    return 0;
}