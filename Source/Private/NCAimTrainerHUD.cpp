#include "NCAimTrainerHUD.h"
#include "NCAimTrainerPlayerController.h"
#include "Engine/Canvas.h"
#include "UTCharacter.h"

namespace
{
	const FLinearColor TrainerInk(0.92f, 0.95f, 0.97f, 1.f);
	const FLinearColor TrainerMuted(0.54f, 0.63f, 0.68f, 1.f);
	const FLinearColor TrainerAccent(0.05f, 0.84f, 0.87f, 1.f);
	const FLinearColor TrainerPanel(0.018f, 0.032f, 0.045f, 0.96f);
	const FLinearColor TrainerCard(0.043f, 0.071f, 0.088f, 1.f);
	const FLinearColor TrainerSelected(0.05f, 0.16f, 0.18f, 1.f);

	const TCHAR* ScenarioName(uint8 Scenario)
	{
		switch (Scenario)
		{
		case 1: return TEXT("HEADSHOTS");
		case 2: return TEXT("INSTAGIB POP-UP");
		default: return TEXT("STRAFE TRACKING");
		}
	}

	FString SafePlayerLabel(const FString& Name)
	{
		FString Result = Name.Left(48);
		Result.ReplaceInline(TEXT("\n"), TEXT(" "));
		Result.ReplaceInline(TEXT("\r"), TEXT(" "));
		Result.ReplaceInline(TEXT("\t"), TEXT(" "));
		return Result;
	}
}

ANCAimTrainerHUD::ANCAimTrainerHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimap = false;
	bDrawDamageNumbers = false;
	bShowScoresWhileDead = false;
}

void ANCAimTrainerHUD::BeginPlay()
{
	// Do not inherit a stock scoreboard or alter the player's saved HUD layout.
	RequiredHudWidgetClasses.Empty();
	HudWidgetClasses.Empty();
	SpectatorHudWidgetClasses.Empty();
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"));
	Super::BeginPlay();
}

bool ANCAimTrainerHUD::ScoreboardIsUp() { return false; }
bool ANCAimTrainerHUD::ShouldDrawMinimap() { return false; }

EInputMode::Type ANCAimTrainerHUD::GetInputMode_Implementation() const
{
	const ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(PlayerOwner);
	return PC && PC->IsTrainerMenuVisible() ? EInputMode::EIM_GameAndUI : EInputMode::EIM_GameOnly;
}

void ANCAimTrainerHUD::Panel(float X, float Y, float W, float H, const FLinearColor& Color)
{
	DrawRect(Color, TrainerOrigin.X + X * TrainerScale, TrainerOrigin.Y + Y * TrainerScale,
		W * TrainerScale, H * TrainerScale);
}

void ANCAimTrainerHUD::Label(const FString& Text, float X, float Y, float Size,
	const FLinearColor& Color, float MaxWidth, bool bCentered)
{
	if (!Canvas || !SmallFont) { return; }
	float FontScale = Size / FMath::Max(1.f, float(SmallFont->GetMaxCharHeight()));
	float XL = 0.f, YL = 0.f;
	FString VisibleText = Text;
	Canvas->TextSize(SmallFont, VisibleText, XL, YL, FontScale, FontScale);
	// Bound remote names/status text; preserve readable type instead of shrinking a long name.
	if (MaxWidth > 0.f && XL > MaxWidth)
	{
		while (VisibleText.Len() > 1 && XL > MaxWidth)
		{
			VisibleText = VisibleText.LeftChop(1);
			Canvas->TextSize(SmallFont, VisibleText + TEXT("..."), XL, YL, FontScale, FontScale);
		}
		VisibleText += TEXT("...");
	}
	Canvas->SetLinearDrawColor(Color);
	Canvas->DrawText(SmallFont, VisibleText,
		TrainerOrigin.X + (X - (bCentered ? 0.5f * XL : 0.f)) * TrainerScale,
		TrainerOrigin.Y + Y * TrainerScale, FontScale * TrainerScale, FontScale * TrainerScale);
}

bool ANCAimTrainerHUD::IsHovered(float X, float Y, float W, float H) const
{
	float MouseX = 0.f, MouseY = 0.f;
	if (!PlayerOwner || !PlayerOwner->GetMousePosition(MouseX, MouseY)) { return false; }
	const FVector2D P = (FVector2D(MouseX, MouseY) - TrainerOrigin) / TrainerScale;
	return P.X >= X && P.Y >= Y && P.X <= X + W && P.Y <= Y + H;
}

void ANCAimTrainerHUD::Button(int32 Action, const FString& Text, float X, float Y, float W, float H, bool bSelected)
{
	const bool bHighlight = bSelected || IsHovered(X, Y, W, H);
	Panel(X, Y, W, H, bHighlight ? TrainerSelected : TrainerCard);
	Panel(X, Y + H - 2.f, W, 2.f, bHighlight ? TrainerAccent : FLinearColor(0.11f, 0.17f, 0.20f, 1.f));
	Label(Text, X + 0.5f * W, Y + 0.5f * (H - 19.f), 19.f,
		bHighlight ? TrainerAccent : TrainerInk, W - 30.f, true);
	FTrainerButton Hit;
	Hit.Min = FVector2D(X, Y);
	Hit.Max = FVector2D(X + W, Y + H);
	Hit.Action = Action;
	TrainerButtons.Add(Hit);
}

void ANCAimTrainerHUD::DrawSessionHeader(const FNCAimTrainerProgress& Progress)
{
	Label(TEXT("NETCODE+ / AIM TRAINER"), 42.f, 25.f, 18.f, TrainerAccent);
	Label(ScenarioName(Progress.Scenario), 42.f, 51.f, 14.f, TrainerMuted);
	const float ColumnX[] = { 626.f, 830.f, 1034.f };
	const TCHAR* Labels[] = { TEXT("POINTS"), TEXT("TIME"), TEXT("ACCURACY") };
	const int32 Seconds = FMath::Max(0, FMath::CeilToInt(Progress.RemainingSeconds));
	const FString Values[] = {
		FString::FromInt(Progress.Score),
		FString::Printf(TEXT("%02d:%02d"), Seconds / 60, Seconds % 60),
		FString::Printf(TEXT("%.1f%%"), FMath::Clamp(Progress.Accuracy, 0.f, 100.f))
	};
	for (int32 Column = 0; Column < 3; ++Column)
	{
		Panel(ColumnX[Column], 17.f, 188.f, 70.f, TrainerPanel);
		Label(Labels[Column], ColumnX[Column] + 15.f, 26.f, 12.f, TrainerMuted);
		Label(Values[Column], ColumnX[Column] + 15.f, 46.f, 27.f, TrainerInk);
	}
}

void ANCAimTrainerHUD::DrawLeaderboard(ANCAimTrainerPlayerController* PC, float Y)
{
	Label(TEXT("UT4STATS / GLOBAL TOP 10"), 120.f, Y, 15.f, TrainerAccent);
	Label(TEXT("BEST RUN PER PLAYER / THIS SCENARIO"), 790.f, Y, 12.f, TrainerMuted, 370.f);
	const TArray<FNCAimTrainerLeaderboardRow>& Rows = PC->GetTrainerLeaderboard();
	if (Rows.Num() == 0)
	{
		Label(TEXT("No leaderboard scores available yet."), 120.f, Y + 43.f, 18.f, TrainerMuted);
	}
	for (int32 Index = 0; Index < FMath::Min(10, Rows.Num()); ++Index)
	{
		const FNCAimTrainerLeaderboardRow& Row = Rows[Index];
		const float X = Index < 5 ? 120.f : 660.f;
		const float RowY = Y + 32.f + float(Index % 5) * 29.f;
		Panel(X, RowY, 500.f, 27.f, Index % 5 == 0 ? TrainerCard : FLinearColor(0.028f, 0.046f, 0.06f, 0.65f));
		Label(FString::FromInt(Row.Rank), X + 10.f, RowY + 6.f, 15.f, TrainerAccent, 34.f);
		Label(SafePlayerLabel(Row.DisplayName), X + 44.f, RowY + 6.f, 15.f, TrainerInk, 260.f);
		Label(FString::FromInt(Row.Score), X + 319.f, RowY + 6.f, 15.f, TrainerInk, 86.f);
		Label(FString::Printf(TEXT("%.1f%%"), Row.AccuracyPercent), X + 422.f, RowY + 6.f, 15.f, TrainerMuted, 70.f);
	}
	Label(PC->GetTrainerOnlineStatus(), 120.f, Y + 189.f, 13.f, TrainerMuted, 1020.f);
}

void ANCAimTrainerHUD::DrawModePicker(ANCAimTrainerPlayerController* PC)
{
	const FNCAimTrainerProgress& Progress = PC->GetTrainerProgress();
	Panel(90.f, 126.f, 1100.f, 551.f, TrainerPanel);
	Label(TEXT("CHOOSE YOUR PRACTICE"), 120.f, 151.f, 27.f, TrainerInk);
	Label(TEXT("REAL UT CHARACTERS. THREE 60-SECOND CHALLENGES."), 120.f, 188.f, 14.f, TrainerMuted);
	const TCHAR* Line1[] = { TEXT("Stay on a strafing character."), TEXT("Sniper precision on real heads."), TEXT("Shoot before targets disappear.") };
	const TCHAR* Line2[] = { TEXT("Keep your crosshair on target. No firing."), TEXT("Body shots do not score."), TEXT("Varied height, distance and timing.") };
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		const float X = 120.f + float(Mode) * 353.f;
		Button(Mode, FString::Printf(TEXT("[%d] %s"), Mode + 1, ScenarioName(uint8(Mode))), X, 224.f, 334.f, 53.f, Progress.Scenario == Mode);
		Label(Line1[Mode], X + 12.f, 294.f, 15.f, TrainerInk, 310.f);
		Label(Line2[Mode], X + 12.f, 318.f, 13.f, TrainerMuted, 310.f);
	}
	Panel(120.f, 358.f, 1040.f, 1.f, FLinearColor(0.10f, 0.16f, 0.19f, 1.f));
	DrawLeaderboard(PC, 378.f);
	Button(10, TEXT("ENTER  /  START RUN"), 799.f, 610.f, 361.f, 43.f, true);
	Label(TEXT("Click a scenario or use 1 / 2 / 3.  ESC opens the game menu."), 120.f, 625.f, 14.f, TrainerMuted, 650.f);
}

void ANCAimTrainerHUD::DrawResults(ANCAimTrainerPlayerController* PC)
{
	const FNCAimTrainerProgress& Progress = PC->GetTrainerProgress();
	Panel(90.f, 126.f, 1100.f, 551.f, TrainerPanel);
	Label(TEXT("RUN COMPLETE"), 640.f, 149.f, 18.f, TrainerAccent, 0.f, true);
	Label(FString::FromInt(Progress.Score), 640.f, 181.f, 63.f, TrainerInk, 700.f, true);
	const FString Detail = Progress.Scenario == 0
		? FString::Printf(TEXT("%.2f s ON TARGET     %.1f%% ACCURACY"), Progress.TrackingSeconds, Progress.Accuracy)
		: FString::Printf(TEXT("%d / %d HITS     %.1f%% ACCURACY     %d EXPIRED"), Progress.Hits, Progress.Shots, Progress.Accuracy, Progress.TargetsExpired);
	Label(Detail, 640.f, 269.f, 17.f, TrainerMuted, 1010.f, true);
	if (Progress.Scenario == 1)
	{
		Label(FString::Printf(TEXT("%d HEADSHOTS"), Progress.Headshots), 640.f, 301.f, 16.f, TrainerAccent, 0.f, true);
	}
	Panel(120.f, 340.f, 1040.f, 1.f, FLinearColor(0.10f, 0.16f, 0.19f, 1.f));
	DrawLeaderboard(PC, 362.f);
	Button(11, TEXT("F6  /  CHOOSE SCENARIO"), 120.f, 610.f, 361.f, 43.f, false);
	Button(10, TEXT("ENTER  /  PLAY AGAIN"), 799.f, 610.f, 361.f, 43.f, true);
}

void ANCAimTrainerHUD::DrawHUD()
{
	TrainerButtons.Empty();
	ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(PlayerOwner);
	if (!Canvas || !PC) { Super::DrawHUD(); return; }
	const FNCAimTrainerProgress& Progress = PC->GetTrainerProgress();
	if (!PC->IsTrainerMenuVisible()) { Super::DrawHUD(); }
	if (!PC->HasTrainerInputFocus()) { return; }
	TrainerScale = FMath::Min(Canvas->ClipX / 1280.f, Canvas->ClipY / 720.f);
	TrainerOrigin = FVector2D((Canvas->ClipX - 1280.f * TrainerScale) * 0.5f, (Canvas->ClipY - 720.f * TrainerScale) * 0.5f);
	DrawSessionHeader(Progress);
	// Stock UT only draws a weapon crosshair when a weapon exists. Tracking can
	// still run if its optional display weapon asset is absent from a test cook.
	const AUTCharacter* Trainee = Cast<AUTCharacter>(PC->GetPawn());
	if (!PC->IsTrainerMenuVisible() && Trainee && !Trainee->GetWeapon())
	{
		Panel(639.f, 354.f, 2.f, 12.f, TrainerInk);
		Panel(634.f, 359.f, 12.f, 2.f, TrainerInk);
	}
	if (Progress.Phase == 0) { DrawModePicker(PC); }
	else if (Progress.Phase == 3) { DrawResults(PC); }
	else if (Progress.Phase == 1)
	{
		Panel(490.f, 258.f, 300.f, 175.f, TrainerPanel);
		Label(TEXT("GET READY"), 640.f, 280.f, 20.f, TrainerAccent, 0.f, true);
		Label(FString::FromInt(FMath::Max(1, FMath::CeilToInt(Progress.RemainingSeconds))), 640.f, 323.f, 67.f, TrainerInk, 0.f, true);
	}
	else
	{
		Label(TEXT("F6  /  END RUN AND RETURN TO PRACTICE MENU"), 640.f, 677.f, 13.f, TrainerMuted, 0.f, true);
		if (Progress.Scenario == 0)
		{
			Label(FString::Printf(TEXT("ON TARGET  %.2f s"), Progress.TrackingSeconds), 640.f, 103.f, 16.f, TrainerAccent, 0.f, true);
		}
	}
}

bool ANCAimTrainerHUD::OverrideMouseClick(FKey Key, EInputEvent EventType)
{
	ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(PlayerOwner);
	if (!PC || !PC->IsTrainerMenuVisible() || !PC->HasTrainerInputFocus()) { return false; }
	if (Key == EKeys::LeftMouseButton && EventType == IE_Pressed)
	{
		for (const FTrainerButton& Hit : TrainerButtons)
		{
			if (IsHovered(Hit.Min.X, Hit.Min.Y, Hit.Max.X - Hit.Min.X, Hit.Max.Y - Hit.Min.Y))
			{
				if (Hit.Action < 3) { PC->SelectTrainerScenario(uint8(Hit.Action)); }
				else if (Hit.Action == 10) { PC->StartTrainerRun(); }
				else { PC->ReturnToTrainerMenu(); }
				break;
			}
		}
	}
	return true;
}
