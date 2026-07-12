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
#include "NCPlusHUDLayout.h"
#include "NCPlusForceModels.h"   // DrawHeadDebug (ncp.DebugHeads)
#include "UTHUDWidget_Spectator.h"

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
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"));
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
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	AUTPlayerState* HUDPS = GetScorerPlayerState();
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
			UTPS->SelectionOrder = (UTPS == HUDPS) ? -1 : UTPS->SpectatingIDTeam;
			SortedPlayers.Add(UTPS);
		}
	}
	SortedPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B) { return A.SelectionOrder > B.SelectionOrder; });
}

// "NOW WATCHING <player>" spectator banner (verbatim port of ANCPlusCTFHUD::
// DrawSpectatorTarget). Bottom-right, suppressed by the caller when the
// scoreboard is up. This is the canonical viewed-player banner for both dead
// players and true spectators; DrawHUD suppresses the stock duplicate only for
// the frame where this banner can replace it.
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
	}

	// Keep portraits up through the round-win window ("RoundCooldown") so the
	// enemy team's final health stays visible after they win — not just InProgress.
	if (bShouldDrawPortraits && !bScoreboardIsUp && GS
		&& (GS->GetMatchState() == MatchState::InProgress
			|| GS->GetMatchState() == FName(TEXT("RoundCooldown"))))
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;

		float TeammateScale = 0.4f;

		float BasePipSize = (32 + (64 * TeammateScale)) * GetHUDWidgetScaleOverride() * RenderScale;
		// Phase 3.11: per-strip Scale override (independent red/blue resize).
		const float WO_RedScale  = NCPlusHUDDrawCall::GetScale(TEXT("portrait_red"));
		const float WO_BlueScale = NCPlusHUDDrawCall::GetScale(TEXT("portrait_blue"));
		const float WO_RedPipSize  = BasePipSize * WO_RedScale;
		const float WO_BluePipSize = BasePipSize * WO_BlueScale;
		float XAdjust = BasePipSize * 1.1f;

		// Stock positions used as fallbacks if the layout has no override.
		const float StockXRed  = 0.4f * Canvas->ClipX - XAdjust - BasePipSize;
		const float StockXBlue = 0.6f * Canvas->ClipX + XAdjust;
		const float StockY     = 0.005f * Canvas->ClipY * GetHUDWidgetScaleOverride() * RenderScale;

		// Layout consult (Phase 3.5) — user can move each strip independently.
		const FVector2D RedStart  = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("portrait_red"),  Canvas, FVector2D(StockXRed,  StockY));
		const FVector2D BlueStart = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("portrait_blue"), Canvas, FVector2D(StockXBlue, StockY));
		const bool bHideRed  = NCPlusHUDDrawCall::IsHidden(TEXT("portrait_red"));
		const bool bHideBlue = NCPlusHUDDrawCall::IsHidden(TEXT("portrait_blue"));

		// Per-strip grow direction — derived from RESOLVED screen position, not
		// the anchor coordinate. Lets dragging the strip across screen-center
		// (via nchud_drag) auto-flip growth so the visible strip stays on
		// screen instead of extending off the now-far edge.
		// Team-defaulted grow direction; flip only if default would clip
		// off-screen. See ElimPlusHUD for full rationale.
		const float EstStripWidth = 5.f * XAdjust;
		float RedGrowSign  = -1.f;
		float BlueGrowSign = +1.f;
		if (RedStart.X  - EstStripWidth < 0.f)             RedGrowSign  = +1.f;
		if (BlueStart.X + EstStripWidth > Canvas->ClipX)   BlueGrowSign = -1.f;

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

		TArray<AUTPlayerState*> LivePlayers;
		GetPlayerListForIcons(LivePlayers);

		// Pre-pass: find the next-to-spawn teammate (lowest RespawnTime > 0)
		AUTPlayerState* MyPS_ForSpawn = Cast<AUTPlayerState>(UTPlayerOwner ? UTPlayerOwner->PlayerState : nullptr);
		uint8 MyTeam = MyPS_ForSpawn ? MyPS_ForSpawn->GetTeamNum() : 255;
		AUTPlayerState* NextToSpawn = nullptr;
		float LowestRespawnTime = BIG_NUMBER;
		for (AUTPlayerState* UTPS : LivePlayers)
		{
			if (UTPS && UTPS != MyPS_ForSpawn && UTPS->GetTeamNum() == MyTeam
				&& UTPS->RespawnTime > 0.f && UTPS->RespawnTime < LowestRespawnTime)
			{
				AUTCharacter* UTC = UTPS->GetUTCharacter();
				bool bDead = !UTC || UTC->IsDead();
				if (bDead)
				{
					LowestRespawnTime = UTPS->RespawnTime;
					NextToSpawn = UTPS;
				}
			}
		}

		for (AUTPlayerState* UTPS : LivePlayers)
		{
			// In Wipeout everyone respawns, so show all non-spectator players
			float OwnerPipScaling = (UTPS == GetScorerPlayerState()) ? 1.25f : 1.f;
			// Per-team Scale: red strip honors portrait_red.Scale, blue honors blue.
			const uint8 PreTeamIdx = UTPS ? UTPS->GetTeamNum() : 255;
			const float TeamPipBase = (PreTeamIdx == 1) ? WO_BluePipSize : WO_RedPipSize;
			float PipSize = TeamPipBase * OwnerPipScaling;

			// Determine if player is alive.
			// On the server, check the controller's current pawn directly
			// (GetUTCharacter() intentionally caches dead characters).
			// On clients, the controller (GetOwner) is null for remote players,
			// so fall back to GetUTCharacter() + IsDead().
			bool bPlayerAlive = false;
			AController* C = Cast<AController>(UTPS->GetOwner());
			if (C != nullptr)
			{
				// Server or local player — check controller's actual pawn
				AUTCharacter* UTC = Cast<AUTCharacter>(C->GetPawn());
				bPlayerAlive = (UTC != nullptr && !UTC->IsDead());
			}
			else
			{
				// Remote player on client — use GetUTCharacter + IsDead
				AUTCharacter* UTC = UTPS->GetUTCharacter();
				bPlayerAlive = (UTC != nullptr && !UTC->IsDead());
			}

			// Respawn progress: 0 = fully dead/waiting, 1 = alive
			float LiveScaling = 1.f;
			if (!bPlayerAlive && UTPS->RespawnTime > 0.f && UTPS->RespawnWaitTime > 0.f)
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
			if (TeamIdx == 0)
			{
				RedPlayerCount++;
				DrawPlayerIcon(UTPS, LiveScaling, XOffsetRed, YOffsetRed, PipSize);
				// Player name above icon — multiply by team scale so text shrinks with the pip.
				{
					/* Portraits (Red) Font + FontSz restyle the player name. */ UFont* NameFont = NCPlusHUDFonts::Resolve(FName(TEXT("portrait_red")), this, SmallFont); if (!NameFont) { NameFont = SmallFont; } const float NameFontExtra = NCPlusHUDFonts::ResolveScale(FName(TEXT("portrait_red")), 1.f);
					const float NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f * WO_RedScale * NameFontExtra;
					// Cached fit + one outlined item (was a per-frame StrLen loop + 5 DrawText).
					FText NameText; float NW, NH;
					NCPlusHUDDrawCall::ResolveFittedName(Canvas, UTPS, NameFont, UTPS->PlayerName, PipSize, NameScale, NameText, NW, NH);
					float NameX = XOffsetRed + (PipSize * 0.5f) - (NW * NameScale * 0.5f);
					float NameY = YOffsetRed + 2.f;
					NCPlusHUDDrawCall::DrawOutlinedText(Canvas, NameFont, NameText, NameX, NameY, NameScale, FLinearColor::White);
				}
				if (UTPS == NextToSpawn)
				{
					// Gold border highlight for next teammate to spawn
					float PipHeight = PipSize * (320.0f / 224.0f);
					FLinearColor Gold(1.f, 0.85f, 0.f, 0.9f);
					float BorderW = 2.f;
					Canvas->SetLinearDrawColor(Gold);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetRed, YOffsetRed, PipSize, BorderW, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetRed, YOffsetRed + PipHeight - BorderW, PipSize, BorderW, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetRed, YOffsetRed, BorderW, PipHeight, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetRed + PipSize - BorderW, YOffsetRed, BorderW, PipHeight, 0, 0, 1, 1);
				}
				XOffsetRed += RedGrowSign * 1.1f * PipSize;
			}
			else if (TeamIdx == 1)
			{
				BluePlayerCount++;
				DrawPlayerIcon(UTPS, LiveScaling, XOffsetBlue, YOffsetBlue, PipSize);
				// Player name above icon — multiply by team scale so text shrinks with the pip.
				{
					/* Portraits (Blue) Font + FontSz restyle the player name. */ UFont* NameFont = NCPlusHUDFonts::Resolve(FName(TEXT("portrait_blue")), this, SmallFont); if (!NameFont) { NameFont = SmallFont; } const float NameFontExtra = NCPlusHUDFonts::ResolveScale(FName(TEXT("portrait_blue")), 1.f);
					const float NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f * WO_BlueScale * NameFontExtra;
					// Cached fit + one outlined item (was a per-frame StrLen loop + 5 DrawText).
					FText NameText; float NW, NH;
					NCPlusHUDDrawCall::ResolveFittedName(Canvas, UTPS, NameFont, UTPS->PlayerName, PipSize, NameScale, NameText, NW, NH);
					float NameX = XOffsetBlue + (PipSize * 0.5f) - (NW * NameScale * 0.5f);
					float NameY = YOffsetBlue + 2.f;
					NCPlusHUDDrawCall::DrawOutlinedText(Canvas, NameFont, NameText, NameX, NameY, NameScale, FLinearColor::White);
				}
				if (UTPS == NextToSpawn)
				{
					float PipHeight = PipSize * (320.0f / 224.0f);
					FLinearColor Gold(1.f, 0.85f, 0.f, 0.9f);
					float BorderW = 2.f;
					Canvas->SetLinearDrawColor(Gold);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetBlue, YOffsetBlue, PipSize, BorderW, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetBlue, YOffsetBlue + PipHeight - BorderW, PipSize, BorderW, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetBlue, YOffsetBlue, BorderW, PipHeight, 0, 0, 1, 1);
					Canvas->DrawTile(Canvas->DefaultTexture, XOffsetBlue + PipSize - BorderW, YOffsetBlue, BorderW, PipHeight, 0, 0, 1, 1);
				}
				XOffsetBlue += BlueGrowSign * 1.1f * PipSize;
			}
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
// Draws team scores, clock, and "You are on X" text at the top center.
// Uses dynamic team colors from AUTTeamInfo::TeamColor instead of hardcoded red/blue.
// When non-standard team colors are detected, shows "Phayder" (red) vs "Liandri" (blue).
void AWipeoutHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !SmallFont || !MediumFont || !LargeFont) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;

	// Phase 3.5 layout consult — anchor + offset for the whole scorebar.
	// Stock placement: top-center, 2px from top edge.
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;

	// Get team colors (respect TeamSkins custom colors).
	// Honors scorebar's `use_team_color` extra: when false, locks to stock red/blue.
	FLinearColor Team0Color = FLinearColor(0.8f, 0.05f, 0.05f, 1.f); // default red
	FLinearColor Team1Color = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);  // default blue
	bool bCustomColors = false;
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));

	if (bUseTeamColor && GS->Teams.IsValidIndex(0) && GS->Teams[0])
	{
		FLinearColor TC = GS->Teams[0]->TeamColor;
		// Check if non-standard (not close to pure red)
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

	// Team names
	static const FString CustomTeam0Name(TEXT("Phayder (R)"));
	static const FString CustomTeam1Name(TEXT("Liandri (B)"));
	static const FString StockTeam0Name(TEXT("RED"));
	static const FString StockTeam1Name(TEXT("BLUE"));
	const FString& Team0Name = bCustomColors ? CustomTeam0Name : StockTeam0Name;
	const FString& Team1Name = bCustomColors ? CustomTeam1Name : StockTeam1Name;

	// Scores
	int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Bar dimensions
	// Phase 3.11: scorebar Scale override scales the bar + clock font uniformly.
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float BarWidth = 220.f * RenderScale * ScoreScale;
	const float BarHeight = 36.f * RenderScale * ScoreScale;
	const float ScoreBoxWidth = 50.f * RenderScale * ScoreScale;
	const float GapWidth = 8.f * RenderScale;

	// Per-element opacity (the editor's Op slider). FadeL scales every tile color's
	// alpha; WhiteOp is the faded text color. Op defaults to 1.0 (no override) so
	// untouched layouts render pixel-identically.
	const float ScoreOp = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	auto FadeL = [ScoreOp](FLinearColor C) -> FLinearColor { C.A *= ScoreOp; return C; };
	const FColor WhiteOp(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(ScoreOp * 255.f), 0, 255));

	// ── Team 0 (left side) ──
	float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(FadeL(Team0Color));
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// Team 0 score box
	float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	// ── Team 1 (right side) ──
	float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(Team1Color));
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// ── Score color tails (extend below score box, width of the number) ──
	float TailHeight = 14.f * RenderScale * ScoreScale;
	float TailAlpha = 1.f;

	// Team 0 tail
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, TailAlpha)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// Team 1 tail
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, TailAlpha)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// ── Center divider (white thin line) ──
	Canvas->SetLinearDrawColor(FadeL(FLinearColor::White));
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	// ── Text ──
	// Honor per-element font override (Phase 3.8). One alias controls both
	// team-name and score-number fonts on the scorebar — they read as a
	// unit, so a single setting keeps the typography coherent.
	UFont* TeamNameFont  = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, SmallFont);
	UFont* TeamScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, LargeFont);
	if (!TeamNameFont)  TeamNameFont  = SmallFont;
	if (!TeamScoreFont) TeamScoreFont = LargeFont;

	// font_scale Extras lets the user shrink/grow text independently of
	// bar dimensions. Useful for tall fonts (Extreme) where ScoreScale alone
	// doesn't shrink LargeFont enough (1.2 * 0.85 ≈ 1.02 = barely changed).
	const float FontExtraScale = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	float FontScale = RenderScale * 0.85f * ScoreScale * FontExtraScale;
	float LargeFontScale = RenderScale * 1.2f * ScoreScale * FontExtraScale;
	float XL, YL;

	// Team 0 name (right-aligned inside left bar)
	FText ResolvedText;
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team0Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	// Team 0 score (centered in score box)
	static bool bHasCachedScore0 = false;
	static int32 CachedScore0 = 0;
	static FString Score0Str;
	if (!bHasCachedScore0 || CachedScore0 != Score0) { bHasCachedScore0 = true; CachedScore0 = Score0; Score0Str = FString::FromInt(Score0); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score0Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	// Team 1 score (centered in score box)
	static bool bHasCachedScore1 = false;
	static int32 CachedScore1 = 0;
	static FString Score1Str;
	if (!bHasCachedScore1 || CachedScore1 != Score1) { bHasCachedScore1 = true; CachedScore1 = Score1; Score1Str = FString::FromInt(Score1); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score1Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	// Team 1 name (left-aligned inside right bar)
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team1Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	// ── Clock (big, centered below bars) ──
	// Round-based modes (Wipeout, ElimPlus) replicate RoundSecondsRemaining
	// on a BP GameState — read it via reflection. Time-based modes (Duel,
	// CTF, etc.) use stock AUTGameState::RemainingTime instead. Try the BP
	// field first; fall back to the stock match clock so non-round modes
	// (NCLeagueDuel especially) still get a visible timer.
	//
	// Static caches: UClass field tables don't change at runtime, so a single
	// FindField per (GameState class, property name) is enough. Stock
	// FindField walks the class hierarchy on every call; this is the cheap
	// trick for hot HUD paths that read replicated ints by name.
	float ClockY = TopY + BarHeight + 2.f * RenderScale;
	int32 ClockSeconds = -1;
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
		// TimeLimit (default 0 → no clock) for untimed modes. Skip drawing
		// when 0/negative so untimed modes don't show "00:00" glued to the
		// score bar.
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
			if (RT > 0) ClockSeconds = RT;
		}
	}

	float RoundClockScale = RenderScale * 1.1f * ScoreScale * FontExtraScale;
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
		NCPlusHUDDrawCall::ResolveStableText(Canvas, MediumFont, RoundClockStr, RoundClockScale, RoundClockScale, ResolvedText, XL, YL);
		// Flash red when under 30 seconds
		if (ClockSeconds <= 30)
			Canvas->DrawColor = FColor(255, 60, 60, WhiteOp.A);
		else
			Canvas->DrawColor = WhiteOp;
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
		ClockY += YL + 1.f * RenderScale;
	}

	// Removed: match elapsed timer and "You are on X" text — too cluttered
}

void AWipeoutHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float PipSize)
{
	const FCanvasIcon CharIcon = NCPlusHUDPortraits::Resolve(PlayerState);
	if (CharIcon.Texture == nullptr)
	{
		return;
	}

	// Per-portrait opacity (Phase 3.5+): scale every SetLinearDrawColor alpha
	// by this so the editor's Op slider fades the entire portrait stack.
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

	// Layer 1: Team-colored background. Honors per-portrait use_team_color
	// extra: false → lock to stock red/blue.
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	const bool bUseTeamColor = NCPlusHUDDrawCall::GetUseTeamColor(PortraitAlias);
	FLinearColor TeamBGColor = (PlayerState->GetTeamNum() == 1)
		? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)    // fallback blue
		: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);     // fallback red
	if (bUseTeamColor && GS && GS->Teams.IsValidIndex(PlayerState->GetTeamNum()) && GS->Teams[PlayerState->GetTeamNum()])
	{
		TeamBGColor = GS->Teams[PlayerState->GetTeamNum()]->TeamColor;
	}
	// Draw solid colored rectangle as background
	Canvas->SetLinearDrawColor(Tinted(TeamBGColor));
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight,
		0, 0, 1, 1);
	Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));

	// Layer 2: Character portrait (dimmed if dead)
	if (LiveScaling < 1.f)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor(0.2f, 0.2f, 0.2f, 1.f)));
	}

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

	// Layer 3: Respawn dark overlay sweeping from right to left
	if (LiveScaling < 1.f)
	{
		Canvas->SetLinearDrawColor(Tinted(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f)));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XOffset + LiveScaling * PipSize, YOffset,
			PipSize - LiveScaling * PipSize, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));
	const FCanvasIcon& OverlayIcon = PlayerState->GetTeamNum() == 1 ? BlueTeamOverlay : RedTeamOverlay;
	Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
		OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);

	// Per-team scale for text inside the pip — keeps countdown / X / HP/Armor
	// glyphs proportional when the strip is shrunk via the layout's Sc spinner.
	const float PortraitTextScale = NCPlusHUDDrawCall::GetScale(PortraitAlias);
	// Per-team font override + size multiplier (HP/Armor numbers are the loudest
	// readability concern at 4K). Both default to SmallFont / 1.0 so behavior is
	// identical when the user hasn't touched the picker.
	UFont* PipFont = NCPlusHUDFonts::Resolve(PortraitAlias, this, MediumFont);
	if (!PipFont) PipFont = MediumFont;
	const float PipFontExtra = NCPlusHUDFonts::ResolveScale(PortraitAlias, 1.f);

	// Layer 5 (Wipeout-specific): Respawn countdown text on dead portraits
	if (LiveScaling < 1.f && PlayerState->RespawnTime > 0.f)
	{
		const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * PortraitTextScale * PipFontExtra;

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

	// Layer 5b: "X" on dead portraits with no respawn (OT / sudden death)
	if (LiveScaling < 1.f && PlayerState->RespawnTime <= 0.f && PlayerState->bOutOfLives)
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
	// PipFontExtra is the user's FontSz multiplier — the headline 4K legibility knob.
	if (LiveScaling >= 1.f && UTPlayerOwner)
	{
		AUTPlayerState* MyPS = Cast<AUTPlayerState>(UTPlayerOwner->PlayerState);
		AUTGameState* MatchGS = GetWorld()->GetGameState<AUTGameState>();
		const bool bSameTeam  = MyPS && MyPS->GetTeamNum() == PlayerState->GetTeamNum();
		const bool bRoundOver = MatchGS && MatchGS->GetMatchState() != MatchState::InProgress;
		const bool bTrueSpec  = MyPS && MyPS->bOnlySpectator;
		if (MyPS && MyPS != PlayerState && (bSameTeam || bRoundOver || bTrueSpec))
		{
			AUTCharacter* UTC = PlayerState->GetUTCharacter();
			if (UTC && !UTC->IsDead())
			{
				const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * 0.7f * PortraitTextScale * PipFontExtra;

				int32 HP = UTC->Health;
				int32 Armor = UTC->GetArmorAmount();

				// Rebuild the HP FText + measured extents only when HP/armor (or the font)
				// change — otherwise the string is identical frame to frame.
				FWipeoutPipCache& PC = PipCacheByPS.FindOrAdd(PlayerState);
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

				float TextX = XOffset + (PipSize * 0.5f) - (PC.HpWidth * FontRenderScale * 0.5f);
				float TextY = YOffset + PipHeight - (PC.HpHeight * FontRenderScale) - 2.f;
				NCPlusHUDDrawCall::DrawOutlinedText(Canvas, PipFont, PC.HpText, TextX, TextY,
					FontRenderScale, FLinearColor::White, FLinearColor::Black, Op);
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
