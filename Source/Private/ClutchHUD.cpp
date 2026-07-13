#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "ClutchScoreboard.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTWeapon.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Interfaces/IImageWrapper.h"
#include "Interfaces/IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"


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

	static UTexture2D* LoadPluginPngTexture(const TCHAR* RelativePath)
	{
#if !UE_SERVER
		const FString GamePluginsDir = FPaths::GamePluginsDir();
		const FString FilePath = FPaths::Combine(
			*GamePluginsDir,
			TEXT("NetcodePlus/Resources/ClutchHUD"),
			RelativePath);
		TArray<uint8> CompressedData;
		if (!FFileHelper::LoadFileToArray(CompressedData, *FilePath)
			|| CompressedData.Num() == 0)
		{
			return nullptr;
		}

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		const TArray<uint8>* RawData = nullptr;
		if (!ImageWrapper.IsValid()
			|| !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num())
			|| !ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData)
			|| !RawData)
		{
			return nullptr;
		}

		const int32 Width = ImageWrapper->GetWidth();
		const int32 Height = ImageWrapper->GetHeight();
		if (Width <= 0 || Height <= 0 || RawData->Num() != Width * Height * 4)
		{
			return nullptr;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(
			Width, Height, PF_B8G8R8A8);
		if (!Texture || !Texture->PlatformData || Texture->PlatformData->Mips.Num() == 0)
		{
			return nullptr;
		}

		Texture->SRGB = true;
		Texture->NeverStream = true;
		FTexture2DMipMap& Mip = Texture->PlatformData->Mips[0];
		void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TextureData, RawData->GetData(), RawData->Num());
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
#else
		return nullptr;
#endif
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
	// Clutch draws a compact portrait strip immediately around the score boxes.
	// Disable Wipeout's wide 40%/60% anchors and its unrelated KDA panel.
	bShouldDrawPortraits = false;
	HudWidgetClasses.Remove(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ClutchScoreboard"));

	bTriedLoadRecoveredHUDTextures = false;
	LegacyCircleTexture = nullptr;
	LegacyInnerCircleTexture = nullptr;
	LegacyAttackingTexture = nullptr;
	LegacyLeftGadgetTexture = nullptr;
	LegacyRightGadgetTexture = nullptr;
	LegacyShieldTexture = nullptr;
	LegacyInstaTexture = nullptr;
	LegacyRocketTexture = nullptr;
	LegacyAttackRailBackTexture = nullptr;
	LegacyAttackRailFrontTexture = nullptr;
	LegacyAttackProgressTexture = nullptr;
	LegacyDefendRailBackTexture = nullptr;
	LegacyDefendRailFrontTexture = nullptr;
	LegacyDefendProgressTexture = nullptr;
	LegacyTopFrameTexture = nullptr;
}


void AClutchHUD::DrawHUD()
{
	// AWipeoutHUD supplies the established NetcodePlus widget stack and calls
	// our virtual DrawTeamScoreBar. Its wide portraits are disabled above.
	Super::DrawHUD();

	const bool bRenderCustomHUD = bShowUTHUD && UTPlayerOwner
		&& (bShowHUD || !UTPlayerOwner->bCinematicMode);
	if (!bRenderCustomHUD || !Canvas || !SmallFont || ScoreboardIsUp())
	{
		return;
	}

	if (AClutchRoundState* State = ResolveClutchState())
	{
		DrawClutchPortraits(State);
		DrawCapturePanel(State);
		DrawRolePanel(State);
	}
}


void AClutchHUD::DrawTeamScoreBar(AUTGameState* GameState)
{
	AClutchRoundState* State = ResolveClutchState();
	if (!Canvas || !GameState || !State || !SmallFont || !MediumFont
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}

	LoadRecoveredHUDTextures();

	// Do not delegate this to AWipeoutHUD. ClutchMini used a transparent
	// 2560x1440 overlay whose angular red/blue frame is now recovered verbatim.
	const float RenderScale = FMath::Min(
		static_cast<float>(Canvas->SizeX) / 1920.0f,
		static_cast<float>(Canvas->SizeY) / 1080.0f);
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float HUDScale = GetHUDWidgetScaleOverride();
	const float FrameScale = ScoreScale * HUDScale;
	const float UnitScale = RenderScale * FrameScale;
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 2.0f * RenderScale));
	const float CenterX = ScoreBarPos.X;
	const float TopY = ScoreBarPos.Y;
	const float ScoreOpacity = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	float ScoreCenters[2] = { 0.0f, 0.0f };
	float ScoreTextY = TopY;
	float ScoreFontScale = 1.02f * UnitScale;
	float ClockY = TopY;
	float ClockFontScale = 0.82f * UnitScale;

	if (LegacyTopFrameTexture)
	{
		const float FrameWidth = Canvas->ClipX * FrameScale;
		const float FrameHeight = Canvas->ClipY * FrameScale;
		const float FrameX = CenterX - FrameWidth * 0.5f;
		const float FrameY = TopY - 2.0f * RenderScale;
		const float FrameScaleX = FrameWidth / 2560.0f;
		const float FrameScaleY = FrameHeight / 1440.0f;

		DrawTextureTile(Canvas, LegacyTopFrameTexture,
			FrameX, FrameY, FrameWidth, FrameHeight,
			FLinearColor(1.0f, 1.0f, 1.0f, ScoreOpacity));

		// Coordinates are taken from the recovered ClutchMini texture. Keeping
		// them in its source space makes the art, scores and portraits scale as
		// one composition at every viewport resolution.
		ScoreCenters[0] = FrameX + 1130.0f * FrameScaleX;
		ScoreCenters[1] = FrameX + 1430.0f * FrameScaleX;
		ScoreTextY = FrameY + 15.0f * FrameScaleY;
		ScoreFontScale = 1.08f * UnitScale;
		ClockY = FrameY + 82.0f * FrameScaleY;
		ClockFontScale = 0.78f * UnitScale;
	}
	else
	{
		// Safe fallback for installs that copy only the binary and omit Resources.
		const float ScoreWidth = 44.0f * UnitScale;
		const float ScoreHeight = 38.0f * UnitScale;
		const float CenterGap = 4.0f * UnitScale;
		const float Score0X = CenterX - CenterGap * 0.5f - ScoreWidth;
		const float Score1X = CenterX + CenterGap * 0.5f;
		const float PipSize = 42.0f * UnitScale;
		const float PipStep = PipSize + 3.0f * UnitScale;
		const float PortraitSpan = PipSize + 2.0f * PipStep;
		const FLinearColor Navy(0.015f, 0.055f, 0.095f, 0.94f * ScoreOpacity);
		FLinearColor TeamColors[2] = {
			FLinearColor(0.92f, 0.045f, 0.02f, ScoreOpacity),
			FLinearColor(0.12f, 0.17f, 1.0f, ScoreOpacity)
		};

		if (NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar")))
		{
			for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
			{
				if (GameState->Teams.IsValidIndex(TeamIndex) && GameState->Teams[TeamIndex])
				{
					TeamColors[TeamIndex] = GameState->Teams[TeamIndex]->TeamColor;
					TeamColors[TeamIndex].A = ScoreOpacity;
				}
			}
		}

		DrawSolidTile(Canvas, Score0X - 4.0f * UnitScale, TopY,
			ScoreWidth * 2.0f + CenterGap + 8.0f * UnitScale,
			ScoreHeight + 5.0f * UnitScale, Navy);
		DrawSolidTile(Canvas, Score0X - PortraitSpan,
			TopY + ScoreHeight - 4.0f * UnitScale,
			PortraitSpan, 4.0f * UnitScale, TeamColors[0]);
		DrawSolidTile(Canvas, Score1X + ScoreWidth,
			TopY + ScoreHeight - 4.0f * UnitScale,
			PortraitSpan, 4.0f * UnitScale, TeamColors[1]);

		FLinearColor ScoreColors[2] = { TeamColors[0], TeamColors[1] };
		for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
		{
			ScoreColors[TeamIndex].R *= 0.78f;
			ScoreColors[TeamIndex].G *= 0.78f;
			ScoreColors[TeamIndex].B *= 0.78f;
		}
		DrawSolidTile(Canvas, Score0X, TopY, ScoreWidth, ScoreHeight, ScoreColors[0]);
		DrawSolidTile(Canvas, Score1X, TopY, ScoreWidth, ScoreHeight, ScoreColors[1]);
		DrawSolidTile(Canvas, CenterX - 1.0f * UnitScale, TopY,
			2.0f * UnitScale, ScoreHeight + 5.0f * UnitScale,
			FLinearColor(0.85f, 0.93f, 1.0f, ScoreOpacity));

		ScoreCenters[0] = Score0X + ScoreWidth * 0.5f;
		ScoreCenters[1] = Score1X + ScoreWidth * 0.5f;
		ScoreTextY = TopY + 2.0f * UnitScale;
		ClockY = TopY + ScoreHeight + 5.0f * UnitScale;
	}

	const int32 Scores[2] = {
		GameState->Teams.IsValidIndex(0) && GameState->Teams[0]
			? static_cast<int32>(GameState->Teams[0]->Score) : 0,
		GameState->Teams.IsValidIndex(1) && GameState->Teams[1]
			? static_cast<int32>(GameState->Teams[1]->Score) : 0
	};
	const uint8 TextAlpha = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(ScoreOpacity * 255.0f), 0, 255));
	const FColor HeaderText(255, 255, 255, TextAlpha);
	DrawCenteredCanvasText(Canvas, MediumFont, FString::FromInt(Scores[0]),
		ScoreCenters[0], ScoreTextY,
		ScoreFontScale, HeaderText);
	DrawCenteredCanvasText(Canvas, MediumFont, FString::FromInt(Scores[1]),
		ScoreCenters[1], ScoreTextY,
		ScoreFontScale, HeaderText);

	const float Now = GameState->GetServerWorldTimeSeconds();
	const int32 Remaining = State->RoundEndServerTime > 0.0f
		? FMath::Max(0, FMath::CeilToInt(State->RoundEndServerTime - Now))
		: 0;
	const FString Clock = FString::Printf(TEXT("%02d:%02d"),
		Remaining / 60, Remaining % 60);
	DrawCenteredCanvasText(Canvas, SmallFont, Clock, CenterX, ClockY,
		ClockFontScale,
		Remaining <= 10 && State->IsGameplayPhase()
			? FColor(255, 80, 80, TextAlpha)
			: HeaderText);
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

	// Editor-safe imports of the layered HUD artwork recovered from the 2020
	// Clutch package. These are the circular assets from the reference captures,
	// not the unrelated square Center_PB prototype.
	LegacyCircleTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_circle.clutch_circle"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_circle.clutch_circle"));
	LegacyInnerCircleTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_inner_circle.clutch_inner_circle"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_inner_circle.clutch_inner_circle"));
	LegacyAttackingTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attacking.clutch_attacking"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attacking.clutch_attacking"));
	LegacyLeftGadgetTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_left_gadget.clutch_left_gadget"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_left_gadget.clutch_left_gadget"));
	LegacyRightGadgetTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_right_gadget.clutch_right_gadget"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_right_gadget.clutch_right_gadget"));
	LegacyShieldTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_shield.clutch_shield"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_shield.clutch_shield"));
	LegacyInstaTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_insta.clutch_insta"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_insta.clutch_insta"));
	LegacyRocketTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_rocket.clutch_rocket"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_rocket.clutch_rocket"));
	LegacyAttackRailBackTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_rail_back.clutch_attack_rail_back"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_rail_back.clutch_attack_rail_back"));
	LegacyAttackRailFrontTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_rail_front.clutch_attack_rail_front"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_rail_front.clutch_attack_rail_front"));
	LegacyAttackProgressTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_attack_progress.clutch_attack_progress"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_attack_progress.clutch_attack_progress"));
	LegacyDefendRailBackTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_rail_back.clutch_defend_rail_back"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_rail_back.clutch_defend_rail_back"));
	LegacyDefendRailFrontTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_rail_front.clutch_defend_rail_front"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_rail_front.clutch_defend_rail_front"));
	LegacyDefendProgressTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/clutch_defend_progress.clutch_defend_progress"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/clutch_defend_progress.clutch_defend_progress"));

	// Prefer a normal editor asset when one has been imported. The checked-in
	// PNG fallback keeps the exact recovered ClutchMini frame available in both
	// source and binary plugin installs without relying on its cooked uasset.
	LegacyTopFrameTexture = LoadTextureWithFallback(
		TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/hud_top_bottom2.hud_top_bottom2"),
		TEXT("/Game/Clutch/HUD/Textures/Legacy/hud_top_bottom2.hud_top_bottom2"));
	if (!LegacyTopFrameTexture)
	{
		LegacyTopFrameTexture = LoadPluginPngTexture(TEXT("hud_top_bottom2.png"));
	}
}


void AClutchHUD::DrawClutchPortraits(AClutchRoundState* State)
{
	if (!State || !Canvas || ScoreboardIsUp()
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}
	LoadRecoveredHUDTextures();

	const float RenderScale = FMath::Min(
		static_cast<float>(Canvas->SizeX) / 1920.0f,
		static_cast<float>(Canvas->SizeY) / 1080.0f);
	const float FrameScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"))
		* GetHUDWidgetScaleOverride();
	const float UnitScale = RenderScale * FrameScale;
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 2.0f * RenderScale));
	const float CenterX = ScoreBarPos.X;
	float PipSize = 42.0f * UnitScale;
	float PipStep = PipSize + 3.0f * UnitScale;
	float PortraitY = ScoreBarPos.Y;
	float FirstPortraitX[2] = {
		CenterX - 46.0f * UnitScale - PipSize,
		CenterX + 46.0f * UnitScale
	};
	float PortraitDirection[2] = { -1.0f, 1.0f };

	if (LegacyTopFrameTexture)
	{
		const float FrameWidth = Canvas->ClipX * FrameScale;
		const float FrameHeight = Canvas->ClipY * FrameScale;
		const float FrameX = CenterX - FrameWidth * 0.5f;
		const float FrameY = ScoreBarPos.Y - 2.0f * RenderScale;
		const float FrameScaleX = FrameWidth / 2560.0f;
		const float FrameScaleY = FrameHeight / 1440.0f;

		// ClutchMini's portrait boxes were laid directly over the recovered
		// frame. These source-space anchors place the inside portrait first,
		// then grow each team away from its score cell.
		PipSize = 76.0f * FrameScaleX;
		PipStep = 79.0f * FrameScaleX;
		PortraitY = FrameY + 4.0f * FrameScaleY;
		FirstPortraitX[0] = FrameX + 980.0f * FrameScaleX;
		FirstPortraitX[1] = FrameX + 1504.0f * FrameScaleX;
	}
	int32 DrawnPerTeam[2] = { 0, 0 };

	// Roster slots are stable across rounds, so the same portrait stays in the
	// same position while active/benched roles rotate.
	for (int32 Slot = 0; Slot < 6; ++Slot)
	{
		for (const FClutchRosterEntry& Entry : State->Roster)
		{
			if (!Entry.PlayerState || Entry.TeamIndex > 1
				|| Entry.RosterSlot != Slot || DrawnPerTeam[Entry.TeamIndex] >= 3)
			{
				continue;
			}

			const int32 TeamIndex = Entry.TeamIndex;
			const int32 DrawIndex = DrawnPerTeam[TeamIndex]++;
			const float PortraitX = FirstPortraitX[TeamIndex]
				+ PortraitDirection[TeamIndex] * DrawIndex * PipStep;
			const float LiveScaling = Entry.PlayerStatus == EClutchStatus::Active
				? 1.0f
				: 0.0f;
			DrawPlayerIcon(Entry.PlayerState, LiveScaling,
				PortraitX, PortraitY, PipSize);
		}
	}
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
	FLinearColor Accent(0.35f, 0.55f, 0.95f, 1.0f);
	bool bActiveAttacker = false;
	bool bActiveDefender = false;
	if (Entry)
	{
		// The original HUD's right rail and weapon glow follow the player's
		// team, while green is reserved for health/armor on the left rail.
		Accent = Entry->TeamIndex == 0
			? FLinearColor(1.0f, 0.12f, 0.05f, 1.0f)
			: FLinearColor(0.27f, 0.29f, 1.0f, 1.0f);

		if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Attacker)
		{
			Title = TEXT("ATTACKING");
			bActiveAttacker = true;
		}
		else if (Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->PlayerRole == EClutchRole::Defender)
		{
			Title = TEXT("DEFENDING");
			bActiveDefender = true;
		}
		else if (Entry->PlayerStatus == EClutchStatus::Benched)
		{
			Title = TEXT("SPECTATING");
			Accent = FLinearColor(0.72f, 0.72f, 0.72f, 1.0f);
		}
		else if (Entry->PlayerStatus == EClutchStatus::Eliminated)
		{
			Title = TEXT("ELIMINATED");
			Accent = FLinearColor(0.85f, 0.20f, 0.20f, 1.0f);
		}
		else
		{
			Title = TEXT("WAITING");
		}
	}

	LoadRecoveredHUDTextures();

	const float RenderScale = static_cast<float>(Canvas->SizeY) / 1080.0f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const float ModuleCenterY = Canvas->ClipY - 50.0f * RenderScale;
	const float CircleWidth = 120.0f * RenderScale;
	const float CircleHeight = 113.0f * RenderScale;
	const float RailY = ModuleCenterY - 2.5f * RenderScale;
	const float RailHeight = 5.0f * RenderScale;
	const float RailWidth = 168.0f * RenderScale;
	const float RailGap = 48.0f * RenderScale;
	const FLinearColor Navy(0.02f, 0.07f, 0.12f, 0.78f);
	const FLinearColor HealthGreen(0.36f, 1.0f, 0.28f, 0.95f);

	float LeftRatio = 1.0f;
	if (bActiveAttacker)
	{
		LeftRatio = static_cast<float>(State->GetEntryArmorRemaining(*Entry))
			/ static_cast<float>(FMath::Max<int32>(1, State->MaxAttackerHits));
	}
	LeftRatio = FMath::Clamp(LeftRatio, 0.0f, 1.0f);

	// Draw the original one-pixel rail art as scalable crisp lines. The back
	// layer establishes the shape, while the progress layer carries gameplay
	// color and the same left/right proportions as the recovered PSD.
	const float LeftRailX = CenterX - RailGap - RailWidth;
	const float RightRailX = CenterX + RailGap;
	UTexture2D* RailBack = bActiveAttacker
		? LegacyAttackRailBackTexture
		: LegacyDefendRailBackTexture;
	UTexture2D* RailFront = bActiveAttacker
		? LegacyAttackRailFrontTexture
		: LegacyDefendRailFrontTexture;
	UTexture2D* RailProgress = bActiveAttacker
		? LegacyAttackProgressTexture
		: LegacyDefendProgressTexture;
	if (RailBack || RailFront)
	{
		DrawTextureTile(Canvas, RailBack, LeftRailX, RailY,
			RailWidth, RailHeight, FLinearColor::White);
		DrawTextureTile(Canvas, RailBack, RightRailX, RailY,
			RailWidth, RailHeight, FLinearColor::White);
		DrawTextureTile(Canvas, RailFront, LeftRailX, RailY,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawTextureTile(Canvas, RailFront, RightRailX, RailY,
			RailWidth, RailHeight, Accent);
		DrawTextureTile(Canvas, RailProgress, LeftRailX, RailY - 2.0f * RenderScale,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawTextureTile(Canvas, RailProgress, RightRailX,
			RailY - 2.0f * RenderScale, RailWidth, RailHeight, Accent);
	}
	else
	{
		DrawSolidTile(Canvas, LeftRailX, RailY, RailWidth, RailHeight, Navy);
		DrawSolidTile(Canvas, LeftRailX, RailY,
			RailWidth * LeftRatio, RailHeight, HealthGreen);
		DrawSolidTile(Canvas, RightRailX, RailY, RailWidth, RailHeight, Accent);
	}

	const float ShieldWidth = 24.0f * RenderScale;
	const float ShieldHeight = 27.0f * RenderScale;
	DrawTextureTile(Canvas, LegacyShieldTexture,
		LeftRailX - 18.0f * RenderScale,
		ModuleCenterY - ShieldHeight * 0.5f,
		ShieldWidth, ShieldHeight, HealthGreen);

	// Gadget wings sit behind the circle in the original composition.
	DrawTextureTile(Canvas, LegacyLeftGadgetTexture,
		CenterX - 88.0f * RenderScale,
		ModuleCenterY - 16.5f * RenderScale,
		94.0f * RenderScale, 33.0f * RenderScale, FLinearColor::White);
	DrawTextureTile(Canvas, LegacyRightGadgetTexture,
		CenterX - 6.0f * RenderScale,
		ModuleCenterY - 16.5f * RenderScale,
		94.0f * RenderScale, 33.0f * RenderScale, FLinearColor::White);

	const float CircleX = CenterX - CircleWidth * 0.5f;
	const float CircleY = ModuleCenterY - CircleHeight * 0.5f;
	if (LegacyCircleTexture)
	{
		DrawTextureTile(Canvas, LegacyCircleTexture, CircleX, CircleY,
			CircleWidth, CircleHeight, FLinearColor::White);
		DrawTextureTile(Canvas, LegacyInnerCircleTexture,
			CenterX - 11.0f * RenderScale,
			ModuleCenterY - 11.0f * RenderScale,
			22.0f * RenderScale, 22.0f * RenderScale, FLinearColor::White);
	}
	else
	{
		DrawSolidTile(Canvas, CircleX, CircleY, CircleWidth, CircleHeight, Navy);
	}

	UTexture2D* RoleWeaponTexture = bActiveDefender
		? LegacyRocketTexture
		: LegacyInstaTexture;
	if (RoleWeaponTexture)
	{
		const float IconWidth = static_cast<float>(RoleWeaponTexture->GetSizeX())
			* RenderScale;
		const float IconHeight = static_cast<float>(RoleWeaponTexture->GetSizeY())
			* RenderScale;
		DrawTextureTile(Canvas, RoleWeaponTexture,
			CenterX - IconWidth * 0.5f,
			ModuleCenterY - IconHeight * 0.5f,
			IconWidth, IconHeight, FLinearColor::White);
	}

	const float TitleWidth = 226.0f * RenderScale;
	const float TitleHeight = 50.0f * RenderScale;
	const float TitleY = ModuleCenterY - 91.0f * RenderScale;
	if (bActiveAttacker && LegacyAttackingTexture)
	{
		DrawTextureTile(Canvas, LegacyAttackingTexture,
			CenterX - TitleWidth * 0.5f, TitleY,
			TitleWidth, TitleHeight, FLinearColor::White);
	}
	else
	{
		// The recovered attacker title is image art. ClutchMini's defender
		// title was plain white lettering; it did not use Wipeout's navy box.
		DrawCenteredCanvasText(Canvas, MediumFont, Title, CenterX,
			TitleY + 11.0f * RenderScale, 0.72f * RenderScale,
			FColor::White);
	}

	if (bActiveAttacker)
	{
		const int32 PipCount = FMath::Clamp<int32>(State->MaxAttackerHits, 1, 10);
		const int32 ArmorRemaining = State->GetEntryArmorRemaining(*Entry);
		const float PipWidth = 13.0f * RenderScale;
		const float PipHeight = 7.0f * RenderScale;
		const float PipGap = 3.0f * RenderScale;
		const float StartX = CenterX + 52.0f * RenderScale;
		for (int32 Index = 0; Index < PipCount; ++Index)
		{
			DrawSolidTile(Canvas,
				StartX + Index * (PipWidth + PipGap),
				RailY - 3.0f * RenderScale,
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
	const float BarWidth = 300.0f * RenderScale;
	const float BarHeight = 6.0f * RenderScale;
	const float BarX = (Canvas->ClipX - BarWidth) * 0.5f;
	// Keep objective state above the recovered 50px role banner.
	const float BarY = Canvas->ClipY - 190.0f * RenderScale;
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
	Canvas->DrawTile(Canvas->DefaultTexture, BarX - 2.0f * RenderScale,
		BarY - 2.0f * RenderScale, BarWidth + 4.0f * RenderScale,
		BarHeight + 4.0f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FLinearColor(0.12f, 0.12f, 0.14f, 1.0f));
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth, BarHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(Fill);
	Canvas->DrawTile(Canvas->DefaultTexture, BarX, BarY,
		BarWidth * Progress, BarHeight, 0, 0, 1, 1);
	DrawCenteredCanvasText(Canvas, SmallFont, Label, Canvas->ClipX * 0.5f,
		BarY - 17.0f * RenderScale, 0.52f * RenderScale, FColor::White);
}
