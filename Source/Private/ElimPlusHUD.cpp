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
#include "ElimPlusStatsReplicator.h"
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
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Paperdoll.bpHW_Paperdoll_C"));
	// Removed bpHW_TeamGameClock — we draw our own team score bar in DrawHUD
	// that respects dynamic team colors from TeamSkins.
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_QuickStats.bpHW_QuickStats_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ElimPlusScoreboard"));
}

EInputMode::Type AElimPlusHUD::GetInputMode_Implementation() const
{
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

	if (!bPostMatchScreenshotTaken)
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS && GS->HasMatchEnded())
		{
			FString Val;
			FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
			if (GConfig->GetString(TEXT("NetcodePlus"), TEXT("HighResScreenshotPostMatch"), Val, ConfigPath))
			{
				bNCPScreenshotEnabled = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
			}

			if (bNCPScreenshotEnabled)
			{
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

void AElimPlusHUD::GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	AUTPlayerState* HUDPS = GetScorerPlayerState();
	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (UTPS != nullptr && !UTPS->bOnlySpectator && !UTPS->bIsInactive
			&& (UTPS->Team != nullptr || UTPS->GetTeamNum() != 255))
		{
			UTPS->SelectionOrder = (UTPS == HUDPS) ? -1 : UTPS->SpectatingIDTeam;
			SortedPlayers.Add(UTPS);
		}
	}
	SortedPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B) { return A.SelectionOrder > B.SelectionOrder; });
}

// Helper: locate the stats replicator on this client (works on server too).
static AElimPlusStatsReplicator* FindElimPlusStatsReplicator(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AElimPlusStatsReplicator> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AElimPlusHUD::DrawHUD()
{
	Super::DrawHUD();

	// Guard: Canvas or fonts may be null during Slate UI overlays
	if (!Canvas || !SmallFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	const bool bScoreboardIsUp = ScoreboardIsUp();

	// Custom team score bar (replaces bpHW_TeamGameClock — respects TeamSkins).
	if (GS && !bScoreboardIsUp)
	{
		DrawTeamScoreBar(GS);
	}

	if (!bScoreboardIsUp && GS && GS->GetMatchState() == MatchState::InProgress)
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;
		const float TeammateScale = 0.4f;

		const float BasePipSize = (32 + (64 * TeammateScale)) * GetHUDWidgetScaleOverride() * RenderScale;
		const float XAdjust = BasePipSize * 1.1f;
		float XOffsetRed  = 0.4f * Canvas->ClipX - XAdjust - BasePipSize;
		float XOffsetBlue = 0.6f * Canvas->ClipX + XAdjust;
		const float YOffset = 0.005f * Canvas->ClipY * GetHUDWidgetScaleOverride() * RenderScale;

		TArray<AUTPlayerState*> LivePlayers;
		GetPlayerListForIcons(LivePlayers);

		// Last-man-standing detection: count alive per team in this pass below.
		// We do a quick pre-pass to find who's the lone survivor (if any) so we
		// can pulse their portrait.
		int32 AliveCountTeam[2] = { 0, 0 };
		AUTPlayerState* SoleSurvivor[2] = { nullptr, nullptr };
		for (AUTPlayerState* UTPS : LivePlayers)
		{
			const uint8 T = UTPS ? UTPS->GetTeamNum() : 255;
			if (T > 1) continue;
			AUTCharacter* UTC = nullptr;
			AController* C = Cast<AController>(UTPS->GetOwner());
			if (C != nullptr) { UTC = Cast<AUTCharacter>(C->GetPawn()); }
			else              { UTC = UTPS->GetUTCharacter(); }
			const bool bAlive = (UTC != nullptr && !UTC->IsDead());
			if (bAlive)
			{
				AliveCountTeam[T]++;
				SoleSurvivor[T] = UTPS;
			}
		}

		// Clear the per-team marker if the team has more than one alive
		if (AliveCountTeam[0] != 1) SoleSurvivor[0] = nullptr;
		if (AliveCountTeam[1] != 1) SoleSurvivor[1] = nullptr;

		AElimPlusStatsReplicator* Stats = FindElimPlusStatsReplicator(GetWorld());

		for (AUTPlayerState* UTPS : LivePlayers)
		{
			const float OwnerPipScaling = (UTPS == GetScorerPlayerState()) ? 1.25f : 1.f;
			const float PipSize = BasePipSize * OwnerPipScaling;
			const float PipHeight = PipSize * (320.0f / 224.0f);

			// Alive check — server uses controller's pawn directly; clients fall
			// back to GetUTCharacter() since GetOwner() is null for remote PSs.
			bool bPlayerAlive = false;
			AController* C = Cast<AController>(UTPS->GetOwner());
			if (C != nullptr)
			{
				AUTCharacter* UTC = Cast<AUTCharacter>(C->GetPawn());
				bPlayerAlive = (UTC != nullptr && !UTC->IsDead());
			}
			else
			{
				AUTCharacter* UTC = UTPS->GetUTCharacter();
				bPlayerAlive = (UTC != nullptr && !UTC->IsDead());
			}

			// In elim, dead = stays dead until round end. Hide portrait entirely
			// so the strip reflows toward the center as players die.
			if (!bPlayerAlive)
			{
				continue;
			}

			const uint8 TeamIdx = UTPS->GetTeamNum();
			float* XOffsetForTeam = nullptr;
			if (TeamIdx == 0)      { RedPlayerCount++;  XOffsetForTeam = &XOffsetRed; }
			else if (TeamIdx == 1) { BluePlayerCount++; XOffsetForTeam = &XOffsetBlue; }
			else                   { continue; }

			const float XOffset = *XOffsetForTeam;
			DrawPlayerIcon(UTPS, bPlayerAlive, XOffset, YOffset, PipSize);

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
				const float NameX = XOffset + (PipSize * 0.5f) - (NXL * NameScale * 0.5f);
				const float NameY = YOffset + 2.f;
				const float OL = 1.f;
				Canvas->SetLinearDrawColor(FLinearColor::Black);
				Canvas->DrawText(TinyFont, FText::FromString(Name), NameX - OL, NameY, NameScale, NameScale, NameRI);
				Canvas->DrawText(TinyFont, FText::FromString(Name), NameX + OL, NameY, NameScale, NameScale, NameRI);
				Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY - OL, NameScale, NameScale, NameRI);
				Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY + OL, NameScale, NameScale, NameRI);
				Canvas->SetLinearDrawColor(FLinearColor::White);
				Canvas->DrawText(TinyFont, FText::FromString(Name), NameX, NameY, NameScale, NameScale, NameRI);
			}

			// Last-man-standing pulse: white-flashing border at 1Hz when this
			// PS is the sole surviving member of their team.
			if (UTPS == SoleSurvivor[TeamIdx])
			{
				const float Pulse = 0.5f + 0.5f * FMath::Sin(GetWorld()->TimeSeconds * 2.f * PI);
				FLinearColor PulseColor = FMath::Lerp(FLinearColor::White, FLinearColor(1.f, 1.f, 0.4f, 1.f), Pulse);
				PulseColor.A = 0.9f;
				const float BorderW = 2.f;
				Canvas->SetLinearDrawColor(PulseColor);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset + PipHeight - BorderW, PipSize, BorderW, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, BorderW, PipHeight, 0, 0, 1, 1);
				Canvas->DrawTile(Canvas->DefaultTexture, XOffset + PipSize - BorderW, YOffset, BorderW, PipHeight, 0, 0, 1, 1);
			}

			// ELO chip below the portrait. At match end, count up over EloAnimDurationSec
			// from (Elo - Delta) to Elo with green/red color fade. Trigger is the first
			// frame the replicator returns Delta != 0; self-clears when Delta returns to 0.
			// Bots use the synthetic "BOT:<name>" key so randomized bot ELOs render too.
			if (Stats && UTPS)
			{
				const FString UidStr = UTPS->UniqueId.IsValid()
					? UTPS->UniqueId.ToString()
					: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
				const int32 ServerElo = Stats->GetEloForPlayer(UidStr);
				const int32 ServerDelta = Stats->GetEloDeltaForPlayer(UidStr);

				int32 DisplayElo = ServerElo;
				int32 DisplayDelta = ServerDelta;
				float ColorBlend = 1.f;  // 1 = full final color, 0 = white

				if (ServerDelta == 0)
				{
					// Mid-match (or pre-match). Drop any stale anim entry.
					EloAnimByPlayerId.Remove(UidStr);
				}
				else
				{
					FElimPlusEloAnim& Anim = EloAnimByPlayerId.FindOrAdd(UidStr);
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
				FFontRenderInfo ChipRI;
				ChipRI.bEnableShadow = true;

				FString EloStr = FString::Printf(TEXT("%d"), DisplayElo);
				if (DisplayDelta != 0)
				{
					EloStr += (DisplayDelta > 0)
						? FString::Printf(TEXT(" +%d"), DisplayDelta)
						: FString::Printf(TEXT(" %d"), DisplayDelta);
				}
				float CXL, CYL;
				Canvas->StrLen(TinyFont, EloStr, CXL, CYL);
				const float ChipX = XOffset + (PipSize * 0.5f) - (CXL * ChipScale * 0.5f);
				const float ChipY = YOffset + PipHeight + 2.f;
				const float OL = 1.f;

				Canvas->SetLinearDrawColor(FLinearColor::Black);
				Canvas->DrawText(TinyFont, FText::FromString(EloStr), ChipX - OL, ChipY, ChipScale, ChipScale, ChipRI);
				Canvas->DrawText(TinyFont, FText::FromString(EloStr), ChipX + OL, ChipY, ChipScale, ChipScale, ChipRI);
				Canvas->DrawText(TinyFont, FText::FromString(EloStr), ChipX, ChipY - OL, ChipScale, ChipScale, ChipRI);
				Canvas->DrawText(TinyFont, FText::FromString(EloStr), ChipX, ChipY + OL, ChipScale, ChipScale, ChipRI);

				FLinearColor TargetColor = FLinearColor::White;
				if (DisplayDelta > 0)      TargetColor = FLinearColor(0.4f, 1.f, 0.4f, 1.f);
				else if (DisplayDelta < 0) TargetColor = FLinearColor(1.f, 0.4f, 0.4f, 1.f);
				const FLinearColor EloColor = FMath::Lerp(FLinearColor::White, TargetColor, ColorBlend);
				Canvas->SetLinearDrawColor(EloColor);
				Canvas->DrawText(TinyFont, FText::FromString(EloStr), ChipX, ChipY, ChipScale, ChipScale, ChipRI);
			}

			// Advance the column offset for the next portrait
			if (TeamIdx == 0) XOffsetRed  -= 1.1f * PipSize;
			else              XOffsetBlue += 1.1f * PipSize;
		}

		// Score / KDA mini widget (top right) — same as Wipeout
		AUTPlayerState* MyPS = GetScorerPlayerState();
		if (MyPS && Canvas && SmallFont)
		{
			const int32 Score = FMath::TruncToInt(MyPS->Score);
			const int32 Kills = MyPS->Kills;
			const int32 Deaths = MyPS->Deaths;
			const int32 Assists = MyPS->KillAssists;

			FString ScoreStr = FString::Printf(TEXT("Score: %d"), Score);
			FString KDAStr = FString::Printf(TEXT("KDA: %d / %d / %d"), Kills, Deaths, Assists);

			float KDAXPos = Canvas->ClipX * 0.98f;
			float KDAYPos = Canvas->ClipY * 0.015f;
			const float FontScale = RenderScale * 0.9f;

			float XL, YL;
			Canvas->TextSize(SmallFont, ScoreStr, XL, YL, FontScale, FontScale);
			Canvas->DrawColor = FColor(255, 255, 255, 220);
			Canvas->DrawText(SmallFont, ScoreStr, KDAXPos - XL, KDAYPos, FontScale, FontScale);
			KDAYPos += YL * 1.1f;

			Canvas->TextSize(SmallFont, KDAStr, XL, YL, FontScale, FontScale);
			Canvas->DrawColor = FColor(200, 200, 200, 200);
			Canvas->DrawText(SmallFont, KDAStr, KDAXPos - XL, KDAYPos, FontScale, FontScale);
		}
	}
}

// Custom team score bar — dynamic team colors, round clock. Same pattern as Wipeout.
void AElimPlusHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !SmallFont || !MediumFont || !LargeFont) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const float TopY = 2.f * RenderScale;

	FLinearColor Team0Color = FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	FLinearColor Team1Color = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);
	bool bCustomColors = false;

	if (GS->Teams.IsValidIndex(0) && GS->Teams[0])
	{
		FLinearColor TC = GS->Teams[0]->TeamColor;
		if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f)
			bCustomColors = true;
		Team0Color = TC;
	}
	if (GS->Teams.IsValidIndex(1) && GS->Teams[1])
	{
		FLinearColor TC = GS->Teams[1]->TeamColor;
		if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f)
			bCustomColors = true;
		Team1Color = TC;
	}

	FString Team0Name = bCustomColors ? TEXT("Liandri") : TEXT("RED");
	FString Team1Name = bCustomColors ? TEXT("Phayder") : TEXT("BLUE");

	const int32 Score0 = GS->Teams.IsValidIndex(0) && GS->Teams[0] ? GS->Teams[0]->Score : 0;
	const int32 Score1 = GS->Teams.IsValidIndex(1) && GS->Teams[1] ? GS->Teams[1]->Score : 0;

	const float BarWidth = 220.f * RenderScale;
	const float BarHeight = 36.f * RenderScale;
	const float ScoreBoxWidth = 50.f * RenderScale;
	const float GapWidth = 8.f * RenderScale;

	const float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(Team0Color);
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(Team1Color);
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float TailHeight = 14.f * RenderScale;
	Canvas->SetLinearDrawColor(FLinearColor(Team0Color.R * 0.7f, Team0Color.G * 0.7f, Team0Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FLinearColor(Team1Color.R * 0.7f, Team1Color.G * 0.7f, Team1Color.B * 0.7f, 1.f));
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	Canvas->SetLinearDrawColor(FLinearColor::White);
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	const float FontScale = RenderScale * 0.85f;
	const float LargeFontScale = RenderScale * 1.2f;
	float XL, YL;

	Canvas->TextSize(SmallFont, Team0Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team0Name, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	FString Score0Str = FString::Printf(TEXT("%d"), Score0);
	Canvas->TextSize(LargeFont, Score0Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score0Str, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	FString Score1Str = FString::Printf(TEXT("%d"), Score1);
	Canvas->TextSize(LargeFont, Score1Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(LargeFont, Score1Str, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	Canvas->TextSize(SmallFont, Team1Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(SmallFont, Team1Name, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// Round Clock — read RoundSecondsRemaining from BP GameState via reflection
	const float ClockY = TopY + BarHeight + 2.f * RenderScale;
	int32 RoundTime = -1;
	UIntProperty* RoundTimeProp = FindField<UIntProperty>(GS->GetClass(), TEXT("RoundSecondsRemaining"));
	if (RoundTimeProp)
	{
		RoundTime = RoundTimeProp->GetPropertyValue_InContainer(GS);
	}

	const float RoundClockScale = RenderScale * 1.1f;
	if (RoundTime >= 0)
	{
		const int32 RMins = RoundTime / 60;
		const int32 RSecs = RoundTime % 60;
		FString RoundClockStr = FString::Printf(TEXT("%02d:%02d"), RMins, RSecs);
		Canvas->TextSize(MediumFont, RoundClockStr, XL, YL, RoundClockScale, RoundClockScale);
		Canvas->DrawColor = (RoundTime <= 30) ? FColor(255, 60, 60, 255) : FColor::White;
		Canvas->DrawText(MediumFont, RoundClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
	}
}

void AElimPlusHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, bool bPlayerAlive, float XOffset, float YOffset, float PipSize)
{
	const FCanvasIcon& CharIcon = PlayerState->GetHUDIcon();
	if (CharIcon.Texture == nullptr)
	{
		return;
	}

	Canvas->SetLinearDrawColor(FLinearColor::White);
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

	// Layer 1: Team-colored background (TeamSkins-aware)
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	FLinearColor TeamBGColor = (PlayerState->GetTeamNum() == 1)
		? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)
		: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);
	if (GS && GS->Teams.IsValidIndex(PlayerState->GetTeamNum()) && GS->Teams[PlayerState->GetTeamNum()])
	{
		TeamBGColor = GS->Teams[PlayerState->GetTeamNum()]->TeamColor;
	}
	Canvas->SetLinearDrawColor(TeamBGColor);
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FLinearColor::White);

	// Layer 2: Character portrait (dimmed if dead)
	if (!bPlayerAlive)
	{
		Canvas->SetLinearDrawColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.f));
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
		Canvas->SetLinearDrawColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipSize, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	Canvas->SetLinearDrawColor(FLinearColor::White);
	const FCanvasIcon& OverlayIcon = PlayerState->GetTeamNum() == 1 ? BlueTeamOverlay : RedTeamOverlay;
	Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipSize, PipHeight,
		OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);

	// Layer 5: Red "X" on dead portraits — always (no respawn this round)
	if (!bPlayerAlive)
	{
		const float FontRenderScale = float(Canvas->SizeY) / 1080.0f;
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;

		FString XStr = TEXT("X");
		float XL, YL;
		Canvas->StrLen(SmallFont, XStr, XL, YL);

		Canvas->SetLinearDrawColor(FLinearColor(1.f, 0.2f, 0.2f, 0.9f));
		Canvas->DrawText(SmallFont, FText::FromString(XStr),
			XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
	}

	// Layer 6: Teammate HP/Armor numbers (alive teammates only, not self)
	if (bPlayerAlive && UTPlayerOwner)
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

				const int32 HP = UTC->Health;
				const int32 Armor = UTC->GetArmorAmount();
				FString HPStr = FString::Printf(TEXT("%d/%d"), HP, Armor);

				float XL, YL;
				Canvas->StrLen(SmallFont, HPStr, XL, YL);

				const float TextX = XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f);
				const float TextY = YOffset + PipHeight - (YL * FontRenderScale) - 2.f;
				const float OutlineOffset = 1.f;
				Canvas->SetLinearDrawColor(FLinearColor::Black);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX - OutlineOffset, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX + OutlineOffset, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY - OutlineOffset, FontRenderScale, FontRenderScale, TextRenderInfo);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY + OutlineOffset, FontRenderScale, FontRenderScale, TextRenderInfo);

				Canvas->SetLinearDrawColor(FLinearColor::White);
				Canvas->DrawText(SmallFont, FText::FromString(HPStr), TextX, TextY, FontRenderScale, FontRenderScale, TextRenderInfo);
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
