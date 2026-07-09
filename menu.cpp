#include "menu.h"
#include "SuitDamageManager.h"
#include "logging.h"

enum PresetType {
    PRESET_CUSTOM = 0,
    PRESET_VANILLA_PLUS = 1,
    PRESET_CINEMATIC = 2,
    PRESET_SURVIVOR = 3
};

int current_preset = PRESET_CINEMATIC; 

class AdvancedMenu : public BaseMenu {
public:
    const char* GetName() override { return "ADVANCED SETTINGS"; }
    
    void Create() override {
        Header("DAMAGE SETTINGS");

        static float dmg_mult_int;
        dmg_mult_int = static_cast<int>(SuitDamageManager::GetDamageMultiplier() * 10.0f); 
        SliderOpts mult_opts;
        
        // Min is now 0, allowing for a 0.0x damage multiplier
        mult_opts.min = 0.0f; mult_opts.max = 50.0f; mult_opts.step = 5.0f;
        mult_opts.display_min = 0.0f; mult_opts.display_max = 50.0f;
        
        Slider("DAMAGE MULTIPLIER", "Adjust damage rate. (10 = 1.0x, 0 = Maintain Suit)", &dmg_mult_int, mult_opts, [&](){
            SuitDamageManager::SetDamageMultiplier(dmg_mult_int / 10.0f);
            current_preset = PRESET_CUSTOM; 
            SuitDamageManager::SaveSettingsToINI();
        });

        Header("FIELD REPAIR (AUTO-HEAL)");
        
        static int field_heal;
        field_heal = SuitDamageManager::IsFieldHealEnabled() ? 1 : 0;
        Toggle("ENABLE FIELD REPAIR", "Suit heals automatically after avoiding damage.", &field_heal, [&](){
            SuitDamageManager::SetFieldHealEnabled(field_heal != 0);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        static int field_speed_idx = 2; // Default to Standard
        float current_f_rate = SuitDamageManager::GetFieldHealRate();
        
        // Match the float to the UI index
        if (current_f_rate >= 3.3f) field_speed_idx = 0;      // Arcade
        else if (current_f_rate >= 1.6f) field_speed_idx = 1; // Fast
        else if (current_f_rate >= 0.8f) field_speed_idx = 2; // Standard
        else if (current_f_rate >= 0.5f) field_speed_idx = 3; // Slow
        else field_speed_idx = 4;                             // Immersive

        const char* f_speed_names[] = { "Arcade (30s)", "Fast (60s)", "Standard (~2 mins)", "Slow (~3 mins)", "Immersive (~5 mins)" };
        Option("FIELD HEAL SPEED", "How quickly the suit auto-repairs while exploring.", &field_speed_idx, f_speed_names, 5, [&](){
            
            float new_f_rate = 0.83f; 
            switch(field_speed_idx) {
                case 0: new_f_rate = 3.33f; break; // Arcade
                case 1: new_f_rate = 1.66f; break; // Fast
                case 2: new_f_rate = 0.83f; break; // Standard
                case 3: new_f_rate = 0.55f; break; // Slow
                case 4: new_f_rate = 0.33f; break; // Immersive
            }
            
            SuitDamageManager::SetFieldHealRate(new_f_rate);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        Header("SAFEHOUSE REPAIR");
        
        static int safe_heal;
        safe_heal = SuitDamageManager::IsSafehouseHealEnabled() ? 1 : 0;
        Toggle("ENABLE SAFEHOUSE REPAIR", "Suit heals automatically while at a Safehouse.", &safe_heal, [&](){
            SuitDamageManager::SetSafehouseHealEnabled(safe_heal != 0);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        static int safehouse_speed_idx = 1; // Default to Standard
        float current_rate = SuitDamageManager::GetSafehouseHealRate();
        
        // Match the engine float to the 4 UI indices
        if (current_rate >= 10.0f) safehouse_speed_idx = 0;      // Fast (10s)
        else if (current_rate >= 3.3f) safehouse_speed_idx = 1;  // Standard (30s)
        else if (current_rate >= 1.6f) safehouse_speed_idx = 2;  // Slow (60s)
        else safehouse_speed_idx = 3;                            // Immersive (120s)

        const char* speed_names[] = { "Fast (10s)", "Standard (~30s)", "Slow (~60s)", "Immersive (~2 mins)" };
        Option("SAFEHOUSE HEAL SPEED", "How quickly the suit repairs at a safehouse.", &safehouse_speed_idx, speed_names, 4, [&](){
            
            float new_rate = 3.33f; 
            switch(safehouse_speed_idx) {
                case 0: new_rate = 10.0f; break; // Fast (10s)
                case 1: new_rate = 3.33f; break; // Standard (30s)
                case 2: new_rate = 1.66f; break; // Slow (60s)
                case 3: new_rate = 0.83f; break; // Immersive (120s)
            }
            
            SuitDamageManager::SetSafehouseHealRate(new_rate);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        Header("SUIT DECAY (WEAR & TEAR)");
        
        static int decay_enabled;
        decay_enabled = SuitDamageManager::IsDecayEnabled() ? 1 : 0;
        Toggle("ENABLE DECAY", "Suit slowly degrades over time while swinging.", &decay_enabled, [&](){
            SuitDamageManager::SetDecayEnabled(decay_enabled != 0);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        static float decay_rate_int = SuitDamageManager::GetDecayRate(); 
        SliderOpts decay_opts;
        
        decay_opts.min = 0.0f; decay_opts.max = 50.0f; decay_opts.step = 5.0f;
        decay_opts.display_min = 0.0f; decay_opts.display_max = 50.0f;
        
        Slider("DECAY MULTIPLIER", "Adjust swinging decay rate. (10 = 1.0x, 0 = Off)", &decay_rate_int, decay_opts, [&](){
            SuitDamageManager::SetDecayRate(decay_rate_int/5.0f);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        Header("SUIT MANAGEMENT");
        
        static int wardrobe_heal;
        wardrobe_heal = SuitDamageManager::IsWardrobeHealEnabled() ? 1 : 0;
        Toggle("SUIT CHANGE HEALS", "Equipping a new suit repairs all damage.", &wardrobe_heal, [&](){
            SuitDamageManager::SetWardrobeHealEnabled(wardrobe_heal != 0);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        // --- NEW RESPAWN UI ---
        Header("DEATH & RESPAWN");
        
        static int respawn_mode;
        respawn_mode = SuitDamageManager::GetRespawnBehavior();
        const char* respawn_names[] = { "Full Heal (Vanilla)", "Maintain Damage", "Suit Destroyed" };
        Option("RESPAWN PENALTY", "What happens to the suit when you reload a checkpoint?", &respawn_mode, respawn_names, 3, [&](){
            SuitDamageManager::SetRespawnBehavior(respawn_mode);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        Header("LOCATION MAPPING");
        
        Button("SET CURRENT LOCATION AS SAFEHOUSE", "Creates a persistent repair zone exactly where you are currently standing.", [](){
            SuitDamageManager::SetCurrentLocationAsSafehouse();
        });
        
        Button("CLEAR CUSTOM SAFEHOUSE", "Removes your custom repair zone. Peter and Miles' homes remain active.", [](){
            SuitDamageManager::ClearCustomSafehouse();
        });
    }
};

void HealthMenu::Create() {
    Header("SUIT DAMAGE OVERHAUL V2");

    const char* preset_names[] = { "Custom", "Vanilla+", "Cinematic", "Survivor" };
    Option("ACTIVE PRESET", "Select a pre-configured gameplay loop.", &current_preset, preset_names, 4, [&](){
        
        if (current_preset == PRESET_VANILLA_PLUS) {
            // Normal damage
            SuitDamageManager::SetDamageMultiplier(1.5f);
            
            // Normal Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(10.0f); 
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(3.33f); 
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(0); // Full Heal
        } 
        else if (current_preset == PRESET_CINEMATIC) {
            // More damage
            SuitDamageManager::SetDamageMultiplier(2.0f);
            
            // Slower Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(3.33f); 
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(0.8f); 
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(1); // Maintain Damage
        }
        else if (current_preset == PRESET_SURVIVOR) {
            // Lowest damage (attrition based)
            SuitDamageManager::SetDamageMultiplier(1.0f);
            
            // No Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(1.66f); 
            SuitDamageManager::SetFieldHealEnabled(false); 
            SuitDamageManager::SetFieldHealRate(0.0f); 
            
            SuitDamageManager::SetDecayEnabled(true);
            SuitDamageManager::SetDecayRate(10.0f); 
            SuitDamageManager::SetWardrobeHealEnabled(false);
            SuitDamageManager::SetRespawnBehavior(2); // Suit Destroyed
        }
        
        SuitDamageManager::SaveSettingsToINI();
        RequestRefresh(); 
    });

    Dropdown<AdvancedMenu>();

    Header("MANUAL OVERRIDES");
    
    static float suit_percent;
    suit_percent = SuitDamageManager::GetSuitHealthPercent();
    SliderOpts percent_opts;
    percent_opts.min = 0.0f; percent_opts.max = 100.0f; percent_opts.step = 1.0f;
    percent_opts.display_min = 0.0f; percent_opts.display_max = 100.0f;
    Slider("SUIT PERCENT", "Manually set the suit's current health percentage.", &suit_percent, percent_opts, [&](){
        SuitDamageManager::SetSuitHealthPercent(suit_percent);
    });

    Button("FULL HEALTH (F5)", "Instantly repair the suit.", [](){
        SuitDamageManager::SetSuitHealthFraction(1.0f);
    });
}