#pragma once
// Optional non-exclusive DirectInput fallback for DS4-class Sony controllers.
// XInput remains the first choice. Other devices are not guessed from an
// arbitrary DirectInput button ordering.
//
// 1.3.0 (user 2026-11-07, "periodic hitch after loading the mod"): measured on the user's PC,
// IDirectInput8::EnumDevices takes 76-81 ms even with no controller attached. The first
// version enumerated on the game thread every 2 s whenever no XInput pad was present, i.e. a
// ~80 ms hitch every 2 s for every keyboard player. Enumeration now runs on a background
// thread (every 3 s, only while wanted); the game thread only opens devices that thread found
// (cheap) and polls them.
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace tower::sony_pad {
struct Sample { uint16_t buttons = 0; int16_t x = 0, y = 0; bool chord = false; };
inline bool Supported(unsigned vid, unsigned pid) {
    // DS4 v1/v2. Virtual DS4 v1 was verified; v2 requires physical testing.
    return vid == 0x054c && (pid == 0x05c4 || pid == 0x09cc);
}
inline Sample Map(const DIJOYSTATE2& s) {
    Sample out{};
    constexpr struct { int index; uint16_t bit; } keys[] = {
        {0,0x4000}, {1,0x1000}, {2,0x2000}, {3,0x8000},
        {4,0x0100}, {5,0x0200}, {10,0x0040}, {11,0x0080}};
    for (auto k:keys) if (s.rgbButtons[k.index] & 0x80) out.buttons |= k.bit;
    const DWORD pov = s.rgdwPOV[0];
    if (pov != 0xffffffff && pov < 36000) {
        if (pov >= 31500 || pov <= 4500) out.buttons |= 0x0001;
        if (pov >= 13500 && pov <= 22500) out.buttons |= 0x0002;
        if (pov >= 22500 && pov <= 31500) out.buttons |= 0x0004;
        if (pov >= 4500 && pov <= 13500) out.buttons |= 0x0008;
    }
    const auto axis=[](LONG n) { return static_cast<int16_t>(std::clamp<long>(n - 32768, -32767, 32767)); };
    out.x = axis(s.lX); out.y = static_cast<int16_t>(-axis(s.lY));
    out.chord = (out.buttons & 0x00c0) == 0x00c0;
    return out;
}

// Background enumeration. Shared state is heap-allocated and intentionally never freed: the
// thread is detached and may outlive any static object during process exit (no join in DllMain).
struct Scanner {
    std::mutex lock;
    std::vector<GUID> found;      // supported device instances from the latest enumeration
    unsigned generation = 0;      // bumped when an enumeration completes
    std::atomic<unsigned long long> wantedUntil{0};  // enumerate only while the game thread is actively asking
    std::atomic<bool> started{false};
    static BOOL CALLBACK Enumerate(const DIDEVICEINSTANCEW* info, void* context) {
        auto* out = static_cast<std::vector<GUID>*>(context);
        if (Supported(LOWORD(info->guidProduct.Data1), HIWORD(info->guidProduct.Data1)) && out->size() < 16) out->push_back(info->guidInstance);
        return DIENUM_CONTINUE;
    }
    void Start() {
        if (started.exchange(true)) return;
        std::thread([this] {
            IDirectInput8W* input = nullptr;
            for (;;) {
                Sleep(3000);
                if (GetTickCount64() > wantedUntil.load()) continue;
                if (!input && FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr))) { input = nullptr; continue; }
                std::vector<GUID> list;
                input->EnumDevices(DI8DEVCLASS_GAMECTRL, Enumerate, &list, DIEDFL_ATTACHEDONLY);
                std::lock_guard lk(lock);
                found = std::move(list);
                ++generation;
            }
        }).detach();
    }
};

class Reader {
    struct Device { IDirectInputDevice8W* ptr; };
    IDirectInput8W* input_ = nullptr;
    std::vector<Device> devices_;
    Scanner* scan_ = new Scanner;  // never freed (see Scanner)
    unsigned seen_ = 0;
    void Open(const std::vector<GUID>& list) {
        if (list.empty()) return;
        if (!input_ && FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&input_), nullptr))) { input_ = nullptr; return; }
        HWND window = GetForegroundWindow();
        if (!window) return;
        for (const auto& g : list) {
            IDirectInputDevice8W* device = nullptr;
            if (FAILED(input_->CreateDevice(g, &device, nullptr))) continue;
            const HRESULT format = device->SetDataFormat(&c_dfDIJoystick2);
            const HRESULT coop = device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            if (FAILED(format) || FAILED(coop) || FAILED(device->Acquire())) { device->Release(); continue; }
            devices_.push_back({device});
        }
    }
public:
    bool Read(Sample* out) {
        *out = {};
        // Ask the background thread to look for devices only while none is open.
        if (devices_.empty()) scan_->wantedUntil.store(GetTickCount64() + 5000);
        if (devices_.empty()) {
            scan_->Start();
            std::vector<GUID> list;
            {
                std::lock_guard lk(scan_->lock);
                if (scan_->generation != seen_) { seen_ = scan_->generation; list = scan_->found; }
            }
            Open(list);
        }
        bool any = false;
        for (size_t i = 0; i < devices_.size();) {
            auto* d = devices_[i].ptr; d->Poll(); DIJOYSTATE2 state{};
            HRESULT hr = d->GetDeviceState(sizeof state, &state);
            if (FAILED(hr) && (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)) {
                if (SUCCEEDED(d->Acquire())) { d->Poll(); hr = d->GetDeviceState(sizeof state, &state); }
            }
            if (FAILED(hr)) { d->Unacquire(); d->Release(); devices_.erase(devices_.begin() + i); continue; }
            ++i; any = true; const auto s = Map(state);
            out->buttons |= s.buttons; out->chord |= s.chord;
            if (abs(static_cast<int>(s.x)) + abs(static_cast<int>(s.y)) > abs(static_cast<int>(out->x)) + abs(static_cast<int>(out->y))) { out->x=s.x; out->y=s.y; }
        }
        return any;
    }
};
}  // namespace tower::sony_pad
