#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "ClutchScoreboard.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTWeapon.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"
#if !UE_SERVER
#include "Interfaces/IImageWrapper.h"
#include "Interfaces/IImageWrapperModule.h"
#endif
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogClutchHUD, Log, All);


namespace
{
	static FString GetPhaseLabel(EClutchRoundPhase Phase)
	{
		switch (Phase)
		{
		case EClutchRoundPhase::Waiting: return TEXT("WAITING");
		case EClutchRoundPhase::OrderSelection: return TEXT("CHOOSE ATTACK ORDER");
		case EClutchRoundPhase::Intermission: return TEXT("ROUND SETUP");
		case EClutchRoundPhase::Combat: return TEXT("POLE LOCKED");
		case EClutchRoundPhase::Capture: return TEXT("POLE ACTIVE");
		case EClutchRoundPhase::RoundEnd: return TEXT("ROUND OVER");
		case EClutchRoundPhase::MatchEnd: return TEXT("MATCH OVER");
		default: return TEXT("CLUTCH");
		}
	}

	static void DrawCenteredCanvasText(UCanvas* Canvas, UFont* Font,
		const FString& Text, float CenterX, float Y, float Scale, const FColor& Color)
	{
		if (!Canvas || !Font)
		{
			return;
		}
		float Width = 0.0f;
		float Height = 0.0f;
		Canvas->TextSize(Font, Text, Width, Height, Scale, Scale);
		Canvas->DrawColor = Color;
		Canvas->DrawText(Font, Text, CenterX - Width * 0.5f, Y, Scale, Scale);
	}

	static UTexture2D* LoadTextureWithFallback(
		const TCHAR* PrimaryPath, const TCHAR* FallbackPath = nullptr)
	{
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, PrimaryPath);
		return Texture || !FallbackPath
			? Texture
			: LoadObject<UTexture2D>(nullptr, FallbackPath);
	}

	static UTexture2D* LoadPluginPngTexture(const TCHAR* RelativePath)
	{
#if !UE_SERVER
		const FString ResourcesDir = FNCPlusHUDLayout::PluginResourcesDir();
		const FString FilePath = FPaths::Combine(
			*ResourcesDir,
			TEXT("ClutchHUD"),
			RelativePath);
		TArray<uint8> CompressedData;
		if (!FFileHelper::LoadFileToArray(CompressedData, *FilePath)
			|| CompressedData.Num() == 0)
		{
			return nullptr;
		}

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		const TArray<uint8>* RawData = nullptr;
		if (!ImageWrapper.IsValid()
			|| !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num())
			|| !ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData)
			|| !RawData)
		{
			return nullptr;
		}

		const int32 Width = ImageWrapper->GetWidth();
		const int32 Height = ImageWrapper->GetHeight();
		if (Width <= 0 || Height <= 0 || RawData->Num() != Width * Height * 4)
		{
			return nullptr;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(
			Width, Height, PF_B8G8R8A8);
		if (!Texture || !Texture->PlatformData || Texture->PlatformData->Mips.Num() == 0)
		{
			return nullptr;
		}

		Texture->SRGB = true;
		Texture->NeverStream = true;
		FTexture2DMipMap& Mip = Texture->PlatformData->Mips[0];
		void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TextureData, RawData->GetData(), RawData->Num());
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
#else
		return nullptr;
#endif
	}

	static void DrawTextureTile(UCanvas* Canvas, UTexture2D* Texture,
		float X, float Y, float Width, float Height, const FLinearColor& Color)
	{
		if (!Canvas || !Texture)
		{
			return;
		}
		Canvas->SetLinearDrawColor(Color);
		Canvas->DrawTile(Texture, X, Y, Width, Height,
			0.0f, 0.0f,
			static_cast<float>(Texture->GetSizeX()),
			static_cast<float>(Texture->GetSizeY()),
			BLEND_Translucent);
	}

	static void DrawSolidTile(UCanvas* Canvas, float X, float Y,
		float Width, float Height, const FLinearColor& Color)
	{
		Canvas->SetLinearDrawColor(Color);
		Canvas->DrawTile(Canvas->DefaultTexture, X, Y, Width, Height,
			0.0f, 0.0f, 1.0f, 1.0f, BLEND_Translucent);
	}

	static FString BuildAttackOrderRosterKey(
		const AClutchRoundState* State, uint8 TeamIndex)
	{
		if (!State || TeamIndex > 1)
		{
			return FString();
		}
		TArray<int32> Slots;
		for (const FClutchRosterEntry& Entry : State->Roster)
		{
			// The draft only becomes stale when team membership changes. Do not key it
			// to AttackOrderIndex (the server may legitimately rewrite order metadata)
			// or PlayerState resolution (actor references can arrive a frame later).
			if (Entry.TeamIndex == TeamIndex
				&& Entry.PlayerStatus != EClutchStatus::Disconnected
				&& Entry.RosterSlot != AClutchRoundState::UnassignedSlot)
			{
				Slots.AddUnique(static_cast<int32>(Entry.RosterSlot));
			}
		}
		Slots.Sort();
		FString Key = FString::Printf(TEXT("%d:"), TeamIndex);
		for (int32 Slot : Slots)
		{
			Key += FString::Printf(TEXT("%d,"), Slot);
		}
		return Key;
	}

	static const FClutchRosterEntry* FindTeamRosterSlot(
		const AClutchRoundState* State, uint8 TeamIndex, int32 RosterSlot)
	{
		if (State)
		{
			for (const FClutchRosterEntry& Entry : State->Roster)
			{
				if (Entry.PlayerState && Entry.TeamIndex == TeamIndex
					&& Entry.RosterSlot == RosterSlot)
				{
					return &Entry;
				}
			}
		}
		return nullptr;
	}
}


AClutchHUD::AClutchHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Clutch draws a compact portrait strip immediately around the score boxes.
	// Disable Wipeout's wide 40%/60% anchors and its unrelated KDA panel.
	bShouldDrawPortraits = false;
	HudWidgetClasses.Remove(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ClutchScoreboard"));
	HudWidgetClasses.AddUnique(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_ReadyUp"));

	bTriedLoadRecoveredHUDTextures = false;
	bAttackOrderInputActive = false;
	bSavedShowMouseCursor = false;
	bSavedEnableClickEvents = false;
	bSavedEnableMouseOverEvents = false;
	bAttackOrderSubmitted = false;
	bEliminatedCameraForced = false;
	bSavedSpectateBehindView = false;
	PoleAudioRoundNumber = INDEX_NONE;
	LastPoleCountdownSecond = INDEX_NONE;
	bPlayedPoleUnlockedAudio = false;
	ObservedArmorRoundNumber = INDEX_NONE;
	LastObservedAttackerHits = INDEX_NONE;
	AttackerHitFlashEndTime = -1000.0f;
	ObservedArmorAttacker.Reset();
	AttackOrderSubmitTime = -1000.0f;
	DraftAttackOrderTeam = AClutchRoundState::NoTeam;
	LegacyCircleTexture = nullptr;
	LegacyInnerCircleTexture = nullptr;
	LegacyAttackingTexture = nullptr;
	LegacyLeftGadgetTexture = nullptr;
	LegacyRightGadgetTexture = nullptr;
	LegacyShieldTexture = nullptr;
	LegacyInstaTexture = nullptr;
	LegacyRocketTexture = nullptr;
	LegacyAttackRailBackTexture = nullptr;
	LegacyAttackRailFrontTexture = nullptr;
	LegacyAttackProgressTexture = nullptr;
	LegacyDefendRailBackTexture = nullptr;
	LegacyDefendRailFrontTexture = nullptr;
	LegacyDefendProgressTexture = nullptr;
	LegacyTopFrameTexture = nullptr;
	PoleUnlockedSound = nullptr;
	PoleCountdownSounds.SetNumZeroed(6);

	if (!IsRunningDedicatedServer())
	{
		// Use the same stock countdown waves as NetcodePlus automatic pause. They
		// bypass the announcer queue so combat announcements cannot swallow the
		// short, objective-critical unlock warning.
		static ConstructorHelpers::FObjectFinder<USoundBase> CD1(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD1.A_AnnouncerF_CD1'"));
		static ConstructorHelpers::FObjectFinder<USoundBase> CD2(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD2.A_AnnouncerF_CD2'"));
		static ConstructorHelpers::FObjectFinder<USoundBase> CD3(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD3.A_AnnouncerF_CD3'"));
		static ConstructorHelpers::FObjectFinder<USoundBase> CD4(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD4.A_AnnouncerF_CD4'"));
		static ConstructorHelpers::FObjectFinder<USoundBase> CD5(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD5.A_AnnouncerF_CD5'"));
		static ConstructorHelpers::FObjectFinder<USoundBase> Capture(
			TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_Capture.A_AnnouncerF_Capture'"));
		PoleCountdownSounds[1] = CD1.Object;
		PoleCountdownSounds[2] = CD2.Object;
		PoleCountdownSounds[3] = CD3.Object;
		PoleCountdownSounds[4] = CD4.Object;
		PoleCountdownSounds[5] = CD5.Object;
		PoleUnlockedSound = Capture.Object;
	}
}


void AClutchHUD::BeginPlay()
{
	Super::BeginPlay();

	// Keep synchronous package lookup / PNG decode / texture upload out of the
	// first HUD Draw. The guarded calls in the draw paths remain as fallbacks.
	if (!IsRunningDedicatedServer())
	{
		LoadRecoveredHUDTextures();
	}
}


void AClutchHUD::DrawHUD()
{
	AClutchRoundState* State = ResolveClutchState();
	UpdatePoleAudio(State);
	UpdateAttackerArmorFeedback(State);
	const bool bOrderSelection = State
		&& State->Phase == EClutchRoundPhase::OrderSelection;
	if (bOrderSelection && bShowScoresWhileDead)
	{
		// ClearRoundPawns can leave the stock two-second death-scoreboard timer armed.
		// If it fires while the modal picker is open, ScoreboardIsUp() would otherwise
		// hide the picker and disable its click input for the rest of the deadline.
		bShowScoresWhileDead = false;
		UE_LOG(LogClutchHUD, Log,
			TEXT("Suppressed automatic death scoreboard during attack-order selection"));
	}

	// AWipeoutHUD supplies the established NetcodePlus widget stack and calls
	// our virtual DrawTeamScoreBar. Its wide portraits are disabled above.
	Super::DrawHUD();

	const bool bRenderCustomHUD = bShowUTHUD && UTPlayerOwner
		&& (bShowHUD || !UTPlayerOwner->bCinematicMode);
	UpdateEliminatedCameraRestriction(State);
	const bool bScoreboardUp = ScoreboardIsUp();
	const bool bPanelVisible = bRenderCustomHUD && Canvas && SmallFont
		&& bOrderSelection;
	UpdateAttackOrderInput(State, bPanelVisible);
	if (!bRenderCustomHUD || !Canvas || !SmallFont
		|| (bScoreboardUp && !bOrderSelection))
	{
		return;
	}

	if (State)
	{
		DrawClutchPortraits(State);
		if (State->Phase == EClutchRoundPhase::OrderSelection)
		{
			DrawAttackOrderPanel(State);
			return;
		}
		DrawDefenderAttackerPanel(State);
		DrawCapturePanel(State);
		DrawRolePanel(State);
	}
}


void AClutchHUD::UpdatePoleAudio(AClutchRoundState* State)
{
	if (!State || !GetWorld() || !State->IsGameplayPhase())
	{
		LastPoleCountdownSecond = INDEX_NONE;
		return;
	}

	if (PoleAudioRoundNumber != State->RoundNumber)
	{
		PoleAudioRoundNumber = State->RoundNumber;
		LastPoleCountdownSecond = INDEX_NONE;
		bPlayedPoleUnlockedAudio = false;
	}

	if (State->Phase == EClutchRoundPhase::Combat)
	{
		AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
		const float Now = GameState
			? GameState->GetServerWorldTimeSeconds()
			: GetWorld()->GetTimeSeconds();
		const int32 Remaining = FMath::Max(0,
			FMath::CeilToInt(State->PoleUnlockServerTime - Now));
		if (Remaining >= 1 && Remaining <= 5
			&& Remaining != LastPoleCountdownSecond
			&& PoleCountdownSounds.IsValidIndex(Remaining)
			&& PoleCountdownSounds[Remaining])
		{
			UGameplayStatics::PlaySound2D(GetWorld(), PoleCountdownSounds[Remaining]);
		}
		LastPoleCountdownSecond = Remaining;
		return;
	}

	if (State->Phase == EClutchRoundPhase::Capture && !bPlayedPoleUnlockedAudio)
	{
		bPlayedPoleUnlockedAudio = true;
		if (PoleUnlockedSound)
		{
			UGameplayStatics::PlaySound2D(GetWorld(), PoleUnlockedSound);
		}
	}
}


void AClutchHUD::UpdateAttackerArmorFeedback(AClutchRoundState* State)
{
	if (!State || !GetWorld() || !State->IsGameplayPhase() || !State->ActiveAttacker)
	{
		ObservedArmorAttacker.Reset();
		ObservedArmorRoundNumber = INDEX_NONE;
		LastObservedAttackerHits = INDEX_NONE;
		return;
	}

	const FClutchRosterEntry* AttackerEntry = State->FindEntry(State->ActiveAttacker);
	if (!AttackerEntry)
	{
		return;
	}

	const int32 HitsTaken = static_cast<int32>(AttackerEntry->HitsTaken);
	if (ObservedArmorAttacker.Get() != State->ActiveAttacker
		|| ObservedArmorRoundNumber != State->RoundNumber
		|| LastObservedAttackerHits == INDEX_NONE)
	{
		ObservedArmorAttacker = State->ActiveAttacker;
		ObservedArmorRoundNumber = State->RoundNumber;
		LastObservedAttackerHits = HitsTaken;
		AttackerHitFlashEndTime = -1000.0f;
		return;
	}

	if (HitsTaken > LastObservedAttackerHits)
	{
		AttackerHitFlashEndTime = GetWorld()->GetTimeSeconds() + 0.75f;
	}
	LastObservedAttackerHits = HitsTaken;
}


EInputMode::Type AClutchHUD::GetInputMode_Implementation() const
{
	// While this player's attack-order picker is active, the mouse cursor must stay free
	// and visible for its click hitboxes. The base AWipeoutHUD forces EIM_GameOnly for the
	// entire InProgress match (capturing the mouse and hiding the cursor, which
	// UTBasePlayerController::UpdateInputMode re-applies every tick and clobbers any
	// cursor flag the HUD sets), so override to GameAndUI for the picker window.
	// bAttackOrderInputActive is the single flag that mirrors the full enable predicate
	// (local selector, order unlocked, more than one eligible slot, panel visible, not yet
	// submitted), so it flips back off the moment the order locks/submits and the base
	// GameOnly capture resumes.
	if (bAttackOrderInputActive)
	{
		return EInputMode::EIM_GameAndUI;
	}
	return Super::GetInputMode_Implementation();
}


void AClutchHUD::UpdateAttackOrderInput(
	AClutchRoundState* State, bool bPanelVisible)
{
	AUTPlayerState* OwnState = UTPlayerOwner
		? Cast<AUTPlayerState>(UTPlayerOwner->PlayerState)
		: nullptr;
	const FClutchRosterEntry* OwnEntry = OwnState && State
		? State->FindEntry(OwnState)
		: nullptr;
	if (State && OwnEntry && OwnEntry->TeamIndex <= 1)
	{
		const FString RosterKey = BuildAttackOrderRosterKey(State, OwnEntry->TeamIndex);
		if (DraftAttackOrderTeam != OwnEntry->TeamIndex
			|| DraftAttackOrderRosterKey != RosterKey)
		{
			ResetAttackOrderDraft(State, OwnEntry->TeamIndex);
		}
	}

	TArray<int32> TeamSlots;
	if (State && OwnEntry)
	{
		State->GetTeamAttackOrderSlots(OwnEntry->TeamIndex, TeamSlots);
	}

	// The submit latch is provisional until the server's lock replicates back. If the
	// lock never appears, the pick was rejected (e.g. a stale single-slot auto-lock at
	// the time of the click) - re-open the picker so the selector can try again
	// instead of leaving them convinced they already picked.
	if (bAttackOrderSubmitted && State && OwnEntry && OwnEntry->TeamIndex <= 1
		&& !State->IsAttackOrderLocked(OwnEntry->TeamIndex)
		&& GetWorld()->GetTimeSeconds() - AttackOrderSubmitTime > 2.0f)
	{
		bAttackOrderSubmitted = false;
	}

	const bool bShouldEnable = bPanelVisible && OwnEntry
		&& OwnEntry->bAttackOrderSelector
		&& !State->IsAttackOrderLocked(OwnEntry->TeamIndex)
		&& TeamSlots.Num() > 1
		&& !bAttackOrderSubmitted;

	if (bShouldEnable && !bAttackOrderInputActive && UTPlayerOwner)
	{
		bSavedShowMouseCursor = UTPlayerOwner->bShowMouseCursor;
		bSavedEnableClickEvents = UTPlayerOwner->bEnableClickEvents;
		bSavedEnableMouseOverEvents = UTPlayerOwner->bEnableMouseOverEvents;
		UTPlayerOwner->bShowMouseCursor = true;
		UTPlayerOwner->bEnableClickEvents = true;
		UTPlayerOwner->bEnableMouseOverEvents = true;
		bAttackOrderInputActive = true;
	}
	else if (!bShouldEnable)
	{
		if (bAttackOrderInputActive)
		{
			UE_LOG(LogClutchHUD, Log,
				TEXT("Attack-order input disabled: panel=%d entry=%d selector=%d locked=%d slots=%d submitted=%d phase=%d"),
				bPanelVisible, OwnEntry != nullptr,
				OwnEntry ? OwnEntry->bAttackOrderSelector : false,
				State && OwnEntry && OwnEntry->TeamIndex <= 1
					? State->IsAttackOrderLocked(OwnEntry->TeamIndex) : false,
				TeamSlots.Num(), bAttackOrderSubmitted,
				State ? static_cast<int32>(State->Phase) : -1);
		}
		RestoreAttackOrderInput();
	}
}


void AClutchHUD::RestoreAttackOrderInput()
{
	if (!bAttackOrderInputActive)
	{
		return;
	}
	if (UTPlayerOwner)
	{
		UTPlayerOwner->bShowMouseCursor = bSavedShowMouseCursor;
		UTPlayerOwner->bEnableClickEvents = bSavedEnableClickEvents;
		UTPlayerOwner->bEnableMouseOverEvents = bSavedEnableMouseOverEvents;
	}
	bAttackOrderInputActive = false;
}


void AClutchHUD::ResetAttackOrderDraft(
	AClutchRoundState* State, uint8 TeamIndex)
{
	DraftAttackOrderTeam = TeamIndex;
	DraftAttackOrderRosterKey = BuildAttackOrderRosterKey(State, TeamIndex);
	DraftAttackOrderSlots.Reset();
	bAttackOrderSubmitted = false;
	AttackOrderSubmitTime = -1000.0f;
}


void AClutchHUD::PickAttackOrderSlot(uint8 RosterSlot)
{
	AClutchRoundState* State = ResolveClutchState();
	if (!State || State->Phase != EClutchRoundPhase::OrderSelection
		|| DraftAttackOrderTeam > 1
		|| DraftAttackOrderSlots.Contains(RosterSlot))
	{
		UE_LOG(LogClutchHUD, Warning,
			TEXT("Attack-order pick ignored: slot=%d state=%d phase=%d team=%d duplicate=%d"),
			RosterSlot, State != nullptr,
			State ? static_cast<int32>(State->Phase) : -1,
			DraftAttackOrderTeam,
			DraftAttackOrderSlots.Contains(RosterSlot));
		return;
	}

	TArray<int32> EligibleSlots;
	State->GetTeamAttackOrderSlots(DraftAttackOrderTeam, EligibleSlots);
	if (!EligibleSlots.Contains(static_cast<int32>(RosterSlot)))
	{
		UE_LOG(LogClutchHUD, Warning,
			TEXT("Attack-order pick ignored: slot %d is not eligible for team %d"),
			RosterSlot, DraftAttackOrderTeam);
		return;
	}
	DraftAttackOrderSlots.Add(RosterSlot);

	// Once every meaningful choice is made, append the sole remaining player.
	if (EligibleSlots.Num() > 1
		&& DraftAttackOrderSlots.Num() == EligibleSlots.Num() - 1)
	{
		for (int32 Slot : EligibleSlots)
		{
			if (!DraftAttackOrderSlots.Contains(static_cast<uint8>(Slot)))
			{
				DraftAttackOrderSlots.Add(static_cast<uint8>(Slot));
			}
		}
	}

	UE_LOG(LogClutchHUD, Log,
		TEXT("Attack-order picked slot %d for team %d: draft contains %d/%d slots"),
		RosterSlot, DraftAttackOrderTeam,
		DraftAttackOrderSlots.Num(), EligibleSlots.Num());
}


void AClutchHUD::SubmitAttackOrderDraft()
{
	AClutchRoundState* State = ResolveClutchState();
	if (!State || !UTPlayerOwner || DraftAttackOrderTeam > 1)
	{
		return;
	}

	TArray<int32> EligibleSlots;
	TArray<int32> ProposedSlots;
	State->GetTeamAttackOrderSlots(DraftAttackOrderTeam, EligibleSlots);
	for (uint8 Slot : DraftAttackOrderSlots)
	{
		ProposedSlots.Add(static_cast<int32>(Slot));
	}
	if (!AClutchRoundState::IsValidAttackOrder(EligibleSlots, ProposedSlots))
	{
		return;
	}

	FString Command(TEXT("ClutchOrder"));
	for (int32 Slot : ProposedSlots)
	{
		Command += FString::Printf(TEXT(" %d"), Slot);
	}
	// AUTPlayerController::Mutate uses the stock reliable ServerMutate RPC.
	UE_LOG(LogClutchHUD, Log,
		TEXT("Submitting attack order for team %d: %s"),
		DraftAttackOrderTeam, *Command);
	UTPlayerOwner->Mutate(Command);
	bAttackOrderSubmitted = true;
	AttackOrderSubmitTime = GetWorld()->GetTimeSeconds();
	RestoreAttackOrderInput();
}


void AClutchHUD::NotifyHitBoxClick(FName BoxName)
{
	const FString Name = BoxName.ToString();
	const FString PickPrefix(TEXT("ClutchOrderPick_"));
	if (Name.StartsWith(PickPrefix, ESearchCase::IgnoreCase))
	{
		const FString SlotText = Name.RightChop(PickPrefix.Len());
		if (SlotText.IsNumeric())
		{
			PickAttackOrderSlot(static_cast<uint8>(FMath::Clamp(
				FCString::Atoi(*SlotText), 0, 255)));
		}
		return;
	}
	if (Name.Equals(TEXT("ClutchOrderConfirm"), ESearchCase::IgnoreCase))
	{
		SubmitAttackOrderDraft();
		return;
	}
	if (Name.Equals(TEXT("ClutchOrderReset"), ESearchCase::IgnoreCase))
	{
		ResetAttackOrderDraft(ResolveClutchState(), DraftAttackOrderTeam);
		return;
	}

	Super::NotifyHitBoxClick(BoxName);
}


void AClutchHUD::Destroyed()
{
	RestoreAttackOrderInput();
	RestoreSpectatorCameraPreference();
	Super::Destroyed();
}


void AClutchHUD::UpdateEliminatedCameraRestriction(AClutchRoundState* State)
{
	AUTPlayerState* OwnState = UTPlayerOwner
		? Cast<AUTPlayerState>(UTPlayerOwner->PlayerState)
		: nullptr;
	const FClutchRosterEntry* OwnEntry = OwnState && State
		? State->FindEntry(OwnState)
		: nullptr;
	const bool bShouldForceFirstPerson = UTPlayerOwner && OwnState
		&& !OwnState->bOnlySpectator
		&& UTPlayerOwner->IsInState(NAME_Spectating)
		&& OwnEntry && OwnEntry->PlayerStatus == EClutchStatus::Eliminated
		&& State->IsGameplayPhase();
	if (!bShouldForceFirstPerson)
	{
		RestoreSpectatorCameraPreference();
		return;
	}

	if (!bEliminatedCameraForced)
	{
		bSavedSpectateBehindView = UTPlayerOwner->bSpectateBehindView;
		bEliminatedCameraForced = true;
	}
	if (UTPlayerOwner->bSpectateBehindView || UTPlayerOwner->IsBehindView())
	{
		// ToggleBehindView may be bound locally and never reaches the game mode.
		// Reassert first person from the owning HUD every frame while eliminated.
		UTPlayerOwner->BehindView(false);
	}
}


void AClutchHUD::RestoreSpectatorCameraPreference()
{
	if (!bEliminatedCameraForced)
	{
		return;
	}

	bEliminatedCameraForced = false;
	if (UTPlayerOwner)
	{
		// A true spectator/caster never enters the restriction. If a playing client
		// leaves the eliminated state, restore exactly the preference they had before.
		UTPlayerOwner->BehindView(bSavedSpectateBehindView);
	}
}


void AClutchHUD::DrawAttackOrderPanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !MediumFont || !UTPlayerOwner)
	{
		return;
	}

	AUTPlayerState* OwnState = Cast<AUTPlayerState>(UTPlayerOwner->PlayerState);
	const FClutchRosterEntry* OwnEntry = OwnState ? State->FindEntry(OwnState) : nullptr;
	if (!OwnEntry || OwnEntry->TeamIndex > 1)
	{
		return;
	}

	const uint8 TeamIndex = OwnEntry->TeamIndex;
	TArray<int32> OrderedSlots;
	State->GetTeamAttackOrderSlots(TeamIndex, OrderedSlots);
	if (OrderedSlots.Num() == 0)
	{
		return;
	}

	if (DraftAttackOrderTeam != TeamIndex
		|| DraftAttackOrderRosterKey != BuildAttackOrderRosterKey(State, TeamIndex))
	{
		ResetAttackOrderDraft(State, TeamIndex);
	}

	const float Scale = FMath::Min(
		static_cast<float>(Canvas->SizeX) / 1920.0f,
		static_cast<float>(Canvas->SizeY) / 1080.0f);
	const float PanelWidth = 720.0f * Scale;
	const float PanelHeight = 350.0f * Scale;
	const float PanelX = (Canvas->ClipX - PanelWidth) * 0.5f;
	const float PanelY = (Canvas->ClipY - PanelHeight) * 0.46f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const FLinearColor TeamColor = TeamIndex == 0
		? FLinearColor(0.95f, 0.08f, 0.025f, 1.0f)
		: FLinearColor(0.20f, 0.25f, 1.0f, 1.0f);

	DrawSolidTile(Canvas, PanelX, PanelY, PanelWidth, PanelHeight,
		FLinearColor(0.008f, 0.018f, 0.035f, 0.94f));
	DrawSolidTile(Canvas, PanelX, PanelY, PanelWidth, 5.0f * Scale, TeamColor);
	DrawCenteredCanvasText(Canvas, MediumFont, TEXT("CHOOSE YOUR ATTACK ORDER"),
		CenterX, PanelY + 24.0f * Scale, 0.86f * Scale, FColor::White);

	AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
	const float Now = GameState
		? GameState->GetServerWorldTimeSeconds()
		: GetWorld()->GetTimeSeconds();
	const int32 SecondsRemaining = FMath::Max(0, FMath::CeilToInt(
		State->AttackOrderDeadlineServerTime - Now));
	DrawCenteredCanvasText(Canvas, SmallFont,
		FString::Printf(TEXT("%d seconds"), SecondsRemaining),
		CenterX, PanelY + 58.0f * Scale, 0.55f * Scale,
		SecondsRemaining <= 5 ? FColor(255, 90, 90) : FColor(190, 210, 230));

	const bool bLocked = State->IsAttackOrderLocked(TeamIndex);
	const bool bCanEdit = OwnEntry->bAttackOrderSelector && !bLocked
		&& !bAttackOrderSubmitted && OrderedSlots.Num() > 1;
	if (!bCanEdit)
	{
		FString StatusText;
		if (bLocked)
		{
			StatusText = TEXT("ORDER LOCKED");
		}
		else if (bAttackOrderSubmitted)
		{
			StatusText = TEXT("ORDER SENT - WAITING FOR SERVER");
		}
		else
		{
			FString SelectorName(TEXT("YOUR TEAM SELECTOR"));
			for (const FClutchRosterEntry& Entry : State->Roster)
			{
				if (Entry.TeamIndex == TeamIndex && Entry.bAttackOrderSelector)
				{
					SelectorName = Entry.PlayerState
						? Entry.PlayerState->PlayerName
						: Entry.PlayerNameFallback;
					break;
				}
			}
			StatusText = FString::Printf(TEXT("WAITING FOR %s"), *SelectorName.ToUpper());
		}
		DrawCenteredCanvasText(Canvas, MediumFont, StatusText, CenterX,
			PanelY + 94.0f * Scale, 0.66f * Scale,
			bLocked ? FColor(100, 255, 130) : FColor(210, 220, 235));

		const float RowY = PanelY + 145.0f * Scale;
		const float RowWidth = (PanelWidth - 80.0f * Scale)
			/ static_cast<float>(FMath::Max(1, OrderedSlots.Num()));
		for (int32 Index = 0; Index < OrderedSlots.Num(); ++Index)
		{
			const FClutchRosterEntry* Entry = FindTeamRosterSlot(
				State, TeamIndex, OrderedSlots[Index]);
			const float X = PanelX + 40.0f * Scale + Index * RowWidth;
			DrawSolidTile(Canvas, X + 3.0f * Scale, RowY,
				RowWidth - 6.0f * Scale, 105.0f * Scale,
				FLinearColor(0.035f, 0.07f, 0.12f, 0.92f));
			DrawCenteredCanvasText(Canvas, MediumFont,
				FString::FromInt(Index + 1), X + RowWidth * 0.5f,
				RowY + 10.0f * Scale, 0.75f * Scale, TeamColor.ToFColor(true));
			DrawCenteredCanvasText(Canvas, SmallFont,
				Entry && Entry->PlayerState ? Entry->PlayerState->PlayerName
					: Entry ? Entry->PlayerNameFallback : TEXT("PLAYER"),
				X + RowWidth * 0.5f, RowY + 58.0f * Scale,
				0.56f * Scale, FColor::White);
		}
		return;
	}

	FString Prompt(TEXT("CHOOSE FIRST ATTACKER"));
	if (DraftAttackOrderSlots.Num() == 1 && OrderedSlots.Num() > 2)
	{
		Prompt = TEXT("CHOOSE SECOND ATTACKER");
	}
	else if (DraftAttackOrderSlots.Num() == OrderedSlots.Num())
	{
		Prompt = TEXT("REVIEW AND CONFIRM");
	}
	DrawCenteredCanvasText(Canvas, MediumFont, Prompt, CenterX,
		PanelY + 91.0f * Scale, 0.70f * Scale, FColor::White);

	const float CardsY = PanelY + 132.0f * Scale;
	const float CardsWidth = PanelWidth - 70.0f * Scale;
	const float CardWidth = CardsWidth
		/ static_cast<float>(FMath::Max(1, OrderedSlots.Num()));
	for (int32 CardIndex = 0; CardIndex < OrderedSlots.Num(); ++CardIndex)
	{
		const uint8 Slot = static_cast<uint8>(OrderedSlots[CardIndex]);
		const int32 SelectedIndex = DraftAttackOrderSlots.Find(Slot);
		const FClutchRosterEntry* Entry = FindTeamRosterSlot(State, TeamIndex, Slot);
		const float X = PanelX + 35.0f * Scale + CardIndex * CardWidth;
		const FLinearColor CardColor = SelectedIndex == INDEX_NONE
			? FLinearColor(0.04f, 0.085f, 0.145f, 0.96f)
			: FLinearColor(0.06f, 0.26f, 0.13f, 0.96f);
		DrawSolidTile(Canvas, X + 4.0f * Scale, CardsY,
			CardWidth - 8.0f * Scale, 112.0f * Scale, CardColor);

		const TCHAR ButtonLetter = static_cast<TCHAR>('A' + CardIndex);
		DrawCenteredCanvasText(Canvas, MediumFont,
			FString::Printf(TEXT("[%c]"), ButtonLetter),
			X + CardWidth * 0.5f, CardsY + 8.0f * Scale,
			0.66f * Scale, TeamColor.ToFColor(true));
		DrawCenteredCanvasText(Canvas, SmallFont,
			Entry && Entry->PlayerState ? Entry->PlayerState->PlayerName
				: Entry ? Entry->PlayerNameFallback : TEXT("PLAYER"),
			X + CardWidth * 0.5f, CardsY + 60.0f * Scale,
			0.54f * Scale, FColor::White);
		if (SelectedIndex != INDEX_NONE)
		{
			DrawCenteredCanvasText(Canvas, SmallFont,
				FString::Printf(TEXT("PICK %d"), SelectedIndex + 1),
				X + CardWidth * 0.5f, CardsY + 88.0f * Scale,
				0.48f * Scale, FColor(100, 255, 130));
		}
		else
		{
			AddHitBox(FVector2D(X + 4.0f * Scale, CardsY),
				FVector2D(CardWidth - 8.0f * Scale, 112.0f * Scale),
				FName(*FString::Printf(TEXT("ClutchOrderPick_%d"), Slot)), true, 20);
		}
	}

	const float ButtonY = PanelY + PanelHeight - 63.0f * Scale;
	if (DraftAttackOrderSlots.Num() > 0)
	{
		const float ResetX = CenterX - 210.0f * Scale;
		DrawSolidTile(Canvas, ResetX, ButtonY, 170.0f * Scale, 38.0f * Scale,
			FLinearColor(0.14f, 0.16f, 0.19f, 0.98f));
		DrawCenteredCanvasText(Canvas, SmallFont, TEXT("RESET"),
			ResetX + 85.0f * Scale, ButtonY + 8.0f * Scale,
			0.54f * Scale, FColor::White);
		AddHitBox(FVector2D(ResetX, ButtonY),
			FVector2D(170.0f * Scale, 38.0f * Scale),
			FName(TEXT("ClutchOrderReset")), true, 30);
	}
	if (DraftAttackOrderSlots.Num() == OrderedSlots.Num())
	{
		const float ConfirmX = CenterX + 40.0f * Scale;
		DrawSolidTile(Canvas, ConfirmX, ButtonY, 170.0f * Scale, 38.0f * Scale,
			FLinearColor(0.08f, 0.55f, 0.20f, 0.98f));
		DrawCenteredCanvasText(Canvas, SmallFont, TEXT("CONFIRM ORDER"),
			ConfirmX + 85.0f * Scale, ButtonY + 8.0f * Scale,
			0.54f * Scale, FColor::White);
		AddHitBox(FVector2D(ConfirmX, ButtonY),
			FVector2D(170.0f * Scale, 38.0f * Scale),
			FName(TEXT("ClutchOrderConfirm")), true, 30);
	}
}


void AClutchHUD::DrawTeamScoreBar(AUTGameState* GameState)
{
	AClutchRoundState* State = ResolveClutchState();
	if (!Canvas || !GameState || !State || !SmallFont || !MediumFont
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}

	LoadRecoveredHUDTextures();

	// Do not delegate this to AWipeoutHUD. ClutchMini used a transparent
	// 2560x1440 overlay whose angular red/blue frame is now recovered verbatim.
	const float RenderScale = FMath::Min(
		static_cast<float>(Canvas->SizeX) / 1920.0f,
		static_cast<float>(Canvas->SizeY) / 1080.0f);
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float HUDScale = GetHUDWidgetScaleOverride();
	const float FrameScale = ScoreScale * HUDScale;
	const float UnitScale = RenderScale * FrameScale;
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 2.0f * RenderScale));
	const float CenterX = ScoreBarPos.X;
	const float TopY = ScoreBarPos.Y;
	const float ScoreOpacity = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	float ScoreCenters[2] = { 0.0f, 0.0f };
	float ScoreTextY = TopY;
	float ScoreFontScale = 1.02f * UnitScale;
	float ClockY = TopY;
	float ClockFontScale = 0.82f * UnitScale;

	if (LegacyTopFrameTexture)
	{
		const float FrameWidth = Canvas->ClipX * FrameScale;
		const float FrameHeight = Canvas->ClipY * FrameScale;
		const float FrameX = CenterX - FrameWidth * 0.5f;
		const float FrameY = TopY - 2.0f * RenderScale;
		const float FrameScaleX = FrameWidth / 2560.0f;
		const float FrameScaleY = FrameHeight / 1440.0f;

		DrawTextureTile(Canvas, LegacyTopFrameTexture,
			FrameX, FrameY, FrameWidth, FrameHeight,
			FLinearColor(1.0f, 1.0f, 1.0f, ScoreOpacity));

		// Coordinates are taken from the recovered ClutchMini texture. Keeping
		// them in its source space makes the art, scores and portraits scale as
		// one composition at every viewport resolution.
		ScoreCenters[0] = FrameX + 1130.0f * FrameScaleX;
		ScoreCenters[1] = FrameX + 1430.0f * FrameScaleX;
		ScoreTextY = FrameY + 15.0f * FrameScaleY;
		ScoreFontScale = 1.08f * UnitScale;
		ClockY = FrameY + 82.0f * FrameScaleY;
		ClockFontScale = 0.78f * UnitScale;
	}
	else
	{
		// Safe fallback for installs that copy only the binary and omit Resources.
		const float ScoreWidth = 44.0f * UnitScale;
		const float ScoreHeight = 38.0f * UnitScale;
		const float CenterGap = 4.0f * UnitScale;
		const float Score0X = CenterX - CenterGap * 0.5f - ScoreWidth;
		const float Score1X = CenterX + CenterGap * 0.5f;
		const float PipSize = 42.0f * UnitScale;
		const float PipStep = PipSize + 3.0f * UnitScale;
		const float PortraitSpan = PipSize + 2.0f * PipStep;
		const FLinearColor Navy(0.015f, 0.055f, 0.095f, 0.94f * ScoreOpacity);
		FLinearColor TeamColors[2] = {
			FLinearColor(0.92f, 0.045f, 0.02f, ScoreOpacity),
			FLinearColor(0.12f, 0.17f, 1.0f, ScoreOpacity)
		};

		if (NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar")))
		{
			for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
			{
				if (GameState->Teams.IsValidIndex(TeamIndex) && GameState->Teams[TeamIndex])
				{
					TeamColors[TeamIndex] = GameState->Teams[TeamIndex]->TeamColor;
					TeamColors[TeamIndex].A = ScoreOpacity;
				}
			}
		}

		DrawSolidTile(Canvas, Score0X - 4.0f * UnitScale, TopY,
			ScoreWidth * 2.0f + CenterGap + 8.0f * UnitScale,
			ScoreHeight + 5.0f * UnitScale, Navy);
		DrawSolidTile(Canvas, Score0X - PortraitSpan,
			TopY + ScoreHeight - 4.0f * UnitScale,
			PortraitSpan, 4.0f * UnitScale, TeamColors[0]);
		DrawSolidTile(Canvas, Score1X + ScoreWidth,
			TopY + ScoreHeight - 4.0f * UnitScale,
			PortraitSpan, 4.0f * UnitScale, TeamColors[1]);

		FLinearColor ScoreColors[2] = { TeamColors[0], TeamColors[1] };
		for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
		{
			ScoreColors[TeamIndex].R *= 0.78f;
			ScoreColors[TeamIndex].G *= 0.78f;
			ScoreColors[TeamIndex].B *= 0.78f;
		}
		DrawSolidTile(Canvas, Score0X, TopY, ScoreWidth, ScoreHeight, ScoreColors[0]);
		DrawSolidTile(Canvas, Score1X, TopY, ScoreWidth, ScoreHeight, ScoreColors[1]);
		DrawSolidTile(Canvas, CenterX - 1.0f * UnitScale, TopY,
			2.0f * UnitScale, ScoreHeight + 5.0f * UnitScale,
			FLinearColor(0.85f, 0.93f, 1.0f, ScoreOpacity));

		ScoreCenters[0] = Score0X + ScoreWidth * 0.5f;
		ScoreCenters[1] = Score1X + ScoreWidth * 0.5f;
		ScoreTextY = TopY + 2.0f * UnitScale;
		ClockY = TopY + ScoreHeight + 5.0f * UnitScale;
	}

	const int32 Scores[2] = {
		GameState->Teams.IsValidIndex(0) && GameState->Teams[0]
			? static_cast<int32>(GameState->Teams[0]->Score) : 0,
		GameState->Teams.IsValidIndex(1) && GameState->Teams[1]
			? static_cast<int32>(GameState->Teams[1]->Score) : 0
	};
	const uint8 TextAlpha = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(ScoreOpacity * 255.0f), 0, 255));
	const FColor HeaderText(255, 255, 255, TextAlpha);
	DrawCenteredCanvasText(Canvas, MediumFont, FString::FromInt(Scores[0]),
		ScoreCenters[0], ScoreTextY,
		ScoreFontScale, HeaderText);
	DrawCenteredCanvasText(Canvas, MediumFont, FString::FromInt(Scores[1]),
		ScoreCenters[1], ScoreTextY,
		ScoreFontScale, HeaderText);

	const float Now = GameState->GetServerWorldTimeSeconds();
	const float RelevantEndTime = State->Phase == EClutchRoundPhase::OrderSelection
		? State->AttackOrderDeadlineServerTime
		: State->RoundEndServerTime;
	const int32 Remaining = RelevantEndTime > 0.0f
		? FMath::Max(0, FMath::CeilToInt(RelevantEndTime - Now))
		: 0;
	const FString Clock = FString::Printf(TEXT("%02d:%02d"),
		Remaining / 60, Remaining % 60);
	DrawCenteredCanvasText(Canvas, SmallFont, Clock, CenterX, ClockY,
		ClockFontScale,
		Remaining <= 10 && State->IsGameplayPhase()
			? FColor(255, 80, 80, TextAlpha)
			: HeaderText);
}


AClutchRoundState* AClutchHUD::ResolveClutchState()
{
	if (CachedClutchState.IsValid()
		&& CachedClutchState->GetWorld() == GetWorld())
	{
		return CachedClutchState.Get();
	}

	CachedClutchState.Reset();
	for (TActorIterator<AClutchRoundState> It(GetWorld()); It; ++It)
	{
		CachedClutchState = *It;
		return *It;
	}
	return nullptr;
}


AUTPlayerState* AClutchHUD::ResolveDisplayedPlayerState(AClutchRoundState* State) const
{
	AUTPlayerState* OwnState = UTPlayerOwner
		? Cast<AUTPlayerState>(UTPlayerOwner->PlayerState)
		: nullptr;
	if (OwnState && State && State->FindEntry(OwnState))
	{
		return OwnState;
	}

	APawn* ViewedPawn = UTPlayerOwner
		? Cast<APawn>(UTPlayerOwner->GetViewTarget())
		: nullptr;
	return ViewedPawn ? Cast<AUTPlayerState>(ViewedPawn->PlayerState) : OwnState;
}


void AClutchHUD::LoadRecoveredHUDTextures()
{
	if (bTriedLoadRecoveredHUDTextures)
	{
		return;
	}
	bTriedLoadRecoveredHUDTextures = true;

	// Editor-safe imports of the layered HUD artwork recovered from the 2020
	// Clutch package. These are the circular assets from the reference captures,
	// not the unrelated square Center_PB prototype.
	LegacyCircleTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_circle.clutch_circle"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_circle.clutch_circle"));
	LegacyInnerCircleTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_inner_circle.clutch_inner_circle"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_inner_circle.clutch_inner_circle"));
	LegacyAttackingTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attacking.clutch_attacking"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attacking.clutch_attacking"));
	LegacyLeftGadgetTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_left_gadget.clutch_left_gadget"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_left_gadget.clutch_left_gadget"));
	LegacyRightGadgetTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_right_gadget.clutch_right_gadget"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_right_gadget.clutch_right_gadget"));
	LegacyShieldTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_shield.clutch_shield"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_shield.clutch_shield"));
	LegacyInstaTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_insta.clutch_insta"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_insta.clutch_insta"));
	LegacyRocketTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_rocket.clutch_rocket"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_rocket.clutch_rocket"));
	LegacyAttackRailBackTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_rail_back.clutch_attack_rail_back"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_rail_back.clutch_attack_rail_back"));
	LegacyAttackRailFrontTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_rail_front.clutch_attack_rail_front"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_rail_front.clutch_attack_rail_front"));
	LegacyAttackProgressTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_progress.clutch_attack_progress"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_progress.clutch_attack_progress"));
	LegacyDefendRailBackTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_rail_back.clutch_defend_rail_back"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_rail_back.clutch_defend_rail_back"));
	LegacyDefendRailFrontTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_rail_front.clutch_defend_rail_front"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_rail_front.clutch_defend_rail_front"));
	LegacyDefendProgressTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_progress.clutch_defend_progress"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_progress.clutch_defend_progress"));

	// Prefer a normal editor asset when one has been imported. The checked-in
	// PNG fallback keeps the exact recovered ClutchMini frame available in both
	// source and binary plugin installs without relying on its cooked uasset.
	LegacyTopFrameTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/hud_top_bottom2.hud_top_bottom2"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/hud_top_bottom2.hud_top_bottom2"));
	if (!LegacyTopFrameTexture)
	{
		LegacyTopFrameTexture = LoadPluginPngTexture(TEXT("hud_top_bottom2.png"));
	}
}


void AClutchHUD::DrawClutchPortraits(AClutchRoundState* State)
{
	if (!State || !Canvas || ScoreboardIsUp()
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}
	LoadRecoveredHUDTextures();

	const float RenderScale = FMath::Min(
		static_cast<float>(Canvas->SizeX) / 1920.0f,
		static_cast<float>(Canvas->SizeY) / 1080.0f);
	const float FrameScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"))
		* GetHUDWidgetScaleOverride();
	const float UnitScale = RenderScale * FrameScale;
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 2.0f * RenderScale));
	const float CenterX = ScoreBarPos.X;
	float PipSize = 42.0f * UnitScale;
	float PipStep = PipSize + 3.0f * UnitScale;
	float PortraitY = ScoreBarPos.Y;
	float FirstPortraitX[2] = {
		CenterX - 46.0f * UnitScale - PipSize,
		CenterX + 46.0f * UnitScale
	};
	float PortraitDirection[2] = { -1.0f, 1.0f };

	if (LegacyTopFrameTexture)
	{
		const float FrameWidth = Canvas->ClipX * FrameScale;
		const float FrameHeight = Canvas->ClipY * FrameScale;
		const float FrameX = CenterX - FrameWidth * 0.5f;
		const float FrameY = ScoreBarPos.Y - 2.0f * RenderScale;
		const float FrameScaleX = FrameWidth / 2560.0f;
		const float FrameScaleY = FrameHeight / 1440.0f;

		// ClutchMini's portrait boxes were laid directly over the recovered
		// frame. These source-space anchors place the inside portrait first,
		// then grow each team away from its score cell.
		PipSize = 76.0f * FrameScaleX;
		PipStep = 79.0f * FrameScaleX;
		PortraitY = FrameY + 4.0f * FrameScaleY;
		FirstPortraitX[0] = FrameX + 980.0f * FrameScaleX;
		FirstPortraitX[1] = FrameX + 1504.0f * FrameScaleX;
	}
	// The portrait nearest each score is that team's chosen first attacker.
	// This makes the persistent top strip double as a compact order reminder.
	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		TArray<int32> OrderedSlots;
		State->GetTeamAttackOrderSlots(static_cast<uint8>(TeamIndex), OrderedSlots);
		for (int32 DrawIndex = 0; DrawIndex < OrderedSlots.Num() && DrawIndex < 3; ++DrawIndex)
		{
			const FClutchRosterEntry* Entry = FindTeamRosterSlot(
				State, static_cast<uint8>(TeamIndex), OrderedSlots[DrawIndex]);
			if (!Entry || !Entry->PlayerState)
			{
				continue;
			}

			const float PortraitX = FirstPortraitX[TeamIndex]
				+ PortraitDirection[TeamIndex] * DrawIndex * PipStep;
			const float LiveScaling = Entry->PlayerStatus == EClutchStatus::Active
				? 1.0f
				: 0.0f;
			DrawPlayerIcon(Entry->PlayerState, LiveScaling,
				PortraitX, PortraitY, PipSize);
		}
	}
}


void AClutchHUD::DrawDefenderAttackerPanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !MediumFont || !UTPlayerOwner
		|| !State->IsGameplayPhase() || !State->ActiveAttacker)
	{
		return;
	}

	const FClutchRosterEntry* AttackerEntry = State->FindEntry(State->ActiveAttacker);
	if (!AttackerEntry)
	{
		return;
	}

	// The attacker armor is round-critical shared information. Show this panel to
	// attackers, defenders, benched teammates, eliminated players and true specs.
	const bool bHitFlash = GetWorld()
		&& GetWorld()->GetTimeSeconds() < AttackerHitFlashEndTime;
	const float Scale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float PanelWidth = 340.0f * Scale;
	const float PanelHeight = 45.0f * Scale;
	const float PanelX = (Canvas->ClipX - PanelWidth) * 0.5f;
	const float PanelY = Canvas->ClipY - 252.0f * Scale;
	const FLinearColor TeamAccent = bHitFlash
		? FLinearColor(1.0f, 0.42f, 0.04f, 1.0f)
		: State->AttackingTeamIndex == 0
		? FLinearColor(0.95f, 0.08f, 0.025f, 0.96f)
		: FLinearColor(0.20f, 0.25f, 1.0f, 0.96f);
	DrawSolidTile(Canvas, PanelX, PanelY, PanelWidth, PanelHeight,
		bHitFlash
			? FLinearColor(0.30f, 0.055f, 0.01f, 0.94f)
			: FLinearColor(0.008f, 0.018f, 0.035f, 0.88f));
	DrawSolidTile(Canvas, PanelX, PanelY, 4.0f * Scale, PanelHeight, TeamAccent);

	const FString AttackerName = State->ActiveAttacker->PlayerName.IsEmpty()
		? AttackerEntry->PlayerNameFallback
		: State->ActiveAttacker->PlayerName;
	DrawCenteredCanvasText(Canvas, SmallFont,
		FString::Printf(bHitFlash ? TEXT("ATTACKER HIT  %s") : TEXT("ATTACKER  %s"),
			*AttackerName),
		Canvas->ClipX * 0.5f, PanelY + 4.0f * Scale,
		0.58f * Scale, FColor::White);

	const int32 PipCount = FMath::Clamp<int32>(State->MaxAttackerHits, 1, 10);
	const int32 Remaining = State->GetEntryArmorRemaining(*AttackerEntry);
	const float PipWidth = 22.0f * Scale;
	const float PipHeight = 7.0f * Scale;
	const float PipGap = 5.0f * Scale;
	const float TotalPipWidth = PipCount * PipWidth + (PipCount - 1) * PipGap;
	const float FirstPipX = Canvas->ClipX * 0.5f - TotalPipWidth * 0.5f;
	const float PipY = PanelY + 29.0f * Scale;
	for (int32 Index = 0; Index < PipCount; ++Index)
	{
		const bool bJustLost = bHitFlash && Index == Remaining;
		DrawSolidTile(Canvas, FirstPipX + Index * (PipWidth + PipGap), PipY,
			PipWidth, PipHeight,
			Index < Remaining
				? FLinearColor(0.36f, 1.0f, 0.28f, 0.98f)
				: bJustLost
					? FLinearColor(1.0f, 0.18f, 0.03f, 1.0f)
					: FLinearColor(0.18f, 0.18f, 0.18f, 0.82f));
	}
}


void AClutchHUD::DrawRolePanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !MediumFont)
	{
		return;
	}

	AUTPlayerState* DisplayedState = ResolveDisplayedPlayerState(State);
	const FClutchRosterEntry* Entry = DisplayedState
		? State->FindEntry(DisplayedState)
		: nullptr;

	FString Title = GetPhaseLabel(State->Phase);
	FLinearColor Accent(0.35f, 0.55f, 0.95f, 1.0f);
	bool bActiveAttacker = false;
	bool bActiveDefender = false;
	if (Entry)
	{
		// The original HUD's right rail and weapon glow follow the player's
		// team, while green is reserved for health/armor on the left rail.
		Accent = Entry->TeamIndex == 0
			? FLinearColor(1.0f, 0.12f, 0.05f, 1.0f)
			: FLinearColor(0.27f, 0.29f, 1.0f, 1.0f);

		if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Attacker)
		{
			Title = TEXT("ATTACKING");
			bActiveAttacker = true;
		}
		else if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Defender)
		{
			Title = TEXT("DEFENDING");
			bActiveDefender = true;
		}
		else if (Entry->PlayerStatus == EClutchStatus::Benched)
		{
			Title = TEXT("SPECTATING");
			Accent = FLinearColor(0.72f, 0.72f, 0.72f, 1.0f);
		}
		else if (Entry->PlayerStatus == EClutchStatus::Eliminated)
		{
			Title = TEXT("ELIMINATED");
			Accent = FLinearColor(0.85f, 0.20f, 0.20f, 1.0f);
		}
		else
		{
			Title = TEXT("WAITING");
		}
	}

	LoadRecoveredHUDTextures();

	const float RenderScale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const float ModuleCenterY = Canvas->ClipY - 50.0f * RenderScale;
	const float CircleWidth = 120.0f * RenderScale;
	const float CircleHeight = 113.0f * RenderScale;
	const float RailY = ModuleCenterY - 2.5f * RenderScale;
	const float RailHeight = 5.0f * RenderScale;
	const float RailWidth = 168.0f * RenderScale;
	const float RailGap = 48.0f * RenderScale;
	const FLinearColor Navy(0.02f, 0.07f, 0.12f, 0.78f);
	const FLinearColor HealthGreen(0.36f, 1.0f, 0.28f, 0.95f);

	float LeftRatio = 1.0f;
	if (bActiveAttacker)
	{
		LeftRatio = static_cast<float>(State->GetEntryArmorRemaining(*Entry))
			/ static_cast<float>(FMath::Max<int32>(1, State->MaxAttackerHits));
	}
	LeftRatio = FMath::Clamp(LeftRatio, 0.0f, 1.0f);

	// Draw the original one-pixel rail art as scalable crisp lines. The back
	// layer establishes the shape, while the progress layer carries gameplay
	// color and the same left/right proportions as the recovered PSD.
	const float LeftRailX = CenterX - RailGap - RailWidth;
	const float RightRailX = CenterX + RailGap;
	UTexture2D* RailBack = bActiveAttacker
		? LegacyAttackRailBackTexture
		: LegacyDefendRailBackTexture;
	UTexture2D* RailFront = bActiveAttacker
		? LegacyAttackRailFrontTexture
		: LegacyDefendRailFrontTexture;
	UTexture2D* RailProgress = bActiveAttacker
		? LegacyAttackProgressTexture
		: LegacyDefendProgressTexture;
	if (RailBack || RailFront)
	{
		DrawTextureTile(Canvas, RailBack, LeftRailX, RailY,
			RailWidth, RailHeight, FLinearColor::White);
		DrawTextureTile(Canvas, RailBack, RightRailX, RailY,
			RailWidth, RailHeight, FLinearColor::White);
		DrawTextureTile(Canvas, RailFront, LeftRailX, RailY,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawTextureTile(Canvas, RailFront, RightRailX, RailY,
			RailWidth, RailHeight, Accent);
		DrawTextureTile(Canvas, RailProgress, LeftRailX, RailY - 2.0f * RenderScale,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawTextureTile(Canvas, RailProgress, RightRailX,
			RailY - 2.0f * RenderScale, RailWidth, RailHeight, Accent);
	}
	else
	{
		DrawSolidTile(Canvas, LeftRailX, RailY, RailWidth, RailHeight, Navy);
		DrawSolidTile(Canvas, LeftRailX, RailY,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawSolidTile(Canvas, RightRailX, RailY, RailWidth, RailHeight, Accent);
	}

	const float ShieldWidth = 24.0f * RenderScale;
	const float ShieldHeight = 27.0f * RenderScale;
	DrawTextureTile(Canvas, LegacyShieldTexture,
		LeftRailX - 18.0f * RenderScale,
		ModuleCenterY - ShieldHeight * 0.5f,
		ShieldWidth, ShieldHeight, HealthGreen);

	// Gadget wings sit behind the circle in the original composition.
	DrawTextureTile(Canvas, LegacyLeftGadgetTexture,
		CenterX - 88.0f * RenderScale,
		ModuleCenterY - 16.5f * RenderScale,
		94.0f * RenderScale, 33.0f * RenderScale, FLinearColor::White);
	DrawTextureTile(Canvas, LegacyRightGadgetTexture,
		CenterX - 6.0f * RenderScale,
		ModuleCenterY - 16.5f * RenderScale,
		94.0f * RenderScale, 33.0f * RenderScale, FLinearColor::White);

	const float CircleX = CenterX - CircleWidth * 0.5f;
	const float CircleY = ModuleCenterY - CircleHeight * 0.5f;
	if (LegacyCircleTexture)
	{
		DrawTextureTile(Canvas, LegacyCircleTexture, CircleX, CircleY,
			CircleWidth, CircleHeight, FLinearColor::White);
		DrawTextureTile(Canvas, LegacyInnerCircleTexture,
			CenterX - 11.0f * RenderScale,
			ModuleCenterY - 11.0f * RenderScale,
			22.0f * RenderScale, 22.0f * RenderScale, FLinearColor::White);
	}
	else
	{
		DrawSolidTile(Canvas, CircleX, CircleY, CircleWidth, CircleHeight, Navy);
	}

	UTexture2D* RoleWeaponTexture = bActiveDefender
		? LegacyRocketTexture
		: LegacyInstaTexture;
	if (RoleWeaponTexture)
	{
		const float IconWidth = static_cast<float>(RoleWeaponTexture->GetSizeX())
			* RenderScale;
		const float IconHeight = static_cast<float>(RoleWeaponTexture->GetSizeY())
			* RenderScale;
		DrawTextureTile(Canvas, RoleWeaponTexture,
			CenterX - IconWidth * 0.5f,
			ModuleCenterY - IconHeight * 0.5f,
			IconWidth, IconHeight, FLinearColor::White);
	}

	const float TitleWidth = 226.0f * RenderScale;
	const float TitleHeight = 50.0f * RenderScale;
	const float TitleY = ModuleCenterY - 91.0f * RenderScale;
	if (bActiveAttacker && LegacyAttackingTexture)
	{
		DrawTextureTile(Canvas, LegacyAttackingTexture,
			CenterX - TitleWidth * 0.5f, TitleY,
			TitleWidth, TitleHeight, FLinearColor::White);
	}
	else
	{
		// The recovered attacker title is image art. ClutchMini's defender
		// title was plain white lettering; it did not use Wipeout's navy box.
		DrawCenteredCanvasText(Canvas, MediumFont, Title, CenterX,
			TitleY + 11.0f * RenderScale, 0.72f * RenderScale,
			FColor::White);
	}

	if (bActiveAttacker)
	{
		const int32 PipCount = FMath::Clamp<int32>(State->MaxAttackerHits, 1, 10);
		const int32 ArmorRemaining = State->GetEntryArmorRemaining(*Entry);
		const float PipWidth = 13.0f * RenderScale;
		const float PipHeight = 7.0f * RenderScale;
		const float PipGap = 3.0f * RenderScale;
		const float StartX = CenterX + 52.0f * RenderScale;
		for (int32 Index = 0; Index < PipCount; ++Index)
		{
			DrawSolidTile(Canvas,
				StartX + Index * (PipWidth + PipGap),
				RailY - 3.0f * RenderScale,
				PipWidth, PipHeight,
				Index < ArmorRemaining
					? Accent
					: FLinearColor(0.18f, 0.18f, 0.18f, 0.75f));
		}
	}
}


void AClutchHUD::DrawCapturePanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !State->IsGameplayPhase())
	{
		return;
	}

	AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
	const float Now = GameState
		? GameState->GetServerWorldTimeSeconds()
		: GetWorld()->GetTimeSeconds();
	const float RenderScale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float BarWidth = 300.0f * RenderScale;
	const float BarHeight = 6.0f * RenderScale;
	const float BarX = (Canvas->ClipX - BarWidth) * 0.5f;
	// Keep objective state above the recovered 50px role banner.
	const float BarY = Canvas->ClipY - 190.0f * RenderScale;
	const float Progress = FMath::Clamp(State->PoleProgress, 0.0f, 100.0f) / 100.0f;

	FString Label;
	FLinearColor Fill(1.0f, 0.55f, 0.08f, 1.0f);
	FColor LabelColor = FColor::White;
	int32 UnlockRemaining = INDEX_NONE;
	if (State->Phase == EClutchRoundPhase::Combat)
	{
		UnlockRemaining = FMath::Max(0,
			FMath::CeilToInt(State->PoleUnlockServerTime - Now));
		Label = FString::Printf(TEXT("POLE UNLOCKS IN %d"), UnlockRemaining);
		Fill = FLinearColor(0.35f, 0.38f, 0.43f, 1.0f);
	}
	else
	{
		const int32 Percent = FMath::RoundToInt(State->PoleProgress);
		switch (State->PoleActivity)
		{
		case EClutchPoleActivity::Capturing:
			Label = FString::Printf(TEXT("CAPTURING %d%%"), Percent);
			Fill = FLinearColor(0.20f, 0.95f, 0.28f, 1.0f);
			break;
		case EClutchPoleActivity::Contested:
			Label = FString::Printf(TEXT("CONTESTED %d%%"), Percent);
			Fill = FLinearColor(1.0f, 0.72f, 0.08f, 1.0f);
			LabelColor = FColor(255, 210, 70);
			break;
		case EClutchPoleActivity::Decaying:
			Label = FString::Printf(TEXT("DECAYING %d%%"), Percent);
			Fill = FLinearColor(1.0f, 0.18f, 0.06f, 1.0f);
			LabelColor = FColor(255, 95, 70);
			break;
		default:
			Label = FString::Printf(TEXT("POLE OPEN %d%%"), Percent);
			break;
		}
	}

	Canvas->SetLinearDrawColor(FLinearColor(0.01f, 0.015f, 0.025f, 0.88f));
	Canvas->DrawTile(Canvas->DefaultTexture, BarX - 2.0f * RenderScale,
		BarY - 2.0f * RenderScale, BarWidth + 4.0f * RenderScale,
		BarHeight + 4.0f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FLinearColor(0.12f, 0.12f, 0.14f, 1.0f));
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth, BarHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(Fill);
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth * Progress, BarHeight, 0, 0, 1, 1);
	DrawCenteredCanvasText(Canvas, SmallFont, Label, Canvas->ClipX * 0.5f,
		BarY - 17.0f * RenderScale, 0.52f * RenderScale, LabelColor);

	if (UnlockRemaining >= 1 && UnlockRemaining <= 5)
	{
		const float Pulse = 1.0f + 0.06f * FMath::Sin(Now * 8.0f);
		DrawCenteredCanvasText(Canvas, MediumFont ? MediumFont : SmallFont,
			FString::Printf(TEXT("POLE OPENS IN %d"), UnlockRemaining),
			Canvas->ClipX * 0.5f, BarY - 62.0f * RenderScale,
			0.82f * RenderScale * Pulse, FColor(255, 210, 70));
	}
}
