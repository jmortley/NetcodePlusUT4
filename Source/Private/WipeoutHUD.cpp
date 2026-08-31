// WipeoutHUD — FlagRun-style portrait strip for Wipeout game mode
#include "WipeoutHUD.h"
#include "NCPlusCTFGameMode.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "WipeoutScoreboard.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "NCPlusBetaTopBar.h"
#include "NCPlusHUDLayout.h"
#include "NCClutchOverlay.h"
#include "NCPlusForceModels.h"   // DrawHeadDebug (ncp.DebugHeads)
#include "UTHUDWidget_Spectator.h"
#include "WipeoutDamageReplicator.h"
#include "ClutchHUD.h"
#include "NCLeagueDuelHUD.h"
#include "NCShaftArenaHUD.h"
#include "ShockDomHUD.h"
#include "EngineUtils.h"

namespace
{
	// The pre-compact strip used a 57.6 x 82.29 design-pixel portrait. Match the
	// new 56px score core without changing the portrait atlas' 224:320 aspect.
	constexpr float WipeoutPortraitAspect = 320.f / 224.f;
	constexpr float WipeoutLegacyPipWidth = 32.f + (64.f * 0.4f);
	constexpr float WipeoutCompactPipHeight = 56.f;
	constexpr float WipeoutCompactPipWidth = WipeoutCompactPipHeight / WipeoutPortraitAspect;
	constexpr float WipeoutCompactTextFactor = WipeoutCompactPipHeight
		/ (WipeoutLegacyPipWidth * WipeoutPortraitAspect);

	void CountWipeoutPortraitPlayers(AUTGameState* GS,
		int32& OutTeam0Count, int32& OutTeam1Count)
	{
		OutTeam0Count = 0;
		OutTeam1Count = 0;
		if (GS == nullptr) return;

		for (APlayerState* PS : GS->PlayerArray)
		{
			AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
			if (UTPS == nullptr || UTPS->bOnlySpectator || UTPS->bIsInactive
				|| (UTPS->Team == nullptr && UTPS->GetTeamNum() == 255))
			{
				continue;
			}

			if (UTPS->GetTeamNum() == 0) ++OutTeam0Count;
			else if (UTPS->GetTeamNum() == 1) ++OutTeam1Count;
		}
	}

	bool IsWipeoutBetaTopBarSupported(const AWipeoutHUD* HUD)
	{
		return HUD != nullptr
			&& !HUD->IsA(AClutchHUD::StaticClass())
			&& !HUD->IsA(ANCLeagueDuelHUD::StaticClass())
			&& !HUD->IsA(ANCShaftArenaHUD::StaticClass())
			&& !HUD->IsA(AShockDomHUD::StaticClass());
	}

	// Advance from the prior deadline instead of Now so a 144/165/500 Hz render
	// cadence does not alias the nominal 120 Hz sampler down to 72/82.5/100 Hz.
	// Long stalls and clock discontinuities reset the phase rather than catching up.
	float AdvancePortraitSampleDeadline(float Now, float Deadline, float Interval)
	{
		if (Deadline <= 0.f || Deadline > Now || Now - Deadline > 4.f * Interval)
		{
			return Now + Interval;
		}
		const int32 PeriodsToSkip = FMath::FloorToInt((Now - Deadline) / Interval) + 1;
		return Deadline + float(PeriodsToSkip) * Interval;
	}
}

constexpr float AWipeoutHUD::PortraitSampleInterval;

AWipeoutHUD::AWipeoutHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Portrait background icons — identical UV coords to AUTFlagRunHUD
	RedTeamIcon.U = 5.f;
	RedTeamIcon.V = 5.f;
	RedTeamIcon.UL = 224.f;
	RedTeamIcon.VL = 310.f;
	RedTeamIcon.Texture = CharacterPortraitAtlas;

	BlueTeamIcon.U = 237.f;
	BlueTeamIcon.V = 5.f;
	BlueTeamIcon.UL = 224.f;
	BlueTeamIcon.VL = 310.f;
	BlueTeamIcon.Texture = CharacterPortraitAtlas;

	BlueTeamOverlay.U = 237.0f;
	BlueTeamOverlay.V = 330.0f;
	BlueTeamOverlay.UL = 224.0f;
	BlueTeamOverlay.VL = 310.0f;
	BlueTeamOverlay.Texture = CharacterPortraitAtlas;

	RedTeamOverlay.U = 5.0f;
	RedTeamOverlay.V = 330.0f;
	RedTeamOverlay.UL = 224.0f;
	RedTeamOverlay.VL = 310.0f;
	RedTeamOverlay.Texture = CharacterPortraitAtlas;

	RedPlayerCount = 0;
	BluePlayerCount = 0;

	// Since AWipeoutHUD is a new class with no DefaultGame.ini section,
	// RequiredHudWidgetClasses would be empty. Load all standard team-game
	// widgets via HudWidgetClasses (built after RequiredHudWidgetClasses in BeginPlay).
	// Widget list mirrors UTHUD_Showdown / UTFlagRunHUD but with our scoreboard.
	// Bottom bar: stock (familiar) vs NCPlus custom — chosen once at construction
	// (see FNCPlusHUDLayout::WantsStockBottomBar). Only ONE family is ever loaded, so
	// the two can never double-draw. Crosshair/powerups below are shared by both.
	if (FNCPlusHUDLayout::WantsStockBottomBar())
	{
		// Stock weapon bar + ammo + health/armor. The stock weapon bar self-reads the
		// player's HUD profile (orientation/scale/opacity/colors/group-remap).
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"));
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"));
		// Health/armor: Paperdoll AND QuickStats — BOTH, like every stock UT HUD
		// (DefaultGame.ini pairs them). They are mutually exclusive at runtime via the
		// profile's QuickStats flag: Paperdoll + WeaponInfo self-HIDE when the player's
		// profile has the QuickStats mini-HUD enabled (UTHUDWidget_Paperdoll.cpp:47,
		// UTHUDWidget_WeaponInfo.cpp:30) expecting bpHW_QuickStats to carry HP/armor/ammo.
		// Omitting QuickStats (the old comment had that gate INVERTED) meant a player
		// whose carried-over profile enables the mini-HUD saw NO HP/armor/ammo anywhere
		// (community fresh-install report 2026-07-01, worst in duel where portraits are
		// off too). Exactly one of the two families draws — no double-draw.
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Paperdoll.bpHW_Paperdoll_C"));
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_QuickStats.bpHW_QuickStats_C"));
	}
	else
	{
		// NCPlus custom split WeaponBar (two strips) + ammo counter + minimal HP/Armor.
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"));
	}
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"));
	// Removed bpHW_TeamGameClock — we draw our own team score bar in DrawHUD
	// that respects dynamic team colors from TeamSkins.
	// Held-pickup status (amp/berserk/siphon countdown + boot charges): keep the stock
	// powerups widget ONLY in stock-bottom-bar mode. In NCPlus mode we draw it ourselves
	// (NCPlusHUDDrawCall::DrawHeldPowerups) so it shows regardless of the player's MiniHUD
	// "Show Powerups" setting, which the NCPlus QuickStats replacement doesn't honor.
	if (FNCPlusHUDLayout::WantsStockBottomBar())
	{
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"));
	}
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Spectator"));
	// Optional opt-in accuracy widget — registered on every NetcodePlus HUD
	// (NCLeagueDuel, ShockDom, NCShaftArena, Wipeout itself) but defaults to
	// hidden because UNCPlusHUDWidget_Accuracy::ShouldDraw requires a layout
	// entry. User enables per-mode by dragging it onto the canvas in `nchud`.
	// NCShaftArenaHUD seeds the layout entry in BeginPlay so it's on by default
	// for that mode. Set the "weapon" extras key (current/linkgun/sniper/etc.)
	// to pin a specific weapon, otherwise it tracks the held weapon.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"));
	// Optional default-hidden overlays (speedometer, minimap, heal-bind icon).
	// All three gate visibility on the presence of a layout entry — instances
	// stay registered but ShouldDraw returns false until the user adds them
	// via nchud. heal_ability additionally short-circuits when the player has
	// no BoostClass (i.e. on modes that don't grant a heal/boost), so it's
	// safe to inherit into NCLeagueDuel/ShockDom via subclass without any
	// per-mode gating here.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Speedometer"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Minimap"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_HealAbility"));
	HudWidgetClasses.AddUnique(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_ReadyUp"));
	// Our custom portrait-row scoreboard
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
}

void AWipeoutHUD::BeginPlay()
{
	Super::BeginPlay();

	// HUD layout system — capture stock defaults before any override pass,
	// then load + apply the live layout. See ElimPlusHUD.cpp for full notes.
	CaptureWidgetDefaults(this);
	FNCPlusHUDLayout::ReloadLive();
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
}

bool AWipeoutHUD::ShouldDrawMinimap()
{
	// A live NCPlus entry owns minimap presentation even when layout-hidden.
	// Suppress only AUTHUD's post-widget stock renderer; bDrawMinimap remains the
	// real ToggleMinimap state for the custom widget and other stock HUD gates.
	if (FNCPlusHUDLayout::GetLive().Find(TEXT("minimap")) != nullptr)
	{
		return false;
	}
	return Super::ShouldDrawMinimap();
}

EInputMode::Type AWipeoutHUD::GetInputMode_Implementation() const
{
	// FIX: Mouse focus loss on death.
	//
	// When a player dies, the base UTHUD::GetInputMode_Implementation() checks
	// bOutOfLives and bOnlySpectator — if either is true (and scoreboard isn't
	// shown), it returns EIM_UIOnly, which releases mouse capture from the
	// viewport. This lets the cursor escape to other monitors/windows.
	//
	// In elimination/wipeout modes, dead players should spectate teammates with
	// full mouse capture until the round ends or they respawn. We force
	// EIM_GameOnly for the entire duration of an in-progress match, regardless
	// of life state. The base class handles all other match states normally
	// (intermission, end of match, warmup, etc.).
	//
	// This same pattern is used in the ElimPlus Blueprint HUD (BaseElm) and
	// in the engine's UTHUD_InstantReplay (which always returns EIM_GameOnly).
	// See also: UTHUD_Showdown::GetInputMode_Implementation() for a GameAndUI
	// variant used during spawn selection.
	//
	// Phase 4.0: drag overlay (nchud_drag) needs cursor freed for Slate input.
	// Checked first so it can override the in-match GameOnly forcing below.
	if (NCPlusHUDDragMode::IsActive())
	{
		return EInputMode::EIM_GameAndUI;
	}

	if (UTPlayerOwner != nullptr)
	{
		AUTPlayerState* PS = UTPlayerOwner->UTPlayerState;
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (PS && !PS->bOnlySpectator && GS && GS->GetMatchState() == MatchState::InProgress)
		{
			return EInputMode::EIM_GameOnly;
		}
	}
	return Super::GetInputMode_Implementation();
}

void AWipeoutHUD::NotifyMatchStateChange()
{
	Super::NotifyMatchStateChange();

	// Post-match screenshot moved to DrawHUD (NCPlusHUDDrawCall::ServicePostMatchScreenshot) — the old
	// "match-ended + 1.5s" timer fired DURING the instant replay (captured the replay, not the scoreboard).
	// The shared helper waits for the replay demo to finish.
}

void AWipeoutHUD::GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers)
{
	UWorld* World = GetWorld();
	AUTGameState* GS = World ? World->GetGameState<AUTGameState>() : nullptr;
	if (!GS)
	{
		if (&SortedPlayers == &PortraitRenderPlayers)
		{
			PortraitRenderPlayers.Reset();
		}
		CachedPortraitGameState = nullptr;
		CachedPortraitSignature.Reset();
		CachedPortraitRenderPlayers.Reset();
		return;
	}

	const bool bContextChanged = CachedPortraitWorld.Get() != World
		|| CachedPortraitGameState.Get() != GS;
	if (bContextChanged)
	{
		CachedPortraitWorld = World;
		CachedPortraitGameState = GS;
		CachedPortraitSignature.Reset();
		CachedPortraitRenderPlayers.Reset();
		PortraitRenderPlayers.Reset();
		PipCacheByPS.Reset();
		PresentationByPS.Reset();
		CachedPortraitScorer = nullptr;
		CachedPortraitPlayerArrayNum = INDEX_NONE;
		NextPortraitRosterSampleTime = 0.f;
		NextRemotePortraitSampleTime = 0.f;
	}
	auto RefreshRawPortraitPlayers = [this]()
	{
		PortraitRenderPlayers.Reset(CachedPortraitRenderPlayers.Num());
		for (const TWeakObjectPtr<AUTPlayerState>& WeakPlayerState : CachedPortraitRenderPlayers)
		{
			if (AUTPlayerState* PlayerState = WeakPlayerState.Get())
			{
				PortraitRenderPlayers.Add(PlayerState);
			}
		}
	};

	AUTPlayerState* HUDPS = GetScorerPlayerState();
	// Keep the presentation budget independent of pause/time dilation. World
	// time would turn 120 Hz into 12 Hz at 0.1x replay speed.
	const float Now = World->GetRealTimeSeconds();
	const bool bRosterInvalidated = bContextChanged
		|| CachedPortraitScorer.Get() != HUDPS
		|| CachedPortraitPlayerArrayNum != GS->PlayerArray.Num()
		|| Now >= NextPortraitRosterSampleTime;
	if (!bRosterInvalidated)
	{
		RefreshRawPortraitPlayers();
		if (&SortedPlayers != &PortraitRenderPlayers)
		{
			const bool bWasEmpty = SortedPlayers.Num() == 0;
			SortedPlayers.Append(PortraitRenderPlayers);
			if (!bWasEmpty)
			{
				SortedPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B)
				{
					return A.SelectionOrder > B.SelectionOrder;
				});
			}
		}
		return;
	}
	CachedPortraitScorer = HUDPS;
	CachedPortraitPlayerArrayNum = GS->PlayerArray.Num();
	NextPortraitRosterSampleTime = AdvancePortraitSampleDeadline(
		Now, NextPortraitRosterSampleTime, PortraitSampleInterval);
	CurrentPortraitSignature.Reset(GS->PlayerArray.Num());
	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		// Include players even if Team pointer is temporarily null (late replication).
		// GetTeamNum() returns a valid index from the replicated byte even before
		// the Team UObject pointer itself replicates. This prevents the "missing 8th
		// player" bug where the last joiner's Team arrives a few frames late.
		if (UTPS != nullptr && !UTPS->bOnlySpectator && !UTPS->bIsInactive
			&& (UTPS->Team != nullptr || UTPS->GetTeamNum() != 255))
		{
			const int32 SortKey = (UTPS == HUDPS) ? -1 : int32(UTPS->SpectatingIDTeam);
			UTPS->SelectionOrder = SortKey;
			FPortraitOrderSignature Signature;
			Signature.PlayerState = UTPS;
			Signature.TeamNum = UTPS->GetTeamNum();
			Signature.SortKey = SortKey;
			CurrentPortraitSignature.Add(Signature);
		}
	}

	bool bOrderChanged = bContextChanged
		|| CurrentPortraitSignature.Num() != CachedPortraitSignature.Num();
	if (!bOrderChanged)
	{
		for (int32 Index = 0; Index < CurrentPortraitSignature.Num(); ++Index)
		{
			const FPortraitOrderSignature& Current = CurrentPortraitSignature[Index];
			const FPortraitOrderSignature& Cached = CachedPortraitSignature[Index];
			if (Current.PlayerState.Get() != Cached.PlayerState.Get()
				|| Current.TeamNum != Cached.TeamNum
				|| Current.SortKey != Cached.SortKey)
			{
				bOrderChanged = true;
				break;
			}
		}
	}

	if (bOrderChanged)
	{
		CachedPortraitSignature = CurrentPortraitSignature;
		PortraitRenderPlayers.Reset(CurrentPortraitSignature.Num());
		for (const FPortraitOrderSignature& Signature : CurrentPortraitSignature)
		{
			if (AUTPlayerState* PlayerState = Signature.PlayerState.Get())
			{
				PortraitRenderPlayers.Add(PlayerState);
			}
		}
		PortraitRenderPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B)
		{
			return A.SelectionOrder > B.SelectionOrder;
		});
		CachedPortraitRenderPlayers.Reset(PortraitRenderPlayers.Num());
		for (AUTPlayerState* PlayerState : PortraitRenderPlayers)
		{
			CachedPortraitRenderPlayers.Add(PlayerState);
		}

		for (auto It = PipCacheByPS.CreateIterator(); It; ++It)
		{
			bool bStillPresent = false;
			for (const FPortraitOrderSignature& Signature : CurrentPortraitSignature)
			{
				if (Signature.PlayerState.Get() == It.Key().Get())
				{
					bStillPresent = true;
					break;
				}
			}
			if (!bStillPresent)
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = PresentationByPS.CreateIterator(); It; ++It)
		{
			bool bStillPresent = false;
			for (const FPortraitOrderSignature& Signature : CurrentPortraitSignature)
			{
				if (Signature.PlayerState.Get() == It.Key().Get())
				{
					bStillPresent = true;
					break;
				}
			}
			if (!bStillPresent)
			{
				It.RemoveCurrent();
			}
		}
	}
	else
	{
		RefreshRawPortraitPlayers();
	}

	if (&SortedPlayers != &PortraitRenderPlayers)
	{
		const bool bWasEmpty = SortedPlayers.Num() == 0;
		SortedPlayers.Append(PortraitRenderPlayers);
		if (!bWasEmpty)
		{
			SortedPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B)
			{
				return A.SelectionOrder > B.SelectionOrder;
			});
		}
	}
}

// "NOW WATCHING <player>" spectator banner (verbatim port of ANCPlusCTFHUD::
// DrawSpectatorTarget). Bottom-right, suppressed by the caller when the
// scoreboard is up. This is the canonical viewed-player banner for both dead
// players and true spectators; DrawHUD suppresses the stock duplicate only for
// the frame where this banner can replace it.
AWipeoutDamageReplicator* AWipeoutHUD::FindDamageReplicator(UWorld* World)
{
	if (!World) return nullptr;
	AUTGameState* CurrentGS = World->GetGameState<AUTGameState>();
	if (CachedDamageReplicatorWorld.Get() != World
		|| CachedDamageReplicatorGameState.Get() != CurrentGS)
	{
		CachedDamageReplicatorWorld = World;
		CachedDamageReplicatorGameState = CurrentGS;
		CachedDamageReplicator = nullptr;
		NextDamageReplicatorRetryTime = 0.f;
	}
	if (CachedDamageReplicator.IsValid())
	{
		return CachedDamageReplicator.Get();
	}

	const float Now = World->GetTimeSeconds();
	if (Now < NextDamageReplicatorRetryTime)
	{
		return nullptr;
	}
	for (TActorIterator<AWipeoutDamageReplicator> It(World); It; ++It)
	{
		CachedDamageReplicator = *It;
		NextDamageReplicatorRetryTime = 0.f;
		return *It;
	}

	CachedDamageReplicator = nullptr;
	NextDamageReplicatorRetryTime = Now + 1.f;
	return nullptr;
}

void AWipeoutHUD::DrawSpectatorTarget()
{
	if (!Canvas || !MediumFont || !SmallFont) return;
	if (!UTPlayerOwner) return;

	AActor* ViewTarget = UTPlayerOwner->GetViewTarget();
	if (!ViewTarget || ViewTarget == UTPlayerOwner) return;

	APawn* ViewPawn = Cast<APawn>(ViewTarget);
	if (!ViewPawn) return;
	if (ViewPawn == UTPlayerOwner->GetPawn()) return;   // own pawn = playing

	AUTPlayerState* PS = Cast<AUTPlayerState>(ViewPawn->PlayerState);
	if (!PS || PS->PlayerName.IsEmpty()) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	const float HeaderScale = RenderScale * 0.75f;
	const float NameScale   = RenderScale * 1.30f;

	static const FString HeaderText(TEXT("NOW WATCHING"));
	const FString& NameText = PS->PlayerName;

	FText HeaderDrawText, NameDrawText;
	float HeaderW, HeaderH, NameW, NameH;
	NCPlusHUDDrawCall::ResolveStableText(Canvas, SmallFont, HeaderText, HeaderScale, HeaderScale, HeaderDrawText, HeaderW, HeaderH);
	NCPlusHUDDrawCall::ResolveStableText(Canvas, MediumFont, NameText, NameScale, NameScale, NameDrawText, NameW, NameH);

	const float PadX = 16.f * RenderScale;
	const float PadY = 8.f  * RenderScale;
	const float Gap  = 4.f  * RenderScale;
	const float PanelW = FMath::Max(HeaderW, NameW) + PadX * 2.f;
	const float PanelH = HeaderH + NameH + PadY * 2.f + Gap;
	const float PanelX = Canvas->ClipX - PanelW - 24.f * RenderScale;
	const float PanelY = Canvas->ClipY - PanelH - 140.f * RenderScale;

	Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.7f));
	Canvas->DrawTile(Canvas->DefaultTexture, PanelX, PanelY, PanelW, PanelH, 0, 0, 1, 1);

	FLinearColor AccentColor(0.9f, 0.9f, 0.9f, 1.f);
	if (PS->Team)
	{
		AccentColor = (PS->Team->TeamIndex == 0)
			? FLinearColor(0.9f, 0.15f, 0.15f, 1.f)
			: FLinearColor(0.15f, 0.4f, 0.95f, 1.f);
	}
	Canvas->SetLinearDrawColor(AccentColor);
	Canvas->DrawTile(Canvas->DefaultTexture, PanelX, PanelY, 3.f * RenderScale, PanelH, 0, 0, 1, 1);

	Canvas->DrawColor = FColor(180, 180, 180, 255);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, SmallFont, HeaderDrawText,
		PanelX + (PanelW - HeaderW) * 0.5f, PanelY + PadY, HeaderScale, HeaderScale, Canvas->DrawColor);

	Canvas->DrawColor = AccentColor.ToFColor(true);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, NameDrawText,
		PanelX + (PanelW - NameW) * 0.5f, PanelY + PadY + HeaderH + Gap, NameScale, NameScale, Canvas->DrawColor);
}

void AWipeoutHUD::DrawHUD()
{
	// Re-apply the live layout each frame so Slate editor edits show up immediately.
	// Cheap when clean (dirty-flag gated).
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
	const bool bRenderCustomHUD = bShowUTHUD && UTPlayerOwner
		&& (bShowHUD || !UTPlayerOwner->bCinematicMode);

	// True spectators normally get UUTHUDWidget_Spectator's bottom-right "Now viewing"
	// panel as well as our compact banner. Suppress that stock panel only during an
	// in-progress frame where our banner has a valid player pawn to draw. Restore its
	// prior hidden state immediately after Super so warmup/respawn/end-state messages,
	// scoreboard behaviour, and the user's nchud visibility choice remain stock-owned.
	bool bRestoreStockSpectator = false;
	bool bStockSpectatorWasHidden = false;
	if (bRenderCustomHUD && SpectatorMessageWidget && UTPlayerOwner->UTPlayerState
		&& UTPlayerOwner->UTPlayerState->bOnlySpectator && !ScoreboardIsUp())
	{
		AUTGameState* PreDrawGS = GetWorld()->GetGameState<AUTGameState>();
		APawn* ViewedPawn = Cast<APawn>(UTPlayerOwner->GetViewTarget());
		AUTPlayerState* ViewedPS = ViewedPawn ? Cast<AUTPlayerState>(ViewedPawn->PlayerState) : nullptr;
		if (PreDrawGS && PreDrawGS->GetMatchState() == MatchState::InProgress
			&& ViewedPawn != UTPlayerOwner->GetPawn() && ViewedPS && !ViewedPS->PlayerName.IsEmpty())
		{
			bRestoreStockSpectator = true;
			bStockSpectatorWasHidden = SpectatorMessageWidget->IsHidden();
			SpectatorMessageWidget->SetHidden(true);
		}
	}

	Super::DrawHUD();

	if (bRestoreStockSpectator)
	{
		SpectatorMessageWidget->SetHidden(bStockSpectatorWasHidden);
	}

	// Auto post-match screenshot (shared; waits for the instant replay to end + the scoreboard to settle).
	NCPlusHUDDrawCall::ServicePostMatchScreenshot(this, PostMatchScreenshotStable, bPostMatchScreenshotTaken);
	if (!bRenderCustomHUD) return;

	// Guard: Canvas or fonts may be null during Slate UI overlays (e.g. weapon skins menu)
	if (!Canvas || !SmallFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	bool bScoreboardIsUp = ScoreboardIsUp();

	// Head-hitbox calibration (cvar `ncp.DebugHeads 1`): GREEN ring = the capsule headshot sphere the server
	// validates, RED cross = the mesh head bone. Warmup-only in NETWORKED play (anti head-ESP) but ALWAYS in
	// standalone/offline so you can calibrate in a live single-player match (you're the host -> cvar drives both).
	if (GS && (GS->GetMatchState() == MatchState::WaitingToStart || GetWorld()->GetNetMode() == NM_Standalone))
	{
		NCPlusForceModels::DrawHeadDebug(Canvas, PlayerOwner);
	}

	// ─── Custom team score bar (replaces bpHW_TeamGameClock) ───
	// Respects dynamic team colors from TeamSkins mutator.
	// Wipeout always uses the portrait top bar: its per-player respawn sweep and
	// countdown are gameplay information the stock Elim-style roster cannot show.
	// ElimPlusHUD continues to honor the user's Stock Team Panel preference.
	if (GS && !bScoreboardIsUp)
	{
		DrawTeamScoreBar(GS);
		// NOW WATCHING banner — self-guards when not spectating another pawn.
		DrawSpectatorTarget();
		if (AWipeoutDamageReplicator* DamageRep = FindDamageReplicator(GetWorld()))
		{
			NCClutchOverlay::Draw(this, Canvas,
				DamageRep->Team0ClutchOverlay, DamageRep->Team1ClutchOverlay);
		}
	}

	// Keep portraits up through the round-win window ("RoundCooldown") so the
	// enemy team's final health stays visible after they win — not just InProgress.
	if (bShouldDrawPortraits && !bScoreboardIsUp && GS
		&& (GS->GetMatchState() == MatchState::InProgress
			|| GS->GetMatchState() == FName(TEXT("RoundCooldown"))))
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		// Match the F5 layout's 1080p design-pixel scale (and the compact score
		// module) so portrait proportions remain stable on non-16:9 viewports.
		const float RenderScale = float(Canvas->SizeY) / 1080.0f;

		// Restyled Wipeout/Shaft cards now share the score core's 56 design-pixel
		// height. The legacy width remains available for any base-HUD subclass that
		// explicitly opts out, while each F5 portrait Scale still multiplies below.
		const float BasePipDesignWidth = bPortraitMockupRestyle
			? WipeoutCompactPipWidth : WipeoutLegacyPipWidth;
		float BasePipSize = BasePipDesignWidth * GetHUDWidgetScaleOverride() * RenderScale;
		// Strip aliases: portrait_red/_blue by default, or portrait_team/_enemy when
		// ViewerRelativePortraits is on — placement then follows the viewer's team
		// instead of a fixed color (PortraitAliasFor).
		const FName RedAlias  = PortraitAliasFor(0);
		const FName BlueAlias = PortraitAliasFor(1);
		static const FName NAME_PortraitEnemyStrip(TEXT("portrait_enemy"));
		// Phase 3.11: per-strip Scale override (independent resize).
		const float WO_RedScale  = NCPlusHUDDrawCall::GetScale(RedAlias);
		const float WO_BlueScale = NCPlusHUDDrawCall::GetScale(BlueAlias);
		float WO_RedPipSize  = BasePipSize * WO_RedScale;
		float WO_BluePipSize = BasePipSize * WO_BlueScale;
		const bool bRedLeft     = (RedAlias != NAME_PortraitEnemyStrip);
		const bool bHideRed  = NCPlusHUDDrawCall::IsHidden(RedAlias);
		const bool bHideBlue = NCPlusHUDDrawCall::IsHidden(BlueAlias);

		FVector2D RedStart;
		FVector2D BlueStart;
		float RedGrowSign  = bRedLeft ? -1.f : +1.f;
		float BlueGrowSign = bRedLeft ? +1.f : -1.f;
		FNCPlusBetaTopBarGeometry BetaGeometry;
		bool bHasBetaGeometry = false;
		if (IsWipeoutBetaTopBarSupported(this) && FNCPlusHUDLayout::WantsBetaTopBar())
		{
			int32 TeamCounts[2] = { 0, 0 };
			CountWipeoutPortraitPlayers(GS, TeamCounts[0], TeamCounts[1]);
			if (bHideRed) TeamCounts[0] = 0;
			if (bHideBlue) TeamCounts[1] = 0;
			const int32 LeftTeamIndex = bRedLeft ? 0 : 1;
			const int32 RightTeamIndex = bRedLeft ? 1 : 0;
			bHasBetaGeometry = NCPlusBetaTopBar::BuildGeometry(this, Canvas,
				TeamCounts[LeftTeamIndex], TeamCounts[RightTeamIndex], BetaGeometry);
		}

		if (bHasBetaGeometry)
		{
			// The shared anchors describe the inside edge of each row. Reusing the
			// existing left-grow adjustment below keeps every portrait overlay,
			// countdown, dead-X, next-spawn border, and join animation unchanged.
			WO_RedPipSize = BetaGeometry.PortraitWidth;
			WO_BluePipSize = BetaGeometry.PortraitWidth;
			RedStart = FVector2D(
				bRedLeft ? BetaGeometry.LeftPortraitAnchorX : BetaGeometry.RightPortraitAnchorX,
				BetaGeometry.PortraitY);
			BlueStart = FVector2D(
				bRedLeft ? BetaGeometry.RightPortraitAnchorX : BetaGeometry.LeftPortraitAnchorX,
				BetaGeometry.PortraitY);
		}
		else
		{
			const float XAdjust = BasePipSize * 1.1f;

			// Stock positions used as fallbacks if the layout has no override. The
			// LEFT slot belongs to red — or, viewer-relative, to MY team — so the
			// enemy-keyed strip falls back right regardless of its color.
			const float StockXLeft  = 0.4f * Canvas->ClipX - XAdjust - BasePipSize;
			const float StockXRight = 0.6f * Canvas->ClipX + XAdjust;
			const float StockY      = 0.005f * Canvas->ClipY * GetHUDWidgetScaleOverride() * RenderScale;
			const float StockXRed   = bRedLeft ? StockXLeft  : StockXRight;
			const float StockXBlue  = bRedLeft ? StockXRight : StockXLeft;

			// Layout consult (Phase 3.5) — user can move each strip independently.
			RedStart = NCPlusHUDDrawCall::ResolveScreenPos(RedAlias, Canvas, FVector2D(StockXRed, StockY));
			BlueStart = NCPlusHUDDrawCall::ResolveScreenPos(BlueAlias, Canvas, FVector2D(StockXBlue, StockY));

			// Per-strip grow direction — derived from RESOLVED screen position, not
			// the anchor coordinate. Lets dragging the strip across screen-center
			// (via nchud_drag) auto-flip growth so the visible strip stays on
			// screen instead of extending off the now-far edge.
			// Side-defaulted grow direction (the left-side strip grows leftward);
			// flip only if the default would clip off-screen. See ElimPlusHUD for
			// full rationale.
			const float EstRedStripWidth = 5.f * 1.1f * WO_RedPipSize;
			const float EstBlueStripWidth = 5.f * 1.1f * WO_BluePipSize;
			if (RedGrowSign < 0.f && RedStart.X - EstRedStripWidth < 0.f) RedGrowSign = +1.f;
			else if (RedGrowSign > 0.f && RedStart.X + EstRedStripWidth > Canvas->ClipX) RedGrowSign = -1.f;
			if (BlueGrowSign < 0.f && BlueStart.X - EstBlueStripWidth < 0.f) BlueGrowSign = +1.f;
			else if (BlueGrowSign > 0.f && BlueStart.X + EstBlueStripWidth > Canvas->ClipX) BlueGrowSign = -1.f;
		}

		float XOffsetRed  = RedStart.X;
		float XOffsetBlue = BlueStart.X;
		// When growing leftward, the resolved point is the strip's RIGHT edge —
		// shift the first pip left by its width so its right edge sits at the
		// anchor instead of extending off-screen.
		if (RedGrowSign  < 0.f) XOffsetRed  -= WO_RedPipSize;
		if (BlueGrowSign < 0.f) XOffsetBlue -= WO_BluePipSize;
		float YOffsetRed  = RedStart.Y;
		float YOffsetBlue = BlueStart.Y;
		float YOffset     = YOffsetRed;  // legacy single-Y for code that doesn't yet split (kept for safety)

		GetPlayerListForIcons(PortraitRenderPlayers);
		const TArray<AUTPlayerState*>& LivePlayers = PortraitRenderPlayers;
		AUTPlayerState* LocalPortraitPS = Cast<AUTPlayerState>(UTPlayerOwner ? UTPlayerOwner->PlayerState : nullptr);
		const float PortraitNow = GetWorld()->GetRealTimeSeconds();
		const bool bSampleRemotePresentation = PortraitNow >= NextRemotePortraitSampleTime;
		if (bSampleRemotePresentation)
		{
			NextRemotePortraitSampleTime = AdvancePortraitSampleDeadline(
				PortraitNow, NextRemotePortraitSampleTime, PortraitSampleInterval);
		}
		for (AUTPlayerState* UTPS : LivePlayers)
		{
			if (!UTPS) continue;
			FWipeoutPresentationSample* Existing = PresentationByPS.Find(UTPS);
			if (UTPS != LocalPortraitPS && !bSampleRemotePresentation && Existing) continue;
			const bool bHadExistingSample = Existing != nullptr;

			FWipeoutPresentationSample& Sample = PresentationByPS.FindOrAdd(UTPS);
			AController* Controller = Cast<AController>(UTPS->GetOwner());
			AUTCharacter* Character = Controller
				? Cast<AUTCharacter>(Controller->GetPawn()) : nullptr;
			if (!Character && (bSampleRemotePresentation || !bHadExistingSample))
			{
				// Preserve stock's cached-character/pawn-scan fallback during
				// possession and replication gaps, but only at the scheduled rate.
				Character = UTPS->GetUTCharacter();
			}
			else if (!Character && bHadExistingSample)
			{
				// Avoid a pawn-list scan every rendered frame while the local
				// player is dead or between possessions. Retain the last coherent
				// sample until the next 120 Hz presentation refresh.
				continue;
			}
			Sample.Character = Character;
			Sample.bAlive = Character != nullptr && !Character->IsDead();
			Sample.Health = Character ? Character->Health : 0;
			Sample.Armor = Character ? Character->GetArmorAmount() : 0;
		}

		// Pre-pass: find the next-to-spawn teammate (lowest RespawnTime > 0)
		AUTPlayerState* MyPS_ForSpawn = LocalPortraitPS;
		uint8 MyTeam = MyPS_ForSpawn ? MyPS_ForSpawn->GetTeamNum() : 255;
		AUTPlayerState* NextToSpawn = nullptr;
		float LowestRespawnTime = BIG_NUMBER;
		for (AUTPlayerState* UTPS : LivePlayers)
		{
			if (UTPS && UTPS != MyPS_ForSpawn && UTPS->GetTeamNum() == MyTeam
				&& UTPS->RespawnWaitTime > 0.f && UTPS->RespawnTime > 0.f
				&& UTPS->RespawnTime < LowestRespawnTime)
			{
				const FWipeoutPresentationSample* Sample = PresentationByPS.Find(UTPS);
				const bool bDead = !Sample || !Sample->bAlive;
				if (bDead)
				{
					LowestRespawnTime = UTPS->RespawnTime;
					NextToSpawn = UTPS;
				}
			}
		}

		struct FWipeoutStripDraw
		{
			AUTPlayerState* PlayerState = nullptr;
			FWipeoutPresentationSample Sample;
			float LiveScaling = 0.f;
			float X = 0.f;
			float Y = 0.f;
			float PipSize = 0.f;
			FName Alias;
			UFont* NameFont = nullptr;
			FText NameText;
			float NameScale = 1.f;
			float NameWidth = 0.f;
			float NameHeight = 0.f;
			bool bNextToSpawn = false;
			bool bJoinAnimating = false;
		};
		// Avoid render-rate heap churn on servers larger than the usual 5v5 roster.
		TArray<FWipeoutStripDraw, TInlineAllocator<32>> PortraitDraws;
		PortraitDraws.Reserve(LivePlayers.Num());

		for (AUTPlayerState* UTPS : LivePlayers)
		{
			if (!UTPS) continue;
			// In Wipeout everyone respawns, so show all non-spectator players
			// Keep every portrait the same size. The old scorer/view-target bump made
			// the row jump and opened uneven gaps whenever spectating changed players.
			// Per-team Scale: red strip honors portrait_red.Scale, blue honors blue.
			const uint8 PreTeamIdx = UTPS ? UTPS->GetTeamNum() : 255;
			const float TeamPipBase = (PreTeamIdx == 1) ? WO_BluePipSize : WO_RedPipSize;
			const float PipSize = TeamPipBase;

			const FWipeoutPresentationSample* Sample = PresentationByPS.Find(UTPS);
			const bool bPlayerAlive = Sample && Sample->bAlive;

			// Respawn progress: 0 = fully dead/waiting, 1 = alive
			float LiveScaling = 1.f;
			const bool bReachableRespawn = !bPlayerAlive
				&& UTPS->RespawnTime > 0.f && UTPS->RespawnWaitTime > 0.f;
			if (bReachableRespawn)
			{
				LiveScaling = FMath::Clamp(1.f - UTPS->RespawnTime / UTPS->RespawnWaitTime, 0.f, 1.f);
			}
			else if (!bPlayerAlive)
			{
				// Any other dead state: pre-timer (no RespawnWaitTime yet) or
				// sudden-death/OT (bOutOfLives + RespawnTime = 0). Always dim.
				LiveScaling = 0.f;
			}

			// Use GetTeamNum() which works even if Team pointer is null
			uint8 TeamIdx = UTPS->GetTeamNum();
			// Phase 3.5 hide gates — skip drawing the strip the user disabled.
			if (TeamIdx == 0 && bHideRed)  continue;
			if (TeamIdx == 1 && bHideBlue) continue;
			float* TeamX = nullptr;
			float TeamY = 0.f;
			float GrowSign = 1.f;
			FName Alias;
			float TeamScale = 1.f;
			if (TeamIdx == 0)
			{
				RedPlayerCount++;
				TeamX = &XOffsetRed; TeamY = YOffsetRed; GrowSign = RedGrowSign;
				Alias = RedAlias; TeamScale = WO_RedScale;
			}
			else if (TeamIdx == 1)
			{
				BluePlayerCount++;
				TeamX = &XOffsetBlue; TeamY = YOffsetBlue; GrowSign = BlueGrowSign;
				Alias = BlueAlias; TeamScale = WO_BlueScale;
			}
			if (!TeamX) continue;

			FWipeoutStripDraw& Draw = PortraitDraws[PortraitDraws.AddDefaulted()];
			Draw.PlayerState = UTPS;
			if (Sample) Draw.Sample = *Sample;
			Draw.LiveScaling = LiveScaling;
			Draw.X = *TeamX;
			Draw.Y = TeamY;
			Draw.PipSize = PipSize;
			Draw.Alias = Alias;
			Draw.bNextToSpawn = UTPS == NextToSpawn;
			Draw.bJoinAnimating = PortraitNow - UTPS->CreationTime < 1.f;
			Draw.NameFont = NCPlusHUDFonts::Resolve(Alias, this, SmallFont);
			if (!Draw.NameFont) Draw.NameFont = SmallFont;
			const float NameFontExtra = NCPlusHUDFonts::ResolveScale(Alias, 1.f);
			Draw.NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f * TeamScale * NameFontExtra;
			NCPlusHUDDrawCall::ResolveFittedName(Canvas, UTPS, Draw.NameFont, UTPS->PlayerName,
				PipSize, Draw.NameScale, Draw.NameText, Draw.NameWidth, Draw.NameHeight);
			*TeamX += GrowSign * (bHasBetaGeometry
				? BetaGeometry.PortraitPitch : 1.1f * PipSize);
		}

		auto DrawPortraitFinish = [this](const FWipeoutStripDraw& Draw)
		{
			const float NameX = Draw.X + 0.5f * (Draw.PipSize - Draw.NameWidth * Draw.NameScale);
			const float NameY = Draw.Y + Draw.PipSize * WipeoutPortraitAspect + 2.f;
			NCPlusHUDDrawCall::DrawOutlinedText(Canvas, Draw.NameFont, Draw.NameText,
				NameX, NameY, Draw.NameScale, FLinearColor::White);
			if (Draw.bNextToSpawn)
			{
				const float PipHeight = Draw.PipSize * WipeoutPortraitAspect;
				const float BorderW = 2.f;
				Canvas->SetLinearDrawColor(FLinearColor(1.f, 0.85f, 0.f, 0.9f));
				Canvas->DrawTile(Canvas->DefaultTexture, Draw.X, Draw.Y, Draw.PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, Draw.X, Draw.Y + PipHeight - BorderW, Draw.PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, Draw.X, Draw.Y, BorderW, PipHeight, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, Draw.X + Draw.PipSize - BorderW, Draw.Y, BorderW, PipHeight, 0, 0, 1, 1);
			}
		};
		// The multipass implementation is deliberately non-virtual. Restrict it to
		// exact in-repo HUD classes known to use the base renderer so an external
		// native subclass overriding DrawPlayerIcon always gets the complete-card path.
		bool bCanBatch = GetClass() == AWipeoutHUD::StaticClass()
			|| GetClass() == ANCShaftArenaHUD::StaticClass();
		for (const FWipeoutStripDraw& Draw : PortraitDraws) bCanBatch &= !Draw.bJoinAnimating;
		for (int32 A = 0; bCanBatch && A < PortraitDraws.Num(); ++A)
		{
			const FWipeoutStripDraw& DA = PortraitDraws[A];
			for (int32 B = A + 1; B < PortraitDraws.Num(); ++B)
			{
				const FWipeoutStripDraw& DB = PortraitDraws[B];
				const float DANameX = DA.X + 0.5f * (DA.PipSize - DA.NameWidth * DA.NameScale);
				const float DBNameX = DB.X + 0.5f * (DB.PipSize - DB.NameWidth * DB.NameScale);
				const float DANameY = DA.Y + DA.PipSize * WipeoutPortraitAspect + 2.f;
				const float DBNameY = DB.Y + DB.PipSize * WipeoutPortraitAspect + 2.f;
				const float TextPadding = 2.f;
				const float DAMinX = FMath::Min(DA.X, DANameX - TextPadding);
				const float DBMinX = FMath::Min(DB.X, DBNameX - TextPadding);
				const float DAMaxX = FMath::Max(DA.X + DA.PipSize,
					DANameX + DA.NameWidth * DA.NameScale + TextPadding);
				const float DBMaxX = FMath::Max(DB.X + DB.PipSize,
					DBNameX + DB.NameWidth * DB.NameScale + TextPadding);
				const float DAMinY = FMath::Min(DA.Y, DANameY - TextPadding);
				const float DBMinY = FMath::Min(DB.Y, DBNameY - TextPadding);
				const float DAMaxY = FMath::Max(DA.Y + DA.PipSize * WipeoutPortraitAspect,
					DANameY + DA.NameHeight * DA.NameScale + TextPadding);
				const float DBMaxY = FMath::Max(DB.Y + DB.PipSize * WipeoutPortraitAspect,
					DBNameY + DB.NameHeight * DB.NameScale + TextPadding);
				const bool bOverlap = DAMinX < DBMaxX && DBMinX < DAMaxX
					&& DAMinY < DBMaxY && DBMinY < DAMaxY;
				if (bOverlap) { bCanBatch = false; break; }
			}
		}
		if (bCanBatch)
		{
			PortraitPassStateByPS.Reset();
			for (ActivePortraitDrawPass = 0; ActivePortraitDrawPass < 6; ++ActivePortraitDrawPass)
			{
				for (const FWipeoutStripDraw& Draw : PortraitDraws)
				{
					PreparedPortraitPS = Draw.PlayerState;
					PreparedPortraitSample = Draw.Sample;
					bHasPreparedPortraitSample = true;
					DrawPlayerIconPass(Draw.PlayerState, Draw.LiveScaling,
						Draw.X, Draw.Y, Draw.PipSize);
				}
			}
			ActivePortraitDrawPass = -1;
			PortraitPassStateByPS.Reset();
			bHasPreparedPortraitSample = false;
			for (const FWipeoutStripDraw& Draw : PortraitDraws) DrawPortraitFinish(Draw);
		}
		else
		{
			for (const FWipeoutStripDraw& Draw : PortraitDraws)
			{
				PreparedPortraitPS = Draw.PlayerState;
				PreparedPortraitSample = Draw.Sample;
				bHasPreparedPortraitSample = true;
				DrawPlayerIcon(Draw.PlayerState, Draw.LiveScaling, Draw.X, Draw.Y, Draw.PipSize);
				DrawPortraitFinish(Draw);
			}
			bHasPreparedPortraitSample = false;
		}
		// ─── Score / KDA mini widget (top right) ───
		// Layout-aware via "score_kda" alias. Position, scale, and font are
		// nchud-overridable; layout scale multiplies into FontScale so editor
		// resizing actually affects rendered text.
		AUTPlayerState* MyPS = GetScorerPlayerState();
		if (MyPS && Canvas && SmallFont && !NCPlusHUDDrawCall::IsHidden(TEXT("score_kda")))
		{
			int32 Score = FMath::TruncToInt(MyPS->Score);
			int32 Kills = MyPS->Kills;
			int32 Deaths = MyPS->Deaths;
			int32 Assists = MyPS->KillAssists;

			static TWeakObjectPtr<AUTPlayerState> CachedKdaPS;
			static int32 CachedScore = MAX_int32, CachedKills = MAX_int32;
			static int32 CachedDeaths = MAX_int32, CachedAssists = MAX_int32;
			static FString ScoreStr, KDAStr;
			if (CachedKdaPS.Get() != MyPS || CachedScore != Score || CachedKills != Kills
				|| CachedDeaths != Deaths || CachedAssists != Assists)
			{
				CachedKdaPS = MyPS;
				CachedScore = Score; CachedKills = Kills; CachedDeaths = Deaths; CachedAssists = Assists;
				ScoreStr = FString::Printf(TEXT("Score: %d"), Score);
				KDAStr = FString::Printf(TEXT("KDA: %d / %d / %d"), Kills, Deaths, Assists);
			}

			const FVector2D StockPos(Canvas->ClipX * 0.98f, Canvas->ClipY * 0.015f);
			const FVector2D ResolvedPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("score_kda"), Canvas, StockPos);
			const float ElemScale = NCPlusHUDDrawCall::GetScale(TEXT("score_kda"));
			const float FontExtra = NCPlusHUDFonts::ResolveScale(TEXT("score_kda"), 1.f);
			float FontScale = RenderScale * 0.9f * ElemScale * FontExtra;

			UFont* KDAFont = NCPlusHUDFonts::Resolve(TEXT("score_kda"), this, SmallFont);
			if (!KDAFont) KDAFont = SmallFont;

			float KDAXPos = ResolvedPos.X;
			float KDAYPos = ResolvedPos.Y;

			// Score line
			FText ResolvedText;
			float XL, YL;
			NCPlusHUDDrawCall::ResolveStableText(Canvas, KDAFont, ScoreStr, FontScale, FontScale, ResolvedText, XL, YL);
			const float KdaOp = NCPlusHUDDrawCall::GetOpacity(TEXT("score_kda"));
			Canvas->DrawColor = FColor(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(220.f * KdaOp), 0, 255));
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, KDAFont, ResolvedText, KDAXPos - XL, KDAYPos, FontScale, FontScale, Canvas->DrawColor);
			KDAYPos += YL * 1.1f;

			// KDA line
			NCPlusHUDDrawCall::ResolveStableText(Canvas, KDAFont, KDAStr, FontScale, FontScale, ResolvedText, XL, YL);
			Canvas->DrawColor = FColor(200, 200, 200, (uint8)FMath::Clamp(FMath::RoundToInt(200.f * KdaOp), 0, 255));
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, KDAFont, ResolvedText, KDAXPos - XL, KDAYPos, FontScale, FontScale, Canvas->DrawColor);
		}
	}

	// Held-pickup status (amp/berserk/siphon countdown + boot charges) — NCPlus mode only.
	// Covers Wipeout + (via inheritance) NCLeagueDuel and NCShaftArena HUDs.
	NCPlusHUDDrawCall::DrawHeldPowerups(this, Canvas);

	// Optional opt-in overlays. Both default OFF (no layout entry = no draw).
	// server_info first so the damage flash tints over it; flash must be LAST so
	// it covers every HUD pixel.
	NCPlusHUDDrawCall::DrawServerInfo(this, Canvas);
	NCPlusHUDDrawCall::DrawDamageFlash(this, Canvas);
}

// ─── Custom team score bar ─────────────────────────────────────────────
// Compact score/clock module shared by Wipeout, Duel, Shaft Arena and ShockDom.
// It deliberately carries no mode label: the inherited HUDs do not all represent
// Wipeout, while the scorebar alias and team-color semantics are shared by them.
int32 AWipeoutHUD::ResolveBetaTopBarClockSeconds(AUTGameState* GS)
{
	if (GS == nullptr) return -1;

	int32 ClockSeconds = -1;
	if (AWipeoutDamageReplicator* DamageRep = FindDamageReplicator(GetWorld()))
	{
		ClockSeconds = DamageRep->GetRoundClockSecondsRemaining();
	}
	if (ClockSeconds < 0)
	{
		static UClass* CachedRoundCls = nullptr;
		static UIntProperty* CachedRoundProp = nullptr;
		UClass* GSCls = GS->GetClass();
		if (CachedRoundCls != GSCls)
		{
			CachedRoundCls = GSCls;
			CachedRoundProp = FindField<UIntProperty>(GSCls, TEXT("RoundSecondsRemaining"));
		}
		if (CachedRoundProp)
		{
			ClockSeconds = CachedRoundProp->GetPropertyValue_InContainer(GS);
		}
	}
	if (ClockSeconds < 0)
	{
		static UClass* CachedRemCls = nullptr;
		static UIntProperty* CachedRemProp = nullptr;
		UClass* GSCls = GS->GetClass();
		if (CachedRemCls != GSCls)
		{
			CachedRemCls = GSCls;
			CachedRemProp = FindField<UIntProperty>(GSCls, TEXT("RemainingTime"));
		}
		if (CachedRemProp)
		{
			const int32 RT = CachedRemProp->GetPropertyValue_InContainer(GS);
			if (GS->TimeLimit > 0) ClockSeconds = FMath::Max(0, RT);
		}
	}
	return ClockSeconds;
}

void AWipeoutHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !GS || !SmallFont || !MediumFont || !LargeFont) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

	// HUD-layout offsets and drag geometry are expressed in 1080p design pixels.
	// Height-based scaling keeps this module consistent on non-16:9 viewports.
	const float RenderScale = float(Canvas->SizeY) / 1080.0f;

	// Phase 3.5 layout consult — anchor + offset for the whole scorebar.
	// Stock placement: top-center, 2px from top edge.
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;

	// Get team colors (respect TeamSkins custom colors). When use_team_color is
	// disabled, retain the familiar fixed red/blue fallback.
	FLinearColor Team0Color = FLinearColor(0.8f, 0.05f, 0.05f, 1.f); // default red
	FLinearColor Team1Color = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);  // default blue
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));

	if (bUseTeamColor && GS->Teams.IsValidIndex(0) && GS->Teams[0])
	{
		Team0Color = GS->Teams[0]->TeamColor;
	}
	if (bUseTeamColor && GS->Teams.IsValidIndex(1) && GS->Teams[1])
	{
		Team1Color = GS->Teams[1]->TeamColor;
	}

	// Scores
	const int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	const int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Keep the score/color segments on the same sides as the active portrait
	// aliases. Fixed red/blue mode remains team 0 left; viewer-relative mode
	// swaps the score core only when team 0 occupies the enemy/right slot.
	static const FName NAME_PortraitEnemy(TEXT("portrait_enemy"));
	const bool bTeam0OnRight = bShouldDrawPortraits && bPortraitMockupRestyle
		&& PortraitAliasFor(0) == NAME_PortraitEnemy;
	const int32 LeftTeamIndex = bTeam0OnRight ? 1 : 0;
	const int32 RightTeamIndex = bTeam0OnRight ? 0 : 1;
	const FLinearColor TeamColors[2] = { Team0Color, Team1Color };

	// Blueprint children of Wipeout participate, while the four known native
	// AWipeoutHUD-derived modes (and any Blueprint children of them) retain their
	// existing score bars.
	if (IsWipeoutBetaTopBarSupported(this) && FNCPlusHUDLayout::WantsBetaTopBar())
	{
		int32 TeamCounts[2] = { 0, 0 };
		const bool bPortraitRound = bShouldDrawPortraits
			&& (GS->GetMatchState() == MatchState::InProgress
				|| GS->GetMatchState() == FName(TEXT("RoundCooldown")));
		if (bPortraitRound)
		{
			CountWipeoutPortraitPlayers(GS, TeamCounts[0], TeamCounts[1]);
			if (NCPlusHUDDrawCall::IsHidden(PortraitAliasFor(0))) TeamCounts[0] = 0;
			if (NCPlusHUDDrawCall::IsHidden(PortraitAliasFor(1))) TeamCounts[1] = 0;
		}

		FNCPlusBetaTopBarGeometry Geometry;
		if (NCPlusBetaTopBar::BuildGeometry(this, Canvas,
			TeamCounts[LeftTeamIndex], TeamCounts[RightTeamIndex], Geometry))
		{
			FNCPlusBetaTopBarCore Core;
			Core.LeftScore = (LeftTeamIndex == 0) ? Score0 : Score1;
			Core.RightScore = (RightTeamIndex == 0) ? Score0 : Score1;
			Core.ClockSeconds = ResolveBetaTopBarClockSeconds(GS);
			Core.LeftTeamColor = TeamColors[LeftTeamIndex];
			Core.RightTeamColor = TeamColors[RightTeamIndex];
			NCPlusBetaTopBar::DrawChassisAndScoreCore(this, Canvas, Geometry, Core);
			return;
		}
	}

	// Three restrained segments replace the old 220px team-name slabs and tails:
	// score | clock | score. All dimensions honor the existing scorebar Scale.
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float ScoreBoxWidth = 84.f * RenderScale * ScoreScale;
	const float ClockBoxWidth = 168.f * RenderScale * ScoreScale;
	const float ModuleHeight = 56.f * RenderScale * ScoreScale;
	const float ModuleWidth = 2.f * ScoreBoxWidth + ClockBoxWidth;
	const float ModuleX = CenterX - 0.5f * ModuleWidth;
	const float ScoreBoxX0 = ModuleX;
	const float ClockBoxX = ScoreBoxX0 + ScoreBoxWidth;
	const float ScoreBoxX1 = ClockBoxX + ClockBoxWidth;

	// Per-element opacity (the editor's Op slider) remains authoritative for
	// plates, accents, scores and clock text.
	const float ScoreOp = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	auto FadeL = [ScoreOp](FLinearColor C) -> FLinearColor { C.A *= ScoreOp; return C; };
	const FColor WhiteOp(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(ScoreOp * 255.f), 0, 255));

	// Muted charcoal/team blends retain TeamSkins identity without turning the
	// whole top of the screen into a pair of saturated ribbons.
	auto MutedTeamPlate = [](const FLinearColor& TeamColor) -> FLinearColor
	{
		return FLinearColor(
			FMath::Clamp(0.025f + 0.30f * TeamColor.R, 0.f, 1.f),
			FMath::Clamp(0.035f + 0.30f * TeamColor.G, 0.f, 1.f),
			FMath::Clamp(0.045f + 0.30f * TeamColor.B, 0.f, 1.f),
			0.94f);
	};
	Canvas->SetLinearDrawColor(FadeL(MutedTeamPlate(TeamColors[LeftTeamIndex])));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, ModuleHeight, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(0.015f, 0.028f, 0.040f, 0.92f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ClockBoxX, TopY, ClockBoxWidth, ModuleHeight, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FadeL(MutedTeamPlate(TeamColors[RightTeamIndex])));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, ModuleHeight, 0, 0, 1, 1, BLEND_Translucent);

	// One restrained color edge per score segment replaces the full perimeter.
	const float AccentHeight = FMath::Max(1.f, 1.5f * RenderScale * ScoreScale);
	FLinearColor LeftAccent = TeamColors[LeftTeamIndex]; LeftAccent.A = 0.82f;
	FLinearColor RightAccent = TeamColors[RightTeamIndex]; RightAccent.A = 0.82f;
	Canvas->SetLinearDrawColor(FadeL(LeftAccent));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, AccentHeight, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FadeL(RightAccent));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, AccentHeight, 0, 0, 1, 1, BLEND_Translucent);

	// The existing F5 font + font-size setting still styles the complete module.
	UFont* TeamScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, LargeFont);
	UFont* ClockFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, MediumFont);
	if (!TeamScoreFont) TeamScoreFont = LargeFont;
	if (!ClockFont) ClockFont = MediumFont;

	// font_scale Extras lets the user shrink/grow text independently of
	// plate dimensions. This remains independent of the element Scale control.
	const float FontExtraScale = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	const float LargeFontScale = RenderScale * 1.15f * ScoreScale * FontExtraScale;
	float XL, YL;

	FText ResolvedText;
	// F5 exposes both custom fonts and an independent FontSz multiplier. Keep
	// either from spilling into a neighboring segment or above/below the module
	// by re-resolving once at the tighter width/height fit when needed.
	auto ResolvePlateFittedText = [this](UFont* Font, const FString& Source,
		float DesiredScale, float MaxWidth, float MaxHeight, FText& OutText,
		float& OutWidth, float& OutHeight) -> float
	{
		float FittedScale = DesiredScale;
		NCPlusHUDDrawCall::ResolveStableText(Canvas, Font, Source,
			FittedScale, FittedScale, OutText, OutWidth, OutHeight);
		float FitRatio = 1.f;
		if (OutWidth > MaxWidth && OutWidth > KINDA_SMALL_NUMBER)
		{
			FitRatio = FMath::Min(FitRatio, MaxWidth / OutWidth);
		}
		if (OutHeight > MaxHeight && OutHeight > KINDA_SMALL_NUMBER)
		{
			FitRatio = FMath::Min(FitRatio, MaxHeight / OutHeight);
		}
		if (FitRatio < 1.f)
		{
			FittedScale *= FitRatio;
			NCPlusHUDDrawCall::ResolveStableText(Canvas, Font, Source,
				FittedScale, FittedScale, OutText, OutWidth, OutHeight);
		}
		return FittedScale;
	};

	static bool bHasCachedScore0 = false;
	static int32 CachedScore0 = 0;
	static FString Score0Str;
	if (!bHasCachedScore0 || CachedScore0 != Score0) { bHasCachedScore0 = true; CachedScore0 = Score0; Score0Str = FString::FromInt(Score0); }
	static bool bHasCachedScore1 = false;
	static int32 CachedScore1 = 0;
	static FString Score1Str;
	if (!bHasCachedScore1 || CachedScore1 != Score1) { bHasCachedScore1 = true; CachedScore1 = Score1; Score1Str = FString::FromInt(Score1); }

	const FString& LeftScoreStr = (LeftTeamIndex == 0) ? Score0Str : Score1Str;
	const FString& RightScoreStr = (RightTeamIndex == 0) ? Score0Str : Score1Str;
	const float ScoreTextMaxWidth = FMath::Max(1.f,
		ScoreBoxWidth - 16.f * RenderScale * ScoreScale);
	const float PlateTextMaxHeight = FMath::Max(1.f,
		ModuleHeight - 10.f * RenderScale * ScoreScale);
	const float LeftScoreScale = ResolvePlateFittedText(TeamScoreFont, LeftScoreStr,
		LargeFontScale, ScoreTextMaxWidth, PlateTextMaxHeight, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (ModuleHeight - YL) * 0.5f, LeftScoreScale, LeftScoreScale, Canvas->DrawColor);

	const float RightScoreScale = ResolvePlateFittedText(TeamScoreFont, RightScoreStr,
		LargeFontScale, ScoreTextMaxWidth, PlateTextMaxHeight, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (ModuleHeight - YL) * 0.5f, RightScoreScale, RightScoreScale, Canvas->DrawColor);

	// ── Clock (center segment) ──
	// Wipeout publishes an exact server-time deadline through its always-relevant
	// damage replicator. Deriving from that anchor every frame keeps this display
	// on the same schedule as Belt/Amp/Siphon instead of showing the legacy BP
	// GameState's up-to-one-second-old 1 Hz sample. Older servers and other
	// round-based modes still fall back to BP RoundSecondsRemaining; time-based
	// modes (Duel, CTF, etc.) finally fall back to stock RemainingTime.
	//
	// Static caches: UClass field tables don't change at runtime, so a single
	// FindField per (GameState class, property name) is enough. Stock
	// FindField walks the class hierarchy on every call; this is the cheap
	// trick for hot HUD paths that read replicated ints by name.
	int32 ClockSeconds = -1;
	if (AWipeoutDamageReplicator* DamageRep = FindDamageReplicator(GetWorld()))
	{
		ClockSeconds = DamageRep->GetRoundClockSecondsRemaining();
	}
	if (ClockSeconds < 0)
	{
		static UClass* CachedRoundCls = nullptr;
		static UIntProperty* CachedRoundProp = nullptr;
		UClass* GSCls = GS->GetClass();
		if (CachedRoundCls != GSCls)
		{
			CachedRoundCls  = GSCls;
			CachedRoundProp = FindField<UIntProperty>(GSCls, TEXT("RoundSecondsRemaining"));
		}
		if (CachedRoundProp)
		{
			ClockSeconds = CachedRoundProp->GetPropertyValue_InContainer(GS);
		}
	}
	if (ClockSeconds < 0)
	{
		// Stock match clock. AUTGameState::RemainingTime is protected, so we
		// can't access it directly from a plugin TU — UHT-generated reflection
		// doesn't honor C++ access modifiers, which is what makes this work.
		// RemainingTime counts down to 0 when a TimeLimit is set, or stays at
		// zero for untimed modes. TimeLimit below disambiguates terminal 00:00
		// from the neutral untimed center marker.
		static UClass* CachedRemCls = nullptr;
		static UIntProperty* CachedRemProp = nullptr;
		UClass* GSCls = GS->GetClass();
		if (CachedRemCls != GSCls)
		{
			CachedRemCls  = GSCls;
			CachedRemProp = FindField<UIntProperty>(GSCls, TEXT("RemainingTime"));
		}
		if (CachedRemProp)
		{
			const int32 RT = CachedRemProp->GetPropertyValue_InContainer(GS);
			// TimeLimit distinguishes a real terminal 00:00 from an untimed mode
			// whose stock RemainingTime also sits at zero.
			if (GS->TimeLimit > 0) ClockSeconds = FMath::Max(0, RT);
		}
	}

	const bool bHasClock = ClockSeconds >= 0;
	const float DesiredCenterScale = RenderScale * (bHasClock ? 0.95f : 0.72f)
		* ScoreScale * FontExtraScale;
	static const FString VersusStr(TEXT("VS"));
	const FString* CenterStr = &VersusStr;
	if (ClockSeconds >= 0)
	{
		int32 RMins = ClockSeconds / 60;
		int32 RSecs = ClockSeconds % 60;
		static int32 CachedClockSeconds = MAX_int32;
		static FString RoundClockStr;
		if (CachedClockSeconds != ClockSeconds)
		{
			CachedClockSeconds = ClockSeconds;
			RoundClockStr = FString::Printf(TEXT("%02d:%02d"), RMins, RSecs);
		}
		CenterStr = &RoundClockStr;
	}

	// Untimed inherited modes (notably Shaft Arena) get a short neutral marker
	// instead of an unexplained empty middle slab. Timed modes retain 00:00.
	const float CenterTextMaxWidth = FMath::Max(1.f,
		ClockBoxWidth - 20.f * RenderScale * ScoreScale);
	const float CenterDrawScale = ResolvePlateFittedText(ClockFont, *CenterStr,
		DesiredCenterScale, CenterTextMaxWidth, PlateTextMaxHeight, ResolvedText, XL, YL);
	if (bHasClock && ClockSeconds <= 30)
		Canvas->DrawColor = FColor(255, 60, 60, WhiteOp.A);
	else if (bHasClock)
		Canvas->DrawColor = WhiteOp;
	else
		Canvas->DrawColor = FColor(205, 218, 228, WhiteOp.A);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, ClockFont, ResolvedText,
		ClockBoxX + (ClockBoxWidth - XL) * 0.5f,
		TopY + (ModuleHeight - YL) * 0.5f,
		CenterDrawScale, CenterDrawScale, Canvas->DrawColor);

	// Removed: match elapsed timer and "You are on X" text — too cluttered
}

FName AWipeoutHUD::PortraitAliasFor(uint8 TeamIdx) const
{
	static const FName NAME_PortraitRed(TEXT("portrait_red"));
	static const FName NAME_PortraitBlue(TEXT("portrait_blue"));
	static const FName NAME_PortraitTeam(TEXT("portrait_team"));
	static const FName NAME_PortraitEnemy(TEXT("portrait_enemy"));
	if (!bPortraitMockupRestyle || !FNCPlusHUDLayout::WantsViewerRelativePortraits())
	{
		return (TeamIdx == 1) ? NAME_PortraitBlue : NAME_PortraitRed;
	}
	// Resolve against the LOCAL viewer's team so "my team" placement follows the
	// user across matches. A teamless viewer (true spectator) buckets red as
	// "ours" — the same convention as NCPlusForceModels::GetViewerTeam.
	uint8 ViewerTeam = 255;
	if (UTPlayerOwner && UTPlayerOwner->UTPlayerState)
	{
		ViewerTeam = UTPlayerOwner->UTPlayerState->GetTeamNum();
	}
	if (ViewerTeam == 255)
	{
		ViewerTeam = 0;
	}
	return (TeamIdx == ViewerTeam) ? NAME_PortraitTeam : NAME_PortraitEnemy;
}

void AWipeoutHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float PipSize)
{
	DrawPlayerIconPass(PlayerState, LiveScaling, XOffset, YOffset, PipSize);
}

void AWipeoutHUD::DrawPlayerIconPass(AUTPlayerState* PlayerState, float LiveScaling,
	float XOffset, float YOffset, float PipSize)
{
	FWipeoutPortraitPassState State;
	const FWipeoutPortraitPassState* CachedState = ActivePortraitDrawPass > 0
		? PortraitPassStateByPS.Find(PlayerState) : nullptr;
	if (CachedState)
	{
		State = *CachedState;
	}
	else
	{
		State.CharIcon = NCPlusHUDPortraits::Resolve(PlayerState);
		State.Alias = PortraitAliasFor(PlayerState->GetTeamNum());
		State.Opacity = NCPlusHUDDrawCall::GetOpacity(State.Alias);
		State.X = XOffset;
		State.Y = YOffset;
		State.Width = PipSize;
		State.Height = PipSize * WipeoutPortraitAspect;
		const float TimeSinceJoin = GetWorld()->TimeSeconds - PlayerState->CreationTime;
		if (TimeSinceJoin < 1.f)
		{
			const float SizeScale = 3.f - 2.f * TimeSinceJoin;
			State.Width *= SizeScale;
			State.Height *= SizeScale;
			State.Y += FMath::InterpEaseIn(State.Height, 0.f, TimeSinceJoin, 3.f);
		}
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		State.TeamColor = (PlayerState->GetTeamNum() == 1)
			? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)
			: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);
		if (NCPlusHUDDrawCall::GetUseTeamColor(State.Alias) && GS
			&& GS->Teams.IsValidIndex(PlayerState->GetTeamNum())
			&& GS->Teams[PlayerState->GetTeamNum()])
		{
			State.TeamColor = GS->Teams[PlayerState->GetTeamNum()]->TeamColor;
		}
		State.PlateColor = State.TeamColor;
		if (bPortraitMockupRestyle)
		{
			State.PlateColor = FLinearColor(
				FMath::Clamp(0.020f + 0.24f * State.TeamColor.R, 0.f, 1.f),
				FMath::Clamp(0.028f + 0.24f * State.TeamColor.G, 0.f, 1.f),
				FMath::Clamp(0.038f + 0.24f * State.TeamColor.B, 0.f, 1.f), 1.f);
		}
		State.TextScale = NCPlusHUDDrawCall::GetScale(State.Alias);
		State.PipFont = NCPlusHUDFonts::Resolve(State.Alias, this, MediumFont);
		if (!State.PipFont) State.PipFont = MediumFont;
		State.PipFontExtra = NCPlusHUDFonts::ResolveScale(State.Alias, 1.f);
		State.CompactTextFactor = bPortraitMockupRestyle ? WipeoutCompactTextFactor : 1.f;
		State.bRespawnQueued = PlayerState->bOutOfLives && PlayerState->RespawnWaitTime > 0.f;
		if (ActivePortraitDrawPass >= 0)
		{
			PortraitPassStateByPS.Add(PlayerState, State);
		}
	}
	if (State.CharIcon.Texture == nullptr) return;
	const FCanvasIcon& CharIcon = State.CharIcon;
	const float Op = State.Opacity;
	auto Tinted = [Op](FLinearColor C) -> FLinearColor { C.A *= Op; return C; };
	XOffset = State.X;
	YOffset = State.Y;
	PipSize = State.Width;
	const float PipHeight = State.Height;
	const FLinearColor TeamBGColor = State.TeamColor;
	const FLinearColor PortraitPlateColor = State.PlateColor;
	const float PortraitTextScale = State.TextScale;
	UFont* PipFont = State.PipFont;
	const float PipFontExtra = State.PipFontExtra;
	const float CompactTextFactor = State.CompactTextFactor;
	const bool bRespawnQueued = State.bRespawnQueued;
	if (ActivePortraitDrawPass < 0 || ActivePortraitDrawPass == 0)
	{
		Canvas->SetLinearDrawColor(Tinted(PortraitPlateColor));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight,
			0, 0, 1, 1);
	}

	// Layer 2: Character portrait (dimmed if dead)
	if (ActivePortraitDrawPass < 0 || ActivePortraitDrawPass == 1)
	{
		Canvas->SetLinearDrawColor(Tinted(LiveScaling < 1.f
			? FLinearColor(0.2f, 0.2f, 0.2f, 1.f) : FLinearColor::White));
		if (PlayerState->GetTeamNum() == 1)
		{
			// Blue team: flip horizontally (same as FlagRun)
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
				CharIcon.U + CharIcon.UL, CharIcon.V, CharIcon.UL * -1.0f, CharIcon.VL);
		}
		else
		{
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
				CharIcon.U, CharIcon.V, CharIcon.UL, CharIcon.VL);
		}
	}

	// Layer 3: Respawn dark overlay sweeping from right to left
	if ((ActivePortraitDrawPass < 0 || ActivePortraitDrawPass == 2) && LiveScaling < 1.f)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f)));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XOffset + LiveScaling * PipSize, YOffset,
			PipSize - LiveScaling * PipSize, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	if (ActivePortraitDrawPass < 0 || ActivePortraitDrawPass == 3)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));
		const FCanvasIcon& OverlayIcon = PlayerState->GetTeamNum() == 1 ? BlueTeamOverlay : RedTeamOverlay;
		Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
			OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);
	}
	if ((ActivePortraitDrawPass < 0 || ActivePortraitDrawPass == 4) && bPortraitMockupRestyle)
	{
		FLinearColor PortraitAccent = TeamBGColor;
		PortraitAccent.A = 0.82f;
		const float AccentHeight = FMath::Max(1.f, 0.022f * PipSize);
		Canvas->SetLinearDrawColor(Tinted(PortraitAccent));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize,
			AccentHeight, 0, 0, 1, 1, BLEND_Translucent);
	}
	if (ActivePortraitDrawPass >= 0 && ActivePortraitDrawPass != 5) return;

	// RespawnWaitTime is stock UT's replicated queue marker; RespawnTime is only
	// the locally ticking display value. A canceled queue must therefore become X
	// even if that stale local countdown is still positive for a frame.

	// Layer 5 (Wipeout-specific): Respawn countdown text on dead portraits
	if (LiveScaling < 1.f && bRespawnQueued && PlayerState->RespawnTime > 0.f)
	{
		float FontRenderScale = float(Canvas->SizeY) / 1080.0f
			* PortraitTextScale * PipFontExtra * CompactTextFactor;

		int32 SecondsRemaining = FMath::CeilToInt(PlayerState->RespawnTime);
		FWipeoutPipCache& PC = PipCacheByPS.FindOrAdd(PlayerState);
		if (PC.CountdownSeconds != SecondsRemaining || PC.CountdownFont != PipFont)
		{
			const FString CountdownStr = FString::FromInt(SecondsRemaining);
			Canvas->StrLen(PipFont, CountdownStr, PC.CountdownWidth, PC.CountdownHeight);
			PC.CountdownText = FText::FromString(CountdownStr);
			PC.CountdownSeconds = SecondsRemaining;
			PC.CountdownFont = PipFont;
		}
		if (bPortraitMockupRestyle)
		{
			if (PC.CountdownWidth > 0.f)
			{
				FontRenderScale = FMath::Min(FontRenderScale,
					0.82f * PipSize / PC.CountdownWidth);
			}
			if (PC.CountdownHeight > 0.f)
			{
				FontRenderScale = FMath::Min(FontRenderScale,
					0.72f * PipHeight / PC.CountdownHeight);
			}
		}

		// Team-tinted countdown color
		FLinearColor CountdownColor = (PlayerState->GetTeamNum() == 0)
			? FLinearColor(1.f, 0.4f, 0.4f, 1.f)    // Red team
			: FLinearColor(0.4f, 0.6f, 1.f, 1.f);     // Blue team

		Canvas->SetLinearDrawColor(Tinted(CountdownColor));
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, PipFont, PC.CountdownText,
			XOffset + (PipSize * 0.5f) - (PC.CountdownWidth * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (PC.CountdownHeight * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, Canvas->DrawColor, true);
	}

	// Layer 5b: "X" on dead portraits with no authoritative respawn queue.
	if (LiveScaling < 1.f && PlayerState->bOutOfLives
		&& !bRespawnQueued)
	{
		float FontRenderScale = float(Canvas->SizeY) / 1080.0f
			* PortraitTextScale * PipFontExtra * CompactTextFactor;
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;

		FWipeoutPipCache& PC = PipCacheByPS.FindOrAdd(PlayerState);
		if (PC.DeadXFont != PipFont)
		{
			static const FString DeadXString(TEXT("X"));
			Canvas->StrLen(PipFont, DeadXString, PC.DeadXWidth, PC.DeadXHeight);
			PC.DeadXText = FText::FromString(DeadXString);
			PC.DeadXFont = PipFont;
		}
		if (bPortraitMockupRestyle)
		{
			if (PC.DeadXWidth > 0.f)
			{
				FontRenderScale = FMath::Min(FontRenderScale,
					0.70f * PipSize / PC.DeadXWidth);
			}
			if (PC.DeadXHeight > 0.f)
			{
				FontRenderScale = FMath::Min(FontRenderScale,
					0.70f * PipHeight / PC.DeadXHeight);
			}
		}

		Canvas->SetLinearDrawColor(Tinted(FLinearColor(1.f, 0.2f, 0.2f, 0.9f)));
		Canvas->DrawText(PipFont, PC.DeadXText,
			XOffset + (PipSize * 0.5f) - (PC.DeadXWidth * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (PC.DeadXHeight * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
	}

	// Layer 6: health over armor, stacked LARGE on the portrait face (mockup
	// style) — alive teammates INCLUDING self (the whole team strip reads as one
	// card). ALSO revealed on enemy pips once the round is over (the strip only
	// draws in InProgress/RoundCooldown, so != InProgress IS the round-win
	// window — the winners' final health should be readable), and at ALL times
	// for TRUE spectators (bOnlySpectator; deliberately NOT bOutOfLives — a dead
	// player must not gain live enemy info mid-round).
	// PipFontExtra is the user's FontSz multiplier — the headline 4K legibility knob.
	if (LiveScaling >= 1.f && UTPlayerOwner)
	{
		AUTPlayerState* MyPS = Cast<AUTPlayerState>(UTPlayerOwner->PlayerState);
		AUTGameState* MatchGS = GetWorld()->GetGameState<AUTGameState>();
		const bool bSameTeam  = MyPS && MyPS->GetTeamNum() == PlayerState->GetTeamNum();
		const bool bRoundOver = MatchGS && MatchGS->GetMatchState() != MatchState::InProgress;
		const bool bTrueSpec  = MyPS && MyPS->bOnlySpectator;
		// Legacy pips (Clutch) keep the old exclusions: no numbers on self.
		if (MyPS && (bPortraitMockupRestyle || MyPS != PlayerState)
			&& (bSameTeam || bRoundOver || bTrueSpec))
		{
			AUTCharacter* UTC = (bHasPreparedPortraitSample && PreparedPortraitPS.Get() == PlayerState)
				? PreparedPortraitSample.Character.Get() : PlayerState->GetUTCharacter();
			if (UTC && !UTC->IsDead())
			{
				const bool bPrepared = bHasPreparedPortraitSample && PreparedPortraitPS.Get() == PlayerState;
				int32 HP = bPrepared ? PreparedPortraitSample.Health : UTC->Health;
				int32 Armor = bPrepared ? PreparedPortraitSample.Armor : UTC->GetArmorAmount();
				FWipeoutPipCache& PC = PipCacheByPS.FindOrAdd(PlayerState);

				if (bPortraitMockupRestyle)
				{
					const float BaseScale = float(Canvas->SizeY) / 1080.0f
						* PortraitTextScale * PipFontExtra * CompactTextFactor;
					float HpScale = BaseScale * 1.15f;
					float ArScale = BaseScale * 0.95f;

					// Rebuild each line's FText + measured extents only when its value
					// (or the font) changes — otherwise identical frame to frame.
					if (PC.HpKeyHP != HP || PC.HpFont != PipFont)
					{
						const FString HPStr = FString::FromInt(HP);
						Canvas->StrLen(PipFont, HPStr, PC.HpWidth, PC.HpHeight);
						PC.HpText  = FText::FromString(HPStr);
						PC.HpKeyHP = HP;
						if (PC.HpFont != PipFont)
						{
							PC.ArKeyAR = MIN_int32;   // font change invalidates the armor line too
						}
						PC.HpFont = PipFont;
					}
					if (PC.ArKeyAR != Armor)
					{
						const FString ArStr = FString::FromInt(Armor);
						Canvas->StrLen(PipFont, ArStr, PC.ArWidth, PC.ArHeight);
						PC.ArText  = FText::FromString(ArStr);
						PC.ArKeyAR = Armor;
					}

					// Keep boosted 3-digit values inside the portrait face even when a
					// wide custom F5 font is selected. This only constrains overflow;
					// ordinary two/three-digit values retain the larger mockup scale.
					if (PC.HpWidth > 0.f)
					{
						HpScale = FMath::Min(HpScale, 0.88f * PipSize / PC.HpWidth);
					}
					if (PC.ArWidth > 0.f)
					{
						ArScale = FMath::Min(ArScale, 0.88f * PipSize / PC.ArWidth);
					}

					// Fit the complete two-line stack into 76% of the card height with
					// a 4% (minimum 1px) gap. Applying one shared correction preserves
					// the intended HP-over-armor size hierarchy without overlap.
					const float LineGap = FMath::Max(1.f, 0.04f * PipHeight);
					float TextStackHeight = PC.HpHeight * HpScale + PC.ArHeight * ArScale;
					const float MaxStackHeight = 0.76f * PipHeight;
					if (TextStackHeight + LineGap > MaxStackHeight
						&& TextStackHeight > KINDA_SMALL_NUMBER)
					{
						const float StackFit = FMath::Max(0.f, MaxStackHeight - LineGap)
							/ TextStackHeight;
						HpScale *= StackFit;
						ArScale *= StackFit;
						TextStackHeight = PC.HpHeight * HpScale + PC.ArHeight * ArScale;
					}
					const float StackHeight = TextStackHeight + LineGap;

					const FLinearColor HealthGreen(0.45f, 1.f, 0.35f, 1.f);
					const FLinearColor ArmorBlue(0.30f, 0.80f, 1.f, 1.f);
					const float HpX = XOffset + (PipSize * 0.5f) - (PC.HpWidth * HpScale * 0.5f);
					const float HpY = YOffset + 0.5f * (PipHeight - StackHeight);
					const float ArX = XOffset + (PipSize * 0.5f) - (PC.ArWidth * ArScale * 0.5f);
					const float ArY = HpY + PC.HpHeight * HpScale + LineGap;
					NCPlusHUDDrawCall::DrawOutlinedText(Canvas, PipFont, PC.HpText, HpX, HpY,
						HpScale, HealthGreen, FLinearColor::Black, Op);
					NCPlusHUDDrawCall::DrawOutlinedText(Canvas, PipFont, PC.ArText, ArX, ArY,
						ArScale, ArmorBlue, FLinearColor::Black, Op);
				}
				else
				{
					// Legacy compact "HP/AR" bottom line (pre-restyle look, Clutch tiles).
					const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * 0.7f * PortraitTextScale * PipFontExtra;
					if (PC.HpKeyHP != HP || PC.ArKeyAR != Armor || PC.HpFont != PipFont)
					{
						const FString HPStr = FString::Printf(TEXT("%d/%d"), HP, Armor);
						Canvas->StrLen(PipFont, HPStr, PC.HpWidth, PC.HpHeight);
						PC.HpText  = FText::FromString(HPStr);
						PC.HpKeyHP = HP;
						PC.ArKeyAR = Armor;
						PC.HpFont  = PipFont;
					}
					float TextX = XOffset + (PipSize * 0.5f) - (PC.HpWidth * FontRenderScale * 0.5f);
					float TextY = YOffset + PipHeight - (PC.HpHeight * FontRenderScale) - 2.f;
					NCPlusHUDDrawCall::DrawOutlinedText(Canvas, PipFont, PC.HpText, TextX, TextY,
						FontRenderScale, FLinearColor::White, FLinearColor::Black, Op);
				}
			}
		}
	}
}

FLinearColor AWipeoutHUD::GetBaseHUDColor()
{
	FLinearColor TeamColor = Super::GetBaseHUDColor();
	APawn* HUDPawn = Cast<APawn>(UTPlayerOwner->GetViewTarget());
	if (HUDPawn)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(HUDPawn->PlayerState);
		if (PS != nullptr && PS->Team != nullptr)
		{
			TeamColor = PS->Team->TeamColor;
		}
	}
	return TeamColor;
}
