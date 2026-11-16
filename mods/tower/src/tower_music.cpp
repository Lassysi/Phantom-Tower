// 影塔层内音乐（v14 创意：第 38 层沐小葵转二阶段循环播放主题曲《心湖》，用户 2026-11-05）。
// 播放玩家本机的 mp3（%LOCALAPPDATA%\PBZTower\music\ 下第一个 .mp3）；文件是游戏资源，只本地自用，不进公开包。
// 本机实测（2026-11-05，静音探测）：MCI 的 mpegvideo 驱动加载失败（266 / 277），MFPlay 一直停在 EMPTY；
// Media Foundation Source Reader 解码成 PCM（279.7 秒、48 kHz 双声道）+ waveOut 单缓冲循环（WHDR_BEGINLOOP|ENDLOOP）可用，位置按实时推进。
// 解码与播放都在独立线程里（解码约几百毫秒，不卡游戏线程）；PCM 解一次后缓存。
// 停止时机由引擎决定：开下一层、出塔 / 保存退出、取消本层（失败重试）。
#include "tower_music.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winmm.lib")
// Media Foundation 延迟加载：Windows N / KN 版默认没有 mfplat.dll，硬依赖会让整个 PBZTower.dll 载入失败（Insert 无反应）。
// 现在缺 MF 只是不放音乐；调用前先 LoadLibrary 确认在（Worker 里），不会触发延迟加载异常。
// 延迟加载参数由发布构建传给链接器（installeruild.ps1 的 PBZ_MODULE_LINK；#pragma 不能传 /DELAYLOAD）
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../src/core/host/log.h"

namespace towermusic {
namespace {

enum class Op { Play, Stop, Quit };
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::pair<Op, std::wstring>> g_ops;
std::thread g_worker;
std::atomic<bool> g_playing{false};
std::string g_track;

struct Pcm { std::wstring path; std::vector<BYTE> data; WAVEFORMATEX fmt{}; };
bool Decode(const std::wstring& path, Pcm* out) {
    IMFSourceReader* r = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r)) || !r) return false;
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    const HRESULT hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
    t->Release();
    UINT32 ch = 0, rate = 0;
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur)) && cur) {
        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch); cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); cur->Release();
    }
    if (FAILED(hr) || !ch || !rate) { r->Release(); return false; }
    out->data.clear();
    for (;;) {
        DWORD flags = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (s) s->Release(); break; }
        if (!s) continue;
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && b) {
            BYTE* p = nullptr; DWORD n = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &n))) { out->data.insert(out->data.end(), p, p + n); b->Unlock(); }
            b->Release();
        }
        s->Release();
        if (out->data.size() > 200u * 1024 * 1024) break;  // 上限 200 MB（约 18 分钟）
    }
    r->Release();
    out->path = path;
    out->fmt = {WAVE_FORMAT_PCM, static_cast<WORD>(ch), rate, rate * ch * 2, static_cast<WORD>(ch * 2), 16, 0};
    return !out->data.empty();
}

void Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = LoadLibraryW(L"mfplat.dll") && LoadLibraryW(L"mfreadwrite.dll") && SUCCEEDED(MFStartup(MF_VERSION));
    Pcm pcm;
    HWAVEOUT wave = nullptr;
    WAVEHDR hdr{};
    auto close = [&] {
        if (!wave) return;
        waveOutReset(wave);
        waveOutUnprepareHeader(wave, &hdr, sizeof hdr);
        waveOutClose(wave);
        wave = nullptr; hdr = {};
    };
    for (;;) {
        std::pair<Op, std::wstring> op{Op::Quit, L""};
        {
            std::unique_lock lk(g_mu);
            g_cv.wait(lk, [] { return !g_ops.empty(); });
            op = g_ops.front(); g_ops.pop_front();
        }
        if (op.first == Op::Play) {
            close();
            if (!mf || (pcm.path != op.second && !Decode(op.second, &pcm))) {
                g_playing = false;
                pbz::log::Info("影塔音乐：解码失败（Media Foundation {}），本次不放", mf ? "可用" : "不可用");
                continue;
            }
            if (waveOutOpen(&wave, WAVE_MAPPER, &pcm.fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wave = nullptr; g_playing = false; pbz::log::Info("影塔音乐：打不开声音输出，本次不放"); continue; }
            waveOutSetVolume(wave, 0xB333B333);  // 约 70%（游戏原生背景音乐照常）
            hdr.lpData = reinterpret_cast<LPSTR>(pcm.data.data());
            hdr.dwBufferLength = static_cast<DWORD>(pcm.data.size());
            hdr.dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
            hdr.dwLoops = 0xFFFFFFFF;  // 一直循环，直到停止
            waveOutPrepareHeader(wave, &hdr, sizeof hdr);
            waveOutWrite(wave, &hdr, sizeof hdr);
            g_playing = true;
            pbz::log::Info("影塔音乐：开始循环播放（{:.0f} 秒一遍）", pcm.data.size() / static_cast<double>(pcm.fmt.nAvgBytesPerSec));
        } else {
            if (wave) pbz::log::Info("影塔音乐：已停止");
            close();
            g_playing = false;
            if (op.first == Op::Quit) { if (mf) MFShutdown(); CoUninitialize(); return; }
        }
    }
}
void Push(Op op, std::wstring arg = L"") {
    {
        std::lock_guard lk(g_mu);
        if (!g_worker.joinable()) g_worker = std::thread(Worker);
        g_ops.push_back({op, std::move(arg)});
    }
    g_cv.notify_one();
}
std::wstring FindTrack() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    const std::filesystem::path dir = std::filesystem::path(base) / L"PBZTower" / L"music";
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && (e.path().extension() == L".mp3" || e.path().extension() == L".MP3")) return e.path().wstring();
    return {};
}

}  // namespace

void Play(const std::string& why) {
    if (g_playing && g_track == why) return;
    const auto track = FindTrack();
    if (track.empty()) { pbz::log::Info("影塔音乐：没找到 %LOCALAPPDATA%\\PBZTower\\music\\*.mp3，跳过（{}）", why); return; }
    g_track = why;
    g_playing = true;
    pbz::log::Info("影塔音乐：{}", why);
    Push(Op::Play, track);
}
void Stop(const char* why) {
    if (!g_playing && g_track.empty()) return;
    g_track.clear();
    g_playing = false;
    pbz::log::Info("影塔音乐：停止（{}）", why);
    Push(Op::Stop);
}
bool Playing() { return g_playing; }
void Shutdown() {
    if (!g_worker.joinable()) return;
    Push(Op::Quit);
    g_worker.join();
}

}  // namespace towermusic
