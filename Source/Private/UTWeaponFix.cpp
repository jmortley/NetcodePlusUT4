
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
#include "UTDamageType.h"   // FUTRadialDamageEvent (grace-buffer direct-hit damage)
#include "UTPlusWeap_RocketLauncher.h"
#include "UTWeaponSkin.h"
#include "UObject/UObjectIterator.h"
#include "ClientHitsounds.h"
#include "EngineUtils.h"
#include "UTGameMode.h"
#include "UTCTFBaseGame.h"
#include "UTPlayerState.h"
#include "ElimPlusGame.h"
#include "WipeoutGame.h"
#include "MutBotEvents.h"
#include "NCFireValCollector.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"


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
static TAutoConsoleVariable<int32> CVarGhostFix(
    TEXT("ncp.GhostFix"), 0,
    TEXT("Ghost-rocket-on-weapon-switch fix: 1=carry real held-fire across a switch, 0=legacy. KNOWN ISSUE (live-confirmed 2026-07-05): 1 BREAKS consecutive held weapon switches (per-weapon held flag never arms on auto-fired weapons) — DO NOT ENABLE until the pawn-level v2. Off by default."),
    ECVF_Default);

static FORCEINLINE bool GhostFix()
{
    return CVarGhostFix.GetValueOnGameThread() > 0;
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
bool AUTWeaponFix::bWeaponSettingsLoaded = false;
// Stomach-height defaults; LoadWeaponSettings overrides from Mod.ini.
float AUTWeaponFix::HiddenBeamBackOffset = 10.f;
float AUTWeaponFix::HiddenBeamDownOffset = 35.f;

static const TCHAR* WEAPON_SETTINGS_SECTION = TEXT("NetcodePlus.WeaponSettings");

void AUTWeaponFix::LoadWeaponSettings()
{
	if (bWeaponSettingsLoaded) return;
	bWeaponSettingsLoaded = true;

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

	// Load hide settings — read keys by class name
	// We iterate weapon classes to get all possible class names, then check Mod.ini for each
	TSet<FName> SeenSkinTags;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(AUTWeapon::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			FName HideKey = FName(*It->GetName());
			if (!HiddenWeaponsByTag.Contains(HideKey))
			{
				FString Key = FString::Printf(TEXT("Hide.%s"), *HideKey.ToString());
				FString Value;
				if (GConfig->GetString(WEAPON_SETTINGS_SECTION, *Key, Value, ModIniPath))
				{
					HiddenWeaponsByTag.Add(HideKey, Value == TEXT("1"));
				}
			}

			// Load saved skin paths — keyed by WeaponSkinCustomizationTag.
			// Disabled via bSkinsEnabled gate (see UTWeaponFix.h): LoadObject sync
			// load hitches the main thread, and MIDs aren't being applied anyway.
			if (bSkinsEnabled)
			{
				AUTWeapon* WeaponCDO = Cast<AUTWeapon>(It->GetDefaultObject());
				if (WeaponCDO && WeaponCDO->WeaponSkinCustomizationTag != NAME_None)
				{
					FName Tag = WeaponCDO->WeaponSkinCustomizationTag;
					if (!SeenSkinTags.Contains(Tag))
					{
						SeenSkinTags.Add(Tag);
						FString SkinKey = FString::Printf(TEXT("Skin.%s"), *Tag.ToString());
						FString SkinPath;
						if (GConfig->GetString(WEAPON_SETTINGS_SECTION, *SkinKey, SkinPath, ModIniPath) && !SkinPath.IsEmpty())
						{
							SavedSkinPaths.Add(Tag, SkinPath);
							UUTWeaponSkin* Skin = LoadObject<UUTWeaponSkin>(nullptr, *SkinPath);
							if (Skin)
							{
								Skin->AddToRoot();
								CachedSkinAssets.Add(Tag, Skin);
							}
						}
					}
				}
			}
		}
	}
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

	GConfig->Flush(false, ModIniPath);
}

AUTWeaponFix::AUTWeaponFix(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Initialize arrays for standard two fire modes
    AuthoritativeFireEventIndex.SetNum(2);
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

    for (int32 i = 0; i < 2; i++)
    {
        AuthoritativeFireEventIndex[i] = 0;
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

    // Fire-validation telemetry gate — decided server-side where the gamemode is
    // unambiguous, then replicated to the owning client. Two conditions, both
    // required:
    //   1. Mode: Elim, instagib-CTF, or Wipeout (regular CTF / Duel / ShockDom off).
    //   2. Weapon: this instance is a UTPlusSniper or UTPlusShockRifle (or child).
    //      Covers instagib rifle + shock rifle (shock children) and sniper + LG
    //      (LG is a sniper reskin). Excludes minigun/enforcer — also hitscan, but
    //      not precision, and would otherwise feed false low-dwell samples.
    if (Role == ROLE_Authority)
    {
        AUTGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AUTGameMode>() : nullptr;
        const bool bElim = GM && GM->IsA(AElimPlusGame::StaticClass());
        const bool bICTF = GM && GM->bIsInstagib && GM->IsA(AUTCTFBaseGame::StaticClass());
        const bool bWipeout = GM && GM->IsA(AUWipeoutGame::StaticClass());
        const bool bFireValWeapon = IsA(AUTPlusSniper::StaticClass()) || IsA(AUTPlusShockRifle::StaticClass());
        // 3. Server master switch: Mod.ini [NetcodePlus] EnableFireVal (default OFF). Gated
        //    here server-side, so when it's off bFireValActive stays false and the
        //    owner-only replicated flag never tells any client to start the tracker.
        const bool bEnabled = FNCFireValCollector::IsEnabled();
        bFireValActive = bEnabled && (bElim || bICTF || bWipeout) && bFireValWeapon;

        // One-time diagnostic for the relevant weapons, so a "no samples" result is never
        // a mystery again: it shows whether the gate armed and which condition failed.
        // (No line at all while holding a sniper/shock => that weapon isn't a UTPlus class.)
        // Log once per process (not per weapon spawn) to confirm the gate without spam.
        static bool bLoggedFireValGate = false;
        if (bFireValWeapon && !bLoggedFireValGate)
        {
            bLoggedFireValGate = true;
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireVal] gate: weapon=%s enabled=%d elim=%d ictf=%d wipeout=%d -> active=%d"),
                *GetClass()->GetName(), bEnabled ? 1 : 0, bElim ? 1 : 0, bICTF ? 1 : 0, bWipeout ? 1 : 0, bFireValActive ? 1 : 0);
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
    // Low-debounce mice and scroll-wheel-bound fire actions generate rapid
    // release+press event pairs that the engine surfaces as separate clicks.
    // Without this guard, every spurious bounce reaches the cooldown gate and
    // either gets absorbed into the retry queue (fine) or — under specific
    // race conditions — eats the user's held intent (broken). The 25ms default
    // floor is well below human physiological double-click cadence (~50-80ms
    // minimum), so this can only catch bounces, not intentional rapid fire.
    //
    // Coalesce, do not block: keep PendingFire=true so any held intent is
    // preserved and refire continues normally. The bounce simply doesn't
    // generate a new fire event.
    if (UTOwner && UTOwner->IsLocallyControlled() &&
        MouseDebounceWindow > 0.f &&
        LastReleaseTime.IsValidIndex(FireModeNum) &&
        LastReleaseTime[FireModeNum] > 0.0f)
    {
        const float SinceRelease = GetWorld()->GetTimeSeconds() - LastReleaseTime[FireModeNum];
        if (SinceRelease >= 0.f && SinceRelease < MouseDebounceWindow)
        {
            // Verbose log so testers can confirm the debounce is engaging
            // when investigating low-debounce-mouse complaints. Off by default.
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[NCFire.Debounce] mode=%d sinceRelease=%.4f window=%.4f"),
                FireModeNum, SinceRelease, MouseDebounceWindow);
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

		ServerStartFireFixed(CurrentFireMode, NextEventIndex, GetWorld()->GetGameState()->GetServerWorldTimeSeconds(), false, ClientRot, ClientHitChar, ZOffset, ClientHeadOffset);
        QueueResendFireFixed(true, CurrentFireMode, NextEventIndex, GetWorld()->GetGameState()->GetServerWorldTimeSeconds(), ClientRot, ZOffset, ClientHitChar);

        // Telemetry sidecar — separate UNRELIABLE RPC, never folded into the fire
        // path above. Sample every shot taken WHILE THE CROSSHAIR IS ON A VISIBLE
        // ENEMY — hits AND on-target misses (a capsule-edge whiff is real signal) —
        // but NOT off-target spam, which would flood the data with meaningless
        // dwell=0 and inflate everyone's low-dwell share. FireValAcquireTime>=0 means
        // the per-frame tracker had an enemy as of last tick; the ClientHitChar
        // fallback catches a fresh flick whose hit lands the same frame the tick
        // hasn't run yet (dwell ~0). bClaimedHit lets the server split hit vs miss.
        if (bFireValActive)
        {
            bool bHitEnemy = false;
            if (ClientHitChar != nullptr && UTOwner && !ClientHitChar->IsDead())
            {
                const uint8 MyTeam = UTOwner->GetTeamNum();
                const uint8 ThTeam = ClientHitChar->GetTeamNum();
                bHitEnemy = (MyTeam == 255 || ThTeam == 255 || MyTeam != ThTeam);
            }
            if (FireValAcquireTime >= 0.0f || bHitEnemy)
            {
                // Fresh-flick fallback (acquire<0 but the muzzle trace hit an enemy:
                // Tick hadn't registered the target yet this frame): floor dwell to
                // ONE frame, not 0 — a literal 0 ms implies impossible negative
                // reaction and would inflate the zero bucket for fast legit flickers.
                const float DwellSec = (FireValAcquireTime >= 0.0f && GetWorld())
                    ? FMath::Max(0.0f, GetWorld()->GetTimeSeconds() - FireValAcquireTime)
                    : FireValFrameTimeEMA;
                const int32 DwellMs = FMath::Clamp(FMath::RoundToInt(DwellSec * 1000.0f), 0, 60000);
                const uint8 FrameMs = (uint8)FMath::Clamp(FMath::RoundToInt(FireValFrameTimeEMA * 1000.0f), 0, 255);
                ServerReportFireValidation(DwellMs, FrameMs, bHitEnemy);
            }
        }

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
/*
    if (bIsChargedMode)
    {
        // Don't log for Mode 0 stops (normal during swaps), but log for Mode 1
        if (FireModeNum == 1)
        {
            //UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Bypassing Transactional Stop for Charged State (Mode 1)"));
        }

        // Standard UT logic handles the release (launching rockets or clearing pending fire)
        Super::StopFire(FireModeNum);
        // --- FIX START: Send Transactional Stop Packet for Charged Release ---
        if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
        {
            // A. Manually Increment Index 
            // Crucial because charged weapons skip the standard FireShot() which usually does this.
            int32 EventIndex = 0;
            if (ClientFireEventIndex.IsValidIndex(FireModeNum))
            {
                ClientFireEventIndex[FireModeNum]++;
                EventIndex = ClientFireEventIndex[FireModeNum];
            }

            // B. Capture Data
            float CurrentTime = GetWorld()->GetTimeSeconds();
            FRotator ClientRot = UTOwner->GetViewRotation();

            // C. Send RPC (With Rotation)
            ServerStopFireFixed(FireModeNum, EventIndex, CurrentTime, ClientRot);

            // D. Queue Retry (With Rotation)
            QueueResendFireFixed(false, FireModeNum, EventIndex, CurrentTime, ClientRot, 0, nullptr);
        }
        // --- FIX END ---
        // CRITICAL: Return here so we don't hit GotoActiveState() below
        return;
    }
*/
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

            // 3. Send transactional packet WITH rotation
            float CurrentTime = GetWorld()->GetTimeSeconds();
            FRotator ClientRot = UTOwner->GetViewRotation();
            ServerStopFireFixed(FireModeNum, EventIndex, CurrentTime, ClientRot);
            QueueResendFireFixed(false, FireModeNum, EventIndex, CurrentTime, ClientRot, 0, nullptr);
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
        float CurrentTime = GetWorld()->GetTimeSeconds();
        // Capture aim at the moment of release
        FRotator ClientRot = UTOwner->GetViewRotation();

        // Pass ClientRot to the server
        ServerStopFireFixed(FireModeNum, EventIndex, CurrentTime, ClientRot);
        QueueResendFireFixed(false, FireModeNum, EventIndex, CurrentTime, FRotator::ZeroRotator, 0, nullptr);
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



void AUTWeaponFix::ServerStartFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset)
{
    // 1. VALIDATION (Your existing transactional checks)
    UWorld* World = GetWorld();
    if (!World) return;

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
        BeginFiringSequence(FireModeNum, bClientPredicted);
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

void AUTWeaponFix::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Fire-validation telemetry — local client only, gated to the equipped weapon
    // in an active mode. One occlusion-aware crosshair trace; see UpdateFireValTracker.
    if (bFireValActive && Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
    {
        UpdateFireValTracker(DeltaTime);
    }

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
        bool bForceRecoverCharged = false;
        if (UUTWeaponStateFiringChargedRocket_Transactional* Chg = Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState))
        {
            // A legitimately-active charged state always has one of these in flight (loading, grace,
            // mid-burst, or the post-burst refire wait) and self-transitions — leave it alone. A charged
            // state that is IsFiring() with NONE of them and not charging is WEDGED: it never self-
            // transitions, and an incoming primary routes through its inherited stock
            // UUTWeaponStateFiring::BeginFiringSequence which just sets PendingFireSequence and returns —
            // no projectile, no reject, no log. Only the rocket has this state, which is why the no-reg
            // is rocket-only. Let the timeout below force it back to Active so primaries fire again.
            FTimerManager& TM = GetWorldTimerManager();
            const bool bBusy = Chg->bCharging
                || TM.IsTimerActive(Chg->LoadTimerHandle)
                || TM.IsTimerActive(Chg->GraceTimerHandle)
                || TM.IsTimerActive(Chg->FireLoadedRocketHandle)
                || TM.IsTimerActive(Chg->RefireCheckHandle);
            if (bBusy)
            {
                return;
            }
            bForceRecoverCharged = true;
        }

        float RefireTime = GetRefireTime(CurrentFireMode);

        // If we haven't received a valid RPC in > 2.5x the refire time, assume connection loss.
        // (e.g., for Link Gun (0.12s), if silent for 0.3s, kill it).
        float TimeoutThreshold = FMath::Max(0.25f, RefireTime * 2.5f);

        // LastFireTime is updated in ServerStartFireFixed
        if (LastFireTime.IsValidIndex(CurrentFireMode) &&
            GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode] > TimeoutThreshold)
        {
            if (bForceRecoverCharged)
            {
                // GotoActiveState, NOT StopFire — StopFire re-enters the charged EndFiringSequence and
                // would re-wedge. Reset the fire-mode tracker so the next primary is accepted.
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s force-recovered a WEDGED ChargedRocket state (mode=%d CurFiring=%d idle=%.1fs) — was swallowing primaries"),
                    *GetName(), CurrentFireMode, CurrentlyFiringMode,
                    GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode]);
                CurrentlyFiringMode = 255;
                for (int32 i = 0; i < FireModeActiveState.Num(); i++) { FireModeActiveState[i] = 0; }
                GotoActiveState();
            }
            else
            {
                // Force stop. This kills the looping audio and resets the state.
                StopFire(CurrentFireMode);
            }
        }
    }
}


void AUTWeaponFix::UpdateFireValTracker(float DeltaTime)
{
    // Smooth the client frame time regardless of on-target state, so the fps
    // context we ship is the player's actual cadence (EMA, ~last 20 frames).
    FireValFrameTimeEMA = (FireValFrameTimeEMA <= 0.0f) ? DeltaTime
                    : FMath::Lerp(FireValFrameTimeEMA, DeltaTime, 0.05f);

    // Only meaningful for the equipped weapon on a living owner.
    if (!UTOwner || UTOwner->IsDead() || UTOwner->GetWeapon() != this)
    {
        FireValAcquireTime = -1.0f;
        return;
    }

    UWorld* World = GetWorld();
    if (!World) { FireValAcquireTime = -1.0f; return; }

    const FVector Start = UTOwner->GetPawnViewLocation();
    const FVector Dir   = UTOwner->GetViewRotation().Vector();
    const FVector End   = Start + Dir * 100000.0f; // beyond any UT sightline; line-trace cost is ~flat in length

    // COLLISION_TRACE_WEAPON blocks on world geometry AND characters — the same
    // channel UT's own crosshair/visibility traces use (UTWeapon.cpp). So the FIRST
    // blocking hit is either a wall (occluded -> reset) or the enemy under the
    // crosshair: one trace yields "crosshair on a VISIBLE enemy", occlusion free.
    // Simple collision (bTraceComplex=false) -> tests the capsule, which is what we want.
    FCollisionQueryParams Params(FName(TEXT("FireValTrace")), false, UTOwner);
    FHitResult Hit;
    const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, COLLISION_TRACE_WEAPON, Params);

    AUTCharacter* OnEnemy = nullptr;
    if (bBlocked)
    {
        AUTCharacter* HitChar = Cast<AUTCharacter>(Hit.GetActor());
        if (HitChar && HitChar != UTOwner && !HitChar->IsDead())
        {
            const uint8 MyTeam = UTOwner->GetTeamNum();
            const uint8 ThTeam = HitChar->GetTeamNum();
            // 255 = no team (FFA): everyone is an enemy. Otherwise differing teams.
            if (MyTeam == 255 || ThTeam == 255 || MyTeam != ThTeam)
            {
                OnEnemy = HitChar;
            }
        }
    }

    if (OnEnemy)
    {
        // Re-acquire (reset dwell) when the crosshair is on a DIFFERENT enemy than
        // last frame. Without this, a snap from a long-tracked target onto a freshly
        // revealed one would inherit the old dwell and hide a fire-validation's
        // target-to-target snap as a "patient" shot. Weak ptr so a destroyed-then-
        // reused address can't be mistaken for the same target.
        if (FireValAcquireTime < 0.0f || FireValLastTarget.Get() != OnEnemy)
        {
            FireValAcquireTime = World->GetTimeSeconds();
        }
        FireValLastTarget = OnEnemy;
    }
    else
    {
        // LOS broken or no enemy under the crosshair -> end the run.
        FireValAcquireTime = -1.0f;
        FireValLastTarget = nullptr;
    }
}

AMutBotEvents* AUTWeaponFix::FindBotEventsMutator() const
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;
    for (TActorIterator<AMutBotEvents> It(World); It; ++It)
    {
        return *It;
    }
    return nullptr;
}

void AUTWeaponFix::ServerReportFireValidation_Implementation(int32 DwellMs, uint8 FrameMs, bool bClaimedHit)
{
    AUTPlayerState* PS = UTOwner ? Cast<AUTPlayerState>(UTOwner->PlayerState) : nullptr;
    if (!PS) return;
    // Record into the standalone collector — mutator-INDEPENDENT, so it works on any
    // NetcodePlus server (NA autopug doesn't load MutBotEvents). Clamp server-side; this
    // is review-only data, never gameplay-affecting.
    FNCFireValCollector::Get().Record(GetWorld(), PS, FMath::Clamp(DwellMs, 0, 60000), FrameMs, bClaimedHit);
}

bool AUTWeaponFix::ServerReportFireValidation_Validate(int32 DwellMs, uint8 FrameMs, bool bClaimedHit)
{
    // Pure telemetry — bounds are enforced in the impl; nothing to reject here.
    return true;
}

bool AUTWeaponFix::ServerStartFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset)
{
    // Sanity-bound the client head offset at the RPC edge. The headshot gate clamps it downstream, but a NaN
    // defeats FMath::Clamp (NaN fails every comparison) and that clamp is currently the sole defense, so reject
    // NaN/Inf or an absurd magnitude here. A legit offset is the rendered head relative to the body (~110u up),
    // so the 1000u bound is hugely generous — no honest client is ever caught; only a tampered one is dropped.
    if (ClientHeadOffset.ContainsNaN() || ClientHeadOffset.SizeSquared() > FMath::Square(1000.0f))
    {
        return false;
    }
    return FireModeNum < GetNumFireModes() &&
        InFireEventIndex > 0 &&
        ClientTimestamp > 0.0f;
}




void AUTWeaponFix::ServerStopFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot)
{
    // FIX: Update sequence ID to reject late Start packets
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        // Only update if this Stop is newer than what we've seen
        if (InFireEventIndex > AuthoritativeFireEventIndex[FireModeNum])
        {
            AuthoritativeFireEventIndex[FireModeNum] = InFireEventIndex;
        }
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

    // GHOST FIX (server): the guard below skips EndFiringSequence (and its PendingFire
    // clear) when the weapon is mid-cooldown / Unequipping — so a genuine release in
    // that window leaves the SERVER's PendingFire stale, and the server auto-fires the
    // next weapon on equip (the authoritative ghost rocket). Stock EndFiringSequence
    // clears PendingFire unconditionally; mirror that on the genuine release this RPC
    // represents. WATCH: this RPC is ALSO sent for an internal continue-fire stop
    // (client StopFire), so a HELD switch could clear it too — verify hold-through-switch
    // on the test hub (ncp.FireDebug logs role/state/pending below).
    if (GhostFix() && UTOwner)
    {
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] ServerStopFire clear mode=%d role=%d state=%s wasPending=%d pendingWpn=%d"),
                FireModeNum, (int32)Role,
                (GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null")),
                (UTOwner->IsPendingFire(FireModeNum) ? 1 : 0),
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


bool AUTWeaponFix::ServerStopFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot)
{
    return FireModeNum < GetNumFireModes();
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

    DOREPLIFETIME(AUTWeaponFix, AuthoritativeFireEventIndex);
    DOREPLIFETIME(AUTWeaponFix, FireModeActiveState);
    DOREPLIFETIME_CONDITION(AUTWeaponFix, bFireValActive, COND_OwnerOnly);
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
		// Only claim-capable projectiles (rocket + flak shell) are tracked; tracking e.g.
		// flak shards (9/shot) would FIFO-evict the shell/rocket from the 10-entry list
		// before its claim RPC arrives.
		const bool bTrackForRewind = bEnableProjectileRewind && NewProjectile &&
			(NewProjectile->IsA(AUTPlusProj_Rocket::StaticClass()) || NewProjectile->IsA(AUTPlusProj_FlakShell::StaticClass()));
		if (bTrackForRewind)
		{
			int32 ServerEventIdx = AuthoritativeFireEventIndex.IsValidIndex(CurrentFireMode)
				? AuthoritativeFireEventIndex[CurrentFireMode] : -1;
			ActiveServerProjectiles.Add(FActiveServerProjectile(NewProjectile, ServerEventIdx, CurrentFireMode));

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





void AUTWeaponFix::BringUp(float OverflowTime)
{
 


    
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

	// Skin support is gated via bSkinsEnabled — see UTWeaponFix.h. When disabled
	// we null WeaponSkin before Super::BringUp so Epic's AttachToOwner->SetSkin
	// path can't kick off a per-equip CreateAndSetMaterialInstanceDynamic chain.
	// The per-life MID allocation was a visible ~1-3ms hitch in duel on respawn.
	if (!bSkinsEnabled)
	{
		WeaponSkin = nullptr;
	}

	Super::BringUp(OverflowTime);

	// Load settings from Mod.ini on first weapon equip
	if (!bWeaponSettingsLoaded)
	{
		LoadWeaponSettings();
	}

	// Per-weapon hide: check if this weapon's class is marked hidden
	FName HideKey = FName(*GetClass()->GetName());
	if (UTOwner)
	{
		bool* bHidden = HiddenWeaponsByTag.Find(HideKey);
		if (bHidden && *bHidden)
		{
			if (Mesh)
			{
				Mesh->SetHiddenInGame(true);
			}
			if (UTOwner->FirstPersonMesh)
			{
				UTOwner->FirstPersonMesh->SetHiddenInGame(true);

				// Reset 1P mesh to default relative transform so the muzzle socket
				// is at a consistent position regardless of which weapon's VeryLowMeshOffset
				// was applied by UpdateWeaponHand. Without this, swapping between
				// hidden weapons shifts the beam origin because each weapon has different offsets.
				USkeletalMeshComponent* FPMesh = UTOwner->FirstPersonMesh;
				USkeletalMeshComponent* FPMeshArchetype = Cast<USkeletalMeshComponent>(FPMesh->GetArchetype());
				if (FPMeshArchetype)
				{
					FPMesh->SetRelativeLocationAndRotation(
						FPMeshArchetype->RelativeLocation,
						FPMeshArchetype->RelativeRotation
					);
				}
			}
		}
	}

	// Skin cache lookup + MID creation + SavedMeshMaterials patching.
	// Gated off via bSkinsEnabled — see UTWeaponFix.h. The MID creation path
	// (CreateAndSetMaterialInstanceDynamicFromMaterial) is the per-life duel
	// hitch: every fresh weapon instance post-respawn allocates 1-N new MIDs.
	if (bSkinsEnabled)
	{
		if (!WeaponSkin && WeaponSkinCustomizationTag != NAME_None)
		{
			UUTWeaponSkin** Cached = CachedSkinAssets.Find(WeaponSkinCustomizationTag);
			if (Cached && *Cached)
			{
				WeaponSkin = *Cached;
			}
		}

		if (WeaponSkin && Mesh && WeaponSkin->FPSMaterial)
		{
			int32 NumSlots = Mesh->GetNumMaterials();
			if (CachedSkinMIDs.Num() != NumSlots)
			{
				CachedSkinMIDs.Empty();
				for (int32 i = 0; i < NumSlots; i++)
				{
					UMaterialInstanceDynamic* MID = Mesh->CreateAndSetMaterialInstanceDynamicFromMaterial(i, WeaponSkin->FPSMaterial);
					CachedSkinMIDs.Add(MID);
				}
			}
			else
			{
				for (int32 i = 0; i < NumSlots; i++)
				{
					if (CachedSkinMIDs[i])
					{
						Mesh->SetMaterial(i, CachedSkinMIDs[i]);
					}
				}
			}
			for (int32 i = 0; i < CachedSkinMIDs.Num(); i++)
			{
				if (CachedSkinMIDs[i] && SavedMeshMaterials.IsValidIndex(i))
				{
					SavedMeshMaterials[i] = CachedSkinMIDs[i];
				}
			}
		}
	}
}



void AUTWeaponFix::GetImpactSpawnPosition(const FVector& TargetLoc, FVector& SpawnLocation, FRotator& SpawnRotation)
{
	// When weapon is hidden, spawn beam effects from camera center
	// instead of the muzzle socket (which is at a wrong position due to VeryLowMeshOffset).
	// This makes beams fire straight from the crosshair, matching projectile behavior.
	FName HideKey = FName(*GetClass()->GetName());
	bool* bHidden = HiddenWeaponsByTag.Find(HideKey);
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

	Super::GetImpactSpawnPosition(TargetLoc, SpawnLocation, SpawnRotation);
}

void AUTWeaponFix::PlayFiringEffects()
{
	// When the weapon is locally hidden, the muzzle flash PSC is still attached to
	// the (hidden) muzzle socket while the beam/impact spawns from the camera-adjusted
	// origin (see GetImpactSpawnPosition). That mismatch produces a visible puff at
	// the hand while the beam comes from chest height. Suppress only the muzzle flash
	// for the current fire mode — sound, anim, kickback, and beam all still fire.
	UParticleSystemComponent* SavedPSC = nullptr;
	int32 SavedIndex = INDEX_NONE;
	const FName HideKey = FName(*GetClass()->GetName());
	const bool* bHidden = HiddenWeaponsByTag.Find(HideKey);
	if (bHidden && *bHidden && UTOwner)
	{
		const uint8 EffectFiringMode = (Role == ROLE_Authority || UTOwner->Controller != nullptr) ? CurrentFireMode : UTOwner->FireMode;
		if (MuzzleFlash.IsValidIndex(EffectFiringMode))
		{
			SavedPSC = MuzzleFlash[EffectFiringMode];
			SavedIndex = EffectFiringMode;
			MuzzleFlash[EffectFiringMode] = nullptr;
		}
	}

	Super::PlayFiringEffects();

	if (SavedIndex != INDEX_NONE && MuzzleFlash.IsValidIndex(SavedIndex))
	{
		MuzzleFlash[SavedIndex] = SavedPSC;
	}
}

void AUTWeaponFix::SetSkin(UMaterialInterface* NewSkin)
{
	Super::SetSkin(NewSkin);

	// Cached-MID re-apply path. Gated off via bSkinsEnabled (see UTWeaponFix.h).
	// With skins disabled, WeaponSkin is nulled in BringUp and CachedSkinMIDs is
	// never populated — this block is already inert, but the explicit gate
	// documents intent and short-circuits the branch chain.
	if (bSkinsEnabled && !NewSkin && WeaponSkin && Mesh && CachedSkinMIDs.Num() > 0)
	{
		int32 NumSlots = FMath::Min(Mesh->GetNumMaterials(), CachedSkinMIDs.Num());
		for (int32 i = 0; i < NumSlots; i++)
		{
			if (CachedSkinMIDs[i])
			{
				Mesh->SetMaterial(i, CachedSkinMIDs[i]);
			}
		}
	}
}

// ============================================================================
// UTWeaponFix.cpp - Transactional Retry System Implementation
// ============================================================================

// 1. QUEUE LOGIC (Client Side)
// Call this inside your FireShot() client block
void AUTWeaponFix::QueueResendFireFixed(bool bIsStartFire, uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot, uint8 ZOffset, AUTCharacter* ClientHitChar)
{
    // Only the owning client needs to queue retries
    if (Role == ROLE_Authority && GetNetMode() != NM_Standalone) return;

    // Create the payload
    FPendingFireEventFix NewEvent(bIsStartFire, FireModeNum, InFireEventIndex, ClientTimestamp, ClientViewRot, ZOffset, ClientHitChar);

    // Queue 2 copies. This gives us 2 retry attempts (spaced by the timer delay)
    // before we give up. This prevents infinite network flooding if the connection is dead.
    ResendFireEvents.Add(NewEvent);
    ResendFireEvents.Add(NewEvent);

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
            ResendServerStartFireFixed(Event.FireModeNum, Event.FireEventIndex, Event.ClientTimestamp, Event.ClientViewRot, Event.ZOffset, Event.HitChar.Get());
        }
        else
        {
            ResendServerStopFireFixed(Event.FireModeNum, Event.FireEventIndex, Event.ClientTimestamp, Event.ClientViewRot);
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
void AUTWeaponFix::ResendServerStartFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot, uint8 ZOffset, AUTCharacter* ClientHitChar)
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

    // Execute the actual fire logic
    // This calculates the delay and fast-forwards the projectile to catch up
    // Resent (dropped-packet) shots don't carry the client head offset (it isn't threaded through the
    // resend path) — pass zero; these fall back to the stock capsule-relative head check. Rare + graceful.
    ServerStartFireFixed(FireModeNum, InFireEventIndex, ClientTimestamp, true, ClientViewRot, ClientHitChar, ZOffset, FVector::ZeroVector);

    bNetDelayedShot = false;
}

// 6. SERVER HANDLER (Stop Fire)
void AUTWeaponFix::ResendServerStopFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot)
{
    // Duplicate check for Stop Fire is less critical but good for consistency
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        int32 LastIdx = AuthoritativeFireEventIndex[FireModeNum];
        if (InFireEventIndex <= LastIdx && (LastIdx - InFireEventIndex) < 100) return;
    }
    if (UTOwner && UTOwner->PlayerState)
    {
        float CurrentPing = UTOwner->PlayerState->ExactPing;
        UE_LOG(LogUTWeaponFix, Verbose, TEXT("[Retry] STOP Fire Accepted for %s. Index: %d | Ping: %.2f ms | RTT Correction Applied"),
            *UTOwner->PlayerState->PlayerName, InFireEventIndex, CurrentPing);
    }
    bNetDelayedShot = true;
    ServerStopFireFixed(FireModeNum, InFireEventIndex, ClientTimestamp, ClientViewRot);
    bNetDelayedShot = false;
}

bool AUTWeaponFix::ResendServerStartFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot, uint8 ZOffset, AUTCharacter* ClientHitChar)
{
    return true;
}

bool AUTWeaponFix::ResendServerStopFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot)
{
    return true;
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
	ServerProjectileHitClaim(HitTarget, HitLocation, -1, FireModeNum);
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

bool AUTWeaponFix::ServerProjectileHitClaim_Validate(AUTCharacter* ClaimedTarget, FVector ClaimedHitLocation,
	int32 ClaimedEventIndex, uint8 ClaimedFireMode)
{
	return true;
}

void AUTWeaponFix::ServerProjectileHitClaim_Implementation(AUTCharacter* ClaimedTarget, FVector ClaimedHitLocation,
	int32 ClaimedEventIndex, uint8 ClaimedFireMode)
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
	// Match by FireMode, oldest first (FIFO). EventIndex match preferred if provided.
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
			if (GraceIndex == -1
				&& Entry.FireMode == ClaimedFireMode
				&& (ClaimedEventIndex < 0 || Entry.EventIndex == ClaimedEventIndex))
			{
				GraceIndex = i;
			}
			continue;
		}

		if (Entry.FireMode != ClaimedFireMode)
		{
			continue;
		}
		// If client sent a specific EventIndex, require exact match
		if (ClaimedEventIndex >= 0 && Entry.EventIndex != ClaimedEventIndex)
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
