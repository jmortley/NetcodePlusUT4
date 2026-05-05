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
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "Engine/Canvas.h"
#include "StatNames.h"
#include "NCPlusHUDLayout.h"

namespace
{
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

bool UNCPlusHUDWidget_Accuracy::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!IsValid(PS) || PS->bOnlySpectator) return false;

	// Opt-in: the widget renders only when the user has placed it via nchud
	// (or a mode that wants it on by default has seeded a layout entry — see
	// NCShaftArenaHUD::BeginPlay). bHidden inside the entry honors the eye
	// toggle in the editor.
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("accuracy"));
	if (!E) return false;
	if (E->bHidden) return false;
	return true;
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

	// Resolve stat source. Two modes:
	//   Pinned weapon (layout Extras["weapon"] = "linkgun" / "shockrifle" /
	//     etc.): stat NAMEs come from the resolver table; always render so
	//     career-style "show me my LG accuracy" works during weapon switches
	//     and even while spectating death.
	//   Current weapon (default / "current"): pull stat NAMEs off the held
	//     AUTWeapon. If there's no live character or weapon yet (just spawned,
	//     mid-respawn, holding a weapon that doesn't track shots like the
	//     translocator and we're being strict), bail — nothing useful to show.
	FName HitsStat = NAME_None;
	FName ShotsStat = NAME_None;
	FString SmallLabel;
	bool bPinnedMode = false;
	if (const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("accuracy")))
	{
		const FString WeaponKey = E->GetExtra(TEXT("weapon"));
		if (const FNCAccWeaponInfo* Info = ResolveWeaponInfo(WeaponKey))
		{
			HitsStat   = Info->HitsStat;
			ShotsStat  = Info->ShotsStat;
			SmallLabel = Info->DisplayLabel;
			bPinnedMode = true;
		}
	}

	if (!bPinnedMode)
	{
		// Use GetPawn() (UPROPERTY, GC-nulled on destroy) rather than
		// GetUTCharacter() — the latter goes through a Cast that can chase a
		// stale ptr through engine code (root cause of the 0x298 crash before
		// these guards landed).
		APawn* Pawn = PC->GetPawn();
		if (!IsValid(Pawn)) return;
		AUTCharacter* Char = Cast<AUTCharacter>(Pawn);
		if (!IsValid(Char)) return;
		AUTWeapon* Weapon = Char->GetWeapon();
		if (!IsValid(Weapon)) return;
		HitsStat  = Weapon->HitsStatsName;
		ShotsStat = Weapon->ShotsStatsName;
		if (HitsStat == NAME_None || ShotsStat == NAME_None) return;
	}

	const int32 Hits  = PS->GetStatsValue(HitsStat);
	const int32 Shots = PS->GetStatsValue(ShotsStat);
	const float Pct = (Shots > 0) ? float(Hits) / float(Shots) * 100.f : 0.f;

	// Color tier: green ≥ 50, yellow ≥ 30, red below.
	const FLinearColor PctColor = (Shots == 0)   ? FLinearColor(1.f, 1.f, 1.f, 0.6f)
	                            : (Pct >= 50.f) ? FLinearColor(0.30f, 1.0f, 0.40f, 1.f)
	                            : (Pct >= 30.f) ? FLinearColor(1.0f, 0.95f, 0.35f, 1.f)
	                            : FLinearColor(1.0f, 0.45f, 0.45f, 1.f);

	UFont* BigFont  = UTHUDOwner->LargeFont ? UTHUDOwner->LargeFont : UTHUDOwner->MediumFont;
	UFont* SmallFnt = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
	// Per-element font override (Phase 3.8) — applied to both percentage
	// number and the small label/sub-text.
	BigFont  = NCPlusHUDFonts::Resolve(TEXT("accuracy"), UTHUDOwner, BigFont);
	SmallFnt = NCPlusHUDFonts::Resolve(TEXT("accuracy"), UTHUDOwner, SmallFnt);
	if (!BigFont || !SmallFnt) return;

	// Optional small label above the number — only when a specific weapon is
	// pinned, so the user knows what they're looking at. Current-weapon mode
	// shows just the number since the held weapon is obvious from the rest of
	// the HUD.
	float NumberY = 0.f;
	if (!SmallLabel.IsEmpty())
	{
		DrawText(FText::FromString(SmallLabel), Size.X * 0.5f, 0.f,
			SmallFnt, RenderScale, 1.0f, FLinearColor(1.f, 1.f, 1.f, 0.75f),
			ETextHorzPos::Center, ETextVertPos::Top);
		NumberY = 14.f;
	}

	const FString PctStr = (Shots > 0)
		? FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct))
		: FString(TEXT("--"));
	DrawText(FText::FromString(PctStr), Size.X * 0.5f, NumberY,
		BigFont, RenderScale, 1.0f, PctColor,
		ETextHorzPos::Center, ETextVertPos::Top);

	const FString SubStr = FString::Printf(TEXT("%d / %d"), Hits, Shots);
	DrawText(FText::FromString(SubStr), Size.X * 0.5f, NumberY + 48.f,
		SmallFnt, RenderScale, 1.0f, FLinearColor(1.f, 1.f, 1.f, 0.85f),
		ETextHorzPos::Center, ETextVertPos::Top);
}
