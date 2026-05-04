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
	// Custom split WeaponBar replaces stock bpHW_WeaponBar (see ElimPlusHUD.cpp).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"));
	// Modernized ammo counter — replaces stock bpHW_WeaponInfo (3 styles, fully editable).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"));
	// Removed bpHW_Paperdoll — fallback +HP/Armor mode would render on top of our widget.
	// Removed bpHW_TeamGameClock — we draw our own team score bar in DrawHUD
	// that respects dynamic team colors from TeamSkins.
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"));
	// Modernized HP/Armor display — replaces stock bpHW_QuickStats (5 styles).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"));
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

	// Take a high-res screenshot once when match ends (if enabled in NCP settings)
	if (!bPostMatchScreenshotTaken)
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS && GS->HasMatchEnded())
		{
			// Check Mod.ini setting
			FString Val;
			FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
			if (GConfig->GetString(TEXT("NetcodePlus"), TEXT("HighResScreenshotPostMatch"), Val, ConfigPath))
			{
				bNCPScreenshotEnabled = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
			}

			if (bNCPScreenshotEnabled)
			{
				// Delay slightly so the final scoreboard has a chance to render
				FTimerHandle ScreenshotTimer;
				GetWorldTimerManager().SetTimer(ScreenshotTimer, [this]()
				{
					if (GetWorld() && GetWorld()->GetFirstPlayerController())
					{
						GetWorld()->GetFirstPlayerController()->ConsoleCommand(TEXT("HighResShot 2"));
					}
				}, 1.5f, false);
			}

			bPostMatchScreenshotTaken = true;
		}
	}
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

void AWipeoutHUD::DrawHUD()
{
	// Re-apply the live layout each frame so Slate editor edits show up immediately.
	// Cheap when clean (dirty-flag gated).
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());

	Super::DrawHUD();

	// Guard: Canvas or fonts may be null during Slate UI overlays (e.g. weapon skins menu)
	if (!Canvas || !SmallFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	bool bScoreboardIsUp = ScoreboardIsUp();

	// ─── Custom team score bar (replaces bpHW_TeamGameClock) ───
	// Respects dynamic team colors from TeamSkins mutator.
	if (GS && !bScoreboardIsUp)
	{
		DrawTeamScoreBar(GS);
	}

	if (!bScoreboardIsUp && GS && GS->GetMatchState() == MatchState::InProgress)
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;
		float TeammateScale = 0.4f;

		float BasePipSize = (32 + (64 * TeammateScale)) * GetHUDWidgetScaleOverride() * RenderScale;
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

		// Per-strip grow direction — flips so portraits grow INWARD when anchored
		// to a screen edge (otherwise the strip extends off-screen). Default
		// preserves stock behavior: red grows left, blue grows right.
		const FVector2D RedAnchor  = FNCPlusHUDLayout::AnchorToScreenCoords(NCPlusHUDDrawCall::GetEffectiveAnchor(TEXT("portrait_red")));
		const FVector2D BlueAnchor = FNCPlusHUDLayout::AnchorToScreenCoords(NCPlusHUDDrawCall::GetEffectiveAnchor(TEXT("portrait_blue")));
		const float RedGrowSign   = (RedAnchor.X  < 0.25f) ? +1.f : -1.f;  // anchored left → grow right
		const float BlueGrowSign  = (BlueAnchor.X > 0.75f) ? -1.f : +1.f;  // anchored right → grow left

		float XOffsetRed  = RedStart.X;
		float XOffsetBlue = BlueStart.X;
		// Right-anchored strips: shift first portrait left by its width so its
		// right edge sits on the anchor instead of extending off-screen.
		if (RedAnchor.X  > 0.75f) XOffsetRed  -= BasePipSize;
		if (BlueAnchor.X > 0.75f) XOffsetBlue -= BasePipSize;
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
			float PipSize = BasePipSize * OwnerPipScaling;

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
				// Player name above icon
				{
					const float NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f;
					FFontRenderInfo NameRI;
					NameRI.bEnableShadow = true;
					FString Name = UTPS->PlayerName;
					float NXL, NYL;
					Canvas->StrLen(TinyFont, Name, NXL, NYL);
					// Truncate if wider than pip
					while (NXL * NameScale > PipSize && Name.Len() > 3)
					{
						Name = Name.Left(Name.Len() - 1);
						Canvas->StrLen(TinyFont, Name, NXL, NYL);
					}
					// Black outline + white fill (matches HP/Armor style)
					float NameX = XOffsetRed + (PipSize * 0.5f) - (NXL * NameScale * 0.5f);
					float NameY = YOffsetRed + 2.f;
					float OL = 1.f;
					Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 1.f));
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX - OL, NameY, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX + OL, NameY, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY - OL, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY + OL, NameScale, NameScale, NameRI);
					Canvas->SetLinearDrawColor(FLinearColor::White);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY, NameScale, NameScale, NameRI);
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
				// Player name above icon
				{
					const float NameScale = float(Canvas->SizeY) / 1080.0f * 0.55f;
					FFontRenderInfo NameRI;
					NameRI.bEnableShadow = true;
					FString Name = UTPS->PlayerName;
					float NXL, NYL;
					Canvas->StrLen(TinyFont, Name, NXL, NYL);
					while (NXL * NameScale > PipSize && Name.Len() > 3)
					{
						Name = Name.Left(Name.Len() - 1);
						Canvas->StrLen(TinyFont, Name, NXL, NYL);
					}
					// Black outline + white fill (matches HP/Armor style)
					float NameX = XOffsetBlue + (PipSize * 0.5f) - (NXL * NameScale * 0.5f);
					float NameY = YOffsetBlue + 2.f;
					float OL = 1.f;
					Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 1.f));
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX - OL, NameY, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX + OL, NameY, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY - OL, NameScale, NameScale, NameRI);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY + OL, NameScale, NameScale, NameRI);
					Canvas->SetLinearDrawColor(FLinearColor::White);
					Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY, NameScale, NameScale, NameRI);
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
		AUTPlayerState* MyPS = GetScorerPlayerState();
		if (MyPS && Canvas && SmallFont)
		{
			int32 Score = FMath::TruncToInt(MyPS->Score);
			int32 Kills = MyPS->Kills;
			int32 Deaths = MyPS->Deaths;
			int32 Assists = MyPS->KillAssists;

			FString ScoreStr = FString::Printf(TEXT("Score: %d"), Score);
			FString KDAStr = FString::Printf(TEXT("KDA: %d / %d / %d"), Kills, Deaths, Assists);

			float KDAXPos = Canvas->ClipX * 0.98f;
			float KDAYPos = Canvas->ClipY * 0.015f;
			float FontScale = RenderScale * 0.9f;

			// Score line
			float XL, YL;
			Canvas->TextSize(SmallFont, ScoreStr, XL, YL, FontScale, FontScale);
			Canvas->DrawColor = FColor(255, 255, 255, 220);
			Canvas->DrawText(SmallFont, ScoreStr, KDAXPos - XL, KDAYPos, FontScale, FontScale);
			KDAYPos += YL * 1.1f;

			// KDA line
			Canvas->TextSize(SmallFont, KDAStr, XL, YL, FontScale, FontScale);
			Canvas->DrawColor = FColor(200, 200, 200, 200);
			Canvas->DrawText(SmallFont, KDAStr, KDAXPos - XL, KDAYPos, FontScale, FontScale);
		}
	}
}

// ─── Custom team score bar ─────────────────────────────────────────────
// Draws team scores, clock, and "You are on X" text at the top center.
// Uses dynamic team colors from AUTTeamInfo::TeamColor instead of hardcoded red/blue.
// When non-standard team colors are detected, shows "Liandri" vs "Phayder" instead.
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
	FString Team0Name = bCustomColors ? TEXT("Liandri") : TEXT("RED");
	FString Team1Name = bCustomColors ? TEXT("Phayder") : TEXT("BLUE");

	// Scores
	int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Bar dimensions
	const float BarWidth = 220.f * RenderScale;
	const float BarHeight = 36.f * RenderScale;
	const float ScoreBoxWidth = 50.f * RenderScale;
	const float GapWidth = 8.f * RenderScale;

	// ── Team 0 (left side) ──
	float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(Team0Color);
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// Team 0 score box
	float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	// ── Team 1 (right side) ──
	float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(Team1Color);
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// ── Score color tails (extend below score box, width of the number) ──
	float TailHeight = 14.f * RenderScale;
	float TailAlpha = 1.f;

	// Team 0 tail
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, TailAlpha));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// Team 1 tail
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, TailAlpha));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// ── Center divider (white thin line) ──
	Canvas->SetLinearDrawColor(FLinearColor::White);
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	// ── Text ──
	float FontScale = RenderScale * 0.85f;
	float LargeFontScale = RenderScale * 1.2f;
	float XL, YL;

	// Team 0 name (right-aligned inside left bar)
	Canvas->TextSize(SmallFont, Team0Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team0Name, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// Team 0 score (centered in score box)
	FString Score0Str = FString::Printf(TEXT("%d"), Score0);
	Canvas->TextSize(LargeFont, Score0Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score0Str, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 score (centered in score box)
	FString Score1Str = FString::Printf(TEXT("%d"), Score1);
	Canvas->TextSize(LargeFont, Score1Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score1Str, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 name (left-aligned inside right bar)
	Canvas->TextSize(SmallFont, Team1Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team1Name, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// ── Round Clock (big, centered below bars) ──
	// Read RoundSecondsRemaining from the BP GameState via reflection
	float ClockY = TopY + BarHeight + 2.f * RenderScale;
	int32 RoundTime = -1;
	UIntProperty* RoundTimeProp = FindField<UIntProperty>(GS->GetClass(), TEXT("RoundSecondsRemaining"));
	if (RoundTimeProp)
	{
		RoundTime = RoundTimeProp->GetPropertyValue_InContainer(GS);
	}

	float RoundClockScale = RenderScale * 1.1f;
	if (RoundTime >= 0)
	{
		int32 RMins = RoundTime / 60;
		int32 RSecs = RoundTime % 60;
		FString RoundClockStr = FString::Printf(TEXT("%02d:%02d"), RMins, RSecs);
		Canvas->TextSize(MediumFont, RoundClockStr, XL, YL, RoundClockScale, RoundClockScale);
		// Flash red when under 30 seconds
		if (RoundTime <= 30)
			Canvas->DrawColor = FColor(255, 60, 60, 255);
		else
			Canvas->DrawColor = FColor::White;
		Canvas->DrawText(MediumFont, RoundClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
		ClockY += YL + 1.f * RenderScale;
	}

	// Removed: match elapsed timer and "You are on X" text — too cluttered
}

void AWipeoutHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float PipSize)
{
	const FCanvasIcon& CharIcon = PlayerState->GetHUDIcon();
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

	// Layer 5 (Wipeout-specific): Respawn countdown text on dead portraits
	if (LiveScaling < 1.f && PlayerState->RespawnTime > 0.f)
	{
		const float FontRenderScale = float(Canvas->SizeY) / 1080.0f;
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;

		int32 SecondsRemaining = FMath::CeilToInt(PlayerState->RespawnTime);
		FString CountdownStr = FString::Printf(TEXT("%i"), SecondsRemaining);
		float XL, YL;
		Canvas->StrLen(SmallFont, CountdownStr, XL, YL);

		// Team-tinted countdown color
		FLinearColor CountdownColor = (PlayerState->GetTeamNum() == 0)
			? FLinearColor(1.f, 0.4f, 0.4f, 1.f)    // Red team
			: FLinearColor(0.4f, 0.6f, 1.f, 1.f);     // Blue team

		Canvas->SetLinearDrawColor(Tinted(CountdownColor));
		Canvas->DrawText(SmallFont, FText::FromString(CountdownStr),
			XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
	}

	// Layer 5b: "X" on dead portraits with no respawn (OT / sudden death)
	if (LiveScaling < 1.f && PlayerState->RespawnTime <= 0.f && PlayerState->bOutOfLives)
	{
		const float FontRenderScale = float(Canvas->SizeY) / 1080.0f;
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;

		FString XStr = TEXT("X");
		float XL, YL;
		Canvas->StrLen(SmallFont, XStr, XL, YL);

		Canvas->SetLinearDrawColor(Tinted(FLinearColor(1.f, 0.2f, 0.2f, 0.9f)));
		Canvas->DrawText(SmallFont, FText::FromString(XStr),
			XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
	}

	// Layer 6: Teammate HP/Armor numbers (alive teammates only, not self)
	if (LiveScaling >= 1.f && UTPlayerOwner)
	{
		AUTPlayerState* MyPS = Cast<AUTPlayerState>(UTPlayerOwner->PlayerState);
		if (MyPS && MyPS != PlayerState && MyPS->GetTeamNum() == PlayerState->GetTeamNum())
		{
			AUTCharacter* UTC = PlayerState->GetUTCharacter();
			if (UTC && !UTC->IsDead())
			{
				const float FontRenderScale = float(Canvas->SizeY) / 1080.0f * 0.7f;
				FFontRenderInfo TextRenderInfo;
				TextRenderInfo.bEnableShadow = true;

				int32 HP = UTC->Health;
				int32 Armor = UTC->GetArmorAmount();
				FString HPStr = FString::Printf(TEXT("%d/%d"), HP, Armor);

				float XL, YL;
				Canvas->StrLen(SmallFont, HPStr, XL, YL);

				float TextX = XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f);
				float TextY = YOffset + PipHeight - (YL * FontRenderScale) - 2.f;

				// Black outline: draw text offset in 4 directions
				FLinearColor GoldOutline(0.f, 0.f, 0.f, 1.f);
				float OutlineOffset = 1.f;
				Canvas->SetLinearDrawColor(Tinted(GoldOutline));
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX - OutlineOffset, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX + OutlineOffset, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY - OutlineOffset, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY + OutlineOffset, FontRenderScale, FontRenderScale, TextRenderInfo);

				// White fill on top
				Canvas->SetLinearDrawColor(Tinted(FLinearColor::White));
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
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
