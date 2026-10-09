// NCPlusSpectatorSlideOut.cpp — see header for the full rationale.

#include "NCPlusSpectatorSlideOut.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "StatNames.h"
#include "EngineUtils.h"
#include "NCAccuracyStatsReplicator.h"
#include "CTFStatsReplicator.h"
#include "WipeoutDamageReplicator.h"
#include "WipeoutGame.h"
#include "UTCTFGameState.h"
#include "UTCarriedObject.h"
#include "UTTeamInfo.h"
#include "Engine/Canvas.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	int32 GMatchOverlayEnabled = -1;
	bool ContainsPoint(const FVector4& Bounds, const FVector2D& Point)
	{
		return Point.X >= Bounds.X && Point.X <= Bounds.Z && Point.Y >= Bounds.Y && Point.Y <= Bounds.W;
	}
	bool IsWipeoutMatch(const AUTGameState* GS)
	{
		return GS && GS->GameModeClass && GS->GameModeClass->IsChildOf(AUWipeoutGame::StaticClass());
	}
}

bool UNCPlusSpectatorSlideOut::IsMatchOverlayEnabled()
{
	if (GMatchOverlayEnabled < 0)
	{
		bool bEnabled = true;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("NetcodePlus"), TEXT("ExpandedSpectatorSlideout"), bEnabled,
				FPaths::GeneratedConfigDir() + TEXT("Mod.ini"));
		}
		GMatchOverlayEnabled = bEnabled ? 1 : 0;
	}
	return GMatchOverlayEnabled != 0;
}

void UNCPlusSpectatorSlideOut::SetMatchOverlayEnabled(bool bEnabled)
{
	GMatchOverlayEnabled = bEnabled ? 1 : 0;
	if (GConfig)
	{
		const FString Path = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		GConfig->SetBool(TEXT("NetcodePlus"), TEXT("ExpandedSpectatorSlideout"), bEnabled, Path);
		GConfig->Flush(false, Path);
	}
}

UNCPlusSpectatorSlideOut::UNCPlusSpectatorSlideOut(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	WeaponListMode = ENCSlideOutWeaponMode::Passthrough;
	bSuppressRosterDraw = false;
	InstagibFallbackRow.Label = NSLOCTEXT("NCSlideOut", "InstagibRifle", "Instagib Rifle");
	InstagibFallbackRow.HitsStat = NAME_InstagibHits;
	InstagibFallbackRow.ShotsStat = NAME_InstagibShots;
}

void UNCPlusSpectatorSlideOut::Draw_Implementation(float DeltaTime)
{
	MatchHitRows.Reset();
	bDrawingMatchOverlay = CanUseMatchOverlay() && UTHUDOwner->TinyFont && Canvas;
	if (bDrawingMatchOverlay)
	{
		UWorld* World = GetWorld();
		if (MatchWorld.Get() != World)
		{
			MatchWorld = World;
			MatchRows.Reset();
			MatchDamageReplicator.Reset();
			NextMatchReplicatorRetryTime = 0.f;
		}
		for (auto It = MatchRows.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid() || It.Key()->bIsInactive) { It.RemoveCurrent(); }
		}
		bMatchInstagib = MatchOverlayMode == ENCSlideOutMatchMode::CTF && IsInstagibMatch();
		if (MatchOverlayMode == ENCSlideOutMatchMode::Wipeout && !MatchDamageReplicator.IsValid()
			&& (World->TimeSeconds >= NextMatchReplicatorRetryTime || NextMatchReplicatorRetryTime - World->TimeSeconds > 1.f))
		{
			NextMatchReplicatorRetryTime = World->TimeSeconds + 1.f;
			for (TActorIterator<AWipeoutDamageReplicator> It(World); It; ++It)
			{
				MatchDamageReplicator = *It;
				break;
			}
		}
	}
	// A TRUE spectator (bOnlySpectator — a caster/observer, not an eliminated player)
	// must ALWAYS get the slide-out: it's their primary tool (camera switching +
	// per-player weapons), exactly as stock Elim shows it. Only suppress the roster
	// VISUAL for an eliminated PLAYER, whose own team panel already shows the roster.
	bool bTrueSpectator = false;
	if (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->PlayerState)
	{
		bTrueSpectator = UTHUDOwner->UTPlayerOwner->PlayerState->bOnlySpectator;
	}

	// Suppress only the roster VISUAL (and only for a non-true-spectator). ShouldDraw
	// still runs — it is the SOLE bootstrap for the interactive spectator Slate window
	// (SUTSpectatorWindow: cursor capture, ESC->menu, camera switch) — so input stays
	// alive regardless; we just skip painting the redundant roster.
	if (bSuppressRosterDraw && !bTrueSpectator)
	{
		return;
	}
	Super::Draw_Implementation(DeltaTime);
}

float UNCPlusSpectatorSlideOut::GetDrawScaleOverride()
{
	// PreDraw applies this scale to text, icons, positions and both stock/custom
	// hit bounds. Keep it out of Draw so scaling never accumulates across frames.
	return Super::GetDrawScaleOverride() * (CanUseMatchOverlay() ? 0.9f : 1.f);
}

bool UNCPlusSpectatorSlideOut::CanUseMatchOverlay() const
{
	const AUTPlayerState* OwnerPS = UTPlayerOwner ? UTPlayerOwner->UTPlayerState : nullptr;
	// Input arrives after PostDraw, which clears Canvas. Only drawing requires
	// that transient pointer; viewer/mode eligibility must also work between frames.
	// Dead Wipeout players retain the stock roster and its visibility restrictions.
	return IsMatchOverlayEnabled() && OwnerPS && OwnerPS->bOnlySpectator && IsValid(UTGameState)
		&& UTHUDOwner
		&& ((MatchOverlayMode == ENCSlideOutMatchMode::CTF && Cast<AUTCTFGameState>(UTGameState))
			|| (MatchOverlayMode == ENCSlideOutMatchMode::Wipeout && IsWipeoutMatch(UTGameState)));
}

int32 UNCPlusSpectatorSlideOut::MatchOverlayColumnCount() const
{
	return MatchOverlayMode == ENCSlideOutMatchMode::Wipeout ? 4 : 7;
}

float UNCPlusSpectatorSlideOut::MatchOverlayPlayerWidth() const
{
	// iCTF has no useful HP/armor totals. Close those columns while retaining
	// the name, weapon/carrier icon and the stock camera-button footprint.
	return bMatchInstagib ? Size.X - 88.f : Size.X;
}

float UNCPlusSpectatorSlideOut::MatchOverlayWidth() const
{
	return MatchOverlayPlayerWidth() + MatchOverlayColumnCount() * 52.f;
}

float UNCPlusSpectatorSlideOut::MatchOverlayX(float StockX) const
{
	// Keep stock camera/flag/powerup controls at their authored size. Only the
	// roster travels its wider distance, so its columns slide fully off screen.
	return StockX * MatchOverlayWidth() / FMath::Max(Size.X, 1.f);
}

void UNCPlusSpectatorSlideOut::DrawMatchCell(const FText& Text, float CenterX, float Y,
	float Width, const FLinearColor& Color)
{
	float XL, YL;
	Canvas->TextSize(UTHUDOwner->TinyFont, Text.ToString(), XL, YL);
	const float Scale = FMath::Min(0.82f, (Width - 6.f) / FMath::Max(XL, 1.f));
	DrawText(Text, CenterX, Y, UTHUDOwner->TinyFont, Scale, 1.f, Color, ETextHorzPos::Center, ETextVertPos::Center);
}

void UNCPlusSpectatorSlideOut::DrawPlayerHeader(float RenderDelta, float XOffset, float YOffset)
{
	if (!bDrawingMatchOverlay)
	{
		Super::DrawPlayerHeader(RenderDelta, XOffset, YOffset);
		return;
	}
	const float X = MatchOverlayX(XOffset);
	if (!bMatchInstagib)
	{
		Super::DrawPlayerHeader(RenderDelta, X, YOffset);
	}
	else if (bMatchInteractive)
	{
		// Stock DrawPlayerHeader always draws HP/AR. Reuse its camera commands,
		// layout and DrawCamBind hit registration without those two header icons.
		const FText CamLabel = UTPlayerOwner->bSpectateBehindView
			? NSLOCTEXT("UTSlideout", "CamType3P", "3P") : NSLOCTEXT("UTSlideout", "CamType1P", "1P");
		const float Spacing = 0.333f * (ColumnHeaderScoreX - 0.05f - CamTypeButtonStart - 2.7f * CamTypeButtonWidth - 0.3f);
		DrawCamBind(TEXT("ToggleBehindView"), CamLabel.ToString(), RenderDelta,
			X + CamTypeButtonStart * Size.X, YOffset, CamTypeButtonWidth * Size.X, false);
		DrawCamBind(TEXT("ToggleTacCom"), TEXT("X-Ray"), RenderDelta,
			X + (CamTypeButtonStart + CamTypeButtonWidth + Spacing) * Size.X, YOffset,
			1.7f * CamTypeButtonWidth * Size.X, UTPlayerOwner->bTacComView);
		DrawCamBind(TEXT("EnableAutoCam"), TEXT("Auto Cam"), RenderDelta,
			X + (CamTypeButtonStart + 2.7f * CamTypeButtonWidth + 2.f * Spacing) * Size.X, YOffset,
			0.3f * Size.X, UTPlayerOwner->bAutoCam);
	}
	const float PlayerWidth = MatchOverlayPlayerWidth();
	DrawTexture(UTHUDOwner->ScoreboardAtlas, X + PlayerWidth, YOffset, MatchOverlayWidth() - PlayerWidth, 0.95f * CellHeight,
		149, 138, 32, 32, 0.65f, FLinearColor::Black);
	static const FText CTFHeaders[] = { FText::FromString(TEXT("CAP")), FText::FromString(TEXT("GRAB")), FText::FromString(TEXT("RET")), FText::FromString(TEXT("K/D")), FText::FromString(TEXT("EFF")), FText::FromString(TEXT("LG%")), FText::FromString(TEXT("SCORE")) };
	static const FText WipeHeaders[] = { FText::FromString(TEXT("K/D")), FText::FromString(TEXT("DMG")), FText::FromString(TEXT("DMG/L")), FText::FromString(TEXT("SCORE")) };
	static const FText InstagibHeader = FText::FromString(TEXT("IG%"));
	if (!bMatchInteractive)
	{
		static const FText PlayerHeader = NSLOCTEXT("NCSlideOut", "Player", "Player");
		DrawText(PlayerHeader, X + 34.f, YOffset + ColumnY, UTHUDOwner->TinyFont, 0.82f, 1.f,
			FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Center);
	}
	for (int32 Col = 0; Col < MatchOverlayColumnCount(); ++Col)
	{
		const FText& Header = MatchOverlayMode == ENCSlideOutMatchMode::CTF
			? ((Col == 5 && bMatchInstagib) ? InstagibHeader : CTFHeaders[Col]) : WipeHeaders[Col];
		DrawMatchCell(Header,
			X + PlayerWidth + (Col + 0.5f) * 52.f, YOffset + ColumnY, 52.f);
	}
}

const UNCPlusSpectatorSlideOut::FMatchRow& UNCPlusSpectatorSlideOut::GetMatchRow(AUTPlayerState* PS)
{
	FMatchRow& Row = MatchRows.FindOrAdd(TWeakObjectPtr<AUTPlayerState>(PS));
	// Names and slot numbers usually stay unchanged for the entire match. Rebuild
	// their text/measurements only on an actual change, before the 5 Hz stat gate
	// so renames, team-slot changes and font changes remain visible immediately.
	if (Row.PlayerName != PS->PlayerName || Row.ClanName != PS->ClanName || Row.NameFont.Get() != SlideOutFont)
	{
		Row.PlayerName = PS->PlayerName;
		Row.ClanName = PS->ClanName;
		Row.NameFont = SlideOutFont;
		const FString Name = Row.ClanName.IsEmpty() ? Row.PlayerName : TEXT("[") + Row.ClanName + TEXT("]") + Row.PlayerName;
		Row.DisplayName = FText::FromString(Name);
		float XL, YL; Canvas->TextSize(SlideOutFont, Name, XL, YL);
		Row.NameScale = FMath::Min(0.9f, 150.f / FMath::Max(XL, 1.f));
	}
	const int32 SpectatingID = UTGameState->bTeamGame ? PS->SpectatingIDTeam : PS->SpectatingID;
	const bool bNumberFontChanged = Row.NumberFont.Get() != UTHUDOwner->TinyFont;
	if (Row.SpectatingID != SpectatingID || bNumberFontChanged)
	{
		Row.SpectatingID = SpectatingID;
		Row.NumberFont = UTHUDOwner->TinyFont;
		Row.SpectatingLabel = FText::AsNumber(SpectatingID);
		float XL, YL; Canvas->TextSize(UTHUDOwner->TinyFont, Row.SpectatingLabel.ToString(), XL, YL);
		Row.SpectatingScale = FMath::Min(0.82f, 22.f / FMath::Max(XL, 1.f));
	}
	const float Now = GetWorld()->TimeSeconds;
	if (!bNumberFontChanged && Now < Row.NextUpdateTime && Row.NextUpdateTime - Now <= 0.2f) { return Row; }
	Row.NextUpdateTime = Now + 0.2f;
	const FString Id = PS->UniqueId.IsValid() ? PS->UniqueId.ToString() : FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
	const FText Missing = FText::FromString(TEXT("-"));
	const FText KD = FText::FromString(FString::Printf(TEXT("%d/%d"), PS->Kills, PS->Deaths));
	if (MatchOverlayMode == ENCSlideOutMatchMode::CTF)
	{
		int32 Hits = 0, Shots = 0;
		ACTFStatsReplicator* Rep = GetCTFStatsReplicator();
		const FCTFReplicatedStatsEntry* Entry = Rep ? Rep->FindEntry(Id) : nullptr;
		Row.Cells[0] = FText::AsNumber(PS->FlagCaptures);
		Row.Cells[1] = Entry ? FText::AsNumber(Entry->FlagGrabs)
			: (PS->HasAuthority() ? FText::AsNumber(PS->GetStatsValue(NAME_FlagGrabs)) : Missing);
		Row.Cells[2] = FText::AsNumber(PS->FlagReturns);
		Row.Cells[3] = KD;
		Row.Cells[4] = FText::FromString(FString::Printf(TEXT("%.0f%%"), 100.f * PS->Kills / FMath::Max(PS->Kills + PS->Deaths, 1)));
		if (Entry) { Hits = Entry->HitscanHits; Shots = Entry->HitscanShots; }
		else if (PS->HasAuthority())
		{
			const bool bIG = bMatchInstagib || PS->GetStatsValue(NAME_InstagibShots) > 0;
			Hits = bIG ? PS->GetStatsValue(NAME_InstagibHits) : PS->GetStatsValue(NAME_SniperHits) + PS->GetStatsValue(NAME_LightningRifleHits);
			Shots = bIG ? PS->GetStatsValue(NAME_InstagibShots) : PS->GetStatsValue(NAME_SniperShots) + PS->GetStatsValue(NAME_LightningRifleShots);
		}
		Row.Cells[5] = Shots > 0 ? FText::FromString(FString::Printf(TEXT("%.0f%%"), FMath::Clamp(100.f * Hits / Shots, 0.f, 100.f))) : Missing;
	}
	else
	{
		const FReplicatedDamageEntry* Entry = MatchDamageReplicator.IsValid() ? MatchDamageReplicator->FindEntry(Id) : nullptr;
		const bool bHasDamage = Entry || PS->HasAuthority();
		const int32 Damage = Entry ? Entry->DamageDone : int32(PS->DamageDone);
		Row.Cells[0] = KD;
		Row.Cells[1] = bHasDamage ? FText::AsNumber(Damage) : Missing;
		Row.Cells[2] = bHasDamage ? FText::AsNumber(Damage / FMath::Max(PS->Deaths + 1, 1)) : Missing;
	}
	Row.Cells[MatchOverlayColumnCount() - 1] = FText::AsNumber(int32(PS->Score));
	// Numeric formatting, replicated-array lookups and stat-cell font measurement
	// run at 5 Hz; live vitals and mouse feedback still follow the render frame.
	for (int32 Col = 0; Col < MatchOverlayColumnCount(); ++Col)
	{
		float XL, YL; Canvas->TextSize(UTHUDOwner->TinyFont, Row.Cells[Col].ToString(), XL, YL);
		Row.TextScales[Col] = FMath::Min(0.82f, 46.f / FMath::Max(XL, 1.f));
	}
	return Row;
}

void UNCPlusSpectatorSlideOut::DrawPlayer(int32 Index, AUTPlayerState* PS, float RenderDelta, float XOffset, float YOffset)
{
	if (!bDrawingMatchOverlay)
	{
		Super::DrawPlayer(Index, PS, RenderDelta, XOffset, YOffset);
		return;
	}
	if (!IsValid(PS) || PS->bIsInactive) { return; }
	const float X = MatchOverlayX(XOffset);
	const float Width = MatchOverlayWidth();
	const FVector4 Bounds(RenderPosition.X + X * RenderScale, RenderPosition.Y + YOffset * RenderScale,
		RenderPosition.X + (X + Width) * RenderScale, RenderPosition.Y + (YOffset + CellHeight) * RenderScale);
	if (bMatchInteractive)
	{
		FMatchHitRow Hit; Hit.Player = PS; Hit.Bounds = Bounds; MatchHitRows.Add(Hit);
	}
	const bool bSelected = UTPlayerOwner->LastSpectatedPlayerId == PS->SpectatingID;
	const bool bHovered = bMatchInteractive && UTPlayerOwner->bShowMouseCursor && ContainsPoint(Bounds, MatchMousePosition);
	FLinearColor TeamColor = PS->Team ? PS->Team->TeamColor : FLinearColor(0.3f, 0.3f, 0.3f);
	TeamColor.R *= 0.5f; TeamColor.G *= 0.5f; TeamColor.B *= 0.5f;
	DrawTexture(UTHUDOwner->ScoreboardAtlas, X, YOffset, Width, 0.95f * CellHeight, 149, 138, 32, 32,
		bSelected ? 0.8f : (bHovered ? 0.65f : 0.48f), TeamColor);
	if (bSelected)
	{
		DrawTexture(UTHUDOwner->ScoreboardAtlas, X, YOffset, 3.f, 0.95f * CellHeight, 149, 138, 32, 32, 1.f, FLinearColor::White);
	}
	const float Y = YOffset + ColumnY;
	AUTCharacter* Character = PS->GetUTCharacter();
	const bool bAlive = IsValid(Character) && Character->Health > 0;
	const FLinearColor TextColor = bAlive ? FLinearColor::White : FLinearColor(0.65f, 0.65f, 0.65f);
	const FMatchRow& Row = GetMatchRow(PS);
	DrawText(Row.SpectatingLabel, X + 17.f, Y, UTHUDOwner->TinyFont, Row.SpectatingScale,
		1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
	DrawText(Row.DisplayName, X + 34.f, Y, SlideOutFont, Row.NameScale,
		1.f, TextColor, ETextHorzPos::Left, ETextVertPos::Center);
	if (IsValid(PS->CarriedObject) && (!bMatchInstagib || bAlive))
	{
		const FLinearColor FlagColor = PS->CarriedObject->Team ? PS->CarriedObject->Team->TeamColor : FLinearColor::White;
		DrawTexture(FlagIcon.Texture, X + 190.f, YOffset + 2.f, 22.f, 22.f,
			FlagIcon.U, FlagIcon.V, FlagIcon.UL, FlagIcon.VL, 1.f, FlagColor);
	}
	else if (bAlive && Character->GetWeaponClass())
	{
		const AUTWeapon* Weapon = Character->GetWeaponClass()->GetDefaultObject<AUTWeapon>();
		if (Weapon && Weapon->WeaponBarSelectedUVs.UL > 0.f && Weapon->WeaponBarSelectedUVs.VL > 0.f)
		{
			const FTextureUVs& UV = Weapon->WeaponBarSelectedUVs;
			const float IconScale = FMath::Min(28.f / UV.UL, 20.f / UV.VL);
			DrawTexture(WeaponAtlas, X + 188.f, Y - 0.5f * IconScale * UV.VL,
				IconScale * UV.UL, IconScale * UV.VL, UV.U + UV.UL, UV.V, -UV.UL, UV.VL, 1.f, FLinearColor::White);
		}
	}
	if (bAlive)
	{
		if (Character->GetWeaponOverlayFlags() != 0)
		{
			DrawTexture(UDamageHUDIcon.Texture, X + 216.f, Y - 6.f, 12.f, 12.f,
				UDamageHUDIcon.U, UDamageHUDIcon.V, UDamageHUDIcon.UL, UDamageHUDIcon.VL, 1.f, FLinearColor::White);
		}
		if (!bMatchInstagib)
		{
			DrawMatchCell(FText::AsNumber(Character->Health), X + Size.X * ColumnHeaderScoreX, Y, 46.f, FLinearColor(0.5f, 1.f, 0.5f));
			DrawMatchCell(FText::AsNumber(Character->GetArmorAmount()), X + Size.X * ColumnHeaderArmor, Y, 46.f, FLinearColor(1.f, 1.f, 0.5f));
		}
	}
	else
	{
		const bool bRespawning = MatchOverlayMode == ENCSlideOutMatchMode::Wipeout && PS->bOutOfLives
			&& PS->RespawnWaitTime > 0.f && PS->RespawnTime > 0.f;
		const FString State = bRespawning ? FString::Printf(TEXT("%ds"), FMath::CeilToInt(PS->RespawnTime)) : (PS->bOutOfLives ? TEXT("OUT") : TEXT("DEAD"));
		DrawMatchCell(FText::FromString(State), X + (bMatchInstagib ? 208.f : Size.X * 0.85f), Y,
			bMatchInstagib ? 40.f : 86.f, TextColor);
	}
	for (int32 Col = 0; Col < MatchOverlayColumnCount(); ++Col)
	{
		DrawText(Row.Cells[Col], X + MatchOverlayPlayerWidth() + (Col + 0.5f) * 52.f, Y, UTHUDOwner->TinyFont,
			Row.TextScales[Col], 1.f, TextColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
	if (bShowingStats && bSelected)
	{
		ShowSelectedPlayerStats(PS, RenderDelta, X + Width + 16.f, YOffset);
	}
}

void UNCPlusSpectatorSlideOut::TrackMouseMovement(FVector2D Position)
{
	MatchMousePosition = Position;
	Super::TrackMouseMovement(Position);
}

void UNCPlusSpectatorSlideOut::SetMouseInteractive(bool bNewInteractive)
{
	bMatchInteractive = bNewInteractive;
	if (!bNewInteractive) { MatchHitRows.Reset(); }
	Super::SetMouseInteractive(bNewInteractive);
}

bool UNCPlusSpectatorSlideOut::MouseClick(FVector2D Position)
{
	if (Super::MouseClick(Position)) { return true; }
	if (!bMatchInteractive || !CanUseMatchOverlay() || !UTPlayerOwner->bShowMouseCursor
		|| MatchWorld.Get() != GetWorld()) { return false; }
	for (const FMatchHitRow& Row : MatchHitRows)
	{
		AUTPlayerState* PS = Row.Player.Get();
		if (PS && !PS->bIsInactive && UTGameState->PlayerArray.Contains(PS)
			&& ContainsPoint(Row.Bounds, Position) && UTGameState->CanSpectate(UTPlayerOwner, PS))
		{
			if (UTPlayerOwner->LastSpectatedPlayerId == PS->SpectatingID) { ToggleStats(); }
			else { UTPlayerOwner->ViewPlayerNum(UTGameState->bTeamGame ? PS->SpectatingIDTeam : PS->SpectatingID, PS->GetTeamNum()); }
			return true;
		}
	}
	return false;
}

void UNCPlusSpectatorSlideOut::DrawWeaponStats(AUTPlayerState* PS, float DeltaTime, float& YPos, float XOffset, float ScoreWidth, float MaxHeight, const FStatsFontInfo& StatsFontInfo)
{
	// Defer to stock behaviour when we have nothing better to show:
	//   - Passthrough: safety default.
	//   - CTFAuto on a NON-instagib match: normal CTF maps have real weapon
	//     pickups, so the stock map enumeration is correct there.
	// (&& short-circuits so IsInstagibMatch's actor walk only runs for CTFAuto.)
	if (WeaponListMode == ENCSlideOutWeaponMode::Passthrough ||
		(MatchOverlayMode == ENCSlideOutMatchMode::Wipeout && !IsWipeoutMatch(UTGameState)) ||
	    (WeaponListMode == ENCSlideOutWeaponMode::CTFAuto && !IsInstagibMatch()))
	{
		Super::DrawWeaponStats(PS, DeltaTime, YPos, XOffset, ScoreWidth, MaxHeight, StatsFontInfo);
		return;
	}

	if (!PS || !IsValid(UTHUDOwner))
	{
		return;
	}

	const FString PlayerId = PS->UniqueId.IsValid()
		? PS->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
	const TArray<FNCSlideRow>* Rows = GetLoadoutRows(PS, PlayerId);
	const bool bUseInstagibFallback = (!Rows || Rows->Num() == 0)
		&& WeaponListMode == ENCSlideOutWeaponMode::CTFAuto;

	// Instagib safety: if we never saw a pawn for this player (joined dead /
	// pawn not yet replicated) the inventory read is empty — still show the one
	// known accuracy weapon so the iCTF panel is never blank.
	if ((!Rows || Rows->Num() == 0) && !bUseInstagibFallback)
	{
		// Elim player we never saw alive — nothing meaningful to list.
		return;
	}

	DrawAccuracyHeader(XOffset, YPos, ScoreWidth, StatsFontInfo);

	auto DrawRow = [&](const FNCSlideRow& R)
	{
		int32 Hits = 0;
		int32 Shots = 0;
		ResolveAccuracy(PS, PlayerId, R.HitsStat, R.ShotsStat, Hits, Shots);
		const float Accuracy = (Shots > 0) ? FMath::Min(100.f * float(Hits) / float(Shots), 100.f) : 0.f;
		// Kills/Deaths passed as -1 -> DrawWeaponStatsLine suppresses those two
		// columns (per-weapon kills/deaths are not replicated to spectators).
		DrawWeaponStatsLine(R.Label, -1, -1, Shots, Accuracy, DeltaTime, XOffset, YPos, StatsFontInfo, ScoreWidth, false);
	};
	if (bUseInstagibFallback)
	{
		DrawRow(InstagibFallbackRow);
	}
	else
	{
		for (const FNCSlideRow& R : *Rows)
		{
			DrawRow(R);
		}
	}
}

const TArray<UNCPlusSpectatorSlideOut::FNCSlideRow>* UNCPlusSpectatorSlideOut::GetLoadoutRows(AUTPlayerState* PS, const FString& PlayerId)
{
	if (!PS)
	{
		return nullptr;
	}
	UWorld* World = IsValid(UTHUDOwner) ? UTHUDOwner->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	if (CachedLoadoutWorld.Get() != World)
	{
		CachedLoadoutWorld = World;
		CachedLoadoutByPlayer.Reset();
		LoadoutScratchRows.Reset();
	}

	FNCSlideLoadoutCache& Cache = CachedLoadoutByPlayer.FindOrAdd(PlayerId);

	static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));

	// Read the player's ACTUAL carried weapons = the real loadout (BP-defined,
	// no hardcoded list). Hits/ShotsStatsName are CDO defaults, client-readable
	// (same source NCPlusHUDWidget_Accuracy uses for the held weapon). The
	// NC+ weapon swaps (AUTPlusShockRifle etc.) and the stock-modified
	// guns all inherit/set these, so each row resolves to the right stat.
	AUTCharacter* Char = PS->GetUTCharacter();
	const float Now = World->GetTimeSeconds();
	if (IsValid(Char) && (Cache.Character.Get() != Char || Now >= Cache.NextRefreshTime))
	{
		Cache.Character = Char;
		Cache.NextRefreshTime = Now + 0.10f;
		LoadoutScratchRows.Reset();
		for (TInventoryIterator<AUTWeapon> It(Char); It; ++It)
		{
			AUTWeapon* W = *It;
			// IsValid guard: TInventoryIterator can yield a stale ptr mid-mutation.
			if (!IsValid(W) || W->ShotsStatsName == NAME_None)
			{
				continue;   // melee / translocator: no accuracy to show
			}
			FNCSlideRow R;
			R.Label    = W->DisplayName;
			R.HitsStat = W->HitsStatsName;
			// Link beam: hits tick per damage chunk, but stock ShotsStatsName
			// (NAME_LinkShots) only ticks per trigger pull. Use the per-refire
			// LinkBeamShots the replicator stores (matches NCPlusHUDWidget_Accuracy).
			R.ShotsStat = (W->HitsStatsName == NAME_LinkHits) ? NAME_LinkBeamShots : W->ShotsStatsName;
			LoadoutScratchRows.Add(R);
		}
		if (LoadoutScratchRows.Num() > 0)
		{
			Swap(Cache.Rows, LoadoutScratchRows);
		}
	}

	return &Cache.Rows;
}

void UNCPlusSpectatorSlideOut::DrawAccuracyHeader(float XOffset, float& YPos, float ScoreWidth, const FStatsFontInfo& StatsFontInfo)
{
	// Mirror the stock header column offsets (UTHUDWidget_SpectatorSlideOut.cpp
	// DrawWeaponStats) but only the two columns we populate.
	UFont* HeaderFont = UTHUDOwner->TinyFont;
	DrawText(NSLOCTEXT("NCSlideOut", "Shots", "Shots"), XOffset + (ShotsColumn - 0.02f) * ScoreWidth, YPos, HeaderFont, 1.f, 1.f, FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Top);
	DrawText(NSLOCTEXT("NCSlideOut", "Accuracy", "Accuracy"), XOffset + (AccuracyColumn - 0.03f) * ScoreWidth, YPos, HeaderFont, 1.f, 1.f, FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Top);
	YPos += StatsFontInfo.TextHeight;
}

void UNCPlusSpectatorSlideOut::ResolveAccuracy(AUTPlayerState* PS, const FString& PlayerId, FName HitsStat, FName ShotsStat, int32& OutHits, int32& OutShots) const
{
	OutHits = 0;
	OutShots = 0;
	if (!PS || (HitsStat == NAME_None && ShotsStat == NAME_None))
	{
		return;
	}

	// StatsData first (correct on a listen server / standalone where the host has
	// authority), then the per-weapon replicator when StatsData is empty
	// (dedicated-server spectators). Mirrors NCPlusHUDWidget_Accuracy.
	OutHits  = PS->GetStatsValue(HitsStat);
	OutShots = PS->GetStatsValue(ShotsStat);
	if (OutHits == 0 && OutShots == 0)
	{
		if (ANCAccuracyStatsReplicator* Rep = GetAccuracyReplicator())
		{
			OutHits  = Rep->GetHitsForPlayer(PlayerId, HitsStat);
			OutShots = Rep->GetShotsForPlayer(PlayerId, ShotsStat);
		}
	}
}

ANCAccuracyStatsReplicator* UNCPlusSpectatorSlideOut::GetAccuracyReplicator() const
{
	if (!IsValid(UTHUDOwner))
	{
		return nullptr;
	}
	UWorld* World = UTHUDOwner->GetWorld();
	if (!World)
	{
		return nullptr;
	}
	if (CachedAccuracyWorld.Get() != World)
	{
		CachedAccuracyWorld = World;
		CachedAccuracyReplicator = nullptr;
		NextAccuracyReplicatorRetryTime = 0.f;
	}
	if (CachedAccuracyReplicator.IsValid())
	{
		return CachedAccuracyReplicator.Get();
	}
	const float Now = World->GetTimeSeconds();
	if (Now < NextAccuracyReplicatorRetryTime && NextAccuracyReplicatorRetryTime - Now <= 1.f)
	{
		return nullptr;
	}
	NextAccuracyReplicatorRetryTime = Now + 1.f;
	for (TActorIterator<ANCAccuracyStatsReplicator> It(World); It; ++It)
	{
		CachedAccuracyReplicator = *It;
		return *It;
	}
	return nullptr;
}

bool UNCPlusSpectatorSlideOut::IsInstagibMatch() const
{
	ACTFStatsReplicator* Rep = GetCTFStatsReplicator();
	return Rep && Rep->bIsInstagibMatch;
}

ACTFStatsReplicator* UNCPlusSpectatorSlideOut::GetCTFStatsReplicator() const
{
	if (!IsValid(UTHUDOwner))
	{
		return nullptr;
	}
	UWorld* World = UTHUDOwner->GetWorld();
	if (!World)
	{
		return nullptr;
	}
	if (CachedInstagibWorld.Get() != World)
	{
		CachedInstagibWorld = World;
		CachedCTFStatsReplicator = nullptr;
		NextInstagibReplicatorRetryTime = 0.f;
	}
	if (CachedCTFStatsReplicator.IsValid())
	{
		return CachedCTFStatsReplicator.Get();
	}
	const float Now = World->GetTimeSeconds();
	if (Now < NextInstagibReplicatorRetryTime && NextInstagibReplicatorRetryTime - Now <= 1.f)
	{
		return nullptr;
	}
	NextInstagibReplicatorRetryTime = Now + 1.f;
	for (TActorIterator<ACTFStatsReplicator> It(World); It; ++It)
	{
		CachedCTFStatsReplicator = *It;
		return *It;
	}
	return nullptr;
}
