// NCPlusHUDWidget_Accuracy.cpp — live weapon accuracy readout.
//
// Visibility is layout-gated: the widget hides itself unless the live layout
// has an "accuracy" entry. This means it's truly opt-in for HUDs that include
// it in HudWidgetClasses but don't want it on by default (ElimPlus, Wipeout,
// ShockDom, NCPlusCTF, NCLeagueDuel). NCShaftArenaHUD seeds a default layout
// entry in BeginPlay so the widget shows on that mode out of the box.
//
// Source weapon is configurable via the layout's per-element extras key
// "weapon". Recognized values:
//   ""               (default) — current held weapon
//   "current"        same as default
//   "linkgun" / "shock" / "shockrifle" / "rocket" / "flak" / "minigun" /
//   "sniper" / "sniperrifle" / "lightning" / "enforcer" / "biorifle" /
//   "redeemer" / "instagib"
// Unknown values fall back to current-weapon behavior.

#include "NCPlusHUDWidget_Accuracy.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "NCShaftArenaHUD.h"          // shaft-only default-on gate in ShouldDraw
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"
#include "StatNames.h"
#include "NCPlusHUDLayout.h"
#include "NCAccuracyStatsReplicator.h"

namespace
{
	static const FName NAME_Accuracy(TEXT("accuracy"));
	static const FName NAME_Weapon(TEXT("weapon"));
	static const FName NAME_Opacity(TEXT("opacity"));
	static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));
	static const float StatsRefreshInterval = 0.05f; // 20 Hz local; replicated data remains 2 Hz.

	struct FNCAccWeaponInfo
	{
		FName HitsStat;
		FName ShotsStat;
		const TCHAR* DisplayLabel;   // small label above the percentage
	};

	/** Map a layout "weapon" extras value to (HitsStat, ShotsStat, label).
	 *  Returns nullptr for "" / "current" / unknown — caller should use the
	 *  held weapon's HitsStatsName/ShotsStatsName instead. */
	const FNCAccWeaponInfo* ResolveWeaponInfo(const FString& Key)
	{
		if (Key.IsEmpty() || Key.Equals(TEXT("current"), ESearchCase::IgnoreCase))
		{
			return nullptr;
		}

		struct FEntry { const TCHAR* Key; FNCAccWeaponInfo Info; };
		static const FEntry Table[] = {
			{ TEXT("linkgun"),     { NAME_LinkHits,           NAME_LinkShots,           TEXT("LG")    } },
			{ TEXT("link"),        { NAME_LinkHits,           NAME_LinkShots,           TEXT("LG")    } },
			{ TEXT("shockrifle"),  { NAME_ShockRifleHits,     NAME_ShockRifleShots,     TEXT("Shock") } },
			{ TEXT("shock"),       { NAME_ShockRifleHits,     NAME_ShockRifleShots,     TEXT("Shock") } },
			{ TEXT("rocket"),      { NAME_RocketHits,         NAME_RocketShots,         TEXT("RL")    } },
			{ TEXT("flak"),        { NAME_FlakHits,           NAME_FlakShots,           TEXT("Flak")  } },
			{ TEXT("minigun"),     { NAME_MinigunHits,        NAME_MinigunShots,        TEXT("Mini")  } },
			{ TEXT("sniperrifle"), { NAME_SniperHits,         NAME_SniperShots,         TEXT("Sniper")} },
			{ TEXT("sniper"),      { NAME_SniperHits,         NAME_SniperShots,         TEXT("Sniper")} },
			{ TEXT("lightning"),   { NAME_LightningRifleHits, NAME_LightningRifleShots, TEXT("Light") } },
			{ TEXT("enforcer"),    { NAME_EnforcerHits,       NAME_EnforcerShots,       TEXT("Enf")   } },
			{ TEXT("biorifle"),    { NAME_BioRifleHits,       NAME_BioRifleShots,       TEXT("Bio")   } },
			{ TEXT("bio"),         { NAME_BioRifleHits,       NAME_BioRifleShots,       TEXT("Bio")   } },
			{ TEXT("redeemer"),    { NAME_RedeemerHits,       NAME_RedeemerShots,       TEXT("Red")   } },
			{ TEXT("instagib"),    { NAME_InstagibHits,       NAME_InstagibShots,       TEXT("Insta") } },
		};
		for (const FEntry& E : Table)
		{
			if (Key.Equals(E.Key, ESearchCase::IgnoreCase)) return &E.Info;
		}
		return nullptr;
	}
}

UNCPlusHUDWidget_Accuracy::UNCPlusHUDWidget_Accuracy(const FObjectInitializer& OI)
	: Super(OI)
	, CachedLayoutRevision(MAX_uint32)
	, bCachedLayoutPresent(false)
	, bCachedLayoutHidden(false)
	, bCachedPinnedMode(false)
	, CachedElementScale(1.f)
	, CachedOpacity(1.f)
	, CachedPinnedHitsStat(NAME_None)
	, CachedPinnedShotsStat(NAME_None)
	, CachedBigFont(nullptr)
	, CachedSmallFont(nullptr)
	, CachedHitsStat(NAME_None)
	, CachedShotsStat(NAME_None)
	, bCachedPlayerIdIsUnique(false)
	, NextStatsRefreshTime(0.f)
	, CachedHits(MIN_int32)
	, CachedShots(MIN_int32)
	, bCachedDisplayValid(false)
	, CachedPercentColor(FLinearColor::White)
{
	// Stock position: bottom-right corner, flush with the bottom edge so live
	// aim feedback sits in the same eye-region as HP/Armor/Ammo. Layout
	// override (Extras + nchud) takes precedence; this is what
	// CaptureWidgetDefaults snapshots for "No override → restore stock".
	Position           = FVector2D(-20.f, 0.f);
	Size               = FVector2D(220.f, 80.f);
	ScreenPosition     = FVector2D(1.0f, 1.0f);    // BottomRight anchor
	Origin             = FVector2D(1.0f, 1.0f);    // pivot at widget's bottom-right
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

void UNCPlusHUDWidget_Accuracy::RefreshLayoutCache()
{
	const uint32 Revision = FNCPlusHUDLayout::GetLiveRevision();
	if (CachedLayoutRevision == Revision) return;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(NAME_Accuracy);
	bCachedLayoutPresent = E != nullptr;
	bCachedLayoutHidden = E && E->bHidden;
	bCachedPinnedMode = false;
	CachedPinnedHitsStat = NAME_None;
	CachedPinnedShotsStat = NAME_None;
	CachedSmallLabel = FText::GetEmpty();
	CachedSmallLabelString.Reset();
	CachedLabelMeasure.Font = nullptr;
	CachedElementScale = E ? FMath::Clamp(E->Scale, 0.25f, 4.f) : 1.f;
	CachedOpacity = E ? FMath::Clamp(E->GetExtraFloat(NAME_Opacity, 1.f), 0.f, 1.f) : 1.f;

	if (E)
	{
		if (const FNCAccWeaponInfo* Info = ResolveWeaponInfo(E->GetExtra(NAME_Weapon)))
		{
			bCachedPinnedMode = true;
			CachedPinnedHitsStat = Info->HitsStat;
			CachedPinnedShotsStat = Info->ShotsStat;
			CachedSmallLabel = FText::FromString(Info->DisplayLabel);
			CachedSmallLabelString = Info->DisplayLabel;
		}
	}

	if (IsValid(UTHUDOwner))
	{
		UFont* BigFallback = UTHUDOwner->LargeFont ? UTHUDOwner->LargeFont : UTHUDOwner->MediumFont;
		UFont* SmallFallback = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
		CachedBigFont = NCPlusHUDFonts::Resolve(NAME_Accuracy, UTHUDOwner, BigFallback);
		CachedSmallFont = NCPlusHUDFonts::Resolve(NAME_Accuracy, UTHUDOwner, SmallFallback);
	}

	CachedSourceWeapon.Reset();
	CachedHitsStat = NAME_None;
	CachedShotsStat = NAME_None;
	NextStatsRefreshTime = 0.f;
	bCachedDisplayValid = false;
	CachedLayoutRevision = Revision;
}

bool UNCPlusHUDWidget_Accuracy::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!IsValid(PS) || PS->bOnlySpectator) return false;

	// Opt-in: the widget renders only when the user has placed it via nchud —
	// EXCEPT shaft arena, where accuracy is on by default. That default is a MODE
	// CHECK here (2026-07-01), not a layout-entry seed: the old NCShaftArenaHUD::
	// BeginPlay seed wrote into the shared live map, and the nchud editor/drag
	// overlay auto-saves the whole map on close — playing shaft once + touching the
	// editor baked a visible accuracy entry into HUDLayout.json for EVERY mode.
	// With no entry, Draw falls back to sane defaults (scale 1, Large font, ctor
	// bottom-right position). bHidden in an entry honors the editor's eye toggle.
	RefreshLayoutCache();
	if (!bCachedLayoutPresent)
	{
		return Cast<ANCShaftArenaHUD>(UTHUDOwner) != nullptr;
	}
	return !bCachedLayoutHidden;
}

void UNCPlusHUDWidget_Accuracy::Draw_Implementation(float DeltaTime)
{
	// Defensive: every dereference along the chain can be invalidated between
	// ShouldDraw and Draw (controller swap on map travel, character respawn
	// mid-tick, pawn destroyed by gamemode hook). IsValid catches both null
	// and pending-kill objects. Crash 0x298 access-violation observed in the
	// wild was a stale pointer dereference here — keep these guards.
	if (!Canvas) return;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return;
	AUTPlayerController* PC = UTHUDOwner->UTPlayerOwner;
	AUTPlayerState* PS = PC->UTPlayerState;
	if (!IsValid(PS)) return;
	RefreshLayoutCache();
	if (!CachedBigFont || !CachedSmallFont || CachedOpacity <= 0.001f) return;

	// Resolve stat source. Two modes:
	//   Pinned weapon (layout Extras["weapon"] = "linkgun" / "shockrifle" /
	//     etc.): stat NAMEs come from the resolver table; always render so
	//     career-style "show me my LG accuracy" works during weapon switches
	//     and even while spectating death.
	//   Current weapon (default / "current"): pull stat NAMEs off the held
	//     AUTWeapon. If there's no live character or weapon yet (just spawned,
	//     mid-respawn, holding a weapon that doesn't track shots like the
	//     translocator and we're being strict), bail — nothing useful to show.
	FName HitsStat = CachedPinnedHitsStat;
	FName ShotsStat = CachedPinnedShotsStat;
	AUTWeapon* SourceWeapon = nullptr;

	if (!bCachedPinnedMode)
	{
		// Use GetPawn() (UPROPERTY, GC-nulled on destroy) rather than
		// GetUTCharacter() — the latter goes through a Cast that can chase a
		// stale ptr through engine code (root cause of the 0x298 crash before
		// these guards landed).
		APawn* Pawn = PC->GetPawn();
		if (!IsValid(Pawn)) return;
		AUTCharacter* Char = Cast<AUTCharacter>(Pawn);
		if (!IsValid(Char)) return;
		SourceWeapon = Char->GetWeapon();
		if (!IsValid(SourceWeapon)) return;
		HitsStat  = SourceWeapon->HitsStatsName;
		ShotsStat = SourceWeapon->ShotsStatsName;
		if (HitsStat == NAME_None || ShotsStat == NAME_None) return;
	}

	// Link-beam accuracy fix: NAME_LinkHits ticks per damage chunk landed,
	// but stock NAME_LinkShots only ticks on trigger pull (sustained beam =
	// 1 shot, many hits → ratio explodes). UTWeap_LinkGun_Plus increments
	// NAME_LinkBeamShots per beam-mode ConsumeAmmo (= 1 per refire tick),
	// giving Quake-style per-tick accuracy when paired with NAME_LinkHits.
	// Swap whenever we're displaying link accuracy.
	if (HitsStat == NAME_LinkHits)
	{
		ShotsStat = NAME_LinkBeamShots;
	}

	const bool bSourceChanged = CachedSourceWeapon.Get() != SourceWeapon
		|| CachedHitsStat != HitsStat || CachedShotsStat != ShotsStat
		|| CachedPlayerState.Get() != PS;
	if (bSourceChanged)
	{
		CachedSourceWeapon = SourceWeapon;
		CachedHitsStat = HitsStat;
		CachedShotsStat = ShotsStat;
		NextStatsRefreshTime = 0.f;
	}

	const float Now = GetWorld()->GetTimeSeconds();
	if (!bCachedDisplayValid || Now >= NextStatsRefreshTime)
	{
		RefreshDisplayCache(PS, HitsStat, ShotsStat, Now);
	}
	if (!bCachedDisplayValid) return;

	// Optional small label above the number — only when a specific weapon is
	// pinned, so the user knows what they're looking at. Current-weapon mode
	// shows just the number since the held weapon is obvious from the rest of
	// the HUD.
	float NumberY = 0.f;
	if (!CachedSmallLabel.IsEmpty())
	{
		DrawCachedText(CachedSmallLabel, CachedSmallLabelString, CachedLabelMeasure,
			Size.X * 0.5f, 0.f, CachedSmallFont, RenderScale * CachedElementScale,
			CachedOpacity, FLinearColor(1.f, 1.f, 1.f, 0.75f));
		NumberY = 14.f * CachedElementScale;
	}

	DrawCachedText(CachedPercentText, CachedPercentString, CachedPercentMeasure,
		Size.X * 0.5f, NumberY, CachedBigFont, RenderScale * CachedElementScale,
		CachedOpacity, CachedPercentColor);

	DrawCachedText(CachedSubText, CachedSubString, CachedSubMeasure,
		Size.X * 0.5f, NumberY + 48.f * CachedElementScale, CachedSmallFont,
		RenderScale * CachedElementScale, CachedOpacity, FLinearColor(1.f, 1.f, 1.f, 0.85f));
}

void UNCPlusHUDWidget_Accuracy::DrawCachedText(const FText& Text, const FString& String,
	FTextMeasureCache& Measure, float X, float Y, UFont* Font, float TextScale,
	float DrawOpacity, const FLinearColor& DrawColor)
{
	if (!Canvas || !Font || Text.IsEmpty()) return;
	if (Measure.Font != Font)
	{
		float XL = 0.f;
		float YL = 0.f;
		Canvas->StrLen(Font, String, XL, YL);
		Measure.Font = Font;
		Measure.Size = FVector2D(XL, YL);
	}
	if (bScaleByDesignedResolution)
	{
		X *= RenderScale;
		Y *= RenderScale;
	}
	const float FinalScale = bScaleByDesignedResolution ? RenderScale * TextScale : TextScale;
	const FVector2D DrawPos(RenderPosition.X + X - Measure.Size.X * FinalScale * 0.5f,
		RenderPosition.Y + Y);
	FLinearColor Color = DrawColor;
	Color.A = Opacity * DrawOpacity * (bIgnoreHUDOpacity ? 1.f : UTHUDOwner->WidgetOpacity);
	FCanvasTextItem TextItem(DrawPos, Text, Font, Color);
	TextItem.Scale = FVector2D(FinalScale, FinalScale);
	Canvas->DrawItem(TextItem);
}

void UNCPlusHUDWidget_Accuracy::RefreshDisplayCache(AUTPlayerState* PS,
	FName HitsStat, FName ShotsStat, float Now)
{
	if (!IsValid(PS)) return;
	const bool bHasUniqueId = PS->UniqueId.IsValid();
	if (CachedPlayerState.Get() != PS || (bHasUniqueId && !bCachedPlayerIdIsUnique))
	{
		CachedPlayerState = PS;
		bCachedPlayerIdIsUnique = bHasUniqueId;
		CachedPlayerId = bHasUniqueId
			? PS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
	}

	// AUTPlayerState::StatsData is server-only on dedicated clients. Query it
	// first for listen/local play, then use one combined replicated-array lookup.
	int32 Hits = PS->GetStatsValue(HitsStat);
	int32 Shots = PS->GetStatsValue(ShotsStat);
	if (Hits == 0 && Shots == 0)
	{
		if (ANCAccuracyStatsReplicator* Rep = GetAccuracyReplicator())
		{
			Rep->GetAccuracyForPlayer(CachedPlayerId, HitsStat, ShotsStat, Hits, Shots);
		}
	}

	if (!bCachedDisplayValid || Hits != CachedHits || Shots != CachedShots)
	{
		CachedHits = Hits;
		CachedShots = Shots;
		const float RawPct = Shots > 0 ? float(Hits) / float(Shots) * 100.f : 0.f;
		const float Pct = FMath::Min(RawPct, 100.f);
		CachedPercentColor = Shots == 0 ? FLinearColor(1.f, 1.f, 1.f, 0.6f)
			: Pct >= 50.f ? FLinearColor(0.30f, 1.0f, 0.40f, 1.f)
			: Pct >= 30.f ? FLinearColor(1.0f, 0.95f, 0.35f, 1.f)
			: FLinearColor(1.0f, 0.45f, 0.45f, 1.f);
		CachedPercentText = Shots > 0
			? FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct)))
			: FText::FromString(TEXT("--"));
		CachedSubText = FText::FromString(FString::Printf(TEXT("%d / %d"), Hits, Shots));
		CachedPercentString = CachedPercentText.ToString();
		CachedSubString = CachedSubText.ToString();
		CachedPercentMeasure.Font = nullptr;
		CachedSubMeasure.Font = nullptr;
	}

	bCachedDisplayValid = true;
	NextStatsRefreshTime = Now + StatsRefreshInterval;
}

ANCAccuracyStatsReplicator* UNCPlusHUDWidget_Accuracy::GetAccuracyReplicator() const
{
	if (CachedAccuracyReplicator.IsValid())
	{
		return CachedAccuracyReplicator.Get();
	}
	if (!IsValid(UTHUDOwner)) return nullptr;
	UWorld* World = UTHUDOwner->GetWorld();
	if (!World) return nullptr;
	for (TActorIterator<ANCAccuracyStatsReplicator> It(World); It; ++It)
	{
		CachedAccuracyReplicator = *It;
		return *It;
	}
	return nullptr;
}
