#include "SuitDamageManager.h"

#include "hooking.h"
#include "logging.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <algorithm>

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

    std::mutex g_suit_mutex;
    float g_suit_value = 1.0f;
    std::ofstream g_debug_log;
    uintptr_t g_hook_target = 0;
    bool g_hook_installed = false;
    constexpr bool kEnableInstructionPatch = true;
    constexpr DWORD kHookInstallDelayMs = 3000;

    // This exposes the variable to the Assembly file without mangling its name
    extern "C" uintptr_t g_return_address = 0;

    extern "C" void SuitDamageHookDetourAsm(); // Tell C++ our ASM file exists
    uintptr_t g_player_pointer = 0; // This will hold the actual memory address

    // Change this from 'bool' to 'float'
    extern "C" float SuitDamageLogic(uintptr_t pointer) {
        // Save the pointer for our UI
        g_player_pointer = pointer; 
        
        // Return our custom suit value. The ASM stub will catch this in xmm0
        // and hand it to the game's rendering engine!
        return g_suit_value; 
    }

    bool IsPatchTargetAccessible(uintptr_t target) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(target), &mbi, sizeof(mbi))) {
            return false;
        }

        if (mbi.State != MEM_COMMIT) {
            return false;
        }

        const DWORD protect = mbi.Protect;
        const bool readable = (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                         PAGE_EXECUTE_WRITECOPY)) != 0;
        return readable;
    }

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

    void __cdecl SuitDamageHookDetour() {
        static bool first_hit = true;
        if (first_hit) {
            first_hit = false;
            LogHookStatus("Suit damage hook reached.");
        }
    }

    // --- NEW: Trampoline Allocator ---
    // Finds empty memory within the 2GB limit of the game's executable
    void* AllocateNear(uintptr_t target) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        uintptr_t minAddr = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
        uintptr_t maxAddr = (uintptr_t)sysInfo.lpMaximumApplicationAddress;
        uintptr_t startPage = target - (target % sysInfo.dwAllocationGranularity);
        
        // Search downwards
        for (uintptr_t addr = startPage; addr > minAddr && (target - addr) < 0x7FFFFF00; addr -= sysInfo.dwAllocationGranularity) {
            void* p = VirtualAlloc((void*)addr, sysInfo.dwAllocationGranularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
        // Search upwards
        for (uintptr_t addr = startPage; addr < maxAddr && (addr - target) < 0x7FFFFF00; addr += sysInfo.dwAllocationGranularity) {
            void* p = VirtualAlloc((void*)addr, sysInfo.dwAllocationGranularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
        return nullptr;
    }

    bool InstallSuitDamageHook() {
        if (g_hook_installed) return true;

        HMODULE game_module = GetModuleHandleA("Spider-Man2.exe");
        if (!game_module) return false;

        uintptr_t base_address = reinterpret_cast<uintptr_t>(game_module);
        g_hook_target = base_address + 0xEE72C2;
        g_return_address = g_hook_target + 5; 

        // 1. Allocate a safe trampoline within 2GB of Spider-Man2.exe
        void* trampoline = AllocateNear(g_hook_target);
        if (!trampoline) {
            LogHookStatus("Hook setup failed: Could not allocate trampoline.");
            return false;
        }

        // 2. Write a 14-byte ABSOLUTE jump into our trampoline pointing to our DLL
        uint8_t* t = reinterpret_cast<uint8_t*>(trampoline);
        t[0] = 0xFF; t[1] = 0x25; t[2] = 0x00; t[3] = 0x00; t[4] = 0x00; t[5] = 0x00;
        *reinterpret_cast<uintptr_t*>(t + 6) = reinterpret_cast<uintptr_t>(&SuitDamageHookDetourAsm);

        // 3. Write the 5-byte RELATIVE jump into the game pointing to our nearby trampoline
        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            
            uint8_t* p = reinterpret_cast<uint8_t*>(g_hook_target);
            p[0] = 0xE9; 
            
            // Calculate distance to the trampoline (which is safely within 2GB)
            *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(trampoline) - g_hook_target - 5
            );
            
            VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, old_protect, &old_protect);

            g_hook_installed = true;
            LogHookStatus("Manual AVX hook with Trampoline installed successfully!");
            return true;
        }

        return false;
    }

    void RemoveSuitDamageHook() {
        if (!g_hook_installed) return;

        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<LPVOID>(g_hook_target), 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            // Restore the original bytes exactly as they were (C5 F8 11 00 90)
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

        // Actually push the value into the game's memory!
        if (g_player_pointer != 0) {
            *reinterpret_cast<float*>(g_player_pointer) = g_suit_value;
        }
    }

    void RepairSuitInternal() {
        SetSuitFraction(1.0f);
    }

    void ApplySuitDamageInternal(float amount) {
        amount = std::clamp(amount, 0.0f, 1.0f);
        const float current = GetCurrentSuitFraction();
        float next = current - amount;
        if (next < 0.0f) {
            next = 0.0f;
        }
        SetSuitFraction(next);
    }

    void ApplySuitRepairInternal(float amount) {
        amount = std::clamp(amount, 0.0f, 1.0f);
        const float current = GetCurrentSuitFraction();
        float next = current + amount;
        if (next > 1.0f) {
            next = 1.0f;
        }
        SetSuitFraction(next);
    }

    void SetDecayRateInternal(int rate) {
        g_decay_rate.store(std::clamp(rate, kMinRate, kMaxRate), std::memory_order_relaxed);
    }

    void SetDecayIntervalInternal(int interval) {
        g_decay_interval.store(std::clamp(interval, kMinInterval, kMaxInterval), std::memory_order_relaxed);
    }

    bool IsKeyDown(int vk) {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    void ShowNotification(const char* message) {
        INFO("%s", message);
    }

    void HandleHotkeys() {
        for (int index = 0; index < kNumHotkeys; ++index) {
            bool down = IsKeyDown(kHotkeyVks[index]);
            if (down && !g_last_key_state[index]) {
                switch (index) {
                    case 0: // F1
                        SetDecayRateInternal(g_decay_rate.load(std::memory_order_relaxed) + 1);
                        ShowNotification("Decay Rate: increased");
                        break;
                    case 1: // F2
                        SetDecayRateInternal(g_decay_rate.load(std::memory_order_relaxed) - 1);
                        ShowNotification("Decay Rate: decreased");
                        break;
                    case 2: // F3
                        SetDecayIntervalInternal(g_decay_interval.load(std::memory_order_relaxed) + 100);
                        ShowNotification("Decay Interval: increased");
                        break;
                    case 3: // F4
                        SetDecayIntervalInternal(g_decay_interval.load(std::memory_order_relaxed) - 100);
                        ShowNotification("Decay Interval: decreased");
                        break;
                    case 4: // F5
                        SetSuitFraction(1.0f);
                        ShowNotification("Suit: Full Health");
                        break;
                    case 5: // F6
                        SetSuitFraction(0.5f);
                        ShowNotification("Suit: 50% Damaged");
                        break;
                    case 6: // F7
                        SetSuitFraction(0.1f);
                        ShowNotification("Suit: Critical");
                        break;
                    case 7: // F8
                        SetSuitFraction(0.0f);
                        ShowNotification("Suit: Destroyed");
                        break;
                    case 8: // F9
                        g_decay_enabled.store(!g_decay_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
                        ShowNotification(g_decay_enabled.load(std::memory_order_relaxed) ? "Suit Decay: ON" : "Suit Decay: OFF");
                        break;
                    case 9: // F10
                        ShowNotification("F10 pressed");
                        break;
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

        // Actively write the new value to the game's memory!
        if (g_player_pointer != 0) {
            *reinterpret_cast<float*>(g_player_pointer) = new_fraction;
        }
    }

    void WorkerThread() {
        if (kEnableInstructionPatch) {
            Sleep(kHookInstallDelayMs);
            InstallSuitDamageHook();
        }

        DWORD last_tick = GetTickCount();
        while (g_running.load(std::memory_order_relaxed)) {
            HandleHotkeys();

            DWORD now = GetTickCount();
            int interval = g_decay_interval.load(std::memory_order_relaxed);
            if (now - last_tick >= static_cast<DWORD>(interval)) {
                last_tick = now;
                ApplyDecay();
            }

            Sleep(16);
        }
    }
}

namespace SuitDamageManager {
    bool g_we_initialized_minhook = false;

    void Init() {
        if (g_running.load(std::memory_order_relaxed))
            return;

        // Safely check MinHook status
        MH_STATUS init_status = MH_Initialize();
        if (init_status == MH_OK) {
            g_we_initialized_minhook = true; // We turned it on
        } else if (init_status != MH_ERROR_ALREADY_INITIALIZED) {
            WARN("Failed to initialize MinHook!");
            LogHookStatus("CRITICAL: MH_Initialize failed completely.");
            return; // Only abort if it's a real error
        }

        g_running.store(true, std::memory_order_relaxed);
        InstallSuitDamageHook();
        g_worker = std::thread(WorkerThread);
    }

    void Shutdown() {
        if (!g_running.load(std::memory_order_relaxed))
            return;

        g_running.store(false, std::memory_order_relaxed);
        if (g_worker.joinable())
            g_worker.join();
        RemoveSuitDamageHook();

        // Only shut MinHook down if WE were the ones who started it
        if (g_we_initialized_minhook) {
            MH_Uninitialize();
        }
    }

    bool IsDecayEnabled() {
        return g_decay_enabled.load(std::memory_order_relaxed);
    }

    int GetDecayRate() {
        return g_decay_rate.load(std::memory_order_relaxed);
    }

    int GetDecayInterval() {
        return g_decay_interval.load(std::memory_order_relaxed);
    }

    float GetSuitHealthFraction() {
        return GetCurrentSuitFraction();
    }

    float GetSuitHealthPercent() {
        return GetCurrentSuitFraction() * 100.0f;
    }

    float GetTimeToDestroyedSeconds() {
        int rate = GetDecayRate();
        if (rate <= 0)
            return -1.0f;

        int interval = GetDecayInterval();
        float ticksNeeded = 1.0f / (rate / 1000.0f);
        float totalMs = ticksNeeded * interval;
        return totalMs / 1000.0f;
    }

    void SetDecayEnabled(bool enabled) {
        g_decay_enabled.store(enabled, std::memory_order_relaxed);
    }

    void SetDecayRate(int rate) {
        SetDecayRateInternal(rate);
    }

    void SetDecayInterval(int interval) {
        SetDecayIntervalInternal(interval);
    }

    void RepairSuit() {
        RepairSuitInternal();
    }

    void SetSuitHealthFraction(float normalized) {
        SetSuitFraction(normalized);
    }

    void SetSuitHealthPercent(float percent) {
        SetSuitFraction(std::clamp(percent / 100.0f, 0.0f, 1.0f));
    }

    void ApplySuitDamage(float amount) {
        ApplySuitDamageInternal(amount);
    }

    void ApplySuitRepair(float amount) {
        ApplySuitRepairInternal(amount);
    }
}
