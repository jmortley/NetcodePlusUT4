#pragma once

#include "NetcodePlus.h"

class AUTWeapon;
class AUTProjectile;
class UUTWeaponState;
struct FHitResult;

// Observation only: no reflected fields, RPCs, timers, or gameplay counter writes.
namespace NCFireDiagnostics
{
    bool Enabled();
    void Record(AUTWeapon* Weapon, const TCHAR* Kind, uint8 Mode,
        int32 Event = INDEX_NONE, uint32 Generation = 0,
        const FString& Details = FString(), const TCHAR* Protocol = TEXT("fixed"));
    void Record(AUTWeapon* Weapon, const TCHAR* Kind, uint8 Mode,
        int32 Event, uint32 Generation, const TCHAR* Details, const TCHAR* Protocol = TEXT("fixed"));
    void Projectile(AUTWeapon* Weapon, uint8 Mode, AUTProjectile* Result, const TCHAR* Outcome);
    void Hitscan(AUTWeapon* Weapon, uint8 Mode, const FHitResult& Hit, bool bDealDamage);
    void BeamSample(AUTWeapon* Weapon, uint8 Mode, const FHitResult& Hit);

    class FShotScope
    {
    public:
        FShotScope(AUTWeapon* Weapon, uint8 Mode, int32 Event = INDEX_NONE,
            uint32 Generation = 0, const TCHAR* Protocol = TEXT("stream"));
        ~FShotScope();
        FShotScope(const FShotScope&) = delete;
        FShotScope& operator=(const FShotScope&) = delete;
        TWeakObjectPtr<AUTWeapon> Weapon;
        uint8 Mode;
        int32 Event;
        uint32 Generation;
        const TCHAR* Protocol;
        uint64 Scope = 0;
        int32 Projectiles = 0;
        int32 Traces = 0;
        int32 DamagingTraces = 0;
        FShotScope* Previous = nullptr;
        bool bEnabled = false;
    };

    class FStateScope
    {
    public:
        FStateScope(AUTWeapon* Weapon, UUTWeaponState* Requested);
        ~FStateScope();
        FStateScope(const FStateScope&) = delete;
        FStateScope& operator=(const FStateScope&) = delete;
    private:
        TWeakObjectPtr<AUTWeapon> Weapon;
        TWeakObjectPtr<UUTWeaponState> Before;
        bool bEnabled = false;
    };
}
