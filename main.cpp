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

    ProcEntry()
    : pid(0), name("(unknown)"), label("unknown"){}

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
static std::vector<ProcEntry> EnumProcesses(void)
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
        if (EnumProcessModules(h, &hMod, sizeof(hMod), &cbNeeded))
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

static ProcEntry GetForegroundProcessInfo(void)
{
    ProcEntry info{};
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return info;
    GetWindowThreadProcessId(hwnd, &info.pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, info.pid);
    if (h) {
        GetModuleBaseNameA(h, nullptr, info.name.data(), MAX_PATH);
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
        https://github.com/mmozeiko/wcap/blob/main/wcap_audio_capture.h
        https://learn.microsoft.com/en-us/windows/win32/coreaudio/mmdevice-api
    */
    bool Start(DWORD pid)
    {
        if (state != RecState::Idle) 
            return false;

        targetPid = pid;

        HRESULT hr;
        ActivationHandler handler;

        if (pid == 0) // Global system playback easy
        {
            /* 
                The Windows Multimedia Device (MMDevice) API enables audio clients to discover audio endpoint devices, 
                determine their capabilities, and create driver instances for those devices. 
                To access the interfaces in the MMDevice API, a client obtains a reference to the IMMDeviceEnumerator 
                interface of a device-enumerator object by calling the CoCreateInstance function.

                IMMDeviceEnumerator provides methods for enumerating audio endpoint devices.

                Through the IMMDeviceEnumerator interface, the client can obtain references to the other interfaces 
                in the MMDevice API.

                The IMMDevice interface encapsulates 
                the generic features of a multimedia device resource. 
                currently it only represents an audio endpoint device

                The MMDevice API lets clients discover the audio endpoint devices in the system and determine 
                which devices are suitable for the application to use.

            */

            IMMDeviceEnumerator *pEnum = nullptr;
            IMMDevice           *pDev  = nullptr;

            /*
                Before enumerating the endpoint devices in the system, 
                the client must first call the Windows CoCreateInstance 
                function to create a device enumerator 
             */
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnum);
            if (FAILED(hr)) 
            { 
                statusMsg = "CoCreateInstance failed"; 
                return false; 
            }
            //pEnum->EnumAudioEndpoints(EDataFlow dataFlow, DWORD dwStateMask, IMMDeviceCollection **ppDevices)
            // We just want the default audio endpoint
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
            // get an audio client from the activated device
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
                                    // 1second
                                     10000000, 0, &capFmt, nullptr);
        }
        else // capture from specific process insane stuff 
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

            IAudioRenderClient
                Writes rendering data to an audio-rendering endpoint buffer.

            IAudioCaptureClient
                Reads captured data from an audio-capture endpoint buffer.

            IAudioSessionControl
                Communicates with the audio session manager to configure and manage the audio session that is associated with the stream.

            ISimpleAudioVolume
                Controls the volume level of the audio session that is associated with the stream.

            IChannelAudioVolume
                Controls the volume levels of the individual channels in the audio session that is associated with the stream.

            IAudioClock
                Monitors the stream data rate and stream position.
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

struct Player
{
    IAudioClient       *pClient  = nullptr;
    IAudioRenderClient *pRender  = nullptr;
    std::thread         thread;
    HANDLE              hEvent   = nullptr;   // WASAPI signals this when hungry
    HANDLE              hStop    = nullptr;   // we signal this to kill the thread

    std::atomic<bool>   playing{false};
    std::atomic<size_t> cursor{0};            // bytes consumed so far
    size_t              totalBytes{0};          
    IAudioClock         *pClock   = nullptr;

    bool Start(const std::vector<uint8_t>& pcm,
               const WAVEFORMATEX& fmt,
               int64_t frameStart, int64_t frameEnd)
    {
        IMMDeviceEnumerator *pEnum = nullptr;
        IMMDevice           *pDev  = nullptr;

        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

        pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
        pEnum->Release();

        pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient);
        pDev->Release();

        // Use the same format the recorder captured at
        pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            10000000, 0, &fmt, nullptr);

        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
        hStop  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pClient->SetEventHandle(hEvent);

        pClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender);
        pClient->GetService(__uuidof(IAudioClock), (void**)&pClock);

        int frameBytes = fmt.nBlockAlign;
        size_t byteStart = frameStart * frameBytes;
        size_t byteEnd   = frameEnd   * frameBytes;
        std::vector<uint8_t> slice(pcm.begin() + byteStart,
                                   pcm.begin() + byteEnd);

        totalBytes = slice.size();

        PrefillBuffer(slice, fmt);

        pClient->Start();
        playing = true;

        thread  = std::thread([this, slice = std::move(slice), fmt]() mutable {
            ThreadFunc(slice, fmt);
        });

        return true;
    }

    void PrefillBuffer(const std::vector<uint8_t>& slice, const WAVEFORMATEX& fmt)
    {
        UINT32 bufferFrames = 0;
        pClient->GetBufferSize(&bufferFrames);

        int    frameBytes    = fmt.nBlockAlign;
        UINT32 framesToWrite = (UINT32)std::min(
            (size_t)bufferFrames,
            slice.size() / (size_t)frameBytes
        );

        BYTE *pData = nullptr;
        if (SUCCEEDED(pRender->GetBuffer(framesToWrite, &pData)))
        {
            memcpy(pData, slice.data(), framesToWrite * frameBytes);
            cursor = framesToWrite * frameBytes;
            pRender->ReleaseBuffer(framesToWrite, 0);
        }
    }

    void ThreadFunc(const std::vector<uint8_t>& slice, const WAVEFORMATEX& fmt)
    {
        int    frameBytes   = fmt.nBlockAlign;
        HANDLE handles[2]   = { hStop, hEvent };  // hStop first = higher priority

        while (true)
        {
            // Block here — zero CPU until WASAPI needs data or we're told to stop.
            // Timeout 200ms guards against a lost wakeup (rare but possible).
            DWORD result = WaitForMultipleObjects(2, handles, FALSE, 200);

            if (result == WAIT_OBJECT_0)        // hStop fired
                break;

            // WAIT_OBJECT_0 + 1 = hEvent, or WAIT_TIMEOUT = safety top-up
            // Either way, try to fill whatever space is available
            UINT32 bufferFrames = 0, padding = 0;
            pClient->GetBufferSize(&bufferFrames);
            pClient->GetCurrentPadding(&padding);

            UINT32 available = bufferFrames - padding;
            if (available == 0) continue;

            size_t bytesLeft     = slice.size() - cursor;
            UINT32 framesToWrite = (UINT32)std::min(
                (size_t)available,
                bytesLeft / (size_t)frameBytes
            );

            if (framesToWrite == 0)
            {
                // All data is queued — wait for the engine to drain naturally,
                // but still respect hStop so we never hang
                while (true)
                {
                    DWORD r = WaitForMultipleObjects(2, handles, FALSE, 10);
                    if (r == WAIT_OBJECT_0) goto done;  // Stop() called
                    pClient->GetCurrentPadding(&padding);
                    if (padding == 0) break;
                }
                break;
            }

            BYTE *pData = nullptr;
            if (FAILED(pRender->GetBuffer(framesToWrite, &pData))) break;
            memcpy(pData, slice.data() + cursor, framesToWrite * frameBytes);
            cursor += framesToWrite * frameBytes;
            pRender->ReleaseBuffer(framesToWrite, 0);
        }

        done:
        pClient->Stop();
        playing = false;
    }

    int64_t FramesDone(uint32_t sampleRate) const
    {
        if (!playing.load() || !pClock) return -1;

        UINT64 freq = 0, pos = 0;
        pClock->GetFrequency(&freq);
        pClock->GetPosition(&pos, nullptr);

        if (freq == 0) return -1;

        return (int64_t)((double)pos / (double)freq * sampleRate);
    }

    void Stop()
    {
        if (hStop) SetEvent(hStop);          // unblocks WaitForMultipleObjects instantly
        if (thread.joinable()) thread.join(); // thread exits on next iteration, very fast
        Cleanup();
    }

    void Cleanup()
    {
        if (pClock)  { pClock->Release();  pClock  = nullptr; }
        if (pRender) { pRender->Release(); pRender = nullptr; }
        if (pClient) { pClient->Release(); pClient = nullptr; }
        if (hEvent)  { CloseHandle(hEvent); hEvent = nullptr; }
        if (hStop)   { CloseHandle(hStop);  hStop  = nullptr; }
    }
};

static void DrawWaveform(ImDrawList                 *dl,
                         ImVec2                     canvas_pos,
                         float                      canvas_w,
                         float                      canvas_h,
                         const std::vector<uint8_t> &snapshot,
                         const WAVEFORMATEX         &fmt,
                         RecState                   rs,
                         int64_t                    *sel_start_frame,   
                         int64_t                    *sel_end_frame)  
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiID id = window->GetID("waveform_crop");
    ImGuiContext& g = *GImGui;

    // draw the waveform backgrouond rect
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_w, canvas_pos.y + canvas_h),
                      IM_COL32(18, 18, 22, 255));
    

    ImRect canvas_bb(canvas_pos, ImVec2(canvas_pos.x + canvas_w, canvas_pos.y + canvas_h));
    ImGui::ItemSize(canvas_bb);
    if (!ImGui::ItemAdd(canvas_bb, id))
        return;

    bool hovered = ImGui::ItemHoverable(canvas_bb, id, g.LastItemData.ItemFlags);

    // draw the center line
    float cy = canvas_pos.y + canvas_h * 0.5f;
    dl->AddLine(ImVec2(canvas_pos.x, cy),
                ImVec2(canvas_pos.x + canvas_w, cy),
                IM_COL32(45, 45, 55, 255));

    const int channels     = (fmt.nChannels     > 0) ? (int)fmt.nChannels     : 2;
    const int bytesPerSamp = (fmt.wBitsPerSample > 0) ? (int)(fmt.wBitsPerSample / 8) : 2;
    const int frameBytes   = channels * bytesPerSamp;

    size_t snapshot_frames = (frameBytes > 0) ? snapshot.size() / (size_t)frameBytes : 0;

    if (snapshot_frames < 2 || canvas_w < 2.f)
    {
        dl->AddLine(ImVec2(canvas_pos.x + canvas_w * 0.3f, cy),
                    ImVec2(canvas_pos.x + canvas_w * 0.7f, cy),
                    IM_COL32(60, 60, 70, 180), 1.f);
        return;
    }

    int cols = (int)canvas_w;

    ImU32 waveColSel  =
        (rs == RecState::Recording)    ? IM_COL32(85,  189,  253, 230) :
        (rs == RecState::Paused)       ? IM_COL32(147, 148,  154, 200) :
        (rs == RecState::ReadyToExport)? IM_COL32( 80, 200, 120, 230) :
                                         IM_COL32(100, 160, 200, 180);
    ImU32 waveColDim  = IM_COL32(60, 60, 70, 130);

    // associating the handles with pos inside the array of samples
    float samples_start_ratio = ((float)(*sel_start_frame) / (float)snapshot_frames);
    float samples_end_ratio = ((float)(*sel_end_frame) / (float)snapshot_frames);

    if (sel_end_frame > sel_start_frame && (int64_t)snapshot_frames > 0)
    {
        float x0sel = canvas_pos.x + (float)canvas_w * samples_start_ratio;
        float x1sel = canvas_pos.x + (float)canvas_w * samples_end_ratio;
        x0sel = ImMax(x0sel, canvas_pos.x);
        x1sel = ImMin(x1sel, canvas_pos.x + canvas_w);
        // selected portion
        dl->AddRectFilled(ImVec2(x0sel, canvas_pos.y),
                          ImVec2(x1sel, canvas_pos.y + canvas_h),
                          IM_COL32(80, 200, 120, 18));
    }

    for (int col = 0; col < cols; ++col)
    {
        size_t f0 = (size_t)((double)col       / cols * snapshot_frames);
        size_t f1 = (size_t)((double)(col + 1) / cols * snapshot_frames);

        // at least a sing sample per column make sure not out of bounds
        if (f1 <= f0) f1 = f0 + 1;
        if (f1 > snapshot_frames) f1 = snapshot_frames;

        float peak_min =  1.f;
        float peak_max = -1.f;

        // iterate over samples in column
        for (size_t f = f0; f < f1; ++f)
        {
            float mixed = 0.f;
            for (int ch = 0; ch < channels; ++ch)
            {
                size_t byteOff = f * (size_t)frameBytes + (size_t)(ch * bytesPerSamp);

                float s = 0.f;
                if (bytesPerSamp == 2)
                {
                    // 16-bit pcm Converts signed int16 → float [-1, 1]
                    int16_t raw;
                    memcpy(&raw, snapshot.data() + byteOff, 2);
                    s = raw / 32768.f;
                }
                else if (bytesPerSamp == 4)
                {
                    // 32-bit float PCM:
                    float raw;
                    memcpy(&raw, snapshot.data() + byteOff, 4);
                    s = raw;
                }
                mixed += s;
            }
            // collapse to mono
            mixed /= (float)channels;

            if (mixed < peak_min) peak_min = mixed;
            if (mixed > peak_max) peak_max = mixed;
        }

        peak_min = ImClamp(peak_min, -1.0f, 1.0f);
        peak_max = ImClamp(peak_max, -1.0f, 1.0f);

        float x  = canvas_pos.x + (float)col;
        float y0 = canvas_pos.y + (1.f - peak_max) * 0.5f * canvas_h; // top 
        float y1 = canvas_pos.y + (1.f - peak_min) * 0.5f * canvas_h; // bottom

        // at least one pixel if silent
        if (y1 - y0 < 1.f) 
        { 
            float mid = (y0 + y1) * 0.5f; 
            y0 = mid - 0.5f;
            y1 = mid + 0.5f; 
        }

        // this portion of the wave form is inside the selection
        bool inSel = true;
        if (sel_end_frame > sel_start_frame && (int64_t)snapshot_frames > 0)
        {
            int64_t colFrame = (int64_t)f0; /* representative frame for this col */
            inSel = (colFrame >= *sel_start_frame && colFrame < *sel_end_frame);
        }
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), 
                    ((rs == RecState::Recording) || inSel) ? waveColSel : waveColDim, 1.f);
    }

    float xStart = canvas_pos.x + (float)canvas_w * samples_start_ratio;
    float xEnd   = canvas_pos.x + (float)canvas_w * samples_end_ratio;

    enum DragTarget { None, Start, End };
    static DragTarget active = None;

    float mx = ImGui::GetIO().MousePos.x;

    if (hovered && ImGui::IsMouseClicked(0))
    {
        float dStart = fabsf(mx - xStart);
        float dEnd   = fabsf(mx - xEnd);

        active = (dStart < dEnd) ? Start : End;

        ImGui::SetActiveID(id, window);
        ImGui::FocusWindow(window);
    }

    if (ImGui::GetActiveID() == id && ImGui::IsMouseDown(0))
    {
        float t = (mx - canvas_pos.x) / canvas_w;
        t = ImClamp(t, 0.0f, 1.0f);

        int64_t frame = (int64_t)(t * snapshot_frames);

        if (active == Start)
        {
            *sel_start_frame = frame;
            if (*sel_start_frame > *sel_end_frame) *sel_start_frame = *sel_end_frame;
        }
        else if (active == End)
        {
            *sel_end_frame = frame;
            if (*sel_end_frame < *sel_start_frame) *sel_end_frame = *sel_start_frame;
        }
    }
    else if (!ImGui::IsMouseDown(0))
    {
        active = None;
        if (ImGui::GetActiveID() == id)
            ImGui::ClearActiveID();
    }
    // draw the handles
    if (rs == RecState::ReadyToExport && *sel_end_frame > *sel_start_frame && (int64_t)snapshot_frames > 0)
    {
        auto drawHandle = [&](int64_t frame, ImU32 col)
        {
            float x = canvas_pos.x + (float)canvas_w * ((float)frame / (float)snapshot_frames);

            dl->AddLine(ImVec2(x, canvas_pos.y),
                        ImVec2(x, canvas_pos.y + canvas_h),
                        col, 2.f);

            dl->AddCircleFilled(ImVec2(x, cy), 3.5f, col);
        };
        drawHandle(*sel_start_frame, IM_COL32(80, 220, 120, 220));   /* green start */
        drawHandle(*sel_end_frame, IM_COL32(220, 100, 60, 220));   /* orange end  */
    }
}

static void TextCentered(const char* text)
{
    float w = ImGui::CalcTextSize(text).x;
    float avail = ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorPosX((avail - w) * 0.5f);
    ImGui::TextUnformatted(text);
}

static void TextCentered(const char* text, float height)
{
    float text_w = ImGui::CalcTextSize(text).x;
    float text_h = ImGui::GetTextLineHeight();

    float avail_w = ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - text_w) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (height - text_h) * 0.5f);

    ImGui::TextUnformatted(text);
}

static void TextCenteredColored(const char* text, ImVec4 col)
{
    ImGui::PushStyleColor(ImGuiCol_Text, col);
        TextCentered(text);
    ImGui::PopStyleColor();
}

static void TextCenteredColored(const char* text, ImVec4 col, float height)
{
    ImGui::PushStyleColor(ImGuiCol_Text, col);
        TextCentered(text, height);
    ImGui::PopStyleColor();
}

bool ButtonColored(const char* label,
                          const ImVec2& size,
                          ImVec4 &col,
                          ImVec4 &hovered,
                          ImVec4 &active)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  active);

    bool pressed = ImGui::Button(label, size);

    ImGui::PopStyleColor(3);
    return pressed;
}

static float BlinkAlpha(float speed = 0.002f, float minAlpha = 0.3f, float maxAlpha = 1.0f)
{
    float t = (float)GetTickCount() * speed;
    return (sinf(t) > 0.0f) ? maxAlpha : minAlpha;
}

inline float CalcHBlockItemWidth(int count)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float sp    = ImGui::GetStyle().ItemSpacing.x;
    return (avail - sp * (count - 1)) / count;
}

struct RecorderWindow
{
    Recorder                      recorder;
    Player                        player;
    RecState                      rs = RecState::Idle; 
    std::vector<ProcEntry>        procs;
    int                           self_proc = 0;        // dont choose it for recording
    char                          proc_filter[128];
    std::vector<uint8_t>          snapshot;
    RecState                      prev_rs = RecState::Idle;
    ProcEntry                     fg_proc;              // current foregrounf proc
    DWORD                         last_poll_tick = 0;   // to poll each x amount for anything
    ImGuiTextFilter               text_filter;

    DWORD                         s;
    DWORD                         h;
    DWORD                         m;

    ImVec4 start_col            = ImVec4(0.15f, 0.55f, 0.25f, 1.f);
    ImVec4 start_hover_col      = ImVec4(0.20f, 0.70f, 0.30f, 1.f);
    ImVec4 start_active_col     = ImVec4(0.10f, 0.40f, 0.18f, 1.f);

    ImVec4 pause_col         = ImVec4(0.15f, 0.45f, 0.75f, 1.f);
    ImVec4 pause_hover_col    = ImVec4(0.20f, 0.60f, 0.95f, 1.f);
    ImVec4 pause_active_col   = ImVec4(0.10f, 0.35f, 0.55f, 1.f);

    ImVec4 resume_col        = ImVec4(0.15f, 0.45f, 0.75f, 1.f);
    ImVec4 resume_hover_col   = ImVec4(0.20f, 0.60f, 0.95f, 1.f);
    ImVec4 resume_active_col  = ImVec4(0.10f, 0.35f, 0.55f, 1.f);

    ImVec4 stop_col          = ImVec4(0.65f, 0.15f, 0.15f, 1.f);
    ImVec4 stop_hover_col     = ImVec4(0.85f, 0.20f, 0.20f, 1.f);
    ImVec4 stop_active_col    = ImVec4(0.45f, 0.10f, 0.10f, 1.f);

    ImVec4 save_col          = ImVec4(0.15f, 0.55f, 0.25f, 1.f);
    ImVec4 save_hover_col     = ImVec4(0.20f, 0.70f, 0.30f, 1.f);
    ImVec4 save_active_col    = ImVec4(0.10f, 0.40f, 0.18f, 1.f);

    ImVec4 discard_col       = ImVec4(0.60f, 0.15f, 0.15f, 1.f);
    ImVec4 discard_hover_col  = ImVec4(0.80f, 0.20f, 0.20f, 1.f);
    ImVec4 discard_active_col = ImVec4(0.45f, 0.10f, 0.10f, 1.f);

    ImVec4 select_col        = ImVec4(0.15f,0.40f,0.60f,1.f);
    ImVec4 select_hover_col   = ImVec4(0.20f,0.55f,0.80f,1.f);
    ImVec4 select_active_col  = ImVec4(0.10f,0.28f,0.45f,1.f);

    void DrawTimer(void)
    {
        s  = (DWORD)recorder.recordSecs;
        h  = s / 3600; s %= 3600;
        m  = s / 60;   s %= 60;
        char buf[32];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
        ImGui::SetWindowFontScale(8.0f);
            TextCentered(buf);
        ImGui::SetWindowFontScale(1.0f);
    }

    void DrawRecordingStateIndicator(void)
    {
        if (rs == RecState::Recording) 
        {
            TextCenteredColored("● RECORDING", ImVec4(1.f, 0.2f, 0.2f, BlinkAlpha()));
        } 
        else if (rs == RecState::Paused) 
        {
            TextCenteredColored("⏸ PAUSED", ImVec4(1.f, 0.75f, 0.f, 1.f));
        } 
        else if (rs == RecState::ReadyToExport) 
        {
            TextCenteredColored("✂ CROP & SAVE", ImVec4(0.3f, 0.9f, 0.5f, 1.f));
        } 
        else 
        {
            TextCenteredColored("◼ IDLE",ImVec4(0.5f, 0.5f, 0.5f, 1.f));
        }
    }

    void DrawRecordingController(void)
    {
        float width = CalcHBlockItemWidth(3);

        ImGui::BeginDisabled(!(rs == RecState::Idle) || procs.empty());
            if (ButtonColored("▶  Start", ImVec2(width, 40),
                            start_col, start_hover_col, start_active_col))
            {
                DWORD pid = procs.empty() ? 0 : procs[self_proc].pid;
                if (!recorder.Start(pid))
                    recorder.statusMsg = "Failed to start. " + recorder.statusMsg;
            }
        ImGui::EndDisabled();

        ImGui::SameLine();

        /* Pause / Resume */
        bool can_pause = (rs == RecState::Recording || rs == RecState::Paused);
        bool is_paused = (rs == RecState::Paused); 

        ImGui::BeginDisabled(!can_pause);
            if (ButtonColored((is_paused) ? "▶  Resume" : "⏸  Pause",
                            ImVec2(width, 40),
                            (is_paused)? resume_col: pause_col,
                            (is_paused)? resume_hover_col: pause_hover_col,
                            (is_paused)? resume_active_col: pause_active_col))
            {
                recorder.TogglePause();
            }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(rs == RecState::Idle);
            if(ButtonColored("■  Stop", ImVec2(width, 40), 
                            stop_col, stop_hover_col, stop_active_col))
            {
                recorder.Stop();
            }
        ImGui::EndDisabled();

    }

    void DrawCaptureStatusInfo(void)
    {
        size_t bytes;
        { 
            std::lock_guard<std::mutex> lk(recorder.pcmMtx);
            bytes = recorder.pcm.size(); 
        }

        char buf[128];
        snprintf(buf, sizeof(buf),
                "Captured: %.1f MB   |   %u Hz / %u-bit / %uch",
                bytes / 1048576.0,
                (unsigned)recorder.capFmt.nSamplesPerSec,
                (unsigned)recorder.capFmt.wBitsPerSample,
                (unsigned)recorder.capFmt.nChannels);

        TextCenteredColored(buf, ImVec4(0.5f, 0.8f, 0.5f, 1.f));
    }

    void DrawExportController(void)
    {
        if (prev_rs != RecState::ReadyToExport)
        {
            // lock it and capture its state to draw
            std::lock_guard<std::mutex> lk(recorder.pcmMtx);
            snapshot = recorder.pcm;
        }
        prev_rs = rs;

        float w = CalcHBlockItemWidth(2);
        bool hasRange = (recorder.cropFrameEnd > recorder.cropFrameStart);

        ImGui::BeginDisabled(!hasRange);
            if(ButtonColored("Save WAV", ImVec2(w, 40), 
                            save_col, save_hover_col, save_active_col))
            {
                nfdchar_t  *outPath = nullptr;
                nfdfilteritem_t filters[] = {{ "WAV Audio", "wav" }};
                nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1,
                                                    nullptr, "recording.wav");
                if (result == NFD_OKAY && outPath)
                {
                    std::string path = outPath;
                    if (path.size() < 4 || path.substr(path.size()-4) != ".wav"){
                        path += ".wav";
                    }

                    // nBlockAlign is the number of bytes per frame. 
                    size_t frameBytes  = (size_t)recorder.capFmt.nBlockAlign;
                    size_t byteStart   = (size_t)recorder.cropFrameStart * frameBytes;
                    size_t byteEnd     = (size_t)recorder.cropFrameEnd   * frameBytes;

                    if (byteEnd > snapshot.size()){
                        byteEnd = snapshot.size();
                    }

                    if (Wav::ExportRange(path, snapshot, byteStart, byteEnd, recorder.capFmt))
                    {
                        recorder.statusMsg = "Saved: " + path;
                        /* Clear and return to Idle */
                        player.Stop();
                        snapshot.clear();
                        prev_rs = RecState::Idle;
                        recorder.Discard();
                    }
                    else
                    {
                        recorder.statusMsg = "Save failed!";
                    }
                    NFD_FreePath(outPath);
                }
                else
                {
                    recorder.statusMsg = "Save cancelled.";
                }

            }
        ImGui::EndDisabled();

        ImGui::SameLine();

        if(ButtonColored("Discard", ImVec2(w, 40), discard_col, discard_hover_col, discard_active_col))
        {
            player.Stop();
            snapshot.clear();
            prev_rs = RecState::Idle;
            recorder.Discard();
        }
    }

    void DrawStatusMessage(void)
    {
        if (!recorder.statusMsg.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.5f, 1.f));
            ImGui::TextWrapped("%s", recorder.statusMsg.c_str());
            ImGui::PopStyleColor();
        }
    }

    void DrawWaveformEditor(void)
    {
        const float WAVE_H = 100.0f;
        const float panelW = ImGui::GetContentRegionAvail().x;

        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        std::vector<uint8_t> display_snapshot;
        {
            std::lock_guard<std::mutex> lk(recorder.pcmMtx);
            display_snapshot = recorder.pcm;
        }

        DrawWaveform(dl, canvas_pos, panelW, WAVE_H,
                     display_snapshot, recorder.capFmt, rs, &recorder.cropFrameStart, &recorder.cropFrameEnd);

        int64_t frames_done      = player.FramesDone(recorder.capFmt.nSamplesPerSec);
        int64_t snapshot_frames  = (int64_t)(display_snapshot.size() / (size_t)recorder.capFmt.nBlockAlign);
        int64_t selection_frames = recorder.cropFrameEnd - recorder.cropFrameStart;

        // drawing selection player indication
        if (frames_done >= 0 && recorder.capFmt.nBlockAlign > 0)
        {
            if (snapshot_frames > 0)
            {
                frames_done = std::min(frames_done, selection_frames);

                float absFrame = (float)(recorder.cropFrameStart + frames_done);
                float px = canvas_pos.x + (absFrame / (float)snapshot_frames) * panelW;

                dl->AddLine(ImVec2(px, canvas_pos.y),
                            ImVec2(px, canvas_pos.y + WAVE_H),
                            IM_COL32(255, 220, 60, 220), 1.5f);
            }
        }
    }

    void DrawPlaybackController(void)
    {
        bool isPlaying = player.playing.load();
        float w2 = CalcHBlockItemWidth(2);

        ImGui::BeginDisabled(isPlaying || !(recorder.cropFrameEnd > recorder.cropFrameStart));
            if (ImGui::Button("▶  Play Selection", ImVec2(w2, 36))) 
            {
                player.Stop();
                std::lock_guard<std::mutex> lk(recorder.pcmMtx);
                player.Start(snapshot, recorder.capFmt, recorder.cropFrameStart, recorder.cropFrameEnd);
            }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!isPlaying);
            if (ButtonColored("■  Stop Playback", ImVec2(w2, 36),
                            stop_col, stop_hover_col, stop_active_col))
            {
                player.Stop();
            }
        ImGui::EndDisabled();
    }

    void DrawProcFgSelect(void)
    {
        if(!(rs == RecState::ReadyToExport))
        {
            bool isSelf = (fg_proc.pid == GetCurrentProcessId());

            char fgBuf[MAX_PATH + 64];
            if (isSelf || !fg_proc.pid)
            {
                snprintf(fgBuf, sizeof(fgBuf), "Active window:  (this recorder)");
            }
            else
            {
                snprintf(fgBuf, sizeof(fgBuf),
                        "Active window:  %s   [PID %lu]",
                        fg_proc.name.c_str()[0] ? fg_proc.name.c_str() : "?", (unsigned long)fg_proc.pid);
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                isSelf ? ImVec4(0.18f, 0.18f, 0.18f, 1.f)
                    : ImVec4(0.10f, 0.22f, 0.35f, 1.f));

            int fg_proc_status_h = 36;

            ImGui::BeginChild("##fgbar", ImVec2(0, fg_proc_status_h), false,
                            ImGuiWindowFlags_NoScrollbar);
                
                TextCenteredColored(fgBuf,isSelf ? ImVec4(0.45f, 0.45f, 0.45f, 1.f)
                : ImVec4(0.55f, 0.85f, 1.00f, 1.f),fg_proc_status_h);

                if (!isSelf && fg_proc.pid && (rs == RecState::Idle))
                {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize("Select").x - 16.f);
                    if(ButtonColored("Select", ImVec2(0, 30), select_col, select_hover_col, select_active_col))
                    {
                        auto findPid = [&]() -> bool {
                            for (int i = 0; i < (int)procs.size(); ++i)
                            {
                                if (procs[i].pid == fg_proc.pid) { self_proc = i; return true; }
                            }
                            return false;
                        };
                        if (!findPid()) {
                            procs = EnumProcesses();
                            proc_filter[0] = '\0';
                            findPid();
                        }
                    }
                }

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }

    void DrawProcSelect(void)
    {
        if (!(rs==RecState::ReadyToExport))
        {
            ImGui::BeginDisabled(!(rs==RecState::Idle));

                if (ImGui::Button("↺ Refresh processes"))
                {
                    procs    = EnumProcesses();
                    self_proc  = 0;
                    proc_filter[0] = '\0';
                }
                ImGui::SameLine();

                text_filter.Draw("##filter", 200.0f); 

                std::vector<int> filtered;
                for (int i = 0; i < (int)procs.size(); ++i)
                {
                    if (text_filter.PassFilter(procs[i].label.c_str()))
                    {
                        filtered.push_back(i);
                    }
                }

                bool selVisible = false;
                for (int fi : filtered)
                {
                    if (fi == self_proc) 
                    { 
                        selVisible = true;
                        break; 
                    }
                }
                if (!selVisible && !filtered.empty())
                {
                    self_proc = filtered[0];
                }

                const char *preview = procs.empty() ? "(none)" : procs[self_proc].label.c_str();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##proc", preview))
                {
                    for (int fi : filtered)
                    {
                        bool sel = (fi == self_proc);
                        if (ImGui::Selectable(procs[fi].label.c_str(), sel))
                            self_proc = fi;
                        if (sel) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

            ImGui::EndDisabled();
        }
    }

    void Update(void)
    {
        rs = recorder.state.load();
        if (rs == RecState::Recording)
        {
            recorder.recordSecs = recorder.pausedSecs +
                              (GetTickCount() - recorder.startTick) / 1000.f;
        }

        DWORD now = GetTickCount();
        if (now - last_poll_tick > 500) { fg_proc = GetForegroundProcessInfo(); last_poll_tick = now; }

    }

    void Init(void)
    {
        procs   = EnumProcesses();
        self_proc = 0;
    }

    void Draw(void)
    {
        ImGui::Begin("Audio Recorder");

            DrawTimer();

            ImGui::Spacing();

            DrawRecordingStateIndicator();

            if (!(rs == RecState::ReadyToExport))
            {
                DrawRecordingController();

                ImGui::Spacing();

                if (rs == RecState::Idle)
                {
                    DrawCaptureStatusInfo();
                }
            }
            else
            {   
                DrawExportController();
            }
            DrawStatusMessage();

            ImGui::Spacing();
            ImGui::Separator();

            DrawWaveformEditor();

            if (rs == RecState::ReadyToExport)
            {
                ImGui::Spacing();

                DrawPlaybackController();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            DrawProcFgSelect();

            ImGui::Spacing();

            DrawProcSelect();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        ImGui::End();
    }

    void Cleanup()
    {
        RecState cur = recorder.state.load();
        if (cur == RecState::Recording || cur == RecState::Paused)
        {
            recorder.stopReq = true;
            if (cur == RecState::Paused && recorder.pClient) recorder.pClient->Start();
            if (recorder.thread.joinable()) recorder.thread.join();
            recorder.Cleanup();
        }
        player.Stop();
    }
};

const char *glsl_version = "#version 130";

static void glfw_error_callback(int error, const char *description) 
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char **)
{
    // The application thread that uses this interface must be initialized for COM.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    RecorderWindow recorder;

    recorder.Init();

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) 
        return 1;

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

    while (!glfwWindowShouldClose(window)) 
    {
        glfwPollEvents();

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) 
        {
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

        recorder.Update();

        recorder.Draw();

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

    recorder.Cleanup();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    NFD_Quit();
    glfwDestroyWindow(window);
    glfwTerminate();
    CoUninitialize();
    return 0;
}