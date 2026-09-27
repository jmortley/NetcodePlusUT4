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
    FString WireTimestamp(float Value);
    void Record(AUTWeapon* Weapon, const TCHAR* Kind, uint8 Mode,
        int32 Event = INDEX_NONE, uint32 Generation = 0,
        const FString& Details = FString(), const TCHAR* Protocol = TEXT("fixed"));
    void Record(AUTWeapon* Weapon, const TCHAR* Kind, uint8 Mode,
        int32 Event, uint32 Generation, const TCHAR* Details, const TCHAR* Protocol = TEXT("fixed"));
    void Projectile(AUTWeapon* Weapon, uint8 Mode, AUTProjectile* Result, const TCHAR* Outcome);
    void Hitscan(AUTWeapon* Weapon, uint8 Mode, const FHitResult& Hit, bool bDealDamage);
    void BeamSample(AUTWeapon* Weapon, uint8 Mode, const FHitResult& Hit);
    void Layout(AUTWeapon* Weapon, uint8 Mode, UUTWeaponState* State);
    void StockValidated(AUTWeapon* Weapon, uint8 Mode, uint8 Event, uint8 Before, uint8 After, bool Accepted);
    void StockSent(AUTWeapon* Weapon, uint8 Mode, uint8 Event, bool Start, bool ClientFired);
    void SequenceBegin(AUTWeapon* Weapon, uint8 Mode, bool Start);
    void SequenceEnd(AUTWeapon* Weapon, uint8 Mode, bool Start, bool Result);
    void ChargeCommitted(AUTWeapon* Weapon, uint8 Mode, int32 Loaded);
    void ChargeEnded(AUTWeapon* Weapon, uint8 Mode);

    // Diagnostic-only synchronous identity. Consumed by at most one dispatch.
    class FRequestScope
    {
    public:
        FRequestScope(AUTWeapon* Weapon, uint8 Mode, int32 Event, uint32 Generation);
        ~FRequestScope();
        FRequestScope(const FRequestScope&) = delete;
        FRequestScope& operator=(const FRequestScope&) = delete;
        TWeakObjectPtr<AUTWeapon> Weapon;
        uint8 Mode;
        int32 Event;
        uint32 Generation;
        uint64 Epoch = 0;
        bool Claimed = false;
        FRequestScope* Previous = nullptr;
    };

    class FInputScope
    {
    public:
        FInputScope(AUTWeapon* Weapon, uint8 Mode, bool Start);
        ~FInputScope();
        FInputScope(const FInputScope&) = delete;
        FInputScope& operator=(const FInputScope&) = delete;
        TWeakObjectPtr<AUTWeapon> Weapon;
        uint8 Mode;
        bool Start;
        uint64 Id = 0, Epoch = 0;
        FInputScope* Previous = nullptr;
    };

    class FStockScope
    {
    public:
        FStockScope(AUTWeapon* Weapon, uint8 Mode, uint8 Event, bool Start, bool Sync, const TCHAR* Route);
        ~FStockScope();
        FStockScope(const FStockScope&) = delete;
        FStockScope& operator=(const FStockScope&) = delete;
        TWeakObjectPtr<AUTWeapon> Weapon;
        uint8 Mode, Event;
        bool Start, Sync;
        int32 Accepted = -1, Applied = 0, ApplyResult = -1;
        uint64 Id = 0, Epoch = 0;
        FStockScope* Previous = nullptr;
    };

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
        uint64 Epoch = 0;
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
        uint64 Epoch = 0;
    };
}
