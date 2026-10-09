#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerOnline.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLayout.h"
#include "NCAimTrainerCharacter.h"
#include "NCAimTrainerCountdownMessage.h"
#include "UTAnnouncer.h"
#include "UTPlayerCameraManager.h"
#include "UTLocalPlayer.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformTime.h"
#include "ClientHitsounds.h"
#include "EngineUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

ANCAimTrainerPlayerController::ANCAimTrainerPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
}

void ANCAimTrainerPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bLeaderboardEnded = true;
	Super::EndPlay(EndPlayReason);
}

void ANCAimTrainerPlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
	Super::ClientRestart_Implementation(NewPawn);
	bTrackingPrimaryHeld = bTrackingAltHeld = false;
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
	Movement->ResetTrainerMovement(TrainerProgress.bMovementPractice,
		NCAimTrainerLayout::PracticeLaneX(TrainerProgress.Scenario));
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
	// Use retail exports rather than UTLocalPlayer virtual slots/member offsets.
	if (LP && (LP->UUTLocalPlayer::AreMenusOpen() || LP->GetQuickChatWidget().IsValid())) { return false; }
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
			if (Key == EKeys::Four || Key == EKeys::NumPadFour) { Scenario = 3; }
			if (Key == EKeys::Five || Key == EKeys::NumPadFive) { Scenario = 4; }
			if (Key == EKeys::Six || Key == EKeys::NumPadSix) { Scenario = 5; }
			if (Key == EKeys::Seven || Key == EKeys::NumPadSeven) { Scenario = 6; }
			if (Key == EKeys::Eight || Key == EKeys::NumPadEight) { Scenario = 7; }
			if (Key == EKeys::Nine || Key == EKeys::NumPadNine) { Scenario = 8; }
			if (Key == EKeys::Zero || Key == EKeys::NumPadZero) { Scenario = 9; }
			if (Key == EKeys::Hyphen || Key == EKeys::Subtract) { Scenario = 10; }
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
	if (TrainerProgress.Phase != 2) { return; }
	if (NCAimTrainerScenarioPolicy::IsTrackingScenario(TrainerProgress.Scenario)) { SetTrackingFireHeld(true, true); }
	else { Super::OnFire(); }
}

void ANCAimTrainerPlayerController::OnAltFire()
{
	if (TrainerProgress.Phase != 2) { return; }
	if (NCAimTrainerScenarioPolicy::IsTrackingScenario(TrainerProgress.Scenario)) { SetTrackingFireHeld(false, true); }
	else { Super::OnAltFire(); }
}

void ANCAimTrainerPlayerController::OnStopFire()
{
	if (NCAimTrainerScenarioPolicy::IsTrackingScenario(TrainerProgress.Scenario)) { SetTrackingFireHeld(true, false); }
	else { Super::OnStopFire(); }
}

void ANCAimTrainerPlayerController::OnStopAltFire()
{
	if (NCAimTrainerScenarioPolicy::IsTrackingScenario(TrainerProgress.Scenario)) { SetTrackingFireHeld(false, false); }
	else { Super::OnStopAltFire(); }
}

void ANCAimTrainerPlayerController::SetTrackingFireHeld(bool bPrimary, bool bHeld)
{
	const bool bWasHeld = bTrackingPrimaryHeld || bTrackingAltHeld;
	if (bPrimary) { bTrackingPrimaryHeld = bHeld; }
	else { bTrackingAltHeld = bHeld; }
	const bool bNowHeld = bTrackingPrimaryHeld || bTrackingAltHeld;
	if (bNowHeld == bWasHeld) { return; }
	// Keep UT's deferred input and real Link beam state. Primary never reaches
	// the plasma/pull path, and a second held button cannot stop or restart it.
	if (bNowHeld) { Super::OnAltFire(); }
	else { Super::OnStopAltFire(); }
}

void ANCAimTrainerPlayerController::SelectTrainerScenario(uint8 Scenario)
{
	if (NCAimTrainerScenarioPolicy::IsValidScenario(Scenario) && IsTrainerMenuVisible()) { ServerTrainerSelectScenario(Scenario, PrefersTrainerLightningGun()); }
}

void ANCAimTrainerPlayerController::StartTrainerRun()
{
	if (IsTrainerMenuVisible()) { ServerTrainerStart(PrefersTrainerLightningGun()); }
}

bool ANCAimTrainerPlayerController::PrefersTrainerLightningGun() const
{
	// Read only on the owning player. Dedicated servers must receive this
	// preference with the menu/start request, never use their own Mod.ini.
	if (!IsLocalController() || !GConfig) { return false; }
	FString Choice;
	GConfig->GetString(TEXT("WeaponSkinsPlus"), TEXT("HitscanChoice"), Choice,
		FPaths::GeneratedConfigDir() + TEXT("Mod.ini"));
	return Choice.Equals(TEXT("LG"), ESearchCase::IgnoreCase);
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

bool ANCAimTrainerPlayerController::ServerTrainerSelectScenario_Validate(uint8 Scenario, bool bUseLightningGun) { return NCAimTrainerScenarioPolicy::IsValidScenario(Scenario); }
void ANCAimTrainerPlayerController::ServerTrainerSelectScenario_Implementation(uint8 Scenario, bool bUseLightningGun)
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(0)) { Game->SelectScenario(this, Scenario, bUseLightningGun); }
}

bool ANCAimTrainerPlayerController::ServerTrainerStart_Validate(bool bUseLightningGun) { return true; }
void ANCAimTrainerPlayerController::ServerTrainerStart_Implementation(bool bUseLightningGun)
{
	ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (Game && AdmitTrainerRequest(1)) { Game->StartTraining(this, bUseLightningGun); }
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
	UpdateTrainerCountdownAudio();
	if (bLastPresentedMovementPractice != TrainerProgress.bMovementPractice
		|| LastPresentedScenario != TrainerProgress.Scenario
		|| (TrainerProgress.Phase == 1 && LastPresentedPhase != 1))
	{
		// A regular score update must never turn an ongoing jump back into Walking.
		ApplyTrainerMovementMode();
		bLastPresentedMovementPractice = TrainerProgress.bMovementPractice;
		LastPresentedScenario = TrainerProgress.Scenario;
	}
#if !UE_SERVER
	if (IsLocalController() && LastPresentedPhase == 255 && TrainerProgress.Phase == 0)
	{
		// Prepare cue assets in the picker, not on the first successful shot.
		AClientHitsounds::EnsureCatalog();
	}
	// UT owns input-mode and cursor transitions; do not mutate profile/bindings.
	UpdateInputMode();
#endif
	if (TrainerProgress.Phase != 2 && LastPresentedPhase != TrainerProgress.Phase)
	{
		// A run ending while fire is held must not carry that hold into a retry.
		bTrackingPrimaryHeld = bTrackingAltHeld = false;
		Super::OnStopFire();
		Super::OnStopAltFire();
	}
	LastPresentedPhase = TrainerProgress.Phase;
}

void ANCAimTrainerPlayerController::UpdateTrainerCountdownAudio()
{
#if !UE_SERVER
	if (!IsLocalController()) { return; }
	const float Seconds = TrainerProgress.RemainingSeconds;
	if (TrainerProgress.Phase != 1 || LastPresentedPhase != 1 || Seconds == 3.f)
	{
		LastAnnouncedCountdown = 4;
	}
	// Standalone account verification holds the display at exactly three.
	// Speak only once the authority's countdown moves; never run a separate clock.
	if (TrainerProgress.Phase != 1 || !Announcer || !FMath::IsFinite(Seconds)
		|| Seconds <= 0.f || Seconds >= 3.f) { return; }
	const int32 Count = FMath::CeilToInt(Seconds);
	if (Count >= LastAnnouncedCountdown) { return; }
	LastAnnouncedCountdown = Count;
	// The existing local announcer supplies the selected NCP pack and volume.
	// Only the current number plays if a replicated update skips a boundary.
	Announcer->PlayAnnouncement(UNCAimTrainerCountdownMessage::StaticClass(), Count, nullptr, nullptr, this);
#endif
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
	// Legacy rows-only entry point; the menu now owns public reads. Rows without
	// a scenario/source cannot safely replace an asynchronous browser selection.
	(void)Rows;
}

bool ANCAimTrainerPlayerController::IsTrainerLeaderboardLocal() const
{
	return bLeaderboardSourceSelected ? bLeaderboardLocal : GetNetMode() == NM_Standalone;
}

int32 ANCAimTrainerPlayerController::SelectedLeaderboardKey() const
{
	const int32 Count = NCAimTrainerScenarioId::ScenarioCount;
	return FMath::Clamp(int32(TrainerProgress.Scenario), 0, Count - 1) + (IsTrainerLeaderboardLocal() ? Count : 0)
		+ (TrainerProgress.bMovementPractice ? Count * 2 : 0);
}

const TArray<FNCAimTrainerLeaderboardRow>& ANCAimTrainerPlayerController::GetTrainerLeaderboard() const
{
	return LeaderboardCache[SelectedLeaderboardKey()];
}

FString ANCAimTrainerPlayerController::GetTrainerLeaderboardStatus() const
{
	const int32 Key = SelectedLeaderboardKey();
	if (LeaderboardInFlight[Key])
	{
		return LeaderboardLoaded[Key] ? TEXT("Refreshing UT4Stats scores...") : TEXT("Loading UT4Stats scores...");
	}
	if (LeaderboardFailed[Key])
	{
		return LeaderboardLoaded[Key] ? TEXT("UT4Stats unavailable. Showing previously loaded scores.")
			: TEXT("UT4Stats leaderboard unavailable. Practice is still available.");
	}
	if (!LeaderboardLoaded[Key]) { return TEXT("Loading UT4Stats scores..."); }
	return LeaderboardCache[Key].Num() == 0 ? TEXT("No scores for this scenario yet.")
		: TEXT("Best run per player for this scenario and movement setting.");
}

void ANCAimTrainerPlayerController::SelectTrainerLeaderboardSource(bool bLocal)
{
	if (!IsLocalController() || !IsTrainerMenuVisible() || bLeaderboardEnded) { return; }
	bLeaderboardLocal = bLocal;
	bLeaderboardSourceSelected = true;
	RefreshTrainerLeaderboard();
}

void ANCAimTrainerPlayerController::RefreshTrainerLeaderboard()
{
	if (!IsLocalController() || !GetWorld() || !IsTrainerMenuVisible() || bLeaderboardEnded) { return; }
	const int32 Key = SelectedLeaderboardKey();
	const double Now = FPlatformTime::Seconds();
	if (LeaderboardInFlight[Key] || Now < NextLeaderboardFetch[Key]) { return; }
	LeaderboardInFlight[Key] = true;
	const uint32 Generation = LeaderboardGeneration[Key];
	TWeakObjectPtr<ANCAimTrainerPlayerController> WeakPC(this);
	TWeakObjectPtr<UWorld> WeakWorld(GetWorld());
	const int32 Count = NCAimTrainerScenarioId::ScenarioCount;
	FNCAimTrainerOnline::Fetch(GetWorld(), Key % Count,
		[WeakPC, WeakWorld, Key, Generation](bool bSuccess, const TArray<FNCAimTrainerLeaderboardRow>& Rows)
	{
		ANCAimTrainerPlayerController* PC = WeakPC.Get();
		if (!PC || !WeakWorld.IsValid() || PC->GetWorld() != WeakWorld.Get() || PC->bLeaderboardEnded) { return; }
		PC->LeaderboardInFlight[Key] = false;
		// A score accepted during this request invalidates its response. The next
		// menu poll refetches, including when the user later returns to this source.
		if (PC->LeaderboardGeneration[Key] != Generation) { return; }
		PC->NextLeaderboardFetch[Key] = FPlatformTime::Seconds() + (bSuccess ? 60.0 : 10.0);
		PC->LeaderboardFailed[Key] = !bSuccess;
		if (bSuccess)
		{
			PC->LeaderboardCache[Key] = Rows;
			if (PC->LeaderboardCache[Key].Num() > 10) { PC->LeaderboardCache[Key].SetNum(10); }
			PC->LeaderboardLoaded[Key] = true;
		}
	}, (Key % (Count * 2)) >= Count, Key >= Count * 2);
}

void ANCAimTrainerPlayerController::NotifyTrainerLeaderboardSubmission(uint8 Scenario, bool bLocal, bool bMovementPractice)
{
	if (Role == ROLE_Authority && NCAimTrainerScenarioPolicy::IsValidScenario(Scenario)) { ClientTrainerLeaderboardSubmitted(Scenario, bLocal, bMovementPractice); }
}

void ANCAimTrainerPlayerController::ClientTrainerLeaderboardSubmitted_Implementation(uint8 Scenario, bool bLocal, bool bMovementPractice)
{
	if (!IsLocalController() || !NCAimTrainerScenarioPolicy::IsValidScenario(Scenario) || bLeaderboardEnded) { return; }
	const int32 Count = NCAimTrainerScenarioId::ScenarioCount;
	const int32 Key = int32(Scenario) + (bLocal ? Count : 0) + (bMovementPractice ? Count * 2 : 0);
	++LeaderboardGeneration[Key];
	NextLeaderboardFetch[Key] = 0.0;
	LeaderboardFailed[Key] = false;
	if (Key == SelectedLeaderboardKey()) { RefreshTrainerLeaderboard(); }
}

void ANCAimTrainerPlayerController::ClientTrainerOnlineStatus_Implementation(const FString& Status)
{
	OnlineStatus = Status.Left(160);
}

void ANCAimTrainerPlayerController::NotifyTrainerHit(float Damage)
{
	if (Role != ROLE_Authority || !FMath::IsFinite(Damage) || Damage <= 0.f) { return; }
	ClientTrainerConfirmedHit(FMath::RoundToInt(FMath::Clamp(Damage, 1.f, 10000.f)));
}

void ANCAimTrainerPlayerController::ClientTrainerConfirmedHit_Implementation(int32 Damage)
{
#if !UE_SERVER
	if (!IsLocalController() || !GetWorld() || Damage <= 0 || Damage > 10000) { return; }
	// Targets bypass normal character damage to stay alive between appearances.
	// Reuse the existing NCP playback and preference path after score acceptance.
	TActorIterator<AClientHitsounds> It(GetWorld());
	if (It)
	{
		if (!It->ShouldSuppressServerHitsound(Damage, false)) { It->PlayHitsound(Damage, false); }
		return;
	}
	// No hitsounds mutator is needed in the trainer. This static playback entry
	// uses the same cue, style, pitch and volume as the player's NCP menu preset.
	AClientHitsounds::PlayPreview(this, AClientHitsounds::LoadConfigFromIni(), false, Damage);
#endif
}
