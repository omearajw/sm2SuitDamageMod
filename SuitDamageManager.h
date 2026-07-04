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
    float GetDamageMultiplier();
    void SetDamageMultiplier(float multiplier);

    bool IsHealEnabled();
    int GetHealRate();
    int GetHealInterval();

    void SetHealEnabled(bool enabled);
    void SetHealRate(int rate);
    void SetHealInterval(int interval);
    bool IsWardrobeHealEnabled();
    void SetWardrobeHealEnabled(bool enabled);
    
    bool IsSafehouseHealEnabled();
    void SetSafehouseHealEnabled(bool enabled);
    
    bool IsFieldHealEnabled();
    void SetFieldHealEnabled(bool enabled);

    float GetSafehouseHealRate();
    void SetSafehouseHealRate(float rate);

    float GetFieldHealRate();
    void SetFieldHealRate(float rate);

    void ApplyFieldHeal(float delta_sec);
    void ApplySafehouseHeal(float delta_sec);
    
    int GetRespawnBehavior();
    void SetRespawnBehavior(int behavior);

    void SetCurrentLocationAsSafehouse();
    void ClearCustomSafehouse();

    void LoadSettingsFromINI();
    void SaveSettingsToINI();
}