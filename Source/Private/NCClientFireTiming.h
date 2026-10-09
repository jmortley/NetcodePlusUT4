#pragma once

#include "CoreMinimal.h"

class AUTWeaponFix;

// Native-only prediction timing. No reflected fields or network timestamps change.
namespace NCClientFireTiming
{
    void Startup();
    void Shutdown();
    bool IsLocal(AUTWeaponFix* Weapon);
    void Record(AUTWeaponFix* Weapon, uint8 Mode, bool bPreserveCadence = false);
    // Signed remainder is intentional: BringUp back-calculates an earlier switch.
    float Remaining(AUTWeaponFix* Weapon, uint8 Mode);
    float MaxRemaining(AUTWeaponFix* Weapon);
    void Forget(AUTWeaponFix* Weapon);
}
