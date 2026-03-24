// WipeoutHUD — FlagRun-style portrait strip for Wipeout game mode
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "WipeoutHUD.h"
#include "WipeoutScoreboard.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"

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
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Paperdoll.bpHW_Paperdoll_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_TeamGameClock.bpHW_TeamGameClock_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_QuickStats.bpHW_QuickStats_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"));
	HudWidgetClasses.Add(TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"));
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"));
	// Our custom portrait-row scoreboard
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
}

void AWipeoutHUD::GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	AUTPlayerState* HUDPS = GetScorerPlayerState();
	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (UTPS != nullptr && UTPS->Team != nullptr && !UTPS->bOnlySpectator && !UTPS->bIsInactive)
		{
			UTPS->SelectionOrder = (UTPS == HUDPS) ? -1 : UTPS->SpectatingIDTeam;
			SortedPlayers.Add(UTPS);
		}
	}
	SortedPlayers.Sort([](AUTPlayerState& A, AUTPlayerState& B) { return A.SelectionOrder > B.SelectionOrder; });
}

void AWipeoutHUD::DrawHUD()
{
	Super::DrawHUD();

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	bool bScoreboardIsUp = ScoreboardIsUp();

	if (!bScoreboardIsUp && GS && GS->GetMatchState() == MatchState::InProgress)
	{
		RedPlayerCount = 0;
		BluePlayerCount = 0;

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;
		float TeammateScale = 0.4f;

		float BasePipSize = (32 + (64 * TeammateScale)) * GetHUDWidgetScaleOverride() * RenderScale;
		float XAdjust = BasePipSize * 1.1f;
		float XOffsetRed = 0.4f * Canvas->ClipX - XAdjust - BasePipSize;
		float XOffsetBlue = 0.6f * Canvas->ClipX + XAdjust;
		float YOffset = 0.005f * Canvas->ClipY * GetHUDWidgetScaleOverride() * RenderScale;

		TArray<AUTPlayerState*> LivePlayers;
		GetPlayerListForIcons(LivePlayers);
		for (AUTPlayerState* UTPS : LivePlayers)
		{
			// In Wipeout everyone respawns, so show all non-spectator players
			float OwnerPipScaling = (UTPS == GetScorerPlayerState()) ? 1.25f : 1.f;
			float PipSize = BasePipSize * OwnerPipScaling;

			// Respawn progress: 0 = fully dead/waiting, 1 = alive
			float LiveScaling = 1.f;
			if (UTPS->RespawnTime > 0.f && UTPS->RespawnWaitTime > 0.f && !UTPS->GetUTCharacter())
			{
				LiveScaling = FMath::Clamp(1.f - UTPS->RespawnTime / UTPS->RespawnWaitTime, 0.f, 1.f);
			}
			else if (!UTPS->GetUTCharacter() && !UTPS->bOutOfLives)
			{
				// Dead but no respawn time info yet — show as dead
				LiveScaling = 0.f;
			}

			if (UTPS->Team->TeamIndex == 0)
			{
				RedPlayerCount++;
				DrawPlayerIcon(UTPS, LiveScaling, XOffsetRed, YOffset, PipSize);
				XOffsetRed -= 1.1f * PipSize;
			}
			else
			{
				BluePlayerCount++;
				DrawPlayerIcon(UTPS, LiveScaling, XOffsetBlue, YOffset, PipSize);
				XOffsetBlue += 1.1f * PipSize;
			}
		}
	}
}

void AWipeoutHUD::DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float PipSize)
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

	// Layer 1: Team-colored background
	const FCanvasIcon* BGIcon = PlayerState->GetTeamNum() == 1 ? &BlueTeamIcon : &RedTeamIcon;
	Canvas->DrawTile(BGIcon->Texture, XOffset, YOffset, PipSize, PipHeight,
		BGIcon->U, BGIcon->V, BGIcon->UL, BGIcon->VL);

	// Layer 2: Character portrait (dimmed if dead)
	if (LiveScaling < 1.f)
	{
		Canvas->SetLinearDrawColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.f));
	}

	BGIcon = &CharIcon;
	if (PlayerState->GetTeamNum() == 1)
	{
		// Blue team: flip horizontally (same as FlagRun)
		Canvas->DrawTile(BGIcon->Texture, XOffset, YOffset, PipSize, PipHeight,
			BGIcon->U + BGIcon->UL, BGIcon->V, BGIcon->UL * -1.0f, BGIcon->VL);
	}
	else
	{
		Canvas->DrawTile(BGIcon->Texture, XOffset, YOffset, PipSize, PipHeight,
			BGIcon->U, BGIcon->V, BGIcon->UL, BGIcon->VL);
	}

	// Layer 3: Respawn dark overlay sweeping from right to left
	if (LiveScaling < 1.f)
	{
		Canvas->SetLinearDrawColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XOffset + LiveScaling * PipSize, YOffset,
			PipSize - LiveScaling * PipSize, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	Canvas->SetLinearDrawColor(FLinearColor::White);
	BGIcon = PlayerState->GetTeamNum() == 1 ? &BlueTeamOverlay : &RedTeamOverlay;
	Canvas->DrawTile(BGIcon->Texture, XOffset, YOffset, PipSize, PipHeight,
		BGIcon->U, BGIcon->V, BGIcon->UL, BGIcon->VL);

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

		Canvas->SetLinearDrawColor(CountdownColor);
		Canvas->DrawText(SmallFont, FText::FromString(CountdownStr),
			XOffset + (PipSize * 0.5f) - (XL * FontRenderScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontRenderScale * 0.5f),
			FontRenderScale, FontRenderScale, TextRenderInfo);
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
