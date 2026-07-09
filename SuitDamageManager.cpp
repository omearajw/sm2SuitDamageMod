#include "SuitDamageManager.h"

#include "hooking.h"
#include "logging.h"
#include "scan.h"
#include "game\HeroSystem.h"
#include "game\Actor.h"
#include "game\Transform.h"

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
#include <deque>

// --- GLOBALS MOVED OUT OF NAMESPACE TO FIX LINKER ERRORS ---
extern "C" uintptr_t g_return_address = 0;
extern "C" void SuitDamageHookDetourAsm(); 
uintptr_t g_player_pointer = 0; 
uintptr_t g_hook_target = 0;
std::atomic<DWORD> g_last_hook_tick{0};
bool g_hook_installed = false;

// --- ANALYTICS GLOBALS ---
std::mutex g_analytics_mutex;
std::deque<std::pair<DWORD, float>> g_suit_history;
std::atomic<float> g_player_speed{0.0f};

extern int current_preset;

// --- NEW FEATURE GLOBALS ---
std::atomic<bool> g_wardrobe_heal_enabled{true};
std::atomic<bool> g_safehouse_heal_enabled{true};
std::atomic<bool> g_field_heal_enabled{true};

// 0 = Vanilla (Heal), 1 = Cinematic (Maintain), 2 = Survivor (Destroyed)
std::atomic<int> g_respawn_behavior{2}; 

// Track time since last hit for Field Repair delay
std::atomic<DWORD> g_last_damage_tick{0};

std::atomic<float> g_safehouse_heal_rate{10.0f}; // 10% per second
std::atomic<float> g_field_heal_rate{0.5f};      // 0.5% per second (very slow)

// --- MULTI-SAFEHOUSE GLOBALS ---
std::atomic<float> g_custom_x{0.0f};
std::atomic<float> g_custom_y{0.0f};
std::atomic<float> g_custom_z{0.0f};
std::atomic<float> g_custom_radius{0.0f}; // 0.0f means no custom safehouse is set yet

std::atomic<bool> g_accumulate_damage{true}; 
std::atomic<float> g_damage_multiplier{1.0f};
float g_suit_value = 1.0f;

std::atomic<bool> g_show_debug_overlay{false};
HANDLE g_console_handle = nullptr;
bool g_console_allocated = false;

Vector3 g_last_position{0.0f, 0.0f, 0.0f};
bool g_has_last_position = false;

extern "C" {
    uintptr_t g_player_base = 0;
    uintptr_t g_coord_return = 0;
    void MasterCoordinateHookAsm();
}

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

    extern "C" float SuitDamageLogic(uintptr_t pointer, float game_intended_value) {
        std::lock_guard<std::mutex> guard(g_suit_mutex);
        
        DWORD now = GetTickCount();
        DWORD time_since_last = now - g_last_hook_tick.load(std::memory_order_relaxed);
        g_last_hook_tick.store(now, std::memory_order_relaxed);

        // A true respawn ALWAYS involves a loading screen. 
        // If this hook hasn't fired in > 2.5 seconds, the game was loading.
        bool was_loading_screen = (time_since_last > 2500);

        if (pointer != g_last_pointer) {
            g_last_pointer = pointer;
            g_player_pointer = pointer; 
            
            float last_val = g_last_game_intended_value.load(std::memory_order_relaxed);

            // --- TRUE RESPAWN DETECTOR ---
            // Only punish the player if the pointer swapped, the game healed them, AND there was a loading screen.
            // This completely ignores rapid cinematic QTEs like the car chase!
            if (was_loading_screen && game_intended_value == 1.0f && last_val < 0.99f) {
                int respawn_mode = g_respawn_behavior.load(std::memory_order_relaxed);
                
                if (respawn_mode == 0) { // VANILLA
                    g_suit_value = 1.0f; 
                } else if (respawn_mode == 1) { // CINEMATIC
                    // Maintain current damage
                } else if (respawn_mode == 2) { // SURVIVOR
                    g_suit_value = 0.0f; 
                }
            }
            g_last_game_intended_value.store(game_intended_value, std::memory_order_relaxed);
            return g_suit_value;
        }
        
        if (g_accumulate_damage.load(std::memory_order_relaxed)) {
            float last_val = g_last_game_intended_value.load(std::memory_order_relaxed);
            
            if (game_intended_value < last_val) {
                g_last_damage_tick.store(now, std::memory_order_relaxed);

                float damage_taken = last_val - game_intended_value;
                float mult = g_damage_multiplier.load(std::memory_order_relaxed);
                float final_damage = (damage_taken * 0.12f) * mult; 
                
                g_suit_value = g_suit_value - final_damage;
                if (g_suit_value < 0.0f) g_suit_value = 0.0f;
            }
            g_last_game_intended_value.store(game_intended_value, std::memory_order_relaxed);
        } else {
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

    bool InstallCoordinateHook() {
        LogHookStatus("Attempting to install Master Coordinate Stealer...");

        HMODULE game_module = GetModuleHandleA("Spider-Man2.exe");
        if (!game_module) return false;

        uintptr_t base_address = reinterpret_cast<uintptr_t>(game_module);

        // The absolute global Z-coordinate writer
        uintptr_t hook_target = base_address + 0x30EEB11; 
        g_coord_return = hook_target + 5; // Perfect 5-byte fit!
        
        void* trampoline = AllocateNear(hook_target);
        if (trampoline) {
            uint8_t* t = reinterpret_cast<uint8_t*>(trampoline);
            t[0] = 0xFF; t[1] = 0x25; t[2] = 0x00; t[3] = 0x00; t[4] = 0x00; t[5] = 0x00;
            *reinterpret_cast<uintptr_t*>(t + 6) = reinterpret_cast<uintptr_t>(&MasterCoordinateHookAsm);

            DWORD old_protect;
            VirtualProtect(reinterpret_cast<LPVOID>(hook_target), 5, PAGE_EXECUTE_READWRITE, &old_protect);
            
            uint8_t* p = reinterpret_cast<uint8_t*>(hook_target);
            p[0] = 0xE9; 
            *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) - hook_target - 5);
            
            // NO PADDING NEEDED!
            
            VirtualProtect(reinterpret_cast<LPVOID>(hook_target), 5, old_protect, &old_protect);
            LogHookStatus("SUCCESS: Master Global Coordinate Hook installed.");
            return true;
        }
        return false;
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

    bool IsInSafehouse(Vector3 pos) {
        // 1. PETER'S HOUSE (Hardcoded)
        bool inPeter = (pos.x >= 2574.5f && pos.x <= 2611.5f) &&
                       (pos.y >= 1.0f && pos.y <= 14.5f) &&
                       (pos.z >= -908.0f && pos.z <= -885.0f);

        // 2. MILES' APARTMENT
        bool inMiles = (pos.x >= 226.5f && pos.x <= 244.0f) &&
                       (pos.y >= 8.0f && pos.y <= 29.5f) &&
                       (pos.z >= -2149.0f && pos.z <= -2122.5f);

        // 3. CUSTOM SAFEHOUSE (Dynamic)
        bool inCustom = false;
        float radius = g_custom_radius.load(std::memory_order_relaxed);
        if (radius > 0.0f) { 
            float cx = g_custom_x.load(std::memory_order_relaxed);
            float cy = g_custom_y.load(std::memory_order_relaxed);
            float cz = g_custom_z.load(std::memory_order_relaxed);
            
            inCustom = (pos.x >= cx - radius && pos.x <= cx + radius) &&
                       (pos.y >= cy - radius && pos.y <= cy + radius) &&
                       (pos.z >= cz - radius && pos.z <= cz + radius);
        }

        return (inPeter || inMiles || inCustom);
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
        // --- SECRET MODIFIER GATE ---
        // Requires holding Left Ctrl (VK_LCONTROL), Left Shift (VK_LSHIFT), and Alt (VK_MENU)
        bool modifiers_held = IsKeyDown(VK_LCONTROL) && IsKeyDown(VK_LSHIFT) && IsKeyDown(VK_MENU);
        
        for (int index = 0; index < kNumHotkeys; ++index) {
            bool down = IsKeyDown(kHotkeyVks[index]);
            
            // Trigger the hotkey action if the button was just pressed
            if (down && !g_last_key_state[index]) {
                switch (index) {
                    case 0: break;
                    case 1: break;
                    case 2: break;
                    case 3: 
                        // F4 (Index 3) specifically requires the secret modifiers to be held down
                        if (modifiers_held) {
                            g_show_debug_overlay.store(!g_show_debug_overlay.load(std::memory_order_relaxed), std::memory_order_relaxed);
                            ShowNotification(g_show_debug_overlay.load() ? "Debug Overlay: ON" : "Debug Overlay: OFF");
                        }
                        break;
                    case 4: SetSuitFraction(1.0f); ShowNotification("Suit: Full Health"); break;
                    case 5: SetSuitFraction(0.5f); ShowNotification("Suit: 50% Damaged"); break;
                    case 6: SetSuitFraction(0.1f); ShowNotification("Suit: Critical"); break;
                    case 7: SetSuitFraction(0.0f); ShowNotification("Suit: Destroyed"); break;
                    case 8: break;
                    case 9: ShowNotification("F10 pressed"); break;
                }
            }
            g_last_key_state[index] = down;
        }
    }

    void ApplyDecay(Vector3 current_pos, float delta_sec) {
        if (!g_decay_enabled.load(std::memory_order_relaxed)) return;
        if (delta_sec <= 0.0f) return;

        // Initialize position on your very first frame
        if (!g_has_last_position) {
            g_last_position = current_pos;
            g_has_last_position = true;
            return;
        }

        // 1. Calculate the 3D distance moved since last frame
        float dx = current_pos.x - g_last_position.x;
        float dy = current_pos.y - g_last_position.y;
        float dz = current_pos.z - g_last_position.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        g_last_position = current_pos;

        // 2. TELEPORT DETECTION: > 150 units in ~16ms is a fast-travel/respawn.
        if (distance > 150.0f) {
            return; 
        }

        // 3. Calculate current velocity
        float current_speed = distance / delta_sec;

        // --- DEBUG UI FLICKER FIX ---
        // Blend 10% of the new speed with 90% of the old speed to smooth the readout
        float old_speed = g_player_speed.load(std::memory_order_relaxed);
        g_player_speed.store((old_speed * 0.9f) + (current_speed * 0.1f), std::memory_order_relaxed);

        // 4. EXPONENTIAL DECAY MATH
        // Square the velocity so walking/jogging produces virtually zero damage
        float speed_squared = current_speed * current_speed;
        
        int user_multiplier = g_decay_rate.load(std::memory_order_relaxed);
        
        // Base scalar lowered significantly to account for the massive squared speed values
        float base_decay_scalar = 0.00000005f; 
        float final_decay = speed_squared * base_decay_scalar * user_multiplier * delta_sec;

        float current_fraction = GetCurrentSuitFraction();
        float new_fraction = current_fraction - final_decay;
        if (new_fraction < 0.0f) new_fraction = 0.0f;

        SetSuitFraction(new_fraction);
    }

    void SetHealRateInternal(int rate) { g_heal_rate.store(std::clamp(rate, kMinRate, kMaxRate), std::memory_order_relaxed); }
    void SetHealIntervalInternal(int interval) { g_heal_interval.store(std::clamp(interval, kMinInterval, kMaxInterval), std::memory_order_relaxed); }

    void ApplyFieldHeal(float delta_sec) {
        if (!g_field_heal_enabled.load(std::memory_order_relaxed)) return;
        float rate = g_field_heal_rate.load(std::memory_order_relaxed);
        if (rate <= 0.0f) return;

        float current_fraction = GetCurrentSuitFraction();
        // Multiply the rate by the exact fraction of a second that has passed
        float new_fraction = current_fraction + ((rate / 100.0f) * delta_sec);
        if (new_fraction > 1.0f) new_fraction = 1.0f; 
        SetSuitFraction(new_fraction);
    }

    void ApplySafehouseHeal(float delta_sec) {
        if (!g_safehouse_heal_enabled.load(std::memory_order_relaxed)) return;
        float rate = g_safehouse_heal_rate.load(std::memory_order_relaxed);
        if (rate <= 0.0f) return;

        float current_fraction = GetCurrentSuitFraction();
        // Multiply the rate by the exact fraction of a second that has passed
        float new_fraction = current_fraction + ((rate / 100.0f) * delta_sec);
        if (new_fraction > 1.0f) new_fraction = 1.0f; 
        SetSuitFraction(new_fraction);
    }

    std::string GetDebugOverlayText() {
        if (!g_show_debug_overlay.load(std::memory_order_relaxed)) return "";

        DWORD now = GetTickCount();
        float current_fraction = GetCurrentSuitFraction();

        // --- 1. PROCESS ANALYTICS ---
        float val_5s_ago = current_fraction;
        float val_30s_ago = current_fraction;
        float max_spike_5s = 0.0f;
        float max_spike_30s = 0.0f;
        
        {
            std::lock_guard<std::mutex> lock(g_analytics_mutex);
            bool found_5s = false;
            float prev_val = -1.0f;
            
            for (const auto& sample : g_suit_history) {
                // Pinpoint the exact health we had 5 seconds ago
                if (!found_5s && (now - sample.first <= 5000)) {
                    val_5s_ago = sample.second;
                    found_5s = true;
                }
                
                // Track max instantaneous damage spikes (e.g., getting hit by a rocket)
                if (prev_val >= 0.0f) {
                    float drop = prev_val - sample.second;
                    if (drop > 0.0f) {
                        max_spike_30s = (std::max)(max_spike_30s, drop);
                        if (now - sample.first <= 5000) {
                            max_spike_5s = (std::max)(max_spike_5s, drop);
                        }
                    }
                }
                prev_val = sample.second;
            }
            if (!g_suit_history.empty()) {
                val_30s_ago = g_suit_history.front().second; // Oldest entry (up to 30s)
            }
        }

        // --- 2. CALCULATE RATES ---
        float loss_5s = (std::max)(0.0f, val_5s_ago - current_fraction);
        float loss_30s = (std::max)(0.0f, val_30s_ago - current_fraction);
        
        float rate_5s = loss_5s / 5.0f;   // % lost per second (Short term)
        float rate_30s = loss_30s / 30.0f; // % lost per second (Long term)

        char etd_buffer[64];
        if (rate_5s > 0.0001f) {
            float seconds_left = current_fraction / rate_5s;
            if (seconds_left > 3600.0f) snprintf(etd_buffer, sizeof(etd_buffer), "> 1 Hour");
            else snprintf(etd_buffer, sizeof(etd_buffer), "%.1f seconds", seconds_left);
        } else {
            snprintf(etd_buffer, sizeof(etd_buffer), "Stable / Healing");
        }

        // --- 3. AUTO-HEAL COUNTDOWN ---
        DWORD last_dmg = g_last_damage_tick.load(std::memory_order_relaxed);
        float field_heal_timer = 0.0f;
        if (now - last_dmg < 30000) {
            field_heal_timer = 30.0f - ((now - last_dmg) / 1000.0f);
        }

        // --- 4. GATHER PLAYER STATE ---
        float p_x = 0.0f, p_y = 0.0f, p_z = 0.0f;
        bool in_safehouse = false;
        
        HeroSystem* hero_sys = GetHeroSystem();
        if (hero_sys != nullptr && hero_sys->GetHero() != nullptr) {
            Vector3 pos = hero_sys->GetHero()->GetPosition();
            p_x = pos.x; p_y = pos.y; p_z = pos.z;
            in_safehouse = IsInSafehouse(pos);
        }

        // --- 5. FORMAT OUTPUT ---
        char buffer[1024];
        snprintf(buffer, sizeof(buffer),
            "--- SUIT DAMAGE V3 PERFORMANCE DEBUG ---\n"
            "Suit Health:       %.2f%%\n"
            "Active Preset:     %d (Dmg Mult: %.2fx)\n"
            "Player Velocity:   %.1f units/sec\n"
            "Auto-Heal CD:      %.1f sec remaining\n"
            "----------------------------------------\n"
            "--- LIVE ATTRITION ANALYTICS ---\n"
            "5-Sec Avg Loss:    -%.2f%% per sec\n"
            "30-Sec Avg Loss:   -%.2f%% per sec\n"
            "Max Hit (5s):      -%.2f%%\n"
            "Max Hit (30s):     -%.2f%%\n"
            "Est. Time to 0%%:   %s\n"
            "----------------------------------------\n"
            "Coords: X:%.1f Y:%.1f Z:%.1f\n"
            "Safehouse Status:  %s",
            current_fraction * 100.0f,
            current_preset, g_damage_multiplier.load(std::memory_order_relaxed),
            g_player_speed.load(std::memory_order_relaxed),
            (std::max)(0.0f, field_heal_timer),
            rate_5s * 100.0f,
            rate_30s * 100.0f,
            max_spike_5s * 100.0f,
            max_spike_30s * 100.0f,
            etd_buffer,
            p_x, p_y, p_z,
            in_safehouse ? "ACTIVE" : "INACTIVE"
        );

        return std::string(buffer);
    }

    void UpdateDebugConsole() {
        if (g_show_debug_overlay.load(std::memory_order_relaxed)) {
            // Allocate the console if it doesn't exist yet
            if (!g_console_allocated) {
                AllocConsole();
                SetConsoleTitleA("Suit Damage V2 - Live Debug");
                g_console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
                g_console_allocated = true;
            }
            
            // Make sure the window is visible
            HWND consoleWindow = GetConsoleWindow();
            if (consoleWindow) ShowWindow(consoleWindow, SW_SHOW);

            // Lock the cursor to the top-left so we overwrite the text instead of spam-scrolling
            if (g_console_handle) {
                COORD cursorPosition;
                cursorPosition.X = 0;
                cursorPosition.Y = 0;
                SetConsoleCursorPosition(g_console_handle, cursorPosition);

                std::string text = GetDebugOverlayText();
                
                // Pad the end with spaces to wipe any ghost characters from previous frames
                text += "\n                                        \n                                        "; 
                
                DWORD written;
                WriteConsoleA(g_console_handle, text.c_str(), static_cast<DWORD>(text.length()), &written, NULL);
            }
        } else {
            // If the overlay is toggled off, hide the console window
            if (g_console_allocated) {
                HWND consoleWindow = GetConsoleWindow();
                if (consoleWindow) ShowWindow(consoleWindow, SW_HIDE);
            }
        }
    }

    void WorkerThread() {
        LogHookStatus("WorkerThread booted successfully.");
        if (kEnableInstructionPatch) {
            LogHookStatus("Delaying hook install by 3000ms...");
            Sleep(kHookInstallDelayMs);
            InstallSuitDamageHook();
            InstallCoordinateHook(); 
        }

        HMODULE game_module = GetModuleHandleA("Spider-Man2.exe");
        uintptr_t base_address = reinterpret_cast<uintptr_t>(game_module);
        uintptr_t suit_id_pointer = base_address + 0xAFB6EE8;

        DWORD last_tick = GetTickCount();
        DWORD last_log_tick = 0;
        DWORD last_heal_tick = last_tick;
        DWORD last_decay_tick = last_tick;

        while (g_running.load(std::memory_order_relaxed)) {
            HandleHotkeys();
            UpdateDebugConsole(); // <-- Renders the live overlay
            DWORD now = GetTickCount();

            // --- ANALYTICS RECORDER ---
            {
                std::lock_guard<std::mutex> lock(g_analytics_mutex);
                g_suit_history.push_back({now, GetCurrentSuitFraction()});
                
                // Erase any data older than 30 seconds
                while (!g_suit_history.empty() && (now - g_suit_history.front().first > 30000)) {
                    g_suit_history.pop_front();
                }
            }

            // --- ENGINE PAUSE DETECTOR ---
            // If the engine hasn't called the hook in over 200ms, the game is paused.
            if (now - g_last_hook_tick.load(std::memory_order_relaxed) > 200) {
                last_tick = now; // Keep delta zeroed out so we don't get massive time jumps
                Sleep(16);
                continue; // Skip all healing, wardrobe, and decay math this frame
            }

            // --- 1. WARDROBE CLEANSE ---
            if (base_address != 0) {
                uint32_t current_suit_id = *reinterpret_cast<uint32_t*>(suit_id_pointer);
                if (current_suit_id != g_last_suit_id.load(std::memory_order_relaxed)) {
                    if (g_last_suit_id.load(std::memory_order_relaxed) != 0) {
                        if (g_wardrobe_heal_enabled.load(std::memory_order_relaxed)) {
                            SuitDamageManager::SetSuitHealthFraction(1.0f);
                            LogHookStatus("Suit changed: Wardrobe Cleanse applied.");
                        }
                    }
                    g_last_suit_id.store(current_suit_id, std::memory_order_relaxed);
                }
            }

            // --- 2. HEAL ENGINE (Safehouse vs Field) ---
            // Calculate exactly how many seconds have passed since the last loop (approx 0.016s)
            float delta_sec = (now - last_tick) / 1000.0f;
            last_tick = now;

            // --- 2. HEAL ENGINE (Safehouse vs Field) ---
            bool in_safehouse = false;
            Vector3 current_player_pos{0.0f, 0.0f, 0.0f};
            
            HeroSystem* hero_sys = GetHeroSystem();
            if (hero_sys != nullptr && hero_sys->GetHero() != nullptr) {
                current_player_pos = hero_sys->GetHero()->GetPosition();
                in_safehouse = IsInSafehouse(current_player_pos);
            }

            // Run the physics decay calculation frame-by-frame
            if (g_decay_enabled.load(std::memory_order_relaxed) && current_player_pos.x != 0.0f) {
                ApplyDecay(current_player_pos, delta_sec);
            }

            if (in_safehouse && g_safehouse_heal_enabled.load(std::memory_order_relaxed)) {
                ApplySafehouseHeal(delta_sec); 
            } 
            else if (g_field_heal_enabled.load(std::memory_order_relaxed)) {
                if (now - g_last_damage_tick.load(std::memory_order_relaxed) > 30000) {
                    ApplyFieldHeal(delta_sec);
                }
            }

            if (GetAsyncKeyState(VK_F9) & 0x8000) {
                DWORD now = GetTickCount();
                if (now - last_log_tick > 1000) { 
                    
                    // 1. Get the game's Hero System
                    HeroSystem* hero_sys = GetHeroSystem();
                    if (hero_sys != nullptr) {
                        
                        // 2. Ask the system for Peter's master entity
                        Actor* peter = hero_sys->GetHero();
                        if (peter != nullptr) {
                            
                            // 3. Grab his absolute 3D position directly from his Transform component
                            Vector3 pos = peter->GetPosition();
                            
                            char log_buffer[256];
                            snprintf(log_buffer, sizeof(log_buffer), 
                                "NATIVE SDK COORDS -> X: %.2f | Y: %.2f | Z: %.2f", 
                                pos.x, pos.y, pos.z);
                                
                            LogHookStatus(log_buffer);
                        } else {
                            LogHookStatus("HeroSystem found, but Peter's Actor is currently null (Loading/Dead?).");
                        }
                    } else {
                        LogHookStatus("HeroSystem is not initialized yet.");
                    }
                    
                    last_log_tick = now;
                }
            }
            Sleep(16); 
        } // <--- THIS BRACE CLOSES THE WHILE LOOP

        // This log only fires ONCE when the game actually closes now!
        LogHookStatus("WorkerThread shutting down.");
    }
}

const char* kConfigPath = ".\\SuitDamageConfig.ini";

namespace SuitDamageManager {
    bool g_we_initialized_minhook = false;

    void SaveSettingsToINI() {
        auto WriteFloat = [](const char* key, float val) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.2f", val);
            WritePrivateProfileStringA("Settings", key, buf, kConfigPath);
        };
        auto WriteInt = [](const char* key, int val) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d", val);
            WritePrivateProfileStringA("Settings", key, buf, kConfigPath);
        };

        WriteInt("ActivePreset", current_preset);
        WriteFloat("DamageMultiplier", g_damage_multiplier.load(std::memory_order_relaxed));
        WriteInt("DecayEnabled", g_decay_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        WriteInt("DecayRate", g_decay_rate.load(std::memory_order_relaxed));
        WriteInt("HealEnabled", g_heal_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        WriteInt("HealRate", g_heal_rate.load(std::memory_order_relaxed));
        WriteInt("SafehouseHealRate", g_safehouse_heal_rate.load(std::memory_order_relaxed));
        WriteInt("FieldHealRate", g_field_heal_rate.load(std::memory_order_relaxed));
        
        // --- NEW SAVES ---
        WriteInt("WardrobeHeal", g_wardrobe_heal_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        WriteInt("SafehouseHeal", g_safehouse_heal_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        WriteInt("FieldHeal", g_field_heal_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        WriteInt("RespawnBehavior", g_respawn_behavior.load(std::memory_order_relaxed));

        WriteFloat("CustomX", g_custom_x.load(std::memory_order_relaxed));
        WriteFloat("CustomY", g_custom_y.load(std::memory_order_relaxed));
        WriteFloat("CustomZ", g_custom_z.load(std::memory_order_relaxed));
        WriteFloat("CustomRadius", g_custom_radius.load(std::memory_order_relaxed));
    }

    void LoadSettingsFromINI() {
        auto ReadFloat = [](const char* key, float default_val) -> float {
            char buf[32];
            GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf), kConfigPath);
            if (buf[0] == '\0') return default_val;
            try { return std::stof(buf); } catch (...) { return default_val; }
        };

        current_preset = GetPrivateProfileIntA("Settings", "ActivePreset", 2, kConfigPath);
        g_damage_multiplier.store(ReadFloat("DamageMultiplier", 2.0f), std::memory_order_relaxed);
        g_decay_enabled.store(GetPrivateProfileIntA("Settings", "DecayEnabled", 0, kConfigPath) != 0, std::memory_order_relaxed);
        g_decay_rate.store(GetPrivateProfileIntA("Settings", "DecayRate", 10, kConfigPath), std::memory_order_relaxed);
        g_heal_enabled.store(GetPrivateProfileIntA("Settings", "HealEnabled", 0, kConfigPath) != 0, std::memory_order_relaxed);
        g_heal_rate.store(GetPrivateProfileIntA("Settings", "HealRate", 5, kConfigPath), std::memory_order_relaxed);
        
        // --- NEW LOADS ---
        g_wardrobe_heal_enabled.store(GetPrivateProfileIntA("Settings", "WardrobeHeal", 1, kConfigPath) != 0, std::memory_order_relaxed);
        g_safehouse_heal_enabled.store(GetPrivateProfileIntA("Settings", "SafehouseHeal", 1, kConfigPath) != 0, std::memory_order_relaxed);
        g_field_heal_enabled.store(GetPrivateProfileIntA("Settings", "FieldHeal", 1, kConfigPath) != 0, std::memory_order_relaxed);
        g_respawn_behavior.store(GetPrivateProfileIntA("Settings", "RespawnBehavior", 2, kConfigPath), std::memory_order_relaxed);

        g_custom_x.store(ReadFloat("CustomX", 0.0f), std::memory_order_relaxed);
        g_custom_y.store(ReadFloat("CustomY", 0.0f), std::memory_order_relaxed);
        g_custom_z.store(ReadFloat("CustomZ", 0.0f), std::memory_order_relaxed);
        g_custom_radius.store(ReadFloat("CustomRadius", 0.0f), std::memory_order_relaxed);
        
        LogHookStatus("SUCCESS: Settings loaded from SuitDamageConfig.ini");
    }

    // --- PASTE THE CUT FUNCTIONS HERE ---
    void SetCurrentLocationAsSafehouse() {
        HeroSystem* hero_sys = GetHeroSystem();
        if (hero_sys != nullptr && hero_sys->GetHero() != nullptr) {
            Vector3 pos = hero_sys->GetHero()->GetPosition();
            
            g_custom_x.store(pos.x, std::memory_order_relaxed);
            g_custom_y.store(pos.y, std::memory_order_relaxed);
            g_custom_z.store(pos.z, std::memory_order_relaxed);
            g_custom_radius.store(25.0f, std::memory_order_relaxed); 
            
            SaveSettingsToINI(); 
            LogHookStatus("SUCCESS: Custom safehouse coordinates locked in.");
        }
    }

    void ClearCustomSafehouse() {
        g_custom_radius.store(0.0f, std::memory_order_relaxed);
        SaveSettingsToINI();
        LogHookStatus("SUCCESS: Custom safehouse cleared.");
    }

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
        LoadSettingsFromINI();
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
    bool IsWardrobeHealEnabled() { return g_wardrobe_heal_enabled.load(std::memory_order_relaxed); }
    void SetWardrobeHealEnabled(bool enabled) { g_wardrobe_heal_enabled.store(enabled, std::memory_order_relaxed); }

    bool IsSafehouseHealEnabled() { return g_safehouse_heal_enabled.load(std::memory_order_relaxed); }
    void SetSafehouseHealEnabled(bool enabled) { g_safehouse_heal_enabled.store(enabled, std::memory_order_relaxed); }

    bool IsFieldHealEnabled() { return g_field_heal_enabled.load(std::memory_order_relaxed); }
    void SetFieldHealEnabled(bool enabled) { g_field_heal_enabled.store(enabled, std::memory_order_relaxed); }

    int GetRespawnBehavior() { return g_respawn_behavior.load(std::memory_order_relaxed); }
    void SetRespawnBehavior(int behavior) { g_respawn_behavior.store(behavior, std::memory_order_relaxed); }

    float GetSafehouseHealRate() { return g_safehouse_heal_rate.load(std::memory_order_relaxed); }
    void SetSafehouseHealRate(float rate) { g_safehouse_heal_rate.store(rate, std::memory_order_relaxed); }
    
    float GetFieldHealRate() { return g_field_heal_rate.load(std::memory_order_relaxed); }
    void SetFieldHealRate(float rate) { g_field_heal_rate.store(rate, std::memory_order_relaxed); }
}