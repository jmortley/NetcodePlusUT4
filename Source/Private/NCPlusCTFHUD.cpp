// NCPlusCTFHUD.cpp — Custom CTF HUD with team score bar and mouse focus fix.
#include "NCPlusCTFHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTHUDWidget_CTFFlagStatus.h"
#include "NCPlusHUDWidget_CTFFlagStatus.h"
#include "NCPlusHUDLayout.h"
#include "NCPlusCTFOTInfo.h"
#include "NCPlusCTFGameMode.h"   // NCPlusReflection helpers (bPlayingAdvantage / bSecondHalf)
#include "NCPlusSpectatorSlideOut.h"
#include "UTHUDWidget_SpectatorSlideOut.h"
#include "EngineUtils.h"

ANCPlusCTFHUD::ANCPlusCTFHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// AUTHUD_CTF constructor sets up RequiredHudWidgetClasses with all stock
	// CTF widgets (flag status, minimap, etc). We add our scoreboard and remove
	// the stock team clock since we draw our own.
	//
	// The stock clock is bpHW_TeamGameClock — loaded via RequiredHudWidgetClasses
	// in the parent. We can't easily remove it from there, so we'll just draw our
	// bar on top. The stock clock's small footprint is acceptable as fallback.

	// Our custom scoreboard widget
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusCTFScoreboard"));
	// Bottom bar: NCPlus custom only when NOT in stock mode. In stock mode the parent
	// AUTHUD_CTF's RequiredHudWidgetClasses stock widgets are kept (the BeginPlay strip
	// below is skipped for them), so we add nothing here. Only ONE family ever loads.
	if (!FNCPlusHUDLayout::WantsStockBottomBar())
	{
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"));
		HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"));
	}
	// Optional opt-in accuracy widget — hidden by default (ShouldDraw requires
	// a layout entry); user enables via nchud and picks current/specific weapon.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"));
	// Optional default-hidden overlays — see WipeoutHUD for full notes.
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Speedometer"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Minimap"));
	// NCPlus CTF flag-status subclass — drops in for the stock engine widget.
	// Adds nchud control over carrier indicator + you-have-flag banner + the
	// NEW enemy-has-flag banner (engine had the FText defined but never rendered).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_CTFFlagStatus"));
}

void ANCPlusCTFHUD::AddSpectatorWidgets()
{
	Super::AddSpectatorWidgets();

	// Replace the stock spectator slide-out with our subclass. In instagib (iCTF)
	// the weapon-stats panel then shows only the instagib rifle with accuracy from
	// the replicated NCAccuracyStatsReplicator (stock reads server-only StatsData,
	// which is 0 on dedicated-server spectators, and enumerated the empty map
	// pickup list). Non-instagib CTF defers to stock behaviour inside the widget.
	// SpectatorHudWidgetClasses (base UTHUD ini section) holds exactly the stock
	// slide-out — remove that one instance (exact-class so we never drop our own).
	if (SpectatorSlideOutWidget && SpectatorSlideOutWidget->GetClass() == UUTHUDWidget_SpectatorSlideOut::StaticClass())
	{
		HudWidgets.Remove(SpectatorSlideOutWidget);
		SpectatorSlideOutWidget = nullptr;
	}
	if (UUTHUDWidget* W = AddHudWidget(UNCPlusSpectatorSlideOut::StaticClass()))
	{
		if (UNCPlusSpectatorSlideOut* SlideOut = Cast<UNCPlusSpectatorSlideOut>(W))
		{
			SlideOut->WeaponListMode = ENCSlideOutWeaponMode::CTFAuto;
		}
	}
}

void ANCPlusCTFHUD::BeginPlay()
{
	// Remove stock widgets we're replacing BEFORE Super::BeginPlay instantiates them.
	// RequiredHudWidgetClasses is loaded from DefaultGame.ini config.
	// Stock bottom-bar widgets are stripped ONLY when we're drawing the NCPlus ones;
	// in stock-bottom-bar mode we keep the parent's stock weapon/ammo/health widgets.
	const bool bStockBottom = FNCPlusHUDLayout::WantsStockBottomBar();
	RequiredHudWidgetClasses.RemoveAll([bStockBottom](const FString& Entry)
	{
		// Always replaced — our scorebar / scoreboard / flag-status supersede these
		// regardless of which bottom bar is active.
		if (Entry.Contains(TEXT("TeamGameClock"))     // stock team score/clock bar
			|| Entry.Contains(TEXT("CTFScoreboard"))  // stock CTF scoreboard
			|| Entry.Contains(TEXT("TeamScoreboard")) // stock team scoreboard (fallback)
			// The stock CTF flag status widget is registered in DefaultGame.ini as
			// /Game/RestrictedAssets/UI/HUDWidgets/bpHW_CTFFlagStatus.bpHW_CTFFlagStatus_C
			// (a BP wrapper around UTHUDWidget_CTFFlagStatus). Matching "CTFFlagStatus"
			// catches both the BP path and any future C++ entry. Our subclass is added
			// via HudWidgetClasses above so this strip removes only the stock one.
			|| Entry.Contains(TEXT("CTFFlagStatus")))
		{
			return true;
		}
		// Bottom bar (weapon/ammo/health): strip the stock widgets only when we're
		// drawing the NCPlus replacements. In stock mode, keep them so they draw.
		if (!bStockBottom)
		{
			return Entry.Contains(TEXT("bpHW_WeaponBar"))    // replaced by our split bar
				|| Entry.Contains(TEXT("bpHW_QuickStats"))   // replaced by our HP/Armor widget
				|| Entry.Contains(TEXT("bpHW_Paperdoll"))    // fallback +HP/Armor mode would conflict
				|| Entry.Contains(TEXT("bpHW_WeaponInfo"))   // replaced by our ammo counter
				|| Entry.Contains(TEXT("bpHW_Powerups"));    // replaced by NCPlusHUDDrawCall::DrawHeldPowerups
		}
		return false;
	});

	Super::BeginPlay();

	// HUD layout system — capture stock defaults, load + apply live layout.
	CaptureWidgetDefaults(this);
	FNCPlusHUDLayout::ReloadLive();
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());

	// CDO-copy fallback: our C++ subclass inherits behavior from
	// UUTHUDWidget_CTFFlagStatus but the engine widget leaves the render-template
	// UPROPERTYs (FlagIconTemplate.Atlas, CircleTemplate.Atlas, FlagStatusText.Font,
	// etc.) unset in C++; they are configured in bpHW_CTFFlagStatus's BP defaults.
	// Without those assets our DrawFlagWorld and DrawStatusMessage overrides have
	// nothing to render with. Pull the BP CDO at runtime and copy its template
	// values onto our live instance so we render with the same assets the stock BP
	// would have provided.
	UClass* StockBPClass = LoadObject<UClass>(nullptr,
		TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_CTFFlagStatus.bpHW_CTFFlagStatus_C"));
	UUTHUDWidget_CTFFlagStatus* StockCDO = StockBPClass
		? Cast<UUTHUDWidget_CTFFlagStatus>(StockBPClass->GetDefaultObject())
		: nullptr;

	if (StockCDO)
	{
		for (UUTHUDWidget* W : HudWidgets)
		{
			UNCPlusHUDWidget_CTFFlagStatus* Ours = Cast<UNCPlusHUDWidget_CTFFlagStatus>(W);
			if (!Ours) continue;
			// Render templates (textures + UV).
			Ours->FlagIconTemplate       = StockCDO->FlagIconTemplate;
			Ours->CircleTemplate         = StockCDO->CircleTemplate;
			Ours->CircleBorderTemplate   = StockCDO->CircleBorderTemplate;
			Ours->TakenIconTemplate      = StockCDO->TakenIconTemplate;
			Ours->DroppedIconTemplate    = StockCDO->DroppedIconTemplate;
			Ours->FlagGoneIconTemplate   = StockCDO->FlagGoneIconTemplate;
			Ours->ArrowTemplate          = StockCDO->ArrowTemplate;
			Ours->CameraIconTemplate     = StockCDO->CameraIconTemplate;
			// Text render objects (font + scale + position).
			Ours->FlagHolderNameTemplate = StockCDO->FlagHolderNameTemplate;
			Ours->FlagStatusText         = StockCDO->FlagStatusText;
			Ours->RallyText              = StockCDO->RallyText;
			// Behavior knobs that the BP commonly overrides — keep them aligned.
			Ours->MaxIconScale           = StockCDO->MaxIconScale;
			Ours->InWorldAlpha           = StockCDO->InWorldAlpha;
			Ours->TopEdgePadding         = StockCDO->TopEdgePadding;
			Ours->BottomEdgePadding      = StockCDO->BottomEdgePadding;
			Ours->LeftEdgePadding        = StockCDO->LeftEdgePadding;
			Ours->RightEdgePadding       = StockCDO->RightEdgePadding;
			Ours->TeamPositions          = StockCDO->TeamPositions;
			// TeamPositions is a BP-only UPROPERTY (never set in C++). The engine's
			// DrawIndicators gates its WHOLE loop — silhouettes AND the carrier
			// indicator — on TeamPositions.IsValidIndex(Team), so if the BP CDO
			// didn't carry two entries the carrier indicator can never draw. Seed
			// defaults when absent. The values only position the silhouettes (which
			// the ctf_flag_status alias can hide); the carrier indicator computes its
			// own world-projected position and ignores them.
			if (Ours->TeamPositions.Num() < 2)
			{
				Ours->TeamPositions.Empty();
				Ours->TeamPositions.Add(FVector2D(-150.f, 30.f));
				Ours->TeamPositions.Add(FVector2D( 150.f, 30.f));
			}
			break;
		}
	}
}

EInputMode::Type ANCPlusCTFHUD::GetInputMode_Implementation() const
{
	// Phase 4.0: drag overlay (nchud_drag) needs cursor freed for Slate input.
	// Checked first so it can override the in-match GameOnly forcing below.
	if (NCPlusHUDDragMode::IsActive())
	{
		return EInputMode::EIM_GameAndUI;
	}

	// Same pattern as WipeoutHUD: keep mouse captured during match.
	// CTF doesn't have elimination, but this prevents click-off during
	// death spectating and halftime transitions.
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

void ANCPlusCTFHUD::DrawHUD()
{
	// Re-apply live layout each frame for live preview (cheap when clean).
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());

	// Auto post-match high-res screenshot (shared; fires after the replay ends + the scoreboard settles).
	// Before the Canvas guard so it polls consistently with ElimPlus/Wipeout (it doesn't touch Canvas).
	NCPlusHUDDrawCall::ServicePostMatchScreenshot(this, PostMatchScreenshotStable, bPostMatchScreenshotTaken);
	const bool bRenderCustomHUD = bShowUTHUD && UTPlayerOwner
		&& (bShowHUD || !UTPlayerOwner->bCinematicMode);

	if (!Canvas || !SmallFont) return;

	// Draw score bar BEFORE Super so flag icons (drawn by Super) render on top
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (bRenderCustomHUD && GS && !ScoreboardIsUp())
	{
		DrawTeamScoreBar();
	}

	// Suppress the engine's crosshair flag-grab flash unless the user opted in via
	// nchud. The stock UTHUDWidget_WeaponCrosshair (which we don't strip/subclass)
	// draws a team-colored flag ballooning over the crosshair for 3s after a grab,
	// keyed off AUTHUD::LastFlagGrabTime (UTHUDWidget_WeaponCrosshair.cpp). That
	// field is read in exactly one place engine-wide (the flash), so stashing it to
	// a far-past value across the widget pass disables only the flash, with no other
	// side effects; restored immediately after Super. Default OFF — opt in by adding
	// a visible `crosshair_flag_grab` layout entry (CTF section in nchud).
	const FNCPlusHUDElement* GrabFlashElem = FNCPlusHUDLayout::GetLive().Find(TEXT("crosshair_flag_grab"));
	const bool bShowGrabFlash = (GrabFlashElem != nullptr && !GrabFlashElem->bHidden);
	const float SavedFlagGrabTime = LastFlagGrabTime;
	if (!bShowGrabFlash)
	{
		LastFlagGrabTime = -1000.f;
	}

	Super::DrawHUD();

	LastFlagGrabTime = SavedFlagGrabTime;
	if (!bRenderCustomHUD) return;

	// Spectator banner — drawn after Super so it sits on top of any stock UI
	// in the same screen region. Suppressed while the scoreboard is open.
	if (GS && !ScoreboardIsUp())
	{
		DrawSpectatorTarget();
		// Warmup-only spawn-point markers (learning aid) — self-gates to
		// WaitingToStart + ncp.WarmupSpawns, so this is a no-op in live play.
		NCPlusHUDDrawCall::DrawWarmupSpawnMarkers(this, Canvas);
	}

	// Held-pickup status (amp/berserk/siphon countdown + boot charges) — NCPlus mode only.
	NCPlusHUDDrawCall::DrawHeldPowerups(this, Canvas);

	// Optional opt-in overlays (default OFF). DrawDamageFlash must be last so it
	// tints over every other HUD draw.
	NCPlusHUDDrawCall::DrawServerInfo(this, Canvas);
	NCPlusHUDDrawCall::DrawDamageFlash(this, Canvas);

	// Replay-only: fire-validation corner feed (self-guards to demo playback).
	NCPlusHUDDrawCall::DrawFireValReplayFeed(this, Canvas);
}

void ANCPlusCTFHUD::DrawSpectatorTarget()
{
	if (!Canvas || !MediumFont || !SmallFont) return;
	if (!UTPlayerOwner) return;

	AActor* ViewTarget = UTPlayerOwner->GetViewTarget();
	if (!ViewTarget || ViewTarget == UTPlayerOwner) return;

	APawn* ViewPawn = Cast<APawn>(ViewTarget);
	if (!ViewPawn) return;

	// Viewing our own pawn = playing, not spectating.
	if (ViewPawn == UTPlayerOwner->GetPawn()) return;

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
	// Bottom-right with margins. ~140px above bottom keeps clear of the
	// stock ammo counter (BottomRight -20/-20) and the OnScreenChat box.
	const float PanelX = Canvas->ClipX - PanelW - 24.f * RenderScale;
	const float PanelY = Canvas->ClipY - PanelH - 140.f * RenderScale;

	// Background panel
	Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.7f));
	Canvas->DrawTile(Canvas->DefaultTexture, PanelX, PanelY, PanelW, PanelH, 0, 0, 1, 1);

	// Team-color accent stripe on the left edge
	FLinearColor AccentColor(0.9f, 0.9f, 0.9f, 1.f);
	if (PS->Team)
	{
		AccentColor = (PS->Team->TeamIndex == 0)
			? FLinearColor(0.9f, 0.15f, 0.15f, 1.f)
			: FLinearColor(0.15f, 0.4f, 0.95f, 1.f);
	}
	Canvas->SetLinearDrawColor(AccentColor);
	Canvas->DrawTile(Canvas->DefaultTexture, PanelX, PanelY, 3.f * RenderScale, PanelH, 0, 0, 1, 1);

	// "NOW WATCHING" header — small muted text
	Canvas->DrawColor = FColor(180, 180, 180, 255);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, SmallFont, HeaderDrawText,
		PanelX + (PanelW - HeaderW) * 0.5f,
		PanelY + PadY,
		HeaderScale, HeaderScale, Canvas->DrawColor);

	// PlayerName — larger, team-colored
	Canvas->DrawColor = AccentColor.ToFColor(true);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, NameDrawText,
		PanelX + (PanelW - NameW) * 0.5f,
		PanelY + PadY + HeaderH + Gap,
		NameScale, NameScale, Canvas->DrawColor);
}

void ANCPlusCTFHUD::DrawTeamScoreBar()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS || !Canvas || !SmallFont || !MediumFont || !LargeFont) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;

	// Phase 3.5 layout consult.
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;

	// Team colors (respect TeamSkins). Honors `use_team_color` extra.
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

	int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Dimensions
	// Phase 3.11: scorebar Scale override scales bar + clock font uniformly.
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

	// Team 0 (left)
	float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(FadeL(Team0Color));
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	// Team 1 (right)
	float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FadeL(Team1Color));
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// Score tails
	float TailHeight = 14.f * RenderScale * ScoreScale;
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FadeL(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f)));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// Center divider
	Canvas->SetLinearDrawColor(FadeL(FLinearColor::White));
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	// Text — single "scorebar" font override drives both team-name and
	// score-number fonts (Phase 3.8).
	// font_scale Extras lets the user shrink/grow text independently of bar
	// dimensions (useful for tall fonts like Extreme).
	const float FontExtraScale = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	float FontScale = RenderScale * 0.85f * ScoreScale * FontExtraScale;
	float LargeFontScale = RenderScale * 1.2f * ScoreScale * FontExtraScale;
	float XL, YL;

	UFont* TeamNameFont  = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, SmallFont);
	UFont* TeamScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, LargeFont);
	if (!TeamNameFont)  TeamNameFont  = SmallFont;
	if (!TeamScoreFont) TeamScoreFont = LargeFont;

	// Team 0 name
	FText ResolvedText;
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team0Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	// Team 0 score
	static bool bHasCachedScore0 = false;
	static int32 CachedScore0 = 0;
	static FString Score0Str;
	if (!bHasCachedScore0 || CachedScore0 != Score0) { bHasCachedScore0 = true; CachedScore0 = Score0; Score0Str = FString::FromInt(Score0); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score0Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	// Team 1 score
	static bool bHasCachedScore1 = false;
	static int32 CachedScore1 = 0;
	static FString Score1Str;
	if (!bHasCachedScore1 || CachedScore1 != Score1) { bHasCachedScore1 = true; CachedScore1 = Score1; Score1Str = FString::FromInt(Score1); }
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamScoreFont, Score1Str, LargeFontScale, LargeFontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamScoreFont, ResolvedText, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale, Canvas->DrawColor);

	// Team 1 name
	NCPlusHUDDrawCall::ResolveStableText(Canvas, TeamNameFont, Team1Name, FontScale, FontScale, ResolvedText, XL, YL);
	Canvas->DrawColor = WhiteOp;
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, TeamNameFont, ResolvedText, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale, Canvas->DrawColor);

	// Match clock (CTF uses match time, not round time)
	float ClockY = TopY + BarHeight + 2.f * RenderScale;
	float ClockBottomY = ClockY;
	float RoundClockScale = RenderScale * 1.1f * ScoreScale * FontExtraScale;

	// Resolve the OT replicator once — OT clock, Advantage clock, and the
	// status-label branch all read off it. ANCPlusCTFOTInfo is spawned
	// authority-only and replicates to every client; lazy-find + cache.
	static TWeakObjectPtr<UWorld> CachedWorld;
	static TWeakObjectPtr<ANCPlusCTFOTInfo> CachedInfo;
	ANCPlusCTFOTInfo* Info = nullptr;
	if (CachedWorld.Get() == GetWorld() && CachedInfo.IsValid())
	{
		Info = CachedInfo.Get();
	}
	else
	{
		for (TActorIterator<ANCPlusCTFOTInfo> It(GetWorld()); It; ++It)
		{
			CachedWorld = GetWorld();
			CachedInfo = *It;
			Info = *It;
			break;
		}
	}

	// OT detection: GetMatchState() == MatchState::MatchIsInOvertime.
	// IsMatchInOvertime() is virtual on UTGameState so it reads the same on
	// clients (MatchState is replicated).
	const bool bInOvertime = GS->IsMatchInOvertime();
	// Advantage runs in MatchState::InProgress with bPlayingAdvantage set —
	// regulation clock is frozen at 00:00 server-side (bStopGameClock). The
	// HUD draws a count-up timer instead so spectators can read it.
	const bool bPlayingAdvantage = !bInOvertime &&
		NCPlusReflection::GetBool(GS, TEXT("bPlayingAdvantage"));
	auto ResolveClockText = [&](int32 TotalSeconds)
	{
		static int32 CachedSeconds = MAX_int32;
		static FString ClockText;
		if (CachedSeconds != TotalSeconds)
		{
			CachedSeconds = TotalSeconds;
			ClockText = FString::Printf(TEXT("%02d:%02d"), TotalSeconds / 60, TotalSeconds % 60);
		}
		NCPlusHUDDrawCall::ResolveStableText(Canvas, MediumFont, ClockText,
			RoundClockScale, RoundClockScale, ResolvedText, XL, YL);
	};

	if (bInOvertime)
	{
		const int32 StartElapsed = (Info && Info->OvertimeStartElapsed >= 0)
			? Info->OvertimeStartElapsed : GS->ElapsedTime;
		const int32 OTSeconds = FMath::Max(0, GS->ElapsedTime - StartElapsed);
		ResolveClockText(OTSeconds);
		Canvas->DrawColor = FColor(255, 200, 60, WhiteOp.A);    // amber for OT
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
		ClockBottomY = ClockY + YL;
	}
	else if (bPlayingAdvantage)
	{
		const int32 StartElapsed = (Info && Info->AdvantageStartElapsed >= 0)
			? Info->AdvantageStartElapsed : GS->ElapsedTime;
		const int32 AdvSeconds = FMath::Max(0, GS->ElapsedTime - StartElapsed);
		ResolveClockText(AdvSeconds);
		Canvas->DrawColor = FColor(255, 200, 60, WhiteOp.A);    // amber matches OT
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
		ClockBottomY = ClockY + YL;
	}
	else
	{
		int32 RemainingTime = GS->GetRemainingTime();
		if (RemainingTime >= 0 && GS->TimeLimit > 0)
		{
			ResolveClockText(RemainingTime);
			if (RemainingTime <= 30)
				Canvas->DrawColor = FColor(255, 60, 60, WhiteOp.A);
			else
				Canvas->DrawColor = WhiteOp;
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
			ClockBottomY = ClockY + YL;
		}
		else
		{
			// No time limit — show elapsed time
			int32 Elapsed = GS->ElapsedTime;
			ResolveClockText(Elapsed);
			Canvas->DrawColor = WhiteOp;
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, MediumFont, ResolvedText, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale, Canvas->DrawColor);
			ClockBottomY = ClockY + YL;
		}
	}

	// Status label below the clock. Priority: Overtime > Advantage > Halves.
	// Halves only show when the gamemode actually has halftime enabled —
	// OTInfo->bHasHalftime mirrors the server-side decision (false for 3v3+),
	// so single-period games no longer show a misleading "1st Half" all match.
	static const FString OvertimeStatus(TEXT("Overtime"));
	static const FString AdvantageStatus(TEXT("Advantage"));
	static const FString FirstHalfStatus(TEXT("1st Half"));
	static const FString SecondHalfStatus(TEXT("2nd Half"));
	const FString* StatusStr = nullptr;
	FColor StatusColor(200, 200, 200, 255);
	if (bInOvertime)
	{
		StatusStr = &OvertimeStatus;
		StatusColor = FColor(255, 200, 60, 255);    // amber to match clock
	}
	else if (bPlayingAdvantage)
	{
		StatusStr = &AdvantageStatus;
		StatusColor = FColor(255, 200, 60, 255);    // amber to match clock
	}
	else if (Info && Info->bHasHalftime)
	{
		const bool bSecondHalf = NCPlusReflection::GetBool(GS, TEXT("bSecondHalf"));
		StatusStr = bSecondHalf ? &SecondHalfStatus : &FirstHalfStatus;
	}

	if (StatusStr)
	{
		const float StatusScale = RenderScale * 0.7f;
		NCPlusHUDDrawCall::ResolveStableText(Canvas, SmallFont, *StatusStr, StatusScale, StatusScale, ResolvedText, XL, YL);
		StatusColor.A = WhiteOp.A; Canvas->DrawColor = StatusColor;
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, SmallFont, ResolvedText, CenterX - XL * 0.5f, ClockBottomY + 1.f * RenderScale, StatusScale, StatusScale, Canvas->DrawColor);
	}
}
