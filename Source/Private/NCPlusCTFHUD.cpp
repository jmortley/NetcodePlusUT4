// NCPlusCTFHUD.cpp — Custom CTF HUD with team score bar and mouse focus fix.
#include "NCPlusCTFHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "NCPlusHUDLayout.h"

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
			|| Entry.Contains(TEXT("bpHW_WeaponInfo")); // replaced by our ammo counter
	});

	Super::BeginPlay();

	// HUD layout system — capture stock defaults, load + apply live layout.
	CaptureWidgetDefaults(this);
	FNCPlusHUDLayout::ReloadLive();
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
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

	// Text
	float FontScale = RenderScale * 0.85f * ScoreScale;
	float LargeFontScale = RenderScale * 1.2f * ScoreScale;
	float XL, YL;

	// Team 0 name
	Canvas->TextSize(SmallFont, Team0Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team0Name, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// Team 0 score
	FString Score0Str = FString::Printf(TEXT("%d"), Score0);
	Canvas->TextSize(LargeFont, Score0Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score0Str, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 score
	FString Score1Str = FString::Printf(TEXT("%d"), Score1);
	Canvas->TextSize(LargeFont, Score1Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score1Str, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	// Team 1 name
	Canvas->TextSize(SmallFont, Team1Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team1Name, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// Match clock (CTF uses match time, not round time)
	float ClockY = TopY + BarHeight + 2.f * RenderScale;
	float ClockBottomY = ClockY;
	float RoundClockScale = RenderScale * 1.1f * ScoreScale;

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

	// Half indicator (only when halftime is enabled)
	// bSecondHalf is on UTCTFGameState — use reflection to avoid ABI mismatch
	UBoolProperty* HalfProp = FindField<UBoolProperty>(GS->GetClass(), TEXT("bSecondHalf"));
	if (HalfProp)
	{
		bool bSecondHalf = HalfProp->GetPropertyValue_InContainer(GS);
		FString HalfStr = bSecondHalf ? TEXT("2nd Half") : TEXT("1st Half");
		float HalfScale = RenderScale * 0.7f;
		Canvas->TextSize(SmallFont, HalfStr, XL, YL, HalfScale, HalfScale);
		Canvas->DrawColor = FColor(200, 200, 200, 255);
		Canvas->DrawText(SmallFont, HalfStr, CenterX - XL * 0.5f, ClockBottomY + 1.f * RenderScale, HalfScale, HalfScale);
	}
}
