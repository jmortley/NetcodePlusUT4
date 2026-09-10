#include "NCPlusXTDMHUD.h"

#include "NCPlusXTDMReplicator.h"
#include "NCPlusXTDMScoreboard.h"
#include "NCPlusXTDMSpectator.h"
#include "NCPlusXTDMMessage.h"
#include "NCPlusHUDLayout.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTLocalPlayer.h"
#include "UTTeamInfo.h"
#include "UTCTFGameMessage.h"
#include "Components/InputComponent.h"
#include "Engine/Canvas.h"

ANCPlusXTDMHUD::ANCPlusXTDMHUD(const FObjectInitializer& OI) : Super(OI)
{
	HudWidgetClasses.Empty();
	RequiredHudWidgetClasses.Empty();
	SpectatorHudWidgetClasses.Empty();
	const TCHAR* Widgets[] = {
		TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"),
		TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"),
		TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"),
		TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"),
		TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"),
		TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Spectator"),
		TEXT("/Script/NetcodePlus.NCPlusHUDWidget_ReadyUp"),
		TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AutoPause"),
		TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"),
		TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Speedometer"),
		TEXT("/Script/NetcodePlus.NCPlusXTDMScoreboard")
	};
	for (const TCHAR* Widget : Widgets) HudWidgetClasses.Add(Widget);
	for (int32 Team = 0; Team < 4; ++Team) CachedScores[Team] = MIN_int32;
	bDrawMinimap = false;
}

void ANCPlusXTDMHUD::BeginPlay()
{
	Super::BeginPlay();
	FNCPlusHUDLayout::ReloadLive();
	CaptureWidgetDefaults(this);
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
}

void ANCPlusXTDMHUD::AddSpectatorWidgets()
{
	if (!SpectatorSlideOutWidget)
		SpectatorSlideOutWidget = Cast<UNCPlusXTDMSpectator>(AddHudWidget(UNCPlusXTDMSpectator::StaticClass()));
}

FString ANCPlusXTDMHUD::TeamLabel(uint8 Team)
{
	static const TCHAR* Labels[] = { TEXT("RED"), TEXT("BLUE"), TEXT("GREEN"), TEXT("YELLOW") };
	return Team < 4 ? Labels[Team] : TEXT("SPECTATOR");
}

FLinearColor ANCPlusXTDMHUD::TeamColor(const AUTGameState* GS, uint8 Team)
{
	// HUD labels need readable luminance against dark cards; character colors remain the replicated team palette.
	static const FLinearColor Colors[] = { FLinearColor(1.f, .22f, .22f), FLinearColor(.2f, .5f, 1.f),
		FLinearColor(.25f, 1.f, .38f), FLinearColor(1.f, .8f, .15f) };
	return Team < 4 ? Colors[Team] : FLinearColor::White;
}

void ANCPlusXTDMHUD::Text(UCanvas* C, UFont* Font, const FString& Value,
	float X, float Y, float Scale, FLinearColor Color, bool bCentered)
{
	if (!C || !Font) return;
	FText Resolved;
	float W, H;
	NCPlusHUDDrawCall::ResolveStableText(C, Font, Value, Scale, Scale, Resolved, W, H);
	NCPlusHUDDrawCall::DrawResolvedText(C, Font, Resolved, bCentered ? X - W * .5f : X,
		Y, Scale, Scale, Color.ToFColor(true), true);
}

void ANCPlusXTDMHUD::Tile(UCanvas* C, float X, float Y, float W, float H, FLinearColor Color)
{
	if (!C) return;
	C->SetLinearDrawColor(Color);
	C->DrawTile(C->DefaultTexture, X, Y, W, H, 0, 0, 1, 1, BLEND_Translucent);
}

void ANCPlusXTDMHUD::GetTeamRoster(uint8 Team, TArray<AUTPlayerState*>& Out)
{
	Out.Reset();
	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS || Team >= 4) return;
	const float Now = GetWorld()->TimeSeconds;
	if (RosterGameState.Get() != GS || Now >= NextRosterRefresh || NextRosterRefresh - Now > .25f)
	{
		RosterGameState = GS;
		NextRosterRefresh = Now + .25f;
		for (auto& Roster : TeamRosters) Roster.Reset();
		for (APlayerState* Player : GS->PlayerArray)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(Player);
			if (IsValid(PS) && !PS->bOnlySpectator && !PS->bIsInactive && PS->GetTeamNum() < 4)
				TeamRosters[PS->GetTeamNum()].Add(PS);
		}
		for (auto& Roster : TeamRosters)
			Roster.Sort([](const TWeakObjectPtr<AUTPlayerState>& A, const TWeakObjectPtr<AUTPlayerState>& B)
			{
				return A->PlayerId < B->PlayerId;
			});
	}
	for (const auto& Weak : TeamRosters[Team])
		if (AUTPlayerState* PS = Weak.Get())
			if (!PS->bOnlySpectator && !PS->bIsInactive && PS->GetTeamNum() == Team) Out.Add(PS);
}

FString ANCPlusXTDMHUD::PlayerStatus(AUTPlayerState* PS, ANCPlusXTDMReplicator* Rep) const
{
	const FNCPlusXTDMPlayerStatus* Status = Rep ? Rep->FindPlayer(PS) : nullptr;
	if (!Status) return TEXT("...");
	if (Rep->bMatchEnded) return TEXT("FINISHED");
	if (Status->bAlive) return TEXT("ALIVE");
	const AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	const float Delay = GS ? Status->RespawnReadyServerTime - GS->GetServerWorldTimeSeconds() : 0.f;
	return Delay > 0.f ? FString::Printf(TEXT("RESPAWN %ds"), FMath::CeilToInt(Delay)) : TEXT("RESPAWNING");
}

void ANCPlusXTDMHUD::DrawScoreStrip(AUTGameState* GS)
{
	const FName Alias(TEXT("xtdm_scorebar"));
	if (NCPlusHUDDrawCall::IsHidden(Alias)) return;
	const float S = Canvas->ClipY / 1080.f * GetHUDWidgetScaleOverride() * NCPlusHUDDrawCall::GetScale(Alias);
	const float Opacity = NCPlusHUDDrawCall::GetOpacity(Alias);
	const FVector2D P = NCPlusHUDDrawCall::ResolveScreenPos(Alias, Canvas, FVector2D(Canvas->ClipX * .5f, 16.f * Canvas->ClipY / 1080.f));
	UFont* Font = NCPlusHUDFonts::Resolve(Alias, this, SmallFont);
	const float FontScale = S * NCPlusHUDFonts::ResolveScale(Alias);
	const int32 Seconds = FMath::Max(0, FMath::FloorToInt(GS->GetClockTime()));
	if (Seconds != CachedClock)
	{
		CachedClock = Seconds;
		ClockString = FString::Printf(TEXT("%02d:%02d"), Seconds / 60, Seconds % 60);
	}
	const FString Phase = !GS->HasMatchStarted() ? TEXT("WARMUP") : GS->HasMatchEnded() ? TEXT("FINAL")
		: GS->IsMatchInOvertime() ? TEXT("OT ") + ClockString : ClockString;
	Text(Canvas, Font, Phase, P.X, P.Y, FontScale, FLinearColor(1, 1, 1, Opacity), true);
	const uint8 LocalTeam = UTPlayerOwner && UTPlayerOwner->UTPlayerState ? UTPlayerOwner->UTPlayerState->GetTeamNum() : 255;
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		const float X = P.X + (Team * 118.f - 233.f) * S;
		FLinearColor Col = TeamColor(GS, Team); Col.A = Opacity;
		Tile(Canvas, X, P.Y + 35.f * S, 112.f * S, 64.f * S, FLinearColor(.018f, .024f, .035f, .88f * Opacity));
		Tile(Canvas, X, P.Y + 35.f * S, 112.f * S, 3.f * S, Col);
		const int32 Score = GS->Teams.IsValidIndex(Team) && GS->Teams[Team] ? GS->Teams[Team]->Score : 0;
		if (CachedScores[Team] != Score) { CachedScores[Team] = Score; ScoreStrings[Team] = FString::FromInt(Score); }
		Text(Canvas, Font, TeamLabel(Team), X + 56.f * S, P.Y + 41.f * S, .56f * FontScale, Col, true);
		Text(Canvas, Font, ScoreStrings[Team], X + 56.f * S, P.Y + 59.f * S, 1.03f * FontScale,
			FLinearColor(1, 1, 1, Opacity), true);
		if (Team == LocalTeam)
		{
			Tile(Canvas, X, P.Y + 96.f * S, 112.f * S, 3.f * S, Col);
			Text(Canvas, Font, TEXT("YOUR TEAM"), X + 56.f * S, P.Y + 103.f * S, .46f * FontScale, Col, true);
		}
	}
}

void ANCPlusXTDMHUD::DrawTeammates(AUTGameState* GS)
{
	const FName Alias(TEXT("xtdm_teammates"));
	AUTPlayerState* Local = UTPlayerOwner ? UTPlayerOwner->UTPlayerState : nullptr;
	if (!Local || Local->bOnlySpectator || Local->GetTeamNum() >= 4 || NCPlusHUDDrawCall::IsHidden(Alias)) return;
	const float S = Canvas->ClipY / 1080.f * GetHUDWidgetScaleOverride() * NCPlusHUDDrawCall::GetScale(Alias);
	const float Opacity = NCPlusHUDDrawCall::GetOpacity(Alias);
	const FVector2D P = NCPlusHUDDrawCall::ResolveScreenPos(Alias, Canvas, FVector2D(22.f * Canvas->ClipY / 1080.f, Canvas->ClipY * .68f));
	UFont* Font = NCPlusHUDFonts::Resolve(Alias, this, SmallFont);
	const float FontScale = S * NCPlusHUDFonts::ResolveScale(Alias);
	TArray<AUTPlayerState*> Players; GetTeamRoster(Local->GetTeamNum(), Players);
	ANCPlusXTDMReplicator* Rep = ANCPlusXTDMReplicator::Find(GetWorld());
	float Y = P.Y;
	for (AUTPlayerState* PS : Players)
	{
		if (PS == Local) continue;
		FLinearColor Col = TeamColor(GS, Local->GetTeamNum()); Col.A = Opacity;
		Tile(Canvas, P.X, Y, 292.f * S, 47.f * S, FLinearColor(.02f, .025f, .035f, .75f * Opacity));
		Tile(Canvas, P.X, Y, 3.f * S, 47.f * S, Col);
		FText Fitted; float W, H;
		NCPlusHUDDrawCall::ResolveFittedName(Canvas, PS, Font, PS->PlayerName, 265.f * S, .62f * FontScale, Fitted, W, H);
		NCPlusHUDDrawCall::DrawResolvedText(Canvas, Font, Fitted, P.X + 12.f * S, Y + 3.f * S, .62f * FontScale, .62f * FontScale, Col.ToFColor(true), true);
		Text(Canvas, Font, PlayerStatus(PS, Rep), P.X + 12.f * S, Y + 26.f * S, .45f * FontScale, FLinearColor(1, 1, 1, Opacity));
		Y += 51.f * S;
	}
}

void ANCPlusXTDMHUD::DrawHUD()
{
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());
	UpdateSpectatorInput();
	Super::DrawHUD();
	if (!Canvas || !SmallFont || !bShowUTHUD || !UTPlayerOwner
		|| (!bShowHUD && UTPlayerOwner->bCinematicMode)) return;
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS && !ScoreboardIsUp()) { DrawScoreStrip(GS); DrawTeammates(GS); }
	NCPlusHUDDrawCall::DrawServerInfo(this, Canvas);
	NCPlusHUDDrawCall::ServicePostMatchScreenshot(this, PostMatchScreenshotStable, bPostMatchScreenshotTaken);
	NCPlusHUDDrawCall::DrawDamageFlash(this, Canvas);
}

FLinearColor ANCPlusXTDMHUD::GetBaseHUDColor()
{
	const uint8 Team = UTPlayerOwner && UTPlayerOwner->UTPlayerState ? UTPlayerOwner->UTPlayerState->GetTeamNum() : 255;
	FLinearColor Color = TeamColor(GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr, Team) * .15f;
	Color.A = 1.f;
	return Color;
}

EInputMode::Type ANCPlusXTDMHUD::GetInputMode_Implementation() const
{
	return NCPlusHUDDragMode::IsActive() ? EInputMode::EIM_GameAndUI : Super::GetInputMode_Implementation();
}

void ANCPlusXTDMHUD::ViewTeamSlot(uint8 Team, int32 Slot)
{
	if (!UTPlayerOwner || !UTPlayerOwner->UTPlayerState || !UTPlayerOwner->UTPlayerState->bOnlySpectator) return;
	TArray<AUTPlayerState*> Players; GetTeamRoster(Team, Players);
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (Players.IsValidIndex(Slot) && GS && Players[Slot]->SpectatingIDTeam > 0 && GS->CanSpectate(UTPlayerOwner, Players[Slot]))
		UTPlayerOwner->ViewPlayerNum(Players[Slot]->SpectatingIDTeam, Team);
}

void ANCPlusXTDMHUD::UpdateSpectatorInput()
{
	if (!UTPlayerOwner) return;
	const AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	const bool bWant = UTPlayerOwner->UTPlayerState && UTPlayerOwner->UTPlayerState->bOnlySpectator
		&& GS && GS->HasMatchStarted() && !GS->HasMatchEnded() && !ScoreboardIsUp()
		&& !NCPlusHUDDragMode::IsActive();
	if (bWant == bSpectatorInputActive) return;
	bSpectatorInputActive = bWant;
	if (!bWant) { DisableInput(UTPlayerOwner); return; }
	EnableInput(UTPlayerOwner);
	if (!InputComponent || InputComponent->KeyBindings.Num() > 0) return;
	const FKey TeamKeys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four };
	const FKey SlotKeys[] = { EKeys::Q, EKeys::W, EKeys::E, EKeys::R };
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		FInputKeyBinding Binding(FInputChord(TeamKeys[Team], false, false, true, false), IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([this, Team]()
		{
			if (UTPlayerOwner && !UTPlayerOwner->AreMenusOpen()) { KeyboardSpectatorTeam = Team; ViewTeamSlot(Team, 0); }
		});
		InputComponent->KeyBindings.Add(Binding);
		FInputKeyBinding SlotBinding(FInputChord(SlotKeys[Team], false, false, true, false), IE_Pressed);
		SlotBinding.KeyDelegate.GetDelegateForManualSet().BindLambda([this, Team]()
		{
			if (UTPlayerOwner && !UTPlayerOwner->AreMenusOpen()) ViewTeamSlot(KeyboardSpectatorTeam, Team);
		});
		InputComponent->KeyBindings.Add(SlotBinding);
	}
}

void ANCPlusXTDMHUD::ReceiveLocalMessage(TSubclassOf<UUTLocalMessage> MessageClass, APlayerState* Player1,
	APlayerState* Player2, uint32 Index, FText Value, UObject* OptionalObject)
{
	if (!MessageClass) return;
	if (MessageClass->IsChildOf(UUTCTFGameMessage::StaticClass())) return;
	if (MessageClass->IsChildOf(UUTGameMessage::StaticClass()) && (Index == 9 || Index == 10))
	{
		MessageClass = UNCPlusXTDMGameMessage::StaticClass();
		Value = MessageClass.GetDefaultObject()->ResolveMessage(Index, true, Player1, Player2, OptionalObject);
	}
	Super::ReceiveLocalMessage(MessageClass, Player1, Player2, Index, Value, OptionalObject);
}

void ANCPlusXTDMHUD::Destroyed()
{
	DisableInput(UTPlayerOwner);
	if (UTPlayerOwner)
		if (UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(UTPlayerOwner->Player)) LP->CloseSpectatorWindow();
	Super::Destroyed();
}
