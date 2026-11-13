// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <future>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// wakes the output thread as XAudio2 finishes a buffer
struct Wake : IXAudio2VoiceCallback {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override { SetEvent(event); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::promise<void> known;
    std::future<void> ready = known.get_future();
    std::thread([render, known = std::move(known)]() mutable {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        static Wake wake;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xa->CreateMasteringVoice(&master))) {
            LOGE("audio: XAudio2 unavailable, no sound");
            known.set_value();
            return;
        }
        // the speakers: as Windows has them (5.1 from six channels up), or the setting's; a 5.1 mix on stereo
        // speakers is folded down by XAudio2
        uint32_t out = app::Get().audio_out;
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        DWORD mask = 0;
        master->GetChannelMask(&mask);
        bool surround = out == 3 || (out == 0 && details.InputChannels >= 6);
        uint32_t ch = surround ? 6 : 2;
        g_channels = ch;
        g_mono = out == 1;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = WORD(ch);
        wf.Format.nSamplesPerSec = kRate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = WORD(4 * ch);
        wf.Format.nAvgBytesPerSec = kRate * 4 * ch;
        wf.Format.cbSize = sizeof(wf) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        // 5.1 as the speakers have it when they are 5.1 (back or side pair): the same order, taken one to one
        wf.dwChannelMask = !surround ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                           : details.InputChannels == 6 ? mask
                                                         : 0x3F;  // front pair, centre, sub, back pair
        wf.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};  // IEEE float
        if (FAILED(xa->CreateSourceVoice(&voice, &wf.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &wake))) {
            LOGE("audio: XAudio2 voice of %u channels refused, no sound", ch);
            known.set_value();
            return;
        }
        LOGI("audio: %s output (Windows: %u channels, mask %lX)", out == 1 ? "mono" : surround ? "5.1" : "stereo",
             details.InputChannels, mask);
        known.set_value();
        voice->Start();
        // 5 ms buffers, 15 ms queued to start with: XAudio2 takes 10 ms a pass, so one pass and half the next are
        // ready; rendered as each buffer ends rather than polled. 10 ms buffers with 30 ms queued, checked every 2 ms,
        // put a shot's sound 30 to 40 ms behind it
        constexpr uint32_t kFrames = kRate / 200;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 20;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * ch);
        uint32_t depth = kMinQueued;
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGI("audio: output starved, queueing %u ms", depth * 5);
            }
            if (st.BuffersQueued >= depth) {
                WaitForSingleObject(wake.event, 20);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * ch];
            render(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 4 * ch;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
    ready.wait();
}

}  // namespace audio
