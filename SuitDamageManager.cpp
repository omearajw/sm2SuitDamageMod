#include "SuitDamageManager.h"

#include "hooking.h"
#include "logging.h"
#include "scan.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <algorithm>
#include <string>

// --- GLOBALS MOVED OUT OF NAMESPACE TO FIX LINKER ERRORS ---
extern "C" uintptr_t g_return_address = 0;
extern "C" void SuitDamageHookDetourAsm(); 
uintptr_t g_player_pointer = 0; 
uintptr_t g_hook_target = 0;
bool g_hook_installed = false;

std::atomic<bool> g_accumulate_damage{true}; 
std::atomic<float> g_damage_multiplier{1.0f};
float g_suit_value = 1.0f;

namespace {
    constexpr int kMinRate = 0;
    constexpr int kMaxRate = 100;
    constexpr int kMinInterval = 100;
    constexpr int kMaxInterval = 2000;
    constexpr int kNumHotkeys = 10;
    const int kHotkeyVks[kNumHotkeys] = {
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
        VK_F6, VK_F7, VK_F8, VK_F9, VK_F10
    };

    std::atomic<bool> g_running{false};
    std::atomic<bool> g_decay_enabled{false};
    
    std::atomic<int> g_decay_rate{10};
    std::atomic<int> g_decay_interval{500};
    std::thread g_worker;
    std::array<bool, kNumHotkeys> g_last_key_state{};

    // --- NEW HEAL GLOBALS ---
    std::atomic<bool> g_heal_enabled{false};
    std::atomic<int> g_heal_rate{5};      // Default heal rate
    std::atomic<int> g_heal_interval{1000}; // Default heal interval (1 second)

    std::mutex g_suit_mutex;
    std::ofstream g_debug_log;

    // Tracks the last known suit hash/state
    std::atomic<uint32_t> g_last_suit_id{0};
    
    constexpr bool kEnableInstructionPatch = true;
    constexpr DWORD kHookInstallDelayMs = 3000;

    void LogHookStatus(const char* message) {
        INFO("%s", message);
        if (!g_debug_log.is_open()) {
            g_debug_log.open("suit_damage_hook.log", std::ios::app);
        }
        if (g_debug_log.is_open()) {
            g_debug_log << message << std::endl;
            g_debug_log.flush();
        }
    }

    // Tracks the game's internal math state
    std::atomic<float> g_last_game_intended_value{1.0f};
    uintptr_t g_last_pointer = 0;

    // --- THE LOGIC BRIDGE ---
    extern "C" float SuitDamageLogic(uintptr_t pointer, float game_intended_value) {
        std::lock_guard<std::mutex> guard(g_suit_mutex);
        
        // 1. Did the memory pointer change? (e.g., Loaded a save or changed suits)
        // We reset our trackers here so we don't accidentally apply massive damage on load.
        if (pointer != g_last_pointer) {
            g_last_pointer = pointer;
            g_player_pointer = pointer; 
            g_last_game_intended_value.store(game_intended_value, std::memory_order_relaxed);
        }
        
        if (g_accumulate_damage.load(std::memory_order_relaxed)) {
            
            float last_val = g_last_game_intended_value.load(std::memory_order_relaxed);
            
            // 2. We ONLY take damage if the game's internal math drops compared to ITS OWN previous state.
            if (game_intended_value < last_val) {
                
                // Calculate the exact delta (how hard the punch was)
                float damage_taken = last_val - game_intended_value;
                
                // Apply the multiplier from your UI menu
                float mult = g_damage_multiplier.load(std::memory_order_relaxed);
                float final_damage = damage_taken * mult;
                
                // Damage our fully independent suit value
                g_suit_value = g_suit_value - final_damage;
                if (g_suit_value < 0.0f) g_suit_value = 0.0f;
            }
            
            // 3. Always sync our tracker to the game's current internal state
            g_last_game_intended_value.store(game_intended_value, std::memory_order_relaxed);
            
        } else {
            // Standard passthrough if the mod is turned off
            g_suit_value = game_intended_value; 
            g_last_game_intended_value.store(game_intended_value, std::memory_order_relaxed);
        }

        return g_suit_value; 
    }

    bool IsPatchTargetAccessible(uintptr_t target) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(target), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect;
        const bool readable = (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                         PAGE_EXECUTE_WRITECOPY)) != 0;
        return readable;
    }

    void* AllocateNear(uintptr_t target) {
        LogHookStatus("Attempting to allocate memory trampoline...");
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        uintptr_t minAddr = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
        uintptr_t maxAddr = (uintptr_t)sysInfo.lpMaximumApplicationAddress;
        uintptr_t startPage = target - (target % sysInfo.dwAllocationGranularity);
        
        for (uintptr_t addr = startPage; addr > minAddr && (target - addr) < 0x7FFFFF00; addr -= sysInfo.dwAllocationGranularity) {
            void* p = VirtualAlloc((void*)addr, sysInfo.dwAllocationGranularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) {
                LogHookStatus("Trampoline allocated successfully (Search Downwards).");
                return p;
            }
        }
        for (uintptr_t addr = startPage; addr < maxAddr && (addr - target) < 0x7FFFFF00; addr += sysInfo.dwAllocationGranularity) {
            void* p = VirtualAlloc((void*)addr, sysInfo.dwAllocationGranularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) {
                LogHookStatus("Trampoline allocated successfully (Search Upwards).");
                return p;
            }
        }
        
        LogHookStatus("CRITICAL ERROR: Failed to allocate trampoline near target.");
        return nullptr;
    }

    bool InstallSuitDamageHook() {
        LogHookStatus("Entering InstallSuitDamageHook()...");
        if (g_hook_installed) return true;

        HMODULE game_module = GetModuleHandleA("Spider-Man2.exe");
        if (!game_module) {
            LogHookStatus("CRITICAL ERROR: Could not find Spider-Man2.exe module.");
            return false;
        }
        
        uintptr_t base_address = reinterpret_cast<uintptr_t>(game_module);

        // 1. A slightly shorter, much more forgiving signature
        const char* signature = "C5 F8 11 00 90 44 89 35 ?? ?? ?? ?? 48 8B CB";
        
        // 2. Attempt the dynamic scan for Nexus users
        Scan::ScanResult result = Scan::Internal::ScanModule("Spider-Man2.exe", signature);
        
        if (!result.found) {
            LogHookStatus("WARNING: Pattern scan failed. Falling back to hardcoded offset.");
            // 3. THE FALLBACK: If the scan fails, guarantee it works on your exact machine!
            g_hook_target = base_address + 0xEE72C2;
        } else {
            g_hook_target = result.loc;
            LogHookStatus("SUCCESS: Pattern found at dynamic address!");
        }

        g_return_address = g_hook_target + 5; 

        // 4. Proceed with the memory trampoline...
        LogHookStatus("Resolving trampoline allocation...");
        void* trampoline = AllocateNear(g_hook_target);
        if (!trampoline) return false;

        LogHookStatus("Writing absolute jump to trampoline...");
        uint8_t* t = reinterpret_cast<uint8_t*>(trampoline);
        t[0] = 0xFF; t[1] = 0x25; t[2] = 0x00; t[3] = 0x00; t[4] = 0x00; t[5] = 0x00;
        *reinterpret_cast<uintptr_t*>(t + 6) = reinterpret_cast<uintptr_t>(&SuitDamageHookDetourAsm);

        LogHookStatus("Writing relative jump to game memory...");
        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            uint8_t* p = reinterpret_cast<uint8_t*>(g_hook_target);
            p[0] = 0xE9; 
            *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(trampoline) - g_hook_target - 5
            );
            VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, old_protect, &old_protect);

            g_hook_installed = true;
            LogHookStatus("SUCCESS: Manual AVX hook with Trampoline installed!");
            return true;
        }

        LogHookStatus("CRITICAL ERROR: VirtualProtect failed on game memory.");
        return false;
    }

    void RemoveSuitDamageHook() {
        if (!g_hook_installed) return;
        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            uint8_t* p = reinterpret_cast<uint8_t*>(g_hook_target);
            p[0] = 0xC5; p[1] = 0xF8; p[2] = 0x11; p[3] = 0x00; p[4] = 0x90;
            VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, old_protect, &old_protect);
        }
        g_hook_installed = false;
        g_hook_target = 0;
        g_return_address = 0;
    }

    float GetCurrentSuitFraction() {
        std::lock_guard<std::mutex> guard(g_suit_mutex);
        return g_suit_value;
    }

    void SetSuitFraction(float normalized) {
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        std::lock_guard<std::mutex> guard(g_suit_mutex);
        g_suit_value = normalized;

        if (g_player_pointer != 0) {
            *reinterpret_cast<float*>(g_player_pointer) = g_suit_value;
        }
    }

    void RepairSuitInternal() { SetSuitFraction(1.0f); }
    void ApplySuitDamageInternal(float amount) {
        float next = GetCurrentSuitFraction() - std::clamp(amount, 0.0f, 1.0f);
        SetSuitFraction(next < 0.0f ? 0.0f : next);
    }
    void ApplySuitRepairInternal(float amount) {
        float next = GetCurrentSuitFraction() + std::clamp(amount, 0.0f, 1.0f);
        SetSuitFraction(next > 1.0f ? 1.0f : next);
    }

    void SetDecayRateInternal(int rate) { g_decay_rate.store(std::clamp(rate, kMinRate, kMaxRate), std::memory_order_relaxed); }
    void SetDecayIntervalInternal(int interval) { g_decay_interval.store(std::clamp(interval, kMinInterval, kMaxInterval), std::memory_order_relaxed); }

    bool IsKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    void ShowNotification(const char* message) { INFO("%s", message); }

    void HandleHotkeys() {
        for (int index = 0; index < kNumHotkeys; ++index) {
            bool down = IsKeyDown(kHotkeyVks[index]);
            if (down && !g_last_key_state[index]) {
                switch (index) {
                    case 0: SetDecayRateInternal(g_decay_rate.load(std::memory_order_relaxed) + 1); ShowNotification("Decay Rate: increased"); break;
                    case 1: SetDecayRateInternal(g_decay_rate.load(std::memory_order_relaxed) - 1); ShowNotification("Decay Rate: decreased"); break;
                    case 2: SetDecayIntervalInternal(g_decay_interval.load(std::memory_order_relaxed) + 100); ShowNotification("Decay Interval: increased"); break;
                    case 3: SetDecayIntervalInternal(g_decay_interval.load(std::memory_order_relaxed) - 100); ShowNotification("Decay Interval: decreased"); break;
                    case 4: SetSuitFraction(1.0f); ShowNotification("Suit: Full Health"); break;
                    case 5: SetSuitFraction(0.5f); ShowNotification("Suit: 50% Damaged"); break;
                    case 6: SetSuitFraction(0.1f); ShowNotification("Suit: Critical"); break;
                    case 7: SetSuitFraction(0.0f); ShowNotification("Suit: Destroyed"); break;
                    case 8: 
                        g_decay_enabled.store(!g_decay_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed); 
                        ShowNotification(g_decay_enabled.load(std::memory_order_relaxed) ? "Suit Decay: ON" : "Suit Decay: OFF"); 
                        break;
                    case 9: ShowNotification("F10 pressed"); break;
                }
            }
            g_last_key_state[index] = down;
        }
    }

    void ApplyDecay() {
        if (!g_decay_enabled.load(std::memory_order_relaxed)) return;
        int rate = g_decay_rate.load(std::memory_order_relaxed);
        if (rate <= 0) return;

        float current_fraction = GetCurrentSuitFraction();
        float new_fraction = current_fraction - (rate / 1000.0f);
        if (new_fraction < 0.0f) new_fraction = 0.0f;

        SetSuitFraction(new_fraction);
    }

    void SetHealRateInternal(int rate) { g_heal_rate.store(std::clamp(rate, kMinRate, kMaxRate), std::memory_order_relaxed); }
    void SetHealIntervalInternal(int interval) { g_heal_interval.store(std::clamp(interval, kMinInterval, kMaxInterval), std::memory_order_relaxed); }

    void ApplyHeal() {
        if (!g_heal_enabled.load(std::memory_order_relaxed)) return;
        int rate = g_heal_rate.load(std::memory_order_relaxed);
        if (rate <= 0) return;

        float current_fraction = GetCurrentSuitFraction();
        
        // REVERSED LOGIC: Add the rate instead of subtracting
        float new_fraction = current_fraction + (rate / 1000.0f);
        if (new_fraction > 1.0f) new_fraction = 1.0f; // Clamp to max health

        SetSuitFraction(new_fraction);
    }

    void WorkerThread() {
        LogHookStatus("WorkerThread booted successfully.");
        if (kEnableInstructionPatch) {
            LogHookStatus("Delaying hook install by 3000ms...");
            Sleep(kHookInstallDelayMs);
            InstallSuitDamageHook();
        }

        // 1. Get the base address of the game for our static offset
        HMODULE game_module = GetModuleHandleA("Spider-Man2.exe");
        uintptr_t base_address = reinterpret_cast<uintptr_t>(game_module);
        
        // 2. Calculate the exact pointer you found in Cheat Engine
        uintptr_t suit_id_pointer = base_address + 0xAFB6EE8;

        DWORD last_tick = GetTickCount();
        while (g_running.load(std::memory_order_relaxed)) {
            HandleHotkeys();

            // --- THE WARDROBE CLEANSE ---
            if (base_address != 0) {
                // Read the current 4-byte Suit ID / State from memory
                uint32_t current_suit_id = *reinterpret_cast<uint32_t*>(suit_id_pointer);
                
                // If it's different from the last frame, the player changed suits!
                if (current_suit_id != g_last_suit_id.load(std::memory_order_relaxed)) {
                    
                    // Don't repair on the very first frame of the game loading
                    if (g_last_suit_id.load(std::memory_order_relaxed) != 0) {
                        SetSuitFraction(1.0f); 
                        LogHookStatus("Suit change detected! Wardrobe Cleanse applied.");
                    }
                    
                    // Sync the tracker
                    g_last_suit_id.store(current_suit_id, std::memory_order_relaxed);
                }
            }

            DWORD now = GetTickCount();
            int interval = g_decay_interval.load(std::memory_order_relaxed);
            if (now - last_tick >= static_cast<DWORD>(interval)) {
                last_tick = now;
                ApplyDecay(); // Or ApplyHeal, depending on the preset
            }

            Sleep(16);
        }
        LogHookStatus("WorkerThread shutting down.");
    }
}

namespace SuitDamageManager {
    bool g_we_initialized_minhook = false;

    void Init() {
        LogHookStatus("========================================");
        LogHookStatus("SuitDamageManager::Init() Called.");
        
        if (g_running.load(std::memory_order_relaxed)) return;

        MH_STATUS init_status = MH_Initialize();
        if (init_status == MH_OK) {
            LogHookStatus("MinHook initialized OK.");
            g_we_initialized_minhook = true;
        } else if (init_status != MH_ERROR_ALREADY_INITIALIZED) {
            LogHookStatus("CRITICAL ERROR: MH_Initialize failed completely.");
            return;
        } else {
            LogHookStatus("MinHook already initialized by another mod (Safe).");
        }

        g_running.store(true, std::memory_order_relaxed);
        g_worker = std::thread(WorkerThread);
    }

    void Shutdown() {
        LogHookStatus("SuitDamageManager::Shutdown() Called.");
        if (!g_running.load(std::memory_order_relaxed)) return;
        g_running.store(false, std::memory_order_relaxed);
        if (g_worker.joinable()) g_worker.join();
        RemoveSuitDamageHook();
        if (g_we_initialized_minhook) MH_Uninitialize();
    }

    bool IsDecayEnabled() { return g_decay_enabled.load(std::memory_order_relaxed); }
    int GetDecayRate() { return g_decay_rate.load(std::memory_order_relaxed); }
    int GetDecayInterval() { return g_decay_interval.load(std::memory_order_relaxed); }
    float GetSuitHealthFraction() { return GetCurrentSuitFraction(); }
    float GetSuitHealthPercent() { return GetCurrentSuitFraction() * 100.0f; }
    float GetDamageMultiplier();
    void SetDamageMultiplier(float multiplier);

    float GetTimeToDestroyedSeconds() {
        int rate = GetDecayRate();
        if (rate <= 0) return -1.0f;
        int interval = GetDecayInterval();
        float ticksNeeded = 1.0f / (rate / 1000.0f);
        float totalMs = ticksNeeded * interval;
        return totalMs / 1000.0f;
    }

    void SetDecayEnabled(bool enabled) { g_decay_enabled.store(enabled, std::memory_order_relaxed); }
    void SetDecayRate(int rate) { SetDecayRateInternal(rate); }
    void SetDecayInterval(int interval) { SetDecayIntervalInternal(interval); }
    void RepairSuit() { RepairSuitInternal(); }
    void SetSuitHealthFraction(float normalized) { SetSuitFraction(normalized); }
    void SetSuitHealthPercent(float percent) { SetSuitFraction(std::clamp(percent / 100.0f, 0.0f, 1.0f)); }
    void ApplySuitDamage(float amount) { ApplySuitDamageInternal(amount); }
    void ApplySuitRepair(float amount) { ApplySuitRepairInternal(amount); }
    float GetDamageMultiplier() { return g_damage_multiplier.load(std::memory_order_relaxed); }
    void SetDamageMultiplier(float multiplier) { g_damage_multiplier.store(multiplier, std::memory_order_relaxed); }
    bool IsHealEnabled() { return g_heal_enabled.load(std::memory_order_relaxed); }
    int GetHealRate() { return g_heal_rate.load(std::memory_order_relaxed); }
    int GetHealInterval() { return g_heal_interval.load(std::memory_order_relaxed); }
    
    void SetHealEnabled(bool enabled) { g_heal_enabled.store(enabled, std::memory_order_relaxed); }
    void SetHealRate(int rate) { SetHealRateInternal(rate); }
    void SetHealInterval(int interval) { SetHealIntervalInternal(interval); }
}