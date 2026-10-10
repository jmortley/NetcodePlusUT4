#include "NetcodePlus.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerTarget.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "UTPlusFlakCannon.h"
#include "UTPlusShockRifle.h"
#include "UTProjectile.h"
#include "UTDamageType.h"
#include "UTCharacterMovement.h"
#include "Components/CapsuleComponent.h"

void ANCAimTrainerGame::PublishDrillStatus()
{
    if (!Trainee || Progress.Phase != 2) { return; }
    if (NCAimTrainerScenarioPolicy::IsShockDefense(Progress.Scenario))
    {
        Trainee->SetTrainerOnlineStatus(FString::Printf(TEXT("%d / 5 CONSECUTIVE HITS   |   %d STOPS   |   %d CAPTURES"),
            Drill.Streak, Drill.Stops, Progress.TargetsExpired));
    }
}

void ANCAimTrainerGame::UpdateDrillShots()
{
    if (Progress.Shots <= Drill.Shot) { return; }
    const float Now = GetWorld()->GetTimeSeconds();
    Drill.ObserveShot(Progress.Shots, Now);
    if (NCAimTrainerScenarioPolicy::IsFlakScenario(Progress.Scenario) && Targets.IsValidIndex(0) && Targets[0]->IsAvailable())
    {
        if (FlakAttemptShot != 0) { RetireDrillTarget(false, Now); }
        else { FlakAttemptShot = Drill.Shot; }
    }
    PublishDrillStatus();
}

void ANCAimTrainerGame::ActivateDrillTarget(float Now)
{
    ANCAimTrainerTarget* Target = Targets[0];
    const float Height = Target->GetClass()->GetDefaultObject<ANCAimTrainerTarget>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const bool bFlak = NCAimTrainerScenarioPolicy::IsFlakScenario(Progress.Scenario);
    FVector Position(NCAimTrainerDrillPolicy::RunnerStartX, Schedule.FRandRange(-250.f, 250.f), Height);
    if (bFlak)
    {
        // Half a native dodge's airborne horizontal travel, capped for this
        // close-range room. Randomise the seat and its angle on every attempt.
        const UUTCharacterMovement* Move = Cast<UUTCharacterMovement>(Target->GetCharacterMovement());
        const float HalfDodge = Move && Move->GetGravityZ() < -1.f
            ? Move->DodgeImpulseHorizontal * Move->DodgeImpulseVertical / -Move->GetGravityZ() : 300.f;
        const float Radius = FMath::Min(320.f, HalfDodge) * Schedule.FRandRange(0.82f, 1.f);
        const float Angle = (float(Schedule.RandRange(0, 5)) * 60.f + Schedule.FRandRange(-20.f, 20.f)) * (PI / 180.f);
        Position = FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Height);
        ClearTrainerProjectiles();
    }
    Target->ActivateTarget(ArenaOrigin + Position, true);
    Target->ConfigureDrillMovement(!bFlak, ArenaOrigin);
    Target->ChooseDrillStrafe(Schedule.FRand(), Schedule.FRand());
    Target->SetActorRotation(FRotator(0.f, bFlak ? FMath::Atan2(-Position.Y, -Position.X) * (180.f / PI) : 180.f, 0.f));
    ++LocalAppearances[0];
    TargetExpiry[0] = PhaseStartedAt + 60.f;
    Drill.NextTarget();
    FlakAttemptShot = 0;
    FlakPellets.Empty();
    bDrillDodgeQueued = false;
    NextDrillAction = Now + (bFlak ? Schedule.FRandRange(0.08f, 0.26f) : Schedule.FRandRange(0.18f, 0.45f));
    NextWiggleTime[0] = Now + Schedule.FRandRange(0.18f, 0.42f);
    NextCrouchTime[0] = CrouchEndTime[0] = 0.f;
    PublishDrillStatus();
}

void ANCAimTrainerGame::RetireDrillTarget(bool bSuccess, float Now)
{
    ANCAimTrainerTarget* Target = Targets[0];
    if (!Target->IsAvailable()) { return; }
    if (NCAimTrainerScenarioPolicy::IsFlakScenario(Progress.Scenario))
    {
        if (bSuccess) { ++Progress.Hits; }
        else { ++Progress.TargetsExpired; }
        RecordLocalTarget(0, bSuccess);
    }
    else if (!bSuccess)
    {
        ++Progress.TargetsExpired;
        RecordLocalTarget(0, false);
    }
    Target->HideTarget();
    Drill.NextTarget();
    NextTargetTime[0] = Now + 0.18f;
    PublishDrillStatus();
}

void ANCAimTrainerGame::UpdateDrillTargets(float Now)
{
    if (!Targets.IsValidIndex(0) || !Targets[0] || Now >= PhaseStartedAt + 60.f) { return; }
    ANCAimTrainerTarget* Target = Targets[0];
    if (!Target->IsAvailable())
    {
        if (Now >= NextTargetTime[0]) { ActivateDrillTarget(Now); }
        return;
    }
    const bool bFlak = NCAimTrainerScenarioPolicy::IsFlakScenario(Progress.Scenario);
    if (bFlak && FlakAttemptShot > 0 && Now - Drill.ShotAt >= NCAimTrainerDrillPolicy::FlakResolveSeconds)
    {
        RetireDrillTarget(false, Now);
        return;
    }
    const FVector Position = Target->GetActorLocation() - ArenaOrigin;
    if (!bFlak && Position.X <= NCAimTrainerDrillPolicy::CaptureX + 120.f
        && FMath::Abs(Position.Y) < 180.f && Position.Z < 500.f)
    {
        RetireDrillTarget(false, Now);
        return;
    }
    Target->TryTrainerDrillWallDodge();
    if (Now >= NextWiggleTime[0])
    {
        // Pick fresh lateral/depth intent, not an alternating A/D loop. These
        // choices use the run's random stream and never inspect the crosshair.
        Target->ChooseDrillStrafe(Schedule.FRand(), Schedule.FRand());
        NextWiggleTime[0] = Now + Schedule.FRandRange(0.18f, 0.42f);
    }
    if (CrouchEndTime[0] > 0.f)
    {
        if (Now >= CrouchEndTime[0] && Target->SetTrainerCrouched(false)) { CrouchEndTime[0] = 0.f; }
    }
    else if (Now >= NextDrillAction)
    {
        if (!Target->CanStartTrainerDrillAction())
        {
            // Choose the next feint after native landing recovery. Choosing
            // while airborne would reject every crouch and silently skip it.
            NextDrillAction = Now + Schedule.FRandRange(0.08f, 0.16f);
            return;
        }
        if (!bDrillDodgeQueued && Schedule.FRand() < (bFlak ? 0.16f : 0.38f) && Target->SetTrainerCrouched(true))
        {
            bDrillDodgeQueued = true;
            CrouchEndTime[0] = Now + Schedule.FRandRange(0.12f, 0.28f);
            NextDrillAction = CrouchEndTime[0] + Schedule.FRandRange(0.04f, 0.12f);
        }
        else if (Target->TryTrainerDrillDodge(Schedule.FRand()))
        {
            bDrillDodgeQueued = false;
            NextDrillAction = Now + (bFlak ? Schedule.FRandRange(0.3f, 0.65f) : Schedule.FRandRange(0.48f, 0.95f));
        }
        else
        {
            // A native landing/cooldown rejection must not skip the next dodge
            // for another whole action interval. Retry without bypassing it.
            bDrillDodgeQueued = true;
            NextDrillAction = Now + Schedule.FRandRange(0.08f, 0.16f);
        }
    }
}

float ANCAimTrainerGame::RecordDrillHit(ANCAimTrainerTarget* Target, float Damage,
    const FDamageEvent& Event, AActor* Causer)
{
    if (Target != Targets[0]) { return 0.f; }
    const float Now = GetWorld()->GetTimeSeconds();
    const bool bFlak = NCAimTrainerScenarioPolicy::IsFlakScenario(Progress.Scenario);
    if (bFlak)
    {
        const AUTPlusFlakCannon* Flak = Cast<AUTPlusFlakCannon>(RunWeapon);
        AUTProjectile* Pellet = Cast<AUTProjectile>(Causer);
        if (!Flak || !Pellet || Pellet->Role != ROLE_Authority || Pellet->InstigatorController != Trainee
            || Pellet->GetInstigator() != Trainee->GetPawn() || Event.DamageTypeClass != Pellet->MyDamageType
            || Pellet->CreationTime < Target->GetAppearanceTime()
            || (!(Flak->ProjClass.IsValidIndex(0) && Pellet->GetClass() == Flak->ProjClass[0])
                && !(Flak->MultiShotProjClass.IsValidIndex(0) && Pellet->GetClass() == Flak->MultiShotProjClass[0]))) { return 0.f; }
        UpdateShotCount(); // The native multishot counter increments once before spawning its shards.
        if (!Target->IsAvailable() || FlakAttemptShot != Drill.Shot || FlakAttemptShot == 0
            || Now - Drill.ShotAt > NCAimTrainerDrillPolicy::FlakResolveSeconds
            || FlakPellets.Num() >= 32 || FlakPellets.Contains(Pellet)) { return 0.f; }
        FlakPellets.Add(Pellet);
        Drill.FlakDamage += Damage;
        if (Drill.FlakDamage >= NCAimTrainerDrillPolicy::FlakKillDamage) { RetireDrillTarget(true, Now); }
    }
    else
    {
        AUTPlusShockRifle* Shock = Cast<AUTPlusShockRifle>(RunWeapon);
        if (Causer != RunWeapon || !Shock || !Event.IsOfType(FUTPointDamageEvent::ClassID) || !Shock->InstantHitInfo.IsValidIndex(0)
            || Shock->GetCurrentFireMode() != 0 || Event.DamageTypeClass != Shock->InstantHitInfo[0].DamageType) { return 0.f; }
        const float Rewind = Shock->GetHitValidationPredictionTime();
        if (!FMath::IsFinite(Rewind) || Rewind < 0.f || Now - Rewind < Target->GetAppearanceTime()) { return 0.f; }
        UpdateShotCount();
        if (Drill.Shot <= 0 || Drill.ShotHit) { return 0.f; }
        ++Progress.Hits;
        RecordLocalTarget(0, true);
        const bool bStopped = Drill.ShockHit();
        Target->ApplyTrainerShockMomentum(Event);
        NextDrillAction = FMath::Max(NextDrillAction, Now + 0.4f);
        if (bStopped) { RetireDrillTarget(true, Now); }
        PublishDrillStatus();
    }
    Trainee->NotifyTrainerHit(Damage);
    UpdateShotCount();
    return Damage;
}
