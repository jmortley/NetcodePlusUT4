#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "ClutchScoreboard.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTWeapon.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"


namespace
{
	static FString GetPhaseLabel(EClutchRoundPhase Phase)
	{
		switch (Phase)
		{
		case EClutchRoundPhase::Waiting: return TEXT("WAITING");
		case EClutchRoundPhase::Intermission: return TEXT("ROUND SETUP");
		case EClutchRoundPhase::Combat: return TEXT("POLE LOCKED");
		case EClutchRoundPhase::Capture: return TEXT("POLE ACTIVE");
		case EClutchRoundPhase::RoundEnd: return TEXT("ROUND OVER");
		case EClutchRoundPhase::MatchEnd: return TEXT("MATCH OVER");
		default: return TEXT("CLUTCH");
		}
	}

	static void DrawCenteredCanvasText(UCanvas* Canvas, UFont* Font,
		const FString& Text, float CenterX, float Y, float Scale, const FColor& Color)
	{
		if (!Canvas || !Font)
		{
			return;
		}
		float Width = 0.0f;
		float Height = 0.0f;
		Canvas->TextSize(Font, Text, Width, Height, Scale, Scale);
		Canvas->DrawColor = Color;
		Canvas->DrawText(Font, Text, CenterX - Width * 0.5f, Y, Scale, Scale);
	}

	static UTexture2D* LoadTextureWithFallback(
		const TCHAR* PrimaryPath, const TCHAR* FallbackPath = nullptr)
	{
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, PrimaryPath);
		return Texture || !FallbackPath
			? Texture
			: LoadObject<UTexture2D>(nullptr, FallbackPath);
	}

	static void DrawTextureTile(UCanvas* Canvas, UTexture2D* Texture,
		float X, float Y, float Width, float Height, const FLinearColor& Color)
	{
		if (!Canvas || !Texture)
		{
			return;
		}
		Canvas->SetLinearDrawColor(Color);
		Canvas->DrawTile(Texture, X, Y, Width, Height,
			0.0f, 0.0f,
			static_cast<float>(Texture->GetSizeX()),
			static_cast<float>(Texture->GetSizeY()),
			BLEND_Translucent);
	}

	static void DrawSolidTile(UCanvas* Canvas, float X, float Y,
		float Width, float Height, const FLinearColor& Color)
	{
		Canvas->SetLinearDrawColor(Color);
		Canvas->DrawTile(Canvas->DefaultTexture, X, Y, Width, Height,
			0.0f, 0.0f, 1.0f, 1.0f, BLEND_Translucent);
	}
}


AClutchHUD::AClutchHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The recovered Clutch reference uses the Wipeout portrait strip around the
	// scores. Its active/benched/eliminated treatment also maps cleanly to the
	// stock AUTPlayerState life flags used by Clutch.
	bShouldDrawPortraits = true;
	HudWidgetClasses.Remove(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ClutchScoreboard"));

	bTriedLoadRecoveredHUDTextures = false;
	RecoveredBaseTexture = nullptr;
	RecoveredKnobTexture = nullptr;
	RecoveredRocketTexture = nullptr;
	WeaponIconAtlas = nullptr;
}


void AClutchHUD::DrawHUD()
{
	// AWipeoutHUD supplies the established NetcodePlus widget stack and calls
	// our virtual DrawTeamScoreBar. Portraits are disabled in the constructor.
	Super::DrawHUD();

	const bool bRenderCustomHUD = bShowUTHUD && UTPlayerOwner
		&& (bShowHUD || !UTPlayerOwner->bCinematicMode);
	if (!bRenderCustomHUD || !Canvas || !SmallFont || ScoreboardIsUp())
	{
		return;
	}

	if (AClutchRoundState* State = ResolveClutchState())
	{
		DrawRolePanel(State);
		DrawCapturePanel(State);
	}
}


void AClutchHUD::DrawTeamScoreBar(AUTGameState* GameState)
{
	Super::DrawTeamScoreBar(GameState);
	AClutchRoundState* State = ResolveClutchState();
	if (!Canvas || !GameState || !State || !SmallFont || !MediumFont
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}

	const float RenderScale = static_cast<float>(Canvas->SizeX) / 1920.0f;
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 2.0f * RenderScale));
	const float CenterX = ScoreBarPos.X;
	const float BarHeight = 36.0f * RenderScale * ScoreScale;
	const float ClockY = ScoreBarPos.Y + BarHeight + 1.0f * RenderScale;
	const float FontScale = RenderScale * ScoreScale;
	const float Now = GameState->GetServerWorldTimeSeconds();

	if (State->IsGameplayPhase() && State->RoundEndServerTime > 0.0f)
	{
		const int32 Remaining = FMath::Max(0,
			FMath::CeilToInt(State->RoundEndServerTime - Now));
		const FString Clock = FString::Printf(TEXT("%02d:%02d"),
			Remaining / 60, Remaining % 60);
		DrawCenteredCanvasText(Canvas, MediumFont, Clock, CenterX, ClockY,
			0.78f * FontScale,
			Remaining <= 10 ? FColor(255, 70, 70, 255) : FColor::White);
	}
}


AClutchRoundState* AClutchHUD::ResolveClutchState()
{
	if (CachedClutchState.IsValid()
		&& CachedClutchState->GetWorld() == GetWorld())
	{
		return CachedClutchState.Get();
	}

	CachedClutchState.Reset();
	for (TActorIterator<AClutchRoundState> It(GetWorld()); It; ++It)
	{
		CachedClutchState = *It;
		return *It;
	}
	return nullptr;
}


AUTPlayerState* AClutchHUD::ResolveDisplayedPlayerState(AClutchRoundState* State) const
{
	AUTPlayerState* OwnState = UTPlayerOwner
		? Cast<AUTPlayerState>(UTPlayerOwner->PlayerState)
		: nullptr;
	if (OwnState && State && State->FindEntry(OwnState))
	{
		return OwnState;
	}

	APawn* ViewedPawn = UTPlayerOwner
		? Cast<APawn>(UTPlayerOwner->GetViewTarget())
		: nullptr;
	return ViewedPawn ? Cast<AUTPlayerState>(ViewedPawn->PlayerState) : OwnState;
}


void AClutchHUD::LoadRecoveredHUDTextures()
{
	if (bTriedLoadRecoveredHUDTextures)
	{
		return;
	}
	bTriedLoadRecoveredHUDTextures = true;

	// These are the recovered 2020 Clutch packages. The _2 object-name
	// fallbacks preserve the names embedded in the supplied duplicate files.
	RecoveredBaseTexture = LoadTextureWithFallback(
		TEXT("/Game/Clutch/HUD/Textures/base.base"),
		TEXT("/Game/Clutch/HUD/Textures/base.base_2"));
	if (!RecoveredBaseTexture)
	{
		RecoveredBaseTexture = LoadTextureWithFallback(
			TEXT("/NetcodePlus/Clutch/HUD/Textures/base.base"),
			TEXT("/NetcodePlus/Clutch/HUD/Textures/base.base_2"));
	}
	RecoveredKnobTexture = LoadTextureWithFallback(
		TEXT("/Game/Clutch/HUD/Textures/knob_copie.knob_copie"));
	if (!RecoveredKnobTexture)
	{
		RecoveredKnobTexture = LoadTextureWithFallback(
			TEXT("/NetcodePlus/Clutch/HUD/Textures/knob_copie.knob_copie"));
	}
	RecoveredRocketTexture = LoadTextureWithFallback(
		TEXT("/Game/Clutch/HUD/Textures/rocket.rocket"));
	if (!RecoveredRocketTexture)
	{
		RecoveredRocketTexture = LoadTextureWithFallback(
			TEXT("/NetcodePlus/Clutch/HUD/Textures/rocket.rocket"));
	}
	WeaponIconAtlas = LoadTextureWithFallback(
		TEXT("/Game/RestrictedAssets/UI/WeaponAtlas01.WeaponAtlas01"));
}


void AClutchHUD::DrawRolePanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !MediumFont)
	{
		return;
	}

	AUTPlayerState* DisplayedState = ResolveDisplayedPlayerState(State);
	const FClutchRosterEntry* Entry = DisplayedState
		? State->FindEntry(DisplayedState)
		: nullptr;

	FString Title = GetPhaseLabel(State->Phase);
	FString Subtitle;
	FLinearColor Accent(0.35f, 0.55f, 0.95f, 1.0f);
	if (Entry)
	{
		if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Attacker)
		{
			Title = TEXT("ATTACKING");
			Subtitle = TEXT("ELIMINATE DEFENDERS OR CAPTURE THE POLE");
			Accent = FLinearColor(1.0f, 0.55f, 0.08f, 1.0f);
		}
		else if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Defender)
		{
			Title = TEXT("DEFENDING");
			Subtitle = TEXT("STOP THE ATTACKER");
			Accent = FLinearColor(0.20f, 0.75f, 1.0f, 1.0f);
		}
		else if (Entry->PlayerStatus == EClutchStatus::Benched)
		{
			Title = TEXT("SPECTATING");
			Subtitle = TEXT("YOUR ATTACKER IS ACTIVE");
			Accent = FLinearColor(0.72f, 0.72f, 0.72f, 1.0f);
		}
		else if (Entry->PlayerStatus == EClutchStatus::Eliminated)
		{
			Title = TEXT("ELIMINATED");
			Subtitle = TEXT("SPECTATING TEAMMATES");
			Accent = FLinearColor(0.85f, 0.20f, 0.20f, 1.0f);
		}
		else
		{
			Title = TEXT("WAITING");
			Subtitle = TEXT("WAITING FOR BOTH TEAMS AND A POLE");
		}
	}
	else if (State->Phase == EClutchRoundPhase::Waiting)
	{
		Subtitle = TEXT("WAITING FOR BOTH TEAMS AND A POLE");
	}

	LoadRecoveredHUDTextures();

	const float RenderScale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const float ModuleCenterY = Canvas->ClipY - 70.0f * RenderScale;
	const float ModuleSize = 72.0f * RenderScale;
	const float RailY = ModuleCenterY - 2.0f * RenderScale;
	const float RailHeight = 5.0f * RenderScale;
	const float RailWidth = 150.0f * RenderScale;
	const float RailGap = ModuleSize * 0.43f;
	const FLinearColor Navy(0.02f, 0.07f, 0.12f, 0.78f);
	const FLinearColor HealthGreen(0.36f, 1.0f, 0.28f, 0.95f);

	// Thin mirrored rails and the recovered circular center module reproduce
	// the compact original HUD without covering the weapon or center view.
	DrawSolidTile(Canvas, CenterX - RailGap - RailWidth, RailY,
		RailWidth, RailHeight, Navy);
	DrawSolidTile(Canvas, CenterX + RailGap, RailY,
		RailWidth, RailHeight, Navy);

	float LeftRatio = 1.0f;
	if (Entry && Entry->PlayerRole == EClutchRole::Attacker)
	{
		LeftRatio = static_cast<float>(State->GetEntryArmorRemaining(*Entry))
			/ static_cast<float>(FMath::Max<int32>(1, State->MaxAttackerHits));
	}
	DrawSolidTile(Canvas, CenterX - RailGap - RailWidth, RailY,
		RailWidth * FMath::Clamp(LeftRatio, 0.0f, 1.0f), RailHeight, HealthGreen);
	DrawSolidTile(Canvas, CenterX + RailGap, RailY,
		RailWidth, RailHeight, Accent);

	const float ModuleX = CenterX - ModuleSize * 0.5f;
	const float ModuleY = ModuleCenterY - ModuleSize * 0.5f;
	if (RecoveredBaseTexture || RecoveredKnobTexture)
	{
		DrawTextureTile(Canvas, RecoveredBaseTexture, ModuleX, ModuleY,
			ModuleSize, ModuleSize, FLinearColor::White);
		const float KnobSize = ModuleSize * 0.84f;
		DrawTextureTile(Canvas, RecoveredKnobTexture,
			CenterX - KnobSize * 0.5f, ModuleCenterY - KnobSize * 0.5f,
			KnobSize, KnobSize, FLinearColor::White);
	}
	else
	{
		DrawSolidTile(Canvas, ModuleX, ModuleY, ModuleSize, ModuleSize, Navy);
		DrawSolidTile(Canvas, ModuleX + 3.0f * RenderScale,
			ModuleY + 3.0f * RenderScale,
			ModuleSize - 6.0f * RenderScale,
			ModuleSize - 6.0f * RenderScale,
			FLinearColor(0.75f, 0.82f, 0.9f, 0.9f));
		DrawSolidTile(Canvas, ModuleX + 8.0f * RenderScale,
			ModuleY + 8.0f * RenderScale,
			ModuleSize - 16.0f * RenderScale,
			ModuleSize - 16.0f * RenderScale, Navy);
	}

	AUTCharacter* Character = DisplayedState ? DisplayedState->GetUTCharacter() : nullptr;
	AUTWeapon* Weapon = Character ? Character->GetWeapon() : nullptr;
	if (Weapon && WeaponIconAtlas && Weapon->WeaponBarSelectedUVs.UL > 0.0f
		&& Weapon->WeaponBarSelectedUVs.VL > 0.0f)
	{
		const FTextureUVs& UVs = Weapon->WeaponBarSelectedUVs;
		const float IconWidth = 48.0f * RenderScale;
		const float IconHeight = IconWidth * UVs.VL / UVs.UL;
		Canvas->SetLinearDrawColor(FLinearColor::White);
		Canvas->DrawTile(WeaponIconAtlas,
			CenterX - IconWidth * 0.5f,
			ModuleCenterY - IconHeight * 0.5f,
			IconWidth, IconHeight, UVs.U, UVs.V, UVs.UL, UVs.VL,
			BLEND_Translucent);
	}
	else if (Entry && Entry->PlayerRole == EClutchRole::Defender)
	{
		DrawTextureTile(Canvas, RecoveredRocketTexture,
			CenterX - 26.0f * RenderScale,
			ModuleCenterY - 13.0f * RenderScale,
			52.0f * RenderScale, 25.0f * RenderScale,
			FLinearColor::White);
	}

	DrawCenteredCanvasText(Canvas, MediumFont, Title, CenterX,
		ModuleY - 38.0f * RenderScale, 0.82f * RenderScale,
		FColor::White);
	if (!Subtitle.IsEmpty())
	{
		DrawCenteredCanvasText(Canvas, SmallFont, Subtitle, CenterX,
			ModuleY - 15.0f * RenderScale, 0.48f * RenderScale,
			FColor(220, 225, 235, 220));
	}

	if (Entry && Entry->PlayerRole == EClutchRole::Attacker)
	{
		const int32 PipCount = FMath::Clamp<int32>(State->MaxAttackerHits, 1, 10);
		const int32 ArmorRemaining = State->GetEntryArmorRemaining(*Entry);
		const float PipWidth = 15.0f * RenderScale;
		const float PipHeight = 8.0f * RenderScale;
		const float PipGap = 4.0f * RenderScale;
		const float StartX = CenterX + RailGap + RailWidth + 8.0f * RenderScale;
		for (int32 Index = 0; Index < PipCount; ++Index)
		{
			DrawSolidTile(Canvas,
				StartX + Index * (PipWidth + PipGap),
				RailY - 2.0f * RenderScale,
				PipWidth, PipHeight,
				Index < ArmorRemaining
					? Accent
					: FLinearColor(0.18f, 0.18f, 0.18f, 0.75f));
		}
	}
}


void AClutchHUD::DrawCapturePanel(AClutchRoundState* State)
{
	if (!State || !Canvas || !SmallFont || !State->IsGameplayPhase())
	{
		return;
	}

	AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
	const float Now = GameState
		? GameState->GetServerWorldTimeSeconds()
		: GetWorld()->GetTimeSeconds();
	const float RenderScale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float BarWidth = 360.0f * RenderScale;
	const float BarHeight = 12.0f * RenderScale;
	const float BarX = (Canvas->ClipX - BarWidth) * 0.5f;
	const float BarY = Canvas->ClipY - 178.0f * RenderScale;
	const float Progress = FMath::Clamp(State->PoleProgress, 0.0f, 100.0f) / 100.0f;

	FString Label;
	FLinearColor Fill(1.0f, 0.55f, 0.08f, 1.0f);
	if (State->Phase == EClutchRoundPhase::Combat)
	{
		const int32 UnlockRemaining = FMath::Max(0,
			FMath::CeilToInt(State->PoleUnlockServerTime - Now));
		Label = FString::Printf(TEXT("POLE UNLOCKS IN %d"), UnlockRemaining);
		Fill = FLinearColor(0.35f, 0.38f, 0.43f, 1.0f);
	}
	else
	{
		Label = FString::Printf(TEXT("CAPTURE %d%%"),
			FMath::RoundToInt(State->PoleProgress));
	}

	Canvas->SetLinearDrawColor(FLinearColor(0.01f, 0.015f, 0.025f, 0.88f));
	Canvas->DrawTile(Canvas->DefaultTexture, BarX - 3.0f * RenderScale,
		BarY - 3.0f * RenderScale, BarWidth + 6.0f * RenderScale,
		BarHeight + 6.0f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FLinearColor(0.12f, 0.12f, 0.14f, 1.0f));
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth, BarHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(Fill);
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth * Progress, BarHeight, 0, 0, 1, 1);
	DrawCenteredCanvasText(Canvas, SmallFont, Label, Canvas->ClipX * 0.5f,
		BarY - 21.0f * RenderScale, 0.62f * RenderScale, FColor::White);
}
