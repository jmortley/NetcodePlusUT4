
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTWeaponStateZooming.h"
#include "UTPlusProj_ShockBall.h"
#include "UTPlusProj_Rocket.h"
#include "UTPlusProj_FlakShell.h"
#include "UTPlusProj_StingerShard.h"
#include "UTDamageType.h"   // FUTRadialDamageEvent (grace-buffer direct-hit damage)
#include "UTPlusWeap_RocketLauncher.h"
#include "UTDualWeapon.h"   // ApplyWeaponHideState: dual-enforcer LeftMesh
#include "UTWeaponSkin.h"
#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformTime.h"
#include "UObject/GCObject.h"
#include "ClientHitsounds.h"
#include "EngineUtils.h"
#include "UTPlayerState.h"


DEFINE_LOG_CATEGORY_STATIC(LogUTWeaponFix, Log, All);


static TAutoConsoleVariable<int32> CVarProjectileTickRate(
    TEXT("ut.ProjectileTickRate"),
    240,
    TEXT("Client-side projectile simulation rate in Hz.\n")
    TEXT("Snapped to nearest multiple of 60. Range: 60-660.\n")
    TEXT("Server always uses native 120Hz tick."),
    ECVF_Scalability
);

// Client fire-input diagnostics (held-M1 beam-stall hunt). 0=off; 1=Warning logs of every
// StartFire/StopFire/retry fork so a repro names the branch that eats the held primary beam.
// Pure logging, no behaviour change, client-side (no replication / no version bump).
static TAutoConsoleVariable<int32> CVarFireDebug(
    TEXT("ncp.FireDebug"), 0,
    TEXT("Client fire-input diagnostics: 1=log every StartFire/StopFire/retry decision (traces the held-M1 beam stall). Off by default, no behaviour change."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarSkinTiming(
    TEXT("ncp.SkinTiming"), 0,
    TEXT("Weapon-skin timing diagnostics: 1=log settings preload, SetSkin, and BringUp timing. Off by default."),
    ECVF_Default);

static FORCEINLINE bool SkinTiming()
{
    return CVarSkinTiming.GetValueOnGameThread() > 0;
}

static FORCEINLINE bool FireDbg()
{
    return CVarFireDebug.GetValueOnGameThread() > 0;
}

// "Ghost rocket" fix toggle (default OFF so the build is identical to today until
// flipped). When 1: carry the REAL held-fire state across a weapon switch instead of
// the retry-timer graduation, and clear the server's PendingFire on a genuine release.
// 0 = legacy retry-graduation + :1779-guarded server clear (today's behaviour).
// Runtime-toggleable (rcon) so ONE hub can A/B it live. See StartFire / StopFire /
// PutDown / ServerStopFireFixed. No replicated/RPC change; pairs with ncp.FireDebug.
static TAutoConsoleVariable<float> CVarMouseDebounceCap(
    TEXT("ncp.MouseDebounceCap"), 0.01f,
    TEXT("Client cap (seconds) on every weapon's MouseDebounceWindow: effective window = min(weapon BP value, this). ")
    TEXT("Default 0.01 (2026-07-17): the BP 30ms default eats REAL clicks on modern mice — optical switches (Viper V3/")
    TEXT("DeathAdder V3) cannot bounce at all, mechanical mice already debounce in firmware (4-8ms), and a fast tap-firer's ")
    TEXT("release->press gap dips under 30ms (high duty cycle; frame quantization stops padding it at high fps). Eaten ")
    TEXT("click = 'my weapon didn't fire'; a rare double-event just gets absorbed by the server ROF gate — so bias small. ")
    TEXT("10ms still covers degraded switches + keeps scroll-wheel notch trains coalescing as held intent. ")
    TEXT("0 = debounce fully off; -1 = no cap (pure BP values, pre-2026-07-17 behaviour)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarGhostFix(
    TEXT("ncp.GhostFix"), 0,
    TEXT("Ghost-rocket-on-weapon-switch fix: 1=carry real held-fire across a switch, 0=legacy. KNOWN ISSUE (live-confirmed 2026-07-05): 1 BREAKS consecutive held weapon switches (per-weapon held flag never arms on auto-fired weapons) — DO NOT ENABLE until the pawn-level v2. Off by default."),
    ECVF_Default);

static FORCEINLINE bool GhostFix()
{
    return CVarGhostFix.GetValueOnGameThread() > 0;
}

// Ghost-rocket first fix (2026-07-22): a received ServerStopFireFixed always clears that
// mode's pawn PendingFire, regardless of the weapon's current state. Restores the stock
// EndFiringSequence semantics (stock clears unconditionally, UTWeapon.cpp:817) that the
// state-gated clear in ServerStopFireFixed lost: a release whose Stop landed while this
// weapon was Unequipping/Equipping/Active left the server's PendingFire latched, and stock
// UUTWeaponStateActive::BeginState then auto-fired the NEXT weapon on equip — the
// authoritative ghost rocket/flak. Safe for hold-through-switch: every client StopFire
// path that emits this RPC has already cleared the client's own PendingFire for the mode
// (in-state EndFiringSequence, the out-of-state else-branch, or the charged Super path),
// and a held switch emits no Stop at all — so this only ever converges server state to
// what the client already applied. Independent of ncp.GhostFix (per-weapon held
// prototype, known broken, stays off). 0 = legacy state-gated clear (kill switch).
static TAutoConsoleVariable<int32> CVarStopClearsPending(
    TEXT("ncp.StopClearsPending"), 1,
    TEXT("Server honors every received Stop: 1=always clear that mode's pawn PendingFire in ServerStopFireFixed regardless of weapon state (default; fixes the authoritative ghost rocket on release-during-switch), 0=legacy state-gated clear (kill switch)."),
    ECVF_Default);

static FORCEINLINE bool StopClearsPending()
{
    return CVarStopClearsPending.GetValueOnGameThread() > 0;
}

// Held-beam stall fix (shock "hold M1, nothing comes out"). A cross-mode press landing
// inside the other mode's firing cycle used to be dropped with NO retry — input is
// edge-triggered, so a HELD button never re-fires the request and the beam stalls until
// re-press. Deterministic repro (captured [FireDbg] 2026-07-05): hold M2 to the edge of
// the 2nd core, release, immediately hold M1. 1 = schedule the same retry the same-mode
// cooldown path uses (fires at cycle end; released tap auto-cancels via StopFire's
// unconditional retry-clear). 0 = legacy drop. Client-side, no replication, no bump.
static TAutoConsoleVariable<int32> CVarCrossModeRetry(
    TEXT("ncp.CrossModeRetry"), 1,
    TEXT("Cross-mode held-fire retry (fixes the held-M1 shock beam stall after a ball): 1=queue a retry at the current cycle's end (default), 0=restore the legacy drop (kill-switch). Standalone-safe with ncp.GhostFix 0: the PutDown graduation skips cross-mode-armed retries (bCrossModeRetryArmed), so no ghost shot on a fast weapon switch."),
    ECVF_Default);

static FORCEINLINE bool CrossModeRetry()
{
    return CVarCrossModeRetry.GetValueOnGameThread() > 0;
}

static TAutoConsoleVariable<float> CVarVisualHitscanClaimTolerance(
    TEXT("ncp.VisualHitscanClaimTolerance"), 4.0f,
    TEXT("Client-only extra radius (units) when confirming an actor-capsule hit against the rendered-position capsule. Default: 4."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarVisualHitscanClaimDebug(
    TEXT("ncp.VisualHitscanClaimDebug"), 0,
    TEXT("Client rendered-capsule claim diagnostics: 0=off, 1=rejections, 2=all actor-capsule candidates."),
    ECVF_Default);

// =========================================================================
// HIT-ATTRIBUTION TELEMETRY (read-only, log-only).
// Server: one [HitAttrib] line per validated hitscan, attributing the
// acceptance route: bare rewound-capsule hit vs claim-conditional forgiveness
// (moving/stationary padding, bidirectional time search) vs miss. Client: one
// [HitAttrib.Client] line per claim-capable shot with the anchor pre-trace
// result, the rendered-vs-anchor visual offset, and the claim actually sent.
// Exists to answer which route grants "gifted" hits landing ahead of the
// rendered model at low ping. Changes no validation behavior at any setting.
// DEFAULT 1 FOR THE 328-RC DOGFOOD ONLY — flip back to 0 before the 328 live
// release (2026-08-14).
// =========================================================================
static TAutoConsoleVariable<int32> CVarHitAttribDebug(
    TEXT("ncp.HitAttribDebug"), 1,
    TEXT("Hitscan acceptance attribution: 0=off, 1=log one [HitAttrib] line per server-validated shot and one [HitAttrib.Client] line per client claim decision. Default 1 during 328-RC dogfood; revert to 0 for live."),
    ECVF_Default);

// leadUU diagnostic only: the server cannot see what the shooter's client
// actually rendered, so it approximates the rendered target position as
// GetRewindLocation(halfRTT + this many ms). The extra term stands in for the
// server->client net update interval plus client-side mesh smoothing delay.
// This is an ESTIMATE from server-side history (it ignores client interp
// filtering); calibrate against phase-2 render-latency measurements.
static TAutoConsoleVariable<float> CVarHitAttribRenderExtraMs(
    TEXT("ncp.HitAttribRenderExtraMs"), 30.0f,
    TEXT("[HitAttrib] estimated render latency beyond half-RTT (net update interval + client smoothing), ms. Diagnostic only. Default: 30."),
    ECVF_Default);

// =========================================================================
// UNCLAIMED-HIT RENDER CHECK — the "fix the server claim" gate.
// Phase-1 attribution (LA dogfood 2026-07-26) showed the gifted leading-edge
// hits arrive as route=primary-rewind-unclaimed: the shooter's client crossed
// NOTHING (claimSent=none) yet the server's under-rewound capsule was hit.
// Rather than hard-requiring claim presence (the reverted 2026-07-18 gate —
// it starved shooters whose claims are lost or unproduceable), the server
// reconstructs the claim: an UNCLAIMED exact-hitscan pawn hit must also cross
// the target's RENDER-TIME rewound capsule (halfRTT + ncp.HitAttribRenderExtraMs,
// plus slack). Shots aimed at the rendered body pass even with a lost claim,
// at any ping; shots at the invisible leading edge fail. Claimed routes
// (primary/padding/time-search) are untouched. Applies only to remote human
// shooters on claim-capable modes (bots and spread weapons never claim).
// =========================================================================
static TAutoConsoleVariable<int32> CVarUnclaimedRenderGate(
    TEXT("ncp.UnclaimedRenderGate"), 1,
    TEXT("Unclaimed exact-hitscan pawn hits must also cross the target's render-time rewound capsule: 1=enforce (default; failing hits demote to world impact), 0=shadow kill switch (verdict still logged via ncp.HitAttribDebug, no behavior change). Server-side only; flippable live on the server console. Claimed routes unaffected."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarUnclaimedRenderSlack(
    TEXT("ncp.UnclaimedRenderSlack"), 20.0f,
    TEXT("Extra radius (uu) forgiven by the unclaimed render-time check, absorbing render-lag estimate error and ping jitter. Default: 20."),
    ECVF_Default);

static bool ShotIntersectsRenderedCapsule(
    AUTCharacter* Target,
    const FVector& StartLocation,
    const FVector& EndTrace,
    float TraceRadius,
    FVector& OutVisualOffset,
    float& OutMissBy)
{
    OutVisualOffset = FVector::ZeroVector;
    OutMissBy = 0.0f;

    // Fail open if visual data is unavailable: this safeguard must never turn a
    // missing/unregistered mesh into a genuine no-reg.
    if (Target == nullptr || Target->GetMesh() == nullptr || !Target->GetMesh()->IsRegistered() ||
        Target->GetCapsuleComponent() == nullptr)
    {
        return true;
    }

    // Network smoothing moves the mesh relative to the actor/capsule. Remove the
    // character's normal mesh offset to recover the capsule centre that was
    // actually rendered to the shooter.
    const FVector ActorLocation = Target->GetActorLocation();
    const FVector ExpectedMeshLocation =
        ActorLocation + Target->GetActorQuat().RotateVector(Target->GetBaseTranslationOffset());
    OutVisualOffset = Target->GetMesh()->GetComponentLocation() - ExpectedMeshLocation;
    FVector VisualTargetLocation = ActorLocation + OutVisualOffset;

    float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (Target->UTCharacterMovement && Target->UTCharacterMovement->bIsFloorSliding)
    {
        VisualTargetLocation.Z = VisualTargetLocation.Z - CollisionHeight + Target->SlideTargetHeight;
        CollisionHeight = Target->SlideTargetHeight;
    }
    const float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();

    FVector ClosestPoint = FVector::ZeroVector;
    FVector ClosestCapsulePoint = VisualTargetLocation;
    if (CollisionRadius >= CollisionHeight)
    {
        ClosestPoint = FMath::ClosestPointOnSegment(VisualTargetLocation, StartLocation, EndTrace);
    }
    else
    {
        const FVector CapsuleSegment(0.0f, 0.0f, CollisionHeight - CollisionRadius);
        FMath::SegmentDistToSegmentSafe(
            StartLocation, EndTrace,
            VisualTargetLocation - CapsuleSegment, VisualTargetLocation + CapsuleSegment,
            ClosestPoint, ClosestCapsulePoint);
    }

    const float BaseRadius = CollisionRadius + TraceRadius;
    const float Distance = FVector::Dist(ClosestPoint, ClosestCapsulePoint);
    OutMissBy = Distance - BaseRadius;
    const float Tolerance = FMath::Max(0.0f, CVarVisualHitscanClaimTolerance.GetValueOnGameThread());
    return Distance <= BaseRadius + Tolerance;
}

// =========================================================================
// PROJECTILE DIRECT-HIT LAG COMPENSATION (rocket + flak shell) — server-only.
// Validates a client's direct-hit claim by finding, in the target's rewound
// history, the instant its capsule sat at the client-reported ClaimedHitLocation,
// then confirming the REAL projectile actually passed through that point
// (server owns the hit decision). See ServerProjectileHitClaim_Implementation.
// =========================================================================
static TAutoConsoleVariable<int32> CVarRocketLagComp(
    TEXT("ut.RocketLagComp"),
    1,
    TEXT("Server switch for projectile direct-hit lag compensation (rocket/flak shell).\n")
    TEXT("1 = on (default), 0 = present-time only. Requires bEnableProjectileRewind on the\n")
    TEXT("weapon so clients are actually sending claims."),
    ECVF_Default
);
static TAutoConsoleVariable<float> CVarRocketLagCompMaxWindowMs(
    TEXT("ut.RocketLagCompMaxWindowMs"),
    200.0f,
    TEXT("Max rewind/lookback window in ms, applied at any ping. Bounds 'shot behind cover'\n")
    TEXT("(keep <= the hitscan rewind envelope) and naturally degrades compensation once a\n")
    TEXT("shooter's RTT exceeds it. Full coverage holds for RTT up to ~window/1.1. Pairs with\n")
    TEXT("ut.RocketLagCompGraceMs (keep them matched; both ~= the ping cutoff)."),
    ECVF_Default
);
// Grace buffer: how long a RESOLVED (exploded) rocket/flak shell is retained so a claim that
// arrives after the server projectile is already gone (the close-range timing race, where the
// claim is ~one shooter-RTT late) can still rewind-rescue. Match to ut.RocketLagCompMaxPingMs.
// 0 disables the grace path entirely (kill switch -> live-projectile-only, the pre-grace behavior).
static TAutoConsoleVariable<float> CVarRocketLagCompGraceMs(
    TEXT("ut.RocketLagCompGraceMs"),
    200.0f,
    TEXT("Grace buffer (ms) for retaining a resolved rocket/flak shell so a late claim can still\n")
    TEXT("rewind-rescue (close-range timing race). Match ut.RocketLagCompMaxPingMs. 0 = disabled."),
    ECVF_Default
);
static TAutoConsoleVariable<float> CVarRocketLagCompMaxPingMs(
    TEXT("ut.RocketLagCompMaxPingMs"),
    150.0f,
    TEXT("Reject direct-hit claims from shooters whose RTT (ms) exceeds this. Anti-abuse cutoff.\n")
    TEXT("150 covers Israel/EU->NYC (~143ms). Matched to MaxWindowMs/GraceMs (both 200)."),
    ECVF_Default
);

// Projectile-rewind claim diagnostics. These can be high-volume on a live server, so keep
// them opt-in just like ncp.RocketPairDebug and ncp.ShockDebug. Gameplay validation and
// rewind decisions are unchanged when logging is disabled.
static TAutoConsoleVariable<int32> CVarRocketLagCompDebug(
    TEXT("ut.RocketLagCompDebug"),
    0,
    TEXT("Projectile rewind claim diagnostics. 0=off (default), 1=claim/rejection/save logs."),
    ECVF_Default
);

static FORCEINLINE bool RocketLagCompDbg()
{
    return CVarRocketLagCompDebug.GetValueOnGameThread() > 0;
}

int32 AUTWeaponFix::GetTargetProjectileTickRate()
{
    int32 TargetHz = CVarProjectileTickRate.GetValueOnGameThread();
    return FMath::Clamp(TargetHz, 120, 720);
}

static int32 GetClampedProjectileHz()
{
    int32 TargetHz = CVarProjectileTickRate.GetValueOnGameThread();
    return FMath::Clamp(TargetHz, 120, 720);
}





//extern FCollisionResponseParams WorldResponseParams;

TMap<FName, bool> AUTWeaponFix::HiddenWeaponsByTag;
TMap<FName, FString> AUTWeaponFix::SavedSkinPaths;
TMap<FName, UUTWeaponSkin*> AUTWeaponFix::CachedSkinAssets;
static TMap<FName, FString> PendingWeaponSkinPaths;
static TMap<FString, UUTWeaponSkin*> PreloadedWeaponSkinCatalog;
static bool bWeaponSkinCatalogReady = false;

class FNCPWeaponSkinCatalogReferences : public FGCObject
{
public:
	TArray<UObject*> Assets;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(Assets);
	}
};

static FNCPWeaponSkinCatalogReferences* WeaponSkinCatalogReferences = nullptr;
bool AUTWeaponFix::bWeaponSettingsLoaded = false;
// Default = BP-parity hide; true = classic camera-beam hide (pre-2026-07-19).
bool AUTWeaponFix::bClassicWeaponHide = false;
// Stomach-height defaults; LoadWeaponSettings overrides from Mod.ini.
float AUTWeaponFix::HiddenBeamBackOffset = 10.f;
float AUTWeaponFix::HiddenBeamDownOffset = 35.f;

static const TCHAR* WEAPON_SETTINGS_SECTION = TEXT("NetcodePlus.WeaponSettings");
static const FName WEAPON_SKIN_CATALOG_ROOT(TEXT("/Game/Blueprints/UT+/UT+/WeaponSkinsPlus"));

// Versioned public selection manifest shared by the current 38-asset RC PAK
// and the older 59-asset staged cook. These 30 paths are unrestricted and carry
// a real weapon-family tag; utility and cook-specific variants remain invalid.
// Future folder additions do not become network-valid automatically.
// NOTE: this list is all-or-nothing — RefreshWeaponSkinCatalog() only marks the
// catalog ready when EVERY entry resolves, so an entry missing from the shipped
// PAK disables weapon skins entirely. Only add assets that are actually cooked.
static const TCHAR* const REQUIRED_WEAPON_SKIN_ASSETS[] =
{
	TEXT("BlackDeath"),
	TEXT("FlakDefault"),
	TEXT("FlakPink"),
	TEXT("FlakRedDeath"),
	TEXT("FlakVoid"),
	TEXT("InvisibleBio"),
	TEXT("LinkBee"),
	TEXT("LinkBeeElim"),
	TEXT("LinkFreedom"),
	TEXT("LinkMint"),
	TEXT("PinkLG"),
	TEXT("Rocket99"),
	TEXT("Rocket99Elim"),
	TEXT("RocketBee"),
	TEXT("RocketBeeElim"),
	TEXT("RocketBurn"),
	TEXT("RocketBurnElim"),
	TEXT("RocketMahogany"),
	TEXT("RocketMahoganyElim"),
	TEXT("RocketSnowElim"),
	TEXT("RocketTiger"),
	TEXT("ShockBlackTiger"),
	TEXT("ShockBlueBird"),
	TEXT("ShockBlueBirdElim"),
	TEXT("ShockFreedom"),
	TEXT("SniperBlueBird"),
	TEXT("SniperMahogany"),
	TEXT("SniperPink"),
	TEXT("SniperRedBird"),
	TEXT("SniperSport")
};

static FString GetWeaponSkinObjectPath(const TCHAR* AssetName)
{
	return FString::Printf(TEXT("%s/%s.%s"),
		*WEAPON_SKIN_CATALOG_ROOT.ToString(), AssetName, AssetName);
}

static int32 RefreshWeaponSkinCatalog()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssetsByPath(WEAPON_SKIN_CATALOG_ROOT, Assets, true);
	TMap<FString, FAssetData> ManifestAssetData;
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClass != UUTWeaponSkin::StaticClass()->GetFName())
		{
			continue;
		}
		ManifestAssetData.Add(Asset.ObjectPath.ToString(), Asset);
	}

	TMap<FString, UUTWeaponSkin*> NewCatalog;
	int32 LoadedRequiredCount = 0;
	auto LoadManifestGroup = [&ManifestAssetData, &NewCatalog](
		const TCHAR* const* AssetNames, int32 AssetCount, int32& LoadedCount)
	{
		for (int32 Index = 0; Index < AssetCount; ++Index)
		{
			const FString ObjectPath = GetWeaponSkinObjectPath(AssetNames[Index]);
			const FAssetData* AssetData = ManifestAssetData.Find(ObjectPath);
			if (AssetData == nullptr)
			{
				continue;
			}

			// This is the only synchronous load in the feature, and it occurs during
			// lifecycle preload. Unlisted folder assets are never loaded or approved.
			UUTWeaponSkin* Skin = Cast<UUTWeaponSkin>(AssetData->GetAsset());
			if (Skin == nullptr || Skin->GetPathName() != ObjectPath ||
				Skin->WeaponType.ToString().IsEmpty() ||
				Skin->WeaponSkinCustomizationTag == NAME_None || Skin->bRequiresItem ||
				Skin->RequiredAchievement != NAME_None)
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("Weapon skin manifest entry rejected: %s"), *ObjectPath);
				continue;
			}

			NewCatalog.Add(ObjectPath, Skin);
			++LoadedCount;
		}
	};

	LoadManifestGroup(REQUIRED_WEAPON_SKIN_ASSETS,
		ARRAY_COUNT(REQUIRED_WEAPON_SKIN_ASSETS), LoadedRequiredCount);

	if (LoadedRequiredCount == ARRAY_COUNT(REQUIRED_WEAPON_SKIN_ASSETS))
	{
		if (WeaponSkinCatalogReferences == nullptr)
		{
			WeaponSkinCatalogReferences = new FNCPWeaponSkinCatalogReferences();
		}
		WeaponSkinCatalogReferences->Assets.Empty(NewCatalog.Num());
		for (const TPair<FString, UUTWeaponSkin*>& Pair : NewCatalog)
		{
			WeaponSkinCatalogReferences->Assets.Add(Pair.Value);
		}
		PreloadedWeaponSkinCatalog = MoveTemp(NewCatalog);
		bWeaponSkinCatalogReady = true;
	}
	else if (!bWeaponSkinCatalogReady)
	{
		PreloadedWeaponSkinCatalog.Empty();
	}

	if (!bWeaponSkinCatalogReady)
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("Weapon skin manifest incomplete: loaded=%d required=%d"),
			LoadedRequiredCount, ARRAY_COUNT(REQUIRED_WEAPON_SKIN_ASSETS));
	}
	return PreloadedWeaponSkinCatalog.Num();
}

UUTWeaponSkin* AUTWeaponFix::FindPreloadedWeaponSkin(const FString& SkinPath)
{
	return (!bWeaponSkinCatalogReady || SkinPath.IsEmpty())
		? nullptr
		: PreloadedWeaponSkinCatalog.FindRef(SkinPath);
}

void AUTWeaponFix::GetPreloadedWeaponSkins(TArray<UUTWeaponSkin*>& OutSkins)
{
	if (!bWeaponSkinCatalogReady)
	{
		OutSkins.Empty();
		return;
	}
	OutSkins.Empty(PreloadedWeaponSkinCatalog.Num());
	PreloadedWeaponSkinCatalog.GenerateValueArray(OutSkins);
	OutSkins.Sort([](const UUTWeaponSkin& A, const UUTWeaponSkin& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
}

bool AUTWeaponFix::IsWeaponSkinCompatible(UUTWeaponSkin* Skin, UClass* WeaponClass)
{
	if (Skin == nullptr || WeaponClass == nullptr)
	{
		return false;
	}
	const AUTWeapon* WeaponCDO = Cast<AUTWeapon>(WeaponClass->GetDefaultObject());
	if (Skin->WeaponSkinCustomizationTag == NAME_None || WeaponCDO == nullptr ||
		WeaponCDO->WeaponSkinCustomizationTag == NAME_None ||
		Skin->WeaponSkinCustomizationTag != WeaponCDO->WeaponSkinCustomizationTag)
	{
		return false;
	}

	FString TargetClassPath = Skin->WeaponType.ToString();
	TargetClassPath.RemoveFromStart(TEXT("BlueprintGeneratedClass'"));
	TargetClassPath.RemoveFromStart(TEXT("Class'"));
	TargetClassPath.RemoveFromEnd(TEXT("'"));
	for (UClass* Candidate = WeaponClass;
		Candidate != nullptr && Candidate->IsChildOf(AUTWeapon::StaticClass());
		Candidate = Candidate->GetSuperClass())
	{
		if (Candidate->GetPathName() == TargetClassPath)
		{
			return true;
		}
	}

	return false;
}

UUTWeaponSkin* AUTWeaponFix::FindWeaponSkinForClass(
	const TArray<UUTWeaponSkin*>& WeaponSkins, UClass* WeaponClass)
{
	for (int32 Index = WeaponSkins.Num() - 1; Index >= 0; --Index)
	{
		UUTWeaponSkin* Candidate = WeaponSkins[Index];
		if (Candidate != nullptr &&
			FindPreloadedWeaponSkin(Candidate->GetPathName()) == Candidate &&
			IsWeaponSkinCompatible(Candidate, WeaponClass))
		{
			return Candidate;
		}
	}
	return nullptr;
}

FString AUTWeaponFix::GetConfiguredWeaponSkinPath(const AUTWeapon* Weapon)
{
	if (!bSkinsEnabled || Weapon == nullptr || Weapon->WeaponSkinCustomizationTag == NAME_None)
	{
		return FString();
	}

	const FString* SkinPath = SavedSkinPaths.Find(Weapon->WeaponSkinCustomizationTag);
	return SkinPath != nullptr ? *SkinPath : FString();
}

UUTWeaponSkin* AUTWeaponFix::GetConfiguredWeaponSkin(const AUTWeapon* Weapon)
{
	if (Weapon == nullptr)
	{
		return nullptr;
	}

	UUTWeaponSkin* Skin = FindPreloadedWeaponSkin(GetConfiguredWeaponSkinPath(Weapon));
	return IsWeaponSkinCompatible(Skin, Weapon->GetClass()) ? Skin : nullptr;
}

void AUTWeaponFix::LoadWeaponSettings()
{
	if (bWeaponSettingsLoaded || GConfig == nullptr) return;
	bWeaponSettingsLoaded = true;
	const bool bLogTiming = SkinTiming();
	const double LoadStartTime = bLogTiming ? FPlatformTime::Seconds() : 0.0;
	const int32 CatalogSkinCount = bSkinsEnabled ? RefreshWeaponSkinCatalog() : 0;
	int32 LoadedSelectionCount = 0;

	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	// Hidden-weapon beam-origin offsets. Clamped 0-100 so a corrupted value
	// can't spawn beams behind the player or through the floor. Sliders in
	// SUTWeaponSkinSelector write back to these same keys.
	{
		FString OffsetStr;
		if (GConfig->GetString(WEAPON_SETTINGS_SECTION, TEXT("HiddenBeamBack"), OffsetStr, ModIniPath) && !OffsetStr.IsEmpty())
		{
			HiddenBeamBackOffset = FMath::Clamp(FCString::Atof(*OffsetStr), 0.f, 100.f);
		}
		if (GConfig->GetString(WEAPON_SETTINGS_SECTION, TEXT("HiddenBeamDown"), OffsetStr, ModIniPath) && !OffsetStr.IsEmpty())
		{
			HiddenBeamDownOffset = FMath::Clamp(FCString::Atof(*OffsetStr), 0.f, 100.f);
		}
	}

	// Hidden-weapon style. Absent key = BP-parity default, so configs from before
	// this option (and untouched seq-51 configs) render without any ini edit.
	GConfig->GetBool(WEAPON_SETTINGS_SECTION, TEXT("ClassicWeaponHide"), bClassicWeaponHide, ModIniPath);

	// Enumerate persisted keys directly so settings can be loaded before map packages
	// (and their Blueprint weapon classes) are resident.
	FConfigSection* SettingsSection = GConfig->GetSectionPrivate(
		WEAPON_SETTINGS_SECTION, false, true, ModIniPath);
	if (SettingsSection != nullptr)
	{
		TSet<FName> SeenSkinTags;
		for (FConfigSection::TIterator It(*SettingsSection); It; ++It)
		{
			const FString Key = It.Key().ToString();
			const FString Value = It.Value().GetValue();
			if (Key.StartsWith(TEXT("Hide.")))
			{
				const FName HideKey(*Key.Mid(5));
				if (HideKey != NAME_None)
				{
					HiddenWeaponsByTag.Add(HideKey,
						Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase));
				}
			}
			else if (bSkinsEnabled && !IsRunningDedicatedServer() &&
				Key.StartsWith(TEXT("Skin.")) && !Value.IsEmpty())
			{
				const FName SkinTag(*Key.Mid(5));
				if (SkinTag != NAME_None && !SeenSkinTags.Contains(SkinTag))
				{
					SeenSkinTags.Add(SkinTag);
					SavedSkinPaths.Add(SkinTag, Value);
					if (UUTWeaponSkin* Skin = FindPreloadedWeaponSkin(Value))
					{
						CachedSkinAssets.Add(SkinTag, Skin);
						++LoadedSelectionCount;
					}
					else
					{
						PendingWeaponSkinPaths.Add(SkinTag, Value);
						UE_LOG(LogUTWeaponFix, Warning,
							TEXT("Weapon skin preload deferred: tag=%s path=%s"),
							*SkinTag.ToString(), *Value);
					}
				}
			}
		}
	}

	if (bLogTiming)
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] catalog/settings preload: catalog=%d selected=%d time=%.3fms"),
			CatalogSkinCount, LoadedSelectionCount,
			(FPlatformTime::Seconds() - LoadStartTime) * 1000.0);
	}
}

void AUTWeaponFix::RetryPendingWeaponSkins()
{
	const bool bLogTiming = SkinTiming();
	const double RetryStartTime = bLogTiming ? FPlatformTime::Seconds() : 0.0;
	const int32 CatalogSkinCount = bSkinsEnabled
		? (bWeaponSkinCatalogReady
			? PreloadedWeaponSkinCatalog.Num()
			: RefreshWeaponSkinCatalog())
		: 0;
	if (IsRunningDedicatedServer() || PendingWeaponSkinPaths.Num() == 0)
	{
		if (bLogTiming)
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[SkinTiming] catalog retry: catalog=%d time=%.3fms"),
				CatalogSkinCount,
				(FPlatformTime::Seconds() - RetryStartTime) * 1000.0);
		}
		return;
	}

	const TMap<FName, FString> RetryPaths = PendingWeaponSkinPaths;
	PendingWeaponSkinPaths.Empty();
	int32 LoadedSkinCount = 0;

	for (const TPair<FName, FString>& Pair : RetryPaths)
	{
		const FString* CurrentPath = SavedSkinPaths.Find(Pair.Key);
		if (CurrentPath == nullptr || *CurrentPath != Pair.Value)
		{
			continue;
		}

		if (UUTWeaponSkin* Skin = FindPreloadedWeaponSkin(Pair.Value))
		{
			CachedSkinAssets.Add(Pair.Key, Skin);
			++LoadedSkinCount;
		}
		else
		{
			PendingWeaponSkinPaths.Add(Pair.Key, Pair.Value);
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("Weapon skin unavailable after world initialization: tag=%s path=%s"),
				*Pair.Key.ToString(), *Pair.Value);
		}
	}

	if (bLogTiming)
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] world-init retry: catalog=%d requested=%d loaded=%d pending=%d time=%.3fms"),
			CatalogSkinCount, RetryPaths.Num(), LoadedSkinCount,
			PendingWeaponSkinPaths.Num(),
			(FPlatformTime::Seconds() - RetryStartTime) * 1000.0);
	}
}

void AUTWeaponFix::CleanupWeaponSettings()
{
	CachedSkinAssets.Empty();
	SavedSkinPaths.Empty();
	PendingWeaponSkinPaths.Empty();
	PreloadedWeaponSkinCatalog.Empty();
	bWeaponSkinCatalogReady = false;
	delete WeaponSkinCatalogReferences;
	WeaponSkinCatalogReferences = nullptr;
	bWeaponSettingsLoaded = false;
}

void AUTWeaponFix::SaveWeaponSettings()
{
	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	// Save hide settings
	for (auto& Pair : HiddenWeaponsByTag)
	{
		FString Key = FString::Printf(TEXT("Hide.%s"), *Pair.Key.ToString());
		GConfig->SetString(WEAPON_SETTINGS_SECTION, *Key, Pair.Value ? TEXT("1") : TEXT("0"), ModIniPath);
	}

	GConfig->SetBool(WEAPON_SETTINGS_SECTION, TEXT("ClassicWeaponHide"), bClassicWeaponHide, ModIniPath);

	GConfig->Flush(false, ModIniPath);
}

AUTWeaponFix::AUTWeaponFix(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Initialize arrays for standard two fire modes
    AuthoritativeFireEventIndex.SetNum(2);
    LastProcessedStopEventIndex.SetNum(2);
    ClientFireEventIndex.SetNum(2);
    LastFireTime.SetNum(2);
    LastReleaseTime.SetNum(2);
    FireModeActiveState.SetNum(2);
    bIsTransactionalFire = false;
    bHandlingRetry = false;
    bFireHeldByPlayer[0] = false;
    bFireHeldByPlayer[1] = false;
    bCrossModeRetryArmed[0] = false;
    bCrossModeRetryArmed[1] = false;
    HitScanPadding = 30.f;
    HitScanPaddingStationary = 10.0f;
	FudgeFactorMs = 20;
	ProjectilePredictionCapMs = 120.0f;
    LastMultiPressTime = 0.f;
    LastShockCoreSpawnTime = 0.0f;
    LastFlakShellSpawnTime = 0.0f;
    MouseDebounceWindow = 0.030f;  // 30ms — mouse-bounce / scroll-wheel coalesce
    OriginalFPSMaterial = nullptr;
    AppliedFPSMaterial = nullptr;
    AppliedFPSMaterialInstance = nullptr;
    bCapturedOriginalFPSMaterial = false;

    for (int32 i = 0; i < 2; i++)
    {
        AuthoritativeFireEventIndex[i] = 0;
        LastProcessedStopEventIndex[i] = INDEX_NONE;
        ClientFireEventIndex[i] = 0;
        LastFireTime[i] = -1.0f;
        LastReleaseTime[i] = -1.0f;
        FireModeActiveState[i] = 0;
    }

    CurrentlyFiringMode = 255; // No mode currently firing
}



void AUTWeaponFix::PostInitProperties()
{
    Super::PostInitProperties();
    /*
    // SWAP THE STATES
    // Replace standard Firing States with our Transactional State.
    // We do this in PostInit to override Blueprint defaults safely.
    if (FiringState.Num() > 0)
    {
        for (int32 i = 0; i < FiringState.Num(); i++)
        {
            // Construct the new state object
            UUTWeaponStateFiring_Transactional* NewState = NewObject<UUTWeaponStateFiring_Transactional>(this, UUTWeaponStateFiring_Transactional::StaticClass());
            if (NewState)
            {
                FiringState[i] = NewState;
            }
        }
    }
    */
}


void AUTWeaponFix::BeginPlay()
{
    Super::BeginPlay();

    // Clear any residual state
    CurrentlyFiringMode = 255;
    for (int32 i = 0; i < FireModeActiveState.Num(); i++)
    {
        FireModeActiveState[i] = 0;
    }

    // One-shot per weapon class (server-side): name each fire mode's ACTUAL FiringState
    // class. The companion-pak BPs override the native constructors' states (production
    // shock ran UTWeaponStateFiring_Transactional in the 2026-07-24 hub logs while the C++
    // layout is stock), and the stock-vs-transactional split decides server-side refire
    // behavior — so settle the deployed layout from any hub log instead of re-deriving it
    // from native code every audit.
    if (Role == ROLE_Authority)
    {
        static TSet<FName> LoggedStateLayouts;
        const FName LayoutClassName = GetClass()->GetFName();
        if (!LoggedStateLayouts.Contains(LayoutClassName))
        {
            LoggedStateLayouts.Add(LayoutClassName);
            FString Layout;
            for (int32 i = 0; i < FiringState.Num(); i++)
            {
                Layout += FString::Printf(TEXT("%smode%d=%s"), (i > 0) ? TEXT(" ") : TEXT(""), i,
                    FiringState[i] ? *FiringState[i]->GetClass()->GetName() : TEXT("null"));
            }
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[StateLayout] %s: %s"), *LayoutClassName.ToString(), *Layout);
        }
    }
}

void AUTWeaponFix::OnRetryTimer(uint8 FireModeNum)
{
    
    bHandlingRetry = true;
    UE_LOG(LogUTWeaponFix, Verbose, TEXT("[OnRetryTimer] Mode %d: Retry firing — calling StartFire"), FireModeNum);
    if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] OnRetryTimer mode=%d -> StartFire"), FireModeNum);
    StartFire(FireModeNum);
    bHandlingRetry = false;
}









void AUTWeaponFix::StartFire(uint8 FireModeNum)
{
    if (FireDbg())
    {
        UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] StartFire mode=%d curFiring=%d state=%s"),
            FireModeNum, CurrentlyFiringMode,
            GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"));
    }

    // ---------------------------------------------------------
    // ZOOM BYPASS (MUST BE FIRST)
    // ---------------------------------------------------------
    // STOCK CODE CONFIRMATION: UTWeaponStateZooming.cpp shows that Zooming
    // does not fire a shot (BeginFiringSequence returns false).
    // Therefore, it should NOT be gated by the weapon's Refire Time.
    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        // Check 1: Is it a child of the Zooming Class?
        // Check 2: Does the name contain "Zoom"? (Safety for BPs)
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateZooming::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Zoom")))
        {
            // Hand off to standard UT Zoom logic immediately
            Super::StartFire(FireModeNum);
            return;
        }
    }

    // ---------------------------------------------------------
    // MOUSE-BOUNCE / SCROLL-WHEEL DEBOUNCE
    // ---------------------------------------------------------
    // Coalesce rapid release+press pairs into held intent (PendingFire stays
    // true, no new fire event). 2026-07-17 REFRAME: the original 30ms BP default
    // was tuned against a "hardware bounce ceiling" that modern mice don't have —
    // optical switches (Viper V3 / DeathAdder V3 class) cannot bounce, and
    // mechanical gaming mice debounce in firmware (4-8ms) before the OS sees an
    // event. Meanwhile the window keys on the RELEASE->press gap, not the full
    // double-click cycle: a fast tap-firer at high duty cycle produces legit
    // gaps under 30ms, and at high fps the frame-quantized timestamps stop
    // padding tiny gaps upward — which is exactly why high-fps players on
    // optical mice reported eaten clicks ("weapon didn't fire"). The failure
    // cost is asymmetric: a rare genuine double-event that slips through is
    // absorbed by the server ROF gate; an eaten real click is the worst feel in
    // the game. So the effective window is CAPPED by ncp.MouseDebounceCap
    // (default 0.01) rather than trusting the per-weapon BP 0.03. Cap semantics:
    // min(BP, cap); cap 0 = debounce off; cap -1 = pure BP (legacy kill-switch).
    float EffectiveDebounce = MouseDebounceWindow;
    {
        const float Cap = CVarMouseDebounceCap.GetValueOnGameThread();
        if (Cap >= 0.f)
        {
            EffectiveDebounce = FMath::Min(MouseDebounceWindow, Cap);
        }
    }
    if (UTOwner && UTOwner->IsLocallyControlled() &&
        EffectiveDebounce > 0.f &&
        LastReleaseTime.IsValidIndex(FireModeNum) &&
        LastReleaseTime[FireModeNum] > 0.0f)
    {
        const float SinceRelease = GetWorld()->GetTimeSeconds() - LastReleaseTime[FireModeNum];
        if (SinceRelease >= 0.f && SinceRelease < EffectiveDebounce)
        {
            // Verbose log so testers can confirm the debounce is engaging
            // when investigating low-debounce-mouse complaints. Off by default.
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[NCFire.Debounce] mode=%d sinceRelease=%.4f window=%.4f (bp=%.4f)"),
                FireModeNum, SinceRelease, EffectiveDebounce, MouseDebounceWindow);
            UTOwner->SetPendingFire(FireModeNum, true);
            return;
        }
    }

	if (GetCurrentState() &&
		(GetCurrentState()->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass())))
	{
		if (FireModeNum != CurrentFireMode)
		{
			UUTWeaponStateFiringChargedRocket_Transactional* TransState =
				Cast<UUTWeaponStateFiringChargedRocket_Transactional>(GetCurrentState());

			if (TransState && !TransState->bCharging)
			{
				// BURSTING: Buffer M1 input for after burst completes
				if (UTOwner)
				{
					UTOwner->SetPendingFire(FireModeNum, true);
				}
				return;
			}

			// LOADING: Cycle rocket mode
			if (FireModeNum < 2)
			{
				GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
			}

            if (UTOwner)
            {
                // [FIX] Set to false to consume the "Click" immediately.
                // This prevents the engine from re-running StartFire on the next frame.
                UTOwner->SetPendingFire(FireModeNum, false);
                OnMultiPress(FireModeNum);
                return;
            }
			return;
		}

		// Same mode - register intent so RefireCheckTimer sees it
		if (UTOwner)
		{
			UTOwner->SetPendingFire(FireModeNum, true);
		}
		return;
	}


	// If the weapon is in Active (Idle) state, it cannot possibly be firing.
	// Any "CurrentlyFiring" flags here are bugs from the Auto-Fire/GraceTimer path.
	// We clear them immediately so they don't block your new input.
	if (GetCurrentState() == ActiveState)
	{
		CurrentlyFiringMode = 255;
		if (FireModeActiveState.IsValidIndex(0)) FireModeActiveState[0] = 0;
		if (FireModeActiveState.IsValidIndex(1)) FireModeActiveState[1] = 0;
	}
    
    // ---------------------------------------------------------
    // 1. SAFETY CHECKS
    // ---------------------------------------------------------
    if (UTOwner && UTOwner->IsFiringDisabled())
    {
        return;
    }

    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
    if (GS && GS->PreventWeaponFire())
    {
        return;
    }

	// GHOST FIX (ncp.GhostFix): record a GENUINE held press. We are past the zoom,
	// mouse-bounce, and charged-rocket-buffer early-returns, so this only sets for a
	// real new input press — not a retry (bHandlingRetry) nor a buffered re-entry.
	// Set even when the press is deferred by cooldown, so a held-during-cooldown switch
	// still carries. Cleared on a genuine release in StopFire; read in PutDown.
	if (GhostFix() && !bHandlingRetry && FireModeNum < 2 && UTOwner && UTOwner->IsLocallyControlled())
	{
		bFireHeldByPlayer[FireModeNum] = true;
	}

	bool bIsSwitching = (CurrentState == UnequippingState) || (UTOwner && UTOwner->GetPendingWeapon());

	if (bIsSwitching)
	{
		if (UTOwner)
		{
			UTOwner->SetPendingFire(FireModeNum, true);
			UE_LOG(LogUTWeaponFix, Verbose, TEXT("Setting pending fire on swap %d"), FireModeNum);
		}
		return;
	}
    // ---------------------------------------------------------
    // 2. COOLDOWN VALIDATION (MOVED TO TOP)
    // ---------------------------------------------------------
    // We check this FIRST to prevent any "Bypass" logic (like Charged States)
    // from entering a new firing sequence illegally.

    float CurrentTime = GetWorld()->GetTimeSeconds();

    bool bIsSwitchingModes = false;

    // Are we currently in a Charged State?
    if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
        CurrentState->GetName().Contains(TEXT("Charged"))))
    {
        // Only allow bypassing cooldowns if we are ACTIVELY CHARGING (holding the load).
        // If we are unloading/firing (bCharging == false), we must respect the rate of fire.
        bool bIsActivelyCharging = false;

        UUTWeaponStateFiringChargedRocket_Transactional* TransState =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);

        if (TransState)
        {
            bIsActivelyCharging = TransState->bCharging;
        }

        // Only flag as a "Mode Switch" if we are holding the charge.
        // If we fired, bIsActivelyCharging is false, so this block is skipped,
        // and the Cooldown Check below will correctly block the rapid-fire attempt.
        if (bIsActivelyCharging && FireModeNum != CurrentFireMode)
        {
            bIsSwitchingModes = true;
        }
    }


    if (!bIsSwitchingModes &&  IsFireModeOnCooldown(FireModeNum, CurrentTime))
    {
        // If we are in FiringState for this mode with a deferred GotoActiveState
        // timer active, the user tapped and is re-pressing during cooldown.
        // Do NOT return early — fall through to the retry logic below so a
        // retry timer is scheduled. Without this, the input is silently eaten
        // because PendingFire never gets set and no retry is scheduled.
        if (GetCurrentState() == FiringState[FireModeNum]
            && GetWorldTimerManager().IsTimerActive(DeferredActiveStateHandle))
        {
            // Set PendingFire so the retry/auto-fire path will pick it up
            if (UTOwner)
            {
                UTOwner->SetPendingFire(FireModeNum, true);
            }
            // Fall through to retry logic at line ~294
        }
        // If we are actively in the firing state (no deferred timer — genuinely
        // mid-fire-sequence), let the state run its course.
        else if (GetCurrentState() == FiringState[FireModeNum])
        {
			if (UTOwner &&
				(FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass())))
			{
				UTOwner->SetPendingFire(FireModeNum, true);
			}

			if (FireModeNum < 2)
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            }
            return;
        }

        // RETRY LOGIC (Smart Wait for Locally Controlled Player)
        // Previously gated on Role < ROLE_Authority. That broke held-fire after a
        // rapid-reclick in standalone PIE / listen server: DeferredGotoActiveState
        // clears PendingFire on cooldown end, and without a retry queued the held
        // shot was lost. Locally controlled is the right gate — dedicated server
        // pawns aren't locally controlled so this still skips correctly there.
        if (UTOwner && UTOwner->IsLocallyControlled())
        {
            // Find when the cooldown actually ends
            float MaxReadyTime = 0.f;
            for (int32 i = 0; i < LastFireTime.Num(); i++)
            {
                if (LastFireTime[i] > 0.0f)
                {
                    float ModeReadyTime = LastFireTime[i] + GetRefireTime(i);
                    if (ModeReadyTime > MaxReadyTime)
                    {
                        MaxReadyTime = ModeReadyTime;
                    }
                }
            }
			if (EarliestFireTime > MaxReadyTime)
			{
				MaxReadyTime = EarliestFireTime;
			}
            float Delay = MaxReadyTime - CurrentTime;

            // Schedule a retry if the delay is significant
            if (Delay > 0.01f)
            {
                float WaitTime = Delay + 0.01f;
                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, WaitTime, false);
            }
            else
            {
                // Poll next frame if delay is tiny (animation lag)
                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, 0.01f, false);
            }
            if (FireModeNum < 2) { bCrossModeRetryArmed[FireModeNum] = false; }   // same-mode arm owns the handle now
            if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] mode=%d ON-COOLDOWN -> retry scheduled (delay=%.3f)"), FireModeNum, Delay);
        }
        return;
    }

    // If we passed cooldown check, clear any pending retries
    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    }


	if (CurrentlyFiringMode != 255 && CurrentlyFiringMode != FireModeNum)
	{
		// 1. IDENTIFY STATE
		UUTWeaponStateFiringChargedRocket_Transactional* TransState =
			Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);

		// Generic check for compatibility/safety
		bool bIsChargedState = (TransState != nullptr) ||
			(CurrentState && CurrentState->GetName().Contains(TEXT("Charged")));

		// 2. HANDLE MODE SWITCH FOR STANDARD WEAPONS
		// On the server, the Mode 1 Start RPC can arrive before Mode 0 Stop is
		// processed (RPC timing). Instead of silently dropping Mode 1 (which causes
		// the "fake core with no auth" bug), stop the current mode and proceed.
		// This mirrors what the client does: release primary → StopFire → then StartFire secondary.
		if (!bIsChargedState)
		{
			UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StartFire] Mode %d: Cross-mode switch from Mode %d — stopping current mode first"), FireModeNum, CurrentlyFiringMode);
			StopFire(CurrentlyFiringMode);
			// CurrentlyFiringMode is now 255, fall through to fire the new mode
		}

		// 3. HANDLE ROCKET LAUNCHER LOGIC
		// We know we are in a Charged State. Now we decide: Cycle Mode or Queue Shot?

		// A) Transactional State Logic (The Fix)
		if (TransState)
		{
			if (TransState->bCharging)
			{
				// LOADING: User input cycles the rocket mode (Spread -> Grenade -> Spiral)
				if (CurrentState->IsFiring())
				{
					// Clear flag to prevent "Ghost Fire" on release
					if (UTOwner) UTOwner->SetPendingFire(FireModeNum, false);
					//OnMultiPress(FireModeNum);
				}
				return; // Consumed input
			}
			else
			{
				// BURSTING: User released load and is unloading rockets.
				// Input intent is to fire Primary immediately after burst.
				// We Buffer the input and Return to prevent Double Drawing.
				if (UTOwner)
				{
					UTOwner->SetPendingFire(FireModeNum, true);
				}
				return; // Consumed input
			}
		}

		// B) Legacy/Fallback Logic (Standard UT behavior)
		if (CurrentState->IsFiring())
		{
			// Stall fix (ncp.CrossModeRetry): the StopFire(CurrentlyFiringMode) above does NOT
			// exit the transactional firing state (the cycle runs out on its own timer), so a
			// press landing in the refire tail still reaches here. Stock keeps PendingFire set
			// and the Active-state pending check fires it at cycle end — but our
			// DeferredGotoActiveState clears PendingFire on cooldown end (see the RETRY LOGIC
			// comment above), so mirror the same-mode ON-COOLDOWN path instead: queue a retry
			// for the moment every mode's refire has elapsed. A release before then cancels it
			// (StopFire clears RetryFireHandle unconditionally) — tap behaves like stock too.
			if (CrossModeRetry() && FireModeNum < 2 && UTOwner && UTOwner->IsLocallyControlled())
			{
				float MaxReadyTime = 0.f;
				for (int32 i = 0; i < LastFireTime.Num(); i++)
				{
					if (LastFireTime[i] > 0.0f)
					{
						float ModeReadyTime = LastFireTime[i] + GetRefireTime(i);
						if (ModeReadyTime > MaxReadyTime)
						{
							MaxReadyTime = ModeReadyTime;
						}
					}
				}
				if (EarliestFireTime > MaxReadyTime)
				{
					MaxReadyTime = EarliestFireTime;
				}
				const float Delay = FMath::Max(MaxReadyTime - GetWorld()->GetTimeSeconds(), 0.f) + 0.01f;
				FTimerDelegate RetryDel;
				RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
				GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, Delay, false);
				bCrossModeRetryArmed[FireModeNum] = true;   // legacy PutDown graduation must skip this arm
				if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] mode=%d CROSS-MODE IsFiring -> retry queued in %.3fs (stall fix)"), FireModeNum, Delay);
				// OnMultiPress only on the PHYSICAL press — a retry re-entry that lands while
				// the state is still firing re-arms above but must not re-trigger the hook
				// (stock fires it once per press; a re-triggering retry would mode-cycle/spam
				// any weapon whose OnMultiPress does work, e.g. RL-style charged states).
				if (!bHandlingRetry)
				{
					OnMultiPress(FireModeNum);
				}
				return;
			}
			if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] mode=%d CROSS-MODE IsFiring -> clear PendingFire + OnMultiPress + RETURN (no fire, NO retry queued)"), FireModeNum);
			if (UTOwner) UTOwner->SetPendingFire(FireModeNum, false);
			OnMultiPress(FireModeNum);
			return;
		}
	}

    // ---------------------------------------------------------
    // 5. CHARGED STATE ENTRY
    // ---------------------------------------------------------
    // Safe to run now because we validated cooldowns at the top.
    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Charged")))
        {

            FireModeActiveState[FireModeNum] = 1;
            CurrentlyFiringMode = FireModeNum;
            Super::StartFire(FireModeNum);
            return;
        }
    }

    // ---------------------------------------------------------
    // 6. STANDARD FIRING LOGIC
    // ---------------------------------------------------------

    // Clean up stale flags
    if (EarliestFireTime > CurrentTime)
    {
        // DIAGNOSTIC (net-safe, survives Shipping): a normal weapon-switch / put-down penalty is
        // sub-second. An EarliestFireTime block of >1s is the silent rocket no-reg pathology — this
        // path returns with NO other log, so surface it server-side to name the gate + value on a repro.
        if (Role == ROLE_Authority && (EarliestFireTime - CurrentTime) > 1.0f)
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s StartFire mode %d blocked %.2fs by EarliestFireTime=%.2f (now=%.2f)"),
                *GetName(), FireModeNum, EarliestFireTime - CurrentTime, EarliestFireTime, CurrentTime);
        }

        // 1. Preserve the user's input so they don't have to click again
        if (UTOwner)
        {
            UTOwner->SetPendingFire(FireModeNum, true);
        }

        // 2. Schedule a retry timer for exactly when the penalty expires
        if (FireModeNum < 2)
        {
            float Delay = EarliestFireTime - CurrentTime;

            // Only set the timer if one isn't already running
            if (Delay > 0.f && !GetWorldTimerManager().IsTimerActive(RetryFireHandle[FireModeNum]))
            {
                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                // Add a tiny buffer (0.01s) to ensure the next frame's check passes
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, Delay + 0.01f, false);
                bCrossModeRetryArmed[FireModeNum] = false;   // same-mode arm owns the handle now
            }
        }

        // 3. Block this immediate attempt
        return;
    }

    if (GetCurrentState() == ActiveState && CurrentlyFiringMode != 255)
    {
        CurrentlyFiringMode = 255;
        for (int32 i = 0; i < FireModeActiveState.Num(); i++)
        {
            FireModeActiveState[i] = 0;
        }
    }

    // Prevent re-entry if already firing this mode.
    // BUT: if we're in FiringState with a deferred GotoActiveState timer running,
    // the player tapped and is now re-pressing. Cancel the deferred timer,
    // transition to ActiveState, then fall through to start the new fire sequence.
    if (FiringState.IsValidIndex(FireModeNum) && CurrentState == FiringState[FireModeNum])
    {
        if (GetWorldTimerManager().IsTimerActive(DeferredActiveStateHandle))
        {
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StartFire] Mode %d: Re-fire during deferred cooldown — cancelling timer, transitioning to ActiveState"), FireModeNum);
            GetWorldTimerManager().ClearTimer(DeferredActiveStateHandle);
            GotoActiveState();
            // Fall through — ActiveState will now allow BeginFiringSequence below
        }
        else
        {
            return;
        }
    }

    // Set Active State Flags
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 1;
        CurrentlyFiringMode = FireModeNum;
    }

    // Set Input Flag
    if (UTOwner)
    {
      UTOwner->SetPendingFire(FireModeNum, true);
    }

	// --- FIX: AUTHORIZE LOGICAL SHOTS ---
		// If the server calls StartFire (e.g. from Equipping State finishing),
		// we must flag it as Transactional so the Gatekeeper lets it through.
	if (Role == ROLE_Authority)
	{
		bIsTransactionalFire = true;
	}

	BeginFiringSequence(FireModeNum, false);

	if (Role == ROLE_Authority)
	{
		bIsTransactionalFire = false;
	}
}






void AUTWeaponFix::FireShot()
{
	// --- REPLAY PLAYBACK: skip all NC prediction/rewind, use stock behavior ---
	// During instant replay, there's no server to do the rewind dance with.
	// Fake projectile handoff, ServerStartFireFixed RPCs, and ClientHitChar
	// prediction all break in replay context. Stock FireShot just spawns the
	// visual projectile directly which is all replay needs.
	UWorld* ReplayWorld = GetWorld();
	if (ReplayWorld && ReplayWorld->DemoNetDriver && ReplayWorld->DemoNetDriver->IsPlaying())
	{
		Super::FireShot();
		return;
	}

	// --- CLIENT SIDE ---
	if (Role < ROLE_Authority)
	{
		// (Keep existing Client Logic unchanged)
		UWorld* World = GetWorld();
		if (!World) return;

		int32 NextEventIndex = GetNextClientFireEventIndex(CurrentFireMode);
		if (ClientFireEventIndex.IsValidIndex(CurrentFireMode))
			ClientFireEventIndex[CurrentFireMode] = NextEventIndex;

        if (LastFireTime.IsValidIndex(CurrentFireMode))
        {
            float CurrentTime = World->GetTimeSeconds();
            float Refire = GetRefireTime(CurrentFireMode);
            float OldTime = LastFireTime[CurrentFireMode];

            // If this isn't the first shot, and we haven't paused firing for a long time...
            if (OldTime > 0.0f && (CurrentTime - OldTime) < (Refire + 0.06f))
            {
                // Snap the timer to the Theoretical Time.
                // Even if we fired 0.08s early, the clock is set as if we fired on time.
                // The NEXT shot will be calculated relative to this Theoretical Time.
                LastFireTime[CurrentFireMode] = OldTime + Refire;
            }
            else
            {
                // First shot or resuming after a pause, reset clock to Now.
                LastFireTime[CurrentFireMode] = CurrentTime;
            }
        }
		FRotator ClientRot = GetUTOwner() ? GetUTOwner()->GetViewRotation() : FRotator::ZeroRotator;
		//EarliestFireTime = 0.f;

		uint8 ZOffset = 0;
		if (UTOwner)
		{
			float RawOffset = UTOwner->GetPawnViewLocation().Z - UTOwner->GetActorLocation().Z;
			float DefaultOffset = UTOwner->BaseEyeHeight;
			if (!FMath::IsNearlyEqual(RawOffset, DefaultOffset, 1.0f))
			{
				ZOffset = (uint8)FMath::Clamp(RawOffset + 127.5f, 0.f, 255.f);
			}
		}

		AUTCharacter* ClientHitChar = nullptr;
		FVector ClientHeadOffset = FVector::ZeroVector;
		if (bTrackHitScanReplication && InstantHitInfo.IsValidIndex(CurrentFireMode) &&
			InstantHitInfo[CurrentFireMode].DamageType != NULL &&
			InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.0f)
		{
			const FVector SpawnLocation = GetFireStartLoc();
			const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
			const FVector FireDir = SpawnRotation.Vector();
			const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

			FHitResult PreHit;
			HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, PreHit, 0.0f);
			ClientHitChar = Cast<AUTCharacter>(PreHit.Actor.Get());

			// [HitAttrib.Client] capture: the anchor-trace result BEFORE the visual
			// filter, and the visual offset/missBy the filter measured.
			AUTCharacter* AttribPretraceHit = ClientHitChar;
			FVector AttribVisualOffset = FVector::ZeroVector;
			float AttribVisualMissBy = 0.0f;

			// ClientHitChar must mean "the ray crossed what I saw", not merely
			// "the ray crossed the invisible replicated actor anchor". TeamArenaCharacter's
			// network smoothing can leave the rendered mesh behind that anchor at high ping.
			if (ClientHitChar != nullptr)
			{
				FVector VisualOffset = FVector::ZeroVector;
				float VisualMissBy = 0.0f;
				const bool bVisualCapsuleHit = ShotIntersectsRenderedCapsule(
					ClientHitChar, SpawnLocation, EndTrace,
					InstantHitInfo[CurrentFireMode].TraceHalfSize, VisualOffset, VisualMissBy);
				AttribVisualOffset = VisualOffset;
				AttribVisualMissBy = VisualMissBy;
				const int32 VisualClaimDebug = CVarVisualHitscanClaimDebug.GetValueOnGameThread();

				if (!bVisualCapsuleHit)
				{
					if (VisualClaimDebug >= 1)
					{
						UE_LOG(LogUTWeaponFix, Warning,
							TEXT("[VisualClaim] REJECT %s actor candidate: visual offset=%s, missed rendered capsule by %.2fuu"),
							*ClientHitChar->GetName(), *VisualOffset.ToString(), VisualMissBy);
					}
					ClientHitChar = nullptr;
				}
				else if (VisualClaimDebug >= 2)
				{
					UE_LOG(LogUTWeaponFix, Warning,
						TEXT("[VisualClaim] ACCEPT %s actor candidate: visual offset=%s, visual margin=%.2fuu"),
						*ClientHitChar->GetName(), *VisualOffset.ToString(), -VisualMissBy);
				}
			}

			// 327 client-informed headshot (SECURE): report WHERE the client rendered the target's head —
			// the offset of its rendered mesh head bone from the target's body — but only when the shot
			// actually passed through that rendered head (normal radius). Under Force Models the client
			// renders the forced model's own head mesh here, so this IS "the head I saw". The server clamps
			// it into the head band and uses a normal sphere (see UTPlusSniper), so it can't be abused.
			// Zero = no claim (an honest head offset is never zero — the head is always above body centre).
			if (ClientHitChar != nullptr && ClientHitChar->GetMesh())
			{
				const FVector RenderedHead = ClientHitChar->GetMesh()->GetSocketLocation(ClientHitChar->HeadBone)
					+ FVector(0.f, 0.f, ClientHitChar->HeadHeight);
				const bool bThroughHead = FMath::PointDistToLine(RenderedHead, FireDir, PreHit.Location)
					< ClientHitChar->HeadRadius * ClientHitChar->HeadScale;
				if (bThroughHead)
				{
					ClientHeadOffset = RenderedHead - ClientHitChar->GetActorLocation();
				}
			}

			// [HitAttrib.Client] shooter-side half of the attribution table, same
			// cvar as the server line: what the anchor pre-trace crossed, how far
			// the rendered mesh sat from the replicated anchor (visOffset — the
			// smoothing term the claim filter corrects; animation pose is NOT in
			// it), the filter's missBy vs the rendered capsule, and the claim that
			// was actually sent. Lands in the SHOOTER's client log.
			if (CVarHitAttribDebug.GetValueOnGameThread() > 0)
			{
				const float ClientPing = (UTOwner && UTOwner->PlayerState) ? UTOwner->PlayerState->ExactPing : 0.f;
				const FString ShooterName = (UTOwner && UTOwner->PlayerState) ? UTOwner->PlayerState->PlayerName
					: (UTOwner ? UTOwner->GetName() : FString(TEXT("unknown")));
				const FString PretraceName = AttribPretraceHit
					? (AttribPretraceHit->PlayerState ? AttribPretraceHit->PlayerState->PlayerName : AttribPretraceHit->GetName())
					: FString(TEXT("none"));
				const TCHAR* ClaimSent = (ClientHitChar == nullptr)
					? TEXT("none") : (!ClientHeadOffset.IsZero() ? TEXT("head") : TEXT("body"));
				UE_LOG(LogUTWeaponFix, Log,
					TEXT("[HitAttrib.Client] shooter=%s ping=%.0f wep=%s mode=%d pretraceHit=%s speed=%.0f visOffset=%.1f visMissBy=%.1f claimSent=%s"),
					*ShooterName, ClientPing, *GetClass()->GetName(), CurrentFireMode,
					*PretraceName,
					AttribPretraceHit ? AttribPretraceHit->GetVelocity().Size() : 0.f,
					AttribVisualOffset.Size(), AttribVisualMissBy, ClaimSent);
			}
		}

		// Client-side hitsound prediction for hitscan weapons
		if (ClientHitChar != nullptr && Role != ROLE_Authority)
		{
			AClientHitsounds* HitsoundsMut = FindClientHitsoundsMutator();
			if (HitsoundsMut)
			{
				int32 EstDamage = InstantHitInfo.IsValidIndex(CurrentFireMode) ? InstantHitInfo[CurrentFireMode].Damage : 0;
				HitsoundsMut->PlayClientPredictedHitsound(EstDamage);
			}
		}

		const float ClientTimestamp = GetWorld()->GetGameState()->GetServerWorldTimeSeconds();
		ServerStartFireFixed(CurrentFireMode, NextEventIndex, ClientTimestamp,
			ClientRot, ClientHitChar, ZOffset, ClientHeadOffset);
        QueueResendStartFireFixed(CurrentFireMode, NextEventIndex, ClientTimestamp,
            ClientRot, ClientHitChar, ZOffset, ClientHeadOffset);

		// Cache the client's exact aim direction at fire-press time.
		// GetBaseFireRotation() will use this for the fake projectile spawn,
		// ensuring the fake fires exactly where the crosshair was — no curve from
		// mouse movement between fire input and SpawnNetPredictedProjectile call.
		CachedTransactionalRotation = ClientRot;
		Super::FireShot();
		CachedTransactionalRotation = FRotator::ZeroRotator; // Clear after spawn
	}
	else
		// --- SERVER SIDE ---
	{
		// 1. GATEKEEPER LOGIC
		bool bInChargedState = false;
		if (CurrentState != nullptr)
		{
			if (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()))
			{
				bInChargedState = true;
			}
		}

		// Fix: Allow shots if State Machine is actively firing (handles "Queued from Equip" shots)
		bool bIsStateFiring = (CurrentState && CurrentState->IsFiring());
		bool bIsListenServerHost = (UTOwner && UTOwner->IsLocallyControlled());

		if (!bIsTransactionalFire && !bNetDelayedShot && !bIsListenServerHost && !bInChargedState && !bIsStateFiring)
		{
			UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireShot] GATEKEEPER BLOCKED Mode %d. Trans=%d Delayed=%d Listen=%d Charged=%d StateFiring=%d"),
				CurrentFireMode, bIsTransactionalFire, bNetDelayedShot, bIsListenServerHost, bInChargedState, bIsStateFiring);
			return;
		}

		// 2. RHYTHM COMPENSATION & TIMESTAMP UPDATE
		if (LastFireTime.IsValidIndex(CurrentFireMode))
		{
			float CurrentTime = GetWorld()->GetTimeSeconds();
			float Refire = GetRefireTime(CurrentFireMode);
			float OldTime = LastFireTime[CurrentFireMode];

			// If this is the first shot (OldTime <= 0) OR if the player stopped firing for a while,
			// reset the clock to NOW.
			// (Tolerance: If gap is > Refire + 0.06s, assume they stopped firing).
			if (OldTime <= 0.0f || (CurrentTime - OldTime) > (Refire + 0.06f))
			{
				LastFireTime[CurrentFireMode] = CurrentTime;
			}
			else
			{
				// We are firing continuously. Apply Rhythm Compensation.
				float TheoreticalTime = OldTime + Refire;

				// If the actual fire time is close to the theoretical time (within 200ms jitter),
				// we snap the timer to the Theoretical Time.
				// This ensures that network jitter doesn't lower the player's DPS over time.
				// CRITICAL: Cap to CurrentTime on the server so LastFireTime never ends up
				// in the future. A future LastFireTime poisons the next ValidateFireRequest
				// check with a negative delta, causing spurious rejections.
				if (CurrentTime < TheoreticalTime + 0.2f)
				{
					LastFireTime[CurrentFireMode] = (Role == ROLE_Authority)
						? FMath::Min(TheoreticalTime, CurrentTime)
						: TheoreticalTime;
				}
				else
				{
					// The delay was too large to be jitter (lag spike or pause). Reset to Now.
					LastFireTime[CurrentFireMode] = CurrentTime;
				}
			}
		}

		// 3. SPAWN PROJECTILE
		UE_LOG(LogUTWeaponFix, Verbose, TEXT("[FireShot] Server spawning Mode %d projectile"), CurrentFireMode);
		Super::FireShot();
	}
}



void AUTWeaponFix::StopFire(uint8 FireModeNum)
{
    if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] StopFire mode=%d curFiring=%d"), FireModeNum, CurrentlyFiringMode);

    // Mouse-bounce debounce: stamp the release time so the next StartFire
    // within MouseDebounceWindow is recognised as a bounce, not a new click.
    if (LastReleaseTime.IsValidIndex(FireModeNum))
    {
        LastReleaseTime[FireModeNum] = GetWorld()->GetTimeSeconds();
    }

    if (UTOwner)
    {
        // Only clear pending fire if user actually released the button, not if switching weapons
        // The new weapon needs to see this flag to auto-fire when it becomes active
        bool bIsSwitchingWeapons = UTOwner->GetPendingWeapon() != nullptr;
        if (!bIsSwitchingWeapons)
        {
            UTOwner->SetPendingFire(FireModeNum, false);
            // GHOST FIX: a genuine (non-switch) release ends held intent. Mirrors the
            // PendingFire clear so an internal stop DURING a switch (held swap) does
            // not falsely clear it and break hold-through-switch.
            if (GhostFix() && FireModeNum < 2) { bFireHeldByPlayer[FireModeNum] = false; }
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Clearing PendingFire %d"), FireModeNum);
        }
    }
    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    }

	// We must clean these flags BEFORE any early returns.
	// Otherwise, the weapon gets stuck thinking it is "Active" in Mode 1.
	if (FireModeActiveState.IsValidIndex(FireModeNum))
	{
		FireModeActiveState[FireModeNum] = 0;
	}

	if (CurrentlyFiringMode == FireModeNum)
	{
		CurrentlyFiringMode = 255;
	}

    if (FiringState.IsValidIndex(FireModeNum))
    {
        if (FiringState[FireModeNum] &&
            FiringState[FireModeNum]->IsA(UUTWeaponStateZooming::StaticClass()))
        {
            //UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d is Zoom – bypassing transactional stop"), FireModeNum);
            Super::StopFire(FireModeNum);
            return;
        }
    }
    
    bool bIsChargedMode = false;

    // Check if the mode we are stopping is configured as a Charged State
    if (FiringState.IsValidIndex(FireModeNum) &&
        FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()))
    {
        bIsChargedMode = true;
    }

    // Check if the weapon is ACTUALLY in a Charged State right now
    if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
        CurrentState->GetName().Contains(TEXT("Charged"))))
    {
        bIsChargedMode = true;
    }
    if (bIsChargedMode)
    {
        if (FireModeNum == 1)
        {
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Bypassing Transactional Stop for Charged State (Mode 1)"));
        }
        // CLIENT: Execute locally AND send transactional packet with rotation
        if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
        {
            // 1. Local execution only (no Epic RPC)
            //EndFiringSequence(FireModeNum);
            Super::StopFire(FireModeNum);
            // 2. Increment index (charged weapons skip FireShot)
            int32 EventIndex = 0;
            if (ClientFireEventIndex.IsValidIndex(FireModeNum))
            {
                ClientFireEventIndex[FireModeNum]++;
                EventIndex = ClientFireEventIndex[FireModeNum];
            }

            // 3. Send the transactional stop and queue identical retries.
            ServerStopFireFixed(FireModeNum, EventIndex);
            QueueResendStopFireFixed(FireModeNum, EventIndex);
        }
        else
        {
            // SERVER/Listen host: Just execute locally
            EndFiringSequence(FireModeNum);
        }

        return;
    }
   //if (FireModeNum < 2)
    //{
    //    GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    //}

    // Guard: only call EndFiringSequence if we're actually in the firing state
    // for this mode. Stock EndFiringSequence dispatches to CurrentState->EndFiringSequence(),
    // so calling it when in the wrong state (e.g. a new StartFire arrived, or PutDown
    // already started) would land on the wrong state object.
    if (FiringState.IsValidIndex(FireModeNum) && GetCurrentState() == FiringState[FireModeNum])
    {
        UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d: In FiringState — EndFiringSequence + kill RefireCheckTimer + defer GotoActiveState"), FireModeNum);

        // Clean up firing effects and PendingFire immediately (not deferred).
        EndFiringSequence(FireModeNum);

        // Kill the RefireCheckTimer — EndState won't run yet (deferred), so the
        // timer is still alive and would cause double-fire overlap.
        UUTWeaponStateFiring* FiringStateObj = Cast<UUTWeaponStateFiring>(FiringState[FireModeNum]);
        if (FiringStateObj)
        {
            GetWorldTimerManager().ClearTimer(FiringStateObj->RefireCheckHandle);
        }

        // Only defer the STATE TRANSITION (GotoActiveState). This keeps the weapon
        // in FiringState during cooldown so PutDown() routes to the cooldown-aware
        // override in UUTWeaponStateFiring_Transactional.
        float CurrentTime = GetWorld()->GetTimeSeconds();
        float ReadyTime = 0.f;

        if (LastFireTime.IsValidIndex(FireModeNum))
        {
            ReadyTime = LastFireTime[FireModeNum] + GetRefireTime(FireModeNum);
        }

        float TimeRemaining = ReadyTime - CurrentTime;

        if (TimeRemaining > 0.01f)
        {
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d: Deferring GotoActiveState by %.3fs"), FireModeNum, TimeRemaining);
            FTimerDelegate Del;
            Del.BindUObject(this, &AUTWeaponFix::DeferredGotoActiveState, FireModeNum);
            GetWorldTimerManager().SetTimer(DeferredActiveStateHandle, Del, TimeRemaining, false);
        }
        else
        {
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d: Cooldown elapsed — immediate GotoActiveState"), FireModeNum);
            GotoActiveState();
        }
    }
    else
    {
        UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d: NOT in FiringState (State=%s) — clearing PendingFire only"),
            FireModeNum, GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"));
        if (UTOwner)
        {
            UTOwner->SetPendingFire(FireModeNum, false);
        }
    }

    if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
    {
        int32 EventIndex = ClientFireEventIndex.IsValidIndex(FireModeNum) ?
            ClientFireEventIndex[FireModeNum] : 0;
        ServerStopFireFixed(FireModeNum, EventIndex);
        QueueResendStopFireFixed(FireModeNum, EventIndex);
    }
    
}

bool AUTWeaponFix::ValidateFireRequest(uint8 FireModeNum, int32 InEventIndex, float ClientTime)
{
    // Critical Fix #5: Multi-layer validation
    // Get player name for logging (do this once at the top)
	FString PlayerName = TEXT("Unknown");
	if (UTOwner && UTOwner->Controller)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
		if (PC && PC->PlayerState)
		{
			PlayerName = PC->PlayerState->PlayerName;
		}
	}
    // Validate fire mode
    if (!FireModeActiveState.IsValidIndex(FireModeNum))
    {
        return false;
    }

    // Validate event sequence
    if (!IsFireEventSequenceValid(FireModeNum, InEventIndex))
    {
        int32 LastProcessed = AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : -1;
        UE_LOG(LogUTWeaponFix, Warning, TEXT("Shot rejected for %s: [Server] STALE EVENT. Mode %d EventIndex %d vs LastProcessed %d"),
            *PlayerName, FireModeNum, InEventIndex, LastProcessed);
        return false;
    }

    // Validate timing with network tolerance
    float ServerTime = GetWorld()->GetTimeSeconds();
    float TimeDiff = FMath::Abs(ServerTime - ClientTime);

    // Allow reasonable network delay but reject obviously wrong timestamps
    if (TimeDiff > 1.0f) // 1 second tolerance should be more than enough
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponFix: Rejected fire due to time desync: %f"), TimeDiff);
        return false;
    }

    /* Check refire rate
    if (LastFireTime.IsValidIndex(FireModeNum) && LastFireTime[FireModeNum] > 0.0f)
    {
        float TimeSinceLastFire = ServerTime - LastFireTime[FireModeNum];
        float MinInterval = GetRefireTime(FireModeNum) - 0.06f; // 50ms network tolerance

        if (TimeSinceLastFire < MinInterval)
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[Server] REJECTED Rapid Fire. Delta: %.3f < Min: %.3f"), TimeSinceLastFire, MinInterval);
            
            return false;
        }
    }
    */
    // SAME-MODE refire check only. Stock UT4 has independent per-mode cooldowns.
    // Cross-mode blocking prevents legitimate alternating fire (shock primary→secondary).
    // The weapon state machine already prevents simultaneous firing.
    if (LastFireTime.IsValidIndex(FireModeNum) && LastFireTime[FireModeNum] > 0.0f)
    {
        float TimeSinceLastFire = ServerTime - LastFireTime[FireModeNum];

        // Scaled jitter tolerance (15% of refire, floored 15ms, capped 40ms).
        // The old fixed 0.15s allowed fast-fire weapons (minigun 0.10s, link
        // beam 0.12s, shock 0.15s) to be fired well above ROF — the tolerance
        // was often larger than the refire window itself, disabling validation.
        // Matches the authority branch of IsFireModeOnCooldown so client and
        // server agree on what counts as rapid fire.
        const float RefireTime = GetRefireTime(FireModeNum);
        const float JitterTolerance = FMath::Clamp(RefireTime * 0.15f, 0.015f, 0.04f);
        float MinInterval = RefireTime - JitterTolerance;

        // Use < with SMALL_NUMBER epsilon to avoid floating-point edge case where
        // rhythm compensation snaps Delta to exactly MinInterval (e.g., 0.550 < 0.550).
        if (TimeSinceLastFire < MinInterval - SMALL_NUMBER)
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("Shot rejected for %s: [Server] REJECTED Rapid Fire. Mode %d. Delta: %.3f < Min: %.3f"),
                *PlayerName, FireModeNum, TimeSinceLastFire, MinInterval);
            return false;
        }
    }

    // CRITICAL: Update AuthoritativeFireEventIndex HERE, atomically with the check.
    // UE4 processes all queued RPCs in a batch within one server frame. If the
    // original fire + a resend both arrive on the same frame, they both pass
    // IsFireEventSequenceValid BEFORE either updates the index in
    // ServerStartFireFixed_Implementation. This causes the server to fire twice
    // for the same event — the anti-dup guard blocks the second spawn, leaving
    // the client's second fake with no auth to pair against.
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        AuthoritativeFireEventIndex[FireModeNum] = InEventIndex;
    }

    return true;
}




bool AUTWeaponFix::IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime)
{
    // CHECK 1: Weapon switch penalty (EarliestFireTime)
    if (EarliestFireTime > CurrentTime)
    {
        return true;
    }

    // Client: essentially strict — prevents tap-fire from beating hold-fire.
    //   The OnRetryTimer in StartFire queues an early click to the next valid
    //   fire window instead of rejecting it outright, so responsiveness is
    //   preserved — the click just fires exactly at ROF, not before.
    // Server: scaled tolerance for network jitter absorption, floored so fast
    //   weapons still have some slack and capped so slow weapons don't get
    //   disproportionate leniency. Replaces the old fixed 0.15s that made
    //   minigun/link-beam server-side validation effectively unreachable.
    const float RequiredInterval = GetRefireTime(FireModeNum);
    const float Tolerance = (Role == ROLE_Authority)
        ? FMath::Clamp(RequiredInterval * 0.15f, 0.015f, 0.04f)
        : SMALL_NUMBER;

    // SAME-MODE COOLDOWN CHECK ONLY.
    // Stock UT4 has independent per-mode cooldowns — the state machine prevents
    // simultaneous firing, but nothing stops secondary immediately after primary.
    // A cross-mode check here blocks legitimate alternating fire (e.g., shock
    // primary then secondary) because the release kills the retry timer before
    // the other mode's cooldown expires.
    if (LastFireTime.IsValidIndex(FireModeNum) && LastFireTime[FireModeNum] > 0.0f)
    {
        float TimeSinceLastFire = CurrentTime - LastFireTime[FireModeNum];

        if (TimeSinceLastFire < (RequiredInterval - Tolerance))
        {
            return true;
        }
    }

    return false;
}





int32 AUTWeaponFix::GetNextClientFireEventIndex(uint8 FireModeNum)
{
    if (!ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        return 1;
    }

    // Critical Fix #6: Use int32 to prevent overflow issues
    return ClientFireEventIndex[FireModeNum] + 1;
}

bool AUTWeaponFix::IsFireEventSequenceValid(uint8 FireModeNum, int32 InEventIndex)
{
    if (!AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        return true; // First event is always valid
    }

    // Event must be newer than last processed, but not too far ahead
    int32 LastProcessed = AuthoritativeFireEventIndex[FireModeNum];
    return (InEventIndex > LastProcessed) && (InEventIndex <= LastProcessed + 10);
}



void AUTWeaponFix::ServerStartFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp,
    FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset)
{
    // 1. VALIDATION (Your existing transactional checks)
    UWorld* World = GetWorld();
    if (!World) return;

    // Server-authoritative fire policy (e.g. single-rocket-only loadouts). Reject a
    // vetoed mode HERE — before the trade-kill spawn, the SetPendingFire latch below, and
    // any state entry — so a modified client cannot latch PendingFire and let stock
    // UUTWeaponStateActive::BeginState auto-enter the firing state. The resend RPC funnels
    // back through this function, so this covers that path too.
    if (!AllowServerFireMode(FireModeNum))
    {
        return;
    }

    // --- TRADE-KILL GRACE PERIOD (projectiles only) ---
    // If UTOwner is null (weapon removed on death) but within the grace window,
    // spawn the projectile directly using the cached position from Removed().
    // Only applies to projectile weapons (rockets, shock balls, etc.) — not hitscan.
    // Specifically important for loaded rockets that were released just before death.
    if (!UTOwner && OwnerLostTime > 0.f)
    {
        float TimeSinceDeath = World->GetTimeSeconds() - OwnerLostTime;
        if (TimeSinceDeath <= TradeKillGracePeriod && ProjClass.IsValidIndex(FireModeNum) && ProjClass[FireModeNum])
        {
            FVector SpawnLoc = CachedFireStartLoc;
            FRotator SpawnRot = ClientViewRot.IsZero() ? CachedFireRotation : ClientViewRot;
            FActorSpawnParameters Params;
            Params.Instigator = Instigator;
            Params.Owner = this;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AUTProjectile* Proj = World->SpawnActor<AUTProjectile>(ProjClass[FireModeNum], SpawnLoc, SpawnRot, Params);
            if (Proj)
            {
                UE_LOG(LogUTWeaponFix, Log, TEXT("[TradeKill] Spawned projectile %.0fms after death (Mode %d)"),
                    TimeSinceDeath * 1000.f, FireModeNum);
            }
            OwnerLostTime = 0.f; // only one grace shot
            return;
        }
        UE_LOG(LogUTWeaponFix, Log, TEXT("[TradeKill] REJECTED: %.0fms after death exceeds %.0fms grace (Mode %d, HasProj=%d)"),
            TimeSinceDeath * 1000.f, TradeKillGracePeriod * 1000.f, FireModeNum,
            (ProjClass.IsValidIndex(FireModeNum) && ProjClass[FireModeNum]) ? 1 : 0);
        return;
    }

    if (!ValidateFireRequest(FireModeNum, InFireEventIndex, ClientTimestamp))
    {
        ClientConfirmFireEvent(FireModeNum, AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : 0);
        return;
    }
    CachedTransactionalRotation = ClientViewRot;
    if (ZOffset != 0)
    {
        // Decode byte back to float
        FireZOffset = ZOffset - 127;
        // IMPORTANT: Set time to NOW so GetFireStartLoc picks it up
        FireZOffsetTime = GetWorld()->GetTimeSeconds();
    }
    else
    {
        FireZOffset = 0;
        FireZOffsetTime = 0.f;
    }
    if (ClientHitChar != nullptr && bTrackHitScanReplication)
    {
        ReceivedHitScanHitChar = ClientHitChar;
        // InFireEventIndex matches FireEventIndex, so (ReceivedHitScanIndex == FireEventIndex) check passes
        ReceivedHitScanIndex = (uint8)InFireEventIndex;
        ReceivedHeadOffset = ClientHeadOffset;   // 327: client's rendered head position; clamped + bounded by the headshot gate
    }
    else
    {
        ReceivedHitScanHitChar = nullptr;
        ReceivedHitScanIndex = 0;
        ReceivedHeadOffset = FVector::ZeroVector;
    }

    // 2. UPDATE STATE
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum)) {

        AuthoritativeFireEventIndex[FireModeNum] = InFireEventIndex;
        FireEventIndex = (uint8)InFireEventIndex;
    }

    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 1;
        CurrentlyFiringMode = FireModeNum;
    }


    TargetedCharacter = nullptr; // Clear Weapon's cached target
    if (UTOwner && UTOwner->Controller)
    {
        AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
        if (PC)
        {
            PC->LastShotTargetGuess = nullptr; // Clear Controller's cached target
        }
    }

    if (UTOwner)
    {
        UTOwner->SetPendingFire(FireModeNum, true);
    }
    // FIX: Cancel any deferred ActiveState transition from a previous stop.
    // Without this, the old timer fires mid-sequence and triggers a ghost shot
    // via ActiveState::BeginState's PendingFire auto-fire check.
    GetWorldTimerManager().ClearTimer(DeferredActiveStateHandle);
    bIsTransactionalFire = true;

    // WEDGE FAST-RECOVERY (event-driven half; the Tick watchdog covers the no-input case):
    // an empty wedged charged state would swallow this Start silently — the transactional
    // cast below fails (the charged state inherits stock UUTWeaponStateFiring, not the
    // transactional state), so dispatch lands in the charged state's inherited stock
    // BeginFiringSequence, which just records PendingFireSequence and returns: no
    // projectile, no reject, no log (sassin's eaten primaries, 2026-07-24). Recover NOW so
    // THIS press fires instead of waiting for the watchdog. Clear both pending flags first
    // so ActiveState::BeginState can't auto-enter a firing state and double-fire before the
    // normal dispatch below re-latches and enters cleanly; GotoActiveState NOT StopFire
    // (StopFire re-enters the charged EndFiringSequence and re-wedges).
    if (UUTWeaponStateFiringChargedRocket_Transactional* WedgedChg = Cast<UUTWeaponStateFiringChargedRocket_Transactional>(GetCurrentState()))
    {
        if (IsChargedRocketStateWedged(WedgedChg))
        {
            AUTPlusWeap_RocketLauncher* WedgeRL = Cast<AUTPlusWeap_RocketLauncher>(this);
            const int32 WedgeLoaded = WedgeRL ? FMath::Max(WedgeRL->NumLoadedRockets, WedgeRL->NumLoadedBarrels) : 0;
            if (WedgeLoaded > 0)
            {
                // LOADED wedge (believed unreachable): NEVER GotoActiveState here — charged
                // EndState() zeroes NumLoadedRockets/Barrels, eating the volley. Force the
                // release through the state's OWN burst path; the burst arms
                // FireLoadedRocketHandle so the state is busy (un-wedged) immediately. This
                // Start then proceeds to normal dispatch below, is recorded as the stock
                // PendingFireSequence with pawn PendingFire latched (set above), and the
                // charged RefireCheckTimer's post-burst primary handoff fires it AFTER the
                // volley — never both in the same frame.
                uint8 WedgeChargedMode = 1;
                for (int32 i = 0; i < FiringState.Num(); i++) { if (FiringState[i] == WedgedChg) { WedgeChargedMode = (uint8)i; break; } }
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s (%s) LOADED wedged ChargedRocket (loaded=%d) — force-releasing volley; incoming Start mode=%d defers to post-burst"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    WedgeLoaded, FireModeNum);
                ChargedWedgeFirstSeenTime = -1.f;
                WedgedChg->EndFiringSequence(WedgeChargedMode);
            }
            else
            {
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s (%s) wedged ChargedRocket recovered by incoming Start mode=%d — firing it instead of swallowing"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    FireModeNum);
                ChargedWedgeFirstSeenTime = -1.f;
                // Hide the pending flags around GotoActiveState so ActiveState::BeginState
                // can't auto-enter a firing state and double-fire this Start (same
                // hide/restore pattern as the charged RefireCheckTimer primary handoff).
                // The OTHER mode's held intent is restored afterwards; the incoming mode is
                // re-latched by the normal dispatch below.
                const uint8 OtherMode = (FireModeNum == 0) ? 1 : 0;
                const bool bOtherModeWasPending = UTOwner && UTOwner->IsPendingFire(OtherMode);
                if (UTOwner)
                {
                    UTOwner->SetPendingFire(0, false);
                    UTOwner->SetPendingFire(1, false);
                }
                GotoActiveState();
                if (UTOwner && bOtherModeWasPending)
                {
                    UTOwner->SetPendingFire(OtherMode, true);
                }
                // Restore this shot's transactional bookkeeping (set above at the top of this
                // function, conceptually cleared by abandoning the wedged session).
                if (FireModeActiveState.IsValidIndex(FireModeNum))
                {
                    for (int32 i = 0; i < FireModeActiveState.Num(); i++) { FireModeActiveState[i] = (i == FireModeNum) ? 1 : 0; }
                    CurrentlyFiringMode = FireModeNum;
                }
            }
        }
    }

    // 3. EXECUTE FIRE (The New Logic)

    // Check if we are ALREADY in the transactional state (i.e., holding the button)
    UUTWeaponStateFiring_Transactional* TransState = Cast<UUTWeaponStateFiring_Transactional>(GetCurrentState());

    if (TransState && GetCurrentFireMode() == FireModeNum)
    {
        // STATE IS ACTIVE: Just trigger the next shot in the sequence.
        TransState->TransactionalFire();
    }
    else
    {
        // STATE IS INACTIVE: Enter the state.
        // If we're currently firing a DIFFERENT mode, stop it first — mirrors
        // the client's cross-mode fix in StartFire. Without this, Mode 1 Start
        // arriving while Mode 0 is still active calls BeginFiringSequence(1)
        // which delegates to FiringState[0]->BeginFiringSequence(1) → OnMultiPress
        // → no state transition → shot never fires → dud projectile.
        if (TransState && GetCurrentFireMode() != FireModeNum)
        {
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[ServerStartFireFixed] Cross-mode: stopping Mode %d before entering Mode %d"),
                GetCurrentFireMode(), FireModeNum);
            EndFiringSequence(GetCurrentFireMode());
            GotoActiveState();
        }
        // BeginState() inside the new class will fire the first shot automatically.
        // This RPC is sent from the client's predicted FireShot path, so the server
        // can derive the value instead of trusting a redundant wire parameter.
        BeginFiringSequence(FireModeNum, true);
    }

    bIsTransactionalFire = false;
    // Mirror the client-side clean-up at FireShot (line 696). Without this,
    // CachedTransactionalRotation stays alive between shots and any future
    // read with a stale gate would pick up the wrong rotation.
    CachedTransactionalRotation = FRotator::ZeroRotator;
	ReceivedHitScanHitChar = nullptr;

    // 4. CONFIRM — always sent, including shock balls. Keeps event indices synced
    // and clears the resend queue. Shock ball fakes are preserved in
    // ClientConfirmFireEvent_Implementation (not destroyed) to prevent the
    // 80+ ping visual hitch. See that function for details.
    if (UTOwner)
    {
        ClientConfirmFireEvent(FireModeNum, InFireEventIndex);
    }
}

void AUTWeaponFix::Removed()
{
	// Cache the owner's last known fire position before Super::Removed() nulls UTOwner.
	// This enables the trade-kill grace period — if a fire RPC arrives within
	// TradeKillGracePeriod after death, we can still spawn the projectile.
	if (UTOwner && Role == ROLE_Authority)
	{
		CachedFireStartLoc = GetFireStartLoc();
		CachedFireRotation = GetBaseFireRotation();
		OwnerLostTime = GetWorld()->GetTimeSeconds();

		// Force-fire loaded rockets on death. If the player was holding alt-fire
		// to load rockets and died before releasing, fire them now.
		AUTPlusWeap_RocketLauncher* RL = Cast<AUTPlusWeap_RocketLauncher>(this);
		if (RL && RL->NumLoadedBarrels > 0)
		{
			UE_LOG(LogUTWeaponFix, Log, TEXT("[DeathRelease] Firing %d loaded rockets on death"), RL->NumLoadedBarrels);
			// FireShot on the charged state will spawn all loaded rockets
			CurrentFireMode = 1; // alt-fire mode for loaded rockets
			Super::FireShot();
		}
	}
	Super::Removed();
}

bool AUTWeaponFix::IsChargedRocketStateWedged(UUTWeaponStateFiringChargedRocket_Transactional* Chg)
{
    if (Chg == nullptr || Chg->bCharging)
    {
        return false;
    }
    FTimerManager& TM = GetWorldTimerManager();
    return !TM.IsTimerActive(Chg->LoadTimerHandle)
        && !TM.IsTimerActive(Chg->GraceTimerHandle)
        && !TM.IsTimerActive(Chg->FireLoadedRocketHandle)
        && !TM.IsTimerActive(Chg->RefireCheckHandle);
}

void AUTWeaponFix::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // --- WATCHDOG UNLOCK ---
    // If the weapon is marked as firing a mode, but the state machine says we are "Active" (Idle),
    // it means the Charged State finished (rockets fired/loaded) and returned to idle
    // without explicitly clearing the CurrentlyFiringMode flag.
    // We must clear it here to unlock the weapon for the next shot.
    if (CurrentlyFiringMode != 255 && GetCurrentState() == ActiveState)
    {
        
        // Clean up the active state array as well just to be safe
        if (FireModeActiveState.IsValidIndex(CurrentlyFiringMode))
        {
            FireModeActiveState[CurrentlyFiringMode] = 0;
        }
		CurrentlyFiringMode = 255;
    }


    // WATCHDOG: Prevent a stuck firing state from hanging (client disconnect / lost Stop packet, OR
    // a WEDGED charged-rocket state silently swallowing primaries — the rocket-only ~20s no-reg).
    if (Role == ROLE_Authority && IsFiring())
    {
        if (UUTWeaponStateFiringChargedRocket_Transactional* Chg = Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState))
        {
            // A legitimately-active charged state always has one of these in flight (loading, grace,
            // mid-burst, or the post-burst refire wait) and self-transitions — leave it alone. A charged
            // state that is IsFiring() with NONE of them and not charging is WEDGED: it never self-
            // transitions, and an incoming primary routes through its inherited stock
            // UUTWeaponStateFiring::BeginFiringSequence which just sets PendingFireSequence and returns —
            // no projectile, no reject, no log. Only the rocket has this state, which is why the no-reg
            // is rocket-only.
            if (!IsChargedRocketStateWedged(Chg))
            {
                ChargedWedgeFirstSeenTime = -1.f;
                return;
            }

            // WEDGED. Stamp + root-cause snapshot on first observation (the wedge-ENTRY path
            // is still unidentified — this names what the state looked like the moment it
            // went dead), then debounce ~0.25s so a transient no-timer instant between state
            // callbacks can't false-trigger. (sassin's 2026-07-24 wedge ate primary clicks
            // for the full 2.5s under the old shared 2.5x-refire gate.)
            AUTPlusWeap_RocketLauncher* RL = Cast<AUTPlusWeap_RocketLauncher>(this);
            const int32 LoadedCount = RL ? FMath::Max(RL->NumLoadedRockets, RL->NumLoadedBarrels) : 0;
            const float Now = GetWorld()->GetTimeSeconds();
            if (ChargedWedgeFirstSeenTime < 0.f)
            {
                ChargedWedgeFirstSeenTime = Now;
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[WedgeArmed] %s (%s) charged state idle without exit: mode=%d CurFiring=%d loaded=%d pend0=%d pend1=%d sinceFire0=%.2fs sinceFire1=%.2fs pendingWpn=%d"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    CurrentFireMode, CurrentlyFiringMode, LoadedCount,
                    (UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
                    (UTOwner && UTOwner->IsPendingFire(1)) ? 1 : 0,
                    LastFireTime.IsValidIndex(0) ? Now - LastFireTime[0] : -1.f,
                    LastFireTime.IsValidIndex(1) ? Now - LastFireTime[1] : -1.f,
                    (UTOwner && UTOwner->GetPendingWeapon()) ? 1 : 0);
                return;
            }
            if (Now - ChargedWedgeFirstSeenTime < 0.25f)
            {
                return;
            }
            if (LoadedCount > 0)
            {
                // LOADED wedge (believed unreachable: loaded-and-waiting always has the grace
                // timer active). NEVER GotoActiveState with rockets loaded — charged EndState()
                // zeroes NumLoadedRockets/Barrels (UTWeaponStateFiringChargedRocket_
                // Transactional.cpp:106-109), silently eating the volley the player is owed.
                // Force the release through the state's OWN burst path instead: the burst arms
                // FireLoadedRocketHandle, so the state reads busy (un-wedged) next tick and
                // completes normally. Only if it is somehow STILL wedged+loaded after 1s of
                // attempts do we abandon as the last resort — loudly.
                if (Now - ChargedWedgeFirstSeenTime >= 1.0f)
                {
                    UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s abandoning a LOADED wedged ChargedRocket after failed force-release (loaded=%d) — volley lost"),
                        *GetName(), LoadedCount);
                    ChargedWedgeFirstSeenTime = -1.f;
                    CurrentlyFiringMode = 255;
                    for (int32 i = 0; i < FireModeActiveState.Num(); i++) { FireModeActiveState[i] = 0; }
                    GotoActiveState();
                    return;
                }
                uint8 ChargedMode = 1;
                for (int32 i = 0; i < FiringState.Num(); i++) { if (FiringState[i] == Chg) { ChargedMode = (uint8)i; break; } }
                UE_LOG(LogUTWeaponFix, Verbose, TEXT("[WedgeArmed] %s force-releasing %d loaded rockets (mode=%d)"), *GetName(), LoadedCount, ChargedMode);
                Chg->EndFiringSequence(ChargedMode);
                return;
            }
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s fast-recovered an empty WEDGED ChargedRocket state (mode=%d CurFiring=%d wedged=%.2fs)"),
                *GetName(), CurrentFireMode, CurrentlyFiringMode, Now - ChargedWedgeFirstSeenTime);
            ChargedWedgeFirstSeenTime = -1.f;
            CurrentlyFiringMode = 255;
            for (int32 i = 0; i < FireModeActiveState.Num(); i++) { FireModeActiveState[i] = 0; }
            GotoActiveState();   // NOT StopFire — StopFire re-enters the charged EndFiringSequence and re-wedges
            return;
        }
        else
        {
            ChargedWedgeFirstSeenTime = -1.f;
        }

        float RefireTime = GetRefireTime(CurrentFireMode);

        // If we haven't received a valid RPC in > 2.5x the refire time, assume connection loss.
        // (e.g., for Link Gun (0.12s), if silent for 0.3s, kill it).
        float TimeoutThreshold = FMath::Max(0.25f, RefireTime * 2.5f);

        // LastFireTime is updated in ServerStartFireFixed
        if (LastFireTime.IsValidIndex(CurrentFireMode) &&
            GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode] > TimeoutThreshold)
        {
            // Charged states never reach here — every wedge path above returns. This is the
            // generic stuck-firing watchdog (client disconnect / lost Stop) for the other
            // firing states only. Force stop: kills the looping audio and resets the state.
            StopFire(CurrentFireMode);
        }
    }
}


bool AUTWeaponFix::ValidateStartFireFixedPayload(uint8 FireModeNum, int32 InFireEventIndex,
    float ClientTimestamp, FRotator ClientViewRot, FVector ClientHeadOffset)
{
    // Sanity-bound the client head offset at the RPC edge. The headshot gate clamps it downstream, but a NaN
    // defeats FMath::Clamp (NaN fails every comparison) and that clamp is currently the sole defense, so reject
    // NaN/Inf or an absurd magnitude here. A legit offset is the rendered head relative to the body (~110u up),
    // so the 1000u bound is hugely generous — no honest client is ever caught; only a tampered one is dropped.
    if (ClientHeadOffset.ContainsNaN()
        || ClientHeadOffset.SizeSquared() > FMath::Square(1000.0f)
        || ClientViewRot.ContainsNaN())
    {
        return false;
    }
    return FireModeNum < GetNumFireModes() &&
        InFireEventIndex > 0 &&
        FMath::IsFinite(ClientTimestamp) &&
        ClientTimestamp > 0.0f;
}

bool AUTWeaponFix::ServerStartFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex,
    float ClientTimestamp, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset,
    FVector ClientHeadOffset)
{
    return ValidateStartFireFixedPayload(FireModeNum, InFireEventIndex, ClientTimestamp,
        ClientViewRot, ClientHeadOffset);
}




void AUTWeaponFix::ServerStopFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex)
{
    // Initial and retry RPCs carry the same stop. Process whichever arrives first,
    // then make later copies idempotent.
    if (LastProcessedStopEventIndex.IsValidIndex(FireModeNum)
        && InFireEventIndex <= LastProcessedStopEventIndex[FireModeNum])
    {
        return;
    }

    // A stop for an older press must not cancel a newer held fire. Also reject an
    // implausible jump instead of letting a client move the authoritative sequence
    // far enough ahead to suppress legitimate starts.
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        const int32 LastAuthoritativeIndex = AuthoritativeFireEventIndex[FireModeNum];
        if (InFireEventIndex < LastAuthoritativeIndex)
        {
            return;
        }
        if (int64(InFireEventIndex) > int64(LastAuthoritativeIndex) + 10)
        {
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[ServerStopFireFixed] Rejected sequence jump. Mode %d EventIndex %d vs LastProcessed %d"),
                FireModeNum, InFireEventIndex, LastAuthoritativeIndex);
            return;
        }
        AuthoritativeFireEventIndex[FireModeNum] = InFireEventIndex;
    }
    if (LastProcessedStopEventIndex.IsValidIndex(FireModeNum))
    {
        LastProcessedStopEventIndex[FireModeNum] = InFireEventIndex;
    }
    
    // 1. Clear authoritative state flags
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 0;
    }
    if (CurrentlyFiringMode == FireModeNum)
    {
        CurrentlyFiringMode = 255;
    }

    // Stop fire is the end of the transactional session — clear the cache so
    // no future read sees stale data. (The previous "inject then clear" block
    // here was dead: it set the cache, then unconditionally cleared it on the
    // very next lines.)
    bIsTransactionalFire = false;
    CachedTransactionalRotation = FRotator::ZeroRotator;

    // Server Stop honor (ncp.StopClearsPending, default ON — see the cvar comment for the
    // full rationale): a received Stop is authoritative notice that this fire mode is no
    // longer held, so clear the pawn flag REGARDLESS of weapon state. The state-gated
    // EndFiringSequence below stays as-is for firing/effect cleanup; this line is what the
    // guard was silently dropping when the Stop landed mid-switch. GhostFix() kept in the
    // gate so flipping that prototype on doesn't lose its old clear if this cvar is 0.
    if ((StopClearsPending() || GhostFix()) && UTOwner)
    {
        const bool bWasPending = UTOwner->IsPendingFire(FireModeNum);
        const bool bInMatchingFiringState = FiringState.IsValidIndex(FireModeNum)
            && GetCurrentState() == FiringState[FireModeNum];
        // The exact hazard this cvar exists for: flag latched + Stop arrived out-of-state.
        // Without the clear, the next UUTWeaponStateActive::BeginState auto-fire reads it.
        // Logged always (not just FireDbg) — proves the stale condition occurred, not that
        // a shot was certain — so a dogfood roll can count occurrences per match.
        if (bWasPending && !bInMatchingFiringState)
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[StalePending] %s (%s) cleared out-of-state PendingFire mode=%d state=%s pendingWpn=%d"),
                *GetName(),
                UTOwner->PlayerState ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                FireModeNum,
                (GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null")),
                (UTOwner->GetPendingWeapon() ? 1 : 0));
        }
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] ServerStopFire clear mode=%d role=%d state=%s wasPending=%d pendingWpn=%d"),
                FireModeNum, (int32)Role,
                (GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null")),
                (bWasPending ? 1 : 0),
                (UTOwner->GetPendingWeapon() ? 1 : 0));
        }
        UTOwner->SetPendingFire(FireModeNum, false);
    }

    // 3. Guard: only call EndFiringSequence if we're actually in the firing state
    // for this mode. Stock EndFiringSequence dispatches to CurrentState->EndFiringSequence(),
    // so calling it when in the wrong state would land on the wrong state object.
    if (FiringState.IsValidIndex(FireModeNum) && GetCurrentState() == FiringState[FireModeNum])
    {
        // Clean up firing effects and PendingFire immediately (not deferred).
        EndFiringSequence(FireModeNum);

        // Kill RefireCheckTimer to prevent overlap with DeferredGotoActiveState
        // (same reasoning as client-side StopFire — see comment there).
        UUTWeaponStateFiring* FiringStateObj = Cast<UUTWeaponStateFiring>(FiringState[FireModeNum]);
        if (FiringStateObj)
        {
            GetWorldTimerManager().ClearTimer(FiringStateObj->RefireCheckHandle);
        }

        // Only defer GotoActiveState — keeps weapon in FiringState during cooldown
        // so PutDown() routes to the cooldown-aware override.
        float CurrentTime = GetWorld()->GetTimeSeconds();
        float ReadyTime = 0.f;

        if (LastFireTime.IsValidIndex(FireModeNum))
        {
            ReadyTime = LastFireTime[FireModeNum] + GetRefireTime(FireModeNum);
        }

        float TimeRemaining = ReadyTime - CurrentTime;

        if (TimeRemaining > 0.01f)
        {
            FTimerDelegate Del;
            Del.BindUObject(this, &AUTWeaponFix::DeferredGotoActiveState, FireModeNum);
            GetWorldTimerManager().SetTimer(DeferredActiveStateHandle, Del, TimeRemaining, false);
        }
        else
        {
            GotoActiveState();
        }
    }

    TargetedCharacter = nullptr; // Clear Weapon's cached target
    if (UTOwner && UTOwner->Controller)
    {
        AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
        if (PC)
        {
            PC->LastShotTargetGuess = nullptr; // Clear Controller's cached target
        }
    }

}



void AUTWeaponFix::DeferredGotoActiveState(uint8 FireModeNum)
{
    // EndFiringSequence already ran in StopFire/ServerStopFireFixed — no need to call it again.
    // Only transition to ActiveState if we are actually still in a firing state.
    // If we are already unequipping or inactive, GotoActiveState would be wrong.
    UE_LOG(LogUTWeaponFix, Verbose, TEXT("[DeferredGotoActiveState] Mode %d: State=%s PendingFire[%d]=%d"),
        FireModeNum, GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"),
        FireModeNum, (UTOwner && UTOwner->IsPendingFire(FireModeNum)) ? 1 : 0);

    // CRITICAL: Clear PendingFire before GotoActiveState to prevent CheckAutoFire
    // from ghost-firing. ActiveState::BeginState calls CheckAutoFire, which sees
    // PendingFire=true and re-enters FiringState — firing a shot the player never
    // intended. This ghost fire consumes the anti-dup guard window, causing the
    // player's NEXT intentional shot to be blocked (animation plays, no projectile).
    //
    // If the player IS holding the button (tap-then-hold), StartFire already
    // scheduled a retry timer which will fire the shot ~10ms after cooldown.
    // Clearing PendingFire here only prevents the CheckAutoFire ghost path;
    // the retry timer is unaffected and handles the real shot.
    if (UTOwner)
    {
        UTOwner->SetPendingFire(FireModeNum, false);
    }

    // CRITICAL: Only transition if we're still in the firing state that SET this
    // timer. There is only ONE DeferredActiveStateHandle shared by both fire modes.
    // When alternating primary→secondary quickly, Mode 0's deferred can fire while
    // the weapon is in FiringState[1] (Mode 1), yanking it out mid-shot. This kills
    // the Mode 1 fire — the auth projectile may have spawned but the state machine
    // is corrupted, causing the server to miss subsequent fires for that mode.
    //
    // By checking FiringState[FireModeNum], stale deferreds from the OTHER mode
    // are harmlessly ignored.
    if (FiringState.IsValidIndex(FireModeNum) && GetCurrentState() == FiringState[FireModeNum])
    {
        GotoActiveState();
    }
    else
    {
        UE_LOG(LogUTWeaponFix, Verbose, TEXT("[DeferredGotoActiveState] Mode %d: STALE — weapon in %s, expected %s. Ignoring."),
            FireModeNum,
            GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"),
            FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum] ? *FiringState[FireModeNum]->GetName() : TEXT("null"));
    }
}


bool AUTWeaponFix::ServerStopFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex)
{
    return FireModeNum < GetNumFireModes() && InFireEventIndex >= 0;
}



void AUTWeaponFix::OnRep_FireModeState()
{
    // Handle fire mode state replication for non-owning clients
    for (int32 i = 0; i < FireModeActiveState.Num(); i++)
    {
        if (FireModeActiveState[i] == 0 && CurrentlyFiringMode == i)
        {
            CurrentlyFiringMode = 255;
        }
        else if (FireModeActiveState[i] == 1)
        {
            CurrentlyFiringMode = i;
        }
    }
}

void AUTWeaponFix::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AUTWeaponFix, FireModeActiveState);
}





float AUTWeaponFix::GetHitValidationPredictionTime() const
{
    if (Role != ROLE_Authority || !UTOwner || !UTOwner->PlayerState)
    {
        return 0.0f;
    }

    APlayerState* PS = Cast<APlayerState>(UTOwner->PlayerState);
    if (!PS)
    {
        return 0.0f;
    }

	float ExactPing = UTOwner->PlayerState->ExactPing;

	// 2. Subtract Fudge Factor (Epic uses 20ms)
	// This subtracts the "Processing/Jitter" time so we don't over-rewind.
	float AdjustedPing = ExactPing - FudgeFactorMs;

	// 3. Clamp (0 to Max Cap)
	float CappedPing = FMath::Clamp(AdjustedPing, 0.0f, MaxRewindMs);

	// 4. Convert to One-Way Seconds
	// (Ping / 2) / 1000  ==  Ping * 0.0005
	return CappedPing * 0.0005f;
}


void AUTWeaponFix::HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime)
{
    // Override the prediction time parameter with hit validation time
    // This ensures we use split prediction's hit validation time (120ms)
    // instead of visual time (0ms) for server-side hit validation
    float ActualPredictionTime = GetHitValidationPredictionTime();

    // Call parent with corrected prediction time
    // Epic's GetRewindLocation() will be called with this value
    // NOTE: We cannot simply call Super::HitScanTrace because it doesn't support our custom padding logic.
    // We must reimplement the trace logic here.

    // [HitAttrib] read-only per-shot attribution state; populated only when the
    // cvar is on, never feeds back into the validation result.
    const bool bHitAttrib = (Role == ROLE_Authority) && (CVarHitAttribDebug.GetValueOnGameThread() > 0);
    float AttribClaimMissBy = BIG_NUMBER;   // claimed target's primary-trace distance beyond the UNPADDED radius
    float AttribClaimPad = 0.f;             // padding the claimed target was granted in the primary loop
    bool bAttribTimeSearchHit = false;
    float AttribTimeSearchRungMs = 0.f;
    float AttribTimeSearchMissBy = 0.f;

    ECollisionChannel TraceChannel = COLLISION_TRACE_WEAPONNOCHARACTER;
    FCollisionQueryParams QueryParams(GetClass()->GetFName(), true, UTOwner);
    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

    // Perform the initial trace against world geometry
    if (TraceRadius <= 0.0f)
    {
        GetWorld()->LineTraceSingleByChannel(Hit, StartLocation, EndTrace, TraceChannel, QueryParams);
    }
    else
    {
        GetWorld()->SweepSingleByChannel(Hit, StartLocation, EndTrace, FQuat::Identity, TraceChannel, FCollisionShape::MakeSphere(TraceRadius), QueryParams);
    }

    if (!Hit.bBlockingHit)
    {
        Hit.Location = EndTrace;
    }


    // Now check against pawns
    AUTCharacter* BestTarget = NULL;
    FVector BestPoint(0.f);
    FVector BestCapsulePoint(0.f);
    float BestCollisionRadius = 0.f;

    for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
    {
        AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
        if (Target && (Target != UTOwner))
        {

            // Standard logic: Teammate checks, etc.
            if (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target))
            {
                
                float ExtraHitPadding = 0.f;

                // Only apply padding if the client explicitly claimed THIS target.
                // If client missed (ReceivedHitScanHitChar is null), this block is skipped (Padding = 0).
                if (Target == ReceivedHitScanHitChar)
                {
                    // Check velocity to decide WHICH padding to use
                    bool bIsMoving = !Target->GetVelocity().IsNearlyZero(1.0f);
					//ExtraHitPadding = bIsMoving ? HitScanPadding : HitScanPaddingStationary;
					if (bIsMoving)
					{
						float OwnerPing = (UTOwner && UTOwner->PlayerState) ? UTOwner->PlayerState->ExactPing : 0.0f;

						// TIERED PADDING SYSTEM
						// Running (940 u/s): 55 units = ~59ms jitter protection
						// Dodging (1700 u/s): 55 units = ~32ms jitter protection
                        ExtraHitPadding = 40.0f;
				
					}
					else
					{
						// Stationary targets don't need velocity compensation
						ExtraHitPadding = HitScanPaddingStationary;
					}
                }
                // find appropriate rewind position, and test against trace from StartLocation to Hit.Location
                FVector TargetLocation = ((ActualPredictionTime > 0.f) && (Role == ROLE_Authority)) ? Target->GetRewindLocation(ActualPredictionTime) : Target->GetActorLocation();
                if (Role == ROLE_Authority && ActualPredictionTime > 0.f)
                {
                    float RTTms = UTOwner && UTOwner->PlayerState ? Cast<APlayerState>(UTOwner->PlayerState)->ExactPing : 0.f;
                    float RewindDistance = (Target->GetActorLocation() - TargetLocation).Size();

      
                }
                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                if (Target->UTCharacterMovement && Target->UTCharacterMovement->bIsFloorSliding)
                {
                    TargetLocation.Z = TargetLocation.Z - CollisionHeight + Target->SlideTargetHeight;
                    CollisionHeight = Target->SlideTargetHeight;
                }
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();

                bool bCheckOutsideHit = false;
                bool bHitTarget = false;
                FVector ClosestPoint(0.f);
                FVector ClosestCapsulePoint = TargetLocation;
                if (CollisionRadius >= CollisionHeight)
                {
                    ClosestPoint = FMath::ClosestPointOnSegment(TargetLocation, StartLocation, Hit.Location);
                    bHitTarget = ((ClosestPoint - TargetLocation).SizeSquared() < FMath::Square(CollisionHeight + TraceRadius + ExtraHitPadding));
                    if (!bHitTarget && (ExtraHitPadding > 0.f))
                    {
                        bCheckOutsideHit = true;
                    }
                }
                else
                {
                    FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
                    FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
                    bHitTarget = ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CollisionRadius + TraceRadius + ExtraHitPadding));
                }

                // [HitAttrib] how far outside the bare rewound capsule the claimed
                // target sat: <=0 means the unpadded rewind trace alone hits it,
                // >0 means only the claim padding (if any) can rescue it.
                if (bHitAttrib && Target == ReceivedHitScanHitChar)
                {
                    const bool bSphereBranch = (CollisionRadius >= CollisionHeight);
                    const float AttribDist = bSphereBranch
                        ? FVector::Dist(ClosestPoint, TargetLocation)
                        : FVector::Dist(ClosestPoint, ClosestCapsulePoint);
                    const float AttribUnpaddedRadius = (bSphereBranch ? CollisionHeight : CollisionRadius) + TraceRadius;
                    AttribClaimMissBy = AttribDist - AttribUnpaddedRadius;
                    AttribClaimPad = ExtraHitPadding;
                }

                // If we hit, update best target
                if (bHitTarget && (!BestTarget || ((ClosestPoint - StartLocation).SizeSquared() < (BestPoint - StartLocation).SizeSquared())))
                {
                    BestTarget = Target;
                    BestPoint = ClosestPoint;
                    BestCapsulePoint = ClosestCapsulePoint;
                    BestCollisionRadius = CollisionRadius;
                    // Cache the total padded radius for ServerShield hitplot normalization
                    LastHitscanPaddedRadius = CollisionRadius + TraceRadius + ExtraHitPadding;
                }
            }
        }
        // --- FIX END ---
    }

	// ============================================================
	// NEWNET-STYLE BIDIRECTIONAL TIME SEARCH
	// If client claimed a hit but we didn't find it, search through time
	// ============================================================
	// Mirror the main-loop team guard (~line 1896): never run the time-search for a CLIENT-NAMED teammate when
	// teammates don't block hitscan. ReceivedHitScanHitChar is fully client-controlled, so without this a client
	// could name a teammate to force a near-graze body hit (FF-gated at damage, but it shouldn't be considered).
	if (Role == ROLE_Authority &&
		ReceivedHitScanHitChar != nullptr &&
		BestTarget != ReceivedHitScanHitChar &&
		(bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, ReceivedHitScanHitChar)))
	{
		AUTCharacter* ClaimedTarget = ReceivedHitScanHitChar;

		float CapRadius = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
		float CapHeight = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

		const float SearchStep = 0.015f;      // 15ms steps
		const float MaxSearchOffset = GetHitscanTimeSearchWindow(); // ±45ms max search (tries ±15, ±30, ±45 on fixed 15ms rungs; ±60 is the next rung but trades attacker recovery for "shot through my dodge" defender complaints — primary rewind still does the heavy lifting)
		float SearchOffset = SearchStep;

		while (FMath::Abs(SearchOffset) <= MaxSearchOffset)
		{
			float AltRewindTime = ActualPredictionTime + SearchOffset;

			// Sanity bounds
			if (AltRewindTime > 0.0f && AltRewindTime < 0.25f)
			{
				FVector AltTargetLoc = ClaimedTarget->GetRewindLocation(AltRewindTime);

				// Handle floor sliding at alternate time
				float AltCapHeight = CapHeight;
				if (ClaimedTarget->UTCharacterMovement && ClaimedTarget->UTCharacterMovement->bIsFloorSliding)
				{
					AltTargetLoc.Z = AltTargetLoc.Z - CapHeight + ClaimedTarget->SlideTargetHeight;
					AltCapHeight = ClaimedTarget->SlideTargetHeight;
				}

				// Capsule-to-line distance check
				FVector ClosestPoint, ClosestCapsulePoint;

				if (CapRadius >= AltCapHeight)
				{
					ClosestPoint = FMath::ClosestPointOnSegment(AltTargetLoc, StartLocation, Hit.Location);
					ClosestCapsulePoint = AltTargetLoc;
				}
				else
				{
					FVector CapsuleSegment = FVector(0.f, 0.f, AltCapHeight - CapRadius);
					FMath::SegmentDistToSegmentSafe(
						StartLocation, Hit.Location,
						AltTargetLoc - CapsuleSegment, AltTargetLoc + CapsuleSegment,
						ClosestPoint, ClosestCapsulePoint);
				}

				// Generous padding for fallback search
				float SearchPadding = 45.0f;
				float CombinedRadius = CapRadius + TraceRadius + SearchPadding;

				if ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CombinedRadius))
				{
					// Found the hit at alternate time
					BestTarget = ClaimedTarget;
					BestPoint = ClosestPoint;
					BestCapsulePoint = ClosestCapsulePoint;
					BestCollisionRadius = CapRadius;

					if (bHitAttrib)
					{
						bAttribTimeSearchHit = true;
						AttribTimeSearchRungMs = SearchOffset * 1000.f;
						// vs the UNPADDED radius at the accepted rung: <=0 is a genuine
						// capsule hit at the alternate time, >0 rode the 45uu search pad.
						AttribTimeSearchMissBy =
							FVector::Dist(ClosestPoint, ClosestCapsulePoint) - (CapRadius + TraceRadius);
					}

					UE_LOG(LogUTWeaponFix, Verbose,
						TEXT("TimeSearch: Found claimed hit at offset %.1fms (base %.1fms)"),
						SearchOffset * 1000.f, ActualPredictionTime * 1000.f);
					break;
				}
			}

			// Oscillate: +15ms, -15ms, +30ms, -30ms, +45ms, -45ms
			if (SearchOffset > 0.f)
				SearchOffset = -SearchOffset;
			else
				SearchOffset = -SearchOffset + SearchStep;
		}
	}


    // ---- UNCLAIMED-HIT RENDER CHECK (see cvar block at top of file) ----
    // Runs when a pawn hit is about to be granted with NO claim. Computes
    // whether the shot also crosses the target's render-time capsule; enforce
    // mode (gate 1, default) demotes failing hits to the world impact, shadow
    // mode (gate 0) only records the verdict for the [HitAttrib] line.
    bool bRenderChkApplicable = false;
    bool bRenderChkPass = true;
    bool bRenderChkDemoted = false;
    float RenderChkMissBy = 0.f;
    AUTCharacter* RenderChkDemotedTarget = nullptr;
    const int32 UnclaimedRenderGate = CVarUnclaimedRenderGate.GetValueOnGameThread();
    if (BestTarget != nullptr && ReceivedHitScanHitChar == nullptr &&
        (bHitAttrib || UnclaimedRenderGate > 0) &&
        Role == ROLE_Authority && GetNetMode() != NM_Standalone &&
        UTOwner != nullptr && UTOwner->PlayerState != nullptr)
    {
        // Same applicability envelope as the claim generator in FireShot: the
        // shooter is a remote human and this mode COULD have claimed. Bots and
        // spread weapons never claim, so their unclaimed hits are legitimate.
        const AUTPlayerController* ShooterPC = Cast<AUTPlayerController>(UTOwner->Controller);
        const bool bClaimCapableMode =
            bTrackHitScanReplication &&
            InstantHitInfo.IsValidIndex(CurrentFireMode) &&
            InstantHitInfo[CurrentFireMode].DamageType != nullptr &&
            InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.0f;
        if (ShooterPC != nullptr && !ShooterPC->IsLocalController() && bClaimCapableMode)
        {
            bRenderChkApplicable = true;

            const float RenderMs = UTOwner->PlayerState->ExactPing * 0.5f +
                FMath::Max(0.f, CVarHitAttribRenderExtraMs.GetValueOnGameThread());
            const float RenderT = FMath::Clamp(RenderMs * 0.001f, 0.f, 0.25f);
            FVector RenderLoc = BestTarget->GetRewindLocation(RenderT);

            // Mirror the primary loop's capsule math exactly, at render time.
            float RenderColHeight = BestTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            if (BestTarget->UTCharacterMovement && BestTarget->UTCharacterMovement->bIsFloorSliding)
            {
                RenderLoc.Z = RenderLoc.Z - RenderColHeight + BestTarget->SlideTargetHeight;
                RenderColHeight = BestTarget->SlideTargetHeight;
            }
            const float RenderColRadius = BestTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();

            FVector RenderClosest(0.f);
            FVector RenderCapsulePoint = RenderLoc;
            float RenderEffRadius;
            if (RenderColRadius >= RenderColHeight)
            {
                RenderClosest = FMath::ClosestPointOnSegment(RenderLoc, StartLocation, Hit.Location);
                RenderEffRadius = RenderColHeight;
            }
            else
            {
                const FVector RenderSeg(0.f, 0.f, RenderColHeight - RenderColRadius);
                FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location,
                    RenderLoc - RenderSeg, RenderLoc + RenderSeg, RenderClosest, RenderCapsulePoint);
                RenderEffRadius = RenderColRadius;
            }
            RenderChkMissBy = FVector::Dist(RenderClosest, RenderCapsulePoint) - (RenderEffRadius + TraceRadius);
            bRenderChkPass = RenderChkMissBy <= FMath::Max(0.f, CVarUnclaimedRenderSlack.GetValueOnGameThread());

            if (!bRenderChkPass && UnclaimedRenderGate > 0)
            {
                // Demote to the world impact computed above; beams and impact
                // effects still terminate correctly (same shape as the reverted
                // 2026-07-18 gate's rejection, but render-reconstructed instead
                // of claim-presence-based, and ping-independent).
                UE_LOG(LogUTWeaponFix, Log,
                    TEXT("[HitAttrib] UnclaimedRenderGate DEMOTED %s: missed render-time capsule by %.1fuu (ping %.0f, renderMs %.1f)"),
                    *BestTarget->GetName(), RenderChkMissBy, UTOwner->PlayerState->ExactPing, RenderMs);
                RenderChkDemotedTarget = BestTarget;
                bRenderChkDemoted = true;
                BestTarget = nullptr;
                BestPoint = FVector::ZeroVector;
                BestCapsulePoint = FVector::ZeroVector;
                BestCollisionRadius = 0.f;
                LastHitscanPaddedRadius = 0.f;
            }
        }
    }

    if (BestTarget)
    {
        // we found a player to hit, so update hit result
        // first find proper hit location on surface of capsule
        float ClosestDistSq = (BestPoint - BestCapsulePoint).SizeSquared();
        float BackDist = FMath::Sqrt(FMath::Max(0.f, BestCollisionRadius * BestCollisionRadius - ClosestDistSq));

        Hit.Location = BestPoint + BackDist * (StartLocation - EndTrace).GetSafeNormal();
        Hit.Normal = (Hit.Location - BestCapsulePoint).GetSafeNormal();
        Hit.ImpactNormal = Hit.Normal;
        Hit.Actor = BestTarget;
        Hit.bBlockingHit = true;
        Hit.Component = BestTarget->GetCapsuleComponent();
        Hit.ImpactPoint = BestPoint;
        Hit.Time = (BestPoint - StartLocation).Size() / (EndTrace - StartLocation).Size();
    }

    // [HitAttrib] one line per validated hitscan: who fired at what ping, what
    // was claimed, which acceptance route decided the shot, and how far the
    // validation-time rewound capsule leads a render-latency-estimate rewind
    // (leadUU > 0 = validation capsule AHEAD of the estimated rendered image
    // along the target's velocity — the "gifted shot" direction).
    if (bHitAttrib)
    {
        auto AttribName = [](const AUTCharacter* C) -> FString
        {
            if (C == nullptr)
            {
                return FString(TEXT("none"));
            }
            return C->PlayerState ? C->PlayerState->PlayerName : C->GetName();
        };

        const float ShooterPing = (UTOwner && UTOwner->PlayerState) ? UTOwner->PlayerState->ExactPing : 0.f;
        const FString ClaimStr = (ReceivedHitScanHitChar == nullptr)
            ? TEXT("none")
            : (!ReceivedHeadOffset.IsZero() ? TEXT("head") : TEXT("body"));

        const TCHAR* Route = TEXT("miss");
        if (BestTarget != nullptr)
        {
            if (bAttribTimeSearchHit)
            {
                Route = TEXT("timesearch-rescue");
            }
            else if (ReceivedHitScanHitChar != nullptr && BestTarget == ReceivedHitScanHitChar)
            {
                Route = (AttribClaimMissBy <= 0.f) ? TEXT("primary-rewind") : TEXT("padding-rescue");
            }
            else if (ReceivedHitScanHitChar != nullptr)
            {
                Route = TEXT("primary-rewind-otherthanclaim");
            }
            else
            {
                Route = TEXT("primary-rewind-unclaimed");
            }
        }

        AUTCharacter* AttribTarget = BestTarget ? BestTarget
            : (RenderChkDemotedTarget ? RenderChkDemotedTarget : ReceivedHitScanHitChar);
        const float RenderEstMs = ShooterPing * 0.5f + FMath::Max(0.f, CVarHitAttribRenderExtraMs.GetValueOnGameThread());
        FString LeadStr(TEXT("na"));
        FString DeltaMagStr(TEXT("na"));
        if (AttribTarget != nullptr)
        {
            const float RenderEstTime = FMath::Clamp(RenderEstMs * 0.001f, 0.f, 0.25f);
            const FVector ValPos = (ActualPredictionTime > 0.f)
                ? AttribTarget->GetRewindLocation(ActualPredictionTime) : AttribTarget->GetActorLocation();
            const FVector RenPos = (RenderEstTime > 0.f)
                ? AttribTarget->GetRewindLocation(RenderEstTime) : AttribTarget->GetActorLocation();
            const FVector ValMinusRender = ValPos - RenPos;
            LeadStr = FString::Printf(TEXT("%.1f"),
                FVector::DotProduct(ValMinusRender, AttribTarget->GetVelocity().GetSafeNormal()));
            DeltaMagStr = FString::Printf(TEXT("%.1f"), ValMinusRender.Size());
        }

        const FString ClaimMissStr = (AttribClaimMissBy < BIG_NUMBER)
            ? FString::Printf(TEXT("%.1f"), AttribClaimMissBy) : TEXT("na");
        const FString TsRungStr = bAttribTimeSearchHit
            ? FString::Printf(TEXT("%.0f"), AttribTimeSearchRungMs) : TEXT("na");
        const FString TsMissStr = bAttribTimeSearchHit
            ? FString::Printf(TEXT("%.1f"), AttribTimeSearchMissBy) : TEXT("na");
        const FString RenderChkStr = !bRenderChkApplicable ? TEXT("na")
            : (bRenderChkPass ? TEXT("pass") : (bRenderChkDemoted ? TEXT("fail-demoted") : TEXT("fail")));
        const FString RenderChkMissStr = bRenderChkApplicable
            ? FString::Printf(TEXT("%.1f"), RenderChkMissBy) : TEXT("na");

        UE_LOG(LogUTWeaponFix, Log,
            TEXT("[HitAttrib] shooter=%s ping=%.0f wep=%s mode=%d rewindMs=%.1f claim=%s target=%s speed=%.0f route=%s claimMissBy=%s pad=%.0f tsRungMs=%s tsMissBy=%s renderEstMs=%.1f leadUU=%s dMag=%s renderChk=%s renderChkMissBy=%s"),
            *AttribName(UTOwner), ShooterPing, *GetClass()->GetName(), CurrentFireMode,
            ActualPredictionTime * 1000.f, *ClaimStr, *AttribName(AttribTarget),
            AttribTarget ? AttribTarget->GetVelocity().Size() : 0.f,
            Route, *ClaimMissStr, AttribClaimPad, *TsRungStr, *TsMissStr,
            RenderEstMs, *LeadStr, *DeltaMagStr, *RenderChkStr, *RenderChkMissStr);
    }

    if (Role == ROLE_Authority)
    {
        OnServerHitScanResult(Hit, ActualPredictionTime);
    }
}


void AUTWeaponFix::OnServerHitScanResult(const FHitResult& Hit, float PredictionTime)
{
    // Default: do nothing. Custom weapons (Shock/Sniper) override this.
}


FRotator AUTWeaponFix::GetAdjustedAim_Implementation(FVector StartFireLoc)
{
    // Server: only honor cache during the actual transactional fire RPC. The
    // cache is set in ServerStartFireFixed_Implementation and lives only for
    // that call. Outside that scope the cache holds stale values from prior
    // shots (server has no per-shot clear), so reading it without the flag
    // gate causes hits to land at where the player aimed many shots ago.
    FRotator BaseAim;

    if (Role == ROLE_Authority && bIsTransactionalFire && !CachedTransactionalRotation.IsZero())
    {
        BaseAim = CachedTransactionalRotation;
    }
    else
    {
        BaseAim = GetBaseFireRotation();
    }

    // CRITICAL: We do NOT call GuessPlayerTarget().
    // The base implementation calls GuessPlayerTarget(), which traces 
    // and updates 'LastShotTargetGuess', causing the magnetism loop.
    // By skipping it, we ensure the weapon fires exactly where the crosshair is.

    // 2. Apply Spread (If applicable)
    // We must re-implement the spread logic since we aren't calling Super.
    if (Spread.IsValidIndex(CurrentFireMode) && Spread[CurrentFireMode] > 0.0f)
    {
        FRotationMatrix Mat(BaseAim);
        FVector X, Y, Z;
        Mat.GetScaledAxes(X, Y, Z);

        // Deterministic spread syncing
        NetSynchRandomSeed();

        float RandY = 0.5f * (FMath::FRand() + FMath::FRand() - 1.f);
        float RandZ = FMath::Sqrt(0.25f - FMath::Square(RandY)) * (FMath::FRand() + FMath::FRand() - 1.f);

        return (X + RandY * Spread[CurrentFireMode] * Y + FMath::Clamp(RandZ * VerticalSpreadScaling, -1.f * MaxVerticalSpread, MaxVerticalSpread) * Spread[CurrentFireMode] * Z).Rotation();
    }

    // 3. Return Raw Aim
    return BaseAim;
}



FRotator AUTWeaponFix::GetBaseFireRotation()
{
    // Server: only honor cache during the actual transactional fire RPC.
    // The cache lives between ServerStartFireFixed calls and isn't cleared
    // per-shot, so an ungated read returns stale rotation from prior shots
    // (visible as damage landing at old aim points after weapon swap or
    // RefireCheckTimer-driven held fire).
    //
    // Client: cache is set right before Super::FireShot and cleared right
    // after, so non-zero already means "this one fake spawn we're in."
    if ((Role == ROLE_Authority && bIsTransactionalFire && !CachedTransactionalRotation.IsZero()) ||
        (Role < ROLE_Authority && !CachedTransactionalRotation.IsZero()))
    {
        return CachedTransactionalRotation;
    }

    return Super::GetBaseFireRotation();
}



FVector AUTWeaponFix::GetFireStartLoc(uint8 FireMode)
{
    // 1. Get the standard start location (Muzzle offset, etc applied to CURRENT Actor Location)
    FVector StartLoc = Super::GetFireStartLoc(FireMode);

    // 2. PARALLAX FIX (PROJECTILES ONLY)
        // We check if a Projectile Class is assigned to this mode. 
        // If ProjClass is NULL, it's likely a Hitscan mode (Sniper, Shock Beam), so we skip.
    bool bIsProjectile = (ProjClass.IsValidIndex(FireMode) && ProjClass[FireMode] != nullptr);

    // Gated on bIsTransactionalFire — same reason as GetAdjustedAim: the cache
    // isn't cleared per-shot on the server, so an ungated read applies
    // parallax shift using stale data from the wrong shot.
    if (bIsProjectile && Role == ROLE_Authority && bIsTransactionalFire &&
        !CachedTransactionalRotation.IsZero() && UTOwner)
    {
        float PredictionTime = GetHitValidationPredictionTime();

        // Rewind the shooter to where they were when they clicked
        FVector RewoundShooterLoc = UTOwner->GetRewindLocation(PredictionTime);

        // Calculate the shift (Parallax Error)
        FVector MovementDelta = (RewoundShooterLoc - UTOwner->GetActorLocation());

        // Shift the muzzle origin back to that spot
        StartLoc += MovementDelta;
    }
    return StartLoc;
}


void AUTWeaponFix::SpawnDelayedFakeProjectile()
{
	// Updated variable name
	if (NetcodeDelayedProjectile.ProjectileClass != nullptr)
	{
		SpawnNetPredictedProjectile(NetcodeDelayedProjectile.ProjectileClass, NetcodeDelayedProjectile.SpawnLocation, NetcodeDelayedProjectile.SpawnRotation);
	}
}


AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectile(
	TSubclassOf<AUTProjectile> ProjectileClass,
	FVector SpawnLocation,
	FRotator SpawnRotation)
{
	// Pitch clamp for shells/rockets firing straight down
	FRotator AdjustedRot = SpawnRotation;
	AdjustedRot.Normalize();
    bool bIsShockCore = ProjectileClass &&
        ProjectileClass->IsChildOf(AUTPlusProj_ShockBall::StaticClass());
    bool bIsFlakShell = ProjectileClass &&
        (ProjectileClass->GetName().Contains(TEXT("FlakShell")) ||
            ProjectileClass->GetName().Contains(TEXT("Shell")));
    // Anti-duplicate guards: each type has its own timestamp so fast weapon-switching
    // cannot block a legitimate first-fire on the other weapon type.
    if (bIsShockCore)
    {
        float CurrentTime = GetWorld()->GetTimeSeconds();
        float TimeSinceLast = CurrentTime - LastShockCoreSpawnTime;
        if (TimeSinceLast < 0.2f)
        {
            if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("ShockCore anti-dup guard BLOCKED spawn. TimeSinceLast=%.4f Role=%d"), TimeSinceLast, (int32)Role);
            return nullptr;
        }
        LastShockCoreSpawnTime = CurrentTime;
    }
    else if (bIsFlakShell)
    {
        float CurrentTime = GetWorld()->GetTimeSeconds();
        float TimeSinceLast = CurrentTime - LastFlakShellSpawnTime;
        if (TimeSinceLast < 0.2f)
        {
            if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("FlakShell anti-dup guard BLOCKED spawn. TimeSinceLast=%.4f Role=%d"), TimeSinceLast, (int32)Role);
            return nullptr;
        }
        LastFlakShellSpawnTime = CurrentTime;
    }
	bool bIsShellOrRocket = ProjectileClass &&
		(ProjectileClass->GetName().Contains(TEXT("Shell")) ||
			ProjectileClass->GetName().Contains(TEXT("Rocket")));
	if (bIsShellOrRocket && AdjustedRot.Pitch < -83.5f)
	{
		SpawnRotation.Pitch = -85.0f;
	}

	AUTPlayerController* OwningPlayer = UTOwner ? Cast<AUTPlayerController>(UTOwner->GetController()) : nullptr;
	// --- ADDED: Needed for Team Checks during Tunnel Sweep ---
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

	// ----------------------------------------
	// 1) Get Current Ping
	// ----------------------------------------
	float CurrentPing = 0.0f;
	if (UTOwner && UTOwner->PlayerState)
	{
		CurrentPing = UTOwner->PlayerState->ExactPing;
	}

	// ----------------------------------------
	// 2) Compute CatchupTickDelta (Half RTT)
	// ----------------------------------------
	float CatchupTickDelta = 0.0f;

	if (CurrentPing >= 20.0f)
	{
		float AdjustedPing = CurrentPing; // -FudgeFactorMs;
		float CappedPing = FMath::Clamp(AdjustedPing, 0.0f, ProjectilePredictionCapMs);
		CatchupTickDelta = CappedPing * 0.0005f;  // Half RTT in seconds
	}

	// ----------------------------------------
	// 3) Client: Check if we should delay spawn for extreme ping
	// ----------------------------------------
	if ((Role != ROLE_Authority) && OwningPlayer)
	{
		float ExcessPing = CurrentPing - FudgeFactorMs - ProjectilePredictionCapMs;

		if (ExcessPing > 10.0f)  // More than 10ms over cap
		{
			float SleepTime = ExcessPing * 0.001f;

			if (!GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle))
			{
				NetcodeDelayedProjectile.ProjectileClass = ProjectileClass;
				NetcodeDelayedProjectile.SpawnLocation = SpawnLocation;
				NetcodeDelayedProjectile.SpawnRotation = SpawnRotation;

				GetWorldTimerManager().SetTimer(
					SpawnDelayedFakeProjHandle,
					this,
					&AUTWeaponFix::SpawnDelayedFakeProjectile,
					SleepTime,
					false);
			}
			return nullptr;
		}
	}

	// ----------------------------------------
	// 4) Spawn the projectile
	// ----------------------------------------
    /*
	FActorSpawnParameters Params;
	Params.Instigator = UTOwner;
	Params.Owner = UTOwner;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.bDeferConstruction = true;

	AUTProjectile* NewProjectile = GetWorld()->SpawnActor<AUTProjectile>(
		ProjectileClass,
		SpawnLocation,
		SpawnRotation,
		Params);
    */

    // ----------------------------------------
    // 4) Spawn the projectile (standard SpawnActor)
    // ----------------------------------------
    // We use regular SpawnActor instead of SpawnActorDeferred because:
    // - SpawnActorDeferred separates construction from BeginPlay, which causes
    //   ProjectileMovement to initialize velocity from the instigator's stale
    //   rotation instead of the spawn transform. This made shock cores and flak
    //   balls fly in the wrong direction on the server at high FPS.
    // - Regular SpawnActor lets BeginPlay run immediately with the correct
    //   transform, so velocity initializes correctly from the start.
    // - Tick intervals are set AFTER spawn — they take effect on the next frame.
    //   One tick at the default interval is acceptable.
    FActorSpawnParameters Params;
    Params.Instigator = UTOwner;
    Params.Owner = UTOwner;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AUTProjectile* NewProjectile = GetWorld()->SpawnActor<AUTProjectile>(
        ProjectileClass,
        SpawnLocation,
        SpawnRotation,
        Params);

	if (!NewProjectile)
	{
		return nullptr;
	}

    // ----------------------------------------
    // 4b) High-FPS stability (Fixed Tick Rate)
    // ----------------------------------------
    // Set after spawn — takes effect starting next frame.
    if (NewProjectile->ProjectileMovement)
    {
        if (Role == ROLE_Authority)
        {
            const float ServerRate = 1.f / 240.f;
            NewProjectile->PrimaryActorTick.TickInterval = ServerRate;
            NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = ServerRate;
        }
        else if (GetNetMode() != NM_DedicatedServer)
        {
            const int32 ClientHz = GetClampedProjectileHz();
            const float ClientInterval = 1.f / static_cast<float>(ClientHz);
            NewProjectile->PrimaryActorTick.TickInterval = ClientInterval;
            NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = ClientInterval;
        }
    }

    // Safety belt: enforce velocity to match SpawnRotation.
    // With regular SpawnActor this should already be correct (BeginPlay initializes
    // from the spawn transform), but we enforce it to be absolutely sure.
    if (NewProjectile->ProjectileMovement && NewProjectile->ProjectileMovement->InitialSpeed > 0.f)
    {
        FVector CorrectVelocity = SpawnRotation.Vector() * NewProjectile->ProjectileMovement->InitialSpeed;
        // Preserve TossZ applied during BeginPlay (e.g., FlakShell TossZ=430 for its arc).
        CorrectVelocity.Z += NewProjectile->TossZ;
        NewProjectile->ProjectileMovement->Velocity = CorrectVelocity;
        NewProjectile->ProjectileMovement->UpdateComponentVelocity();

        // Re-cache drift correction direction so it matches enforced velocity.
        // BeginPlay cached OriginalFireDirection before this enforcement ran;
        // if they differ (rotation quantization), the drift correction in Tick()
        // would snap the ball back to the stale direction, causing a check-mark.
        AUTPlusProj_ShockBall* ShockBall = Cast<AUTPlusProj_ShockBall>(NewProjectile);
        if (ShockBall)
        {
            ShockBall->SetOriginalFireDirection(NewProjectile->ProjectileMovement->Velocity.GetSafeNormal());
        }
    }

	// ----------------------------------------
	// 5) Visual offsets (weapon hand)
	// ----------------------------------------
	if (NewProjectile->OffsetVisualComponent)
	{
		switch (GetWeaponHand())
		{
		case EWeaponHand::HAND_Center:
			NewProjectile->InitialVisualOffset = NewProjectile->InitialVisualOffset + LowMeshOffset;
			NewProjectile->OffsetVisualComponent->RelativeLocation = NewProjectile->InitialVisualOffset;
			break;
		case EWeaponHand::HAND_Hidden:
			NewProjectile->InitialVisualOffset = NewProjectile->InitialVisualOffset + VeryLowMeshOffset;
			NewProjectile->OffsetVisualComponent->RelativeLocation = NewProjectile->InitialVisualOffset;
			break;
		default:
			break;
		}
	}

	if (UTOwner)
	{
		UTOwner->LastFiredProjectile = NewProjectile;
		NewProjectile->ShooterLocation = UTOwner->GetActorLocation();
		NewProjectile->ShooterRotation = UTOwner->GetActorRotation();
	}

	// ----------------------------------------
	// 6) SERVER: Fast-forward authoritative projectile
	// ----------------------------------------
	if (Role == ROLE_Authority)
	{
		NewProjectile->HitsStatsName = HitsStatsName;

		// Track server projectile for rewind validation (if enabled).
		// Only claim-capable projectiles are tracked; tracking e.g. flak shards
		// (9/shot) would FIFO-evict the shell/rocket from the 10-entry list before
		// its claim RPC arrives. The minigun stinger shard IS claim-capable
		// (AUTPlusProj_StingerShard) and is included: alt-fire is ~0.45s cadence
		// with a 5s lifespan, so up to ~11 could co-exist and clip the 10-cap, but
		// shards air-explode on approach and rarely live out their lifespan, so
		// steady-state concurrency stays well under the cap. If eviction shows up
		// in ProjRewind logs for the shard, raise the per-weapon cap below.
		const bool bTrackForRewind = bEnableProjectileRewind && NewProjectile &&
			(NewProjectile->IsA(AUTPlusProj_Rocket::StaticClass())
			 || NewProjectile->IsA(AUTPlusProj_FlakShell::StaticClass())
			 || NewProjectile->IsA(AUTPlusProj_StingerShard::StaticClass()));
		if (bTrackForRewind)
		{
			ActiveServerProjectiles.Add(FActiveServerProjectile(NewProjectile, CurrentFireMode));

			// Cleanup stale entries
			for (int32 i = ActiveServerProjectiles.Num() - 1; i >= 0; i--)
			{
				if (!ActiveServerProjectiles[i].Projectile.IsValid())
				{
					ActiveServerProjectiles.RemoveAt(i);
				}
			}
			while (ActiveServerProjectiles.Num() > 10)
			{
				ActiveServerProjectiles.RemoveAt(0);
			}
		}

		// GUARD RAIL: Minimum Threshold (prevents 0-ping PIE physics bugs)
		const float MinCatchupThreshold = 0.005f;

		if ((CatchupTickDelta > MinCatchupThreshold) && NewProjectile->ProjectileMovement)
		{
			// =========================================================================
			// LAG COMPENSATION: REWIND CHECK
			// 
			// Because clients don't predict enemy positions (GetClientVisualPredictionTime = 0),
			// targets on the client's screen are behind their actual server position.
			// 
			// This check rewinds enemies to where they were when the client fired,
			// then tests if the projectile path would have hit them. This provides
			// lag compensation for both:
			// - Fast projectiles that could tunnel through targets
			// - Projectiles aimed at where the enemy appeared on screen
			// =========================================================================

			FVector CatchupStart = SpawnLocation;
			// Use SpawnRotation directly — velocity was already enforced after FinishSpawning,
			// but deriving from SpawnRotation is authoritative and avoids any edge cases.
			FVector CatchupVelocity = SpawnRotation.Vector() * NewProjectile->ProjectileMovement->InitialSpeed;
			FVector CatchupEnd = CatchupStart + (CatchupVelocity * CatchupTickDelta);

			// Get projectile's effective hit detection radius
			// Priority: CollisionComp > PawnOverlapSphere > fallback
			float ProjHitRadius = 0.f;
			if (NewProjectile->CollisionComp)
			{
				ProjHitRadius = NewProjectile->CollisionComp->GetScaledSphereRadius();
			}
			// Flak shards have CollisionComp = 0 but use PawnOverlapSphere (36 units) for hit detection
			if (ProjHitRadius <= 0.f && NewProjectile->PawnOverlapSphere)
			{
				ProjHitRadius = NewProjectile->PawnOverlapSphere->GetScaledSphereRadius();
			}
			// Final fallback for projectiles with neither
			if (ProjHitRadius <= 0.f)
			{
				ProjHitRadius = 10.f;
			}

			// Optimize Search Area — expanded to 350 to cover dodge-speed targets
			// (1700 u/s * 60ms = 102u, plus capsule radius)
			FVector MinVec = CatchupStart.ComponentMin(CatchupEnd);
			FVector MaxVec = CatchupStart.ComponentMax(CatchupEnd);
			FBox PathBounds(MinVec, MaxVec);
			PathBounds = PathBounds.ExpandBy(350.0f);

			// CatchupTickDelta is already capped by ProjectilePredictionCapMs (120ms -> 60ms half-RTT).
			// No additional cap needed.
			float RewindTime = CatchupTickDelta;
			bool bHitRegistered = false;

			// =========================================================================
			// MULTI-TIME-SAMPLE REWIND CHECK
			//
			// Same pattern as hitscan (HitScanTrace lines 1378-1454): if the primary
			// rewind time misses, try nearby timestamps. This handles clock drift and
			// SavedPosition gaps without expanding hitboxes (no ghost hits).
			// =========================================================================
			static const float RewindOffsets[] = { 0.0f, 0.015f, -0.015f, 0.030f, -0.030f };
			static const int32 NumRewindSamples = ARRAY_COUNT(RewindOffsets);

			for (int32 SampleIdx = 0; SampleIdx < NumRewindSamples && !bHitRegistered; ++SampleIdx)
			{
				float SampleRewindTime = FMath::Max(0.0f, RewindTime + RewindOffsets[SampleIdx]);
				// Skip duplicate zero-offset samples
				if (SampleIdx > 0 && SampleRewindTime <= 0.0f) continue;

				for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
				{
					AUTCharacter* Target = Cast<AUTCharacter>(*It);

					if (Target && Target != UTOwner && !Target->IsDead() &&
						PathBounds.IsInside(Target->GetActorLocation()))
					{
						// Skip teammates
						if (GS && GS->OnSameTeam(UTOwner, Target)) continue;

						// 1. REWIND: Where was the target when the client fired?
						FVector RewoundLoc = Target->GetRewindLocation(SampleRewindTime);

						// 2. GEOMETRY: Construct Rewound Capsule centerline
						float CapRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
						float CapHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

						FVector CapsuleTop = RewoundLoc + FVector(0, 0, CapHeight - CapRadius);
						FVector CapsuleBot = RewoundLoc - FVector(0, 0, CapHeight - CapRadius);

						// 3. MATH: Find closest points between projectile path and capsule centerline
						FVector PointOnPath, PointOnCapsule;
						FMath::SegmentDistToSegmentSafe(
							CatchupStart, CatchupEnd,
							CapsuleBot, CapsuleTop,
							PointOnPath, PointOnCapsule
						);

						float DistSqr = FVector::DistSquared(PointOnPath, PointOnCapsule);

						// 4. COLLISION CHECK: exact capsule dimensions (no expansion)
						float CombinedRadius = CapRadius + ProjHitRadius;

						if (DistSqr < (CombinedRadius * CombinedRadius))
						{
							FVector DirToPath = (PointOnPath - PointOnCapsule).GetSafeNormal();
							FVector HitLocation = PointOnCapsule + (DirToPath * CapRadius);
							FVector HitNormal = (CatchupStart - CatchupEnd).GetSafeNormal();

							// ProcessHit handles all projectile types correctly:
							// direct damage (flak: FUTPointDamageEvent) and
							// splash damage (rockets: FUTRadialDamageEvent)
							NewProjectile->ProcessHit(Target, Target->GetCapsuleComponent(), HitLocation, HitNormal);

							bHitRegistered = true;
							break;
						}
					}
				}
			}

			// Only fast-forward if we didn't hit a rewound target
			if (!bHitRegistered)
			{
				const float ScaledDelta = CatchupTickDelta * NewProjectile->CustomTimeDilation;

				// FIX: Only tick ProjectileMovement, NOT TickActor.
				// TickActor ticks all components (including ProjectileMovement), so calling
				// both TickActor + TickComponent moves the projectile at 2x speed, causing
				// tunneling and overshooting. ProjectileMovement::TickComponent handles
				// substeps internally via MaxSimulationTimeStep.
				NewProjectile->ProjectileMovement->MaxSimulationTimeStep = 1.f / 240.f;
				NewProjectile->ProjectileMovement->TickComponent(ScaledDelta, LEVELTICK_All, nullptr);
				NewProjectile->SetForwardTicked(true);

				// =========================================================================
				// POST-FAST-FORWARD OVERLAP CHECK
				//
				// Catches mid-range hits where the spawn-time rewind check missed because
				// the projectile hadn't reached the target yet. Now that the projectile has
				// been fast-forwarded, check if its new position overlaps a rewound capsule.
				// Uses exact capsule dimensions (no expansion) — just checks at a new point.
				// =========================================================================
				FVector PostTickLoc = NewProjectile->GetActorLocation();
				for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
				{
					AUTCharacter* Target = Cast<AUTCharacter>(*It);
					if (Target && Target != UTOwner && !Target->IsDead())
					{
						if (GS && GS->OnSameTeam(UTOwner, Target)) continue;

						FVector RewoundLoc = Target->GetRewindLocation(RewindTime);
						float CapRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
						float CapHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

						// Point-to-capsule distance check
						FVector CapsuleTop = RewoundLoc + FVector(0, 0, CapHeight - CapRadius);
						FVector CapsuleBot = RewoundLoc - FVector(0, 0, CapHeight - CapRadius);
						FVector ClosestOnCapsule = FMath::ClosestPointOnSegment(PostTickLoc, CapsuleBot, CapsuleTop);
						float DistSqr = FVector::DistSquared(PostTickLoc, ClosestOnCapsule);

						float CombinedRadius = CapRadius + ProjHitRadius;
						if (DistSqr < (CombinedRadius * CombinedRadius))
						{
							FVector DirToProj = (PostTickLoc - ClosestOnCapsule).GetSafeNormal();
							FVector HitLocation = ClosestOnCapsule + (DirToProj * CapRadius);
							FVector HitNormal = -CatchupVelocity.GetSafeNormal();

							NewProjectile->ProcessHit(Target, Target->GetCapsuleComponent(), HitLocation, HitNormal);
							bHitRegistered = true;
							break;
						}
					}
				}

				if (bHitRegistered)
				{
					return nullptr;
				}

				// Subtract the fast-forward time from the projectile's remaining
				// lifespan so its total flight matches what it would have been
				// without the catchup. Clamped to a 0.1s floor so a heavily
				// fast-forwarded projectile (e.g., spawned during a server stall
				// where CatchupTickDelta > original lifespan) doesn't expire on
				// the same frame it spawned, and never goes negative — engine
				// behavior on negative LifeSpan is treated as "never expire" in
				// some paths, which would leak immortal projectiles.
				if (NewProjectile->GetLifeSpan() > 0.f)
				{
					const float Remaining = NewProjectile->GetLifeSpan() - CatchupTickDelta;
					NewProjectile->SetLifeSpan(FMath::Max(0.1f, Remaining));
				}
			}
			else
			{
				// Hit registered via rewind check - projectile already processed
				return nullptr;
			}
		}
		else
		{
			NewProjectile->SetForwardTicked(false);
		}
	}
	// ----------------------------------------
	// 7) CLIENT: Setup fake projectile
	// ----------------------------------------
    /*
    else
    {
        NewProjectile->InitFakeProjectile(OwningPlayer);

        // Shock cores need pristine trajectories - don't mess with their lifespan
        // or track them for fake projectile confirmation
        if (!bIsShockCore)
        {
            
            if (CatchupTickDelta > 0.f)
            {
                //NewProjectile->SetLifeSpan(
                //    FMath::Min(NewProjectile->GetLifeSpan(), 2.f * FMath::Max(0.f, CatchupTickDelta))
                //);
                float PingSeconds = (OwningPlayer->PlayerState) ? OwningPlayer->PlayerState->ExactPing * 0.001f : 0.0f;
                NewProjectile->SetLifeSpan(PingSeconds + 0.10f);
            }
            

            PendingFakeProjectile = NewProjectile;
            PendingFakeProjectileEventIndex = ClientFireEventIndex.IsValidIndex(CurrentFireMode)
                ? ClientFireEventIndex[CurrentFireMode] : -1;
        }
    }
    */
else
{
    NewProjectile->InitFakeProjectile(OwningPlayer);

    // Track in our custom array for rejection cleanup via ClientConfirmFireEvent
    int32 EventIdx = ClientFireEventIndex.IsValidIndex(CurrentFireMode)
        ? ClientFireEventIndex[CurrentFireMode] : -1;

    PendingFakeProjectiles.Add(FPendingFakeProjectile(NewProjectile, EventIdx, CurrentFireMode));

    // Cleanup: Remove stale entries (destroyed projectiles or old indices)
    // Keep array from growing indefinitely
    for (int32 i = PendingFakeProjectiles.Num() - 1; i >= 0; i--)
    {
        if (!PendingFakeProjectiles[i].Projectile.IsValid())
        {
            PendingFakeProjectiles.RemoveAt(i);
        }
    }

    // Safety cap - if somehow we have too many pending, trim oldest
    while (PendingFakeProjectiles.Num() > 10)
    {
        PendingFakeProjectiles.RemoveAt(0);
    }
}



	return NewProjectile;
}




void AUTWeaponFix::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
    // COMPLETE REIMPLEMENTATION - Don't call Super!
    // Calculate aim ONCE and use those values throughout

    checkSlow(InstantHitInfo.IsValidIndex(CurrentFireMode));

    // 1. Calculate aim ONCE - these values will be used for the entire function
    const FVector SpawnLocation = GetFireStartLoc();
    const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
    const FVector FireDir = SpawnRotation.Vector();
    const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

    // DEBUG: Log what we calculated


    // 2. Do the hit trace
    FHitResult Hit;
    AUTPlayerController* UTPC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
    AUTPlayerState* PS = (UTOwner && UTOwner->Controller) ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : nullptr;
    float PredictionTime = GetHitValidationPredictionTime();
    HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, Hit, PredictionTime);



    // --------------------------------------------------------------------------
// START DEBUG LOGGING
// --------------------------------------------------------------------------
    if (Role == ROLE_Authority)
    {
        // Case 1: Client claimed a hit, but Server disagrees
        if (ReceivedHitScanHitChar != nullptr && Hit.Actor != ReceivedHitScanHitChar)
        {
            // Calculate how close the shot actually came on the Server
            float ClosestDist = 9999.f;
            FVector ClosestPointOnRay, ClosestPointOnCapsule;

            // Rewind the claimed target to where the Server thinks it was
            FVector RewoundLoc = ReceivedHitScanHitChar->GetRewindLocation(PredictionTime);
            float CapRadius = ReceivedHitScanHitChar->GetCapsuleComponent()->GetScaledCapsuleRadius();
            float CapHeight = ReceivedHitScanHitChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

            // Math: Distance between the Shot Ray and the Rewound Capsule Segment
            FVector CapsuleSegTop = RewoundLoc + FVector(0, 0, CapHeight - CapRadius);
            FVector CapsuleSegBot = RewoundLoc - FVector(0, 0, CapHeight - CapRadius);

            FMath::SegmentDistToSegmentSafe(
                SpawnLocation, EndTrace,
                CapsuleSegBot, CapsuleSegTop,
                ClosestPointOnRay, ClosestPointOnCapsule
            );

            ClosestDist = FVector::Dist(ClosestPointOnRay, ClosestPointOnCapsule);
            float MissMargin = ClosestDist - CapRadius; // How far off the "skin" of the capsule
            /*
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[DEBUG] HIT REJECTED! Client Claimed: %s | Server Hit: %s | RewindTime: %.3fms | Missed Capsule By: %.2f units"),
                *ReceivedHitScanHitChar->GetName(),
                Hit.Actor.Get() ? *Hit.Actor->GetName() : TEXT("None"),
                PredictionTime * 1000.f,
                MissMargin); 
			*/
        }

        // Case 2: Ghost Miss (Both missed, but maybe it was close?)
        // Useful for checking if your Rewind Math is aligning the hitbox correctly
        else if (ReceivedHitScanHitChar == nullptr && Hit.Actor == nullptr)
        {
            // Scan for nearest player to see how close we were
            float BestDist = 9999.f;
            AUTCharacter* NearestChar = nullptr;

            for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
            {
                AUTCharacter* TestChar = Cast<AUTCharacter>(*It);
                if (TestChar && TestChar != UTOwner && !TestChar->IsDead())
                {
                    FVector TestRewind = TestChar->GetRewindLocation(PredictionTime);
                    // Simple point-to-line check for debug speed
                    float Dist = FMath::PointDistToLine(TestRewind, EndTrace - SpawnLocation, SpawnLocation);
                    if (Dist < BestDist) { BestDist = Dist; NearestChar = TestChar; }
                }
            }

            if (NearestChar && BestDist < 80.0f) // Only log if reasonably close (e.g. < 80 units)
            {
                //UE_LOG(LogUTWeaponFix, Log, TEXT("[DEBUG] NEAR MISS. Nearest: %s | Dist: %.2f | RewindTime: %.3fms"),
                //    *NearestChar->GetName(), BestDist, PredictionTime * 1000.f);
            }
        }
    }



    // 3. Check for headshot (using the SAME SpawnLocation and FireDir)
    if (UTPC && bCheckHeadSphere && (Cast<AUTCharacter>(Hit.Actor.Get()) == nullptr) &&
        ((Spread.Num() <= GetCurrentFireMode()) || (Spread[GetCurrentFireMode()] == 0.f)) &&
        (UTOwner->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere))
    {
        AUTCharacter* AltTarget = Cast<AUTCharacter>(UUTGameplayStatics::ChooseBestAimTarget(
            UTPC, SpawnLocation, FireDir, 0.7f, (Hit.Location - SpawnLocation).Size(),
            150.f, AUTCharacter::StaticClass()));
        if (AltTarget != nullptr && (AltTarget->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere) &&
            AltTarget->IsHeadShot(SpawnLocation, FireDir, 1.1f, UTOwner, PredictionTime))
        {
            Hit = FHitResult(AltTarget, AltTarget->GetCapsuleComponent(),
                SpawnLocation + FireDir * ((AltTarget->GetHeadLocation() - SpawnLocation).Size() -
                    AltTarget->GetCapsuleComponent()->GetUnscaledCapsuleRadius()), -FireDir);
        }
    }

    // 4. Server-side processing
    if (Role == ROLE_Authority)
    {
        if (PS && (ShotsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(ShotsStatsName, 1);
        }
        UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
        UTOwner->SetFlashExtra(0, CurrentFireMode);
        UTOwner->ForceNetUpdate();
        // Bot warnings
        if (UTPC != nullptr)
        {
            APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
            if (PawnTarget != nullptr)
            {
                // DON'T cache this! That's what causes the ghost hits
                // UTPC->LastShotTargetGuess = PawnTarget;
            }
            if (bDealDamage && PawnTarget != nullptr)
            {
                AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                if (EnemyBot != nullptr)
                {
                    EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                }
            }
        }
        else if (bDealDamage)
        {
            AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
            if (B != nullptr)
            {
                APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
                if (PawnTarget == nullptr)
                {
                    PawnTarget = Cast<APawn>(B->GetTarget());
                }
                if (PawnTarget != nullptr)
                {
                    AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                    if (EnemyBot != nullptr)
                    {
                        EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                    }
                }
            }
        }
    }
    else
    {
        // CLIENT SIDE:
        // If we have prediction time (delayed shot), queue the effect.
        if (PredictionTime > 0.f)
        {
            PlayPredictedImpactEffects(Hit.Location);
        }
        // If Prediction is 0 (Instant Hit / Your Setup), set it NOW.
        // This was missing! Without this, the local beam never draws.
        else
        {
            UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
        }
    }
    // 5. Deal damage
    if (Hit.Actor != nullptr && Hit.Actor->bCanBeDamaged && bDealDamage)
    {
        if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(HitsStatsName, 1);
        }
        // Cache impact point for ServerShield hitbox analysis (server only, read in ModifyDamage)
        LastHitscanImpactPoint = Hit.ImpactPoint;
        OnHitScanDamage(Hit, FireDir);
        Hit.Actor->TakeDamage(InstantHitInfo[CurrentFireMode].Damage,
            FUTPointDamageEvent(InstantHitInfo[CurrentFireMode].Damage, Hit, FireDir,
                InstantHitInfo[CurrentFireMode].DamageType, FireDir * GetImpartedMomentumMag(Hit.Actor.Get())),
            UTOwner->Controller, this);
    }

    if (OutHit != nullptr)
    {
        *OutHit = Hit;
    }

    // 6. Clear caches
    if (UTOwner)
    {
        if (UTPC)
        {
            UTPC->LastShotTargetGuess = nullptr;
        }
        TargetedCharacter = nullptr;
    }
}


void AUTWeaponFix::DetachFromOwner_Implementation()
{
    GetWorldTimerManager().ClearTimer(DeferredActiveStateHandle);
    GetWorldTimerManager().ClearTimer(DelayedPutDownHandle);
    // Safety: Kill timers if the weapon is destroyed or dropped
    for (int32 i = 0; i < 2; i++)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
    }
    ClearPendingFakeProjectiles();
    // Call the base class implementation (which does the unregistering/holstering logic you pasted)
    Super::DetachFromOwner_Implementation();
}




bool AUTWeaponFix::PutDown()
{
    // NOTE: Do NOT clear DeferredActiveStateHandle here.
    // The deferred timer keeps us in FiringState so Super::PutDown() routes to
    // UUTWeaponStateFiring_Transactional::PutDown(), which has cooldown-aware
    // weapon switch timing. Clearing it would bypass that logic.
    // The timer firing later is harmless — DeferredGotoActiveState guards against
    // running in wrong states (UnequippingState, InactiveState, ActiveState).
    // 1. Try to put the weapon down via the base class
    bool bPutDownResult = Super::PutDown();
    // 2. If it succeeded, kill the timers immediately.
    // This prevents the "Backpack Fire" bug where a buffered shot 
    // goes off 0.1s after you switched weapons.
    if (bPutDownResult)
    {
        // If we have a Retry Timer running, it means the user is holding Fire 
        // waiting for cooldown. Since we are putting this gun away, we must 
        // tell the Pawn "User is holding fire" so the NEXT gun picks it up.
        if (UTOwner)
        {
            for (int32 i = 0; i < 2; i++)
            {
                if (GhostFix())
                {
                    // GHOST FIX: carry the REAL held state across the switch instead of
                    // graduating a stale cooldown-retry. Locally-controlled (client / listen
                    // host): held -> new weapon auto-fires (feature kept); tap/released ->
                    // cleared, no phantom rocket. Dedicated-server remote players: PendingFire
                    // is owned by Server{Start,Stop}FireFixed, never graduated here.
                    if (UTOwner->IsLocallyControlled())
                    {
                        UTOwner->SetPendingFire(i, bFireHeldByPlayer[i]);
                    }
                    if (FireDbg())
                    {
                        UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] PutDown graduate mode=%d role=%d local=%d held=%d retryActive=%d -> pending=%d"),
                            i, (int32)Role, (UTOwner->IsLocallyControlled() ? 1 : 0),
                            (bFireHeldByPlayer[i] ? 1 : 0),
                            (GetWorldTimerManager().IsTimerActive(RetryFireHandle[i]) ? 1 : 0),
                            (UTOwner->IsPendingFire(i) ? 1 : 0));
                    }
                }
                else if (GetWorldTimerManager().IsTimerActive(RetryFireHandle[i]))
                {
                    // LEGACY (ncp.GhostFix=0): graduate the local retry timer to a Pawn flag.
                    // NEVER graduate a cross-mode stall-fix retry (ncp.CrossModeRetry): that
                    // arm covers a press landing in another mode's firing tail — the classic
                    // tap-then-switch motion — and graduating it makes the next weapon fire a
                    // shot the player never pressed (the exact ghost class GhostFix targets).
                    if (!bCrossModeRetryArmed[i])
                    {
                        UTOwner->SetPendingFire(i, true);
                        UE_LOG(LogUTWeaponFix, Verbose, TEXT("PutDown: Transferring Retry %d to Pawn PendingFire"), i);
                    }
                    else if (FireDbg())
                    {
                        UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] PutDown SKIP graduation of cross-mode retry mode=%d (stall-fix arm, not held intent)"), i);
                    }
                }
            }
        }
        // A) Kill any pending retry timers
        for (int32 i = 0; i < 2; i++)
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
        }
        // B) Reset the Gatekeeper Flags
        // This fixes the "Jam" bug where the weapon remembers it was firing Mode 1.
        CurrentlyFiringMode = 255;
        // C) Clear Replication Flags
        // Ensures the server state is clean for this weapon instance.
        for (int32 i = 0; i < FireModeActiveState.Num(); i++)
        {
            FireModeActiveState[i] = 0;
        }
        // --- FIX: CLEAR PAWN INPUT ---
            // This stops the "PendingFire" flag from bleeding into the next weapon
            // causing it to auto-fire immediately upon equip.
        //if (UTOwner)
        //{
        //    UTOwner->SetPendingFire(0, false);
        //    UTOwner->SetPendingFire(1, false);
        //}
    }
    return bPutDownResult;
}


void AUTWeaponFix::FireCone()
{
    //UE_LOG(LogUTWeapon, Verbose, TEXT("%s::FireCone()"), *GetName());

    checkSlow(InstantHitInfo.IsValidIndex(CurrentFireMode));
    checkSlow(InstantHitInfo[CurrentFireMode].ConeDotAngle > 0.0f);

    const FVector SpawnLocation = GetFireStartLoc();
    const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
    const FVector FireDir = SpawnRotation.Vector();
    const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

    AUTPlayerController* UTPC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : NULL;
    AUTPlayerState* PS = (UTOwner && UTOwner->Controller) ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : NULL;
    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

    // --- FIX START ---
    // Use custom prediction time logic (Transactional 120ms cap logic)
    float PredictionTime = GetHitValidationPredictionTime();
    // --- FIX END ---

    // --- FIX START ---
    // Use DefaultResponseParam instead of the private global 'WorldResponseParams'
    FCollisionResponseParams TraceResponseParams = FCollisionResponseParams::DefaultResponseParam;
    // --- FIX END ---

    TraceResponseParams.CollisionResponse.SetResponse(COLLISION_PROJECTILE_SHOOTABLE, ECR_Block);
    TArray<FOverlapResult> OverlapHits;
    TArray<FHitResult> RealHits;
    GetWorld()->OverlapMultiByChannel(OverlapHits, SpawnLocation, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceRange));
    for (const FOverlapResult& Overlap : OverlapHits)
    {
        if (Overlap.GetActor() != nullptr)
        {
            FVector ObjectLoc = Overlap.GetComponent()->Bounds.Origin;
            if (((ObjectLoc - SpawnLocation).GetSafeNormal() | FireDir) >= InstantHitInfo[CurrentFireMode].ConeDotAngle)
            {
                bool bClear;
                int32 Retries = 2;
                FCollisionQueryParams QueryParams(NAME_None, true, UTOwner);
                do
                {
                    FHitResult Hit;
                    if (InstantHitInfo[CurrentFireMode].TraceHalfSize <= 0.0f)
                    {
                        bClear = !GetWorld()->LineTraceSingleByChannel(Hit, SpawnLocation, ObjectLoc, COLLISION_TRACE_WEAPONNOCHARACTER, QueryParams, TraceResponseParams);
                    }
                    else
                    {
                        bClear = !GetWorld()->SweepSingleByChannel(Hit, SpawnLocation, ObjectLoc, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceHalfSize), QueryParams, TraceResponseParams);
                    }
                    if (bClear || Hit.GetActor() == nullptr || !ShouldTraceIgnore(Hit.GetActor()))
                    {
                        break;
                    }
                    else
                    {
                        QueryParams.AddIgnoredActor(Hit.GetActor());
                    }
                } while (Retries-- > 0);
                if (bClear)
                {
                    // trace only against target to get good hit info
                    FHitResult Hit;
                    if (!Overlap.GetComponent()->LineTraceComponent(Hit, SpawnLocation, ObjectLoc, FCollisionQueryParams(NAME_None, true, UTOwner)))
                    {
                        Hit = FHitResult(Overlap.GetActor(), Overlap.GetComponent(), ObjectLoc, -FireDir);
                    }
                    RealHits.Add(Hit);
                }
            }
        }
    }
    // do characters separately to handle forward prediction
    for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
    {
        AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
        if (Target && (Target != UTOwner) && (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target)))
        {
            // find appropriate rewind position, and test against trace from StartLocation to Hit.Location
            // NOTE: This uses GetRewindLocation, which in your Character override respects 'PredictionTime' on the server
            FVector TargetLocation = ((PredictionTime > 0.f) && (Role == ROLE_Authority)) ? Target->GetRewindLocation(PredictionTime) : Target->GetActorLocation();

            const FVector Diff = TargetLocation - SpawnLocation;
            if (Diff.Size() <= InstantHitInfo[CurrentFireMode].TraceRange && (Diff.GetSafeNormal() | FireDir) >= InstantHitInfo[CurrentFireMode].ConeDotAngle)
            {
                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                if (Target->UTCharacterMovement && Target->UTCharacterMovement->bIsFloorSliding)
                {
                    TargetLocation.Z = TargetLocation.Z - CollisionHeight + Target->SlideTargetHeight;
                    CollisionHeight = Target->SlideTargetHeight;
                }
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();

                bool bHitTarget = false;
                FVector ClosestPoint(0.f);
                FVector ClosestCapsulePoint = TargetLocation;
                if (CollisionRadius >= CollisionHeight)
                {
                    ClosestPoint = TargetLocation;
                }
                else
                {
                    FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
                    FMath::SegmentDistToSegmentSafe(SpawnLocation, TargetLocation, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
                }
                // first find proper hit location on surface of capsule
                float ClosestDistSq = (ClosestPoint - ClosestCapsulePoint).SizeSquared();
                float BackDist = FMath::Sqrt(FMath::Max(0.f, CollisionRadius * CollisionRadius - ClosestDistSq));
                const FVector HitLocation = ClosestPoint + BackDist * (SpawnLocation - EndTrace).GetSafeNormal();

                bool bClear;
                int32 Retries = 2;
                FCollisionQueryParams QueryParams(NAME_None, true, UTOwner);
                do
                {
                    FHitResult Hit;
                    if (InstantHitInfo[CurrentFireMode].TraceHalfSize <= 0.0f)
                    {
                        bClear = !GetWorld()->LineTraceTestByChannel(SpawnLocation, HitLocation, COLLISION_TRACE_WEAPONNOCHARACTER, QueryParams, TraceResponseParams);
                    }
                    else
                    {
                        bClear = !GetWorld()->SweepTestByChannel(SpawnLocation, HitLocation, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceHalfSize), QueryParams, TraceResponseParams);
                    }
                    if (bClear || Hit.GetActor() == nullptr || !ShouldTraceIgnore(Hit.GetActor()))
                    {
                        break;
                    }
                    else
                    {
                        QueryParams.AddIgnoredActor(Hit.GetActor());
                    }
                } while (Retries-- > 0);
                if (bClear)
                {
                    FHitResult* NewHit = new(RealHits) FHitResult;
                    NewHit->Location = HitLocation;
                    NewHit->Normal = (EndTrace - ClosestCapsulePoint).GetSafeNormal();
                    NewHit->ImpactNormal = NewHit->Normal;
                    NewHit->Actor = Target;
                    NewHit->bBlockingHit = true;
                    NewHit->Component = Target->GetCapsuleComponent();
                    NewHit->ImpactPoint = ClosestPoint; //FIXME
                    NewHit->Time = (ClosestPoint - SpawnLocation).Size() / (EndTrace - SpawnLocation).Size();
                }
            }
        }
    }
    RealHits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });

    if (Role == ROLE_Authority)
    {
        if (PS && (ShotsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(ShotsStatsName, 1);
        }
        //UTOwner->IncrementFlashCount(CurrentFireMode);
        // fix projectile spawning of flak shards for others
        FVector FlashLoc = RealHits.Num() > 0 ? RealHits[0].Location : EndTrace;
        UTOwner->SetFlashLocation(FlashLoc, CurrentFireMode);
        // warn bot target, if any
        if (UTPC != nullptr)
        {
            APawn* PawnTarget = RealHits.Num() > 0 ? Cast<APawn>(RealHits[0].Actor.Get()) : nullptr;
            if (PawnTarget != nullptr)
            {
                // UTPC->LastShotTargetGuess = PawnTarget; // Disabled for transactional accuracy
            }
            if (PawnTarget) // Added check to prevent crash if cast failed
            {
                AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                if (EnemyBot != nullptr)
                {
                    EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                }
            }
        }
        else
        {
            AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
            if (B != NULL)
            {
                APawn* PawnTarget = RealHits.Num() > 0 ? Cast<APawn>(RealHits[0].Actor.Get()) : nullptr;
                if (PawnTarget == NULL)
                {
                    PawnTarget = Cast<APawn>(B->GetTarget());
                }
                if (PawnTarget != nullptr)
                {
                    AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                    if (EnemyBot != nullptr)
                    {
                        EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                    }
                }
            }
        }
    }
    for (const FHitResult& Hit : RealHits)
    {
        if (UTOwner && Hit.Actor != NULL && Hit.Actor->bCanBeDamaged)
        {
            if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None))
            {
                PS->ModifyStatsValue(HitsStatsName, 1);
            }
            Hit.Actor->TakeDamage(InstantHitInfo[CurrentFireMode].Damage, FUTPointDamageEvent(InstantHitInfo[CurrentFireMode].Damage, Hit, FireDir, InstantHitInfo[CurrentFireMode].DamageType, FireDir * GetImpartedMomentumMag(Hit.Actor.Get())), UTOwner->Controller, this);
        }
    }
}





void AUTWeaponFix::PrepareConfiguredWeaponSkin()
{
	if (!bSkinsEnabled || UTOwner == nullptr)
	{
		ApplyResolvedWeaponSkin(nullptr);
		return;
	}

	// The replicated Character array is the acknowledgement for every viewer,
	// including the owner. Never let an unaccepted local config remain visible.
	ApplyResolvedWeaponSkin(FindWeaponSkinForClass(UTOwner->WeaponSkins, GetClass()));
}

uint32 AUTWeaponFix::GetWeaponSkinTargetSlotMask(FName WeaponSkinCustomizationTag,
	bool bFirstPersonMesh)
{
	// Verified in-editor (328-rc) by matching each shipped skin material's texture set
	// against the mesh slot it replaces:
	//   Flak — FlakVoid 1P M_Flak_Skin_Void01_P / 3P M_Flak_Skin_Void01 replace
	//     M_Flak_Gun_Inst / M_Flak_Gun_3P_Inst, and Flak_Cannon_1p AND _3p each carry
	//     that body material on slot 0 AND slot 1 -> {0,1} in both views.
	//   Lightning Gun — ASYMMETRIC. PinkLG 1P MAT_INS_LG_Pink_E0_1p uses the PartTWO
	//     textures (T_LightingGunTwo_*), which Lightning_Gun_1p carries on slot 0.
	//     PinkLG 3P MAT_INS_LG_Pink_E1_3p uses the PartONE textures
	//     (T_LightingGun_one_*), which Lightning_Gun_3p carries on slot 1. The authored
	//     E0/E1 names are the element indices -> 1P {0}, 3P {1}. Writing the other slot
	//     would paint that section with the wrong part's textures.
	//   Everything else keeps stock behaviour: slot 0 only.
	// Slots outside the mask are never captured or written, so ammo counters, decals,
	// glass, the LG's other part, and SetupSpecialMaterials()' Shock Rifle screen all
	// keep their originals. Slot NAMES on these meshes are unreliable (scrambled /
	// generic), which is why these are explicit verified indices.
	static const FName NAME_FlakCannonSkins(TEXT("FlakCannon_Skins"));
	// Both spellings accepted while the LG weapon/skin tags are being harmonised
	// (UTNPLightningGun CDO reads LightningRifle_Skins; PinkLG reads LG_Skins).
	static const FName NAME_LGSkins(TEXT("LG_Skins"));
	static const FName NAME_LightningRifleSkins(TEXT("LightningRifle_Skins"));

	if (WeaponSkinCustomizationTag == NAME_FlakCannonSkins)
	{
		return 0x3u;
	}
	if (WeaponSkinCustomizationTag == NAME_LGSkins ||
		WeaponSkinCustomizationTag == NAME_LightningRifleSkins)
	{
		return bFirstPersonMesh ? 0x1u : 0x2u;
	}
	return 0x1u;
}

void AUTWeaponFix::ApplyResolvedWeaponSkin(UUTWeaponSkin* Skin)
{
	// Only authority needs the weapon-level identity: it is copied to a dropped
	// pickup. Viewers resolve their materials from the replicated Character array.
	if (Role == ROLE_Authority)
	{
		WeaponSkin = Skin;
	}
	if (!bSkinsEnabled || GetNetMode() == NM_DedicatedServer || Mesh == nullptr ||
		Mesh->GetNumMaterials() < 1)
	{
		return;
	}

	// Slots this weapon family renders its 1P skin on: slot 0 for normal weapons and
	// the Lightning Gun, slots 0+1 for Flak. Slots outside the mask (counters, decals,
	// glass, the Shock screen) stay owned by the mesh / SetupSpecialMaterials() and are
	// never captured or touched here.
	const uint32 TargetSlotMask = GetWeaponSkinTargetSlotMask(WeaponSkinCustomizationTag, true);

	// Capture the untouched originals once, per targeted slot, so Default restores the
	// exact instance the slot shipped with. Stock SetSkin() captures every slot; if it
	// has not run yet, seed up to the highest slot we are going to patch.
	const int32 SeedSlotCount = FMath::Min(
		((TargetSlotMask & 0x2u) != 0u) ? 2 : 1, Mesh->GetNumMaterials());
	while (SavedMeshMaterials.Num() < SeedSlotCount)
	{
		SavedMeshMaterials.Add(Mesh->GetMaterial(SavedMeshMaterials.Num()));
	}
	if (!bCapturedOriginalFPSMaterial)
	{
		OriginalFPSMaterial =
			((TargetSlotMask & 0x1u) != 0u && SavedMeshMaterials.IsValidIndex(0))
			? SavedMeshMaterials[0]
			: nullptr;
		OriginalFPSMaterialSecondary =
			((TargetSlotMask & 0x2u) != 0u && SavedMeshMaterials.IsValidIndex(1))
			? SavedMeshMaterials[1]
			: nullptr;
		bCapturedOriginalFPSMaterial = true;
	}

	const bool bUseSelectedMaterial = Skin != nullptr && Skin->FPSMaterial != nullptr;
	UMaterialInterface* DesiredParent =
		bUseSelectedMaterial
		? Skin->FPSMaterial
		: OriginalFPSMaterial;
	if (DesiredParent != AppliedFPSMaterial ||
		bUseSelectedMaterial != (AppliedFPSMaterialInstance != nullptr))
	{
		AppliedFPSMaterial = DesiredParent;
		AppliedFPSMaterialInstance =
			(bUseSelectedMaterial && DesiredParent != nullptr)
			? UMaterialInstanceDynamic::Create(DesiredParent, Mesh)
			: nullptr;
	}

	// One actor-local MID (AppliedFPSMaterialInstance) is reused across every targeted
	// slot of THIS mesh; Default restores each slot's captured original. The
	// SavedMeshMaterials patch makes a later body-override clear restore this choice.
	UMaterialInterface* const OriginalBySlot[MaxWeaponSkinTargetSlots] =
		{ OriginalFPSMaterial, OriginalFPSMaterialSecondary };
	const bool bBodyOverrideActive = (UTOwner != nullptr && UTOwner->GetSkin() != nullptr);
	static const FName NAME_Scale(TEXT("Scale"));
	for (int32 Slot = 0; Slot < MaxWeaponSkinTargetSlots; ++Slot)
	{
		if (((TargetSlotMask >> Slot) & 0x1u) == 0u || Slot >= Mesh->GetNumMaterials())
		{
			continue;
		}
		UMaterialInterface* DesiredSlotMaterial = AppliedFPSMaterialInstance != nullptr
			? Cast<UMaterialInterface>(AppliedFPSMaterialInstance)
			: OriginalBySlot[Slot];
		if (SavedMeshMaterials.IsValidIndex(Slot))
		{
			SavedMeshMaterials[Slot] = DesiredSlotMaterial;
		}
		// Character body overrides own every visible weapon slot while active.
		if (bBodyOverrideActive)
		{
			continue;
		}
		Mesh->SetMaterial(Slot, DesiredSlotMaterial);
		// MeshMIDs is compact; index i is slot i's MID whenever slot i has a
		// material. Avoid touching it for the null-slot edge case.
		if ((OriginalBySlot[Slot] != nullptr || AppliedFPSMaterialInstance != nullptr) &&
			MeshMIDs.IsValidIndex(Slot))
		{
			MeshMIDs[Slot] = Cast<UMaterialInstanceDynamic>(DesiredSlotMaterial);
			if (MeshMIDs[Slot] != nullptr)
			{
				MeshMIDs[Slot]->SetScalarParameterValue(NAME_Scale, WeaponRenderScale);
			}
		}
	}
	if (!bBodyOverrideActive && SkinTiming())
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] %s applied skin=%s slot-mask=0x%x"), *GetName(),
			Skin != nullptr ? *Skin->GetName() : TEXT("Default"), TargetSlotMask);
	}
}

void AUTWeaponFix::BringUp(float OverflowTime)
{
	const bool bLogSkinTiming = SkinTiming();
	const double BringUpStartTime = bLogSkinTiming ? FPlatformTime::Seconds() : 0.0;
 


    
    float CurrentTime = GetWorld()->GetTimeSeconds();
	float MaxBlockTime = 0.f;

	// =======================================================================
	// FIX #1: CHECK THIS WEAPON'S OWN COOLDOWN DEBT FIRST
	// =======================================================================
	// When switching Sniper → Shock → Sniper, the Sniper's own LastFireTime
	// still has the cooldown debt from before the switch.
	for (int32 i = 0; i < LastFireTime.Num(); i++)
	{
		if (LastFireTime[i] > 0.f)
		{
			float RefireEnd = LastFireTime[i] + GetRefireTime(i);

			// If cooldown hasn't expired yet, we must wait
			if (RefireEnd > CurrentTime && RefireEnd > MaxBlockTime)
			{
				MaxBlockTime = RefireEnd;
			}
		}
	}

	// =======================================================================
	// FIX #2: CHECK OTHER WEAPONS 
	// =======================================================================
	// This handles the case where you fire Shock → switch to Sniper
	// The Sniper inherits the Shock's remaining cooldown (scaled)
	if (UTOwner)
	{
		for (TInventoryIterator<AUTWeapon> It(UTOwner); It; ++It)
		{
			AUTWeapon* OtherWeapon = *It;

			// Only check OTHER valid AUTWeaponFix weapons
			if (OtherWeapon && OtherWeapon != this && OtherWeapon->IsA(AUTWeaponFix::StaticClass()))
			{
				AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(OtherWeapon);
				if (FixWeapon)
				{
					// Back-calculate when the switch actually started
					float PutDownDuration = FixWeapon->GetPutDownTime();
					float SwitchStartTime = CurrentTime - OverflowTime - PutDownDuration;

					for (int32 i = 0; i < FixWeapon->LastFireTime.Num(); i++)
					{
						if (FixWeapon->LastFireTime[i] > 0.f)
						{
							float RefireEnd = FixWeapon->LastFireTime[i] + FixWeapon->GetRefireTime(i);
							float RemainingAtSwitch = RefireEnd - SwitchStartTime;

							// Only penalize if there was actual debt at moment of switch
							if (RemainingAtSwitch > 0.f)
							{
								// Apply scaling (e.g., 0.65 for fast-switch gamemodes)
								float ScaledRemaining = RemainingAtSwitch * FixWeapon->RefirePutDownTimePercent;
								float TheoreticalReadyTime = SwitchStartTime + ScaledRemaining;

								if (TheoreticalReadyTime > MaxBlockTime)
								{
									MaxBlockTime = TheoreticalReadyTime;
								}
							}
						}
					}
				}
			}
		}
	}

	// =======================================================================
	// APPLY THE RESTRICTION
	// =======================================================================
	if (MaxBlockTime > CurrentTime)
	{
		if (MaxBlockTime > EarliestFireTime)
		{
			EarliestFireTime = MaxBlockTime;
			UE_LOG(LogUTWeaponFix, Verbose,
				TEXT("[BringUp] %s: EarliestFireTime set to %.3f (blocks for %.3fms)"),
				*GetName(), EarliestFireTime, (MaxBlockTime - CurrentTime) * 1000.f);
			// DIAGNOSTIC (net-safe, survives Shipping): flag an ABNORMAL bring-up block (>1s) — the
			// prime suspect for the silent multi-second rocket fire stall. Shows what set it + how far.
			if (Role == ROLE_Authority && (MaxBlockTime - CurrentTime) > 1.0f)
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[FireBlock] %s BringUp set EarliestFireTime %.2fs ahead (=%.2f, now=%.2f)"),
					*GetName(), MaxBlockTime - CurrentTime, EarliestFireTime, CurrentTime);
			}
		}
	}

	PrepareConfiguredWeaponSkin();
	const double SuperStartTime = bLogSkinTiming ? FPlatformTime::Seconds() : 0.0;
	Super::BringUp(OverflowTime);
	const double SuperEndTime = bLogSkinTiming ? FPlatformTime::Seconds() : 0.0;

	// Per-weapon hide (BP-parity, 2026-07-19): visibility-only — see
	// ApplyWeaponHideState. Applied BOTH ways every equip so the shared arm
	// bones, and a mesh left invisible by a previous owner's local hide, are
	// restored when this weapon is NOT hidden. Local viewer only: this is pure
	// render state; hiding bones server-side would be wasted work.
	if (UTOwner && GetNetMode() != NM_DedicatedServer && UTOwner->IsLocallyControlled())
	{
		bool* bHidden = HiddenWeaponsByTag.Find(FName(*GetClass()->GetName()));
		ApplyWeaponHideState(this, UTOwner, bHidden && *bHidden);
	}

	if (bLogSkinTiming)
	{
		const double BringUpEndTime = FPlatformTime::Seconds();
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] %s BringUp: pre-super=%.3fms super=%.3fms post=%.3fms total=%.3fms skin=%s"),
			*GetName(),
			(SuperStartTime - BringUpStartTime) * 1000.0,
			(SuperEndTime - SuperStartTime) * 1000.0,
			(BringUpEndTime - SuperEndTime) * 1000.0,
			(BringUpEndTime - BringUpStartTime) * 1000.0,
			WeaponSkin != nullptr ? *WeaponSkin->GetName() : TEXT("None"));
	}
}



void AUTWeaponFix::GetImpactSpawnPosition(const FVector& TargetLoc, FVector& SpawnLocation, FRotator& SpawnRotation)
{
	// CLASSIC hide only: spawn beam effects from camera center instead of the
	// muzzle socket (while classic-hidden the 1P rig is parked at its default
	// archetype seat, so the socket is nowhere near the crosshair). Makes beams
	// fire straight from the crosshair, matching projectile behavior. The default
	// BP-parity hide never enters this branch — its muzzle socket stays live at
	// the centergun seat, so Super's socket origin is correct.
	if (bClassicWeaponHide)
	{
		const bool* bHidden = HiddenWeaponsByTag.Find(FName(*GetClass()->GetName()));
		if (bHidden && *bHidden && UTOwner && UTOwner->CharacterCameraComponent)
		{
			SpawnRotation = UTOwner->CharacterCameraComponent->GetComponentRotation();
			// Offset back+down from camera so the beam is visible (spawning at exact
			// camera position makes the beam edge-on/invisible when stationary).
			// Magnitudes come from Mod.ini [NetcodePlus.WeaponSettings] HiddenBeamBack
			// + HiddenBeamDown via LoadWeaponSettings; the weaponskins menu has
			// sliders. Defaults (10 / 35) reproduce the original hardcoded behavior.
			const FVector Forward = SpawnRotation.Vector();
			const FVector Down(0.f, 0.f, -1.f);
			SpawnLocation = UTOwner->CharacterCameraComponent->GetComponentLocation()
				+ Forward * -HiddenBeamBackOffset   // pulls particles back into body
				+ Down    *  HiddenBeamDownOffset;   // drop below eye line
			return;
		}
	}

	Super::GetImpactSpawnPosition(TargetLoc, SpawnLocation, SpawnRotation);
}

void AUTWeaponFix::PlayFiringEffects()
{
	// CLASSIC hide only: the muzzle flash PSC is still attached to the (hidden)
	// muzzle socket while the beam/impact spawns from the camera-adjusted origin
	// (see GetImpactSpawnPosition). That mismatch produces a visible puff at the
	// hand while the beam comes from chest height. Suppress only the muzzle flash
	// for the current fire mode — sound, anim, kickback, and beam all still fire.
	// The default BP-parity hide needs none of this: SetVisibility(propagate)
	// already silenced the PSC children.
	UParticleSystemComponent* SavedPSC = nullptr;
	int32 SavedIndex = INDEX_NONE;
	if (bClassicWeaponHide && UTOwner)
	{
		const bool* bHidden = HiddenWeaponsByTag.Find(FName(*GetClass()->GetName()));
		if (bHidden && *bHidden)
		{
			const uint8 EffectFiringMode = (Role == ROLE_Authority || UTOwner->Controller != nullptr) ? CurrentFireMode : UTOwner->FireMode;
			if (MuzzleFlash.IsValidIndex(EffectFiringMode))
			{
				SavedPSC = MuzzleFlash[EffectFiringMode];
				SavedIndex = EffectFiringMode;
				MuzzleFlash[EffectFiringMode] = nullptr;
			}
		}
	}

	Super::PlayFiringEffects();

	if (SavedIndex != INDEX_NONE && MuzzleFlash.IsValidIndex(SavedIndex))
	{
		MuzzleFlash[SavedIndex] = SavedPSC;
	}
}

// BP-parity hidden-weapon apply (2026-07-19). The old HiddenWeaponsUTPL BP hid
// weapons by turning off RENDERING only — SetVisibility(false, propagate) on the
// gun mesh + HideBoneByName(upperarm_l/r) on the arms — leaving every transform
// and the weapon-hand pipeline untouched, so stock GetImpactSpawnPosition kept
// spawning beams from the live muzzle socket. Constraints that shape this:
//  - bVisible is OURS here; stock code owns bHiddenInGame (zoom, OverlayMesh
//    parking, spectate paths all toggle it) — using SetHiddenInGame gets fought.
//  - propagate=true also silences the muzzle-flash PSC children for free
//    (UE4.15 ActivateSystem never forces visibility back on).
//  - Arms are hidden per-BONE, not per-component: the component keeps rendering
//    (zero-scaled) and ticking, so the hand socket the weapon hangs from stays
//    live, and stock behind-view/spectate visibility logic is never fought.
// Per-component attribution of what THIS code hid (ComponentTags on the meshes it
// touched), so the show path only heals its own residue. The old UT+ content
// (HiddenWeaponsUTPL / WeaponSkinsPlus BPs, Mod.ini [UTPL] HideGun) hides with the
// SAME primitives (bVisible + bone hiding) on its own poll — force-showing state
// we didn't set wins or loses against that poll by tick order and intermittently
// un-hides guns/arms those players expect hidden (seq-51 never fought them; it
// only ever touched bHiddenInGame). Tags rather than session statics: they live
// and die with the component instance, so a hidden weapon dropped on death still
// heals on re-pickup, a respawned pawn's fresh arms are untagged (no one-shot
// reveal of bones a BP hid), and nothing cross-talks between local players.
static const FName NAME_NCPHidGun(TEXT("NCP_HidGun"));
static const FName NAME_NCPHidArms(TEXT("NCP_HidArms"));

// Remove-and-report: true if WE had hidden this component (tag present).
static bool TakeNCPHideTag(UActorComponent* Comp, const FName& Tag)
{
	return Comp != nullptr && Comp->ComponentTags.Remove(Tag) > 0;
}

void AUTWeaponFix::ApplyWeaponHideState(AUTWeapon* Weapon, AUTCharacter* Char, bool bHide)
{
	USkeletalMeshComponent* WeapMesh = (Weapon != nullptr) ? Weapon->GetMesh() : nullptr;
	AUTDualWeapon* Dual = Cast<AUTDualWeapon>(Weapon);
	USkeletalMeshComponent* LeftMesh = (Dual != nullptr) ? Dual->LeftMesh : nullptr;
	USkeletalMeshComponent* ArmsMesh = (Char != nullptr) ? Char->FirstPersonMesh : nullptr;
	// Bone names verified against the HiddenWeaponsUTPL export; missing bones
	// on a custom hands skeleton just log a warning.
	static const FName NAME_UpperArmL(TEXT("upperarm_l"));
	static const FName NAME_UpperArmR(TEXT("upperarm_r"));

	if (bHide && bClassicWeaponHide)
	{
		// CLASSIC hide (pre-2026-07-19 behavior, selectable in the weaponskins
		// menu): SetHiddenInGame both meshes; the beam comes from the camera via
		// the GetImpactSpawnPosition override. Heal BP-style residue first (style
		// flipped while hidden) — but only where the tag says it's ours.
		if (WeapMesh != nullptr)
		{
			if (TakeNCPHideTag(WeapMesh, NAME_NCPHidGun))
			{
				WeapMesh->SetVisibility(true, true);
			}
			WeapMesh->SetHiddenInGame(true);
			WeapMesh->ComponentTags.AddUnique(NAME_NCPHidGun);
		}
		if (LeftMesh != nullptr)
		{
			// Diverges from the gun mesh's pure-bHiddenInGame classic hide on
			// purpose: a mid-wield dual upgrade runs AttachLeftMesh's
			// SetHiddenInGame(false) with no BringUp and no weapon-pointer change
			// (no re-apply fires), so the hide must ALSO ride bVisible, which
			// AttachLeftMesh never touches. The show path restores both, tag-gated.
			LeftMesh->SetVisibility(false, true);
			LeftMesh->SetHiddenInGame(true);
			LeftMesh->ComponentTags.AddUnique(NAME_NCPHidGun);
		}
		if (ArmsMesh != nullptr)
		{
			if (TakeNCPHideTag(ArmsMesh, NAME_NCPHidArms))
			{
				ArmsMesh->UnHideBoneByName(NAME_UpperArmL);
				ArmsMesh->UnHideBoneByName(NAME_UpperArmR);
			}
			ArmsMesh->SetHiddenInGame(true);
			ArmsMesh->ComponentTags.AddUnique(NAME_NCPHidArms);

			// seq-51 parity: reset the 1P rig to its default seat while hidden.
			// Behaviorally inert in classic — the beam origin is computed from the
			// camera, not this seat, and the mesh is hidden; the show path re-seats
			// via UpdateWeaponHand. Kept so classic matches seq-51 exactly.
			USkeletalMeshComponent* FPMeshArchetype = Cast<USkeletalMeshComponent>(ArmsMesh->GetArchetype());
			if (FPMeshArchetype != nullptr)
			{
				ArmsMesh->SetRelativeLocationAndRotation(
					FPMeshArchetype->RelativeLocation,
					FPMeshArchetype->RelativeRotation);
			}
		}
		return;
	}

	if (WeapMesh != nullptr)
	{
		if (bHide)
		{
			// One-shot pose bake: a weapon hidden from BringUp may never render,
			// and the ctor's OnlyTickPoseWhenRendered would leave its bones at REF
			// pose — the BP's poll always hid after a rendered frame, freezing at
			// idle. Bake once so the muzzle socket freezes at idle too. A direct
			// RefreshBoneTransforms has no rendered/update-flag gate in 4.15; only
			// the per-frame tick path is gated.
			WeapMesh->TickAnimation(0.f, false);
			WeapMesh->RefreshBoneTransforms();
			// Heal classic-style residue (style flipped while hidden, ours only):
			// this hide works through bVisible; a stuck bHiddenInGame would stop
			// the 1P rig rendering and break the muzzle socket the beam rides on.
			if (WeapMesh->ComponentTags.Contains(NAME_NCPHidGun))
			{
				WeapMesh->SetHiddenInGame(false);
			}
			WeapMesh->SetVisibility(false, true);
			WeapMesh->ComponentTags.AddUnique(NAME_NCPHidGun);
			// Dual-wield second gun — the BP toggled LeftMesh alongside Mesh.
			// bVisible=false must survive a later single→dual upgrade so the left
			// gun comes up hidden too; bHiddenInGame stays stock's (true while
			// single-wielding, cleared by AttachLeftMesh on going dual).
			if (LeftMesh != nullptr)
			{
				LeftMesh->SetVisibility(false, true);
				LeftMesh->ComponentTags.AddUnique(NAME_NCPHidGun);
			}
		}
		else
		{
			// Show heals ONLY components WE tagged — force-showing anything else
			// would fight the old UT+ BP hide (see NAME_NCPHidGun above).
			if (TakeNCPHideTag(WeapMesh, NAME_NCPHidGun))
			{
				WeapMesh->SetVisibility(true, true);
				WeapMesh->SetHiddenInGame(false);   // classic residue
			}
			if (TakeNCPHideTag(LeftMesh, NAME_NCPHidGun))
			{
				LeftMesh->SetVisibility(true, true);
				// Stock keeps LeftMesh bHiddenInGame=true while single-wielding
				// (cleared only by AttachLeftMesh on going dual) — clearing it
				// here would reveal a phantom second gun to a single-wield user.
				if (Dual->bDualWeaponMode)
				{
					LeftMesh->SetHiddenInGame(false);
				}
			}
		}
	}

	if (ArmsMesh != nullptr)
	{
		if (bHide)
		{
			// Heal classic residue (ours only), then the BP hide chain.
			if (ArmsMesh->ComponentTags.Contains(NAME_NCPHidArms))
			{
				ArmsMesh->SetHiddenInGame(false);
			}
			ArmsMesh->HideBoneByName(NAME_UpperArmL, PBO_None);
			ArmsMesh->HideBoneByName(NAME_UpperArmR, PBO_None);
			ArmsMesh->ComponentTags.AddUnique(NAME_NCPHidArms);
			// BP parity: the BP's hide chain then re-seated the arms rig at an
			// ABSOLUTE relative transform — loc (-10,-20,-10), rot (0,-90,0) — i.e.
			// ~20uu left + 10 down of the stock (-15,0,0) hands seat: its
			// "centergun" position (same values its `mutate centergun` used on
			// visible guns). The 1P weapon Mesh hangs off FirstPersonMesh, so the
			// muzzle socket — and therefore the hidden BEAM ORIGIN — rides this
			// seat: center-low and bobbing, NOT the right-hand muzzle.
			ArmsMesh->SetRelativeLocationAndRotation(
				FVector(-10.f, -20.f, -10.f), FRotator(0.f, -90.f, 0.f));
		}
		else
		{
			// Only undo arm state WE tagged — the old UT+ BP hides these same
			// bones on its own schedule for its own users (see NAME_NCPHidArms).
			if (TakeNCPHideTag(ArmsMesh, NAME_NCPHidArms))
			{
				ArmsMesh->UnHideBoneByName(NAME_UpperArmL);
				ArmsMesh->UnHideBoneByName(NAME_UpperArmR);
				ArmsMesh->SetHiddenInGame(false);
			}
			// Re-seat through stock UpdateWeaponHand, NOT a bare archetype reset:
			// the archetype seat is only correct for HAND_Right. UpdateWeaponHand
			// re-applies the viewer's weapon-position preference on top — Lowered =
			// LowMeshOffset, Very Low (HAND_Hidden) = VeryLowMeshOffset — which
			// stock only applies at equip time, so stomping it here made visible
			// guns render at the normal seat and ignore the preference (the
			// seq-52 "very low is gone / guns look huge" regression). It also
			// covers the case this reset was for: a mid-equip `weaponhand show`
			// no longer parks the arms at the hidden (-10,-20,-10) seat.
			if (Weapon != nullptr)
			{
				Weapon->UpdateWeaponHand();
			}
		}
	}
}

void AUTWeaponFix::SetSkin(UMaterialInterface* NewSkin)
{
	const bool bLogSkinTiming = SkinTiming();
	const double SetSkinStartTime = bLogSkinTiming ? FPlatformTime::Seconds() : 0.0;
	const uint32 TargetSlotMask = (Mesh != nullptr)
		? GetWeaponSkinTargetSlotMask(WeaponSkinCustomizationTag, true)
		: 0u;
	UMaterialInterface* SlotZeroBefore = (Mesh != nullptr && Mesh->GetNumMaterials() > 0)
		? Mesh->GetMaterial(0)
		: nullptr;

	// The selected skin's one actor-local MID is reused across every targeted slot;
	// Default restores each slot's captured original. Patch SavedMeshMaterials first
	// so stock's restore path below reproduces this choice on every affected slot.
	UMaterialInterface* const OriginalBySlot[MaxWeaponSkinTargetSlots] =
		{ OriginalFPSMaterial, OriginalFPSMaterialSecondary };
	UMaterialInstanceDynamic* const DesiredSlotMID = AppliedFPSMaterialInstance;
	if (bCapturedOriginalFPSMaterial)
	{
		for (int32 Slot = 0; Slot < MaxWeaponSkinTargetSlots; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u && SavedMeshMaterials.IsValidIndex(Slot))
			{
				SavedMeshMaterials[Slot] = (DesiredSlotMID != nullptr)
					? Cast<UMaterialInterface>(DesiredSlotMID)
					: OriginalBySlot[Slot];
			}
		}
	}

	Super::SetSkin(NewSkin);

	bool bReusedSlotZero = false;
	if (NewSkin == nullptr && DesiredSlotMID != nullptr && Mesh != nullptr)
	{
		// A selected skin already has one actor-local MID, and stock's MeshMIDs
		// rebuild would re-wrap it. Restore that exact instance on each targeted slot.
		static const FName NAME_Scale(TEXT("Scale"));
		for (int32 Slot = 0; Slot < MaxWeaponSkinTargetSlots; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) == 0u || Slot >= Mesh->GetNumMaterials())
			{
				continue;
			}
			Mesh->SetMaterial(Slot, DesiredSlotMID);
			if (SavedMeshMaterials.IsValidIndex(Slot))
			{
				SavedMeshMaterials[Slot] = DesiredSlotMID;
			}
			if ((OriginalBySlot[Slot] != nullptr || AppliedFPSMaterialInstance != nullptr) &&
				MeshMIDs.IsValidIndex(Slot))
			{
				MeshMIDs[Slot] = DesiredSlotMID;
				DesiredSlotMID->SetScalarParameterValue(NAME_Scale, WeaponRenderScale);
			}
			bReusedSlotZero = true;
		}
	}

	if (bLogSkinTiming)
	{
		UMaterialInterface* SlotZeroAfter = (Mesh != nullptr && Mesh->GetNumMaterials() > 0)
			? Mesh->GetMaterial(0)
			: nullptr;
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] %s SetSkin: body-override=%d slots=%d slot-mask=0x%x unchanged=%d skin-reasserted=%d time=%.3fms"),
			*GetName(), NewSkin != nullptr ? 1 : 0,
			Mesh != nullptr ? Mesh->GetNumMaterials() : 0, TargetSlotMask,
			SlotZeroBefore != nullptr && SlotZeroBefore == SlotZeroAfter ? 1 : 0,
			bReusedSlotZero ? 1 : 0,
			(FPlatformTime::Seconds() - SetSkinStartTime) * 1000.0);
	}
}

// ============================================================================
// UTWeaponFix.cpp - Transactional Retry System Implementation
// ============================================================================

// 1. QUEUE LOGIC (Client Side)
void AUTWeaponFix::QueueResendStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex,
    float ClientTimestamp, FRotator ClientViewRot, AUTCharacter* ClientHitChar,
    uint8 ZOffset, FVector ClientHeadOffset)
{
    QueueResendFireEventFixed(FPendingFireEventFix(FireModeNum, InFireEventIndex,
        ClientTimestamp, ClientViewRot, ClientHitChar, ZOffset, ClientHeadOffset));
}

void AUTWeaponFix::QueueResendStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex)
{
    QueueResendFireEventFixed(FPendingFireEventFix(FireModeNum, InFireEventIndex));
}

void AUTWeaponFix::QueueResendFireEventFixed(const FPendingFireEventFix& Event)
{
    // Only the owning client needs to queue retries
    if (Role == ROLE_Authority && GetNetMode() != NM_Standalone) return;

    // Queue 2 copies. This gives us 2 retry attempts (spaced by the timer delay)
    // before we give up. This prevents infinite network flooding if the connection is dead.
    ResendFireEvents.Add(Event);
    ResendFireEvents.Add(Event);

    // Start the heartbeat timer if it's not running
    if (!GetWorldTimerManager().IsTimerActive(ResendFireHandle))
    {
        GetWorldTimerManager().SetTimer(ResendFireHandle, this, &AUTWeaponFix::ResendNextFireEventFixed, 0.04f, true);
    }
}

// 2. TIMER LOOP (Client Side)
// This runs every 0.04s (40ms) to check if we need to resend
void AUTWeaponFix::ResendNextFireEventFixed()
{
    // Safety Check: If weapon is invalid or owner is dead, abort everything
    if (!UTOwner || UTOwner->IsPendingKillPending() || UTOwner->GetWeapon() != this)
    {
        ClearFireEventsFixed();
        return;
    }

    if (ResendFireEvents.Num() > 0)
    {
        // Get the next event in the queue
        FPendingFireEventFix Event = ResendFireEvents[0];
        ResendFireEvents.RemoveAt(0);

        // SEND THE PACKET
        // NOTE: calling this Server function from the Client ONLY sends a packet.
        // It does NOT execute the fire logic locally again.
        if (Event.bIsStartFire)
        {
            ResendServerStartFireFixed(Event.FireModeNum, Event.FireEventIndex,
                Event.ClientTimestamp, Event.ClientViewRot, Event.HitChar.Get(),
                Event.ZOffset, Event.ClientHeadOffset);
        }
        else
        {
            ResendServerStopFireFixed(Event.FireModeNum, Event.FireEventIndex);
        }
    }

    // If we have drained the queue, stop the timer to save CPU
    if (ResendFireEvents.Num() == 0)
    {
        GetWorldTimerManager().ClearTimer(ResendFireHandle);
    }
}

// 3. CLEANUP (Client Side)
// Call this in DetachFromOwner or PutDown
void AUTWeaponFix::ClearFireEventsFixed()
{
    ResendFireEvents.Empty();
    GetWorldTimerManager().ClearTimer(ResendFireHandle);
}


/*
// 4. CONFIRMATION (Client Side)
// When server ACKs a shot, remove it from the retry queue so we stop bothering the server
void AUTWeaponFix::ClientConfirmFireEvent_Implementation(uint8 FireModeNum, int32 InAuthorizedEventIndex)
{
    if (ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        // Capture expected index BEFORE overwriting
        int32 ExpectedIndex = ClientFireEventIndex[FireModeNum];

        // Update to server's authoritative value
        ClientFireEventIndex[FireModeNum] = InAuthorizedEventIndex;

        // If server sent back an index LESS than what we sent, shots were rejected
        if (InAuthorizedEventIndex < ExpectedIndex)
        {
            // Find and destroy ALL fake projectiles with rejected indices
            for (int32 i = PendingFakeProjectiles.Num() - 1; i >= 0; i--)
            {
                FPendingFakeProjectile& Pending = PendingFakeProjectiles[i];

                // If this projectile's event index is GREATER than what server accepted,
                // it was rejected
                if (Pending.FireMode == FireModeNum && Pending.EventIndex > InAuthorizedEventIndex)
                {
                    if (Pending.Projectile.IsValid())
                    {
                        UE_LOG(LogUTWeaponFix, Verbose,
                            TEXT("Destroying rejected fake projectile (Event %d > Server accepted %d)"),
                            Pending.EventIndex, InAuthorizedEventIndex);
                        Pending.Projectile->Destroy();
                    }
                    PendingFakeProjectiles.RemoveAt(i);
                }
            }
        }
        else
        {
            // Shot was accepted - remove from pending list (let projectile live)
            for (int32 i = PendingFakeProjectiles.Num() - 1; i >= 0; i--)
            {
                FPendingFakeProjectile& Pending = PendingFakeProjectiles[i];

                if (Pending.FireMode == FireModeNum && Pending.EventIndex <= InAuthorizedEventIndex)
                {
                    // This projectile was accepted, stop tracking it
                    PendingFakeProjectiles.RemoveAt(i);
                }
            }
        }
    }

    // Clear retries for confirmed events (unchanged)
    for (int32 i = ResendFireEvents.Num() - 1; i >= 0; i--)
    {
        if (ResendFireEvents[i].FireModeNum == FireModeNum &&
            ResendFireEvents[i].FireEventIndex <= InAuthorizedEventIndex)
        {
            ResendFireEvents.RemoveAt(i);
        }
    }

    if (ResendFireEvents.Num() == 0)
    {
        GetWorldTimerManager().ClearTimer(ResendFireHandle);
    }
}
*/



void AUTWeaponFix::ClientConfirmFireEvent_Implementation(uint8 FireModeNum, int32 InAuthorizedEventIndex)
{
    // FIX 1: Do NOT rollback the local sequence generator.
    // Only update if server is AHEAD (rare resync case).
    if (ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        if (InAuthorizedEventIndex > ClientFireEventIndex[FireModeNum])
        {
            ClientFireEventIndex[FireModeNum] = InAuthorizedEventIndex;
        }
    }

    // FIX 2: Destroy CONFIRMED fakes only (server spawned the real one).
    // Do NOT touch fakes with index > authorized - those are still in-flight, not rejected.
    for (int32 i = PendingFakeProjectiles.Num() - 1; i >= 0; i--)
    {
        FPendingFakeProjectile& Pending = PendingFakeProjectiles[i];

        if (Pending.FireMode == FireModeNum && Pending.EventIndex <= InAuthorizedEventIndex)
        {
            // Confirmed — server spawned the real projectile.
            // For shock balls: DON'T destroy the fake. BeginFakeProjectileSynch has
            // already paired it with the auth. The fake renders smoothly while the
            // real is hidden. Destroying the fake would cause a visual hitch at 80+ ping
            // as the real un-hides at its forward-ticked position. The fake is cleaned
            // up naturally when the real explodes/expires.
            // For all other projectiles: destroy the fake as before.
            if (Pending.Projectile.IsValid())
            {
                if (!Cast<AUTPlusProj_ShockBall>(Pending.Projectile.Get())
                    && !Cast<AUTPlusProj_Rocket>(Pending.Projectile.Get()))
                {
                    Pending.Projectile->Destroy();
                }
            }
            PendingFakeProjectiles.RemoveAt(i);
        }
        // Fakes with EventIndex > InAuthorizedEventIndex: LEAVE ALONE
        // They're not rejected, just not processed yet
    }

    // Clear confirmed retries from queue
    for (int32 i = ResendFireEvents.Num() - 1; i >= 0; i--)
    {
        if (ResendFireEvents[i].FireModeNum == FireModeNum &&
            ResendFireEvents[i].FireEventIndex <= InAuthorizedEventIndex)
        {
            ResendFireEvents.RemoveAt(i);
        }
    }

    if (ResendFireEvents.Num() == 0)
    {
        GetWorldTimerManager().ClearTimer(ResendFireHandle);
    }
}




void AUTWeaponFix::ClearPendingFakeProjectiles()
{
    // Don't destroy - just stop tracking. 
    // Valid projectiles should live, rejected ones will have been destroyed already.
    PendingFakeProjectiles.Empty();
}


// 5. SERVER HANDLER (Start Fire)
// This receives the retry packet
void AUTWeaponFix::ResendServerStartFireFixed_Implementation(uint8 FireModeNum,
    int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot,
    AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset)
{
    // DUPLICATE CHECK
    // If the server already processed this index (or a newer one), ignore this packet.
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        int32 LastIdx = AuthoritativeFireEventIndex[FireModeNum];

        // Wrap-around safe check: if the index is <= last seen, it's old.
        if (InFireEventIndex <= LastIdx && (LastIdx - InFireEventIndex) < 100)
        {
            return; // SILENT REJECT - Already fired this shot
        }
    }

    // Set flag so internal logic knows this is a delayed/retry shot
    bNetDelayedShot = true;

    // Execute the same implementation with the same logical payload. The retry-only
    // context still lets projectile spawning compensate for network delay.
    ServerStartFireFixed_Implementation(FireModeNum, InFireEventIndex, ClientTimestamp,
        ClientViewRot, ClientHitChar, ZOffset, ClientHeadOffset);

    bNetDelayedShot = false;
}

// 6. SERVER HANDLER (Stop Fire)
void AUTWeaponFix::ResendServerStopFireFixed_Implementation(uint8 FireModeNum,
    int32 InFireEventIndex)
{
    bNetDelayedShot = true;
    ServerStopFireFixed_Implementation(FireModeNum, InFireEventIndex);
    bNetDelayedShot = false;
}

bool AUTWeaponFix::ResendServerStartFireFixed_Validate(uint8 FireModeNum,
    int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot,
    AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset)
{
    return ValidateStartFireFixedPayload(FireModeNum, InFireEventIndex, ClientTimestamp,
        ClientViewRot, ClientHeadOffset);
}

bool AUTWeaponFix::ResendServerStopFireFixed_Validate(uint8 FireModeNum,
    int32 InFireEventIndex)
{
    return FireModeNum < GetNumFireModes() && InFireEventIndex >= 0;
}


// =========================================================================
// PROJECTILE REWIND LAG COMPENSATION
// =========================================================================

void AUTWeaponFix::NotifyFakeProjectileHit(AUTCharacter* HitTarget, const FVector& HitLocation, uint8 FireModeNum)
{
	// During replay playback, skip all rewind/prediction logic
	UWorld* W = GetWorld();
	if (W && W->DemoNetDriver && W->DemoNetDriver->IsPlaying())
	{
		return;
	}

	// Client-side hitsound prediction for projectile weapons
	if (HitTarget != nullptr && Role != ROLE_Authority)
	{
		AClientHitsounds* HitsoundsMut = FindClientHitsoundsMutator();
		if (HitsoundsMut)
		{
			int32 EstDamage = 0;
			if (ProjClass.IsValidIndex(FireModeNum) && ProjClass[FireModeNum])
			{
				AUTProjectile* DefProj = ProjClass[FireModeNum]->GetDefaultObject<AUTProjectile>();
				if (DefProj) EstDamage = DefProj->DamageParams.BaseDamage;
			}
			HitsoundsMut->PlayClientPredictedHitsound(EstDamage);
		}
	}

	if (!bEnableProjectileRewind || !HitTarget)
	{
		return;
	}

	// Send the claim with FireMode only — server matches against ActiveServerProjectiles
	// by fire mode (oldest first). No EventIndex needed from the client since we're
	// using the replicated real projectile, not the fake (which is already destroyed).
	ServerProjectileHitClaim(HitTarget, HitLocation, FireModeNum);
}

void AUTWeaponFix::OnTrackedProjectileResolved(AUTProjectile* Proj, AUTCharacter* DamagedChar)
{
	// Server-only. Snapshot a tracked projectile's final state at the moment it resolves
	// (explodes), BEFORE the engine destroys it, so the grace buffer can rewind-rescue a
	// claim that arrives after the projectile is gone. Capturing here (vs a per-tick poll)
	// gives the exact explosion position/velocity AND what it actually hit (for the
	// double-damage guard).
	if (Role != ROLE_Authority || !Proj)
	{
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	for (FActiveServerProjectile& E : ActiveServerProjectiles)
	{
		if (E.Projectile.Get() != Proj)
		{
			continue;
		}
		E.FinalLoc = Proj->GetActorLocation();
		E.FinalVel = Proj->GetVelocity();
		E.FinalGravityZ = Proj->ProjectileMovement ? Proj->ProjectileMovement->GetGravityZ() : 0.f;
		E.BaseDamage = Proj->DamageParams.BaseDamage;
		E.Momentum = Proj->Momentum;
		E.DamageType = Proj->MyDamageType;
		float R = 0.f;
		if (Proj->CollisionComp) { R = Proj->CollisionComp->GetScaledSphereRadius(); }
		if (R <= 0.f && Proj->PawnOverlapSphere) { R = Proj->PawnOverlapSphere->GetScaledSphereRadius(); }
		E.HitRadius = (R > 0.f) ? R : 10.f;
		E.ExpireTime = Now;
		E.DamagedTarget = DamagedChar;
		break;
	}
}

bool AUTWeaponFix::ServerProjectileHitClaim_Validate(AUTCharacter* ClaimedTarget,
	FVector ClaimedHitLocation, uint8 ClaimedFireMode)
{
	return true;
}

void AUTWeaponFix::ServerProjectileHitClaim_Implementation(AUTCharacter* ClaimedTarget,
	FVector ClaimedHitLocation, uint8 ClaimedFireMode)
{
	// Master gates: per-weapon feature flag (also gates the client send) AND server kill-switch.
	if (!bEnableProjectileRewind || CVarRocketLagComp.GetValueOnGameThread() == 0)
	{
		return;
	}

	// 1. Validate target
	if (!ClaimedTarget || ClaimedTarget->IsDead())
	{
		return;
	}

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS && GS->OnSameTeam(UTOwner, ClaimedTarget))
	{
		return;
	}

	// 2. Validate shooter, anti-abuse ping cutoff, and the rewind WINDOW (seconds).
	// The window is the lookback into the target's history needed to reach the silhouette
	// the shooter shot at. ClaimedHitLocation IS that silhouette, so the lookback is the
	// shooter's FULL round-trip (snapshot age) - NOT half-RTT, and NOT the target's ping
	// (the target's own lag is already baked into its recorded positions). The window cap
	// bounds 'shot behind cover' and naturally degrades comp once RTT exceeds it.
	if (!UTOwner || !UTOwner->PlayerState)
	{
		return;
	}
	const float PingMs = UTOwner->PlayerState->ExactPing;

	// DIAGNOSTIC: log every claim that reaches here (passed target/team validation), with the
	// shooter ping and how many projectiles are currently tracked. Tells us whether claims are
	// even arriving for high-ping shooters, and whether their rocket got tracked at all.
	const int32 TrackedAtClaim = ActiveServerProjectiles.Num();
	if (RocketLagCompDbg())
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("ProjRewind CLAIM: tgt=%s fm=%d ping=%.0f tracked=%d"),
			*ClaimedTarget->GetName(), (int32)ClaimedFireMode, PingMs, TrackedAtClaim);
	}

	if (PingMs > CVarRocketLagCompMaxPingMs.GetValueOnGameThread())
	{
		// DIAGNOSTIC: previously a silent return — now logged so over-cutoff shooters (e.g. Kuj
		// at ~143) show up in the log instead of vanishing.
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind REJECTED: shooter over ping cutoff (ping=%.0f > %.0f)"),
				PingMs, CVarRocketLagCompMaxPingMs.GetValueOnGameThread());
		}
		return; // shooter too laggy for projectile lag comp
	}
	const float MaxWindowMs = CVarRocketLagCompMaxWindowMs.GetValueOnGameThread();
	float WindowSec = (PingMs * 0.001f) * 1.1f; // ~full RTT + slack
	WindowSec = FMath::Clamp(WindowSec, 0.016f, MaxWindowMs * 0.001f);

	// 3. Find the real (authoritative) projectile
	// Match by FireMode, oldest first (FIFO).
	// Prefer a LIVE projectile; if none, fall back to the GRACE BUFFER — a matching projectile
	// that resolved (exploded) within ut.RocketLagCompGraceMs, for the close-range timing race
	// where the server projectile detonated before this ~RTT-late claim arrived.
	const float NowSec = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const float GraceSec = FMath::Max(0.f, CVarRocketLagCompGraceMs.GetValueOnGameThread() * 0.001f);

	AUTProjectile* RealProjectile = nullptr;
	int32 FoundIndex = -1;
	int32 GraceIndex = -1;   // fallback: a matching resolved projectile still within the grace window

	// DIAGNOSTIC counters for the no-op path — distinguish "claim too late for the grace window"
	// (raise grace) from "no matching projectile tracked / resolve hook never fired" (fix the trigger).
	// Filled in as the loop prunes out-of-grace entries below.
	int32 DiagFmDroppedTooOld   = 0;    // fm-matching resolved entries past the grace window
	int32 DiagFmInvalidNoExpire = 0;    // fm-matching entries invalidated WITHOUT a resolve snapshot
	float DiagNewestDroppedAgeMs = -1.f;// age of the fm-match that MOST NEARLY fit (smallest age > grace)

	for (int32 i = 0; i < ActiveServerProjectiles.Num(); i++)
	{
		FActiveServerProjectile& Entry = ActiveServerProjectiles[i];
		const bool bLive = Entry.Projectile.IsValid()
			&& !Entry.Projectile.Get()->bExploded
			&& !Entry.Projectile.Get()->IsPendingKillPending();

		if (!bLive)
		{
			// Retain a RESOLVED entry only while it's inside the grace window AND grace is on;
			// otherwise drop it. A resolved entry has ExpireTime >= 0 (set by
			// OnTrackedProjectileResolved); a never-resolved-but-now-invalid entry (ExpireTime < 0)
			// is dropped immediately as before.
			const bool bWithinGrace = (GraceSec > 0.f) && (Entry.ExpireTime >= 0.f)
				&& ((NowSec - Entry.ExpireTime) <= GraceSec);
			if (!bWithinGrace)
			{
				// DIAGNOSTIC: record fm-matching entries we're about to drop, to explain a later no-op.
				if (Entry.FireMode == ClaimedFireMode)
				{
					if (Entry.ExpireTime >= 0.f)
					{
						const float AgeMs = (NowSec - Entry.ExpireTime) * 1000.f;
						DiagFmDroppedTooOld++;
						if (DiagNewestDroppedAgeMs < 0.f || AgeMs < DiagNewestDroppedAgeMs)
						{
							DiagNewestDroppedAgeMs = AgeMs; // the one that most nearly fit the grace window
						}
					}
					else
					{
						DiagFmInvalidNoExpire++; // invalidated without OnTrackedProjectileResolved firing
					}
				}
				ActiveServerProjectiles.RemoveAt(i);
				i--;
				continue;
			}
			// Eligible grace fallback if it matches; remember the first (oldest) one.
			if (GraceIndex == -1 && Entry.FireMode == ClaimedFireMode)
			{
				GraceIndex = i;
			}
			continue;
		}

		if (Entry.FireMode != ClaimedFireMode)
		{
			continue;
		}
		RealProjectile = Entry.Projectile.Get();
		FoundIndex = i;
		break; // Oldest first (array is insertion-ordered)
	}

	// Grace-buffer fallback when no LIVE projectile matched (the close-range timing race).
	bool bFromGrace = false;
	FVector GraceFinalLoc = FVector::ZeroVector;
	FVector GraceFinalVel = FVector::ZeroVector;
	float GraceFinalGravityZ = 0.f;
	float GraceHitRadius = 10.f;
	float GraceExpireTime = 0.f;
	float GraceBaseDamage = 0.f;
	float GraceMomentum = 0.f;
	TSubclassOf<UDamageType> GraceDamageType = nullptr;
	if (!RealProjectile && GraceIndex != -1)
	{
		FActiveServerProjectile& E = ActiveServerProjectiles[GraceIndex];
		// Double-damage guard: if this projectile already directly hit the CLAIMED target
		// present-time, the damage was applied by its natural collision — do NOT rescue.
		if (E.DamagedTarget.Get() == ClaimedTarget)
		{
			return;
		}
		bFromGrace = true;
		FoundIndex = GraceIndex;
		GraceFinalLoc = E.FinalLoc;
		GraceFinalVel = E.FinalVel;
		GraceFinalGravityZ = E.FinalGravityZ;
		GraceHitRadius = E.HitRadius;
		GraceExpireTime = E.ExpireTime;
		GraceBaseDamage = E.BaseDamage;
		GraceMomentum = E.Momentum;
		GraceDamageType = E.DamageType;
	}

	if (!RealProjectile && !bFromGrace)
	{
		// No live projectile AND nothing rescuable in the grace buffer. The server rocket either
		// hit the target present-time and applied damage (normal), or detonated/whiffed and its
		// grace window already expired (claim arrived too late, or grace disabled). Don't re-apply.
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind no-op: no live/grace proj (fm=%d ping=%.0f tracked=%d fmDroppedTooOld=%d newestAge=%.0fms fmInvalidNoResolve=%d grace=%.0fms) — present-time hit OR claim past grace"),
				(int32)ClaimedFireMode, PingMs, TrackedAtClaim, DiagFmDroppedTooOld, DiagNewestDroppedAgeMs, DiagFmInvalidNoExpire, GraceSec * 1000.f);
		}
		return;
	}

	// 4. Capsule dims for the rewound target.
	const float CapRadius = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float CapHeight = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float SegHalf = FMath::Max(0.f, CapHeight - CapRadius);

	// 5. ClaimedHitLocation is a SEARCH ANCHOR only, never the damage origin (using it as the
	// origin would let a modified client convert a near-miss into a center-mass direct hit).
	// Walk the target's history across the window; find the instant its capsule passed
	// closest to the claimed point.
	float BestDelta = 0.f;
	float BestDistSq = BIG_NUMBER;
	FVector BestCenter = ClaimedTarget->GetActorLocation();
	const float StepSec = 1.f / 240.f;
	for (float Delta = 0.f; Delta <= WindowSec + KINDA_SMALL_NUMBER; Delta += StepSec)
	{
		const FVector Center = ClaimedTarget->GetRewindLocation(Delta);
		const FVector OnSeg = FMath::ClosestPointOnSegment(ClaimedHitLocation,
			Center - FVector(0.f, 0.f, SegHalf), Center + FVector(0.f, 0.f, SegHalf));
		const float DistSq = FVector::DistSquared(ClaimedHitLocation, OnSeg);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestDelta = Delta;
			BestCenter = Center;
		}
	}

	// Anti-fabrication #1: the target must actually have occupied the claimed point.
	const float ClaimMatchTol = CapRadius + 25.f;
	if (BestDistSq > ClaimMatchTol * ClaimMatchTol)
	{
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind REJECTED: target not at claim (dist=%.1f ping=%.0f win=%.0fms)"),
				FMath::Sqrt(BestDistSq), PingMs, WindowSec * 1000.f);
		}
		return;
	}

	// 6. Reconstruct where the projectile was at the rewind instant, analytically under constant
	// gravity (rocket g=0, flak shell g<0); no history buffer needed.
	//   LIVE:  walk back from current state:  pos(t-d) = pos - vel*d + 0.5*g*d^2
	//   GRACE: the projectile resolved at GraceExpireTime at GraceFinalLoc; the rewind instant
	//          (NowSec - BestDelta) is BEFORE the explosion, so walk back from the explosion by
	//          BackDt = ExpireTime - (Now - BestDelta).
	FVector ProjPast;
	if (bFromGrace)
	{
		const float BackDt = FMath::Max(0.f, GraceExpireTime - (NowSec - BestDelta));
		ProjPast = GraceFinalLoc - (GraceFinalVel * BackDt) + FVector(0.f, 0.f, 0.5f * GraceFinalGravityZ * BackDt * BackDt);
	}
	else
	{
		const FVector ProjLoc = RealProjectile->GetActorLocation();
		const FVector ProjVel = RealProjectile->GetVelocity();
		const float GravZ = RealProjectile->ProjectileMovement ? RealProjectile->ProjectileMovement->GetGravityZ() : 0.f;
		ProjPast = ProjLoc - (ProjVel * BestDelta) + FVector(0.f, 0.f, 0.5f * GravZ * BestDelta * BestDelta);
	}

	// 7. Server-authoritative contact test. THIS owns the hit decision, not the client.
	float ProjHitRadius = 0.f;
	if (bFromGrace)
	{
		ProjHitRadius = GraceHitRadius;   // captured at resolution (real projectile is gone)
	}
	else if (RealProjectile->CollisionComp)
	{
		ProjHitRadius = RealProjectile->CollisionComp->GetScaledSphereRadius();
	}
	if (ProjHitRadius <= 0.f && !bFromGrace && RealProjectile->PawnOverlapSphere)
	{
		ProjHitRadius = RealProjectile->PawnOverlapSphere->GetScaledSphereRadius();
	}
	if (ProjHitRadius <= 0.f)
	{
		ProjHitRadius = 10.f;
	}

	const FVector SegTop = BestCenter + FVector(0.f, 0.f, SegHalf);
	const FVector SegBot = BestCenter - FVector(0.f, 0.f, SegHalf);
	const FVector OnCap = FMath::ClosestPointOnSegment(ProjPast, SegBot, SegTop);
	const float ContactDistSq = FVector::DistSquared(ProjPast, OnCap);
	const float ContactRadius = CapRadius + ProjHitRadius;

	if (ContactDistSq > ContactRadius * ContactRadius)
	{
		// Real projectile did NOT pass within the capsule at that instant: not a confirmable
		// direct hit. v1 declines (present-time already handled any true contact).
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind REJECTED: no server contact (dist=%.1f need=%.1f ping=%.0f win=%.0fms delta=%.0fms)"),
				FMath::Sqrt(ContactDistSq), ContactRadius, PingMs, WindowSec * 1000.f, BestDelta * 1000.f);
		}
		return;
	}

	// Anti-fabrication #2: the claimed point must also lie on the real projectile path.
	if (FVector::DistSquared(ProjPast, ClaimedHitLocation) > FMath::Square(ContactRadius + ClaimMatchTol))
	{
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning, TEXT("ProjRewind REJECTED: claim off projectile path"));
		}
		return;
	}

	// 8. LOS: never award a hit through geometry.
	FCollisionQueryParams WallParams(TEXT("ProjRewindWallCheck"), true, RealProjectile);
	WallParams.AddIgnoredActor(ClaimedTarget);
	WallParams.AddIgnoredActor(UTOwner);
	if (GetWorld()->LineTraceTestByChannel(ProjPast, BestCenter, COLLISION_TRACE_WEAPON, WallParams))
	{
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning, TEXT("ProjRewind REJECTED: wall between projectile and target"));
		}
		return;
	}

	// 9. Confirmed direct hit at the rewound contact point.
	const FVector HitNormal = (ProjPast - OnCap).GetSafeNormal();
	// targetMoved = how far the target's authoritative capsule advanced past where the shooter
	// hit it == roughly how badly the un-compensated server test would have missed (> capsule
	// radius ~46u means this hit ONLY landed because of lag comp).
	const float TargetPingMs = ClaimedTarget->PlayerState ? ClaimedTarget->PlayerState->ExactPing : -1.f;
	const float TargetMoved = (ClaimedTarget->GetActorLocation() - BestCenter).Size();

	if (bFromGrace)
	{
		// Real projectile already exploded (close-range timing race). Apply its DIRECT-hit damage
		// ourselves — mirrors AUTProjectile::DamageImpactedActor's radial branch with MinimumDamage
		// forced to full (a direct hit deals full damage regardless of radial falloff). The
		// double-damage guard already ensured this projectile did NOT hit ClaimedTarget present-time,
		// and same-team was rejected earlier — so this is a clean rescue, not a re-application.
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind GRACE SAVE: tgt=%s fm=%d shooterPing=%.0f targetPing=%.0f win=%.0fms rewind=%.0fms graceAge=%.0fms contact=%.1f targetMoved=%.1f dmg=%.0f"),
				*ClaimedTarget->GetName(), (int32)ClaimedFireMode, PingMs, TargetPingMs,
				WindowSec * 1000.f, BestDelta * 1000.f, (NowSec - GraceExpireTime) * 1000.f,
				FMath::Sqrt(ContactDistSq), TargetMoved, GraceBaseDamage);
		}

		FUTRadialDamageEvent DmgEvent;
		DmgEvent.BaseMomentumMag = GraceMomentum;
		DmgEvent.Params = FRadialDamageParams(GraceBaseDamage, 1.0f);
		DmgEvent.Params.MinimumDamage = GraceBaseDamage; // force full damage for a direct hit
		DmgEvent.DamageTypeClass = GraceDamageType ? GraceDamageType : TSubclassOf<UDamageType>(UDamageType::StaticClass());
		DmgEvent.Origin = OnCap;
		new(DmgEvent.ComponentHits) FHitResult(ClaimedTarget, ClaimedTarget->GetCapsuleComponent(), OnCap, HitNormal);
		DmgEvent.ComponentHits[0].TraceStart = OnCap - GraceFinalVel;
		DmgEvent.ComponentHits[0].TraceEnd = OnCap + GraceFinalVel;
		DmgEvent.ShotDirection = GraceFinalVel.GetSafeNormal();
		AController* InstC = UTOwner ? UTOwner->GetController() : nullptr;
		ClaimedTarget->TakeDamage(GraceBaseDamage, DmgEvent, InstC, this);
	}
	else if (!RealProjectile->bExploded)
	{
		// Live projectile still in flight: reuse stock damage semantics — ProcessHit ->
		// DamageImpactedActor + Explode (incl. direct/splash dedup), consuming the projectile
		// (bExploded) so the present-time collision cannot also fire.
		if (RocketLagCompDbg())
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("ProjRewind SAVE: tgt=%s fm=%d shooterPing=%.0f targetPing=%.0f win=%.0fms rewind=%.0fms contact=%.1f targetMoved=%.1f"),
				*ClaimedTarget->GetName(), (int32)ClaimedFireMode, PingMs, TargetPingMs,
				WindowSec * 1000.f, BestDelta * 1000.f, FMath::Sqrt(ContactDistSq), TargetMoved);
		}

		RealProjectile->ProcessHit(ClaimedTarget, ClaimedTarget->GetCapsuleComponent(), OnCap, HitNormal);
	}

	// 10. Consume the tracking entry.
	if (FoundIndex >= 0 && FoundIndex < ActiveServerProjectiles.Num())
	{
		ActiveServerProjectiles.RemoveAt(FoundIndex);
	}
}

// =========================================================================
// FIRING STATE GUARD — prevent crash when fire RPC arrives after owner death
// =========================================================================

void AUTWeaponFix::ServerUpdateFiringStates_Implementation(uint8 FireSettings)
{
	// Guard: if owner is dead/destroyed, discard the RPC.
	// Race condition: player dies, weapon is being torn down, but a replicated
	// ServerUpdateFiringStates was already in flight and arrives this frame.
	// Base class dereferences UTOwner without null check → access violation.
	if (!GetUTOwner() || GetUTOwner()->IsDead() || IsPendingKillPending())
	{
		return;
	}
	Super::ServerUpdateFiringStates_Implementation(FireSettings);
}

// =========================================================================
// CLIENT-SIDE HITSOUND PREDICTION HELPER
// =========================================================================

AClientHitsounds* AUTWeaponFix::FindClientHitsoundsMutator()
{
	// Return cached pointer if still valid
	if (CachedClientHitsounds.IsValid())
	{
		return CachedClientHitsounds.Get();
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Search via TActorIterator (works on both client and server)
	for (TActorIterator<AClientHitsounds> It(World); It; ++It)
	{
		CachedClientHitsounds = *It;
		return *It;
	}

	return nullptr;
}
