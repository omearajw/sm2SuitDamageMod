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

        static float dmg_mult;
        dmg_mult = SuitDamageManager::GetDamageMultiplier(); 
        SliderOpts mult_opts;
        mult_opts.min = 5; mult_opts.max = 50; mult_opts.step = 5;
        mult_opts.display_min = 5; mult_opts.display_max = 50;
        Slider("DAMAGE MULTIPLIER", "Adjust damage rate. (10 = 1.0x)", &dmg_mult, mult_opts, [&](){
            SuitDamageManager::SetDamageMultiplier(dmg_mult / 10.0f);
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

        static float f_heal_rate;
        f_heal_rate = SuitDamageManager::GetFieldHealRate();
        SliderOpts f_rate_opts;
        f_rate_opts.min = 0.0f; f_rate_opts.max = 20.0f; f_rate_opts.step = 0.5f;
        f_rate_opts.display_min = 0; f_rate_opts.display_max = 20;
        Slider("FIELD HEAL RATE", "% of health restored per second in the field.", &f_heal_rate, f_rate_opts, [&](){
            SuitDamageManager::SetFieldHealRate(f_heal_rate);
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

        static int safehouse_speed_idx = 2; // Default to Standard
        float current_rate = SuitDamageManager::GetSafehouseHealRate();
        if (current_rate >= 20.0f) safehouse_speed_idx = 4;
        else if (current_rate >= 10.0f) safehouse_speed_idx = 3;
        else if (current_rate >= 3.0f) safehouse_speed_idx = 2;
        else if (current_rate >= 1.5f) safehouse_speed_idx = 1;
        else safehouse_speed_idx = 0;

        const char* speed_names[] = { "Immersive (~2 mins)", "Slow (~60s)", "Standard (~30s)", "Fast (10s)", "Arcade (5s)" };
        Option("SAFEHOUSE HEAL SPEED", "How quickly the suit repairs at a safehouse.", &safehouse_speed_idx, speed_names, 5, [&](){
            
            float new_rate = 3.33f; 
            switch(safehouse_speed_idx) {
                case 0: new_rate = 0.8f; break;  // 120 seconds
                case 1: new_rate = 1.66f; break; // 60 seconds
                case 2: new_rate = 3.33f; break; // 30 seconds
                case 3: new_rate = 10.0f; break; // 10 seconds
                case 4: new_rate = 20.0f; break; // 5 seconds
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

        static float decay_rate;
        decay_rate = static_cast<float>(SuitDamageManager::GetDecayRate());
        SliderOpts d_rate_opts;
        d_rate_opts.min = 0.0f; d_rate_opts.max = 100.0f; d_rate_opts.step = 1.0f;
        d_rate_opts.display_min = 0; d_rate_opts.display_max = 100;
        Slider("DECAY RATE", "Speed of natural wear and tear.", &decay_rate, d_rate_opts, [&](){
            SuitDamageManager::SetDecayRate(static_cast<int>(decay_rate));
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
            SuitDamageManager::SetDamageMultiplier(1.0f);
            
            // Normal Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(20.0f); 
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(5.0f); 
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(0); // Full Heal
        } 
        else if (current_preset == PRESET_CINEMATIC) {
            // More damage
            SuitDamageManager::SetDamageMultiplier(2.0f);
            
            // Slower Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(10.0f); 
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(1.0f); 
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(1); // Maintain Damage
        }
        else if (current_preset == PRESET_SURVIVOR) {
            // Lowest damage (attrition based)
            SuitDamageManager::SetDamageMultiplier(0.5f);
            
            // No Out-of-Combat Healing
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(4.0f); 
            SuitDamageManager::SetFieldHealEnabled(false); 
            SuitDamageManager::SetFieldHealRate(0.0f); 
            
            SuitDamageManager::SetDecayEnabled(true);
            SuitDamageManager::SetDecayRate(1); 
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
    percent_opts.display_min = 0; percent_opts.display_max = 100;
    Slider("SUIT PERCENT", "Manually set the suit's current health percentage.", &suit_percent, percent_opts, [&](){
        SuitDamageManager::SetSuitHealthPercent(suit_percent);
    });

    Button("FULL HEALTH (F5)", "Instantly repair the suit.", [](){
        SuitDamageManager::SetSuitHealthFraction(1.0f);
    });
}