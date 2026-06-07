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
	// Custom split WeaponBar + modernized HP/Armor (replace stock variants).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"));
	// Modernized ammo counter — replaces stock bpHW_WeaponInfo (3 styles, fully editable).
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"));
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

void ANCPlusCTFHUD::BeginPlay()
{
	// Remove stock widgets we're replacing BEFORE Super::BeginPlay instantiates them.
	// RequiredHudWidgetClasses is loaded from DefaultGame.ini config.
	RequiredHudWidgetClasses.RemoveAll([](const FString& Entry)
	{
		return Entry.Contains(TEXT("TeamGameClock"))     // stock team score/clock bar
			|| Entry.Contains(TEXT("CTFScoreboard"))     // stock CTF scoreboard
			|| Entry.Contains(TEXT("TeamScoreboard"))    // stock team scoreboard (fallback)
			|| Entry.Contains(TEXT("bpHW_WeaponBar"))    // replaced by our split bar
			|| Entry.Contains(TEXT("bpHW_QuickStats"))   // replaced by our HP/Armor widget
			|| Entry.Contains(TEXT("bpHW_Paperdoll"))    // fallback +HP/Armor mode would conflict
			|| Entry.Contains(TEXT("bpHW_WeaponInfo"))   // replaced by our ammo counter
			// The stock CTF flag status widget is registered in DefaultGame.ini as
			// /Game/RestrictedAssets/UI/HUDWidgets/bpHW_CTFFlagStatus.bpHW_CTFFlagStatus_C
			// (a BP wrapper around UTHUDWidget_CTFFlagStatus). Matching "CTFFlagStatus"
			// catches both the BP path and any future C++ entry. Our subclass is added
			// via HudWidgetClasses below so this strip removes only the stock one.
			|| Entry.Contains(TEXT("CTFFlagStatus"));
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

	if (!Canvas || !SmallFont) return;

	// Draw score bar BEFORE Super so flag icons (drawn by Super) render on top
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS && !ScoreboardIsUp())
	{
		DrawTeamScoreBar();
	}

	Super::DrawHUD();

	// Spectator banner — drawn after Super so it sits on top of any stock UI
	// in the same screen region. Suppressed while the scoreboard is open.
	if (GS && !ScoreboardIsUp())
	{
		DrawSpectatorTarget();
	}
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

	const FString HeaderText = TEXT("NOW WATCHING");
	const FString NameText   = PS->PlayerName;

	float HeaderW, HeaderH, NameW, NameH;
	Canvas->TextSize(SmallFont,  HeaderText, HeaderW, HeaderH, HeaderScale, HeaderScale);
	Canvas->TextSize(MediumFont, NameText,   NameW,   NameH,   NameScale,   NameScale);

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
	Canvas->DrawText(SmallFont, HeaderText,
		PanelX + (PanelW - HeaderW) * 0.5f,
		PanelY + PadY,
		HeaderScale, HeaderScale);

	// PlayerName — larger, team-colored
	Canvas->DrawColor = AccentColor.ToFColor(true);
	Canvas->DrawText(MediumFont, NameText,
		PanelX + (PanelW - NameW) * 0.5f,
		PanelY + PadY + HeaderH + Gap,
		NameScale, NameScale);
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

	FString Team0Name = bCustomColors ? TEXT("Liandri (R)") : TEXT("RED");
	FString Team1Name = bCustomColors ? TEXT("Phayder (B)") : TEXT("BLUE");

	int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	// Dimensions
	// Phase 3.11: scorebar Scale override scales bar + clock font uniformly.
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float BarWidth = 220.f * RenderScale * ScoreScale;
	const float BarHeight = 36.f * RenderScale * ScoreScale;
	const float ScoreBoxWidth = 50.f * RenderScale * ScoreScale;
	const float GapWidth = 8.f * RenderScale;

	// Team 0 (left)
	float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(Team0Color);
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	// Team 1 (right)
	float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(Team1Color);
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	// Score tails
	float TailHeight = 14.f * RenderScale * ScoreScale;
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	// Center divider
	Canvas->SetLinearDrawColor(FLinearColor::White);
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
	Canvas->TextSize(TeamNameFont, Team0Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(TeamNameFont, Team0Name, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// Team 0 score
	FString Score0Str = FString::Printf(TEXT("%d"), Score0);
	Canvas->TextSize(TeamScoreFont, Score0Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(TeamScoreFont, Score0Str, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 score
	FString Score1Str = FString::Printf(TEXT("%d"), Score1);
	Canvas->TextSize(TeamScoreFont, Score1Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(TeamScoreFont, Score1Str, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 name
	Canvas->TextSize(TeamNameFont, Team1Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(TeamNameFont, Team1Name, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

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

	if (bInOvertime)
	{
		const int32 StartElapsed = (Info && Info->OvertimeStartElapsed >= 0)
			? Info->OvertimeStartElapsed : GS->ElapsedTime;
		const int32 OTSeconds = FMath::Max(0, GS->ElapsedTime - StartElapsed);
		const int32 Mins = OTSeconds / 60;
		const int32 Secs = OTSeconds % 60;
		FString ClockStr = FString::Printf(TEXT("%02d:%02d"), Mins, Secs);
		Canvas->TextSize(MediumFont, ClockStr, XL, YL, RoundClockScale, RoundClockScale);
		Canvas->DrawColor = FColor(255, 200, 60, 255);    // amber for OT
		Canvas->DrawText(MediumFont, ClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
		ClockBottomY = ClockY + YL;
	}
	else if (bPlayingAdvantage)
	{
		const int32 StartElapsed = (Info && Info->AdvantageStartElapsed >= 0)
			? Info->AdvantageStartElapsed : GS->ElapsedTime;
		const int32 AdvSeconds = FMath::Max(0, GS->ElapsedTime - StartElapsed);
		const int32 Mins = AdvSeconds / 60;
		const int32 Secs = AdvSeconds % 60;
		FString ClockStr = FString::Printf(TEXT("%02d:%02d"), Mins, Secs);
		Canvas->TextSize(MediumFont, ClockStr, XL, YL, RoundClockScale, RoundClockScale);
		Canvas->DrawColor = FColor(255, 200, 60, 255);    // amber matches OT
		Canvas->DrawText(MediumFont, ClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
		ClockBottomY = ClockY + YL;
	}
	else
	{
		int32 RemainingTime = GS->GetRemainingTime();
		if (RemainingTime >= 0 && GS->TimeLimit > 0)
		{
			int32 Mins = RemainingTime / 60;
			int32 Secs = RemainingTime % 60;
			FString ClockStr = FString::Printf(TEXT("%02d:%02d"), Mins, Secs);
			Canvas->TextSize(MediumFont, ClockStr, XL, YL, RoundClockScale, RoundClockScale);
			if (RemainingTime <= 30)
				Canvas->DrawColor = FColor(255, 60, 60, 255);
			else
				Canvas->DrawColor = FColor::White;
			Canvas->DrawText(MediumFont, ClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
			ClockBottomY = ClockY + YL;
		}
		else
		{
			// No time limit — show elapsed time
			int32 Elapsed = GS->ElapsedTime;
			int32 Mins = Elapsed / 60;
			int32 Secs = Elapsed % 60;
			FString ClockStr = FString::Printf(TEXT("%02d:%02d"), Mins, Secs);
			Canvas->TextSize(MediumFont, ClockStr, XL, YL, RoundClockScale, RoundClockScale);
			Canvas->DrawColor = FColor::White;
			Canvas->DrawText(MediumFont, ClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
			ClockBottomY = ClockY + YL;
		}
	}

	// Status label below the clock. Priority: Overtime > Advantage > Halves.
	// Halves only show when the gamemode actually has halftime enabled —
	// OTInfo->bHasHalftime mirrors the server-side decision (false for 3v3+),
	// so single-period games no longer show a misleading "1st Half" all match.
	FString StatusStr;
	FColor StatusColor(200, 200, 200, 255);
	if (bInOvertime)
	{
		StatusStr = TEXT("Overtime");
		StatusColor = FColor(255, 200, 60, 255);    // amber to match clock
	}
	else if (bPlayingAdvantage)
	{
		StatusStr = TEXT("Advantage");
		StatusColor = FColor(255, 200, 60, 255);    // amber to match clock
	}
	else if (Info && Info->bHasHalftime)
	{
		const bool bSecondHalf = NCPlusReflection::GetBool(GS, TEXT("bSecondHalf"));
		StatusStr = bSecondHalf ? TEXT("2nd Half") : TEXT("1st Half");
	}

	if (!StatusStr.IsEmpty())
	{
		const float StatusScale = RenderScale * 0.7f;
		Canvas->TextSize(SmallFont, StatusStr, XL, YL, StatusScale, StatusScale);
		Canvas->DrawColor = StatusColor;
		Canvas->DrawText(SmallFont, StatusStr, CenterX - XL * 0.5f, ClockBottomY + 1.f * RenderScale, StatusScale, StatusScale);
	}
}
