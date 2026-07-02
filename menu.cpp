#include "menu.h"
#include "SuitDamageManager.h"
#include "logging.h"

// It's best to use fully capitalised text for all your labels, 
// to better fit in with the games design style

float health_change = 0.0f;
void HealthMenu::Create() {
    Button("LOG SUIT HEALTH", "Print the current suit health fraction.", [](){
        DEBUG("Suit health fraction: %.3f", SuitDamageManager::GetSuitHealthFraction());
    });
    
    Button("REPAIR SUIT", "Restore suit health to full.", [](){
        SuitDamageManager::RepairSuit();
    });

    static int decay_enabled = SuitDamageManager::IsDecayEnabled() ? 1 : 0;
    Toggle("DECAY ENABLED", "Enable or disable suit decay.", &decay_enabled, [&](){
        SuitDamageManager::SetDecayEnabled(decay_enabled != 0);
    });

    static float decay_rate = static_cast<float>(SuitDamageManager::GetDecayRate());
    static float decay_interval = static_cast<float>(SuitDamageManager::GetDecayInterval());
    static float suit_percent = SuitDamageManager::GetSuitHealthPercent();

    SliderOpts rate_opts;
    rate_opts.min = 0.0f;
    rate_opts.max = 100.0f;
    rate_opts.display_min = 0;
    rate_opts.display_max = 100;
    rate_opts.step = 1.0f;
    Slider("DECAY RATE", "Suit decay rate in units per second.", &decay_rate, rate_opts, [&](){
        SuitDamageManager::SetDecayRate(static_cast<int>(decay_rate));
    });

    SliderOpts interval_opts;
    interval_opts.min = 100.0f;
    interval_opts.max = 2000.0f;
    interval_opts.display_min = 100;
    interval_opts.display_max = 2000;
    interval_opts.step = 100.0f;
    Slider("DECAY INTERVAL", "Update interval for suit decay in milliseconds.", &decay_interval, interval_opts, [&](){
        SuitDamageManager::SetDecayInterval(static_cast<int>(decay_interval));
    });

    SliderOpts percent_opts;
    percent_opts.min = 0.0f;
    percent_opts.max = 100.0f;
    percent_opts.display_min = 0;
    percent_opts.display_max = 100;
    percent_opts.step = 1.0f;
    Slider("SUIT PERCENT", "Set the suit's current health percentage.", &suit_percent, percent_opts, [&](){
        SuitDamageManager::SetSuitHealthPercent(suit_percent);
    });

    Button("APPLY 10% DAMAGE", "Reduce the suit health by 10%.", [](){
        SuitDamageManager::ApplySuitDamage(0.1f);
    });

    Button("REPAIR 10%", "Increase the suit health by 10%.", [](){
        SuitDamageManager::ApplySuitRepair(0.1f);
    });

    Header("SUIT SHORTCUTS");
    Button("FULL HEALTH (F5)", "Set the suit to full health.", [](){
        SuitDamageManager::SetSuitHealthFraction(1.0f);
    });
    Button("50% DAMAGE (F6)", "Set the suit health to 50%.", [](){
        SuitDamageManager::SetSuitHealthFraction(0.5f);
    });
    Button("CRITICAL (F7)", "Set the suit health to 10%.", [](){
        SuitDamageManager::SetSuitHealthFraction(0.1f);
    });
    Button("DESTROYED (F8)", "Set the suit health to 0%.", [](){
        SuitDamageManager::SetSuitHealthFraction(0.0f);
    });
}
