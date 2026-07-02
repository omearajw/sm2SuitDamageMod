#pragma once

namespace SuitDamageManager {
    void Init();
    void Shutdown();

    bool IsDecayEnabled();
    int GetDecayRate();
    int GetDecayInterval();
    float GetSuitHealthFraction();
    float GetSuitHealthPercent();
    float GetTimeToDestroyedSeconds();

    void SetDecayEnabled(bool enabled);
    void SetDecayRate(int rate);
    void SetDecayInterval(int interval);

    void RepairSuit();
    void SetSuitHealthFraction(float normalized);
    void SetSuitHealthPercent(float percent);
    void ApplySuitDamage(float amount);
    void ApplySuitRepair(float amount);
}