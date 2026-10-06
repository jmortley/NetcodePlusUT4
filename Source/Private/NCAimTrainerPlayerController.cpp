#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerGame.h"
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

void ANCAimTrainerPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController() && !IsMoveInputIgnored()) { SetIgnoreMoveInput(true); }
}

void ANCAimTrainerPlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
	Super::ClientRestart_Implementation(NewPawn);
	// The stock restart resets ignore-input flags after this controller's
	// BeginPlay, and PawnClientRestart restores Walking. Mirror the authority's
	// fixed lane locally without disabling movement replication or mouse-look.
	if (IsLocalController())
	{
		if (!IsMoveInputIgnored()) { SetIgnoreMoveInput(true); }
		AUTCharacter* Character = Cast<AUTCharacter>(GetPawn());
		if (Character && Character->GetCharacterMovement())
		{
			Character->GetCharacterMovement()->DisableMovement();
		}
	}
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

bool ANCAimTrainerPlayerController::AdmitTrainerRequest(uint8 Action)
{
	const double Now = FPlatformTime::Seconds();
	if (Action >= 3 || Now < NextTrainerRequestTime[Action]) { return false; }
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
