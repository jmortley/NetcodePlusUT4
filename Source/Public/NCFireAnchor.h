#pragma once
#include "NetcodePlus.h"

class AUTCharacter;
class AUTWeapon;
class APlayerController;
class UNetConnection;

/** Server-only immutable shot snapshot. A movement marker is bounded evidence,
 *  not proof of when the physical click happened. Never reused by another shot. */
struct FNCFireAnchor
{
    bool bValid = false;
    bool bEnforce = false;
    float AcceptedAt = 0.f;
    float MoveProcessedAt = 0.f;
    float ExtraAtAccept = 0.f;
    float BaseRewind = 0.f;
    float ObservedRTTMs = 0.f;
    float Cap = 0.f;
    uint64 Epoch = 0;
    FVector Origin = FVector::ZeroVector;
    TWeakObjectPtr<AUTCharacter> Pawn;
    TWeakObjectPtr<AUTWeapon> Weapon;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<APlayerController> Controller;
    TWeakObjectPtr<UNetConnection> Connection;
};

namespace NCFireAnchor
{
    void Startup();
    void Shutdown();
    // 0 disabled; 1 evaluates/logs only; 2 bounded correction. Default = shadow.
    int32 Mode();
    void RecordMove(AUTCharacter* Pawn, bool bShotSpawned);
    void InvalidateWeapon(AUTWeapon* Weapon);
    FNCFireAnchor Resolve(AUTWeapon* Weapon, AUTCharacter* Pawn, uint8 FireMode,
        int32 Event, float MoveTime, const FVector& Origin, const FRotator& Aim,
        float BaseRewind, float ObservedRTTMs, float Cap);
    bool Dispatch(const FNCFireAnchor& Anchor, float Now, float& ExtraSeconds);
    bool LogEnabled();
}
