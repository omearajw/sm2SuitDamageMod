#pragma once

#include "ModSettings.h"

class HealthMenu : public ModMenu {
public:
    const char* GetName() override { return "Suit Damage Controller"; }
    const char* GetDescription() override { return "Control suit damage settings."; }
    MenuType GetType() override { return MenuType::Pause; }
    void Create() override;
};

