// INITGUID 必须在所有使用 DEFINE_GUID 的头文件之前定义（且只能在一个编译单元中）
#define INITGUID

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propidl.h>        // PROPVARIANT, PropVariantClear
#include <cmath>
#include <algorithm>

#include "audio.h"

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
// 手动定义，避免依赖 functiondiscoverykeys_devpkey.h（部分 MinGW 版本没有）
static const PROPERTYKEY kFriendlyName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14
};

// ============================================================
//  IAudioEndpointVolumeCallback — 当前设备音量变化回调
// ============================================================
struct VolumeCallback final : IAudioEndpointVolumeCallback {
    LONG             refCnt{1};
    AudioController* owner;
    explicit VolumeCallback(AudioController* o) : owner(o) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown ||
            riid == IID_IAudioEndpointVolumeCallback) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
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
        owner->HandleVolumeNotify();
        return S_OK;
    }
};

// ============================================================
//  IMMNotificationClient — 系统默认播放设备变化回调
// ============================================================
struct DeviceNotifier final : IMMNotificationClient {
    LONG             refCnt{1};
    AudioController* owner;
    explicit DeviceNotifier(AudioController* o) : owner(o) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown ||
            riid == IID_IMMNotificationClient) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCnt));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refCnt);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    // 只关心默认播放设备（eRender + eMultimedia）的变化
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
            EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia)
            owner->HandleDeviceNotify();
        return S_OK;
    }
    // 其余事件不需要处理，返回 S_OK 即可
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override           { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override         { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
            LPCWSTR, const PROPERTYKEY) override                        { return S_OK; }
};

// ============================================================
//  AudioController::Impl — 封装所有 COM 对象
// ============================================================
struct AudioController::Impl {
    IMMDeviceEnumerator*  enumerator  {nullptr};
    IMMDevice*            device      {nullptr};
    IAudioEndpointVolume* vol         {nullptr};
    VolumeCallback*       volCb       {nullptr};
    DeviceNotifier*       devNotifier {nullptr};
    UINT                  channels    {0};
    std::wstring          deviceId;
    std::wstring          deviceName;
};

// ============================================================
//  AudioController — 公共接口
// ============================================================

AudioController::AudioController() : _impl(new Impl) {}

AudioController::~AudioController() {
    // 先注销设备通知，防止析构期间回调触发
    if (_impl->enumerator && _impl->devNotifier)
        _impl->enumerator->UnregisterEndpointNotificationCallback(
            _impl->devNotifier);
    if (_impl->devNotifier) _impl->devNotifier->Release();

    // 注销音量回调并释放当前设备资源
    if (_impl->vol && _impl->volCb)
        _impl->vol->UnregisterControlChangeNotify(_impl->volCb);
    if (_impl->volCb)    _impl->volCb->Release();
    if (_impl->vol)      _impl->vol->Release();
    if (_impl->device)   _impl->device->Release();
    if (_impl->enumerator) _impl->enumerator->Release();
    delete _impl;
}

bool AudioController::Initialize() {
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                   CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                   reinterpret_cast<void**>(&_impl->enumerator));
    if (FAILED(hr)) return false;

    // 注册设备变化监听
    _impl->devNotifier = new DeviceNotifier(this);
    _impl->enumerator->RegisterEndpointNotificationCallback(_impl->devNotifier);

    // 绑定到当前默认设备
    IMMDevice* dev = nullptr;
    hr = _impl->enumerator->GetDefaultAudioEndpoint(
             eRender, eMultimedia, &dev);
    if (FAILED(hr)) return false;

    bool ok = BindImpl(dev);
    dev->Release();
    return ok;
}

std::vector<DeviceInfo> AudioController::EnumerateDevices() {
    std::vector<DeviceInfo> result;

    // 获取当前默认设备 ID 用于标记
    std::wstring defaultId;
    {
        IMMDevice* def = nullptr;
        if (SUCCEEDED(_impl->enumerator->GetDefaultAudioEndpoint(
                eRender, eMultimedia, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id))) {
                defaultId = id;
                CoTaskMemFree(id);
            }
            def->Release();
        }
    }

    IMMDeviceCollection* col = nullptr;
    if (FAILED(_impl->enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &col)))
        return result;

    UINT count = 0;
    col->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;

        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.name      = GetDeviceFriendlyName(dev);
        info.isDefault = (info.id == defaultId);
        result.push_back(std::move(info));
        dev->Release();
    }
    col->Release();
    return result;
}

bool AudioController::BindToDevice(const std::wstring& deviceId) {
    IMMDevice* dev    = nullptr;
    bool       found  = true;
    HRESULT    hr;

    if (deviceId.empty()) {
        hr = _impl->enumerator->GetDefaultAudioEndpoint(
                 eRender, eMultimedia, &dev);
    } else {
        hr = _impl->enumerator->GetDevice(deviceId.c_str(), &dev);
        if (FAILED(hr)) {
            // 设备未连接，回退到默认设备
            found = false;
            hr = _impl->enumerator->GetDefaultAudioEndpoint(
                     eRender, eMultimedia, &dev);
        }
    }

    if (FAILED(hr) || !dev) return false;

    bool ok = BindImpl(dev);
    dev->Release();
    return ok && found;
}

std::wstring AudioController::GetCurrentDeviceId()   const { return _impl->deviceId;   }
std::wstring AudioController::GetCurrentDeviceName() const { return _impl->deviceName; }

float AudioController::GetMaster() const {
    float v = 0.0f;
    if (_impl->vol) _impl->vol->GetMasterVolumeLevelScalar(&v);
    return v;
}

std::wstring AudioController::GetRatioText() const {
    int l = std::max(0, static_cast<int>(std::roundf(_leftFactor  * 100.0f)));
    int r = std::max(0, static_cast<int>(std::roundf(_rightFactor * 100.0f)));
    int g = GCD(l, r);
    if (g == 0) return L"\u2013";
    return std::to_wstring(l / g) + L" : " + std::to_wstring(r / g);
}

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

// ============================================================
//  AudioController — 内部实现
// ============================================================

bool AudioController::BindImpl(IMMDevice* device) {
    // 注销并释放旧设备的音量回调和接口
    if (_impl->vol && _impl->volCb)
        _impl->vol->UnregisterControlChangeNotify(_impl->volCb);
    if (_impl->volCb)  { _impl->volCb->Release();  _impl->volCb  = nullptr; }
    if (_impl->vol)    { _impl->vol->Release();     _impl->vol    = nullptr; }
    if (_impl->device) { _impl->device->Release();  _impl->device = nullptr; }
    _impl->channels  = 0;
    _impl->deviceId.clear();
    _impl->deviceName.clear();

    // 绑定新设备
    _impl->device = device;
    _impl->device->AddRef();

    // 读取设备 ID 与友好名称
    LPWSTR idStr = nullptr;
    if (SUCCEEDED(_impl->device->GetId(&idStr))) {
        _impl->deviceId = idStr;
        CoTaskMemFree(idStr);
    }
    _impl->deviceName = GetDeviceFriendlyName(_impl->device);

    // 获取音量控制接口
    HRESULT hr = _impl->device->Activate(IID_IAudioEndpointVolume,
                                          CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(&_impl->vol));
    if (FAILED(hr)) return false;

    hr = _impl->vol->GetChannelCount(&_impl->channels);
    if (FAILED(hr)) return false;

    // 从硬件当前状态反推初始比例
    if (_impl->channels >= 2) {
        float l = 0.0f, r = 0.0f;
        _impl->vol->GetChannelVolumeLevelScalar(0, &l);
        _impl->vol->GetChannelVolumeLevelScalar(1, &r);
        StoreNormalized(l, r);
    }

    // 注册音量变化回调
    _impl->volCb = new VolumeCallback(this);
    _impl->vol->RegisterControlChangeNotify(_impl->volCb);

    return true;
}

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

    if (std::abs(curL - wantL) > kEpsilon)
        _impl->vol->SetChannelVolumeLevelScalar(0, wantL, nullptr);
    if (std::abs(curR - wantR) > kEpsilon)
        _impl->vol->SetChannelVolumeLevelScalar(1, wantR, nullptr);
}

void AudioController::HandleVolumeNotify() {
    if (_isBusy.load(std::memory_order_relaxed)) return;
    ApplyLockedBalance();
    if (OnStateChanged) OnStateChanged();
}

void AudioController::HandleDeviceNotify() {
    if (OnDeviceChanged) OnDeviceChanged();
}

std::wstring AudioController::GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return L"未知设备";

    IPropertyStore* ps = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &ps)))
        return L"未知设备";

    PROPVARIANT pv = {};
    std::wstring name = L"未知设备";
    if (SUCCEEDED(ps->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        name = pv.pwszVal;
    PropVariantClear(&pv);
    ps->Release();
    return name;
}

int AudioController::GCD(int a, int b) {
    a = std::abs(a); b = std::abs(b);
    if (a == 0) return b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}
