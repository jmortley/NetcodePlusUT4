// ElimPlusScoreboard.cpp — country-flag scoreboard for ElimPlus, with an
// optional recovered Elimination 1.13 visual skin.
// Reads stats from AElimPlusStatsReplicator (Damage, PPRCurrent, Elo, LGAcc).
// Falls back to PlayerState->DamageDone when the replicator isn't available
// (listen-server / standalone). 7 columns: Name | K | D | DMG | PPR | ELO | LG_Acc | Ping.

#include "ElimPlusScoreboard.h"
#include "NCPlusScoreboardHost.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "UTBot.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/Texture2D.h"
#include "ElimPlusStatsReplicator.h"
#include "EngineUtils.h"
#if !UE_SERVER
#include "Interfaces/IImageWrapper.h"
#include "Interfaces/IImageWrapperModule.h"
#endif
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
	enum class EAbsoluteElimRowStyle : uint8
	{
		Normal = 0,
		Dead = 1,
		Totals = 2
	};

	struct FAbsoluteElimScoreboardTextures
	{
		bool bTriedLoad = false;
		UTexture2D* Banner[2] = { nullptr, nullptr };
		UTexture2D* Row[2][3] =
		{
			{ nullptr, nullptr, nullptr },
			{ nullptr, nullptr, nullptr }
		};
		UTexture2D* Categories = nullptr;
	};

	FAbsoluteElimScoreboardTextures GAbsoluteElimScoreboardTextures;

	UTexture2D* LoadAbsoluteElimScoreboardTexture(const TCHAR* RelativePath,
		float Saturation = 1.f, float Multiply = 1.f)
	{
#if !UE_SERVER
		const FString FilePath = FPaths::Combine(
			*FNCPlusHUDLayout::PluginResourcesDir(),
			TEXT("AbsoluteElimHUD"),
			RelativePath);
		TArray<uint8> CompressedData;
		if (!FFileHelper::LoadFileToArray(CompressedData, *FilePath)
			|| CompressedData.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[AbsoluteElimScoreboard] Missing resource: %s"), *FilePath);
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
			UE_LOG(LogTemp, Warning, TEXT("[AbsoluteElimScoreboard] Could not decode: %s"), *FilePath);
			return nullptr;
		}

		const int32 Width = ImageWrapper->GetWidth();
		const int32 Height = ImageWrapper->GetHeight();
		if (Width <= 0 || Height <= 0 || RawData->Num() != Width * Height * 4)
		{
			return nullptr;
		}

		TArray<uint8> Pixels = *RawData;
		if (!FMath::IsNearlyEqual(Saturation, 1.f) || !FMath::IsNearlyEqual(Multiply, 1.f))
		{
			for (int32 Pixel = 0; Pixel < Pixels.Num(); Pixel += 4)
			{
				const float B = float(Pixels[Pixel]);
				const float G = float(Pixels[Pixel + 1]);
				const float R = float(Pixels[Pixel + 2]);
				const float Luma = 0.0722f * B + 0.7152f * G + 0.2126f * R;
				Pixels[Pixel] = uint8(FMath::Clamp(FMath::RoundToInt((Luma + (B - Luma) * Saturation) * Multiply), 0, 255));
				Pixels[Pixel + 1] = uint8(FMath::Clamp(FMath::RoundToInt((Luma + (G - Luma) * Saturation) * Multiply), 0, 255));
				Pixels[Pixel + 2] = uint8(FMath::Clamp(FMath::RoundToInt((Luma + (R - Luma) * Saturation) * Multiply), 0, 255));
			}
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!Texture || !Texture->PlatformData || Texture->PlatformData->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->SRGB = true;
		Texture->NeverStream = true;
		FTexture2DMipMap& Mip = Texture->PlatformData->Mips[0];
		void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TextureData, Pixels.GetData(), Pixels.Num());
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		Texture->AddToRoot();
		return Texture;
#else
		return nullptr;
#endif
	}

	bool EnsureAbsoluteElimScoreboardTextures()
	{
		FAbsoluteElimScoreboardTextures& T = GAbsoluteElimScoreboardTextures;
		if (!T.bTriedLoad)
		{
			T.bTriedLoad = true;
			T.Banner[0] = LoadAbsoluteElimScoreboardTexture(TEXT("LeftTeamBannerRed.png"));
			T.Banner[1] = LoadAbsoluteElimScoreboardTexture(TEXT("RightTeamBannerBlue.png"));
			T.Row[0][int32(EAbsoluteElimRowStyle::Normal)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowLeftSideRed.png"));
			T.Row[1][int32(EAbsoluteElimRowStyle::Normal)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowRightSideBlue.png"));
			T.Row[0][int32(EAbsoluteElimRowStyle::Dead)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowLeftSideRed.png"), 0.8f, 0.5f);
			T.Row[1][int32(EAbsoluteElimRowStyle::Dead)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowRightSideBlue.png"), 0.8f, 0.5f);
			T.Row[0][int32(EAbsoluteElimRowStyle::Totals)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowLeftSideRed.png"), 0.12f, 0.65f);
			T.Row[1][int32(EAbsoluteElimRowStyle::Totals)] = LoadAbsoluteElimScoreboardTexture(TEXT("RowRightSideBlue.png"), 0.12f, 0.65f);
			T.Categories = LoadAbsoluteElimScoreboardTexture(TEXT("CategoriesRight.png"));
		}

		return T.Banner[0] && T.Banner[1] && T.Categories
			&& T.Row[0][0] && T.Row[0][1] && T.Row[0][2]
			&& T.Row[1][0] && T.Row[1][1] && T.Row[1][2];
	}

	void DrawAbsoluteElimScoreboardTile(UCanvas* Canvas, UTexture2D* Texture,
		float X, float Y, float Width, float Height, float Opacity = 1.f,
		bool bFlipHorizontal = false)
	{
		if (!Canvas || !Texture) return;
		Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, Opacity));
		const float TextureWidth = float(Texture->GetSizeX());
		Canvas->DrawTile(Texture, X, Y, Width, Height,
			bFlipHorizontal ? TextureWidth : 0.f, 0.f,
			bFlipHorizontal ? -TextureWidth : TextureWidth, float(Texture->GetSizeY()),
			BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor::White);
	}
}

UElimPlusScoreboard::UElimPlusScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CellHeight = 80.f;
	CellWidth = 850.f;

	// 6 stat columns after Name (K/D/DMG/PPR/ELO/LG_Acc) + Ping. Spaced for
	// readability at 1080p — wider gaps than initial 9-column layout. Available
	// width = 0.83 (between 0.10 name and 0.93 ping); 0.83/7 ≈ 0.12 per slot.
	ColumnHeaderPlayerX  = 0.10f;
	ColumnHeaderScoreX   = 0.30f; // unused but required by base class
	ColumnHeaderKillsX   = 0.35f;
	ColumnHeaderDeathsX  = 0.43f;
	ColumnHeaderDamageX  = 0.53f;
	ColumnHeaderPPRCurX  = 0.63f;
	ColumnHeaderEloX     = 0.74f; // wider neighbor gaps for "1400 +12" delta text
	ColumnHeaderLGAccX   = 0.85f;
	ColumnHeaderPingX    = 0.94f;

	CH_Kills  = NSLOCTEXT("ElimPlusScoreboard", "Kills",  "K");
	CH_Deaths = NSLOCTEXT("ElimPlusScoreboard", "Deaths", "D");
	CH_Damage = NSLOCTEXT("ElimPlusScoreboard", "Damage", "DMG");
	CH_PPRCur = NSLOCTEXT("ElimPlusScoreboard", "PPRCur", "PPR");
	CH_Elo    = NSLOCTEXT("ElimPlusScoreboard", "Elo",    "ELO");
	CH_LGAcc  = NSLOCTEXT("ElimPlusScoreboard", "LGAcc",  "LG_Acc");

	bUseRoundKills = false; // overall match stats

}

bool UElimPlusScoreboard::HasCustomTeamColors() const
{
	if (!UTGameState) return false;

	for (int32 i = 0; i < 2; i++)
	{
		if (!UTGameState->Teams.IsValidIndex(i) || !UTGameState->Teams[i]) continue;
		FLinearColor TC = UTGameState->Teams[i]->TeamColor;
		if (i == 0)
		{
			if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f)
				return true;
		}
		else
		{
			if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f)
				return true;
		}
	}
	return false;
}

bool UElimPlusScoreboard::ShouldDrawAbsoluteElimScoreboard() const
{
	return FNCPlusHUDLayout::WantsStockTeamPanel()
		&& FNCPlusHUDLayout::WantsAbsoluteElimTeamPanel()
		&& EnsureAbsoluteElimScoreboardTextures();
}

void UElimPlusScoreboard::DrawTeamPanel(float RenderDelta, float& YOffset)
{
	if (ShouldDrawAbsoluteElimScoreboard())
	{
		DrawAbsoluteTeamPanel(RenderDelta, YOffset);
		return;
	}
	if (!UTGameState || UTGameState->Teams.Num() < 2 || !UTGameState->Teams[0] || !UTGameState->Teams[1]) return;

	// Faction names only when custom team colors are in use AND the scorebar's
	// Team-Color toggle is on — untick it and the scoreboard reads plain RED/BLUE
	// (unifies with the top bar; kills the "am I red, blue, or phayder?" confusion).
	const bool bCustom = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
	RedTeamText  = bCustom ? FText::FromString(TEXT("PHAYDER (R)")) : FText::FromString(TEXT("RED"));
	BlueTeamText = bCustom ? FText::FromString(TEXT("LIANDRI (B)")) : FText::FromString(TEXT("BLUE"));

	const float Width = 0.5f * (Size.X - 400.f) * RenderScale;
	const float FrontSize = 35.f * RenderScale;
	const float EndSize = 16.f * RenderScale;
	const float MiddleSize = Width - FrontSize - EndSize;
	const float BackgroundY = YOffset + 22.f * RenderScale;
	const float TeamTextY = YOffset + 40.f * RenderScale;
	const float TeamScoreY = YOffset + 36.f * RenderScale;
	const float BackgroundHeight = 65.f * RenderScale;
	const float TeamEdgeSize = 40.f * RenderScale;
	const float NamePosition = TeamEdgeSize + FrontSize + 0.25f * MiddleSize;

	// Background color follows the same toggle as the names: custom team colors when
	// faction mode is on, stock red/blue when the user wants plain RED vs BLUE — so a
	// "RED" label never sits on a magenta bar.
	const FLinearColor Team0Color = bCustom ? UTGameState->Teams[0]->TeamColor : FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	const FLinearColor Team1Color = bCustom ? UTGameState->Teams[1]->TeamColor : FLinearColor(0.05f, 0.1f, 0.9f, 1.f);

	// Team 0 (left)
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize, BackgroundY, FrontSize, BackgroundHeight, 0, 188, 36, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize, BackgroundY, MiddleSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize + MiddleSize, BackgroundY, EndSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);

	// Concept-D gloss: translucent light band over the top of the team bar + a
	// darker band along the bottom (cheap "glossy bevel", no new texture).
	{
		const float bX = TeamEdgeSize, bY = BackgroundY, bW = FrontSize + MiddleSize + EndSize, bH = BackgroundHeight;
		Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, 0.16f));
		Canvas->DrawTile(Canvas->DefaultTexture, bX + 4.f * RenderScale, bY + 2.f * RenderScale, bW - 8.f * RenderScale, bH * 0.42f, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.22f));
		Canvas->DrawTile(Canvas->DefaultTexture, bX + 4.f * RenderScale, bY + bH * 0.72f, bW - 8.f * RenderScale, bH * 0.28f, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor::White);
	}

	DrawText(RedTeamText, NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[0]->Score), TeamEdgeSize + FrontSize + MiddleSize - EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0, 0), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * RedScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Right, ETextVertPos::Center);

	// Team 1 (right)
	const float LeftEdge = Canvas->ClipX - TeamEdgeSize - FrontSize - MiddleSize - EndSize;

	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize + MiddleSize, BackgroundY, FrontSize, BackgroundHeight, 196, 188, 36, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize, BackgroundY, MiddleSize, BackgroundHeight, 130, 188, 64, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge, BackgroundY, EndSize, BackgroundHeight, 117, 188, 16, 65, 1.f, Team1Color);

	// Concept-D gloss (right team bar).
	{
		const float bX = LeftEdge, bY = BackgroundY, bW = EndSize + MiddleSize + FrontSize, bH = BackgroundHeight;
		Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, 0.16f));
		Canvas->DrawTile(Canvas->DefaultTexture, bX + 4.f * RenderScale, bY + 2.f * RenderScale, bW - 8.f * RenderScale, bH * 0.42f, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.22f));
		Canvas->DrawTile(Canvas->DefaultTexture, bX + 4.f * RenderScale, bY + bH * 0.72f, bW - 8.f * RenderScale, bH * 0.28f, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor::White);
	}

	DrawText(BlueTeamText, Canvas->ClipX - NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Right, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[1]->Score), LeftEdge + 2.f * EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0.f, 0.f), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * BlueScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Left, ETextVertPos::Center);

	YOffset += 119.f * RenderScale;
	BlueScoreScaling = FMath::Max(BlueScoreScaling - RenderDelta, 1.f);
	RedScoreScaling = FMath::Max(RedScoreScaling - RenderDelta, 1.f);
}

void UElimPlusScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	if (ShouldDrawAbsoluteElimScoreboard())
	{
		DrawAbsoluteScoreHeaders(RenderDelta, YOffset);
		return;
	}
	float XOffset = ScaledEdgeSize;
	const float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		// Header background
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height,
			149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));

		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_Kills,  XOffset + (ScaledCellWidth * ColumnHeaderKillsX),  YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Deaths, XOffset + (ScaledCellWidth * ColumnHeaderDeathsX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_PPRCur, XOffset + (ScaledCellWidth * ColumnHeaderPPRCurX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Elo,    XOffset + (ScaledCellWidth * ColumnHeaderEloX),    YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_LGAcc,  XOffset + (ScaledCellWidth * ColumnHeaderLGAccX),  YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
		}
		DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4.f;
}

void UElimPlusScoreboard::DrawPlayerFlag(AUTPlayerState* PlayerState, float XOffset, float YOffset,
	float FlagWidth, float FlagHeight, float Opacity)
{
	if (!UTHUDOwner || !Canvas || !PlayerState) return;

	FTextureUVs FlagUV;
	UTexture2D* FlagTexture = UTHUDOwner->ResolveFlag(PlayerState, FlagUV);
	if (!FlagTexture) return;

	const float Border = FMath::Max(1.f, 2.f * RenderScale);
	Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.85f * Opacity));
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset - Border, YOffset - Border,
		FlagWidth + 2.f * Border, FlagHeight + 2.f * Border, 0.f, 0.f, 1.f, 1.f,
		BLEND_Translucent);

	AUTCharacter* Character = PlayerState->GetUTCharacter();
	const bool bIsDead = !Character || Character->IsDead();
	const float Luminance = bIsDead ? 0.35f : 1.f;
	Canvas->SetLinearDrawColor(FLinearColor(Luminance, Luminance, Luminance, Opacity));
	Canvas->DrawTile(FlagTexture, XOffset, YOffset, FlagWidth, FlagHeight,
		FlagUV.U, FlagUV.V, FlagUV.UL, FlagUV.VL, BLEND_Translucent);
	Canvas->SetLinearDrawColor(FLinearColor::White);
}

// Helper: locate the stats replicator on this client
static AElimPlusStatsReplicator* FindElimPlusStatsRep(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AElimPlusStatsReplicator> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void UElimPlusScoreboard::DrawAbsoluteTeamPanel(float RenderDelta, float& YOffset)
{
	if (!Canvas || !UTHUDOwner || !UTGameState || UTGameState->Teams.Num() < 2) return;

	const float S = FMath::Min(float(Canvas->ClipX) / 2560.f, float(Canvas->ClipY) / 1440.f);
	const float CenterX = Canvas->ClipX * 0.5f;
	const float TopY = 314.f * S;
	const float BannerW = 960.f * S;
	const float BannerH = 140.f * S;
	const float BannerY = TopY - BannerH;
	const float LeftX = CenterX - BannerW;
	const float RightX = CenterX;

	FAbsoluteElimScoreboardTextures& T = GAbsoluteElimScoreboardTextures;
	DrawAbsoluteElimScoreboardTile(Canvas, T.Banner[0], LeftX, BannerY, BannerW, BannerH, 0.9275f);
	DrawAbsoluteElimScoreboardTile(Canvas, T.Banner[1], RightX, BannerY, BannerW, BannerH, 0.9275f, true);

	UFont* TeamNameFont = UTHUDOwner->MediumFont ? UTHUDOwner->MediumFont : UTHUDOwner->SmallFont;
	UFont* TeamScoreFont = UTHUDOwner->HugeFont ? UTHUDOwner->HugeFont : TeamNameFont;
	const FLinearColor ScoreboardWhite(0.75f, 0.75f, 0.75f, 1.f);
	const float NameY = BannerY + 45.f * S;
	// Keep the large score numerals on the same visual centerline as the team names.
	const float ScoreY = NameY;
	DrawText(FText::FromString(TEXT("RED TEAM")), LeftX + 89.f * S, NameY,
		TeamNameFont, 2.f * S, 1.f, ScoreboardWhite, ETextHorzPos::Left, ETextVertPos::Center);
	DrawText(FText::FromString(TEXT("BLUE TEAM")), RightX + 89.f * S, NameY,
		TeamNameFont, 2.f * S, 1.f, ScoreboardWhite, ETextHorzPos::Left, ETextVertPos::Center);

	const int32 RedScore = UTGameState->Teams.IsValidIndex(0) && UTGameState->Teams[0]
		? UTGameState->Teams[0]->Score : 0;
	const int32 BlueScore = UTGameState->Teams.IsValidIndex(1) && UTGameState->Teams[1]
		? UTGameState->Teams[1]->Score : 0;
	DrawText(FText::AsNumber(RedScore), LeftX + 807.f * S, ScoreY,
		TeamScoreFont, S * RedScoreScaling, 1.f, FLinearColor::White,
		ETextHorzPos::Center, ETextVertPos::Center);
	DrawText(FText::AsNumber(BlueScore), RightX + 807.f * S, ScoreY,
		TeamScoreFont, S * BlueScoreScaling, 1.f, FLinearColor::White,
		ETextHorzPos::Center, ETextVertPos::Center);

	BlueScoreScaling = FMath::Max(BlueScoreScaling - RenderDelta, 1.f);
	RedScoreScaling = FMath::Max(RedScoreScaling - RenderDelta, 1.f);
	YOffset = TopY;
}

void UElimPlusScoreboard::DrawAbsoluteScoreHeaders(float RenderDelta, float& YOffset)
{
	if (!Canvas || !UTHUDOwner) return;

	const float S = FMath::Min(float(Canvas->ClipX) / 2560.f, float(Canvas->ClipY) / 1440.f);
	const float CenterX = Canvas->ClipX * 0.5f;
	const float CategoriesW = 800.f * S;
	const float CategoriesH = 30.f * S;
	const float RowW = 830.f * S;
	const float LeftRowX = CenterX - RowW;
	const float RightRowX = CenterX;
	DrawAbsoluteElimScoreboardTile(Canvas, GAbsoluteElimScoreboardTextures.Categories,
		CenterX - CategoriesW, YOffset, CategoriesW, CategoriesH, 0.75f, true);
	DrawAbsoluteElimScoreboardTile(Canvas, GAbsoluteElimScoreboardTextures.Categories,
		CenterX, YOffset, CategoriesW, CategoriesH, 0.75f, true);

	UFont* Font = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
	const float TextY = YOffset + 11.f * S;
	const float TextScale = 0.8f * S;
	const FLinearColor TextColor(0.75f, 0.75f, 0.75f, 1.f);
	// Column order mirrors the standard ElimPlus board (K | D | DMG | ELO, ping
	// last) so both scoreboard skins read the same left-to-right. Header, player
	// row, and totals row all share these slots — keep the three in sync.
	const TCHAR* Labels[] = { TEXT("K"), TEXT("D"), TEXT("DMG"), TEXT("ELO"), TEXT("PING") };
	const float Offsets[] = { 400.f, 470.f, 549.f, 640.f, 740.f };

	DrawText(FText::FromString(TEXT("PLAYER")), LeftRowX + 80.f * S, TextY,
		Font, TextScale, 1.f, TextColor, ETextHorzPos::Left, ETextVertPos::Center);
	DrawText(FText::FromString(TEXT("PLAYER")), RightRowX + 80.f * S, TextY,
		Font, TextScale, 1.f, TextColor, ETextHorzPos::Left, ETextVertPos::Center);
	for (int32 Column = 0; Column < 5; ++Column)
	{
		const FText Label = FText::FromString(Labels[Column]);
		DrawText(Label, LeftRowX + Offsets[Column] * S, TextY,
			Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
		DrawText(Label, RightRowX + Offsets[Column] * S, TextY,
			Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
	}

	YOffset += CategoriesH;
}

void UElimPlusScoreboard::DrawAbsolutePlayer(AUTPlayerState* PlayerState, int32 TeamIndex,
	float XOffset, float YOffset, float AbsoluteScale)
{
	if (!Canvas || !UTHUDOwner || !PlayerState) return;

	const float S = AbsoluteScale;
	const float RowW = 830.f * S;
	const float RowH = 64.f * S;
	AUTCharacter* Character = PlayerState->GetUTCharacter();
	const bool bIsDead = !Character || Character->IsDead();
	const int32 Style = int32(bIsDead ? EAbsoluteElimRowStyle::Dead : EAbsoluteElimRowStyle::Normal);
	DrawAbsoluteElimScoreboardTile(Canvas,
		GAbsoluteElimScoreboardTextures.Row[TeamIndex][Style],
		XOffset, YOffset, RowW, RowH, 0.9275f);

	if (bIsInteractive)
	{
		const FVector4 Bounds(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + RowW, RenderPosition.Y + YOffset + RowH);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
	}

	const bool bOwner = UTHUDOwner->UTPlayerOwner
		&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState;
	if (bOwner)
	{
		const float Border = FMath::Max(1.f, 2.f * S);
		Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.9704f, 1.f, 0.9f));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, RowW, Border, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset + RowH - Border, RowW, Border, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor::White);
	}

	const float FlagW = 36.f * S;
	const float FlagH = 26.f * S;
	const float FlagX = XOffset + 30.f * S;
	DrawPlayerFlag(PlayerState, FlagX, YOffset + 13.f * S, FlagW, FlagH,
		bIsDead ? 0.6f : 1.f);

	UFont* RowFont = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
	const float RowTextScale = 1.2f * S;
	const float TextY = YOffset + RowH * 0.5f + 2.f * S;
	FLinearColor TextColor = bIsDead
		? FLinearColor(0.35f, 0.35f, 0.35f, 1.f)
		: FLinearColor(0.75f, 0.75f, 0.75f, 1.f);
	if (bOwner)
	{
		TextColor = bIsDead
			? FLinearColor(0.04f, 0.098f, 0.10f, 1.f)
			: FLinearColor(0.f, 0.9704f, 1.f, 1.f);
	}

	float NameXL = 0.f, NameYL = 0.f;
	Canvas->StrLen(RowFont, PlayerState->PlayerName, NameXL, NameYL);
	const float NameScale = FMath::Min(RowTextScale, 285.f * S / FMath::Max(NameXL, 1.f));
	const float NameX = XOffset + 80.f * S;
	DrawText(FText::FromString(PlayerState->PlayerName), NameX, TextY,
		RowFont, NameScale, 1.f, TextColor,
		ETextHorzPos::Left,
		ETextVertPos::Center);

	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());
	const FString PlayerId = PlayerState->UniqueId.IsValid()
		? PlayerState->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *PlayerState->PlayerName);
	const FElimPlusStatsEntry* Entry = Stats ? Stats->FindEntry(PlayerId) : nullptr;
	const int32 Damage = Entry ? Entry->DamageDone : int32(PlayerState->DamageDone);
	const int32 Elo = Entry ? Entry->Elo : 1400;
	const int32 Kills = PlayerState->Kills + PlayerState->KillAssists;

	FString PingString;
	if (AUTBot* Bot = Cast<AUTBot>(PlayerState->GetOwner()))
	{
		PingString = FString::Printf(TEXT("%.1f"), Bot->Skill);
	}
	else if (GetWorld()->GetNetMode() != NM_Standalone)
	{
		const int32 Ping = bOwner ? PlayerState->ExactPing : PlayerState->Ping * 4;
		PingString = FString::Printf(TEXT("%d"), Ping);
	}

	auto ColumnX = [XOffset, S](float Offset)
	{
		return XOffset + Offset * S;
	};
	const float StatScale = 0.92f * S;
	if (!UTGameState->HasMatchStarted())
	{
		const FString ReadyString = PlayerState->bPendingTeamSwitch
			? TEXT("SWITCH TEAMS") : (PlayerState->bIsWarmingUp ? TEXT("WARMUP") : TEXT("NOT READY"));
		DrawText(FText::FromString(ReadyString), ColumnX(640.f), TextY,
			RowFont, StatScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
	else
	{
		DrawText(FText::AsNumber(Kills), ColumnX(400.f), TextY,
			RowFont, StatScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
		DrawText(FText::AsNumber(PlayerState->Deaths), ColumnX(470.f), TextY,
			RowFont, StatScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
		DrawText(FText::AsNumber(Damage), ColumnX(549.f), TextY,
			RowFont, StatScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
		DrawText(FText::AsNumber(Elo), ColumnX(640.f), TextY,
			RowFont, StatScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
		DrawText(FText::FromString(PingString), ColumnX(740.f), TextY,
			RowFont, 0.67f * S, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
}

void UElimPlusScoreboard::DrawAbsolutePlayerScores(float RenderDelta, float& YOffset)
{
	if (!Canvas || !UTHUDOwner || !UTGameState) return;

	const float S = FMath::Min(float(Canvas->ClipX) / 2560.f, float(Canvas->ClipY) / 1440.f);
	const float CenterX = Canvas->ClipX * 0.5f;
	const float RowW = 830.f * S;
	const float RowH = 64.f * S;
	float MaxY = YOffset;
	TArray<FString> SpectatorNames;
	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());

	auto GetEntry = [Stats](const AUTPlayerState* PlayerState) -> const FElimPlusStatsEntry*
	{
		if (!Stats || !PlayerState) return nullptr;
		const FString PlayerId = PlayerState->UniqueId.IsValid()
			? PlayerState->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PlayerState->PlayerName);
		return Stats->FindEntry(PlayerId);
	};

	for (int32 Team = 0; Team < 2; ++Team)
	{
		TArray<AUTPlayerState*> Players;
		for (APlayerState* PlayerBase : UTGameState->PlayerArray)
		{
			AUTPlayerState* PlayerState = Cast<AUTPlayerState>(PlayerBase);
			if (!PlayerState) continue;
			if (PlayerState->bOnlySpectator)
			{
				if (Team == 0 && !PlayerState->bIsDemoRecording)
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
				continue;
			}
			if (PlayerState->GetTeamNum() == Team)
			{
				Players.Add(PlayerState);
			}
		}

		// Rank by TOTAL damage (the number the DMG column shows), not the PPR
		// mean: PPR divides by rounds-played, so a late joiner's one hot round
		// outranked players carrying the whole match. Kills then score tiebreak.
		Players.Sort([GetEntry](const AUTPlayerState& A, const AUTPlayerState& B)
		{
			const FElimPlusStatsEntry* EA = GetEntry(&A);
			const FElimPlusStatsEntry* EB = GetEntry(&B);
			const int32 DA = EA ? EA->DamageDone : int32(A.DamageDone);
			const int32 DB = EB ? EB->DamageDone : int32(B.DamageDone);
			if (DA != DB) return DA > DB;
			const int32 KA = A.Kills + A.KillAssists;
			const int32 KB = B.Kills + B.KillAssists;
			if (KA != KB) return KA > KB;
			return A.Score > B.Score;
		});

		const float RowX = Team == 0 ? CenterX - RowW : CenterX;
		float DrawY = YOffset;
		const int32 MaxRows = ShouldDrawScoringStats() ? 5 : Players.Num();
		int32 DrawnRows = 0;
		for (AUTPlayerState* PlayerState : Players)
		{
			if (DrawnRows >= MaxRows) break;
			DrawAbsolutePlayer(PlayerState, Team, RowX, DrawY, S);
			DrawY += RowH;
			++DrawnRows;
		}

		if (Players.Num() > 0)
		{
			int32 TotalKills = 0, TotalDeaths = 0, TotalDamage = 0;
			int64 TotalElo = 0, TotalPing = 0;
			int32 EloCount = 0, PingCount = 0;
			for (AUTPlayerState* PlayerState : Players)
			{
				const FElimPlusStatsEntry* Entry = GetEntry(PlayerState);
				TotalKills += PlayerState->Kills + PlayerState->KillAssists;
				TotalDeaths += PlayerState->Deaths;
				TotalDamage += Entry ? Entry->DamageDone : int32(PlayerState->DamageDone);
				TotalElo += Entry ? Entry->Elo : 1400;
				++EloCount;
				if (!Cast<AUTBot>(PlayerState->GetOwner()) && GetWorld()->GetNetMode() != NM_Standalone)
				{
					const bool bOwner = UTHUDOwner->UTPlayerOwner
						&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState;
					TotalPing += bOwner ? PlayerState->ExactPing : PlayerState->Ping * 4;
					++PingCount;
				}
			}

			DrawAbsoluteElimScoreboardTile(Canvas,
				GAbsoluteElimScoreboardTextures.Row[Team][int32(EAbsoluteElimRowStyle::Totals)],
				RowX, DrawY, RowW, RowH, 0.9275f);
			UFont* Font = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
			const float TextY = DrawY + RowH * 0.5f + 2.f * S;
			const float TextScale = 0.92f * S;
			const FLinearColor TextColor(0.75f, 0.75f, 0.75f, 1.f);
			auto ColumnX = [RowX, S](float Offset)
			{
				return RowX + Offset * S;
			};
			DrawText(FText::FromString(TEXT("TOTAL")), ColumnX(80.f), TextY,
				Font, 1.2f * S, 1.f, TextColor,
				ETextHorzPos::Left, ETextVertPos::Center);
			DrawText(FText::AsNumber(TotalKills), ColumnX(400.f), TextY,
				Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::AsNumber(TotalDeaths), ColumnX(470.f), TextY,
				Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::AsNumber(TotalDamage), ColumnX(549.f), TextY,
				Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::AsNumber(EloCount > 0 ? int32(TotalElo / EloCount) : 1400), ColumnX(640.f), TextY,
				Font, TextScale, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
			if (PingCount > 0)
			{
				DrawText(FText::AsNumber(int32(TotalPing / PingCount)), ColumnX(740.f), TextY,
					Font, 0.67f * S, 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
			}
			DrawY += RowH;
		}

		MaxY = FMath::Max(MaxY, DrawY);
	}

	YOffset = MaxY;
	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		const FString Spectators = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(Spectators), CenterX, YOffset + 26.f * S,
			UTHUDOwner->SmallFont, 0.8f * S, 1.f,
			FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}
}

void UElimPlusScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset)
{
	if (PlayerState == nullptr) return;

	float BarOpacity = FNCPlusHUDLayout::GetScoreboardOpacity();
	bool bIsUnderCursor = false;

	if (bIsInteractive)
	{
		FVector4 Bounds = FVector4(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + ScaledCellWidth, RenderPosition.Y + YOffset + CellHeight * RenderScale);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
		bIsUnderCursor = (CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z &&
			CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W);
	}

	PlayerState->ScoreCorner = FVector(RenderPosition.X + XOffset, RenderPosition.Y + YOffset + 0.25f * CellHeight * RenderScale, 0.f);
	if (!PlayerState->Team || (PlayerState->Team->TeamIndex != 1))
	{
		PlayerState->ScoreCorner.X += ScaledCellWidth;
	}

	const bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner) BarOpacity = FMath::Min(1.f, FNCPlusHUDLayout::GetScoreboardOpacity() + 0.2f);

	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	// Owner row: the stock light-Gray highlight washes out the (dead-dimmed) text on
	// the dark Concept-D panel — impossible to read your own row when dead. Force a
	// dark readable tint; the ">" caret + the higher owner opacity still mark it as
	// your row (cursor/selected still override below).
	if (bIsOwner) { BarColor = FLinearColor(0.10f, 0.13f, 0.20f, 1.f); }
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor) { BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0); FinalBarOpacity = 0.75f; }
	if (PlayerState == SelectedPlayer) { BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0); FinalBarOpacity = 0.75f; }

	DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, 0.95f * CellHeight * RenderScale,
		149, 138, 32, 32, FinalBarOpacity, BarColor);

	// Concept-D "you are here" row: the LOCAL player's own row gets the team-colored
	// border + faint tint (was the top-PPR row — redundant once the board is
	// score-sorted; finding your own row is the real mid-match scan task). True
	// spectators have no row in the team lists, so nothing highlights for them.
	// Additive — over the cell bg, under the row content.
	if (bIsOwner)
	{
		const uint8 OwnTeam = PlayerState->GetTeamNum();
		const bool bUseTC = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
		FLinearColor Accent = (OwnTeam == 1) ? FLinearColor(0.05f, 0.1f, 0.9f, 1.f) : FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
		if (bUseTC && UTGameState && UTGameState->Teams.IsValidIndex(OwnTeam) && UTGameState->Teams[OwnTeam])
		{
			Accent = UTGameState->Teams[OwnTeam]->TeamColor;
		}
		const float CW = ScaledCellWidth, CHgt = 0.95f * CellHeight * RenderScale, BW = 2.f * RenderScale;
		FLinearColor Tint = Accent; Tint.A = 0.12f;
		Canvas->SetLinearDrawColor(Tint);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, CW, CHgt, 0, 0, 1, 1, BLEND_Translucent);
		FLinearColor Border = Accent; Border.A = 0.9f;
		Canvas->SetLinearDrawColor(Border);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, CW, BW, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset + CHgt - BW, CW, BW, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, BW, CHgt, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset + CW - BW, YOffset, BW, CHgt, 0, 0, 1, 1, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor::White);
	}

	// Country flag replaces the old character-portrait pip. Keep the stock UT
	// 36:26 aspect and center it in the row's icon lane.
	const float FlagWidth = 54.f * RenderScale;
	const float FlagHeight = 39.f * RenderScale;
	const float FlagX = XOffset + 11.f * RenderScale;
	const float FlagY = YOffset + (0.95f * CellHeight * RenderScale - FlagHeight) * 0.5f;
	DrawPlayerFlag(PlayerState, FlagX, FlagY, FlagWidth, FlagHeight);

	// Player name
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);
	AUTCharacter* UTC_Name = PlayerState->GetUTCharacter();
	const bool bIsDead = (UTC_Name == nullptr || UTC_Name->IsDead());
	if (bIsDead) DrawColor *= 0.6f;

	FString DisplayName = PlayerState->PlayerName;
	float NameXL, NameYL;
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	const float MaxNameWidth = 0.22f * ScaledCellWidth; // tighter — 9 stat columns to fit
	const float NameScaling = FMath::Min(RenderScale, MaxNameWidth / FMath::Max(NameXL, 1.f));

	const float NameX = XOffset + (ScaledCellWidth * ColumnHeaderPlayerX);
	const float NameY = YOffset + ColumnY * 0.7f;

	if (!PlayerState->EpicAccountName.IsEmpty())
	{
		DrawText(FText::FromString(DisplayName), NameX, NameY, UTHUDOwner->SmallFont, false,
			FVector2D(0.f, 0.f), FLinearColor::Black, true, GetPlayerHighlightColorFor(PlayerState),
			NameScaling, 1.0f, DrawColor, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), ETextHorzPos::Left, ETextVertPos::Center);
	}
	else
	{
		DrawText(FText::FromString(DisplayName), NameX, NameY, UTHUDOwner->SmallFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
	}

	if (bIsOwner)
	{
		// U+25B6 BLACK RIGHT-POINTING TRIANGLE. Escape form is required because the
		// literal char in source bytes gets misinterpreted under Windows ANSI source
		// encoding (rendered as garbage glyphs like "ä-").
		DrawText(FText::FromString(FString::Chr(0x25B6)), NameX - 14.f * RenderScale, NameY, UTHUDOwner->TinyFont, RenderScale, 1.0f,
			FLinearColor(0.3f, 1.f, 0.3f, 1.f), ETextHorzPos::Left, ETextVertPos::Center);
	}

	// Match-host badge — tags the player who pressed Enter to start the match.
	NCPlusScoreboardHost::DrawHostMarker(this, UTHUDOwner, PlayerState, UTGameState,
		NameX + NameXL * NameScaling + 8.f * RenderScale, NameY, RenderScale);

	// HP/Armor bars for alive teammates (same pattern as Wipeout)
	AUTCharacter* UTC = PlayerState->GetUTCharacter();
	if (UTC != nullptr)
	{
		bool bShowBars = true;
		if (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState)
		{
			AUTPlayerState* LocalPS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
			bShowBars = UTGameState->OnSameTeam(PlayerState, LocalPS) || LocalPS->bOnlySpectator;
		}

		if (bShowBars)
		{
			const float HealthPct = FMath::Clamp(float(UTC->Health) / float(UTC->SuperHealthMax), 0.f, 1.f);
			const float ArmorPct = float(UTC->GetArmorAmount()) / float(FMath::Max(UTC->MaxStackedArmor, 1));
			const float BarHeight = 5.f * RenderScale;
			const float BarY = NameY + 14.f * RenderScale;
			const float BarX = NameX;
			const float HealthBarWidth = 80.f * RenderScale;
			const float ArmorBarWidth = 60.f * RenderScale;

			FLinearColor BarBG(0.15f, 0.15f, 0.15f, 0.6f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX, BarY, HealthBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			FLinearColor HealthColor(0.25f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX + 1.f, BarY + 1.f, (HealthBarWidth - 2.f) * HealthPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, HealthColor);

			const float ArmorBarX = BarX + HealthBarWidth + 6.f * RenderScale;
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX, BarY, ArmorBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			FLinearColor ArmorColor(0.8f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX + 1.f, BarY + 1.f, (ArmorBarWidth - 2.f) * ArmorPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, ArmorColor);
		}
	}

	// Stat columns
	if (UTGameState && UTGameState->HasMatchStarted())
	{
		if (PlayerState->bPendingTeamSwitch && !PlayerState->bIsABot)
		{
			// Queued team change (stock replicated bPendingTeamSwitch — set by
			// AUTTeamGameMode::ChangeTeam when a mid-match switch has to wait for
			// balance or a counterpart). Stock TEAM SWAP tag in place of the stat
			// columns, same treatment as the CTF/Duel/Shaft scoreboards.
			DrawText(TeamSwapText, XOffset + (ScaledCellWidth * ColumnHeaderScoreX),
				YOffset + ColumnY, UTHUDOwner->SmallFont, RenderScale, 1.0f,
				FLinearColor::White, ETextHorzPos::Center, ETextVertPos::Center);
		}
		else
		{
			DrawPlayerScore(PlayerState, XOffset, YOffset, ScaledCellWidth, DrawColor);
		}
	}
	else
	{
		DrawReadyText(PlayerState, XOffset, YOffset, ScaledCellWidth);
	}

	// Ping / Bot skill (same as Wipeout)
	AUTBot* Bot = Cast<AUTBot>(PlayerState->GetOwner());
	if (Bot)
	{
		static const FNumberFormattingOptions SkillFmt = FNumberFormattingOptions()
			.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		DrawText(FText::AsNumber(Bot->Skill, &SkillFmt), XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
	else if (GetWorld()->GetNetMode() != NM_Standalone)
	{
		const int32 Ping = bIsOwner ? PlayerState->ExactPing : (PlayerState->Ping * 4);
		const FLinearColor PingColor = (Ping < 60) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
			: (Ping < 120) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
			: FLinearColor(1.f, 0.25f, 0.25f, 1.f);
		DrawText(FText::FromString(FString::Printf(TEXT("%dms"), Ping)),
			XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, PingColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
}

void UElimPlusScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	// Resolve replicator + player id once. Bots have invalid UniqueIds, so use
	// the same synthetic "BOT:<name>" key the rating/replicator pair publishes
	// — so when bRandomizeBotElo is on, bots show their assigned ELO instead
	// of the default 1400 fallback.
	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());
	FString PId;
	if (PlayerState)
	{
		PId = PlayerState->UniqueId.IsValid()
			? PlayerState->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PlayerState->PlayerName);
	}
	const FElimPlusStatsEntry* Entry = (Stats && !PId.IsEmpty()) ? Stats->FindEntry(PId) : nullptr;

	const FLinearColor DimColor = (PlayerState->GetUTCharacter() == nullptr) ? FLinearColor(0.6f, 0.6f, 0.6f, 1.f) * DrawColor : DrawColor;

	// Kills
	const int32 Kills = PlayerState->Kills + PlayerState->KillAssists;
	DrawText(FText::AsNumber(Kills),
		XOffset + (Width * ColumnHeaderKillsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Deaths
	DrawText(FText::AsNumber(PlayerState->Deaths),
		XOffset + (Width * ColumnHeaderDeathsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage — replicator preferred; fall back to direct PlayerState read on listen-server
	int32 Damage = 0;
	if (Entry)
	{
		Damage = Entry->DamageDone;
	}
	else
	{
		Damage = int32(PlayerState->DamageDone);
	}
	const FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f) * (DimColor / DrawColor);
	DrawText(FText::AsNumber(Damage),
		XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// PPR (Current) — match-running mean across completed rounds (gamemode populates)
	const float PPRCur = Entry ? Entry->PPRCurrent : 0.f;
	DrawText(FText::FromString(FString::Printf(TEXT("%.1f"), PPRCur)),
		XOffset + (Width * ColumnHeaderPPRCurX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// ELO + delta — source of truth is the replicator (gamemode pushes from
	// Mods.db). Don't fall back to PlayerState rank fields — TDMRank etc. are
	// defunct in this fork (see feedback_no_epic_mcp_or_tdmrank memory).
	const int32 Elo = Entry ? Entry->Elo : 1400;
	const int32 EloDelta = Entry ? Entry->EloDeltaThisMatch : 0;
	const int32 Rank = Entry ? Entry->GlobalRank : 0;
	FString EloStr = FString::Printf(TEXT("%d"), Elo);
	if (Rank > 0)
	{
		// Global leaderboard rank in parens, with English ordinal suffix:
		// 1st, 2nd, 3rd, 4th ... 11th-13th, 21st, 200th.
		const int32 M100 = Rank % 100;
		const int32 M10  = Rank % 10;
		const TCHAR* Suf = (M100 >= 11 && M100 <= 13) ? TEXT("th")
			: (M10 == 1) ? TEXT("st") : (M10 == 2) ? TEXT("nd") : (M10 == 3) ? TEXT("rd") : TEXT("th");
		EloStr += FString::Printf(TEXT(" (%d%s)"), Rank, Suf);
	}
	if (EloDelta != 0)
	{
		EloStr += (EloDelta > 0)
			? FString::Printf(TEXT(" +%d"), EloDelta)
			: FString::Printf(TEXT(" %d"), EloDelta);
	}
	FLinearColor EloColor = DimColor;
	if (EloDelta > 0)      EloColor = FLinearColor(0.4f, 1.f, 0.4f, 1.f);
	else if (EloDelta < 0) EloColor = FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	DrawText(FText::FromString(EloStr),
		XOffset + (Width * ColumnHeaderEloX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, EloColor, ETextHorzPos::Center, ETextVertPos::Center);

	// LG_Acc — Sniper / Lightning Gun hitscan accuracy, computed + replicated
	// server-side (NAME_SniperHits/Shots). -1 = no sniper shots fired -> show "-"
	// (matches NCPlusCTFScoreboard) instead of a misleading 0%.
	const int32 LGAccPacked = Entry ? Entry->LinkGunAccuracyTimes100 : -1;
	const bool bHasLGAcc = (LGAccPacked >= 0);
	const float LGAcc = bHasLGAcc ? (static_cast<float>(LGAccPacked) / 100.f) : 0.f;
	const FLinearColor LGColor = !bHasLGAcc       ? FLinearColor(0.5f, 0.5f, 0.5f, 1.f)
		: (LGAcc >= 35.f) ? FLinearColor(0.4f, 1.f, 0.4f, 1.f)
		: (LGAcc >= 20.f) ? FLinearColor(1.f, 1.f, 0.4f, 1.f)
		: (LGAcc > 0.f)   ? FLinearColor(1.f, 0.5f, 0.4f, 1.f)
		: FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
	DrawText(FText::FromString(bHasLGAcc ? FString::Printf(TEXT("%.0f%%"), LGAcc) : TEXT("-")),
		XOffset + (Width * ColumnHeaderLGAccX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, LGColor, ETextHorzPos::Center, ETextVertPos::Center);
}

void UElimPlusScoreboard::DrawPlayerScores(float RenderDelta, float& YOffset)
{
	if (ShouldDrawAbsoluteElimScoreboard())
	{
		DrawAbsolutePlayerScores(RenderDelta, YOffset);
		return;
	}
	if (!UTGameState) return;

	int32 XOffset = ScaledEdgeSize;
	float MaxYOffset = 0.f;
	TArray<FString> SpectatorNames;

	// Damage lookup for row ordering — same replicator + uid resolution
	// DrawPlayerScore uses (bots key on the synthetic "BOT:<name>"). Falls back
	// to the PlayerState's own tally when the replicator isn't up yet, so the
	// board still orders sensibly in the opening seconds.
	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());
	auto GetDamage = [Stats](AUTPlayerState* PS) -> int32
	{
		if (!PS) return 0;
		const FString PId = PS->UniqueId.IsValid()
			? PS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
		const FElimPlusStatsEntry* E = (Stats && !PId.IsEmpty()) ? Stats->FindEntry(PId) : nullptr;
		return E ? E->DamageDone : int32(PS->DamageDone);
	};

	for (int8 Team = 0; Team < 2; Team++)
	{
		int32 Place = 1;
		float DrawOffset = YOffset;
		const int32 NumPlayersToShow = ShouldDrawScoringStats() ? 5 : UTGameState->PlayerArray.Num();

		// Collect this team's players (harvesting spectators once, on the team-0
		// pass), then sort by total damage desc. Damage, not the PPR mean: PPR
		// divides by rounds-played, so a late joiner's one hot round outranked
		// players carrying the whole match. Kills then score break ties.
		TArray<AUTPlayerState*> TeamPlayers;
		TMap<const AUTPlayerState*, int32> DamageByPlayer;
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PlayerState = Cast<AUTPlayerState>(UTGameState->PlayerArray[i]);
			if (!PlayerState) continue;
			if (PlayerState->bOnlySpectator)
			{
				if (Team == 0 && !PlayerState->bIsDemoRecording)
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
				continue;
			}
			if (PlayerState->GetTeamNum() == Team)
			{
				TeamPlayers.Add(PlayerState);
				DamageByPlayer.Add(PlayerState, GetDamage(PlayerState));
			}
		}
		TeamPlayers.Sort([&DamageByPlayer](const AUTPlayerState& A, const AUTPlayerState& B)
		{
			const int32 DA = DamageByPlayer.FindRef(&A);
			const int32 DB = DamageByPlayer.FindRef(&B);
			if (DA != DB) return DA > DB;
			const int32 KA = A.Kills + A.KillAssists;
			const int32 KB = B.Kills + B.KillAssists;
			if (KA != KB) return KA > KB;
			return A.Score > B.Score;
		});

		// Concept-D dark panel backdrop behind this team's rows + a thin team-color
		// top accent (no new texture — a translucent dark tile under the rows).
		{
			const int32 ActualRows = FMath::Min(NumPlayersToShow, TeamPlayers.Num());
			const float RowsH = ActualRows * CellHeight * RenderScale;
			const float PadX = 6.f * RenderScale, PadTop = 4.f * RenderScale;
			const bool bUseTC = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
			FLinearColor Accent = (bUseTC && UTGameState->Teams.IsValidIndex(Team) && UTGameState->Teams[Team]) ? UTGameState->Teams[Team]->TeamColor : ((Team == 1) ? FLinearColor(0.05f, 0.1f, 0.9f, 1.f) : FLinearColor(0.8f, 0.05f, 0.05f, 1.f));
			Canvas->SetLinearDrawColor(FLinearColor(0.03f, 0.04f, 0.06f, 0.78f));
			Canvas->DrawTile(Canvas->DefaultTexture, XOffset - PadX, DrawOffset - PadTop, ScaledCellWidth + 2.f * PadX, RowsH + PadTop + 40.f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
			Accent.A = 0.9f;
			Canvas->SetLinearDrawColor(Accent);
			Canvas->DrawTile(Canvas->DefaultTexture, XOffset - PadX, DrawOffset - PadTop, ScaledCellWidth + 2.f * PadX, 2.f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
			Canvas->SetLinearDrawColor(FLinearColor::White);
		}

		for (AUTPlayerState* PlayerState : TeamPlayers)
		{
			DrawPlayer(Place, PlayerState, RenderDelta, XOffset, DrawOffset);
			Place++;
			DrawOffset += CellHeight * RenderScale;
			if (Place > NumPlayersToShow) break;
		}

		// Team totals row under this team's rows — sums of the columns above, aligned
		// under each column. K / D / DMG / PPR are additive; ELO / LG_Acc / Ping aren't,
		// so they're left blank. (Replaced the per-team MVP banner.)
		if (TeamPlayers.Num() > 0)
		{
			int32 SumK = 0, SumD = 0, SumDMG = 0;
			float SumPPR = 0.f;
			int64 SumElo = 0;  int32 CountElo = 0;   // ELO: team average
			float SumAcc = 0.f; int32 CountAcc = 0;  // LG_Acc: average of players with data
			int64 SumPing = 0; int32 CountPing = 0;  // Ping: average of HUMANs (bots show skill)
			const bool bNetworked = (GetWorld()->GetNetMode() != NM_Standalone);
			for (AUTPlayerState* TP : TeamPlayers)
			{
				if (!TP) continue;
				SumK += TP->Kills;
				SumD += TP->Deaths;
				const FString PId = TP->UniqueId.IsValid() ? TP->UniqueId.ToString() : FString::Printf(TEXT("BOT:%s"), *TP->PlayerName);
				const FElimPlusStatsEntry* E = (Stats && !PId.IsEmpty()) ? Stats->FindEntry(PId) : nullptr;
				SumPPR += E ? E->PPRCurrent : 0.f;
				SumDMG += E ? E->DamageDone : int32(TP->DamageDone);
				SumElo += E ? E->Elo : 1400; ++CountElo;
				if (E && E->LinkGunAccuracyTimes100 >= 0) { SumAcc += float(E->LinkGunAccuracyTimes100) / 100.f; ++CountAcc; }
				if (bNetworked && !Cast<AUTBot>(TP->GetOwner()))
				{
					const bool bTPOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState == TP);
					SumPing += bTPOwner ? TP->ExactPing : (TP->Ping * 4);
					++CountPing;
				}
			}

			const float BX = XOffset - 6.f * RenderScale, BW2 = ScaledCellWidth + 12.f * RenderScale;
			const float BY = DrawOffset + 4.f * RenderScale, BH2 = 26.f * RenderScale;
			const float TY = BY + BH2 * 0.5f;
			const bool bUseTC = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
			FLinearColor Accent = (bUseTC && UTGameState->Teams.IsValidIndex(Team) && UTGameState->Teams[Team]) ? UTGameState->Teams[Team]->TeamColor : ((Team == 1) ? FLinearColor(0.05f, 0.1f, 0.9f, 1.f) : FLinearColor(0.8f, 0.05f, 0.05f, 1.f));

			Canvas->SetLinearDrawColor(FLinearColor(0.f, 0.f, 0.f, 0.5f));
			Canvas->DrawTile(Canvas->DefaultTexture, BX, BY, BW2, BH2, 0, 0, 1, 1, BLEND_Translucent);
			// Team-color "TOTAL" chip in the name column.
			Canvas->SetLinearDrawColor(Accent);
			Canvas->DrawTile(Canvas->DefaultTexture, BX + 6.f * RenderScale, BY + 5.f * RenderScale, 56.f * RenderScale, BH2 - 10.f * RenderScale, 0, 0, 1, 1, BLEND_Translucent);
			Canvas->SetLinearDrawColor(FLinearColor::White);
			DrawText(FText::FromString(TEXT("TOTAL")), BX + 34.f * RenderScale, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::White, ETextHorzPos::Center, ETextVertPos::Center);

			// Sums centered under the same column X's the rows use.
			const FLinearColor TotCol(0.88f, 0.90f, 0.95f, 1.f);
			DrawText(FText::AsNumber(SumK),   XOffset + ScaledCellWidth * ColumnHeaderKillsX,  TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, TotCol, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::AsNumber(SumD),   XOffset + ScaledCellWidth * ColumnHeaderDeathsX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, TotCol, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::AsNumber(SumDMG), XOffset + ScaledCellWidth * ColumnHeaderDamageX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor(1.f, 0.8f, 0.25f, 1.f), ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(FText::FromString(FString::Printf(TEXT("%.1f"), SumPPR)), XOffset + ScaledCellWidth * ColumnHeaderPPRCurX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, TotCol, ETextHorzPos::Center, ETextVertPos::Center);

			// Averages (not additive): ELO, LG_Acc, Ping — centered under their columns.
			if (CountElo > 0)
			{
				DrawText(FText::AsNumber(int32(SumElo / CountElo)), XOffset + ScaledCellWidth * ColumnHeaderEloX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, TotCol, ETextHorzPos::Center, ETextVertPos::Center);
			}
			DrawText(FText::FromString(CountAcc > 0 ? FString::Printf(TEXT("%.0f%%"), SumAcc / CountAcc) : TEXT("-")),
				XOffset + ScaledCellWidth * ColumnHeaderLGAccX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, TotCol, ETextHorzPos::Center, ETextVertPos::Center);
			if (CountPing > 0)
			{
				const int32 AvgPing = int32(SumPing / CountPing);
				const FLinearColor PingCol = (AvgPing < 60) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
					: (AvgPing < 120) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
					: FLinearColor(1.f, 0.25f, 0.25f, 1.f);
				DrawText(FText::FromString(FString::Printf(TEXT("%dms"), AvgPing)), XOffset + ScaledCellWidth * ColumnHeaderPingX, TY, UTHUDOwner->TinyFont, RenderScale, RenderScale, PingCol, ETextHorzPos::Center, ETextVertPos::Center);
			}

			DrawOffset += BH2 + 6.f * RenderScale;
		}

		MaxYOffset = FMath::Max(DrawOffset, MaxYOffset);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset = MaxYOffset;

	if (UTGameState->GoalScore > 0 && !ShouldDrawScoringStats())
	{
		FString GoalStr = FString::Printf(TEXT("First to %d"), UTGameState->GoalScore);
		DrawText(FText::FromString(GoalStr), Canvas->ClipX * 0.5f, YOffset + 4.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}
