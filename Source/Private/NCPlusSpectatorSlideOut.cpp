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

void UNCPlusSpectatorSlideOut::DrawWeaponStats(AUTPlayerState* PS, float DeltaTime, float& YPos, float XOffset, float ScoreWidth, float MaxHeight, const FStatsFontInfo& StatsFontInfo)
{
	// Defer to stock behaviour when we have nothing better to show:
	//   - Passthrough: safety default.
	//   - CTFAuto on a NON-instagib match: normal CTF maps have real weapon
	//     pickups, so the stock map enumeration is correct there.
	// (&& short-circuits so IsInstagibMatch's actor walk only runs for CTFAuto.)
	if (WeaponListMode == ENCSlideOutWeaponMode::Passthrough ||
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
	if (Now < NextAccuracyReplicatorRetryTime)
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
	if (!IsValid(UTHUDOwner))
	{
		return false;
	}
	UWorld* World = UTHUDOwner->GetWorld();
	if (!World)
	{
		return false;
	}
	if (CachedInstagibWorld.Get() != World)
	{
		CachedInstagibWorld = World;
		CachedCTFStatsReplicator = nullptr;
		NextInstagibReplicatorRetryTime = 0.f;
	}
	if (CachedCTFStatsReplicator.IsValid())
	{
		return CachedCTFStatsReplicator->bIsInstagibMatch;
	}
	const float Now = World->GetTimeSeconds();
	if (Now < NextInstagibReplicatorRetryTime)
	{
		return false;
	}
	NextInstagibReplicatorRetryTime = Now + 1.f;
	for (TActorIterator<ACTFStatsReplicator> It(World); It; ++It)
	{
		CachedCTFStatsReplicator = *It;
		return It->bIsInstagibMatch;
	}
	return false;
}
