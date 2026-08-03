#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// 前向声明 COM 接口，避免在 audio.h 中包含 mmdeviceapi.h
struct IMMDevice;

// 单台设备的描述（用于填充下拉列表）
struct DeviceInfo {
    std::wstring id;        // IMMDevice::GetId() 返回的唯一 ID，设备重插不变
    std::wstring name;      // 人类可读名称，如 "USB Audio Device"
    bool         isDefault; // 是否为当前系统默认播放设备
};

// ---------------------------------------------------------------------------
// AudioController
//   管理一台音频端点设备的左右声道比例锁定，支持运行时切换绑定设备。
//
//   OnStateChanged / OnDeviceChanged 均在 WASAPI 音频线程触发，
//   调用方必须 PostMessage 切回 UI 线程后再操作窗口。
// ---------------------------------------------------------------------------
class AudioController {
public:
    AudioController();
    ~AudioController();

    // 初始化枚举器并绑定到当前默认设备
    bool Initialize();

    // 枚举当前所有激活的播放设备
    std::vector<DeviceInfo> EnumerateDevices();

    // 绑定到指定设备；deviceId 为空 = 绑定到当前默认设备
    // 若设备未连接则自动回退到默认设备并返回 false
    bool BindToDevice(const std::wstring& deviceId);

    std::wstring GetCurrentDeviceId()   const;
    std::wstring GetCurrentDeviceName() const;

    float        GetMaster()      const;
    float        GetLeftFactor()  const { return _leftFactor; }
    float        GetRightFactor() const { return _rightFactor; }
    std::wstring GetRatioText()   const;

    void SetBalance(float left, float right);
    void SetMasterVolume(float scalar);

    std::function<void()> OnDeviceChanged;  // 系统默认设备变化（音频线程）
    std::function<void()> OnStateChanged;   // 当前设备音量变化（音频线程）

    // 由 audio.cpp 内部的 COM 回调对象调用（音频线程）
    void HandleVolumeNotify();
    void HandleDeviceNotify();

private:
    static constexpr float kEpsilon = 0.002f;

    float             _leftFactor  = 1.0f;
    float             _rightFactor = 1.0f;
    std::atomic<bool> _isBusy{false};

    struct Impl;
    Impl* _impl;

    bool BindImpl(IMMDevice* device);

    void StoreNormalized(float l, float r);
    void ApplyLockedBalance();
    void ApplyLockedBalanceCore();

    static std::wstring GetDeviceFriendlyName(IMMDevice* device);
    static int GCD(int a, int b);
};
