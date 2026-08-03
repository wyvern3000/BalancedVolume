// INITGUID 必须在所有使用 DEFINE_GUID 的头文件之前定义，
// 且只能在一个编译单元中定义（否则产生重复符号）。
// 效果：DEFINE_GUID(IID_IAudioEndpointVolume, ...) 会生成变量定义而非声明。
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <cmath>
#include <algorithm>

#include "audio.h"

// ============================================================
//  IAudioEndpointVolumeCallback 实现
//  WASAPI 主音量变化时在音频线程上回调 OnNotify
// ============================================================
struct VolumeCallback final : IAudioEndpointVolumeCallback {
    LONG             refCnt{1};
    AudioController* owner;

    explicit VolumeCallback(AudioController* o) : owner(o) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown ||
            riid == IID_IAudioEndpointVolumeCallback) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCnt));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refCnt);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        owner->HandleNotify();
        return S_OK;
    }
};

// ============================================================
//  AudioController::Impl  —  封装 COM 对象
// ============================================================
struct AudioController::Impl {
    IMMDeviceEnumerator*  enumerator{nullptr};
    IMMDevice*            device    {nullptr};
    IAudioEndpointVolume* vol       {nullptr};
    VolumeCallback*       callback  {nullptr};
    UINT                  channels  {0};
};

// ============================================================
//  AudioController
// ============================================================
AudioController::AudioController() : _impl(new Impl) {}

AudioController::~AudioController() {
    // 先注销回调，保证析构后不再有任何 OnNotify 调用
    if (_impl->vol && _impl->callback)
        _impl->vol->UnregisterControlChangeNotify(_impl->callback);
    if (_impl->callback)  _impl->callback->Release();
    if (_impl->vol)       _impl->vol->Release();
    if (_impl->device)    _impl->device->Release();
    if (_impl->enumerator) _impl->enumerator->Release();
    delete _impl;
}

bool AudioController::Initialize() {
    HRESULT hr;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                          CLSCTX_ALL,
                          IID_IMMDeviceEnumerator,
                          reinterpret_cast<void**>(&_impl->enumerator));
    if (FAILED(hr)) return false;

    hr = _impl->enumerator->GetDefaultAudioEndpoint(
             eRender, eMultimedia, &_impl->device);
    if (FAILED(hr)) return false;

    hr = _impl->device->Activate(IID_IAudioEndpointVolume,
                                  CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&_impl->vol));
    if (FAILED(hr)) return false;

    hr = _impl->vol->GetChannelCount(&_impl->channels);
    if (FAILED(hr)) return false;

    // 启动时从硬件读取已有比例，避免每次启动都重置平衡
    if (_impl->channels >= 2) {
        float l = 0.0f, r = 0.0f;
        _impl->vol->GetChannelVolumeLevelScalar(0, &l);
        _impl->vol->GetChannelVolumeLevelScalar(1, &r);
        StoreNormalized(l, r);
    }

    // 注册音量变化回调
    _impl->callback = new VolumeCallback(this);
    _impl->vol->RegisterControlChangeNotify(_impl->callback);

    return true;
}

// ── 属性 ────────────────────────────────────────────────────────────────────

float AudioController::GetMaster() const {
    float v = 0.0f;
    if (_impl->vol)
        _impl->vol->GetMasterVolumeLevelScalar(&v);
    return v;
}

std::wstring AudioController::GetRatioText() const {
    int l = std::max(0, static_cast<int>(std::roundf(_leftFactor  * 100.0f)));
    int r = std::max(0, static_cast<int>(std::roundf(_rightFactor * 100.0f)));
    int g = GCD(l, r);
    if (g == 0) return L"\u2013";  // "–"
    return std::to_wstring(l / g) + L" : " + std::to_wstring(r / g);
}

// ── 命令 ────────────────────────────────────────────────────────────────────

void AudioController::SetBalance(float left, float right) {
    if (left < 0.0f || right < 0.0f) return;
    StoreNormalized(left, right);
    ApplyLockedBalance();
}

void AudioController::SetMasterVolume(float scalar) {
    _isBusy.store(true, std::memory_order_relaxed);
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    if (_impl->vol)
        _impl->vol->SetMasterVolumeLevelScalar(scalar, nullptr);
    ApplyLockedBalanceCore();
    _isBusy.store(false, std::memory_order_relaxed);
}

// ── 内部 ────────────────────────────────────────────────────────────────────

void AudioController::StoreNormalized(float l, float r) {
    float mx = std::max(l, r);
    if (mx <= 0.0f) return;
    _leftFactor  = l / mx;
    _rightFactor = r / mx;
}

void AudioController::ApplyLockedBalance() {
    _isBusy.store(true, std::memory_order_relaxed);
    ApplyLockedBalanceCore();
    _isBusy.store(false, std::memory_order_relaxed);
}

void AudioController::ApplyLockedBalanceCore() {
    if (!_impl->vol || _impl->channels < 2) return;

    float master = 0.0f;
    _impl->vol->GetMasterVolumeLevelScalar(&master);
    master = std::clamp(master, 0.0f, 1.0f);

    float wantL = std::clamp(master * _leftFactor,  0.0f, 1.0f);
    float wantR = std::clamp(master * _rightFactor, 0.0f, 1.0f);

    float curL = 0.0f, curR = 0.0f;
    _impl->vol->GetChannelVolumeLevelScalar(0, &curL);
    _impl->vol->GetChannelVolumeLevelScalar(1, &curR);

    // 避免重复写入相同值，减少某些驱动上的通知抖动
    if (std::abs(curL - wantL) > kEpsilon)
        _impl->vol->SetChannelVolumeLevelScalar(0, wantL, nullptr);
    if (std::abs(curR - wantR) > kEpsilon)
        _impl->vol->SetChannelVolumeLevelScalar(1, wantR, nullptr);
}

void AudioController::HandleNotify() {
    // 本函数在音频线程上运行
    if (_isBusy.load(std::memory_order_relaxed)) return;
    ApplyLockedBalance();
    if (OnStateChanged) OnStateChanged();
}

int AudioController::GCD(int a, int b) {
    a = std::abs(a);
    b = std::abs(b);
    if (a == 0) return b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}
