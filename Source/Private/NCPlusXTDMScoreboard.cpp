#include "NCPlusXTDMScoreboard.h"

#include "NCPlusXTDMHUD.h"
#include "NCPlusXTDMReplicator.h"
#include "NCPlusHUDLayout.h"
#include "NCPlusScoreboardReady.h"
#include "NCPlusScoreboardHost.h"
#include "NCAccuracyStatsReplicator.h"
#include "UTGameState.h"
#include "UTTeamInfo.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "StatNames.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"

UNCPlusXTDMScoreboard::UNCPlusXTDMScoreboard(const FObjectInitializer& OI) : Super(OI)
{
	bDrawMinimapInScoreboard = false;
}

void UNCPlusXTDMScoreboard::Draw_Implementation(float DeltaTime)
{
	ANCPlusXTDMHUD* HUD = Cast<ANCPlusXTDMHUD>(UTHUDOwner);
	if (!HUD || !Canvas || !UTGameState || !HUD->SmallFont) return;
	SelectionStack.Reset();
	for (auto It = Rows.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || It.Key()->bIsInactive) It.RemoveCurrent();
	const float Now = GetWorld()->TimeSeconds;
	if (!AccuracyReplicator.IsValid() && (Now >= NextAccuracySearch || NextAccuracySearch - Now > 1.f))
	{
		NextAccuracySearch = Now + 1.f;
		for (TActorIterator<ANCAccuracyStatsReplicator> It(GetWorld()); It; ++It) { AccuracyReplicator = *It; break; }
	}
	// Absolute canvas geometry also drives SelectionStack, so clicks match every card at all scales.
	const float Fit = FMath::Min(Canvas->ClipX / 1400.f, Canvas->ClipY / 780.f);
	const float S = FMath::Min(Canvas->ClipY / 1080.f * HUD->GetHUDWidgetScaleOverride(), Fit);
	const float X = (Canvas->ClipX - 1320.f * S) * .5f;
	const float Y = (Canvas->ClipY - 690.f * S) * .5f;
	const float PanelW = 646.f * S;
	const float PanelH = 259.f * S;
	UFont* Font = HUD->SmallFont;
	ANCPlusXTDMReplicator* Rep = ANCPlusXTDMReplicator::Find(GetWorld());
	const uint8 LocalTeam = UTPlayerOwner && UTPlayerOwner->UTPlayerState ? UTPlayerOwner->UTPlayerState->GetTeamNum() : 255;
	HUD->Tile(Canvas, X - 20.f * S, Y - 16.f * S, 1360.f * S, 718.f * S,
		FLinearColor(.012f, .017f, .025f, FMath::Max(.78f, FNCPlusHUDLayout::GetScoreboardOpacity())));
	FString Heading = TEXT("xTDM  /  FOUR-TEAM INSTAGIB");
	FLinearColor HeadingColor = FLinearColor::White;
	if (Rep && Rep->bMatchEnded)
	{
		Heading = Rep->WinningTeamIndex < 4 ? HUD->TeamLabel(Rep->WinningTeamIndex) + TEXT(" WINS") : TEXT("DRAW");
		HeadingColor = HUD->TeamColor(UTGameState, Rep->WinningTeamIndex);
	}
	HUD->Text(Canvas, Font, Heading, Canvas->ClipX * .5f, Y, 1.02f * S, HeadingColor, true);
	const int32 Clock = FMath::Max(0, FMath::FloorToInt(UTGameState->GetClockTime()));
	FString Options = FString::Printf(TEXT("%dv%dv%dv%d   |   %s%02d:%02d"),
		Rep ? Rep->TeamSize : 2, Rep ? Rep->TeamSize : 2, Rep ? Rep->TeamSize : 2, Rep ? Rep->TeamSize : 2,
		UTGameState->IsMatchInOvertime() ? TEXT("OT ") : TEXT(""), Clock / 60, Clock % 60);
	if (UTGameState->GoalScore > 0) Options += FString::Printf(TEXT("   |   LIMIT %d"), UTGameState->GoalScore);
	HUD->Text(Canvas, Font, Options, Canvas->ClipX * .5f, Y + 36.f * S, .57f * S, FLinearColor(.65f, .7f, .78f), true);
	const float Columns[] = { .575f, .705f, .835f, .95f };
	const TCHAR* Labels[] = { TEXT("FRAGS"), TEXT("D"), TEXT("ACC"), TEXT("PING") };
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		const float PX = X + (Team % 2) * 674.f * S;
		const float PY = Y + (80.f + (Team / 2) * 282.f) * S;
		const FLinearColor Col = HUD->TeamColor(UTGameState, Team);
		HUD->Tile(Canvas, PX, PY, PanelW, PanelH, FLinearColor(.04f, .05f, .067f, .95f));
		HUD->Tile(Canvas, PX, PY, PanelW, 4.f * S, Col);
		TArray<AUTPlayerState*> Players; HUD->GetTeamRoster(Team, Players);
		const int32 Score = UTGameState->Teams.IsValidIndex(Team) && UTGameState->Teams[Team] ? UTGameState->Teams[Team]->Score : 0;
		HUD->Text(Canvas, Font, HUD->TeamLabel(Team) + (Team == LocalTeam ? TEXT("  /  YOUR TEAM") : TEXT("")),
			PX + 14.f * S, PY + 12.f * S, .73f * S, Col);
		HUD->Text(Canvas, Font, FString::FromInt(Score), PX + PanelW - 34.f * S, PY + 10.f * S, .88f * S, Col, true);
		HUD->Text(Canvas, Font, TEXT("PLAYER"), PX + 14.f * S, PY + 48.f * S, .43f * S, FLinearColor(.65f, .7f, .78f));
		for (int32 C = 0; C < 4; ++C)
			HUD->Text(Canvas, Font, Labels[C], PX + PanelW * Columns[C], PY + 48.f * S, .43f * S, FLinearColor(.65f, .7f, .78f), true);
		for (int32 Row = 0; Row < 4; ++Row)
		{
			const float RY = PY + (72.f + Row * 45.f) * S;
			if (!Players.IsValidIndex(Row))
			{
				const int32 TeamCapacity = Rep ? Rep->TeamSize : 2;
				if (Row < TeamCapacity) HUD->Text(Canvas, Font, TEXT("OPEN SLOT"), PX + 14.f * S, RY + 9.f * S, .49f * S, FLinearColor(.35f, .39f, .46f));
				continue;
			}
			AUTPlayerState* PS = Players[Row];
			const FVector4 Bounds(PX, RY, PX + PanelW, RY + 43.f * S);
			SelectionStack.Add(FSelectionObject(PS, Bounds));
			const bool bHovered = bIsInteractive && CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z
				&& CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W;
			if (bHovered || SelectedPlayer.Get() == PS || (UTPlayerOwner && UTPlayerOwner->UTPlayerState == PS))
			{
				FLinearColor Background = Col; Background.A = bHovered || SelectedPlayer.Get() == PS ? .25f : .12f;
				HUD->Tile(Canvas, PX, RY, PanelW, 43.f * S, Background);
			}
			FRowText& Cached = Rows.FindOrAdd(PS);
			if (Now >= Cached.RefreshAt || Cached.RefreshAt - Now > .25f)
			{
				Cached.RefreshAt = Now + .25f;
				Cached.Name = PS->PlayerName;
				Cached.Frags = FString::FromInt(FMath::RoundToInt(PS->Score));
				Cached.Deaths = FString::FromInt(PS->Deaths);
				Cached.Ping = FString::FromInt(UTPlayerOwner && UTPlayerOwner->UTPlayerState == PS ? FMath::RoundToInt(PS->ExactPing) : PS->Ping * 4);
				int32 Hits = PS->GetStatsValue(NAME_InstagibHits), Shots = PS->GetStatsValue(NAME_InstagibShots);
				if (GetWorld()->GetNetMode() == NM_Client && AccuracyReplicator.IsValid())
				{
					const FString Id = PS->UniqueId.IsValid() ? PS->UniqueId.ToString() : FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
					AccuracyReplicator->GetAccuracyForPlayer(Id, NAME_InstagibHits, NAME_InstagibShots, Hits, Shots);
				}
				Cached.Accuracy = Shots > 0 ? FString::Printf(TEXT("%.0f%%"), FMath::Min(100.f, 100.f * Hits / Shots)) : TEXT("--");
			}
			FText Fitted; float NW, NH;
			NCPlusHUDDrawCall::ResolveFittedName(Canvas, PS, Font, Cached.Name, PanelW * .46f, .60f * S, Fitted, NW, NH);
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, Font, Fitted, PX + 14.f * S, RY + 4.f * S, .60f * S, .60f * S, FColor::White, true);
			const FString* Cells[] = { &Cached.Frags, &Cached.Deaths, &Cached.Accuracy, &Cached.Ping };
			for (int32 C = 0; C < 4; ++C)
				HUD->Text(Canvas, Font, *Cells[C], PX + PanelW * Columns[C], RY + 9.f * S, .59f * S, FLinearColor::White, true);
			FText Ready;
			FString Status = HUD->PlayerStatus(PS, Rep);
			if (!UTGameState->HasMatchStarted() && NCPlusScoreboardReady::TryGetText(GetWorld(), PS, TeamSwapText, Ready)) Status = Ready.ToString();
			HUD->Text(Canvas, Font, Status, PX + 14.f * S, RY + 25.f * S, .36f * S, Col);
			PS->ScoreCorner = FVector(PX + PanelW, RY, 0.f);
		}
	}
	int32 Spectators = 0;
	for (APlayerState* PS : UTGameState->PlayerArray) if (PS && PS->bOnlySpectator && !PS->bIsInactive) ++Spectators;
	HUD->Text(Canvas, Font, FString::Printf(TEXT("%d SPECTATORS   |   ARROWS: SELECT PLAYER   |   ENTER: VIEW / PLAYER CARD"), Spectators),
		Canvas->ClipX * .5f, Y + 652.f * S, .44f * S, FLinearColor(.65f, .7f, .78f), true);
}

void UNCPlusXTDMScoreboard::SelectNext(int32 Offset, bool bDoNoWrap)
{
	if (SelectionStack.Num() == 0) return;
	int32 Index = SelectionStack.IndexOfByPredicate([this](const FSelectionObject& Entry) { return Entry.ScoreOwner == SelectedPlayer; });
	if (Index == INDEX_NONE) Index = Offset < 0 ? SelectionStack.Num() - 1 : 0;
	else
	{
		Index += Offset;
		if (bDoNoWrap && (Index < 0 || Index >= SelectionStack.Num())) return;
		Index = (Index % SelectionStack.Num() + SelectionStack.Num()) % SelectionStack.Num();
	}
	SelectedPlayer = SelectionStack[Index].ScoreOwner;
}

void UNCPlusXTDMScoreboard::SelectOtherColumn()
{
	ANCPlusXTDMHUD* HUD = Cast<ANCPlusXTDMHUD>(UTHUDOwner);
	AUTPlayerState* PS = SelectedPlayer.Get();
	if (!HUD || !PS || PS->GetTeamNum() >= 4) { SelectNext(1); return; }
	TArray<AUTPlayerState*> Old, Next;
	HUD->GetTeamRoster(PS->GetTeamNum(), Old);
	HUD->GetTeamRoster(PS->GetTeamNum() ^ 1, Next);
	if (Next.Num() > 0) SelectedPlayer = Next[FMath::Clamp(Old.Find(PS), 0, Next.Num() - 1)];
}

void UNCPlusXTDMScoreboard::SelectionLeft() { SelectOtherColumn(); }
void UNCPlusXTDMScoreboard::SelectionRight() { SelectOtherColumn(); }

void UNCPlusXTDMScoreboard::SelectionClick()
{
	AUTPlayerState* PS = SelectedPlayer.Get();
	if (PS && UTPlayerOwner && UTPlayerOwner->UTPlayerState && UTPlayerOwner->UTPlayerState->bOnlySpectator
		&& PS->SpectatingIDTeam > 0 && UTGameState && UTGameState->CanSpectate(UTPlayerOwner, PS))
	{
		UTPlayerOwner->ViewPlayerNum(PS->SpectatingIDTeam, PS->GetTeamNum());
		return;
	}
	Super::SelectionClick();
}
