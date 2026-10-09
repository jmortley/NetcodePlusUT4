#include "TeamArenaCharacter.h"
#include "UTPlayerController.h"
#include "UTWeapon.h"
#include "UTWeaponStateInactive.h"
#include "UTGameState.h"
#include "HAL/PlatformTime.h"
#include "Engine/DemoNetDriver.h"
#if !UE_SERVER
#include "UTLocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Console.h"
#include "UnrealClient.h"
#endif

void ATeamArenaCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
#if !UE_SERVER
	AUTPlayerController* PC = Cast<AUTPlayerController>(Controller);
	UWorld* World = GetWorld();
	if (World == nullptr || GetNetMode() == NM_DedicatedServer || PC == nullptr
		|| !PC->IsLocalController() || PC->GetPawn() != this
		|| IsDead() || IsPendingKillPending())
	{
		CancelSpawnHeldFire();
		return;
	}
	// Repeated restart RPCs for the same pawn/controller must not extend the
	// deadline or manufacture another synthetic press after recovery completed.
	if (SpawnHeldFireController.Get() == PC)
	{
		return;
	}
	CancelSpawnHeldFire();
	SpawnHeldFireController = PC;
	SpawnHeldFireWorld = World;
	SpawnHeldFireWeapon = GetWeapon();
	bSpawnHeldFireWeaponBound = GetWeapon() != nullptr;
	SpawnHeldFireDeadline = FPlatformTime::Seconds() + 0.5;
	// ClientRestart changes the controller to Playing AFTER PawnClientRestart.
	World->GetTimerManager().SetTimerForNextTick(
		this, &ATeamArenaCharacter::RetrySpawnHeldFire);
#endif
}

void ATeamArenaCharacter::CancelSpawnHeldFire()
{
	UWorld* World = SpawnHeldFireWorld.Get();
	if (World != nullptr)
	{
		World->GetTimerManager().ClearTimer(SpawnHeldFireHandle);
	}
	SpawnHeldFireDeadline = 0.0;
	SpawnHeldFireWorld.Reset();
	SpawnHeldFireWeapon.Reset();
	bSpawnHeldFireWeaponBound = false;
	// Keep the controller identity to deduplicate restarts of this same pawn.
}

void ATeamArenaCharacter::RetrySpawnHeldFire()
{
#if !UE_SERVER
	UWorld* World = GetWorld();
	AUTPlayerController* PC = SpawnHeldFireController.Get();
	if (SpawnHeldFireDeadline <= 0.0 || FPlatformTime::Seconds() >= SpawnHeldFireDeadline
		|| World == nullptr || World != SpawnHeldFireWorld.Get()
		|| GetNetMode() == NM_DedicatedServer || PC == nullptr
		|| Controller != PC || PC->GetPawn() != this || PC->IsPendingKillPending()
		|| !PC->IsLocalController() || !IsLocallyControlled() || !IsPlayerControlled()
		|| IsDead() || IsPendingKillPending() || World->IsPaused()
		|| (World->DemoNetDriver && World->DemoNetDriver->IsPlaying())
		|| PC->IsMoveInputIgnored() || PC->ShouldShowMouseCursor()
		|| IsFiringDisabled() || TauntCount != 0 || IsFeigningDeath())
	{
		CancelSpawnHeldFire();
		return;
	}
	UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(PC->Player);
	UGameViewportClient* Viewport = LP ? LP->ViewportClient : nullptr;
	AUTGameState* GS = World->GetGameState<AUTGameState>();
	if (LP == nullptr || LP->AreMenusOpen() || LP->IsQuickChatOpen()
		|| Viewport == nullptr || Viewport->IgnoreInput()
		|| Viewport->Viewport == nullptr || !Viewport->Viewport->HasFocus()
		|| (Viewport->ViewportConsole && Viewport->ViewportConsole->ConsoleActive())
		|| (GS && GS->PreventWeaponFire()))
	{
		CancelSpawnHeldFire();
		return;
	}
	// A changed game/spectator state is a cancellation, not a later opportunity
	// to recover an old hold. Only initial possession readiness may wait.
	if (!PC->IsInState(NAME_Playing) && !PC->IsInState(NAME_Inactive))
	{
		CancelSpawnHeldFire();
		return;
	}
	AUTWeapon* CurrentWeapon = GetWeapon();
	if (GetPendingWeapon() != nullptr
		|| (bSpawnHeldFireWeaponBound && SpawnHeldFireWeapon.Get() != CurrentWeapon)
		|| (CurrentWeapon != nullptr && CurrentWeapon->IsUnEquipping()))
	{
		CancelSpawnHeldFire();
		return;
	}
	if (!bSpawnHeldFireWeaponBound && CurrentWeapon != nullptr)
	{
		SpawnHeldFireWeapon = CurrentWeapon;
		bSpawnHeldFireWeaponBound = true;
	}
	if (!PC->IsInState(NAME_Playing) || PC->AcknowledgedPawn != this || PC->GetUTCharacter() != this
		|| CurrentWeapon == nullptr || CurrentWeapon->GetUTOwner() != this
		|| CurrentWeapon->IsPendingKillPending() || CurrentWeapon->GetCurrentState() == nullptr
		|| CurrentWeapon->GetCurrentState()->IsA(UUTWeaponStateInactive::StaticClass()))
	{
		World->GetTimerManager().SetTimer(SpawnHeldFireHandle,
			this, &ATeamArenaCharacter::RetrySpawnHeldFire, 0.01f, false);
		return;
	}
	// Public stock API exposes current held bits only through this verifier.
	// If either mode already belongs to normal input, leave BOTH modes alone:
	// equipping is not IsFiring(), but can already have a valid pending press.
	const bool bNeedsVerification = !IsPendingFire(0) && !IsPendingFire(1)
		&& !PC->HasDeferredFireInputs() && !CurrentWeapon->IsFiring();
	CancelSpawnHeldFire();
	if (bNeedsVerification)
	{
		// Reads the controller's held bits now, so a released button creates no
		// input. The ordinary deferred-input and weapon paths retain all gates.
		PC->ClientVerifyFiringInputs();
	}
#endif
}
