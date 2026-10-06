#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerCharacter.h"
#include "UTPlayerCameraManager.h"
#include "UTLocalPlayer.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformTime.h"

ANCAimTrainerPlayerController::ANCAimTrainerPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
	OnlineStatus = TEXT("Waiting for UT4Stats leaderboard...");
}

void ANCAimTrainerPlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
	Super::ClientRestart_Implementation(NewPawn);
	// PawnClientRestart restores Walking. Mirror the authority's selected lane
	// locally while leaving the movement component ticking.
	// Do not use SetIgnoreMoveInput: stock ApplyDeferredFireInputs also treats
	// that flag as a firing lock, including for the standalone local player.
	ApplyTrainerMovementMode();
}

void ANCAimTrainerPlayerController::ApplyTrainerMovementMode()
{
	if (!IsLocalController()) { return; }
	AUTCharacter* TraineePawn = Cast<AUTCharacter>(GetPawn());
	UNCAimTrainerMovement* Movement = TraineePawn ? Cast<UNCAimTrainerMovement>(TraineePawn->GetCharacterMovement()) : nullptr;
	if (!Movement) { return; }
	bIsHoldingFloorSlide = false;
	// bIsCrouched only replicates to simulated proxies, so the owning client
	// must restore its capsule too when the server starts another fixed preset.
	Movement->ResetTrainerMovement(TrainerProgress.bMovementPractice);
}

void ANCAimTrainerPlayerController::MoveForward(float /*Value*/)
{
	// Keep target distances fixed, including when the player turns their view.
	MovementForwardAxis = 0.f;
}

void ANCAimTrainerPlayerController::MoveRight(float Value)
{
	MovementStrafeAxis = Value;
	AUTCharacter* TraineePawn = Cast<AUTCharacter>(GetPawn());
	if (TrainerProgress.bMovementPractice && TraineePawn && Value != 0.f)
	{
		TraineePawn->AddMovementInput(FVector(0.f, 1.f, 0.f), Value);
	}
}

void ANCAimTrainerPlayerController::Jump()
{
	if (TrainerProgress.bMovementPractice) { Super::Jump(); }
}

void ANCAimTrainerPlayerController::Crouch()
{
	if (TrainerProgress.bMovementPractice) { Super::Crouch(); }
}

void ANCAimTrainerPlayerController::ToggleCrouch()
{
	if (TrainerProgress.bMovementPractice) { Super::ToggleCrouch(); }
}

void ANCAimTrainerPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(ANCAimTrainerPlayerController, TrainerProgress, COND_OwnerOnly);
}

bool ANCAimTrainerPlayerController::IsTrainerMenuVisible() const
{
	return TrainerProgress.Phase == 0 || TrainerProgress.Phase == 3;
}

bool ANCAimTrainerPlayerController::HasTrainerInputFocus() const
{
#if !UE_SERVER
	UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(Player);
	if (LP && (LP->AreMenusOpen() || LP->IsQuickChatOpen())) { return false; }
	if (LP && LP->ViewportClient && LP->ViewportClient->ViewportConsole
		&& LP->ViewportClient->ViewportConsole->ConsoleActive()) { return false; }
#endif
	return IsLocalController();
}

bool ANCAimTrainerPlayerController::InputKey(FKey Key, EInputEvent EventType, float AmountDepressed, bool bGamepad)
{
	if (HasTrainerInputFocus())
	{
		if (Key == EKeys::F6)
		{
			if (EventType == IE_Pressed) { ReturnToTrainerMenu(); }
			return true;
		}
		if (IsTrainerMenuVisible())
		{
			if (Key == EKeys::M)
			{
				if (EventType == IE_Pressed) { ToggleTrainerMovementPractice(); }
				return true;
			}
			int32 Scenario = INDEX_NONE;
			if (Key == EKeys::One || Key == EKeys::NumPadOne) { Scenario = 0; }
			if (Key == EKeys::Two || Key == EKeys::NumPadTwo) { Scenario = 1; }
			if (Key == EKeys::Three || Key == EKeys::NumPadThree) { Scenario = 2; }
			if (Scenario != INDEX_NONE)
			{
				if (EventType == IE_Pressed) { SelectTrainerScenario(uint8(Scenario)); }
				return true;
			}
			if (Key == EKeys::Enter)
			{
				if (EventType == IE_Pressed) { StartTrainerRun(); }
				return true;
			}
		}
	}
	return Super::InputKey(Key, EventType, AmountDepressed, bGamepad);
}

void ANCAimTrainerPlayerController::OnFire()
{
	if (TrainerProgress.Phase == 2 && TrainerProgress.Scenario != 0) { Super::OnFire(); }
}

void ANCAimTrainerPlayerController::OnAltFire()
{
	if (TrainerProgress.Phase == 2 && TrainerProgress.Scenario != 0) { Super::OnAltFire(); }
}

void ANCAimTrainerPlayerController::SelectTrainerScenario(uint8 Scenario)
{
	if (Scenario < 3 && IsTrainerMenuVisible()) { ServerTrainerSelectScenario(Scenario); }
}

void ANCAimTrainerPlayerController::StartTrainerRun()
{
	if (IsTrainerMenuVisible()) { ServerTrainerStart(); }
}

void ANCAimTrainerPlayerController::ReturnToTrainerMenu()
{
	ServerTrainerAbort();
}

void ANCAimTrainerPlayerController::ToggleTrainerMovementPractice()
{
	if (IsTrainerMenuVisible()) { ServerTrainerSetMovementPractice(!TrainerProgress.bMovementPractice); }
}

bool ANCAimTrainerPlayerController::AdmitTrainerRequest(uint8 Action)
{
	const double Now = FPlatformTime::Seconds();
	if (Action >= 4 || Now < NextTrainerRequestTime[Action]) { return false; }
	NextTrainerRequestTime[Action] = Now + 0.15;
	return true;
}

bool ANCAimTrainerPlayerController::ServerTrainerSelectScenario_Validate(uint8 Scenario) { return Scenario < 3; }
void ANCAimTrainerPlayerController::ServerTrainerSelectScenario_Implementation(uint8 Scenario)
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(0)) { Game->SelectScenario(this, Scenario); }
}

bool ANCAimTrainerPlayerController::ServerTrainerStart_Validate() { return true; }
void ANCAimTrainerPlayerController::ServerTrainerStart_Implementation()
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(1)) { Game->StartTraining(this); }
}

bool ANCAimTrainerPlayerController::ServerTrainerAbort_Validate() { return true; }
void ANCAimTrainerPlayerController::ServerTrainerAbort_Implementation()
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(2)) { Game->AbortTraining(this); }
}

bool ANCAimTrainerPlayerController::ServerTrainerSetMovementPractice_Validate(bool bEnabled) { return true; }
void ANCAimTrainerPlayerController::ServerTrainerSetMovementPractice_Implementation(bool bEnabled)
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(3)) { Game->SetMovementPractice(this, bEnabled); }
}

void ANCAimTrainerPlayerController::SetTrainerProgress(const FNCAimTrainerProgress& Progress)
{
	if (Role != ROLE_Authority) { return; }
	const bool bPhaseChanged = TrainerProgress.Phase != Progress.Phase;
	TrainerProgress = Progress;
	if (bPhaseChanged) { ForceNetUpdate(); }
	if (IsLocalController()) { OnRep_TrainerProgress(); }
}

void ANCAimTrainerPlayerController::OnRep_TrainerProgress()
{
	if (bLastPresentedMovementPractice != TrainerProgress.bMovementPractice
		|| (TrainerProgress.Phase == 1 && LastPresentedPhase != 1))
	{
		// A regular score update must never turn an ongoing jump back into Walking.
		ApplyTrainerMovementMode();
		bLastPresentedMovementPractice = TrainerProgress.bMovementPractice;
	}
#if !UE_SERVER
	// UT owns input-mode and cursor transitions; do not mutate profile/bindings.
	UpdateInputMode();
#endif
	if (TrainerProgress.Phase != 2 && LastPresentedPhase != TrainerProgress.Phase)
	{
		// A run ending while fire is held must not carry that hold into a retry.
		Super::OnStopFire();
		Super::OnStopAltFire();
	}
	LastPresentedPhase = TrainerProgress.Phase;
}

void ANCAimTrainerPlayerController::SetTrainerLeaderboard(const TArray<FNCAimTrainerLeaderboardRow>& Rows)
{
	if (Role == ROLE_Authority) { ClientTrainerLeaderboard(Rows); }
}

void ANCAimTrainerPlayerController::SetTrainerOnlineStatus(const FString& Status)
{
	if (Role == ROLE_Authority) { ClientTrainerOnlineStatus(Status); }
}

void ANCAimTrainerPlayerController::ClientTrainerLeaderboard_Implementation(const TArray<FNCAimTrainerLeaderboardRow>& Rows)
{
	Leaderboard = Rows;
	if (Leaderboard.Num() > 10) { Leaderboard.SetNum(10); }
}

void ANCAimTrainerPlayerController::ClientTrainerOnlineStatus_Implementation(const FString& Status)
{
	OnlineStatus = Status.Left(160);
}
