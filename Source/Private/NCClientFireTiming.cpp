#include "NCClientFireTiming.h"
#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/UObjectBase.h"

namespace
{
    // TimerManager accumulates its clock in double; UWorld accumulates in float.
    // Reconstruct the former using a one-second epoch timer and its short elapsed
    // remainder. A single long elapsed timer would cast a large clock back to float.
    // All reads occur on the game thread, outside this timer's trivial callback.
    struct FWorldCadenceClock
    {
        double Epoch = 0.0;
        FTimerHandle Anchor;
    };
    struct FWeaponCadence
    {
        TWeakObjectPtr<AUTCharacter> Owner;
        TWeakObjectPtr<UWorld> World;
        TArray<double> LastShot;
    };
    TMap<TWeakObjectPtr<UWorld>, TSharedPtr<FWorldCadenceClock>> Clocks;
    TMap<TWeakObjectPtr<AUTWeaponFix>, FWeaponCadence> Weapons;
    FDelegateHandle CadenceCleanupHandle;

    void CleanupCadenceWorld(UWorld* World, bool, bool)
    {
        const TWeakObjectPtr<UWorld> Key(World);
        const TSharedPtr<FWorldCadenceClock>* Clock = Clocks.Find(Key);
        if (World && Clock) World->GetTimerManager().ClearTimer((*Clock)->Anchor);
        Clocks.Remove(Key);
        for (auto It = Weapons.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid() || It.Value().World.Get() == World) It.RemoveCurrent();
        }
    }

    double CadenceNow(UWorld* World)
    {
        const TWeakObjectPtr<UWorld> Key(World);
        TSharedPtr<FWorldCadenceClock>* Found = Clocks.Find(Key);
        if (!Found)
        {
            for (auto It = Clocks.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid()) It.RemoveCurrent();
            }
            for (auto It = Weapons.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid()) It.RemoveCurrent();
            }
            const TSharedPtr<FWorldCadenceClock> Clock = MakeShareable(new FWorldCadenceClock());
            Clocks.Add(Key, Clock);
            World->GetTimerManager().SetTimer(Clock->Anchor,
                FTimerDelegate::CreateLambda([Clock]() { Clock->Epoch += 1.0; }),
                1.0f, true);
            Found = Clocks.Find(Key);
        }
        const TSharedPtr<FWorldCadenceClock> Clock = *Found;
        return Clock->Epoch + FMath::Max(0.f, World->GetTimerManager().GetTimerElapsed(Clock->Anchor));
    }

    FWeaponCadence* FindCadence(AUTWeaponFix* Weapon)
    {
        FWeaponCadence* Entry = Weapons.Find(TWeakObjectPtr<AUTWeaponFix>(Weapon));
        if (Entry && (Entry->Owner.Get() != Weapon->GetUTOwner()
            || Entry->World.Get() != Weapon->GetWorld()))
        {
            Weapons.Remove(TWeakObjectPtr<AUTWeaponFix>(Weapon));
            Entry = nullptr;
        }
        return Entry;
    }
}

void NCClientFireTiming::Startup()
{
    if (!CadenceCleanupHandle.IsValid())
        CadenceCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&CleanupCadenceWorld);
}

void NCClientFireTiming::Shutdown()
{
    FWorldDelegates::OnWorldCleanup.Remove(CadenceCleanupHandle);
    CadenceCleanupHandle.Reset();
    if (UObjectInitialized())
    {
        for (auto It = Clocks.CreateIterator(); It; ++It)
        {
            UWorld* World = It.Key().Get();
            if (World) World->GetTimerManager().ClearTimer(It.Value()->Anchor);
        }
    }
    Clocks.Empty();
    Weapons.Empty();
}

bool NCClientFireTiming::IsLocal(AUTWeaponFix* Weapon)
{
    return Weapon && Weapon->GetWorld() && Weapon->GetUTOwner()
        && Weapon->GetUTOwner()->IsLocallyControlled()
        && !(Weapon->GetWorld()->DemoNetDriver && Weapon->GetWorld()->DemoNetDriver->IsPlaying());
}

void NCClientFireTiming::Record(AUTWeaponFix* Weapon, uint8 Mode, bool bPreserveCadence)
{
    if (!IsLocal(Weapon) || !Weapon->LastFireTime.IsValidIndex(Mode)) return;
    const double Now = CadenceNow(Weapon->GetWorld());
    FWeaponCadence* Entry = FindCadence(Weapon);
    if (!Entry)
    {
        FWeaponCadence Fresh;
        Fresh.Owner = Weapon->GetUTOwner();
        Fresh.World = Weapon->GetWorld();
        Fresh.LastShot.Init(-1.0, Weapon->LastFireTime.Num());
        Weapons.Add(TWeakObjectPtr<AUTWeaponFix>(Weapon), Fresh);
        Entry = FindCadence(Weapon);
    }
    if (!Entry->LastShot.IsValidIndex(Mode)) return;
    const double Previous = Entry->LastShot[Mode];
    const double Refire = Weapon->GetRefireTime(Mode);
    // Keep small late-frame cadence corrections, but never manufacture future debt.
    Entry->LastShot[Mode] = bPreserveCadence && Previous >= 0.0
        && Now - Previous < Refire + 0.06
        ? FMath::Min(Previous + Refire, Now) : Now;
}

float NCClientFireTiming::Remaining(AUTWeaponFix* Weapon, uint8 Mode)
{
    if (!Weapon || !Weapon->GetWorld()) return 0.f;
    if (IsLocal(Weapon))
    {
        const FWeaponCadence* Entry = FindCadence(Weapon);
        if (Entry && Entry->LastShot.IsValidIndex(Mode) && Entry->LastShot[Mode] >= 0.0)
        {
            return float(Entry->LastShot[Mode] + double(Weapon->GetRefireTime(Mode))
                - CadenceNow(Weapon->GetWorld()));
        }
    }
    // Remote authority and modes that have not passed a local shot use the native
    // timestamp path. This keeps server rate validation and replay behavior intact.
    return Weapon->LastFireTime.IsValidIndex(Mode) && Weapon->LastFireTime[Mode] > 0.f
        ? Weapon->LastFireTime[Mode] + Weapon->GetRefireTime(Mode) - Weapon->GetWorld()->GetTimeSeconds()
        : 0.f;
}

float NCClientFireTiming::MaxRemaining(AUTWeaponFix* Weapon)
{
    float RemainingTime = Weapon && Weapon->GetWorld()
        ? FMath::Max(0.f, Weapon->EarliestFireTime - Weapon->GetWorld()->GetTimeSeconds()) : 0.f;
    if (Weapon)
    {
        for (int32 Mode = 0; Mode < Weapon->LastFireTime.Num(); ++Mode)
            RemainingTime = FMath::Max(RemainingTime, Remaining(Weapon, uint8(Mode)));
    }
    return RemainingTime;
}

void NCClientFireTiming::Forget(AUTWeaponFix* Weapon)
{
    Weapons.Remove(TWeakObjectPtr<AUTWeaponFix>(Weapon));
}
