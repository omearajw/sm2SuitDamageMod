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
        mult_opts.min = 0.5f; mult_opts.max = 5.0f; mult_opts.step = 0.5f;
        mult_opts.display_min = 0.5; mult_opts.display_max = 5.0;
        Slider("DAMAGE MULTIPLIER", "Adjust damage rate.", &dmg_mult, mult_opts, [&](){
            SuitDamageManager::SetDamageMultiplier(dmg_mult);
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
        Toggle("ENABLE SAFEHOUSE REPAIR", "Suit heals automatically while at Peter's House.", &safe_heal, [&](){
            SuitDamageManager::SetSafehouseHealEnabled(safe_heal != 0);
            current_preset = PRESET_CUSTOM;
            SuitDamageManager::SaveSettingsToINI();
        });

        static float s_heal_rate;
        s_heal_rate = SuitDamageManager::GetSafehouseHealRate();
        SliderOpts s_rate_opts;
        s_rate_opts.min = 0.0f; s_rate_opts.max = 100.0f; s_rate_opts.step = 1.0f;
        s_rate_opts.display_min = 0; s_rate_opts.display_max = 100;
        Slider("SAFEHOUSE HEAL RATE", "% of health restored per second in the safehouse.", &s_heal_rate, s_rate_opts, [&](){
            SuitDamageManager::SetSafehouseHealRate(s_heal_rate);
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
    }
};

// --- THE MAIN MENU ---
void HealthMenu::Create() {
    Header("SUIT DAMAGE OVERHAUL V2");

    const char* preset_names[] = { "Custom", "Vanilla+", "Cinematic", "Survivor" };
    Option("ACTIVE PRESET", "Select a pre-configured gameplay loop.", &current_preset, preset_names, 4, [&](){
        
        if (current_preset == PRESET_VANILLA_PLUS) {
            SuitDamageManager::SetDamageMultiplier(1.0f);
            
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(20.0f); // 5 seconds to heal
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(5.0f); // Fast auto-heal
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(0); 
        } 
        else if (current_preset == PRESET_CINEMATIC) {
            SuitDamageManager::SetDamageMultiplier(2.0f);
            
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(10.0f); // 10 seconds to heal
            SuitDamageManager::SetFieldHealEnabled(true);
            SuitDamageManager::SetFieldHealRate(1.0f); // Very slow auto-heal
            
            SuitDamageManager::SetDecayEnabled(false);
            SuitDamageManager::SetWardrobeHealEnabled(true);
            SuitDamageManager::SetRespawnBehavior(1); 
        }
        else if (current_preset == PRESET_SURVIVOR) {
            SuitDamageManager::SetDamageMultiplier(2.5f);
            
            SuitDamageManager::SetSafehouseHealEnabled(true);
            SuitDamageManager::SetSafehouseHealRate(4.0f); // 25 seconds to heal
            SuitDamageManager::SetFieldHealEnabled(false); 
            SuitDamageManager::SetFieldHealRate(0.0f); 
            
            SuitDamageManager::SetDecayEnabled(true);
            SuitDamageManager::SetDecayRate(1); 
            SuitDamageManager::SetWardrobeHealEnabled(false);
            SuitDamageManager::SetRespawnBehavior(2); 
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