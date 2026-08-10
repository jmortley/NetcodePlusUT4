// ElimPlusHUD.cpp — Portrait strip for ElimPlus. Adapted from AWipeoutHUD.
// Removes Wipeout's respawn-countdown UI and next-to-spawn gold border since
// elim has no mid-round respawns. Adds an ELO chip per portrait.

#include "ElimPlusHUD.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "ElimPlusScoreboard.h"
#include "ElimPlusStatsReplicator.h"
#include "NCPlusHUDLayout.h"
#include "NCPlusForceModels.h"   // DrawHeadDebug (ncp.DebugHeads) — warmup-only head-hitbox calibration
#include "NCPlusSpectatorSlideOut.h"
#include "UTHUDWidget_SpectatorSlideOut.h"
#include "UTHUDWidget_Spectator.h"
#include "EngineUtils.h"

AElimPlusHUD::AElimPlusHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Portrait background icons — same UV coords as AUTFlagRunHUD / AWipeoutHUD.
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

	// AElimPlusHUD is a new class with no DefaultGame.ini section, so
	// RequiredHudWidgetClasses would be empty. Load standard team-game widgets
	// via HudWidgetClasses (built after RequiredHudWidgetClasses in BeginPlay).
	// Custom split WeaponBar — two independent strips. Replaces stock bpHW_WeaponBar.
	// Per-side anchor + offset come from the layout; per-weapon side assignment
	// from the layout's weapon_groups block.
	// Bottom bar: stock (familiar) vs NCPlus custom — chosen once at construction
	// (see FNCPlusHUDLayout::WantsStockBottomBar). Only ONE family is ever loaded, so
	// the two can never double-draw. Crosshair/powerups below are shared by both.
	if (FNCPlusHUDLayout::WantsStockBottomBar())
	{
		// Stock weapon bar + ammo + health/armor. The stock weapon bar self-reads the
		// player's HUD profile (orientation/scale/opacity/colors/weapon-group remap),
		// so the player's existing preference is honored with zero extra wiring.
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"));
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"));
		// Health/armor: Paperdoll AND QuickStats — BOTH, like every stock UT HUD.
		// Mutually exclusive at runtime via the profile's QuickStats flag: Paperdoll +
		// WeaponInfo self-HIDE when the mini-HUD is enabled (UTHUDWidget_Paperdoll.cpp:47,
		// UTHUDWidget_WeaponInfo.cpp:30) expecting bpHW_QuickStats to carry HP/armor/ammo;
		// omitting it (old comment had the gate INVERTED) left such profiles with NO
		// HP/armor/ammo (community fresh-install report 2026-07-01). No double-draw.
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Paperdoll.bpHW_Paperdoll_C"));
		HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_QuickStats.bpHW_QuickStats_C"));
	}
	else
	{
		// NCPlus custom split WeaponBar (two strips, per-side anchor/offset + per-weapon
		// side from the layout's weapon_groups block) — replaces stock bpHW_WeaponBar.
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"));
		// Modernized ammo counter — replaces stock bpHW_WeaponInfo (3 styles, fully editable).
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"));
		// Modernized minimal-typography health/armor display — replaces stock bpHW_QuickStats.
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
	// Optional opt-in accuracy widget — hidden by default (ShouldDraw requires
	// a layout entry); user enables via nchud and picks current/specific weapon.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"));
	// Optional default-hidden overlays — see WipeoutHUD for full notes.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Speedometer"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Minimap"));
	HudWidgetClasses.AddUnique(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_ReadyUp"));
	// ElimPlus-only: through-wall world-space markers for the candy orbs
	// dropped on player death (1 jump-boot + ammo restore on pickup). BP
	// widget that already does the world-to-screen projection + on-/off-
	// screen edge clamping. Hidden until user enables via nchud.
	HudWidgetClasses.Add(TEXT("/Game/Blueprints/ElimPlusStuff/CandyPickupMarker.CandyPickupMarker_C"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ElimPlusScoreboard"));
}

void AElimPlusHUD::BeginPlay()
{
	Super::BeginPlay();

	// Absolute Elim uses loose recovered PNGs. Decode and upload both the live
	// panel and scoreboard sets during HUD setup, never for the first time in Draw.
	if (!IsRunningDedicatedServer() && FNCPlusHUDLayout::WantsAbsoluteElimTeamPanel())
	{
		NCPlusHUDDrawCall::PreloadAbsoluteElimTeamPanelTextures();
		UElimPlusScoreboard::PreloadAbsoluteTextures();
	}

	// Snapshot the engine-spawned widget positions/origins BEFORE we override
	// anything. ApplyLayoutToWidgets uses these as the "no override" fallback
	// so Reset / Reset All restore the stock look exactly.
	CaptureWidgetDefaults(this);

	// Load JSON overrides from disk into the live singleton.
	// Slate editor mutates the singleton; DrawHUD re-applies each frame
	// (gated by a dirty flag) → any edit is visible next render tick.
	FNCPlusHUDLayout::ReloadLive();
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
}

void AElimPlusHUD::AddSpectatorWidgets()
{
	Super::AddSpectatorWidgets();

	// Replace the stock spectator slide-out with our subclass so the per-player
	// weapon-stats panel lists the Elim loadout and reads accuracy from the
	// replicated NCAccuracyStatsReplicator (stock reads server-only StatsData,
	// which is 0 on dedicated-server spectators). SpectatorHudWidgetClasses (the
	// base UTHUD ini section) contains exactly the stock slide-out — remove that
	// one instance (exact-class match so a re-entrant call can't drop our own).
	if (SpectatorSlideOutWidget && SpectatorSlideOutWidget->GetClass() == UUTHUDWidget_SpectatorSlideOut::StaticClass())
	{
		HudWidgets.Remove(SpectatorSlideOutWidget);
		SpectatorSlideOutWidget = nullptr;
	}
	// ALWAYS register the slide-out: its ShouldDraw bootstraps the interactive spectator
	// Slate window (cursor / ESC / camera switching). When the stock team panel is on we
	// suppress only its ROSTER VISUAL (bSuppressRosterDraw) — removing it entirely left
	// the cursor stuck + ESC dead (no SUTSpectatorWindow ever opened).
	if (UUTHUDWidget* W = AddHudWidget(UNCPlusSpectatorSlideOut::StaticClass()))
	{
		if (UNCPlusSpectatorSlideOut* SlideOut = Cast<UNCPlusSpectatorSlideOut>(W))
		{
			SlideOut->WeaponListMode = ENCSlideOutWeaponMode::ElimLoadout;
			SlideOut->bSuppressRosterDraw = FNCPlusHUDLayout::WantsStockTeamPanel();
		}
	}
}

EInputMode::Type AElimPlusHUD::GetInputMode_Implementation() const
{
	// Drag overlay (nchud_drag) needs cursor freed so Slate gets mouse events.
	// Checked first because it overrides the in-match GameOnly forcing below.
	if (NCPlusHUDDragMode::IsActive())
	{
		return EInputMode::EIM_GameAndUI;
	}

	// FIX: Mouse focus loss on death. Same pattern as AWipeoutHUD — base class
	// returns EIM_UIOnly when dead/out-of-lives, releasing mouse capture from
	// the viewport. In elim, dead players spectate teammates with full mouse
	// capture until the round ends. Force EIM_GameOnly during InProgress.
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

void AElimPlusHUD::NotifyMatchStateChange()
{
	Super::NotifyMatchStateChange();

	// (Pre-match team preview is rendered by DrawPreMatchTeamPreview() each
	// frame from DrawHUD; no per-state-change setup needed here.)

	// Post-match screenshot moved to DrawHUD (NCPlusHUDDrawCall::ServicePostMatchScreenshot) — the old
	// "match-ended + 1.5s" timer fired DURING the instant replay (captured the replay/win-banner, not the
	// final scoreboard). The shared helper waits for the replay demo to finish.
}

void AElimPlusHUD::GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	if (!bUsePreparedPlayerSnapshot)
	{
		BuildPlayerSnapshot(GS, GetScorerPlayerState());
	}
	const bool bFastEmptyOutput = SortedPlayers.Num() == 0;
	SortedPlayers.Reserve(SortedPlayers.Num() + PlayerSnapshot.PortraitPlayerIndices.Num());
	for (int32 PlayerIndex : PlayerSnapshot.PortraitPlayerIndices)
	{
		if (PlayerSnapshot.Players.IsValidIndex(PlayerIndex))
		{
			SortedPlayers.Add(PlayerSnapshot.Players[PlayerIndex].PlayerState);
		}
	}
	if (!bFastEmptyOutput)
	{
		auto ResolveSortKey = [this](AUTPlayerState* Candidate)
		{
			for (const FElimPlusHUDPlayerSnapshot& Player : PlayerSnapshot.Players)
			{
				if (Player.PlayerState == Candidate) return Player.PortraitSortKey;
			}
			return Candidate ? Candidate->SelectionOrder : uint8(0);
		};
		SortedPlayers.Sort([&ResolveSortKey](AUTPlayerState& A, AUTPlayerState& B)
		{
			return ResolveSortKey(&A) > ResolveSortKey(&B);
		});
	}
}

void AElimPlusHUD::BuildPlayerSnapshot(AUTGameState* GS, AUTPlayerState* ScorerPS)
{
	UWorld* World = GetWorld();
	if (!World || !GS)
	{
		PlayerSnapshot.ResetFrame(0);
		return;
	}

	const bool bContextChanged = CachedSnapshotWorld.Get() != World
		|| CachedSnapshotGameState.Get() != GS;
	if (bContextChanged)
	{
		CachedPortraitSignature.Reset();
		CachedPortraitOrder.Reset();
		PipCacheByPS.Reset();
		EloAnimByPlayerId.Reset();
		CachedSnapshotWorld = World;
		CachedSnapshotGameState = GS;
	}

	PlayerSnapshot.ResetFrame(GS->PlayerArray.Num());
	PlayerSnapshot.ScorerPS = ScorerPS;
	PlayerSnapshot.LocalPS = (UTPlayerOwner != nullptr)
		? Cast<AUTPlayerState>(UTPlayerOwner->PlayerState) : nullptr;

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
		if (!PS || PS->bOnlySpectator || PS->bIsInactive) continue;

		const uint8 TeamNum = PS->GetTeamNum();
		if (TeamNum > 1) continue;

		AUTCharacter* Character = nullptr;
		if (AController* Controller = Cast<AController>(PS->GetOwner()))
		{
			Character = Cast<AUTCharacter>(Controller->GetPawn());
		}
		else
		{
			Character = PS->GetUTCharacter();
		}

		FElimPlusHUDPlayerSnapshot FramePlayer;
		FramePlayer.PlayerState = PS;
		FramePlayer.Character = Character;
		FramePlayer.TeamNum = TeamNum;
		FramePlayer.bAlive = Character != nullptr && !Character->IsDead();
		FramePlayer.Health = Character ? Character->Health : 0;
		FramePlayer.Armor = Character ? Character->GetArmorAmount() : 0;
		// SelectionOrder is uint8 in the stock implementation: assigning -1 wraps
		// to 255, which places the scorer first under the descending comparator.
		FramePlayer.PortraitSortKey = (PS == ScorerPS) ? 255 : PS->SpectatingIDTeam;

		const int32 PlayerIndex = PlayerSnapshot.Players.Add(FramePlayer);
		PlayerSnapshot.TeamPlayerIndices[TeamNum].Add(PlayerIndex);
		if (FramePlayer.bAlive)
		{
			++PlayerSnapshot.AliveCountTeam[TeamNum];
			PlayerSnapshot.SoleSurvivor[TeamNum] = PS;
		}
	}

	for (int32 TeamNum = 0; TeamNum < 2; ++TeamNum)
	{
		if (PlayerSnapshot.AliveCountTeam[TeamNum] != 1)
		{
			PlayerSnapshot.SoleSurvivor[TeamNum] = nullptr;
		}
	}

	bool bPortraitOrderChanged = bContextChanged
		|| CachedPortraitSignature.Num() != PlayerSnapshot.Players.Num();
	if (!bPortraitOrderChanged)
	{
		for (int32 Index = 0; Index < PlayerSnapshot.Players.Num(); ++Index)
		{
			const FElimPlusHUDPlayerSnapshot& Player = PlayerSnapshot.Players[Index];
			const FPortraitOrderSignature& Signature = CachedPortraitSignature[Index];
			if (Signature.PlayerState.Get() != Player.PlayerState
				|| Signature.TeamNum != Player.TeamNum
				|| Signature.SortKey != Player.PortraitSortKey)
			{
				bPortraitOrderChanged = true;
				break;
			}
		}
	}

	if (bPortraitOrderChanged)
	{
		CachedPortraitSignature.Reset(PlayerSnapshot.Players.Num());
		CachedPortraitOrder.Reset(PlayerSnapshot.Players.Num());
		for (int32 Index = 0; Index < PlayerSnapshot.Players.Num(); ++Index)
		{
			const FElimPlusHUDPlayerSnapshot& Player = PlayerSnapshot.Players[Index];
			FPortraitOrderSignature Signature;
			Signature.PlayerState = Player.PlayerState;
			Signature.TeamNum = Player.TeamNum;
			Signature.SortKey = Player.PortraitSortKey;
			CachedPortraitSignature.Add(Signature);
			CachedPortraitOrder.Add(Index);
		}
		CachedPortraitOrder.Sort([this](const int32 A, const int32 B)
		{
			const int32 AKey = PlayerSnapshot.Players[A].PortraitSortKey;
			const int32 BKey = PlayerSnapshot.Players[B].PortraitSortKey;
			return (AKey != BKey) ? (AKey > BKey) : (A < B);
		});

		auto IsCurrentRosterPlayer = [this](AUTPlayerState* Candidate)
		{
			for (const FElimPlusHUDPlayerSnapshot& Player : PlayerSnapshot.Players)
			{
				if (Player.PlayerState == Candidate) return true;
			}
			return false;
		};
		for (auto It = PipCacheByPS.CreateIterator(); It; ++It)
		{
			if (!IsCurrentRosterPlayer(It.Key().Get())) It.RemoveCurrent();
		}
		for (auto It = EloAnimByPlayerId.CreateIterator(); It; ++It)
		{
			if (!IsCurrentRosterPlayer(It.Key().Get())) It.RemoveCurrent();
		}
	}

	PlayerSnapshot.PortraitPlayerIndices.Append(CachedPortraitOrder);
}

// Locate the stats replicator without walking the actor list every render frame.
// The weak positive/negative cache is per HUD, so simultaneous worlds do not thrash it.
AElimPlusStatsReplicator* AElimPlusHUD::FindStatsReplicator(UWorld* World)
{
	if (!World) return nullptr;
	AUTGameState* CurrentGS = World->GetGameState<AUTGameState>();
	if (CachedStatsWorld.Get() != World || CachedStatsGameState.Get() != CurrentGS)
	{
		CachedStatsWorld = World;
		CachedStatsGameState = CurrentGS;
		CachedStatsReplicator = nullptr;
		NextStatsReplicatorRetryTime = 0.f;
	}
	if (CachedStatsReplicator.IsValid()) return CachedStatsReplicator.Get();

	// Negative cache: before the replicator has spawned/replicated in (fresh join or
	// map travel) the scan below finds nothing, so without this we'd walk the entire
	// actor list EVERY frame. Retry at most ~1x/second while it's absent. A world change
	// bypasses the gate immediately because the per-HUD world cache resets above.
	const float Now = World->GetTimeSeconds();
	if (Now < NextStatsReplicatorRetryTime)
	{
		return nullptr;
	}

	for (TActorIterator<AElimPlusStatsReplicator> It(World); It; ++It)
	{
		CachedStatsReplicator = *It;
		return *It;
	}

	// Not found: retry at most once per second until replication catches up.
	CachedStatsReplicator = nullptr;
	NextStatsReplicatorRetryTime = Now + 1.0f;
	return nullptr;
}

// "NOW WATCHING <player>" spectator banner (verbatim port of ANCPlusCTFHUD::
// DrawSpectatorTarget). Bottom-right, suppressed by the caller when the
// scoreboard is up. This is the canonical viewed-player banner for both dead
// players and true spectators; DrawHUD suppresses the stock duplicate only for
// the frame where this banner can replace it.
void AElimPlusHUD::DrawSpectatorTarget()
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

void AElimPlusHUD::DrawHUD()
{
	// Re-apply the live layout every frame so Slate editor edits show up
	// immediately (Phase 2 live preview). Cheap — just a few field assignments
	// per registered widget, gated by a class-name lookup.
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

	// Guard: Canvas or fonts may be null during Slate UI overlays
	if (!Canvas || !SmallFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	const bool bScoreboardIsUp = ScoreboardIsUp();

	// Head-hitbox calibration (cvar `ncp.DebugHeads 1`): GREEN ring = the capsule headshot sphere the server
	// validates, RED cross = the mesh head bone (the visible head). Warmup-only in NETWORKED play (anti head-ESP)
	// but ALWAYS in standalone/offline so you can calibrate in a live single-player match (host -> cvar drives both).
	if (GS && (GS->GetMatchState() == MatchState::WaitingToStart || GetWorld()->GetNetMode() == NM_Standalone))
	{
		NCPlusForceModels::DrawHeadDebug(Canvas, PlayerOwner);
	}

	// Pre-match team preview overlay — replaces the unreliable scoreboard
	// auto-show. Drawn during PlayerIntro, CountdownToBegin, and the first
	// PreviewHoldAfterMatchStart seconds of InProgress so players see the
	// rebalance result before the match starts moving.
	DrawPreMatchTeamPreview();

	// Custom team score bar (replaces bpHW_TeamGameClock — respects TeamSkins).
	// Suppressed when the stock team panel is on AND visible: that panel shows team
	// scores + the round clock itself, so the scorebar would be redundant (no more
	// hand-hiding it in nchud). If the panel is hidden, the scorebar returns so you
	// never lose both. Non-stock mode still honors the scorebar's own nchud hide gate.
	const bool bStockPanelActive = FNCPlusHUDLayout::WantsStockTeamPanel() && !NCPlusHUDDrawCall::IsHidden(TEXT("team_panel"));
	if (GS && !bScoreboardIsUp)
	{
		if (!bStockPanelActive)
		{
			DrawTeamScoreBar(GS);
		}
		// NOW WATCHING banner — self-guards when not spectating another pawn.
		DrawSpectatorTarget();
	}

	// Keep portraits up through the round-win window ("RoundCooldown") so the
	// enemy team's final health stays visible after they win — not just InProgress.
	if (!bScoreboardIsUp && GS
		&& (GS->GetMatchState() == MatchState::InProgress
			|| GS->GetMatchState() == FName(TEXT("RoundCooldown"))))
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;
		AUTPlayerState* FrameScorerPS = GetScorerPlayerState();

		// A top-left team panel replaces the portrait strip when the user opts in.
		// Fresh installs use the recovered Absolute Elim 1.13 artwork; existing
		// users keep their prior procedural-stock/portrait choice.
		const bool bStockTeamPanel = FNCPlusHUDLayout::WantsStockTeamPanel();
		if (bStockTeamPanel)
		{
			if (!NCPlusHUDDrawCall::IsHidden(TEXT("team_panel")))
			{
				BuildPlayerSnapshot(GS, FrameScorerPS);
				if (FNCPlusHUDLayout::WantsAbsoluteElimTeamPanel())
				{
					NCPlusHUDDrawCall::DrawAbsoluteElimTeamPanel(this, Canvas, PlayerSnapshot);
				}
				else
				{
					NCPlusHUDDrawCall::DrawStockTeamPanel(this, Canvas, PlayerSnapshot);
				}
			}
		}
		else
		{
		BuildPlayerSnapshot(GS, FrameScorerPS);
		PortraitRenderPlayers.Reset();
		bUsePreparedPlayerSnapshot = true;
		GetPlayerListForIcons(PortraitRenderPlayers);
		bUsePreparedPlayerSnapshot = false;
		const float TeammateScale = 0.4f;

		const float BasePipSize = (32 + (64 * TeammateScale)) * GetHUDWidgetScaleOverride() * RenderScale;
		// Phase 3.11: per-strip Scale override. Layout entries can independently
		// resize red vs blue (e.g. user wants their team larger).
		const float RedScale  = NCPlusHUDDrawCall::GetScale(TEXT("portrait_red"));
		const float BlueScale = NCPlusHUDDrawCall::GetScale(TEXT("portrait_blue"));
		const float RedPipSize  = BasePipSize * RedScale;
		const float BluePipSize = BasePipSize * BlueScale;
		const float XAdjust = BasePipSize * 1.1f;

		// Stock fallback positions.
		const float StockXRed  = 0.4f * Canvas->ClipX - XAdjust - BasePipSize;
		const float StockXBlue = 0.6f * Canvas->ClipX + XAdjust;
		const float StockY     = 0.005f * Canvas->ClipY * GetHUDWidgetScaleOverride() * RenderScale;

		// Phase 3.5 layout consult.
		const FVector2D RedStart  = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("portrait_red"),  Canvas, FVector2D(StockXRed,  StockY));
		const FVector2D BlueStart = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("portrait_blue"), Canvas, FVector2D(StockXBlue, StockY));
		const bool bHideRed  = NCPlusHUDDrawCall::IsHidden(TEXT("portrait_red"));
		const bool bHideBlue = NCPlusHUDDrawCall::IsHidden(TEXT("portrait_blue"));

		// Per-strip grow direction — team-defaulted (red grows leftward, blue
		// grows rightward, matching conventional CTF/elim layouts). Only flip
		// when the default direction would push the strip off-screen — handles
		// edge-anchored placements (TopLeft / TopRight) and aggressive drags.
		const float EstStripWidth = 5.f * XAdjust;  // 5 pips × spacing — worst-case team size
		float RedGrowSign  = -1.f;  // red default: extend leftward from anchor
		float BlueGrowSign = +1.f;  // blue default: extend rightward from anchor
		if (RedStart.X  - EstStripWidth < 0.f)               RedGrowSign  = +1.f;
		if (BlueStart.X + EstStripWidth > Canvas->ClipX)     BlueGrowSign = -1.f;

		float XOffsetRed  = RedStart.X;
		float XOffsetBlue = BlueStart.X;
		// When growing leftward, the resolved point is the strip's RIGHT edge —
		// shift the first pip left by its width so its right edge sits at the
		// anchor instead of extending off-screen. Use the per-side scaled pip
		// size so the right-edge alignment stays correct under custom Scale.
		if (RedGrowSign  < 0.f) XOffsetRed  -= RedPipSize;
		if (BlueGrowSign < 0.f) XOffsetBlue -= BluePipSize;
		const float YOffsetRed  = RedStart.Y;
		const float YOffsetBlue = BlueStart.Y;
		const float YOffset     = YOffsetRed;  // legacy single-Y for code that doesn't yet split

		AElimPlusStatsReplicator* Stats = FindStatsReplicator(GetWorld());

		for (int32 DrawOrdinal = 0; DrawOrdinal < PortraitRenderPlayers.Num(); ++DrawOrdinal)
		{
			AUTPlayerState* UTPS = PortraitRenderPlayers[DrawOrdinal];
			if (!UTPS) continue;
			const FElimPlusHUDPlayerSnapshot* FramePlayerPtr = nullptr;
			if (PlayerSnapshot.PortraitPlayerIndices.IsValidIndex(DrawOrdinal))
			{
				const int32 CachedIndex = PlayerSnapshot.PortraitPlayerIndices[DrawOrdinal];
				if (PlayerSnapshot.Players.IsValidIndex(CachedIndex)
					&& PlayerSnapshot.Players[CachedIndex].PlayerState == UTPS)
				{
					FramePlayerPtr = &PlayerSnapshot.Players[CachedIndex];
				}
			}
			for (int32 CandidateIndex = 0; !FramePlayerPtr && CandidateIndex < PlayerSnapshot.Players.Num(); ++CandidateIndex)
			{
				const FElimPlusHUDPlayerSnapshot& Candidate = PlayerSnapshot.Players[CandidateIndex];
				if (Candidate.PlayerState == UTPS)
				{
					FramePlayerPtr = &Candidate;
					break;
				}
			}
			FElimPlusHUDPlayerSnapshot FallbackPlayer;
			if (!FramePlayerPtr)
			{
				FallbackPlayer.PlayerState = UTPS;
				FallbackPlayer.TeamNum = UTPS->GetTeamNum();
				if (FallbackPlayer.TeamNum > 1) continue;
				if (AController* Controller = Cast<AController>(UTPS->GetOwner()))
				{
					FallbackPlayer.Character = Cast<AUTCharacter>(Controller->GetPawn());
				}
				else
				{
					FallbackPlayer.Character = UTPS->GetUTCharacter();
				}
				FallbackPlayer.bAlive = FallbackPlayer.Character != nullptr
					&& !FallbackPlayer.Character->IsDead();
				FallbackPlayer.Health = FallbackPlayer.Character ? FallbackPlayer.Character->Health : 0;
				FallbackPlayer.Armor = FallbackPlayer.Character
					? FallbackPlayer.Character->GetArmorAmount() : 0;
				FramePlayerPtr = &FallbackPlayer;
			}
			const FElimPlusHUDPlayerSnapshot& FramePlayer = *FramePlayerPtr;
			const float OwnerPipScaling = (UTPS == FrameScorerPS) ? 1.25f : 1.f;
			// Per-team pip size: red strip honors portrait_red.Scale, blue honors
			// portrait_blue.Scale. Scoring-player gets a 1.25x bump on top.
			const uint8 PreTeamIdx = FramePlayer.TeamNum;
			const float TeamPipBase = (PreTeamIdx == 1) ? BluePipSize : RedPipSize;
			const float PipSize = TeamPipBase * OwnerPipScaling;
			const float PipHeight = PipSize * (320.0f / 224.0f);

			// Pawn/alive state was sampled once for this render frame.
			const bool bPlayerAlive = FramePlayer.bAlive;

			// In elim, dead = stays dead until round end. Hide portrait entirely
			// so the strip reflows toward the center as players die.
			if (!bPlayerAlive)
			{
				continue;
			}

			const uint8 TeamIdx = FramePlayer.TeamNum;
			// Phase 3.5 hide gates.
			if (TeamIdx == 0 && bHideRed)  continue;
			if (TeamIdx == 1 && bHideBlue) continue;
			float* XOffsetForTeam = nullptr;
			float  YForTeam = YOffset;
			if (TeamIdx == 0)      { RedPlayerCount++;  XOffsetForTeam = &XOffsetRed;  YForTeam = YOffsetRed;  }
			else if (TeamIdx == 1) { BluePlayerCount++; XOffsetForTeam = &XOffsetBlue; YForTeam = YOffsetBlue; }
			else                   { continue; }

			const float XOffset = *XOffsetForTeam;
			ActiveFramePlayer = FramePlayer;
			bHasActiveFramePlayer = true;
			DrawPlayerIcon(UTPS, bPlayerAlive, XOffset, YForTeam, PipSize);
			bHasActiveFramePlayer = false;

			// Player name above icon — multiply by team scale so text stays
			// proportional when the strip is shrunk via the layout's Sc spinner.
			{
				/* Portraits (Red)/(Blue) Font + FontSz now restyle the player name. */ const FName PortraitAlias = (TeamIdx == 1) ? FName(TEXT("portrait_blue")) : FName(TEXT("portrait_red")); UFont* NameFont = NCPlusHUDFonts::Resolve(PortraitAlias, this, SmallFont); if (!NameFont) { NameFont = SmallFont; } const float NameFontExtra = NCPlusHUDFonts::ResolveScale(PortraitAlias, 1.f);
					const float TeamScale = (PreTeamIdx == 1) ? BlueScale : RedScale;
				const float NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f * TeamScale * NameFontExtra;
				// Cached fit (no per-frame chop-one-char StrLen loop) + one outlined item.
				FText NameText; float NW, NH;
				NCPlusHUDDrawCall::ResolveFittedName(Canvas, UTPS, NameFont, UTPS->PlayerName, PipSize, NameScale, NameText, NW, NH);
				const float NameX = XOffset + (PipSize * 0.5f) - (NW * NameScale * 0.5f);
				const float NameY = YForTeam + 2.f;
				NCPlusHUDDrawCall::DrawOutlinedText(Canvas, NameFont, NameText, NameX, NameY, NameScale, FLinearColor::White);
			}

			// Last-man-standing pulse: white-flashing border at 1Hz when this
			// PS is the sole surviving member of their team.
			if (UTPS == PlayerSnapshot.SoleSurvivor[TeamIdx])
			{
				const float Pulse = 0.5f + 0.5f * FMath::Sin(GetWorld()->TimeSeconds * 2.f * PI);
				FLinearColor PulseColor = FMath::Lerp(FLinearColor::White, FLinearColor(1.f, 1.f, 0.4f, 1.f), Pulse);
				PulseColor.A = 0.9f;
				const float BorderW = 2.f;
				Canvas->SetLinearDrawColor(PulseColor);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YForTeam, PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YForTeam + PipHeight - BorderW, PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YForTeam, BorderW, PipHeight, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset + PipSize - BorderW, YForTeam, BorderW, PipHeight, 0, 0, 1, 1);
			}

			// ELO chip below the portrait. At match end, count up over EloAnimDurationSec
			// from (Elo - Delta) to Elo with green/red color fade. Trigger is the first
			// frame the replicator returns Delta != 0; self-clears when Delta returns to 0.
			// Bots use the synthetic "BOT:<name>" key so randomized bot ELOs render too.
			if (Stats && UTPS)
			{
				FElimPipCache& PC = PipCacheByPS.FindOrAdd(UTPS);
				const bool bHasOnlineId = UTPS->UniqueId.IsValid();
				const bool bBotNameChanged = !bHasOnlineId
					&& !PC.UidSourceName.Equals(UTPS->PlayerName, ESearchCase::CaseSensitive);
				if (!PC.bUidValid || PC.bUidFromOnlineId != bHasOnlineId || bBotNameChanged)
				{
					// Refresh when a delayed human UniqueId replaces the provisional bot key.
					PC.UidStr = bHasOnlineId
						? UTPS->UniqueId.ToString()
						: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
					PC.UidSourceName = bHasOnlineId ? FString() : UTPS->PlayerName;
					PC.bUidValid = true;
					PC.bUidFromOnlineId = bHasOnlineId;
				}
				const int32 ServerElo = Stats->GetEloForPlayer(PC.UidStr);
				const int32 ServerDelta = Stats->GetEloDeltaForPlayer(PC.UidStr);

				int32 DisplayElo = ServerElo;
				int32 DisplayDelta = ServerDelta;
				float ColorBlend = 1.f;  // 1 = full final color, 0 = white

				if (ServerDelta == 0)
				{
					// Mid-match (or pre-match). Drop any stale anim entry.
					EloAnimByPlayerId.Remove(UTPS);
				}
				else
				{
					FElimPlusEloAnim& Anim = EloAnimByPlayerId.FindOrAdd(UTPS);
					if (Anim.FinalDelta == 0)  // freshly created this frame
					{
						Anim.StartTime  = GetWorld()->TimeSeconds;
						Anim.FromElo    = ServerElo - ServerDelta;
						Anim.ToElo      = ServerElo;
						Anim.FinalDelta = ServerDelta;
					}
					const float t = FMath::Clamp(
						(GetWorld()->TimeSeconds - Anim.StartTime) / EloAnimDurationSec, 0.f, 1.f);
					DisplayElo   = FMath::RoundToInt(FMath::Lerp(float(Anim.FromElo), float(Anim.ToElo), t));
					DisplayDelta = DisplayElo - Anim.FromElo;
					ColorBlend   = t;
				}

				const float ChipScale = float(Canvas->SizeY) / 1080.0f * 0.55f;

				// Rebuild the chip FText + measured width only when the displayed value
				// changes (stable mid-match; only the 4s match-end count-up churns it).
				if (PC.EloKeyElo != DisplayElo || PC.EloKeyDelta != DisplayDelta)
				{
					FString EloStr = FString::Printf(TEXT("%d"), DisplayElo);
					if (DisplayDelta != 0)
					{
						EloStr += (DisplayDelta > 0)
							? FString::Printf(TEXT(" +%d"), DisplayDelta)
							: FString::Printf(TEXT(" %d"), DisplayDelta);
					}
					float CXL, CYL;
					Canvas->StrLen(TinyFont, EloStr, CXL, CYL);
					PC.EloText     = FText::FromString(EloStr);
					PC.EloWidth    = CXL;
					PC.EloKeyElo   = DisplayElo;
					PC.EloKeyDelta = DisplayDelta;
				}

				const float ChipX = XOffset + (PipSize * 0.5f) - (PC.EloWidth * ChipScale * 0.5f);
				const float ChipY = YForTeam + PipHeight + 2.f;

				FLinearColor TargetColor = FLinearColor::White;
				if (DisplayDelta > 0)      TargetColor = FLinearColor(0.4f, 1.f, 0.4f, 1.f);
				else if (DisplayDelta < 0) TargetColor = FLinearColor(1.f, 0.4f, 0.4f, 1.f);
				const FLinearColor EloColor = FMath::Lerp(FLinearColor::White, TargetColor, ColorBlend);
				NCPlusHUDDrawCall::DrawOutlinedText(Canvas, TinyFont, PC.EloText, ChipX, ChipY, ChipScale, EloColor);
			}

			// Advance the column offset for the next portrait
			if (TeamIdx == 0) XOffsetRed  += RedGrowSign  * 1.1f * PipSize;
			else              XOffsetBlue += BlueGrowSign * 1.1f * PipSize;
		}
		} // end else — NCPlus portrait strip (stock panel handled above)

		// Score / KDA mini widget (top right) — layout-aware via "score_kda"
		// alias. Position, scale, and font are nchud-overridable; layout
		// scale multiplies into FontScale so resizing in the editor actually
		// shrinks/grows the rendered text (the unconditional 0.9f stays as
		// the design-default scale relative to SmallFont).
		AUTPlayerState* MyPS = FrameScorerPS;
		if (MyPS && Canvas && SmallFont && !NCPlusHUDDrawCall::IsHidden(TEXT("score_kda")))
		{
			const int32 Score = FMath::TruncToInt(MyPS->Score);
			const int32 Kills = MyPS->Kills;
			const int32 Deaths = MyPS->Deaths;
			const int32 Assists = MyPS->KillAssists;

			if (CachedKdaPS.Get() != MyPS || CachedKdaScore != Score || CachedKdaKills != Kills
				|| CachedKdaDeaths != Deaths || CachedKdaAssists != Assists)
			{
				CachedKdaPS = MyPS;
				CachedKdaScore = Score;
				CachedKdaKills = Kills;
				CachedKdaDeaths = Deaths;
				CachedKdaAssists = Assists;
				CachedScoreString = FString::Printf(TEXT("Score: %d"), Score);
				CachedKdaString = FString::Printf(TEXT("KDA: %d / %d / %d"), Kills, Deaths, Assists);
			}

			const FVector2D StockPos(Canvas->ClipX * 0.98f, Canvas->ClipY * 0.015f);
			const FVector2D ResolvedPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("score_kda"), Canvas, StockPos);
			const float ElemScale = NCPlusHUDDrawCall::GetScale(TEXT("score_kda"));
			const float KdaOp = NCPlusHUDDrawCall::GetOpacity(TEXT("score_kda"));
			const float FontExtra = NCPlusHUDFonts::ResolveScale(TEXT("score_kda"), 1.f);
			const float FontScale = RenderScale * 0.9f * ElemScale * FontExtra;

			UFont* KDAFont = NCPlusHUDFonts::Resolve(TEXT("score_kda"), this, SmallFont);
			if (!KDAFont) KDAFont = SmallFont;

			float KDAXPos = ResolvedPos.X;
			float KDAYPos = ResolvedPos.Y;

			FText ResolvedText;
			float XL, YL;
			NCPlusHUDDrawCall::ResolveStableText(Canvas, KDAFont, CachedScoreString, FontScale, FontScale, ResolvedText, XL, YL);
			Canvas->DrawColor = FColor(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(220.f * KdaOp), 0, 255));
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, KDAFont, ResolvedText, KDAXPos - XL, KDAYPos, FontScale, FontScale, Canvas->DrawColor);
			KDAYPos += YL * 1.1f;

			NCPlusHUDDrawCall::ResolveStableText(Canvas, KDAFont, CachedKdaString, FontScale, FontScale, ResolvedText, XL, YL);
			Canvas->DrawColor = FColor(200, 200, 200, (uint8)FMath::Clamp(FMath::RoundToInt(200.f * KdaOp), 0, 255));
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, KDAFont, ResolvedText, KDAXPos - XL, KDAYPos, FontScale, FontScale, Canvas->DrawColor);
		}
	}

	// Held-pickup status (amp/berserk/siphon countdown + boot charges) — NCPlus mode only.
	NCPlusHUDDrawCall::DrawHeldPowerups(this, Canvas);

	// Optional opt-in overlays (default OFF). DrawDamageFlash must be last so it
	// tints over every other HUD draw.
	NCPlusHUDDrawCall::DrawServerInfo(this, Canvas);
	NCPlusHUDDrawCall::DrawDamageFlash(this, Canvas);

}

// Custom team score bar — dynamic team colors, round clock. Same pattern as Wipeout.
void AElimPlusHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !SmallFont || !MediumFont || !LargeFont) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;

	// Phase 3.5 layout consult.
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;

	FLinearColor Team0Color = FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	FLinearColor Team1Color = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);
	bool bCustomColors = false;
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));

	if (bUseTeamColor && GS->Teams.IsValidIndex(0) && GS->Teams[0])
	{
		FLinearColor TC = GS->Teams[0]->TeamColor;
		if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f)
			bCustomColors = true;
		Team0Color = TC;
	}
	if (bUseTeamColor && GS->Teams.IsValidIndex(1) && GS->Teams[1])
	{
		FLinearColor TC = GS->Teams[1]->TeamColor;
		if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f)
			bCustomColors = true;
		Team1Color = TC;
	}

	static const FString CustomTeam0Name(TEXT("Phayder (R)"));
	static const FString CustomTeam1Name(TEXT("Liandri (B)"));
	static const FString StockTeam0Name(TEXT("RED"));
	static const FString StockTeam1Name(TEXT("BLUE"));
	const FString& Team0Name = bCustomColors ? CustomTeam0Name : StockTeam0Name;
	const FString& Team1Name = bCustomColors ? CustomTeam1Name : StockTeam1Name;

	const int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	const int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Phase 3.11: scorebar Scale override scales the whole bar (and clock font
	// scales below) uniformly. RenderScale stays for resolution-independence.
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float BarWidth = 220.f * RenderScale * ScoreScale;
	const float BarHeight = 36.f * RenderScale * ScoreScale;
	const float ScoreBoxWidth = 50.f * RenderScale * ScoreScale;
	const float GapWidth = 8.f * RenderScale;

	// Per-element opacity (the editor's Op slider). Mirror the portrait Tinted-lambda:
	// FadeL scales every tile color's alpha; WhiteOp is the faded text color. Op
	// defaults to 1.0 (no override) so untouched layouts render pixel-identically.
	const float ScoreOp = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	auto FadeL = [ScoreOp](FLinearColor C) -> FLinearColor { C.A *= ScoreOp; return C; };
	const FColor WhiteOp(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(ScoreOp * 255.f), 0, 255));

	const float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(FadeL(Team0Color));
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(Team1Color));
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float TailHeight = 14.f * RenderScale * ScoreScale;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	Canvas->SetLinearDrawColor(FadeL(FLinearColor::White));
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	// font_scale Extras lets the user shrink/grow text independently of bar
	// dimensions. Useful for tall fonts (Extreme) where ScoreScale alone
	// undershoots because the LargeFont 1.2x multiplier offsets the bar
	// shrink (1.2 * 0.85 ≈ 1.02 = barely changed).
	const float FontExtraScale = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	const float FontScale = RenderScale * 0.85f * ScoreScale * FontExtraScale;
	const float LargeFontScale = RenderScale * 1.2f * ScoreScale * FontExtraScale;
	float XL, YL;

	// Per-element font override (Phase 3.8). Single "scorebar" alias drives
	// both team-name and score-number fonts.
	UFont* TeamNameFont  = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, SmallFont);
	UFont* TeamScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, LargeFont);
	if (!TeamNameFont)  TeamNameFont  = SmallFont;
	if (!TeamScoreFont) TeamScoreFont = LargeFont;

	FText ResolvedText;
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team0Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	static bool bHasCachedScore0 = false;
	static int32 CachedScore0 = 0;
	static FString Score0Str;
	if (!bHasCachedScore0 || CachedScore0 != Score0) { bHasCachedScore0 = true; CachedScore0 = Score0; Score0Str = FString::FromInt(Score0); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score0Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	static bool bHasCachedScore1 = false;
	static int32 CachedScore1 = 0;
	static FString Score1Str;
	if (!bHasCachedScore1 || CachedScore1 != Score1) { bHasCachedScore1 = true; CachedScore1 = Score1; Score1Str = FString::FromInt(Score1); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score1Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team1Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	// Round Clock — read RoundSecondsRemaining from BP GameState via reflection.
	// Static cache: FindField walks the class hierarchy and was running every
	// frame; (GameState class, property name) is immutable so a one-shot
	// resolution is enough.
	const float ClockY = TopY + BarHeight + 2.f * RenderScale;
	int32 RoundTime = -1;
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
		RoundTime = CachedRoundProp->GetPropertyValue_InContainer(GS);
	}

	const float RoundClockScale = RenderScale * 1.1f * ScoreScale * FontExtraScale;
	if (RoundTime >= 0)
	{
		const int32 RMins = RoundTime / 60;
		const int32 RSecs = RoundTime % 60;
		static int32 CachedRoundTime = MAX_int32;
		static FString RoundClockStr;
		if (CachedRoundTime != RoundTime)
		{
			CachedRoundTime = RoundTime;
			RoundClockStr = FString::Printf(TEXT("%02d:%02d"), RMins, RSecs);
		}
		NCPlusHUDDrawCall::ResolveStableText(Canvas, MediumFont, RoundClockStr, RoundClockScale, RoundClockScale, ResolvedText, XL, YL);
		Canvas->DrawColor = (RoundTime <= 30) ? FColor(255, 60, 60, WhiteOp.A) : WhiteOp;
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
	}
}

void AElimPlusHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, bool bPlayerAlive,
	float XOffset, float YOffset, float PipSize)
{
	DrawPlayerIconFromSnapshot(PlayerState, bPlayerAlive, XOffset, YOffset, PipSize,
		bHasActiveFramePlayer ? &ActiveFramePlayer : nullptr);
}

void AElimPlusHUD::DrawPlayerIconFromSnapshot(AUTPlayerState* PlayerState, bool bPlayerAlive,
	float XOffset, float YOffset, float PipSize, const FElimPlusHUDPlayerSnapshot* FramePlayer)
{
	const FCanvasIcon CharIcon = NCPlusHUDPortraits::Resolve(PlayerState);
	if (CharIcon.Texture == nullptr)
	{
		return;
	}

	// Per-portrait opacity (Phase 3.5+): scale every SetLinearDrawColor alpha by
	// this so the editor's Op slider fades the entire portrait stack consistently.
	const FName PortraitAlias = (PlayerState->GetTeamNum() == 1) ? FName(TEXT("portrait_blue")) : FName(TEXT("portrait_red"));
	const float Op = NCPlusHUDDrawCall::GetOpacity(PortraitAlias);
	auto Tinted = [Op](FLinearColor C) -> FLinearColor { C.A *= Op; return C; };

	Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));
	float PipHeight = PipSize * (320.0f / 224.0f);

	// Join animation — pop-in over 1 second (same as FlagRun)
	const float TimeSinceJoin = GetWorld()->TimeSeconds - PlayerState->CreationTime;
	if (TimeSinceJoin < 1.0f)
	{
		const float SizeScale = 3.0f - (2.0f * TimeSinceJoin);
		PipSize *= SizeScale;
		PipHeight *= SizeScale;
		YOffset += FMath::InterpEaseIn(PipHeight, 0.0f, TimeSinceJoin, 3.0f);
	}

	// Layer 1: Team-colored background (TeamSkins-aware).
	// Honors per-portrait `use_team_color` extra: when false, locks to stock red/blue.
	AUTGameState* GS = (FramePlayer && CachedSnapshotWorld.Get() == GetWorld())
		? CachedSnapshotGameState.Get() : GetWorld()->GetGameState<AUTGameState>();
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(PortraitAlias);
	FLinearColor TeamBGColor = (PlayerState->GetTeamNum() == 1)
		? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)
		: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);
	if (bUseTeamColor && GS && GS->Teams.IsValidIndex(PlayerState->GetTeamNum()) && GS->Teams[PlayerState->GetTeamNum()])
	{
		TeamBGColor = GS->Teams[PlayerState->GetTeamNum()]->TeamColor;
	}
	Canvas->SetLinearDrawColor(Tinted(TeamBGColor));
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));

	// Layer 2: Character portrait (dimmed if dead)
	if (!bPlayerAlive)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor(0.2f, 0.2f, 0.2f, 1.f)));
	}
	if (PlayerState->GetTeamNum() == 1)
	{
		Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
			CharIcon.U + CharIcon.UL, CharIcon.V, CharIcon.UL * -1.0f, CharIcon.VL);
	}
	else
	{
		Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
			CharIcon.U, CharIcon.V, CharIcon.UL, CharIcon.VL);
	}

	// Layer 3: Full-pip dim overlay if dead (no sweep animation in elim — they
	// stay dead until the round ends).
	if (!bPlayerAlive)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f)));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));
	const FCanvasIcon& OverlayIcon = PlayerState->GetTeamNum() == 1 ? BlueTeamOverlay : RedTeamOverlay;
	Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
		OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);

	// Per-team scale + font override for text inside the pip. PipFont defaults
	// to SmallFont; PipFontExtra is the nchud FontSz multiplier (headline 4K
	// legibility knob). Both are no-ops until the user touches the picker.
	const float PortraitTextScale = NCPlusHUDDrawCall::GetScale(PortraitAlias);
	UFont* PipFont = NCPlusHUDFonts::Resolve(PortraitAlias, this, MediumFont);
	if (!PipFont) PipFont = MediumFont;
	const float PipFontExtra = NCPlusHUDFonts::ResolveScale(PortraitAlias, 1.f);

	// Layer 5: Red "X" on dead portraits — always (no respawn this round)
	if (!bPlayerAlive)
	{
		const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * PortraitTextScale * PipFontExtra;
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;

		FString XStr = TEXT("X");
		float XL, YL;
		Canvas->StrLen(PipFont, XStr, XL, YL);

		Canvas->SetLinearDrawColor(Tinted(FLinearColor(1.f, 0.2f, 0.2f, 0.9f)));
		Canvas->DrawText(PipFont, FText::FromString(XStr),
			XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
	}

	// Layer 6: HP/Armor numbers — alive teammates (not self). ALSO revealed on
	// enemy pips once the round is over (the strip only draws in
	// InProgress/RoundCooldown, so != InProgress IS the round-win window — the
	// winners' final health should be readable), and at ALL times for TRUE
	// spectators (bOnlySpectator; deliberately NOT bOutOfLives — a dead player
	// must not gain live enemy info mid-round).
	if (bPlayerAlive && UTPlayerOwner)
	{
		AUTPlayerState* MyPS = FramePlayer ? PlayerSnapshot.LocalPS
			: Cast<AUTPlayerState>(UTPlayerOwner->PlayerState);
		AUTGameState* MatchGS = GS;
		const bool bSameTeam  = MyPS && MyPS->GetTeamNum() == PlayerState->GetTeamNum();
		const bool bRoundOver = MatchGS && MatchGS->GetMatchState() != MatchState::InProgress;
		const bool bTrueSpec  = MyPS && MyPS->bOnlySpectator;
		if (MyPS && MyPS != PlayerState && (bSameTeam || bRoundOver || bTrueSpec))
		{
			AUTCharacter* UTC = FramePlayer ? FramePlayer->Character : PlayerState->GetUTCharacter();
			if (UTC && !UTC->IsDead())
			{
				const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * 0.7f * PortraitTextScale * PipFontExtra;

				const int32 HP = FramePlayer ? FramePlayer->Health : UTC->Health;
				const int32 Armor = FramePlayer ? FramePlayer->Armor : UTC->GetArmorAmount();

				// Rebuild the HP FText + measured extents only when HP/armor (or the font)
				// change — otherwise the string is identical frame to frame.
				FElimPipCache& PC = PipCacheByPS.FindOrAdd(PlayerState);
				if (PC.HpKeyHP != HP || PC.HpKeyAR != Armor || PC.HpFont != PipFont)
				{
					const FString HPStr = FString::Printf(TEXT("%d/%d"), HP, Armor);
					float XL, YL;
					Canvas->StrLen(PipFont, HPStr, XL, YL);
					PC.HpText   = FText::FromString(HPStr);
					PC.HpWidth  = XL;
					PC.HpHeight = YL;
					PC.HpKeyHP  = HP;
					PC.HpKeyAR  = Armor;
					PC.HpFont   = PipFont;
				}

				const float TextX = XOffset + (PipSize * 0.5f) - (PC.HpWidth * FontRenderScale * 0.5f);
				const float TextY = YOffset + PipHeight - (PC.HpHeight * FontRenderScale) - 2.f;
				NCPlusHUDDrawCall::DrawOutlinedText(Canvas, PipFont, PC.HpText, TextX, TextY,
					FontRenderScale, FLinearColor::White, FLinearColor::Black, Op);
			}
		}
	}
}

FLinearColor AElimPlusHUD::GetBaseHUDColor()
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


void AElimPlusHUD::DrawPreMatchTeamPreview()
{
	if (!Canvas || !SmallFont || !MediumFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	// Compute fade alpha based on match state.
	//   PlayerIntro      → full opacity (Alpha = 1.0)
	//   CountdownToBegin → fade 1.0 → 0.0 over PreviewFadeDurationSec, lined up
	//                      with the "3" countdown announcement (CountdownToBegin
	//                      starts ~3s before InProgress on standard configs).
	//   anything else    → don't draw.
	const FName State = GS->GetMatchState();
	float Alpha = 1.f;
	if (State == MatchState::PlayerIntro)
	{
		// Reset so the NEXT CountdownToBegin entry re-arms the fade timer.
		CountdownStartTimeSeconds = -1.f;
	}
	else if (State == MatchState::CountdownToBegin)
	{
		if (CountdownStartTimeSeconds < 0.f)
		{
			CountdownStartTimeSeconds = GetWorld()->GetTimeSeconds();
		}
		const float Elapsed = GetWorld()->GetTimeSeconds() - CountdownStartTimeSeconds;
		Alpha = FMath::Clamp(1.f - (Elapsed / PreviewFadeDurationSec), 0.f, 1.f);
	}
	else
	{
		return;
	}

	if (Alpha <= 0.f) return;

	// Helper: scale a color's alpha component by the fade alpha so all draws
	// fade uniformly. Multiplies in linear-alpha space — fine for our tile +
	// text mix since SetLinearDrawColor is the source of truth for both.
	auto Faded = [Alpha](FLinearColor C) { C.A *= Alpha; return C; };

	AElimPlusStatsReplicator* Stats = FindStatsReplicator(GetWorld());

	// Hide the preview overlay entirely when the admin disabled balancing
	// (?BalanceTeams=false on the server URL). No point telegraphing an
	// ELO-balanced split that isn't going to happen. We still let the fade
	// alpha be computed above so CountdownStartTimeSeconds gets reset
	// consistently; we just don't draw anything.
	if (!Stats || !Stats->IsBalanceTeamsActive())
	{
		return;
	}

	// Layout: centered panel, 60% width × 50% height.
	const float W  = float(Canvas->SizeX);
	const float H  = float(Canvas->SizeY);
	const float PW = W * 0.55f;
	const float PH = H * 0.45f;
	const float PX = (W - PW) * 0.5f;
	const float PY = H * 0.18f;
	const float TextScale = H / 1080.0f;

	// Background tile (dim black with alpha).
	Canvas->SetLinearDrawColor(Faded(FLinearColor(0.f, 0.f, 0.f, 0.75f)));
	Canvas->DrawTile(Canvas->DefaultTexture, PX, PY, PW, PH, 0, 0, 1, 1);

	// Top border accent.
	Canvas->SetLinearDrawColor(Faded(FLinearColor(1.f, 0.85f, 0.2f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, PX, PY, PW, 3.f, 0, 0, 1, 1);
	Canvas->DrawTile(Canvas->DefaultTexture, PX, PY + PH - 3.f, PW, 3.f, 0, 0, 1, 1);

	FFontRenderInfo RI;
	RI.bEnableShadow = true;

	// Title centered at top.
	{
		const FString Title = TEXT("MATCH BALANCE");
		float TXL, TYL;
		Canvas->StrLen(MediumFont, Title, TXL, TYL);
		const float TitleScale = TextScale * 1.1f;
		Canvas->SetLinearDrawColor(Faded(FLinearColor::White));
		Canvas->DrawText(MediumFont, FText::FromString(Title),
			PX + (PW - TXL * TitleScale) * 0.5f, PY + 14.f,
			TitleScale, TitleScale, RI);
	}

	// Two columns: RED / BLUE.
	const float ColW = PW * 0.5f;
	const float HeaderY = PY + 60.f * TextScale;
	const float RowY0   = PY + 110.f * TextScale;
	const float RowH    = 32.f * TextScale;

	auto DrawTeamColumn = [&](int32 TeamIdx, float ColX, const TCHAR* Label, FLinearColor Accent)
	{
		// Header
		Canvas->SetLinearDrawColor(Faded(Accent));
		Canvas->DrawText(MediumFont, FText::FromString(Label),
			ColX + 24.f * TextScale, HeaderY, TextScale, TextScale, RI);

		// "ELO" column header — right-aligned to match the per-row ELO values below,
		// and vertically centered against the larger MediumFont team-name label so
		// both header labels sit on a shared centerline (they were top-aligned at
		// HeaderY, which left the smaller ELO floating high next to RED/BLUE).
		{
			const FString EloHdr = TEXT("ELO");
			float HXL, HYL;
			Canvas->StrLen(SmallFont, EloHdr, HXL, HYL);
			float LXL, LYL;
			Canvas->StrLen(MediumFont, FString(Label), LXL, LYL);
			const float EloHdrY = HeaderY + (LYL - HYL) * 0.5f * TextScale;
			Canvas->SetLinearDrawColor(Faded(Accent));
			Canvas->DrawText(SmallFont, FText::FromString(EloHdr),
				ColX + ColW - 24.f * TextScale - HXL * TextScale, EloHdrY, TextScale, TextScale, RI);
		}

		// Player rows
		float Y = RowY0;
		int64 TeamStrength = 0;
		int32 RowCount = 0;
		for (APlayerState* PS : GS->PlayerArray)
		{
			AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
			if (!UTPS || UTPS->bOnlySpectator) continue;
			if (UTPS->GetTeamNum() != TeamIdx) continue;

			const FString Key = UTPS->UniqueId.IsValid()
				? UTPS->UniqueId.ToString()
				: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
			const int32 Elo = Stats ? Stats->GetEloForPlayer(Key) : 1400;
			TeamStrength += Elo;

			// Player name (left) + ELO (right of column)
			Canvas->SetLinearDrawColor(Faded(FLinearColor::White));
			Canvas->DrawText(SmallFont, FText::FromString(UTPS->PlayerName),
				ColX + 24.f * TextScale, Y, TextScale, TextScale, RI);

			const FString EloStr = FString::Printf(TEXT("%d"), Elo);
			float EXL, EYL;
			Canvas->StrLen(SmallFont, EloStr, EXL, EYL);
			Canvas->SetLinearDrawColor(Faded(Accent));
			Canvas->DrawText(SmallFont, FText::FromString(EloStr),
				ColX + ColW - 24.f * TextScale - EXL * TextScale, Y, TextScale, TextScale, RI);

			Y += RowH;
			++RowCount;
		}

		// Total strength at bottom of column
		const float TotalY = PY + PH - 50.f * TextScale;
		const FString TotalStr = FString::Printf(TEXT("Team Strength: %lld"), TeamStrength);
		Canvas->SetLinearDrawColor(Faded(Accent));
		Canvas->DrawText(MediumFont, FText::FromString(TotalStr),
			ColX + 24.f * TextScale, TotalY, TextScale, TextScale, RI);
	};

	// Faction suffix only when custom team colors are actually in use (gated on the
	// scorebar Team-Color toggle) — otherwise plain RED/BLUE, matching the scorebar +
	// scoreboards. Kills the "am I red, blue, or phayder?" confusion.
	bool bCustomColors = false;
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
	if (bUseTeamColor && GS->Teams.IsValidIndex(0) && GS->Teams[0])
	{
		const FLinearColor TC = GS->Teams[0]->TeamColor;
		if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f) bCustomColors = true;
	}
	if (bUseTeamColor && GS->Teams.IsValidIndex(1) && GS->Teams[1])
	{
		const FLinearColor TC = GS->Teams[1]->TeamColor;
		if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f) bCustomColors = true;
	}
	const FString RedLabel  = bCustomColors ? TEXT("RED  (PHAYDER)") : TEXT("RED");
	const FString BlueLabel = bCustomColors ? TEXT("BLUE (LIANDRI)") : TEXT("BLUE");

	DrawTeamColumn(0, PX,         *RedLabel,  FLinearColor(1.f,  0.35f, 0.35f, 1.f));
	DrawTeamColumn(1, PX + ColW,  *BlueLabel, FLinearColor(0.4f, 1.f,   0.4f,  1.f));

	// Subtle divider between columns
	Canvas->SetLinearDrawColor(Faded(FLinearColor(1.f, 1.f, 1.f, 0.25f)));
	Canvas->DrawTile(Canvas->DefaultTexture, PX + ColW - 1.f, PY + 50.f * TextScale,
		2.f, PH - 90.f * TextScale, 0, 0, 1, 1);
}
