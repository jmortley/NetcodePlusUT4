#include "NetcodePlus.h"

// Isolated migration diagnostic. No compiled code or registration in game/server targets.
#if WITH_EDITOR
#include "TeamArenaCharacter.h"
#include "TeamArenaCharacterMovement.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace NCMigrationMovementProbe
{
static FString RunId;
static FString Side;
static FString Variant;
static FDelegateHandle PreTickHandle;
static FDelegateHandle PostTickHandle;
static TWeakObjectPtr<AUTPlayerController> SelectedController;
static TWeakObjectPtr<AUTCharacter> SelectedPawn;
static double RegisteredAt = 0.0;
static double PhaseStartedAt = 0.0;
static double LastSampleAt = 0.0;
static double ReadySince = 0.0;
static FString Phase(TEXT("waiting"));
static FVector Direction = FVector::ZeroVector;
static FVector InitialLocation = FVector::ZeroVector;
static bool bFinished = false;
static bool bSawJump = false;
static bool bSawSlide = false;
static bool bJumpReleased = false;
static int32 InputFrames = 0;
static FKey JumpKey;
static FKey SlideKey;
static bool bJumpKeyHeld = false;
static bool bSlideKeyHeld = false;
static constexpr double TimeoutSeconds = 175.0;
static constexpr float RunwayLength = 1800.0f;

static FString ExpectedPawnClass()
{
    return Variant == TEXT("NonStock")
        ? TEXT("/Game/Blueprints/Netcode/TeamArenaCharacter.TeamArenaCharacter_C")
        : TEXT("/Game/Blueprints/Netcode/TeamArenaCharacterStock.TeamArenaCharacterStock_C");
}

static void Record(const TCHAR* Kind, UWorld* World, AUTCharacter* Pawn, const FString& Detail = FString())
{
    TSharedRef<FJsonObject> Data = MakeShareable(new FJsonObject());
    Data->SetStringField(TEXT("kind"), Kind);
    Data->SetStringField(TEXT("run"), RunId);
    Data->SetStringField(TEXT("side"), Side);
    Data->SetStringField(TEXT("variant"), Variant);
    Data->SetStringField(TEXT("phase"), Phase);
    Data->SetStringField(TEXT("detail"), Detail);
    Data->SetNumberField(TEXT("elapsed"), FPlatformTime::Seconds() - RegisteredAt);
    Data->SetNumberField(TEXT("inputFrames"), InputFrames);
    Data->SetNumberField(TEXT("readySeconds"), ReadySince > 0.0 ? FPlatformTime::Seconds() - ReadySince : 0.0);
    Data->SetStringField(TEXT("jumpKey"), JumpKey.ToString());
    Data->SetStringField(TEXT("slideKey"), SlideKey.ToString());
    if (World)
    {
        Data->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
        Data->SetNumberField(TEXT("netMode"), int32(World->GetNetMode()));
        AGameStateBase* State = World->GetGameState();
        Data->SetNumberField(TEXT("serverTime"), State ? State->GetServerWorldTimeSeconds() : World->GetTimeSeconds());
        if (AUTGameState* UTState = Cast<AUTGameState>(State))
        {
            Data->SetStringField(TEXT("matchState"), UTState->GetMatchState().ToString());
        }
    }
    if (Pawn)
    {
        Data->SetStringField(TEXT("pawn"), Pawn->GetName());
        Data->SetStringField(TEXT("class"), Pawn->GetClass()->GetPathName());
        Data->SetNumberField(TEXT("role"), int32(Pawn->GetLocalRole()));
        Data->SetBoolField(TEXT("local"), Pawn->IsLocallyControlled());
        if (AController* Controller = Pawn->GetController())
        {
            Data->SetBoolField(TEXT("moveInputIgnored"), Controller->IsMoveInputIgnored());
        }
        APlayerState* PlayerState = Pawn->GetPlayerState();
        Data->SetNumberField(TEXT("playerId"), PlayerState ? PlayerState->GetPlayerId() : -1);
        const FVector Location = Pawn->GetActorLocation();
        const FVector Velocity = Pawn->GetVelocity();
        Data->SetNumberField(TEXT("x"), Location.X);
        Data->SetNumberField(TEXT("y"), Location.Y);
        Data->SetNumberField(TEXT("z"), Location.Z);
        Data->SetNumberField(TEXT("vx"), Velocity.X);
        Data->SetNumberField(TEXT("vy"), Velocity.Y);
        Data->SetNumberField(TEXT("vz"), Velocity.Z);
        Data->SetNumberField(TEXT("speed"), Velocity.Size());
        Data->SetNumberField(TEXT("horizontalSpeed"), Velocity.Size2D());
        if (UUTCharacterMovement* Movement = Pawn->UTCharacterMovement)
        {
            Data->SetStringField(TEXT("movementClass"), Movement->GetClass()->GetPathName());
            Data->SetNumberField(TEXT("movementMode"), int32(Movement->MovementMode));
            Data->SetBoolField(TEXT("sliding"), Movement->bIsFloorSliding);
            Data->SetBoolField(TEXT("falling"), Movement->IsFalling());
            Data->SetNumberField(TEXT("maxInitialSlide"), Movement->MaxInitialFloorSlideSpeed);
            Data->SetNumberField(TEXT("maxSlide"), Movement->MaxFloorSlideSpeed);
            Data->SetNumberField(TEXT("acceleration"), Movement->GetCurrentAcceleration().Size());
            Data->SetNumberField(TEXT("movementTime"), Movement->GetCurrentMovementTime());
        }
    }
    FString Line;
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
    FJsonSerializer::Serialize(Data, Writer);
    UE_LOG(LogTemp, Display, TEXT("[NCPMigrationMovement] %s"), *Line);
}

// Resolve live, unmodified action mappings and let the normal input stack call the
// controller's protected Jump/JumpRelease and Slide/StopSlide handlers.
static bool FindActionKey(AUTPlayerController* Controller, FName Action, FKey& OutKey)
{
    if (!Controller || !Controller->PlayerInput || !Controller->InputComponent) { return false; }
    int32 PressHandlers = 0;
    int32 ReleaseHandlers = 0;
    for (int32 Index = 0; Index < Controller->InputComponent->GetNumActionBindings(); ++Index)
    {
        const FInputActionBinding& Binding = Controller->InputComponent->GetActionBinding(Index);
        if (Binding.GetActionName() == Action && Binding.ActionDelegate.IsBound())
        {
            PressHandlers += Binding.KeyEvent == IE_Pressed ? 1 : 0;
            ReleaseHandlers += Binding.KeyEvent == IE_Released ? 1 : 0;
        }
    }
    if (PressHandlers != 1 || ReleaseHandlers != 1) { return false; }
    for (const FInputActionKeyMapping& Mapping : Controller->PlayerInput->GetKeysForAction(Action))
    {
        if (Mapping.Key.IsValid() && !Mapping.Key.IsGamepadKey()
            && !Mapping.bShift && !Mapping.bCtrl && !Mapping.bAlt && !Mapping.bCmd)
        {
            OutKey = Mapping.Key;
            return true;
        }
    }
    return false;
}

static void ReleaseInput()
{
    AUTPlayerController* Controller = SelectedController.Get();
    if (Controller && Controller->IsLocalController())
    {
        // Release only keys this probe pressed, even if possession changed.
        if (bJumpKeyHeld) { Controller->InputKey(JumpKey, IE_Released, 0.f, false); }
        if (bSlideKeyHeld) { Controller->InputKey(SlideKey, IE_Released, 0.f, false); }
    }
    bJumpKeyHeld = false;
    bSlideKeyHeld = false;
}

static void Finish(UWorld* World, const FString& Reason, bool bComplete = false)
{
    if (bFinished) { return; }
    ReleaseInput();
    Record(bComplete ? TEXT("input-complete") : TEXT("incomplete"), World, SelectedPawn.Get(), Reason);
    bFinished = true;
}

// Read-only collision checks: no teleport, physics edits, temporary actors, or nav changes.
static bool FindRunway(UWorld* World, AUTCharacter* Pawn, FVector& OutDirection)
{
    UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent();
    if (!Capsule) { return false; }
    const FVector Start = Pawn->GetActorLocation();
    const float Radius = Capsule->GetScaledCapsuleRadius();
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(NCMigrationRunway), false, Pawn);
    // Raising and extending the capsule also reserves headroom for a normal jump.
    const FVector SweepStart = Start + FVector(0.f, 0.f, 160.f);
    const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius + 5.f, HalfHeight + 160.f - 3.f);
    for (int32 DirectionIndex = 0; DirectionIndex < 16; ++DirectionIndex)
    {
        const FVector Candidate = FRotator(0.f, Pawn->GetControlRotation().Yaw + DirectionIndex * 22.5f, 0.f).Vector();
        FHitResult Obstacle;
        if (World->SweepSingleByChannel(Obstacle, SweepStart, SweepStart + Candidate * RunwayLength,
            FQuat::Identity, ECC_Pawn, Shape, Params)) { continue; }
        bool bHasFloor = true;
        for (int32 Step = 0; Step <= 18; ++Step)
        {
            const FVector Point = Start + Candidate * (100.f * Step);
            FHitResult FloorHit;
            if (!World->LineTraceSingleByChannel(FloorHit, Point, Point - FVector(0.f, 0.f, HalfHeight + 100.f), ECC_Pawn, Params)
                || FloorHit.ImpactNormal.Z < 0.9f
                || FMath::Abs((Start.Z - HalfHeight) - FloorHit.ImpactPoint.Z) > 20.f)
            {
                bHasFloor = false;
                break;
            }
        }
        if (bHasFloor) { OutDirection = Candidate; return true; }
    }
    return false;
}

static bool IsTestWorld(UWorld* World)
{
    return World && World->IsGameWorld()
        && World->GetOutermost()->GetName() == TEXT("/Game/RestrictedAssets/Maps/DM-Outpost23")
        && ((Side == TEXT("InputClient") && World->GetNetMode() == NM_Client)
            || (Side == TEXT("ObserveServer") && World->GetNetMode() == NM_DedicatedServer));
}

static bool IsExpectedPawn(AUTCharacter* Pawn)
{
    return Pawn && !Pawn->IsDead() && Pawn->GetClass()->GetPathName() == ExpectedPawnClass()
        && Pawn->UTCharacterMovement && Pawn->UTCharacterMovement->IsA<UTeamArenaCharacterMovement>();
}

static void SetPhase(UWorld* World, const TCHAR* NewPhase)
{
    Phase = NewPhase;
    PhaseStartedAt = FPlatformTime::Seconds();
    Record(TEXT("phase"), World, SelectedPawn.Get());
}

static void PreTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
    if (bFinished || !IsTestWorld(World) || Side != TEXT("InputClient") || TickType == LEVELTICK_ViewportsOnly) { return; }
    const double Now = FPlatformTime::Seconds();
    if (Now - RegisteredAt > TimeoutSeconds) { Finish(World, TEXT("Timed out waiting for a playable pawn, safe runway, or phase completion.")); return; }
    if (Phase == TEXT("waiting"))
    {
        AUTPlayerController* Controller = Cast<AUTPlayerController>(World->GetFirstPlayerController());
        AUTCharacter* Pawn = Controller ? Cast<AUTCharacter>(Controller->GetPawn()) : nullptr;
        AUTGameState* State = World->GetGameState<AUTGameState>();
        if (!Controller || !Controller->IsLocalController() || Controller->IsMoveInputIgnored()
            || !IsExpectedPawn(Pawn) || Pawn->GetLocalRole() != ROLE_AutonomousProxy
            || !Pawn->IsLocallyControlled() || !State || State->GetMatchState() == FName(TEXT("PlayerIntro")))
        {
            ReadySince = 0.0;
            SelectedController.Reset();
            SelectedPawn.Reset();
            return;
        }
        // Readiness is stable possession and input eligibility. Small replicated
        // floor corrections may transiently report Falling while standing still;
        // they delay input until a real grounded frame, not restart this timer.
        if (ReadySince == 0.0 || SelectedController.Get() != Controller || SelectedPawn.Get() != Pawn)
        {
            SelectedController = Controller;
            SelectedPawn = Pawn;
            ReadySince = Now;
            return;
        }
        if (Now - ReadySince < 1.0 || !Pawn->UTCharacterMovement->IsMovingOnGround()
            || Pawn->GetVelocity().Size() > 5.f) { return; }
        const float ExpectedSlide = Variant == TEXT("NonStock") ? 1100.f : 900.f;
        if (!FMath::IsNearlyEqual(Pawn->UTCharacterMovement->MaxInitialFloorSlideSpeed, 1350.f, 0.01f)
            || !FMath::IsNearlyEqual(Pawn->UTCharacterMovement->MaxFloorSlideSpeed, ExpectedSlide, 0.01f))
        {
            Finish(World, TEXT("Live movement limits do not match the requested Blueprint route."));
            return;
        }
        if (!FindRunway(World, Pawn, Direction))
        {
            Finish(World, TEXT("No 1800 cm level runway with jump headroom exists at this spawn; no input injected."));
            return;
        }
        if (!FindActionKey(Controller, FName(TEXT("Jump")), JumpKey)
            || !FindActionKey(Controller, FName(TEXT("Slide")), SlideKey) || JumpKey == SlideKey)
        {
            Finish(World, TEXT("Missing distinct live Jump/Slide keys with paired ordinary input handlers; no input injected."));
            return;
        }
        InitialLocation = Pawn->GetActorLocation();
        Record(TEXT("begin"), World, Pawn, FString::Printf(TEXT("Read-only runway direction %s, length %.0f cm"), *Direction.ToString(), RunwayLength));
        SetPhase(World, TEXT("jump"));
        Controller->InputKey(JumpKey, IE_Pressed, 1.f, false);
        bJumpKeyHeld = true;
        return;
    }
    AUTPlayerController* Controller = SelectedController.Get();
    AUTCharacter* Pawn = SelectedPawn.Get();
    if (!Controller || !IsExpectedPawn(Pawn) || Controller->GetPawn() != Pawn
        || Pawn->GetLocalRole() != ROLE_AutonomousProxy || !Pawn->IsLocallyControlled() || Controller->IsMoveInputIgnored())
    {
        Finish(World, TEXT("Selected pawn died, changed, lost possession, or stopped accepting movement."));
        return;
    }
    UUTCharacterMovement* Movement = Pawn->UTCharacterMovement;
    const double PhaseElapsed = Now - PhaseStartedAt;
    bSawJump |= Phase == TEXT("jump") && Movement->IsFalling() && Pawn->GetVelocity().Z > 50.f;
    bSawSlide |= Movement->bIsFloorSliding;
    if (Phase == TEXT("jump"))
    {
        if (!bJumpReleased && PhaseElapsed > 0.1)
        {
            Controller->InputKey(JumpKey, IE_Released, 0.f, false);
            bJumpKeyHeld = false;
            bJumpReleased = true;
        }
        if (bSawJump && PhaseElapsed > 0.4 && Movement->IsMovingOnGround()) { SetPhase(World, TEXT("run")); }
        else if (PhaseElapsed > 4.0) { Finish(World, TEXT("Normal jump and landing were not observed.")); }
    }
    else if (Phase == TEXT("run") || Phase == TEXT("slide"))
    {
        if (FVector::Dist2D(InitialLocation, Pawn->GetActorLocation()) > RunwayLength - 300.f)
        {
            Finish(World, TEXT("Runway distance budget reached; input released."));
            return;
        }
        Pawn->AddMovementInput(Direction, 1.f, false);
        ++InputFrames;
        if (Phase == TEXT("run") && PhaseElapsed >= 0.6)
        {
            if (Pawn->GetVelocity().Size2D() <= 0.5f * Movement->MaxWalkSpeed || Movement->GetCurrentAcceleration().IsNearlyZero())
            {
                Finish(World, TEXT("Run-up did not reach the normal slide input preconditions."));
                return;
            }
            SetPhase(World, TEXT("slide"));
            Controller->InputKey(SlideKey, IE_Pressed, 1.f, false);
            bSlideKeyHeld = true;
        }
        else if (Phase == TEXT("slide") && PhaseElapsed >= 0.9)
        {
            Controller->InputKey(SlideKey, IE_Released, 0.f, false);
            bSlideKeyHeld = false;
            SetPhase(World, TEXT("settle"));
        }
    }
    else if (Phase == TEXT("settle") && PhaseElapsed >= 3.0)
    {
        const bool bMoved = FVector::Dist2D(InitialLocation, Pawn->GetActorLocation()) >= 100.f;
        const bool bSettled = Movement->IsMovingOnGround() && Pawn->GetVelocity().Size() < 5.f;
        Finish(World, bSawJump && bSawSlide && bMoved && bSettled
            ? TEXT("Client input sequence completed; authoritative server evidence must still be checked by the harness.")
            : TEXT("Missing client displacement, jump, floor-slide, or settled-state evidence."), bSawJump && bSawSlide && bMoved && bSettled);
    }
}

static void PostTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
    if (!IsTestWorld(World) || TickType == LEVELTICK_ViewportsOnly) { return; }
    const double Now = FPlatformTime::Seconds();
    if (Now - RegisteredAt > (Side == TEXT("ObserveServer") ? 370.0 : TimeoutSeconds + 5.0) || Now - LastSampleAt < 0.1) { return; }
    LastSampleAt = Now;
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* Controller = It->Get();
        AUTCharacter* Pawn = Controller ? Cast<AUTCharacter>(Controller->GetPawn()) : nullptr;
        if (!Pawn || !Pawn->GetPlayerState() || !Pawn->UTCharacterMovement) { continue; }
        if (Side == TEXT("InputClient") && (!Controller->IsLocalController() || Pawn->GetLocalRole() != ROLE_AutonomousProxy)) { continue; }
        Record(TEXT("sample"), World, Pawn);
    }
}
}

void RegisterNCMigrationMovementProbe()
{
    using namespace NCMigrationMovementProbe;
    if (IsRunningCommandlet() || PreTickHandle.IsValid() || PostTickHandle.IsValid()
        || !FParse::Value(FCommandLine::Get(), TEXT("NCPMigrationMovementProbe="), RunId)) { return; }
    FParse::Value(FCommandLine::Get(), TEXT("NCPMigrationMovementRole="), Side);
    FParse::Value(FCommandLine::Get(), TEXT("NCPMigrationMovementPawn="), Variant);
    bool bValidRunId = !RunId.IsEmpty() && RunId.Len() <= 64;
    for (TCHAR Character : RunId) { bValidRunId &= FChar::IsAlnum(Character) || Character == TCHAR('-') || Character == TCHAR('_'); }
    if (!bValidRunId || (Side != TEXT("InputClient") && Side != TEXT("ObserveServer"))
        || (Variant != TEXT("Stock") && Variant != TEXT("NonStock")))
    {
        UE_LOG(LogTemp, Warning, TEXT("[NCPMigrationMovement] Invalid explicit probe arguments; no hooks registered."));
        return;
    }
    RegisteredAt = FPlatformTime::Seconds();
    Phase = TEXT("waiting");
    bFinished = false;
    bSawJump = false;
    bSawSlide = false;
    bJumpReleased = false;
    InputFrames = 0;
    JumpKey = SlideKey = EKeys::Invalid;
    bJumpKeyHeld = bSlideKeyHeld = false;
    ReadySince = LastSampleAt = 0.0;
    SelectedController.Reset();
    SelectedPawn.Reset();
    PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddStatic(&PreTick);
    PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddStatic(&PostTick);
    Record(TEXT("registered"), nullptr, nullptr, TEXT("WITH_EDITOR opt-in only; server observes; client uses existing movement input; no RPC or pawn replacement."));
}

void UnregisterNCMigrationMovementProbe()
{
    using namespace NCMigrationMovementProbe;
    // Only delegates and weak references are touched: also safe before the module's late-exit UObject guard.
    if (PreTickHandle.IsValid()) { FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle); PreTickHandle.Reset(); }
    if (PostTickHandle.IsValid()) { FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle); PostTickHandle.Reset(); }
    SelectedController.Reset();
    SelectedPawn.Reset();
}
#endif // WITH_EDITOR
