#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// AudioController
//   固化左右声道比例：内部只保存归一化因子（较大声道 = 1.0），
//   每次写硬件时 channel = master * factor，确保比例在 0→100 拉回时不漂移。
//
//   OnStateChanged 回调在 WASAPI 音频线程上触发，调用方务必用 PostMessage
//   等方式切回 UI 线程再刷新界面。
// ---------------------------------------------------------------------------
class AudioController {
public:
    AudioController();
    ~AudioController();

    // 初始化 WASAPI，失败返回 false（例如无默认输出设备）
    bool Initialize();

    float        GetMaster()      const;
    float        GetLeftFactor()  const { return _leftFactor; }
    float        GetRightFactor() const { return _rightFactor; }
    std::wstring GetRatioText()   const;

    // left/right 为任意正数，只取比值
    void SetBalance(float left, float right);
    // scalar: 0.0–1.0
    void SetMasterVolume(float scalar);

    // 外部（键盘/系统）改变主音量时，在音频线程上触发
    std::function<void()> OnStateChanged;

    // 由 audio.cpp 内的 VolumeCallback::OnNotify 调用（音频线程）
    void HandleNotify();

private:
    static constexpr float kEpsilon = 0.002f;

    float             _leftFactor  = 1.0f;
    float             _rightFactor = 1.0f;
    std::atomic<bool> _isBusy{false};

    // WASAPI COM 对象封装在 Impl 中，避免把 mmdeviceapi.h 暴露给所有包含方
    struct Impl;
    Impl* _impl;

    void StoreNormalized(float l, float r);
    void ApplyLockedBalance();
    void ApplyLockedBalanceCore();

    static int GCD(int a, int b);
};
