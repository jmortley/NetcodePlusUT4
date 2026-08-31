
#include "UTWeaponFix.h"
#include "NCShockInputTrace.h"
#include "NCPClockSync.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeaponAttachment.h"
#include "Engine/World.h"
#include "Components/InputComponent.h"
#include "Engine/NetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTWeaponStateZooming.h"
#include "UTWeaponStateFiringSpinUp.h"
#include "UTPlusShockRifle.h"
#include "UTPlusProj_ShockBall.h"
#include "UTPlusProj_Rocket.h"
#include "UTPlusProj_FlakShell.h"
#include "UTProj_FlakShard.h"
#include "UTProj_FlakShell.h"
#include "UTPlusProj_StingerShard.h"
#include "UTInventory.h"    // TInventoryIterator (FindFiringWeaponForProjectile)
#include "UTDamageType.h"   // FUTRadialDamageEvent (grace-buffer direct-hit damage)
#include "UTPlusWeap_RocketLauncher.h"
#include "UTDualWeapon.h"   // ApplyWeaponHideState: dual-enforcer LeftMesh
#include "UTWeaponSkin.h"
#include "AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
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

// Rocket primary diagnostics only. This intentionally changes no firing state, timers,
// timestamps, or RPC payloads. Level 1 traces the M1 transaction/cadence lifecycle; level 2
// also traces predicted-projectile delay, charged load/release ownership, and wedge transitions.
// Default 0 for live (restored for the 2026-08-14 328 cut; was 2 during the 328-RC dogfood).
static TAutoConsoleVariable<int32> CVarRocketPrimaryDiag(
    TEXT("ncp.RocketPrimaryDiag"), 0,
    TEXT("Rocket M1 diagnostics: 0=off, 1=refire/event/server/ACK lifecycle, 2=also fake-delay and charged load/release details. Set separately on clients and dedicated servers. Logging only; no behavior change."),
    ECVF_Default);

static FORCEINLINE int32 RocketPrimaryDiagLevel()
{
    return CVarRocketPrimaryDiag.GetValueOnGameThread();
}

static FORCEINLINE bool RocketPrimaryDiagFor(AUTWeaponFix* Weapon, uint8 FireModeNum = 0, int32 RequiredLevel = 1)
{
	const int32 Level = RocketPrimaryDiagLevel();
	return Level >= RequiredLevel
		&& (FireModeNum == 0 || Level >= 2)
		&& Cast<AUTPlusWeap_RocketLauncher>(Weapon) != nullptr;
}

static FORCEINLINE const TCHAR* RocketPrimaryDiagPlayer(AUTWeaponFix* Weapon)
{
	return Weapon && Weapon->GetUTOwner() && Weapon->GetUTOwner()->PlayerState
		? *Weapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?");
}

// "Ghost rocket" fix toggle (default OFF so the build is identical to today until
// flipped). When 1: carry the REAL held-fire state across a weapon switch instead of
// the retry-timer graduation, and clear the server's PendingFire on a genuine release.
// 0 = legacy retry-graduation + :1779-guarded server clear (today's behaviour).
// Runtime-toggleable (rcon) so ONE hub can A/B it live. See StartFire / StopFire /
// PutDown / ServerStopFireFixed. No replicated/RPC change; pairs with ncp.FireDebug.
// Click buffer (ncp.ClickBufferMs): Shock primary only. A same-mode press queued
// to the next legal fire time whose RELEASE arrives inside the configured window
// keeps that release-time view rotation and fires exactly once at the legal time.
// The shot timestamp, muzzle, collision world, target claim, and server ROF remain
// legal-execution-time values; this does not grant extra rewind. 0 = off. The
// effective value is hard-clamped to 40ms even if the cvar is set higher; a
// separate 50ms snapshot-age ceiling leaves 10ms of high-FPS timer slack.
static TAutoConsoleVariable<float> CVarClickBufferMs(
    TEXT("ncp.ClickBufferMs"), 0.0f,
    TEXT("Shock-primary rapid-click reliability: a queued click released while its shot is due within this many ms fires once ")
    TEXT("at the legal time using RELEASE-time aim. Execution time/world/rewind are not backdated. Effective range 0-40ms; 0=off."),
    ECVF_Default);

static constexpr float MaxClickBufferMs = 40.0f;
static constexpr float ClickBufferDispatchSlackSeconds = 0.025f;
static constexpr float MaxBufferedAimAgeSeconds = 0.050f;

// Exact zero rotation is a legitimate aim. This synchronous scope is the
// validity bit for CachedTransactionalRotation; it avoids changing the weapon
// object layout or relying on FRotator::IsZero() as a sentinel.
static TSet<const AUTWeaponFix*> ScopedTransactionalAimWeapons;

static FORCEINLINE bool IsShockPrimaryClickBuffer(AUTWeaponFix* Weapon, uint8 FireModeNum)
{
    return FireModeNum == 0 && Cast<AUTPlusShockRifle>(Weapon) != nullptr;
}

static FORCEINLINE float GetClickBufferWindowSeconds()
{
    return FMath::Clamp(CVarClickBufferMs.GetValueOnGameThread(), 0.0f, MaxClickBufferMs) * 0.001f;
}

static FORCEINLINE bool HasScopedTransactionalAim(const AUTWeaponFix* Weapon)
{
    return ScopedTransactionalAimWeapons.Contains(Weapon);
}

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

// Server-live exact-hitscan validation tuning. These are deliberately separate
// from the legacy per-weapon FudgeFactorMs/HitScanPadding fields: the former is
// also used by projectile presentation, while the latter was bypassed by the
// hardcoded moving-target primary pad below. Client values have no authority.
static TAutoConsoleVariable<float> CVarHitscanFudgeMs(
    TEXT("ncp.HitscanFudgeMs"), 10.0f,
    TEXT("Full-RTT buffer subtracted before halving server-observed RTT for hitscan target rewind, ms. ")
    TEXT("10 means a 5ms-newer-than-half-RTT primary epoch. Server-authoritative; live rollback: 20."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarHitscanPrimaryPadding(
    TEXT("ncp.HitscanPrimaryPadding"), 40.0f,
    TEXT("Extra radius (uu) for the specifically client-claimed moving target at the primary hitscan epoch. ")
    TEXT("Stationary targets continue to use HitScanPaddingStationary. Server-authoritative; live."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarHitscanSearchPadding(
    TEXT("ncp.HitscanSearchPadding"), 40.0f,
    TEXT("Extra radius (uu) at each claimed-target hitscan time-search fallback rung. ")
    TEXT("Server-authoritative; live rollback: 45."),
    ECVF_Default);

// =========================================================================
// RESCUE LEAD GATE (time search) — a CREDITED-POSITION displacement gate,
// not an aim judge. Each accepted time-search rung credits a historical
// capsule position; the gate bounds how far that position may sit ahead of
// the render-epoch estimate (halfRTT + ncp.HitAttribRenderExtraMs), measured
// along the target's HISTORICAL motion between the two epochs. It carries no
// shot-ray term: two shots at the same target get the same verdict, and for
// steady movement the quantity approximates target speed x epoch gap, so the
// cap is also a de-facto speed threshold (~1150 uu/s at 40uu under default
// epochs; dodges block, runs pass). Ray-relative telemetry (rescueRayAheadUU,
// rescueRenderMissUU) is logged per shot so a shadow night can separate
// rendered-body aim from leading aim BEFORE enforcement is trusted; the
// 2026-08 pug corpus (386 rescues, 95% crediting positions ahead of render,
// honest ping-spike recoveries near zero) is the motivating prior, not the
// calibration — the shadow fields are. Above the 250ms rewind cap the
// validation/render epoch gap grows with RTT and honest fast movement can
// exceed the cap; that is a deliberate bound on extreme-latency rescues.
// Primary and padding acceptance are untouched.
// =========================================================================
static TAutoConsoleVariable<int32> CVarHitscanRescueLeadGate(
    TEXT("ncp.HitscanRescueLeadGate"), 0,
    TEXT("Per-rung credited-position lead gate on the claimed-target time search: 0=shadow ")
    TEXT("(default; verdicts appear as rescueLead= fields on [HitAttrib] lines, which ")
    TEXT("require ncp.HitAttribDebug=1 — no behavior change), 1=enforce (a rung crediting ")
    TEXT("a position more than ncp.HitscanMaxRescueLeadUU ahead of the render-epoch ")
    TEXT("estimate is skipped; deeper rungs may still accept, otherwise the claimed-target ")
    TEXT("search yields nothing and any primary/world result stands). No RTT measurement = ")
    TEXT("fail closed. Server-side only; flippable live."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarHitscanMaxRescueLeadUU(
    TEXT("ncp.HitscanMaxRescueLeadUU"), 40.0f,
    TEXT("Maximum distance (uu) a time-search rung's credited capsule position may sit ")
    TEXT("ahead of the render-epoch estimate, along the target's historical motion. Also ")
    TEXT("acts as a speed threshold (~cap/epoch-gap uu/s) for steady movers. Calibrate ")
    TEXT("from shadow-night rescueLeadUU data before enforcing. Default: 40."),
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
// Default 0 for live (restored for the 2026-08-14 328 cut; was 1 during the
// 328-RC dogfood — SERVER-ADMINS.md documents the live default).
// =========================================================================
static TAutoConsoleVariable<int32> CVarHitAttribDebug(
    TEXT("ncp.HitAttribDebug"), 0,
    TEXT("Hitscan acceptance attribution: 0=off, 1=log one [HitAttrib] line per server-validated shot and one [HitAttrib.Client] line per client claim decision."),
    ECVF_Default);

// The server cannot see what the shooter's client actually rendered, so the
// exact-hitscan unclaimed render gate and leadUU telemetry approximate that
// position as GetRewindLocation(halfRTT + this many ms). Keep this estimate
// independent from Link/Minigun's claimless render-authority estimate below:
// those modes use a different client proxy-prediction path and need to be
// calibrated without moving the Shock/Sniper acceptance epoch.
static TAutoConsoleVariable<float> CVarHitAttribRenderExtraMs(
    TEXT("ncp.HitAttribRenderExtraMs"), 30.0f,
    TEXT("Estimated render latency beyond half-RTT for the unclaimed exact-hitscan render gate and [HitAttrib], ms. Does not affect Link/Minigun render authority. Default: 30."),
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

// Render-authoritative targeting for opted-in claim-INCAPABLE fire (the Link
// beam's per-tick re-traces and Minigun spread bullets). Those modes can never
// provide a client target claim. Testing BOTH raw validation history and the
// estimated render-time history creates two separate acceptance lobes: a target
// can be hit at a server-history position the shooter was never shown. With this
// enabled, the render-time capsule REPLACES the raw capsule as the sole target-
// selection sample. The ray, world-geometry clipping, timing estimate, and
// target choice remain server-owned — this is not client-side hit detection.
// The legacy cvar/method names are retained for live rollback and config
// compatibility. Weapons opt in via AUTWeaponFix::SupportsRenderCredit().
static TAutoConsoleVariable<int32> CVarRenderCredit(
    TEXT("ncp.RenderCredit"), 1,
    TEXT("Opted-in claim-incapable fire (Link beam, Minigun) selects targets only at the estimated render-time capsule: 1=render-authoritative (default), 0=raw rewind only (live rollback). Server-side only; flippable live."),
    ECVF_Default);

// Link beam and Minigun primary cannot attach a per-tick target claim, and the
// client proxy path used while holding those weapons forward-simulates remote
// characters. Its residual presentation delay is therefore not the same value
// calibrated for unclaimed Shock/Sniper shots above. The current 50ms movement
// smoothing setting is an exponential correction window, not a fixed 50ms
// interpolation buffer; character proxies also extrapolate between replicated
// updates. Start with 15ms as a conservative residual estimate and keep this
// server-live CVar separate so Link/Minigun tuning cannot silently change exact
// hitscan registration. The half-RTT term still represents the age of the
// client aim state when it reaches the server.
static TAutoConsoleVariable<float> CVarRenderCreditExtraMs(
    TEXT("ncp.RenderCreditExtraMs"), 15.0f,
    TEXT("Estimated presentation delay beyond half server-observed RTT for render-authoritative Link beam and Minigun primary targeting, ms. Independent of ncp.HitAttribRenderExtraMs. Default: 15."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarRenderCreditSlack(
    TEXT("ncp.RenderCreditSlack"), 0.0f,
    TEXT("Extra radius (uu) around the render-authoritative capsule for opted-in claimless fire. Deliberately 0 by default; widen only with evidence."),
    ECVF_Default);

bool AUTWeaponFix::GetServerObservedRTTMs(const AUTPlayerController* ShooterPC,
    float& OutRTTMs)
{
    OutRTTMs = 0.f;
    // UT's ExactPing is reported back through an RPC whose validation accepts
    // any float. Prefer the server connection's ACK-derived RTT so a modified
    // client cannot select its own target-history epoch. AvgLag initializes to
    // 9999; report "unavailable" until the server has a measurement so each
    // caller can reject or deliberately use a zero base epoch.
    if (ShooterPC != nullptr)
    {
        const UNetConnection* Connection = ShooterPC->GetNetConnection();
        if (Connection != nullptr && Connection->AvgLag >= 0.f && Connection->AvgLag < 5.f)
        {
            OutRTTMs = Connection->AvgLag * 1000.f;
            return true;
        }
    }
    return false;
}

float AUTWeaponFix::GetConfiguredHitscanFudgeMs()
{
    return FMath::Max(0.f, CVarHitscanFudgeMs.GetValueOnGameThread());
}

// GetRewindLocation() silently falls back to the oldest/current location when
// the requested time is not bracketed by history. That is acceptable for
// stock-style forgiveness, but not when this sample is the sole authority for
// what a remote shooter was estimated to have rendered. Require a real bracket
// and reject any interval containing a teleport marker.
static bool HasContinuousRenderHistory(AUTCharacter* Target, float RenderTime,
    float ValidationTime, int32& OutOlderIndex, int32& OutNewerIndex)
{
    OutOlderIndex = INDEX_NONE;
    OutNewerIndex = INDEX_NONE;
    if (Target == nullptr || Target->GetWorld() == nullptr)
    {
        return false;
    }
    if (RenderTime <= 0.f)
    {
        return true;
    }

    const TArray<FSavedPosition>& History = Target->SavedPositions;
    if (History.Num() < 2)
    {
        // Preserve the sole available endpoint as a conservative blocker.
        if (History.Num() == 1)
        {
            OutNewerIndex = 0;
        }
        return false;
    }

    const float Now = Target->GetWorld()->GetTimeSeconds();
    const float RenderWorldTime = Now - RenderTime;
    for (int32 Index = History.Num() - 1; Index >= 0; --Index)
    {
        // Match GetRewindLocation()'s strict comparison so the same two
        // samples are treated as the render-time bracket.
        if (History[Index].Time < RenderWorldTime)
        {
            OutOlderIndex = Index;
            break;
        }
    }

    if (OutOlderIndex == INDEX_NONE)
    {
        // Requested epoch predates retained history. We cannot validate a hit,
        // but the oldest available capsule must still occlude farther targets.
        OutNewerIndex = 0;
        return false;
    }
    if (OutOlderIndex == History.Num() - 1)
    {
        // No newer sample exists. A stationary authority anchor is still
        // provable: clamping to it cannot invent a different target position.
        // A moving/changed anchor remains unverifiable and fails closed.
        return !History[OutOlderIndex].bTeleported &&
            (Target->GetActorLocation() - History[OutOlderIndex].Position).IsNearlyZero(0.1f);
    }

    OutNewerIndex = OutOlderIndex + 1;
    if (History[OutOlderIndex].bTeleported || History[OutNewerIndex].bTeleported)
    {
        return false;
    }

    const float IntervalStart = Now - FMath::Max(RenderTime, ValidationTime);
    const float IntervalEnd = Now - FMath::Min(RenderTime, ValidationTime);
    for (int32 Index = 0; Index < History.Num(); ++Index)
    {
        if (History[Index].Time > IntervalEnd)
        {
            break;
        }
        if (History[Index].Time >= IntervalStart && History[Index].bTeleported)
        {
            return false;
        }
    }

    return true;
}

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

    const float BaseRadius = FMath::Min(CollisionRadius, CollisionHeight) +
        TraceRadius;
    const float Distance = FVector::Dist(ClosestPoint, ClosestCapsulePoint);
    OutMissBy = Distance - BaseRadius;
    const float Tolerance = FMath::Max(0.0f, CVarVisualHitscanClaimTolerance.GetValueOnGameThread());
    return Distance <= BaseRadius + Tolerance;
}

// Return the same geometry-clipped end point used by HitScanTrace's pawn pass.
// The rendered capsule must be in front of client-visible cover before an
// immediate sound is trustworthy. Claims keep their existing path; this extra
// trace is performed only after a client pawn candidate already exists.
static FVector GetPredictedHitsoundTraceEnd(
    UWorld* World,
    AActor* IgnoreActor,
    const FVector& StartLocation,
    const FVector& EndTrace,
    float TraceRadius)
{
    if (World == nullptr)
    {
        return EndTrace;
    }

    FHitResult WorldHit;
    FCollisionQueryParams QueryParams(
        FName(TEXT("PredictedHitsoundWorldClip")), true, IgnoreActor);

    constexpr int32 MaxCosmeticAttachmentSkips = 16;
    int32 CosmeticAttachmentSkips = 0;
    for (;;)
    {
        const bool bWorldHit = (TraceRadius <= 0.0f)
            ? World->LineTraceSingleByChannel(
                WorldHit, StartLocation, EndTrace,
                COLLISION_TRACE_WEAPONNOCHARACTER, QueryParams)
            : World->SweepSingleByChannel(
                WorldHit, StartLocation, EndTrace, FQuat::Identity,
                COLLISION_TRACE_WEAPONNOCHARACTER,
                FCollisionShape::MakeSphere(TraceRadius), QueryParams);

        if (!bWorldHit || !WorldHit.bBlockingHit)
        {
            return EndTrace;
        }

        AUTWeaponAttachment* CosmeticAttachment =
            Cast<AUTWeaponAttachment>(WorldHit.GetActor());
        if (CosmeticAttachment == nullptr ||
            CosmeticAttachmentSkips >= MaxCosmeticAttachmentSkips)
        {
            return WorldHit.Location;
        }

        QueryParams.AddIgnoredActor(CosmeticAttachment);
        ++CosmeticAttachmentSkips;
    }
}

// Client prediction is intentionally stricter than hit claiming. A claim can
// still be corrected or rejected by the authority, but an immediate sound
// cannot be taken back. Keep edge contacts and targets the server is already
// guaranteed to reject on the authoritative-sound path.
static bool IsHighConfidencePredictedHitsoundTarget(const AUTCharacter* Target)
{
    return Target != nullptr &&
        !Target->IsDead() &&
        Target->Health > 0 &&
        Target->bCanBeDamaged &&
        !Target->bSpawnProtectionEligible;
}

// Require the hitscan ray to pass this far inside the rendered capsule before
// producing speculative audio. This is separate from (and deliberately more
// conservative than) ncp.VisualHitscanClaimTolerance: edge claims still reach
// the server and receive an authoritative sound if accepted.
static constexpr float PredictedHitsoundCapsuleInset = 4.0f;

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

// ── Slide-posture grace for hit validation ─────────────────────────────────
// A floor slide shrinks the authoritative capsule THE SAME FRAME it starts
// (bWantsToCrouch |= bIsFloorSliding), but the shooter's screen keeps a
// mostly-standing body for one replication interp (~50-100ms) plus the animBP
// blend-in (~150-250ms). Position rewind cannot fix this: it reconstructs WHERE
// the target was, never WHAT SHAPE. Slide posture, unlike posture in general,
// IS reconstructible after the fact — PerformFloorSlide re-stamps
// FloorSlideTapTime at true slide start (landing slides included), so the slide
// age at the claimed moment is (movement-time now - tap time) - rewind.
static TAutoConsoleVariable<float> CVarSlideGraceMs(
    TEXT("ncp.SlideGraceMs"),
    250.0f,
    TEXT("Grace window (ms) after a floor slide starts during which hitscan AND projectile\n")
    TEXT("rewind validation test the standing capsule envelope instead of the slide capsule, covering the\n")
    TEXT("replication interp + slide anim blend-in still on the shooter's screen.\n")
    TEXT("0 = off (always the slide capsule, the pre-grace behavior)."),
    ECVF_Default
);

bool AUTWeaponFix::IsLiveHitscanTarget(const AUTCharacter* Target)
{
    // bHidden is intentional: ping-compensated spawn uses it to mark a live
    // pawn that must not be shootable until reveal. Feign death does not hide
    // the character actor; it only disables the capsule's query collision.
    if (Target == nullptr || Target->IsDead() || Target->IsPendingKillPending() ||
        Target->bHidden)
    {
        return false;
    }

    // Manual hitscan uses capsule geometry directly, independent of the
    // component's collision state. A live feigning player is ragdolled with a
    // NoCollision capsule and must remain hittable; collision-state filtering
    // here would turn the FeignDeath console command into hitscan immunity.
    return Target->GetCapsuleComponent() != nullptr;
}

void AUTWeaponFix::ApplySlidePostureForValidation(const AUTCharacter* Target,
    float RewindTime, FVector& InOutTargetLocation, float& InOutCollisionHeight)
{
    if (Target == nullptr || Target->UTCharacterMovement == nullptr ||
        !Target->UTCharacterMovement->bIsFloorSliding)
    {
        return;
    }

    const float GraceSeconds =
        FMath::Max(0.f, CVarSlideGraceMs.GetValueOnGameThread() * 0.001f);
    // Slide age at the claimed moment, not at validation time: the claim is
    // RewindTime in the past. Negative = target had not even started sliding at
    // the claimed moment, which the standing envelope also covers.
    const float SlideElapsedAtClaim =
        (Target->UTCharacterMovement->GetCurrentMovementTime() -
            Target->UTCharacterMovement->FloorSlideTapTime) - RewindTime;
    if (GraceSeconds > 0.f && SlideElapsedAtClaim < GraceSeconds)
    {
        // Bottom-aligned standing envelope. Raising the (already crouch-shrunk)
        // live capsule centre by (standing - live) covers BOTH regimes a rewound
        // location can be in: its top reaches a pre-shrink standing head exactly,
        // its bottom keeps the post-shrink feet. Standing strictly contains the
        // slide capsule (same radius, greater half-height), so one test suffices.
        const ACharacter* DefaultChar =
            Target->GetClass()->GetDefaultObject<ACharacter>();
        const float StandingHalfHeight =
            (DefaultChar != nullptr && DefaultChar->GetCapsuleComponent() != nullptr)
            ? DefaultChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
            : InOutCollisionHeight;
        if (StandingHalfHeight > InOutCollisionHeight)
        {
            InOutTargetLocation.Z += StandingHalfHeight - InOutCollisionHeight;
            InOutCollisionHeight = StandingHalfHeight;
        }
        return;
    }

    // Established slide (or grace disabled): the classic bottom-aligned shrink.
    InOutTargetLocation.Z =
        InOutTargetLocation.Z - InOutCollisionHeight + Target->SlideTargetHeight;
    InOutCollisionHeight = Target->SlideTargetHeight;
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
/** Ready AND every optional entry loaded. Ready gates catalog use; complete gates the
 *  periodic rescan, so an optional skin whose PAK mounts late can still join. */
static bool bWeaponSkinCatalogComplete = false;

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
static const FName WEAPON_SKIN_CATALOG_ROOT(TEXT("/Game/NetcodePlusOptional"));

// Versioned public selection manifest for the weapon skins shipped in the
// MutAnnouncers optional-content PAK. These paths are unrestricted and carry a
// real weapon-family tag; utility and cook-specific variants remain invalid.
// Future folder additions do not become network-valid automatically.
// Two tiers:
//   REQUIRED — all-or-nothing: the catalog only goes ready when EVERY entry
//     resolves, so a missing entry disables weapon skins entirely. This is the
//     deployment tripwire for content proven to be in the shipped PAK.
//   OPTIONAL — best-effort: entries that resolve join the catalog; missing ones
//     are logged and skipped without taking the feature down. New/experimental
//     skins go here until their cook is proven, then graduate to REQUIRED.
// Both tiers pass the same per-asset validation; nothing outside these lists
// ever becomes network-valid.
static const TCHAR* const REQUIRED_WEAPON_SKIN_ASSETS[] =
{
	TEXT("FlakPink"),
	TEXT("FlakRedDeath"),
	TEXT("FlakVoid"),
	TEXT("InvisibleBio"),
	TEXT("InvisibleFlak"),
	TEXT("InvisibleLG"),
	TEXT("InvisibleLinkElim"),
	TEXT("InvisibleMinigun"),
	TEXT("InvisibleRocketRegular"),
	TEXT("InvisibleShock"),
	TEXT("InvisibleSniper"),
	TEXT("RocketBeeElim"),
	TEXT("RocketBurn"),
	TEXT("RocketMahoganyElim"),
	TEXT("RocketSnowElim"),
	TEXT("ShockBlackTiger"),
	TEXT("ShockBlueBird"),
	TEXT("SniperBlueBird"),
	TEXT("SniperMahogany"),
	TEXT("SniperPink")
};

static const TCHAR* const OPTIONAL_WEAPON_SKIN_ASSETS[] =
{
	TEXT("GhostBio"),
	TEXT("GhostFlak"),
	TEXT("GhostIGRifle"),
	TEXT("GhostLG"),
	TEXT("GhostLinkElim"),
	TEXT("InvisibleIGRifle"),
	TEXT("PinkLG"),
	TEXT("RocketPink")
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
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TMap<FString, UUTWeaponSkin*> NewCatalog;
	int32 LoadedRequiredCount = 0;
	int32 LoadedOptionalCount = 0;
	auto LoadManifestGroup = [&AssetRegistry, &NewCatalog](
		const TCHAR* const* AssetNames, int32 AssetCount, int32& LoadedCount)
	{
		for (int32 Index = 0; Index < AssetCount; ++Index)
		{
			const FString ObjectPath = GetWeaponSkinObjectPath(AssetNames[Index]);
			// Query only the exact cooked manifest entry. GetAssetsByPath() defaults
			// to including in-memory assets; in UE 4.15 that walks every live asset
			// under the path and calls GetAssetRegistryTags(). Custom announcer waves
			// share this mount, and their ResourceSize tag can trip SoundWave.cpp's
			// DTYPE_Native shipping ensure during startup or map travel.
			const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(
				FName(*ObjectPath), true);
			if (!AssetData.IsValid()
				|| AssetData.AssetClass != UUTWeaponSkin::StaticClass()->GetFName())
			{
				continue;
			}

			// This is the only synchronous load in the feature, and it occurs during
			// lifecycle preload. Unlisted folder assets are never loaded or approved.
			UUTWeaponSkin* Skin = Cast<UUTWeaponSkin>(AssetData.GetAsset());
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
	LoadManifestGroup(OPTIONAL_WEAPON_SKIN_ASSETS,
		ARRAY_COUNT(OPTIONAL_WEAPON_SKIN_ASSETS), LoadedOptionalCount);

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
		// Missing optionals must be surfaced before NewCatalog is consumed below.
		if (LoadedOptionalCount != ARRAY_COUNT(OPTIONAL_WEAPON_SKIN_ASSETS))
		{
			for (int32 Index = 0; Index < ARRAY_COUNT(OPTIONAL_WEAPON_SKIN_ASSETS); ++Index)
			{
				const FString ObjectPath =
					GetWeaponSkinObjectPath(OPTIONAL_WEAPON_SKIN_ASSETS[Index]);
				if (!NewCatalog.Contains(ObjectPath))
				{
					UE_LOG(LogUTWeaponFix, Warning,
						TEXT("Weapon skin manifest optional entry missing: %s"),
						*ObjectPath);
				}
			}
		}
		PreloadedWeaponSkinCatalog = MoveTemp(NewCatalog);
		bWeaponSkinCatalogReady = true;
		bWeaponSkinCatalogComplete =
			LoadedOptionalCount == ARRAY_COUNT(OPTIONAL_WEAPON_SKIN_ASSETS);
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
	// Gate the rescan on COMPLETE, not ready: a ready-but-incomplete catalog keeps
	// refreshing here so an optional skin whose PAK mounts late can still join.
	const int32 CatalogSkinCount = bSkinsEnabled
		? (bWeaponSkinCatalogComplete
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
	bWeaponSkinCatalogComplete = false;
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
    NextDelayedFlakReservationId = 1;
    MouseDebounceWindow = 0.030f;  // 30ms — mouse-bounce / scroll-wheel coalesce
    bBufferedClickPending[0] = false;
    bBufferedClickPending[1] = false;
    AppliedFPSMaterialSlotMask = 0u;
    bCapturedOriginalFPSMaterials = false;
    FirstPersonHologramDepthMesh = nullptr;
    bFirstPersonHologramSkinActive = false;
    ShockInputTraceInputComponent = nullptr;
    ShockInputTraceController = nullptr;
    ShockInputTraceActionComponent = nullptr;
	bShockInputTraceDeferredSnapshotValid = false;
	bShockInputTraceHadDeferredStartBeforeDown = false;

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

void AUTWeaponFix::RefreshShockInputTrace()
{
	// This diagnostic is deliberately client-local and shock-primary only. The
	// cast includes Blueprint instagib-rifle children whose native parent is
	// AUTPlusShockRifle. No server, RPC, or weapon-state path is touched.
	const bool bShouldTrace = NCShockInputTrace::GetMode() > 0
		&& GetNetMode() != NM_DedicatedServer
		&& Cast<AUTPlusShockRifle>(this) != nullptr
		&& UTOwner != nullptr
		&& UTOwner->IsLocallyControlled()
		&& UTOwner->GetWeapon() == this;
	AUTPlayerController* PC = bShouldTrace
		? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;

	if (!bShouldTrace || PC == nullptr || PC->InputComponent == nullptr)
	{
		StopShockInputTrace();
		return;
	}
	if (ShockInputTraceInputComponent != nullptr
		&& ShockInputTraceController == PC
		&& ShockInputTraceActionComponent == PC->InputComponent)
	{
		return;
	}

	StopShockInputTrace();

	// Refuse to install the post-action observers unless the stock PC bindings
	// are already present. Appending is what guarantees OnFire/OnStopFire run
	// first, letting the observer verify their deferred-queue result without
	// wrapping or replacing gameplay input.
	bool bHasStockStart = false;
	bool bHasStockStop = false;
	for (int32 Index = 0; Index < PC->InputComponent->GetNumActionBindings(); ++Index)
	{
		const FInputActionBinding& Binding = PC->InputComponent->GetActionBinding(Index);
		if (Binding.ActionName == FName(TEXT("StartFire"))
			&& Binding.KeyEvent == IE_Pressed
			&& Binding.ActionDelegate.IsBoundToObject(PC))
		{
			bHasStockStart = true;
		}
		else if (Binding.ActionName == FName(TEXT("StopFire"))
			&& Binding.KeyEvent == IE_Released
			&& Binding.ActionDelegate.IsBoundToObject(PC))
		{
			bHasStockStop = true;
		}
	}
	if (!bHasStockStart || !bHasStockStop)
	{
		return;
	}

	ShockInputTraceController = PC;
	ShockInputTraceActionComponent = PC->InputComponent;
	ShockInputTraceInputComponent = NewObject<UInputComponent>(
		PC, UInputComponent::StaticClass(), NAME_None, RF_Transient);
	if (ShockInputTraceInputComponent == nullptr)
	{
		ShockInputTraceController = nullptr;
		ShockInputTraceActionComponent = nullptr;
		return;
	}
	ShockInputTraceInputComponent->Priority = MAX_int32;
	ShockInputTraceInputComponent->bBlockInput = false;
	ShockInputTraceInputComponent->RegisterComponent();

	FInputKeyBinding& DownBinding = ShockInputTraceInputComponent->BindKey(
		EKeys::LeftMouseButton, IE_Pressed, this,
		&AUTWeaponFix::ShockInputTracePlayerDown);
	DownBinding.bConsumeInput = false;
	DownBinding.bExecuteWhenPaused = false;
	FInputKeyBinding& UpBinding = ShockInputTraceInputComponent->BindKey(
		EKeys::LeftMouseButton, IE_Released, this,
		&AUTWeaponFix::ShockInputTracePlayerUp);
	UpBinding.bConsumeInput = false;
	UpBinding.bExecuteWhenPaused = false;
	PC->PushInputComponent(ShockInputTraceInputComponent);

	FInputActionBinding& StartObserver = ShockInputTraceActionComponent->BindAction(
		TEXT("StartFire"), IE_Pressed, this,
		&AUTWeaponFix::ShockInputTraceActionStart);
	StartObserver.bConsumeInput = false;
	StartObserver.bExecuteWhenPaused = false;
	FInputActionBinding& StopObserver = ShockInputTraceActionComponent->BindAction(
		TEXT("StopFire"), IE_Released, this,
		&AUTWeaponFix::ShockInputTraceActionStop);
	StopObserver.bConsumeInput = false;
	StopObserver.bExecuteWhenPaused = false;

	NCShockInputTrace::Start(this);
}

void AUTWeaponFix::StopShockInputTrace()
{
	if (ShockInputTraceInputComponent == nullptr
		&& ShockInputTraceController == nullptr
		&& ShockInputTraceActionComponent == nullptr)
	{
		return;
	}

	NCShockInputTrace::Stop(this);
	AUTPlayerController* PC = ShockInputTraceController;
	UInputComponent* ActionComponent = ShockInputTraceActionComponent;
	if (ActionComponent != nullptr)
	{
		// Remove only the two observers installed above. Reverse traversal keeps
		// indices valid and UE4.15's paired-action bookkeeping intact.
		for (int32 Index = ActionComponent->GetNumActionBindings() - 1;
			Index >= 0; --Index)
		{
			const FInputActionBinding& Binding =
				ActionComponent->GetActionBinding(Index);
			const bool bOurAction = (Binding.ActionName == FName(TEXT("StartFire"))
					&& Binding.KeyEvent == IE_Pressed)
				|| (Binding.ActionName == FName(TEXT("StopFire"))
					&& Binding.KeyEvent == IE_Released);
			if (bOurAction && Binding.ActionDelegate.IsBoundToObject(this))
			{
				ActionComponent->RemoveActionBinding(Index);
			}
		}
	}
	if (PC != nullptr && ShockInputTraceInputComponent != nullptr)
	{
		PC->PopInputComponent(ShockInputTraceInputComponent);
	}
	if (ShockInputTraceInputComponent != nullptr)
	{
		ShockInputTraceInputComponent->DestroyComponent();
	}
	ShockInputTraceInputComponent = nullptr;
	ShockInputTraceController = nullptr;
	ShockInputTraceActionComponent = nullptr;
	bShockInputTraceDeferredSnapshotValid = false;
	bShockInputTraceHadDeferredStartBeforeDown = false;
}

void AUTWeaponFix::ShockInputTracePlayerDown()
{
	// This component has MAX_int32 priority and does not consume the key, so this
	// snapshot runs before AUTPlayerController::OnFire appends its queue entry.
	bShockInputTraceDeferredSnapshotValid = ShockInputTraceController != nullptr;
	bShockInputTraceHadDeferredStartBeforeDown = ShockInputTraceController
		&& ShockInputTraceController->HasDeferredFireInputs();
	NCShockInputTrace::RecordPlayerInput(this, true);
}

void AUTWeaponFix::ShockInputTracePlayerUp()
{
	NCShockInputTrace::RecordPlayerInput(this, false);
	bShockInputTraceDeferredSnapshotValid = false;
}

void AUTWeaponFix::ShockInputTraceActionStart(FKey Key)
{
	if (Key != EKeys::LeftMouseButton || ShockInputTraceController == nullptr)
	{
		return;
	}
	// The queue itself is protected in UE4.15. Comparing the public before/after
	// predicate proves this click introduced a start only when the queue did not
	// already contain one. A pre-existing start makes the evidence ambiguous; do
	// not turn that ambiguity into either a match or a chain-gap accusation.
	const bool bHasDeferredStartAfter =
		ShockInputTraceController->HasDeferredFireInputs();
	const bool bQueueEvidenceConclusive =
		bShockInputTraceDeferredSnapshotValid
		&& !bShockInputTraceHadDeferredStartBeforeDown;
	const bool bMatched = bQueueEvidenceConclusive && bHasDeferredStartAfter;
	NCShockInputTrace::RecordAction(this, true, bMatched,
		bQueueEvidenceConclusive, -1);
	bShockInputTraceDeferredSnapshotValid = false;
}

void AUTWeaponFix::ShockInputTraceActionStop(FKey Key)
{
	if (Key != EKeys::LeftMouseButton || ShockInputTraceController == nullptr)
	{
		return;
	}
	// There is no public UE4.15 query for a queued StopFire entry. Record the
	// post-stock action boundary without pretending the protected tail was read;
	// the diagnostic's loss classification is intentionally based on StartFire.
	NCShockInputTrace::RecordAction(this, false, false, false, -1);
}

bool AUTWeaponFix::ShouldDrawFFIndicator(APlayerController* Viewer,
    AUTPlayerState*& HitPlayerState) const
{
    bool bDrawIndicator = false;
    if (FriendlyTargetProbeCache.TryReuse(Viewer, UTOwner, HitPlayerState,
        bDrawIndicator))
    {
        return bDrawIndicator;
    }

    bDrawIndicator = Super::ShouldDrawFFIndicator(Viewer, HitPlayerState);
    FriendlyTargetProbeCache.Store(Viewer, UTOwner, HitPlayerState, bDrawIndicator);
    return bDrawIndicator;
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

    // Lazily ensure the world's clock-sync beacon (server only). Weapons exist
    // in every hub mode from warmup onward — including stock TDM, where no
    // NetcodePlus game-mode code ever runs — so this is the one hook that
    // covers them all.
    if (Role == ROLE_Authority)
    {
        ANCPClockSync::Ensure(GetWorld());
    }

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
	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f;
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] RETRY_CALLBACK frame=%u t=%.4f role=%d net=%d local=%d mode=%d state=%s tracker=%d pending0=%d retryRemain=%.4f deferredRemain=%.4f lft0=%.4f earliest=%.4f"),
			(uint32)GFrameCounter, Now, (int32)Role, (int32)GetNetMode(),
			(UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0, FireModeNum,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, (UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			FireModeNum < 2 ? GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[FireModeNum]) : -1.f,
			GetWorldTimerManager().GetTimerRemaining(DeferredActiveStateHandle),
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f, EarliestFireTime);
	}
    UE_LOG(LogUTWeaponFix, Verbose, TEXT("[OnRetryTimer] Mode %d: Retry firing — calling StartFire"), FireModeNum);
    if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] OnRetryTimer mode=%d -> StartFire"), FireModeNum);
    StartFire(FireModeNum);
    bHandlingRetry = false;
}


void AUTWeaponFix::OnBufferedClickRetryTimer(uint8 FireModeNum, FRotator ReleaseAim, float ReleaseTime)
{
    UWorld* World = GetWorld();
    const float BufferWindow = GetClickBufferWindowSeconds();
    const float Now = World ? World->GetTimeSeconds() : -1.0f;
    const float SnapshotAge = (Now >= 0.0f && ReleaseTime >= 0.0f) ? (Now - ReleaseTime) : BIG_NUMBER;

    // A cancelled/replaced delegate must not clear the PendingFire belonging to
    // newer input. Only the callback that still owns the buffered flag may
    // mutate player or retry state.
    if (FireModeNum >= 2 || !bBufferedClickPending[FireModeNum]
        || !IsShockPrimaryClickBuffer(this, FireModeNum))
    {
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[FireDbg] BufferedShock IGNORE mode=%d reason=payload_no_longer_owned"),
                FireModeNum);
        }
        return;
    }

    const bool bOwnerStillValid = UTOwner != nullptr
        && !UTOwner->IsPendingKillPending()
        && !UTOwner->IsDead()
        && !UTOwner->IsFiringDisabled()
        && UTOwner->GetWeapon() == this
        && UTOwner->GetPendingWeapon() == nullptr;
    const bool bSnapshotStillFresh = SnapshotAge >= 0.0f
        && SnapshotAge <= FMath::Min(MaxBufferedAimAgeSeconds,
            BufferWindow + ClickBufferDispatchSlackSeconds);

    if (World == nullptr || BufferWindow <= 0.0f
        || ReleaseAim.ContainsNaN() || !bOwnerStillValid || !bSnapshotStillFresh)
    {
        if (World != nullptr && FireModeNum < 2)
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
        }
        if (FireModeNum < 2)
        {
            bBufferedClickPending[FireModeNum] = false;
        }
        if (UTOwner != nullptr && UTOwner->GetWeapon() == this
            && UTOwner->GetPendingWeapon() == nullptr && FireModeNum < 2)
        {
            UTOwner->SetPendingFire(FireModeNum, false);
        }
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[FireDbg] BufferedShock CANCEL mode=%d ageMs=%.2f ownerValid=%d fresh=%d windowMs=%.2f"),
                FireModeNum, SnapshotAge * 1000.0f, bOwnerStillValid ? 1 : 0,
                bSnapshotStillFresh ? 1 : 0, BufferWindow * 1000.0f);
        }
        return;
    }

    ReleaseAim.Normalize();
    const FRotator CurrentAim = UTOwner->GetViewRotation();
    const float AimDot = FVector::DotProduct(ReleaseAim.Vector(), CurrentAim.Vector());
    const float AimDeltaDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(AimDot, -1.0f, 1.0f)));
    if (FireDbg())
    {
        UE_LOG(LogUTWeaponFix, Warning,
            TEXT("[FireDbg] BufferedShock DISPATCH mode=%d ageMs=%.2f aimDeltaDeg=%.3f releaseAim=%s"),
            FireModeNum, SnapshotAge * 1000.0f, AimDeltaDegrees, *ReleaseAim.ToString());
    }

    // Stage the release direction only for this synchronous logical shot. The
    // timestamp, muzzle, target geometry, and hit claim are still computed now.
    bHandlingRetry = true;
    CachedTransactionalRotation = ReleaseAim;
    ScopedTransactionalAimWeapons.Add(this);
    StartFire(FireModeNum);
    ScopedTransactionalAimWeapons.Remove(this);
    CachedTransactionalRotation = FRotator::ZeroRotator;

    if (!bBufferedClickPending[FireModeNum])
    {
        // FireShot marked the queued click committed. The physical button is
        // already up, so end the sequence without turning this synthetic stop
        // into a new release or another buffered click.
        if (UTOwner != nullptr && UTOwner->GetWeapon() == this
            && UTOwner->GetPendingWeapon() == nullptr)
        {
            StopFireInternal(FireModeNum);
        }
        bHandlingRetry = false;
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[FireDbg] BufferedShock COMMIT mode=%d ageMs=%.2f"),
                FireModeNum, SnapshotAge * 1000.0f);
        }
        return;
    }

    if (GetWorldTimerManager().IsTimerActive(RetryFireHandle[FireModeNum]))
    {
        if (UTOwner == nullptr || UTOwner->IsPendingKillPending() || UTOwner->IsDead()
            || UTOwner->GetWeapon() != this || UTOwner->GetPendingWeapon() != nullptr)
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            bBufferedClickPending[FireModeNum] = false;
            bHandlingRetry = false;
            if (FireDbg())
            {
                UE_LOG(LogUTWeaponFix, Warning,
                    TEXT("[FireDbg] BufferedShock CANCEL mode=%d reason=owner_changed_during_dispatch"),
                    FireModeNum);
            }
            return;
        }

        // A float-boundary check re-armed the generic retry. Preserve the
        // original physical release payload instead of sampling newer aim.
        const float Remaining = GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[FireModeNum]);
        FTimerDelegate BufferedRetry;
        BufferedRetry.BindUObject(this, &AUTWeaponFix::OnBufferedClickRetryTimer,
            FireModeNum, ReleaseAim, ReleaseTime);
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
        GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], BufferedRetry,
            (Remaining > 0.0f) ? Remaining : 0.001f, false);
        UTOwner->SetPendingFire(FireModeNum, false);
        bHandlingRetry = false;
        if (FireDbg())
        {
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[FireDbg] BufferedShock REARM mode=%d remainingMs=%.2f originalAgeMs=%.2f"),
                FireModeNum, Remaining * 1000.0f, SnapshotAge * 1000.0f);
        }
        return;
    }

    // StartFire returned without committing and without a legal retry path.
    bBufferedClickPending[FireModeNum] = false;
    if (UTOwner != nullptr && UTOwner->GetWeapon() == this
        && UTOwner->GetPendingWeapon() == nullptr)
    {
        UTOwner->SetPendingFire(FireModeNum, false);
    }
    bHandlingRetry = false;
    if (FireDbg())
    {
        UE_LOG(LogUTWeaponFix, Warning,
            TEXT("[FireDbg] BufferedShock CANCEL mode=%d reason=no_commit_no_retry ageMs=%.2f"),
            FireModeNum, SnapshotAge * 1000.0f);
    }
}









void AUTWeaponFix::StartFire(uint8 FireModeNum)
{
	if (FireModeNum == 0 && ShockInputTraceInputComponent != nullptr
		&& Cast<AUTPlusShockRifle>(this) != nullptr)
	{
		UWorld* TraceWorld = GetWorld();
		const float Now = TraceWorld ? TraceWorld->GetTimeSeconds() : 0.f;
		float ReadyAt = EarliestFireTime;
		for (int32 Mode = 0; Mode < LastFireTime.Num(); ++Mode)
		{
			if (LastFireTime[Mode] > 0.f)
			{
				ReadyAt = FMath::Max(ReadyAt,
					LastFireTime[Mode] + GetRefireTime(Mode));
			}
		}
		const float ReadyInMs = FMath::Max(0.f, (ReadyAt - Now) * 1000.f);
		float TraceDebounceWindow = MouseDebounceWindow;
		const float TraceDebounceCap = CVarMouseDebounceCap.GetValueOnGameThread();
		if (TraceDebounceCap >= 0.f)
		{
			TraceDebounceWindow = FMath::Min(TraceDebounceWindow,
				TraceDebounceCap);
		}
		const float SinceRelease = LastReleaseTime.IsValidIndex(0)
			&& LastReleaseTime[0] > 0.f ? Now - LastReleaseTime[0] : -1.f;
		const bool bDebounceWillQueue = !bHandlingRetry
			&& TraceDebounceWindow > 0.f && SinceRelease >= 0.f
			&& SinceRelease < TraceDebounceWindow;
		AUTGameState* TraceGS = TraceWorld
			? TraceWorld->GetGameState<AUTGameState>() : nullptr;
		const bool bMovementBlocks = UTOwner && bRootWhileFiring
			&& UTOwner->GetCharacterMovement()
			&& UTOwner->GetCharacterMovement()->MovementMode == MOVE_Falling;
		const bool bExpectedImmediateShot = UTOwner != nullptr
			&& !UTOwner->IsPendingKillPending()
			&& UTOwner->GetWeapon() == this
			&& UTOwner->GetPendingWeapon() == nullptr
			&& HasAmmo(0)
			&& FiringState.IsValidIndex(0) && FiringState[0] != nullptr
			&& CurrentState == ActiveState
			&& !UTOwner->IsFiringDisabled()
			&& !bMovementBlocks
			&& (TraceGS == nullptr || !TraceGS->PreventWeaponFire())
			&& !bDebounceWillQueue
			&& ReadyInMs <= 1.f;
		NCShockInputTrace::RecordWeaponStart(this, bHandlingRetry,
			bExpectedImmediateShot, ReadyInMs,
			GetCurrentState() ? GetCurrentState()->GetFName() : NAME_None);
	}

	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f;
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] START_INPUT frame=%u t=%.4f role=%d net=%d local=%d mode=%d retry=%d state=%s tracker=%d active0=%d pending0=%d pending1=%d lft0=%.4f refire=%.4f earliest=%.4f retryRemain=%.4f deferredRemain=%.4f"),
			(uint32)GFrameCounter, Now, (int32)Role, (int32)GetNetMode(),
			(UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0, FireModeNum, bHandlingRetry ? 1 : 0,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			(UTOwner && UTOwner->IsPendingFire(1)) ? 1 : 0,
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f, GetRefireTime(0), EarliestFireTime,
			GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[0]),
			GetWorldTimerManager().GetTimerRemaining(DeferredActiveStateHandle));
	}

    if (FireDbg())
    {
        UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] StartFire mode=%d curFiring=%d state=%s"),
            FireModeNum, CurrentlyFiringMode,
            GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"));
    }

    // Any fresh physical press supersedes a previously released buffered click,
    // including a new secondary press while primary was queued. Do this before
    // debounce: otherwise debounce can clear the flag but leave the old payload
    // delegate alive, which later consumes the newly held intent.
    if (!bHandlingRetry && UTOwner && UTOwner->IsLocallyControlled())
    {
        for (int32 BufferedMode = 0; BufferedMode < 2; ++BufferedMode)
        {
            if (bBufferedClickPending[BufferedMode])
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[BufferedMode]);
                bBufferedClickPending[BufferedMode] = false;
                if (FireDbg())
                {
                    UE_LOG(LogUTWeaponFix, Warning,
                        TEXT("[FireDbg] BufferedShock CANCEL mode=%d reason=fresh_press newMode=%d"),
                        BufferedMode, FireModeNum);
                }
            }
        }
    }

	// Equip-lifetime ownership must be resolved before zoom, debounce, or charged
	// weapon special cases. Those paths return early and previously let input
	// mutate the outgoing sniper/rocket after a switch had already begun.
	if (!UTOwner || UTOwner->IsPendingKillPending() || UTOwner->GetWeapon() != this
		|| CurrentState == nullptr || CurrentState == InactiveState)
	{
		return;
	}

	AUTWeapon* PendingWeapon = UTOwner->GetPendingWeapon();
	const bool bIsSwitchingAway = CurrentState == UnequippingState
		|| (PendingWeapon != nullptr && PendingWeapon != this);
	if (bIsSwitchingAway)
	{
		// A cooldown/buffer retry belongs to this weapon's old equip lifetime. It
		// must not manufacture held input for the incoming weapon.
		if (bHandlingRetry)
		{
			UE_LOG(LogUTWeaponFix, Verbose,
				TEXT("Discarding stale fire retry during weapon swap (mode %d)"), FireModeNum);
			return;
		}

		// This branch latches input without calling Super::StartFire, so retain
		// stock's preflight restrictions before handing the physical press across.
		if (UTOwner->IsFiringDisabled())
		{
			return;
		}
		if (bRootWhileFiring && UTOwner->GetCharacterMovement()
			&& UTOwner->GetCharacterMovement()->MovementMode == MOVE_Falling)
		{
			return;
		}
		AUTGameState* SwitchGS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
		if (SwitchGS && SwitchGS->PreventWeaponFire())
		{
			return;
		}

		if (GhostFix() && FireModeNum < 2 && UTOwner->IsLocallyControlled())
		{
			bFireHeldByPlayer[FireModeNum] = true;
		}
		UTOwner->SetPendingFire(FireModeNum, true);
		UE_LOG(LogUTWeaponFix, Verbose,
			TEXT("Deferring physical fire input across weapon swap (mode %d)"), FireModeNum);
		return;
	}

    // ---------------------------------------------------------
	// ZOOM BYPASS (BEFORE COOLDOWN, AFTER EQUIP OWNERSHIP)
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
    if (!bHandlingRetry && UTOwner && UTOwner->IsLocallyControlled() &&
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
            // R4 (2026-08-06): queue the press instead of eating it. The bare
            // early-return relied on the pending flag being honoured later, but
            // DeferredGotoActiveState deliberately clears it — a chatter pair
            // mid-hold left the gun silent until a fresh physical click. Arm the
            // same retry the cooldown path arms so this press fires at the next
            // legal time; a genuine release cancels it (or converts it to a
            // buffered click) in StopFire exactly like any queued press.
            if (FireModeNum < 2)
            {
                // GhostFix held-intent: this early return consumes the press, so
                // record the physical hold HERE — a weapon switch during the
                // debounce window otherwise graduates a stale false flag in
                // PutDown. A genuine release still clears it in StopFire.
                if (GhostFix())
                {
                    bFireHeldByPlayer[FireModeNum] = true;
                }
                bBufferedClickPending[FireModeNum] = false;   // this press owns the timer now
                if (!GetWorldTimerManager().IsTimerActive(RetryFireHandle[FireModeNum]))
                {
                    float MaxReadyTime = 0.f;
                    for (int32 i = 0; i < LastFireTime.Num(); i++)
                    {
                        if (LastFireTime[i] > 0.0f)
                        {
                            MaxReadyTime = FMath::Max(MaxReadyTime, LastFireTime[i] + GetRefireTime(i));
                        }
                    }
                    MaxReadyTime = FMath::Max(MaxReadyTime, EarliestFireTime);
                    const float Delay = MaxReadyTime - GetWorld()->GetTimeSeconds();
                    FTimerDelegate RetryDel;
                    RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                    // Exact delay, no slack — timers never fire early; a tiny
                    // handle-bound rate (not SetTimerForNextTick) keeps the
                    // due-now case cancellable by StopFire.
                    GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel,
                        (Delay > 0.f) ? Delay : 0.001f, false);
                    bCrossModeRetryArmed[FireModeNum] = false;
                }
            }
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
                // Mode-cycle only on the PHYSICAL press — a retry re-entry landing
                // during loading must not phantom-cycle the rocket type (mirrors
                // the cross-mode block's bHandlingRetry guard).
                if (!bHandlingRetry)
                {
                    OnMultiPress(FireModeNum);
                }
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
	// A fresh physical press owns the input from here — a previously buffered
	// (released) click must not double-fire behind it.
	if (!bHandlingRetry && FireModeNum < 2)
	{
		bBufferedClickPending[FireModeNum] = false;
	}

	if (GhostFix() && !bHandlingRetry && FireModeNum < 2 && UTOwner && UTOwner->IsLocallyControlled())
	{
		bFireHeldByPlayer[FireModeNum] = true;
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
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			const float Lft = LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f;
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] START_COOLDOWN frame=%u t=%.4f role=%d local=%d state=%s ownsState=%d pending0=%d lft0=%.4f since=%.4f refire=%.4f earliestRemain=%.4f deferredRemain=%.4f"),
				(uint32)GFrameCounter, CurrentTime, (int32)Role,
				(UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				(FiringState.IsValidIndex(0) && GetCurrentState() == FiringState[0]) ? 1 : 0,
				(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, Lft,
				Lft > 0.f ? CurrentTime - Lft : -1.f, GetRefireTime(0), EarliestFireTime - CurrentTime,
				GetWorldTimerManager().GetTimerRemaining(DeferredActiveStateHandle));
		}

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

            // Exact-delay arm (2026-08-06): timers never fire early, so the old
            // +10ms slack only made every rescued click land 1-2 frames late at
            // high fps. If float-boundary timing re-blocks the shot, StartFire
            // re-arms through this same path and costs one frame, not 10ms. The
            // due-now case uses a tiny handle-bound rate (not SetTimerForNextTick)
            // so a release can still cancel it.
            FTimerDelegate RetryDel;
            RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
            GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel,
                (Delay > 0.f) ? Delay : 0.001f, false);
            if (FireModeNum < 2) { bCrossModeRetryArmed[FireModeNum] = false; }   // same-mode arm owns the handle now
			if (RocketPrimaryDiagFor(this, FireModeNum))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] START_RETRY_ARMED frame=%u t=%.4f mode=%d computedDelay=%.4f timerRate=%.4f timerRemain=%.4f pending0=%d"),
					(uint32)GFrameCounter, CurrentTime, FireModeNum, Delay,
					GetWorldTimerManager().GetTimerRate(RetryFireHandle[FireModeNum]),
					GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[FireModeNum]),
					(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0);
			}
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
			StopFireInternal(CurrentlyFiringMode);
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
			if (RocketPrimaryDiagFor(this, FireModeNum))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] START_ALREADY_FIRING_RETURN frame=%u t=%.4f mode=%d state=%s pending0=%d deferredActive=0"),
					(uint32)GFrameCounter, CurrentTime, FireModeNum,
					GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
					(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0);
			}
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

	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] START_DISPATCH frame=%u t=%.4f role=%d net=%d mode=%d state=%s tracker=%d active0=%d pending0=%d trans=%d"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			(int32)Role, (int32)GetNetMode(), FireModeNum,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, bIsTransactionalFire ? 1 : 0);
	}
	BeginFiringSequence(FireModeNum, false);

	if (Role == ROLE_Authority)
	{
		bIsTransactionalFire = false;
	}
}






void AUTWeaponFix::FireShot()
{
	if (CurrentFireMode == 0 && ShockInputTraceInputComponent != nullptr
		&& Cast<AUTPlusShockRifle>(this) != nullptr)
	{
		NCShockInputTrace::RecordFireShot(this,
			GetCurrentState() ? GetCurrentState()->GetFName() : NAME_None);
	}

	const uint8 BufferedDispatchMode = CurrentFireMode;
	const bool bBufferedShockDispatch = BufferedDispatchMode < 2
		&& bBufferedClickPending[BufferedDispatchMode]
		&& IsShockPrimaryClickBuffer(this, BufferedDispatchMode)
		&& HasScopedTransactionalAim(this);

	if (RocketPrimaryDiagFor(this, CurrentFireMode))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] FIRE_SHOT_ENTER frame=%u t=%.4f role=%d net=%d local=%d currentMode=%d tracker=%d state=%s pending0=%d active0=%d trans=%d delayed=%d lft0=%.4f"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			(int32)Role, (int32)GetNetMode(), (UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0,
			CurrentFireMode, CurrentlyFiringMode,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			bIsTransactionalFire ? 1 : 0, bNetDelayedShot ? 1 : 0,
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f);
	}

	// --- REPLAY PLAYBACK: skip all NC prediction/rewind, use stock behavior ---
	// During instant replay, there's no server to do the rewind dance with.
	// Fake projectile handoff, ServerStartFireFixed RPCs, and ClientHitChar
	// prediction all break in replay context. Stock FireShot just spawns the
	// visual projectile directly which is all replay needs.
	UWorld* ReplayWorld = GetWorld();
	if (ReplayWorld && ReplayWorld->DemoNetDriver && ReplayWorld->DemoNetDriver->IsPlaying())
	{
		Super::FireShot();
		if (bBufferedShockDispatch)
		{
			bBufferedClickPending[BufferedDispatchMode] = false;
			ScopedTransactionalAimWeapons.Remove(this);
			CachedTransactionalRotation = FRotator::ZeroRotator;
		}
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
		FRotator ClientRot = bBufferedShockDispatch
			? CachedTransactionalRotation
			: (GetUTOwner() ? GetUTOwner()->GetViewRotation() : FRotator::ZeroRotator);
		if (bBufferedShockDispatch)
		{
			ClientRot.Normalize();
		}

		// A buffered Shock shot was scoped by its payload callback before this
		// pretrace. Normal shots retain their existing timing path.
		if (bBufferedShockDispatch)
		{
			CachedTransactionalRotation = ClientRot;
			ScopedTransactionalAimWeapons.Add(this);
		}
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
		float AttribVisualMissBy = 0.0f;
		bool bHighConfidenceHitsoundGeometry = false;
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

				if (ClientHitChar != nullptr)
				{
					const FVector AudibleTraceEnd = GetPredictedHitsoundTraceEnd(
						World, UTOwner, SpawnLocation, EndTrace,
						InstantHitInfo[CurrentFireMode].TraceHalfSize);
					FVector AudibleVisualOffset = FVector::ZeroVector;
					float AudibleVisualMissBy = 0.0f;
					const bool bAudibleCapsuleHit = ShotIntersectsRenderedCapsule(
						ClientHitChar, SpawnLocation, AudibleTraceEnd,
						InstantHitInfo[CurrentFireMode].TraceHalfSize,
						AudibleVisualOffset, AudibleVisualMissBy);
					bHighConfidenceHitsoundGeometry = bAudibleCapsuleHit &&
						AudibleVisualMissBy <= -PredictedHitsoundCapsuleInset;
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

		// Client-side hitsound prediction for hitscan weapons.
		// Team-aware: predicting the ENEMY cue for a teammate (who on a no-FF
		// server takes no damage at all) is worse feedback than silence.
		if (ClientHitChar != nullptr && Role != ROLE_Authority &&
			bHighConfidenceHitsoundGeometry &&
			IsHighConfidencePredictedHitsoundTarget(ClientHitChar))
		{
			AClientHitsounds* HitsoundsMut = FindClientHitsoundsMutator();
			if (HitsoundsMut)
			{
				AUTGameState* HitsoundGS = GetWorld()->GetGameState<AUTGameState>();
				const bool bFriendlyTarget = HitsoundGS && HitsoundGS->OnSameTeam(UTOwner, ClientHitChar);
				// A shot that is sending a head claim predicts the headshot
				// damage (AUTPlusSniper override), not the base damage — a
				// sniper headshot must not sound like a bodyshot. If the server
				// rejects or demotes the claim, the authoritative sound plays
				// through the dedup window as the correction (different tier).
				int32 EstDamage = GetPredictedHitsoundDamage(CurrentFireMode, !ClientHeadOffset.IsZero());
				// Mirror the server broadcast's amp scaling (GetScaledDamage):
				// it reports DamageScaling * damage, and an uncompensated
				// prediction would register as a different tier on every amped
				// bullet and re-play through the correction path.
				if (UTOwner != nullptr)
				{
					EstDamage = FMath::TruncToInt(UTOwner->DamageScaling * (float)EstDamage);
				}
				if (bFriendlyTarget || EstDamage > 0)
				{
					HitsoundsMut->PlayClientPredictedHitsound(EstDamage, bFriendlyTarget);
				}
			}
		}

		const float ClientTimestamp = GetWorld()->GetGameState()->GetServerWorldTimeSeconds();
		if (RocketPrimaryDiagFor(this, CurrentFireMode))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] CLIENT_SEND frame=%u t=%.4f serverT=%.4f event=%d mode=%d state=%s pending0=%d lft0=%.4f refire=%.4f"),
				(uint32)GFrameCounter, GetWorld()->GetTimeSeconds(), ClientTimestamp, NextEventIndex,
				CurrentFireMode, GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
				LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f, GetRefireTime(0));
		}
		ServerStartFireFixed(CurrentFireMode, NextEventIndex, ClientTimestamp,
			ClientRot, ClientHitChar, ZOffset, ClientHeadOffset);
        QueueResendStartFireFixed(CurrentFireMode, NextEventIndex, ClientTimestamp,
            ClientRot, ClientHitChar, ZOffset, ClientHeadOffset);

		// Existing fake-projectile/effect path for non-buffered shots. Buffered
		// Shock already staged the same rotation before its pretrace.
		CachedTransactionalRotation = ClientRot;
		Super::FireShot();
		if (bBufferedShockDispatch)
		{
			// Commit marker consumed by OnBufferedClickRetryTimer.
			bBufferedClickPending[BufferedDispatchMode] = false;
		}
		ScopedTransactionalAimWeapons.Remove(this);
		CachedTransactionalRotation = FRotator::ZeroRotator;
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
			if (RocketPrimaryDiagFor(this, CurrentFireMode))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] SERVER_FIRE_GATE_BLOCK frame=%u t=%.4f state=%s trans=%d delayed=%d listen=%d charged=%d stateFiring=%d pending0=%d tracker=%d"),
					(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
					GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
					bIsTransactionalFire ? 1 : 0, bNetDelayedShot ? 1 : 0, bIsListenServerHost ? 1 : 0,
					bInChargedState ? 1 : 0, bIsStateFiring ? 1 : 0,
					(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, CurrentlyFiringMode);
			}
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
		if (bBufferedShockDispatch)
		{
			// Listen-server local player: same commit contract as the client path.
			bBufferedClickPending[BufferedDispatchMode] = false;
			ScopedTransactionalAimWeapons.Remove(this);
			CachedTransactionalRotation = FRotator::ZeroRotator;
		}
	}
}



void AUTWeaponFix::StopFireInternal(uint8 FireModeNum)
{
	const bool bWasHandlingRetry = bHandlingRetry;
	bHandlingRetry = true;
	StopFire(FireModeNum);
	bHandlingRetry = bWasHandlingRetry;
}


void AUTWeaponFix::StopOwnerFireInternal(uint8 FireModeNum)
{
	const bool bWasHandlingRetry = bHandlingRetry;
	bHandlingRetry = true;
	if (UTOwner != nullptr && UTOwner->GetWeapon() == this)
	{
		// Preserve AUTCharacter's ghost-recording and input cleanup while making
		// it explicit that this stop was game/state driven, not a mouse release.
		UTOwner->StopFire(FireModeNum);
	}
	else
	{
		StopFire(FireModeNum);
	}
	bHandlingRetry = bWasHandlingRetry;
}


void AUTWeaponFix::StopFire(uint8 FireModeNum)
{
	if (FireModeNum == 0 && ShockInputTraceInputComponent != nullptr
		&& Cast<AUTPlusShockRifle>(this) != nullptr)
	{
		NCShockInputTrace::RecordWeaponStop(this, bHandlingRetry,
			GetCurrentState() ? GetCurrentState()->GetFName() : NAME_None);
	}

	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] STOP_INPUT frame=%u t=%.4f role=%d net=%d local=%d mode=%d state=%s tracker=%d active0=%d pending0=%d pending1=%d lft0=%.4f retryRemain=%.4f deferredRemain=%.4f"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			(int32)Role, (int32)GetNetMode(), (UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0,
			FireModeNum, GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			(UTOwner && UTOwner->IsPendingFire(1)) ? 1 : 0,
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f,
			GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[0]),
			GetWorldTimerManager().GetTimerRemaining(DeferredActiveStateHandle));
	}

    if (FireDbg()) UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireDbg] StopFire mode=%d curFiring=%d"), FireModeNum, CurrentlyFiringMode);

    // Mouse-bounce debounce: stamp the release time so the next StartFire
    // within MouseDebounceWindow is recognised as a bounce, not a new click.
    if (!bHandlingRetry && LastReleaseTime.IsValidIndex(FireModeNum))
    {
        LastReleaseTime[FireModeNum] = GetWorld()->GetTimeSeconds();
    }

    if (UTOwner)
    {
		// A genuine release must cancel input even during a switch; otherwise a
		// press+release before equip leaves PendingFire latched and ghost-fires the
		// incoming weapon. Internal state-driven stops use bHandlingRetry and retain
		// the hold-through-switch latch.
		const bool bIsSwitchingWeapons = UTOwner->GetPendingWeapon() != nullptr;
		if (!bIsSwitchingWeapons || !bHandlingRetry)
		{
			UTOwner->SetPendingFire(FireModeNum, false);
			// GHOST FIX prototype: a genuine release ends held intent. Internal stops
			// during a held switch still take the preserving branch above.
            if (GhostFix() && FireModeNum < 2) { bFireHeldByPlayer[FireModeNum] = false; }
            UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Clearing PendingFire %d"), FireModeNum);
        }
    }
    if (FireModeNum < 2)
    {
        // Shock primary dogfood buffer. Eligibility remains RELEASE-to-ready:
        // preserve the raw release-time direction, then execute against the
        // current legal-time world. Rebinding the timer carries the snapshot as
        // delegate payload so the weapon-wide transactional cache stays empty
        // while merely queued.
        const float BufferWindow = GetClickBufferWindowSeconds();
        const bool bReplayPlayback = GetWorld() && GetWorld()->DemoNetDriver
            && GetWorld()->DemoNetDriver->IsPlaying();
        const float Remaining = GetWorldTimerManager().IsTimerActive(RetryFireHandle[FireModeNum])
            ? GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[FireModeNum]) : -1.0f;
        if (!bHandlingRetry
            && IsShockPrimaryClickBuffer(this, FireModeNum)
            && BufferWindow > 0.f
            && UTOwner && UTOwner->IsLocallyControlled()
            && !UTOwner->IsFiringDisabled()
            && UTOwner->GetPendingWeapon() == nullptr
            && !bReplayPlayback
            && !bCrossModeRetryArmed[FireModeNum]
            && GetWorldTimerManager().IsTimerActive(RetryFireHandle[FireModeNum])
            && Remaining <= BufferWindow)
        {
            FRotator ReleaseAim = UTOwner->GetViewRotation();
            ReleaseAim.Normalize();
            if (!ReleaseAim.ContainsNaN())
            {
                const float ReleaseTime = GetWorld()->GetTimeSeconds();
                FTimerDelegate BufferedRetry;
                BufferedRetry.BindUObject(this, &AUTWeaponFix::OnBufferedClickRetryTimer,
                    FireModeNum, ReleaseAim, ReleaseTime);
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], BufferedRetry,
                    (Remaining > 0.0f) ? Remaining : 0.001f, false);
                bBufferedClickPending[FireModeNum] = true;
                if (FireDbg())
                {
                    UE_LOG(LogUTWeaponFix, Warning,
                        TEXT("[FireDbg] StopFire BUFFERED ShockPrimary mode=%d remainMs=%.2f releaseAim=%s"),
                        FireModeNum, Remaining * 1000.0f, *ReleaseAim.ToString());
                }
            }
            else
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
                bBufferedClickPending[FireModeNum] = false;
            }
        }
        else
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            bBufferedClickPending[FireModeNum] = false;
        }
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
        // CLIENT: execute locally and retain both 328 Stop transports.
        if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
        {
            // Keep the stock Stop RPC and its retries in 328. Charged Start still
            // uses the stock unreliable/retry family; removing only stock Stop
            // would let a delayed Start retry begin charging after release. The
            // charged state's release latch makes this stock Stop and the fixed
            // Stop below converge on one local/server release commit.
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
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] CLIENT_STOP_SEND frame=%u t=%.4f mode=%d event=%d state=%s pending0=%d deferredRemain=%.4f"),
				(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
				FireModeNum, EventIndex,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				UTOwner->IsPendingFire(0) ? 1 : 0,
				GetWorldTimerManager().GetTimerRemaining(DeferredActiveStateHandle));
		}
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
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_VALIDATE_REJECT reason=BAD_MODE frame=%u t=%.4f mode=%d event=%d"),
				(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
				FireModeNum, InEventIndex);
		}
        return false;
    }

    // Validate event sequence
    if (!IsFireEventSequenceValid(FireModeNum, InEventIndex))
    {
        int32 LastProcessed = AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : -1;
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_VALIDATE_REJECT reason=STALE_OR_JUMP frame=%u t=%.4f mode=%d event=%d authEvent=%d clientT=%.4f"),
				(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
				FireModeNum, InEventIndex, LastProcessed, ClientTime);
		}
        UE_LOG(LogUTWeaponFix, Warning, TEXT("Shot rejected for %s: [Server] STALE EVENT. Mode %d EventIndex %d vs LastProcessed %d"),
            *PlayerName, FireModeNum, InEventIndex, LastProcessed);
        return false;
    }

    // Validate timing with network tolerance
    float ServerTime = GetWorld()->GetTimeSeconds();
    float TimeDiff = FMath::Abs(ServerTime - ClientTime);

    // Stale client clock estimate. ClientTime is the client's
    // GetServerWorldTimeSeconds, which the engine re-syncs only every 5s
    // (GameStateBase ServerWorldTimeSecondsUpdateFrequency) — a pause freezes
    // the server clock while unpaused clients keep counting, so for seconds
    // after any unpause every honest fire RPC arrives with a huge offset (the
    // 13.3s bursts of 2026-08-06). The timestamp is VALIDATION-ONLY today
    // (rewind uses ping, never this value), so a stale clock must not
    // fire-dead the player: accept the shot, log throttled. Unpause paths also
    // force an immediate resync (NCPlusHostPause::ResyncServerWorldTime), so
    // this staying loud means a genuine client hitch or a new unpause path.
    if (TimeDiff > 1.0f)
    {
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_VALIDATE_DESYNC_ACCEPT frame=%u serverT=%.4f clientT=%.4f diff=%.4f mode=%d event=%d"),
				(uint32)GFrameCounter, ServerTime, ClientTime, TimeDiff, FireModeNum, InEventIndex);
		}
        static double LastDesyncWarnTime = 0.0;
        const double NowRT = FPlatformTime::Seconds();
        if (NowRT - LastDesyncWarnTime >= 5.0)
        {
            LastDesyncWarnTime = NowRT;
            UE_LOG(LogUTWeaponFix, Warning,
                TEXT("[TimeDesync] %s fire timestamp off by %.2fs — accepting (validation-only value; likely post-pause clock staleness, throttled 5s)"),
                *PlayerName, TimeDiff);
        }
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
			if (RocketPrimaryDiagFor(this, FireModeNum))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] SERVER_VALIDATE_REJECT reason=EARLY frame=%u t=%.4f mode=%d event=%d authEvent=%d lft=%.4f delta=%.4f min=%.4f refire=%.4f tolerance=%.4f"),
					(uint32)GFrameCounter, ServerTime, FireModeNum, InEventIndex,
					AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : -1,
					LastFireTime[FireModeNum], TimeSinceLastFire, MinInterval, RefireTime, JitterTolerance);
			}
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
	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] SERVER_VALIDATE_ACCEPT frame=%u t=%.4f mode=%d event=%d clientT=%.4f state=%s pending0=%d lft0=%.4f"),
			(uint32)GFrameCounter, ServerTime, FireModeNum, InEventIndex, ClientTime,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f);
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
	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] SERVER_RX frame=%u t=%.4f wep=%p player=%s resend=%d mode=%d event=%d clientT=%.4f authEvent=%d state=%s currentMode=%d tracker=%d active0=%d pending0=%d lft0=%.4f"),
			(uint32)GFrameCounter, World->GetTimeSeconds(), this, RocketPrimaryDiagPlayer(this),
			bNetDelayedShot ? 1 : 0, FireModeNum, InFireEventIndex, ClientTimestamp,
			AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : -1,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentFireMode, CurrentlyFiringMode,
			FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f);
	}

    // Server-authoritative fire policy (e.g. single-rocket-only loadouts). Reject a
    // vetoed mode HERE — before the trade-kill spawn, the SetPendingFire latch below, and
    // any state entry — so a modified client cannot latch PendingFire and let stock
    // UUTWeaponStateActive::BeginState auto-enter the firing state. The resend RPC funnels
    // back through this function, so this covers that path too.
    if (!AllowServerFireMode(FireModeNum))
    {
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_REJECT reason=MODE_POLICY frame=%u t=%.4f mode=%d event=%d"),
				(uint32)GFrameCounter, World->GetTimeSeconds(), FireModeNum, InFireEventIndex);
		}
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

	// Reliable fire RPCs and ServerSwitchWeapon travel on different actors, so
	// their cross-actor arrival order is not guaranteed. Once this weapon no
	// longer owns (or is already leaving) the server equip lifetime, an old Start
	// must not latch pawn-global PendingFire or re-enter an outgoing charged state.
	AUTWeapon* ServerPendingWeapon = UTOwner ? UTOwner->GetPendingWeapon() : nullptr;
	const bool bOwnsServerEquipLifetime = UTOwner != nullptr
		&& UTOwner->GetWeapon() == this
		&& (ServerPendingWeapon == nullptr || ServerPendingWeapon == this)
		&& CurrentState != nullptr
		&& CurrentState != UnequippingState
		&& CurrentState != InactiveState;
	if (!bOwnsServerEquipLifetime)
	{
		UE_LOG(LogUTWeaponFix, Verbose,
			TEXT("[ServerStartFireFixed] Ignoring stale equip-lifetime Start mode=%d event=%d state=%s current=%d pending=%d"),
			FireModeNum, InFireEventIndex,
			GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"),
			(UTOwner && UTOwner->GetWeapon() == this) ? 1 : 0,
			ServerPendingWeapon ? 1 : 0);
		return;
	}

    if (!ValidateFireRequest(FireModeNum, InFireEventIndex, ClientTimestamp))
    {
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_REJECT reason=VALIDATION frame=%u t=%.4f mode=%d event=%d ack=%d state=%s pending0=%d"),
				(uint32)GFrameCounter, World->GetTimeSeconds(), FireModeNum, InFireEventIndex,
				AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : 0,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0);
		}
        ClientConfirmFireEvent(FireModeNum, AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : 0);
        return;
    }
    CachedTransactionalRotation = ClientViewRot;
    if (IsShockPrimaryClickBuffer(this, FireModeNum))
    {
        ScopedTransactionalAimWeapons.Add(this);
    }
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
            const int32 WedgeLoadedRockets = WedgeRL ? FMath::Max(0, WedgeRL->NumLoadedRockets) : 0;
            const int32 WedgeLoadedBarrels = WedgeRL ? FMath::Max(0, WedgeRL->NumLoadedBarrels) : 0;
            if (WedgeLoadedRockets > 0)
            {
                // Only NumLoadedRockets is authoritative gameplay work owed to the
                // player. NumLoadedBarrels tracks loading/visual state and can remain
                // stale after a spread volley; it is not an unfired-projectile count.
                // A real loaded wedge must preserve and release its logical rockets
                // through the state's OWN burst path, which arms
                // FireLoadedRocketHandle so the state is busy (un-wedged) immediately. This
                // Start then proceeds to normal dispatch below, is recorded as the stock
                // PendingFireSequence with pawn PendingFire latched (set above), and the
                // charged RefireCheckTimer's post-burst primary handoff fires it AFTER the
                // volley — never both in the same frame.
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s (%s) LOADED wedged ChargedRocket (loadedR=%d loadedB=%d) — force-releasing volley; incoming Start mode=%d defers to post-burst"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    WedgeLoadedRockets, WedgeLoadedBarrels, FireModeNum);
                ChargedWedgeFirstSeenTime = -1.f;
                WedgedChg->RecoverWedgedRelease();
            }
            else
            {
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s (%s) EMPTY wedged ChargedRocket (loadedR=0 loadedB=%d) recovered by incoming Start mode=%d — firing it instead of swallowing"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    WedgeLoadedBarrels, FireModeNum);
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
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_DISPATCH_EXISTING frame=%u t=%.4f mode=%d event=%d state=%s pending0=%d tracker=%d"),
				(uint32)GFrameCounter, World->GetTimeSeconds(), FireModeNum, InFireEventIndex,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, CurrentlyFiringMode);
		}
        // STATE IS ACTIVE: Just trigger the next shot in the sequence.
        TransState->TransactionalFire();
    }
    else
    {
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_DISPATCH_ENTER frame=%u t=%.4f mode=%d event=%d state=%s currentMode=%d transState=%d pending0=%d tracker=%d"),
				(uint32)GFrameCounter, World->GetTimeSeconds(), FireModeNum, InFireEventIndex,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				GetCurrentFireMode(), TransState ? 1 : 0,
				(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, CurrentlyFiringMode);
		}
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
	ScopedTransactionalAimWeapons.Remove(this);
	ReceivedHitScanHitChar = nullptr;

    // 4. CONFIRM — always sent, including shock balls. Keeps event indices synced
    // and clears the resend queue. Shock ball fakes are preserved in
    // ClientConfirmFireEvent_Implementation (not destroyed) to prevent the
    // 80+ ping visual hitch. See that function for details.
    if (UTOwner)
    {
		if (RocketPrimaryDiagFor(this, FireModeNum))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] SERVER_ACK_SEND frame=%u t=%.4f wep=%p player=%s mode=%d event=%d state=%s currentMode=%d tracker=%d pending0=%d lft0=%.4f"),
				(uint32)GFrameCounter, World->GetTimeSeconds(), this, RocketPrimaryDiagPlayer(this),
				FireModeNum, InFireEventIndex,
				GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
				CurrentFireMode, CurrentlyFiringMode, UTOwner->IsPendingFire(0) ? 1 : 0,
				LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f);
		}
        ClientConfirmFireEvent(FireModeNum, InFireEventIndex);
    }
}

void AUTWeaponFix::Removed()
{
	StopShockInputTrace();
	// A delayed Flak prediction belongs to this weapon instance. Once it is removed,
	// the authoritative replicated projectile (if any) is the only valid visual source.
	ClearDelayedFlakFakeProjectiles();
	DestroyFirstPersonHologramDepthMesh();

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
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		GetWorldTimerManager().ClearTimer(RetryFireHandle[Mode]);
		bBufferedClickPending[Mode] = false;
	}
	ScopedTransactionalAimWeapons.Remove(this);
	CachedTransactionalRotation = FRotator::ZeroRotator;
	Super::Removed();
}

void AUTWeaponFix::Destroyed()
{
	StopShockInputTrace();
	// Direct replay/admin destruction can bypass normal inventory removal. Break
	// the master-pose relationship before the actor's component teardown starts.
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		GetWorldTimerManager().ClearTimer(RetryFireHandle[Mode]);
		bBufferedClickPending[Mode] = false;
	}
	ScopedTransactionalAimWeapons.Remove(this);
	CachedTransactionalRotation = FRotator::ZeroRotator;
	DestroyFirstPersonHologramDepthMesh();
	Super::Destroyed();
}

bool AUTWeaponFix::IsChargedRocketStateWedged(UUTWeaponStateFiringChargedRocket_Transactional* Chg)
{
    if (Chg == nullptr)
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
#if !UE_SERVER
	RefreshShockInputTrace();
	if (ShockInputTraceInputComponent != nullptr)
	{
		NCShockInputTrace::Tick(this);
	}
#endif

    // ChargedWedgeFirstSeenTime is a transition marker, not state that may leak
    // into another weapon state or charge cycle. Clear it even when the normal
    // watchdog body below is skipped because the weapon already left firing.
    if (Role == ROLE_Authority && ChargedWedgeFirstSeenTime >= 0.f)
    {
        UUTWeaponStateFiringChargedRocket_Transactional* MarkerState =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(GetCurrentState());
        const bool bLeftObservedState = !IsFiring() || MarkerState == nullptr;
        if (bLeftObservedState)
        {
            if (RocketPrimaryDiagFor(this, 0, 2))
            {
                AUTPlusWeap_RocketLauncher* MarkerRL = Cast<AUTPlusWeap_RocketLauncher>(this);
                UE_LOG(LogUTWeaponFix, Warning,
                    TEXT("[RocketM1Diag] WEDGE_MARKER_RESET frame=%u t=%.4f reason=%s observedFor=%.4f state=%s loadedR=%d loadedB=%d"),
                    (uint32)GFrameCounter, GetWorld()->GetTimeSeconds(),
                    TEXT("state_exit"),
                    GetWorld()->GetTimeSeconds() - ChargedWedgeFirstSeenTime,
                    GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
                    MarkerRL ? MarkerRL->NumLoadedRockets : -1,
                    MarkerRL ? MarkerRL->NumLoadedBarrels : -1);
            }
            ChargedWedgeFirstSeenTime = -1.f;
        }
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
        if (UUTWeaponStateFiringChargedRocket_Transactional* Chg = Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState))
        {
            // A legitimately-active charged state always has one of these in flight (loading, grace,
            // mid-burst, or the post-burst refire wait) and self-transitions — leave it alone. A charged
            // state that is IsFiring() with NONE of them is WEDGED: it never self-
            // transitions, and an incoming primary routes through its inherited stock
            // UUTWeaponStateFiring::BeginFiringSequence which just sets PendingFireSequence and returns —
            // no projectile, no reject, no log. Only the rocket has this state, which is why the no-reg
            // is rocket-only.
            if (!IsChargedRocketStateWedged(Chg))
            {
                if (ChargedWedgeFirstSeenTime >= 0.f && RocketPrimaryDiagFor(this, 0, 2))
                {
                    AUTPlusWeap_RocketLauncher* ClearedRL = Cast<AUTPlusWeap_RocketLauncher>(this);
                    UE_LOG(LogUTWeaponFix, Warning,
                        TEXT("[RocketM1Diag] WEDGE_CLEARED frame=%u t=%.4f observedFor=%.4f charging=%d loadedR=%d loadedB=%d timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
                        (uint32)GFrameCounter, GetWorld()->GetTimeSeconds(),
                        GetWorld()->GetTimeSeconds() - ChargedWedgeFirstSeenTime,
                        Chg->bCharging ? 1 : 0,
                        ClearedRL ? ClearedRL->NumLoadedRockets : -1,
                        ClearedRL ? ClearedRL->NumLoadedBarrels : -1,
                        GetWorldTimerManager().GetTimerRemaining(Chg->LoadTimerHandle),
                        GetWorldTimerManager().GetTimerRemaining(Chg->GraceTimerHandle),
                        GetWorldTimerManager().GetTimerRemaining(Chg->FireLoadedRocketHandle),
                        GetWorldTimerManager().GetTimerRemaining(Chg->RefireCheckHandle));
                }
                ChargedWedgeFirstSeenTime = -1.f;
                return;
            }

            // WEDGED. Stamp + root-cause snapshot on first observation (the wedge-ENTRY path
            // is still unidentified — this names what the state looked like the moment it
            // went dead), then debounce ~0.25s so a transient no-timer instant between state
            // callbacks can't false-trigger. (sassin's 2026-07-24 wedge ate primary clicks
            // for the full 2.5s under the old shared 2.5x-refire gate.)
            AUTPlusWeap_RocketLauncher* RL = Cast<AUTPlusWeap_RocketLauncher>(this);
            const int32 LoadedRockets = RL ? FMath::Max(0, RL->NumLoadedRockets) : 0;
            const int32 LoadedBarrels = RL ? FMath::Max(0, RL->NumLoadedBarrels) : 0;
            const float Now = GetWorld()->GetTimeSeconds();
            if (ChargedWedgeFirstSeenTime < 0.f)
            {
                ChargedWedgeFirstSeenTime = Now;
				if (RocketPrimaryDiagFor(this, 0, 2))
				{
					UE_LOG(LogUTWeaponFix, Warning,
						TEXT("[RocketM1Diag] WEDGE_ARMED frame=%u t=%.4f role=%d net=%d state=%s currentMode=%d tracker=%d loadedR=%d loadedB=%d pending0=%d pending1=%d lft0=%.4f timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
						(uint32)GFrameCounter, Now, (int32)Role, (int32)GetNetMode(),
						GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
						CurrentFireMode, CurrentlyFiringMode, LoadedRockets, LoadedBarrels,
						(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
						(UTOwner && UTOwner->IsPendingFire(1)) ? 1 : 0,
						LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f,
						GetWorldTimerManager().GetTimerRemaining(Chg->LoadTimerHandle),
						GetWorldTimerManager().GetTimerRemaining(Chg->GraceTimerHandle),
						GetWorldTimerManager().GetTimerRemaining(Chg->FireLoadedRocketHandle),
						GetWorldTimerManager().GetTimerRemaining(Chg->RefireCheckHandle));
				}
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[WedgeArmed] %s (%s) charged state idle without exit: mode=%d CurFiring=%d loadedR=%d loadedB=%d pend0=%d pend1=%d sinceFire0=%.2fs sinceFire1=%.2fs pendingWpn=%d"),
                    *GetName(),
                    (UTOwner && UTOwner->PlayerState) ? *UTOwner->PlayerState->PlayerName : TEXT("?"),
                    CurrentFireMode, CurrentlyFiringMode, LoadedRockets, LoadedBarrels,
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
            if (LoadedRockets > 0)
            {
                // Preserve only real logical rockets. The loading/visual barrel count can
                // remain nonzero after a normal spread burst and must never cause another
                // projectile. A genuine loaded wedge is force-released through the charged
                // state's own path.
                if (Now - ChargedWedgeFirstSeenTime >= 1.0f)
                {
                    UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s abandoning a LOADED wedged ChargedRocket after failed force-release (loadedR=%d loadedB=%d) — volley lost"),
                        *GetName(), LoadedRockets, LoadedBarrels);
                    ChargedWedgeFirstSeenTime = -1.f;
                    CurrentlyFiringMode = 255;
                    for (int32 i = 0; i < FireModeActiveState.Num(); i++) { FireModeActiveState[i] = 0; }
                    GotoActiveState();
                    return;
                }
                Chg->RecoverWedgedRelease();
                return;
            }
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[FireBlock] %s fast-recovered an empty WEDGED ChargedRocket state (mode=%d CurFiring=%d loadedR=0 loadedB=%d wedged=%.2fs)"),
                *GetName(), CurrentFireMode, CurrentlyFiringMode, LoadedBarrels, Now - ChargedWedgeFirstSeenTime);
			if (RocketPrimaryDiagFor(this, 0, 2))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] WEDGE_RECOVER frame=%u t=%.4f wedgedFor=%.4f state=%s currentMode=%d tracker=%d pending0=%d loadedR=%d loadedB=%d"),
					(uint32)GFrameCounter, Now, Now - ChargedWedgeFirstSeenTime,
					GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
					CurrentFireMode, CurrentlyFiringMode,
					(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0, LoadedRockets, LoadedBarrels);
			}
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

        // LastFireTime is stamped by AUTWeaponFix::FireShot and the transactional/beam
        // states — and by stock-routed overrides (minigun mode 0) that must stamp it
        // themselves, because nothing else on the stock path writes it.
        if (LastFireTime.IsValidIndex(CurrentFireMode))
        {
            if (LastFireTime[CurrentFireMode] <= 0.f)
            {
                // Constructor sentinel: this mode has never stamped. Comparing against -1
                // reads as instantly stale and would StopFire a healthy stream on its first
                // tick (the NCPMinigun mode-0 collapse). Arm the clock from now instead —
                // a genuinely dead state still gets cleaned up one threshold later.
                LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
            }
            else if (GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode] > TimeoutThreshold)
            {
                // Spin-down is not a wedge: after release, stock UUTWeaponStateFiringSpinUp
                // stays the current state (IsFiring) through CoolDownTime with nothing
                // stamping LastFireTime, so the stale test trips on nearly every minigun
                // release — and StopFire here repeats every tick (it never exits the state)
                // while erasing a re-pressed PendingFire mid-cooldown (feathered-respin
                // no-reg). A live cooldown timer proves the state exits on its own; keep
                // the idle clock current instead, so a state that somehow outlives its
                // cooldown is still reaped one threshold later.
                UUTWeaponStateFiringSpinUp* SpinDown = Cast<UUTWeaponStateFiringSpinUp>(CurrentState);
                if (SpinDown != nullptr && GetWorldTimerManager().IsTimerActive(SpinDown->CoolDownFinishedHandle))
                {
                    LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
                }
                else
                {
                    // Charged states never reach here — every wedge path above returns. This is the
                    // generic stuck-firing watchdog (client disconnect / lost Stop) for the other
                    // firing states only. Force stop: kills the looping audio and resets the state.
                    UE_LOG(LogUTWeaponFix, Warning,
                        TEXT("[FireBlock] %s stuck-fire watchdog StopFire: mode=%d idle=%.2fs threshold=%.2fs state=%s"),
                        *GetName(), CurrentFireMode,
                        GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode], TimeoutThreshold,
                        GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"));
                    StopFireInternal(CurrentFireMode);
                }
            }
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

	// Log only the accepted fixed Stop. Retry copies return at the idempotency gate
	// above, keeping level-2 dogfood volume transition-oriented.
	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UUTWeaponStateFiringChargedRocket_Transactional* const ChargedState =
			Cast<UUTWeaponStateFiringChargedRocket_Transactional>(GetCurrentState());
		AUTPlusWeap_RocketLauncher* const RocketWeapon = Cast<AUTPlusWeap_RocketLauncher>(this);
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] SERVER_STOP_ACCEPT frame=%u t=%.4f player=%s mode=%d event=%d authEvent=%d state=%s tracker=%d active0=%d pending0=%d pending1=%d charged=%d charging=%d releaseReq=%d releaseCommit=%d loadedR=%d loadedB=%d timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			RocketPrimaryDiagPlayer(this), FireModeNum, InFireEventIndex,
			AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : -1,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			(UTOwner && UTOwner->IsPendingFire(1)) ? 1 : 0,
			ChargedState ? 1 : 0, (ChargedState && ChargedState->bCharging) ? 1 : 0,
			(ChargedState && ChargedState->bReleaseRequested) ? 1 : 0,
			(ChargedState && ChargedState->bReleaseCommitted) ? 1 : 0,
			RocketWeapon ? RocketWeapon->NumLoadedRockets : -1,
			RocketWeapon ? RocketWeapon->NumLoadedBarrels : -1,
			ChargedState ? GetWorldTimerManager().GetTimerRemaining(ChargedState->LoadTimerHandle) : -1.f,
			ChargedState ? GetWorldTimerManager().GetTimerRemaining(ChargedState->GraceTimerHandle) : -1.f,
			ChargedState ? GetWorldTimerManager().GetTimerRemaining(ChargedState->FireLoadedRocketHandle) : -1.f,
			ChargedState ? GetWorldTimerManager().GetTimerRemaining(ChargedState->RefireCheckHandle) : -1.f);
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
    ScopedTransactionalAimWeapons.Remove(this);

	// A delayed Stop may arrive after another weapon becomes current because the
	// weapon RPC and ServerSwitchWeapon use different actor channels. Preserve the
	// pawn-global release contract below (ncp.StopClearsPending), but do not let the
	// old weapon run a state transition or clear controller data owned by the new
	// equip lifetime.
	const bool bOwnsWeaponStateLifetime = UTOwner != nullptr
		&& UTOwner->GetWeapon() == this
		&& CurrentState != nullptr
		&& CurrentState != InactiveState;

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

	if (!bOwnsWeaponStateLifetime)
	{
		TargetedCharacter = nullptr;
		UE_LOG(LogUTWeaponFix, Verbose,
			TEXT("[ServerStopFireFixed] Honored stale release; skipping old equip-lifetime state transition mode=%d event=%d state=%s"),
			FireModeNum, InFireEventIndex,
			GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"));
		return;
	}

    // 3. Guard: only call EndFiringSequence if we're actually in the firing state
    // for this mode. Stock EndFiringSequence dispatches to CurrentState->EndFiringSequence(),
    // so calling it when in the wrong state would land on the wrong state object.
    if (FiringState.IsValidIndex(FireModeNum) && GetCurrentState() == FiringState[FireModeNum])
    {
        // Capture ownership before EndFiringSequence: an empty charged release can
        // transition immediately. Charged rockets own their load, burst, and refire
        // timers; the generic Stop cleanup below is only valid for ordinary firing
        // states. The state-local release latch handles the parallel stock Stop.
        UUTWeaponStateFiring* const FiringStateObj = Cast<UUTWeaponStateFiring>(FiringState[FireModeNum]);
        const bool bChargedRocketOwnsCompletion =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(FiringStateObj) != nullptr;

        // Clean up firing effects and PendingFire immediately (not deferred).
        EndFiringSequence(FireModeNum);

        if (!bChargedRocketOwnsCompletion)
        {
            // Ordinary transactional states use the weapon-level deferred-active
            // transition, so their state refire timer would be a competing owner.
            if (FiringStateObj)
            {
                GetWorldTimerManager().ClearTimer(FiringStateObj->RefireCheckHandle);
            }

            // Only defer GotoActiveState — keeps an ordinary weapon in FiringState
            // during cooldown so PutDown() routes to its cooldown-aware override.
            const float CurrentTime = GetWorld()->GetTimeSeconds();
            float ReadyTime = 0.f;

            if (LastFireTime.IsValidIndex(FireModeNum))
            {
                ReadyTime = LastFireTime[FireModeNum] + GetRefireTime(FireModeNum);
            }

            const float TimeRemaining = ReadyTime - CurrentTime;

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
	const bool bOwnsExpectedState = UTOwner != nullptr
		&& UTOwner->GetWeapon() == this
		&& FiringState.IsValidIndex(FireModeNum)
		&& FiringState[FireModeNum] != nullptr
		&& GetCurrentState() == FiringState[FireModeNum];

	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] DEFERRED_ACTIVE_CALLBACK frame=%u t=%.4f role=%d net=%d mode=%d ownsExpected=%d state=%s expected=%s tracker=%d active0=%d pendingBefore0=%d retryRemain=%.4f lft0=%.4f"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			(int32)Role, (int32)GetNetMode(), FireModeNum, bOwnsExpectedState ? 1 : 0,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum]
				? *FiringState[FireModeNum]->GetClass()->GetName() : TEXT("null"),
			CurrentlyFiringMode, FireModeActiveState.IsValidIndex(0) ? FireModeActiveState[0] : 255,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			GetWorldTimerManager().GetTimerRemaining(RetryFireHandle[0]),
			LastFireTime.IsValidIndex(0) ? LastFireTime[0] : -1.f);
	}

    // EndFiringSequence already ran in StopFire/ServerStopFireFixed — no need to call it again.
    // Only transition to ActiveState if we are actually still in a firing state.
    // If we are already unequipping or inactive, GotoActiveState would be wrong.
    UE_LOG(LogUTWeaponFix, Verbose, TEXT("[DeferredGotoActiveState] Mode %d: State=%s PendingFire[%d]=%d"),
        FireModeNum, GetCurrentState() ? *GetCurrentState()->GetName() : TEXT("null"),
        FireModeNum, (UTOwner && UTOwner->IsPendingFire(FireModeNum)) ? 1 : 0);

	// CRITICAL: Only transition if we're still in the firing state that SET this
    // timer. There is only ONE DeferredActiveStateHandle shared by both fire modes.
    // When alternating primary→secondary quickly, Mode 0's deferred can fire while
    // the weapon is in FiringState[1] (Mode 1), yanking it out mid-shot. This kills
    // the Mode 1 fire — the auth projectile may have spawned but the state machine
    // is corrupted, causing the server to miss subsequent fires for that mode.
    //
    // By checking FiringState[FireModeNum], stale deferreds from the OTHER mode
    // are harmlessly ignored.
	if (bOwnsExpectedState)
	{
		// Clear only while this callback still owns the current weapon/state. During
		// a real switch, ActiveState::BeginState calls PutDown before its auto-fire
		// check, so the physical held input belongs to the incoming weapon.
		if (UTOwner->GetPendingWeapon() == nullptr)
		{
			UTOwner->SetPendingFire(FireModeNum, false);
		}
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
	return GetPredictionTimeWithFudgeMs(GetConfiguredHitscanFudgeMs());
}

float AUTWeaponFix::GetPredictionTimeWithFudgeMs(float InFudgeMs) const
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

	const AUTPlayerController* ShooterPC =
		Cast<AUTPlayerController>(UTOwner->Controller);
	const bool bRemoteHuman =
		ShooterPC != nullptr && !ShooterPC->IsLocalController();
	float ObservedRTTMs = bRemoteHuman
		? 0.f : UTOwner->PlayerState->ExactPing;
	if (bRemoteHuman)
	{
		// ExactPing is written by ServerUpdatePing from a client-supplied float.
		// Remote hit validation must use the server's ACK-derived RTT instead.
		// AvgLag starts at 9999, so use a zero base rewind until measured.
		// The separate fixed-rung claim time search may still probe +15/30/45ms.
		if (!GetServerObservedRTTMs(ShooterPC, ObservedRTTMs))
		{
			return 0.0f;
		}
	}

	// 2. Subtract the caller-selected full-RTT buffer before converting to
	// one-way time. Clamp negative cvar/property values rather than over-rewind.
	const float AdjustedPing = ObservedRTTMs - FMath::Max(0.f, InFudgeMs);

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

    // A previous trace's demotion must never leak into this one (the sniper /
    // head-sphere fallbacks consult this flag after we return).
    bLastUnclaimedRenderDemoted = false;
    LastHitscanPaddedRadius = 0.f;

    // Claim-capable = a mode whose FIRED shots carry hit claims (exact-trace,
    // damaging, replication-tracked). Gates both the attribution telemetry and
    // the unclaimed render check. Deliberately excludes per-tick beam re-traces
    // (Link beam runs FireInstantHit every server tick for remote visuals and
    // would flood one line per tick) and spread/zoom modes.
    const bool bClaimCapableMode =
        bTrackHitScanReplication &&
        InstantHitInfo.IsValidIndex(CurrentFireMode) &&
        InstantHitInfo[CurrentFireMode].DamageType != nullptr &&
        InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.0f;

    // [HitAttrib] read-only per-shot attribution state; populated only when the
    // cvar is on, never feeds back into the validation result.
    const bool bHitAttrib = (Role == ROLE_Authority) && bClaimCapableMode &&
        (CVarHitAttribDebug.GetValueOnGameThread() > 0);
    float AttribClaimMissBy = BIG_NUMBER;   // claimed target's primary-trace distance beyond the UNPADDED radius
    float AttribClaimPad = 0.f;             // padding the claimed target was granted in the primary loop
    bool bAttribTimeSearchHit = false;
    float AttribTimeSearchRungMs = 0.f;
    float AttribTimeSearchMissBy = 0.f;

    ECollisionChannel TraceChannel = COLLISION_TRACE_WEAPONNOCHARACTER;
    FCollisionQueryParams QueryParams(GetClass()->GetFName(), true, UTOwner);
    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

    // Applicability is based on the mode's capabilities, not the mutable claim
    // cache. Link clears claims before every beam trace; Minigun cannot produce
    // one. A stale/unsolicited claim must never switch either mode back to raw
    // target history. Bots, standalone, and the listen host keep raw validation
    // because there is no remote rendered view to reconstruct.
    const AUTPlayerController* RenderAuthorityShooterPC =
        (UTOwner != nullptr) ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
    const bool bRenderAuthoritativeTargeting =
        !bClaimCapableMode && SupportsRenderCredit() &&
        CVarRenderCredit.GetValueOnGameThread() > 0 &&
        Role == ROLE_Authority && GetNetMode() != NM_Standalone &&
        UTOwner != nullptr && UTOwner->PlayerState != nullptr &&
        RenderAuthorityShooterPC != nullptr &&
        !RenderAuthorityShooterPC->IsLocalController();
    float RenderAuthorityRTTMs = 0.f;
    const bool bRenderAuthorityTimingValid = bRenderAuthoritativeTargeting &&
        GetServerObservedRTTMs(RenderAuthorityShooterPC, RenderAuthorityRTTMs);
    const float RenderAuthorityExtraMs = bRenderAuthoritativeTargeting
        ? FMath::Max(0.f, CVarRenderCreditExtraMs.GetValueOnGameThread())
        : 0.f;
    const float RenderAuthoritativeMs = bRenderAuthoritativeTargeting
        ? RenderAuthorityRTTMs * 0.5f + RenderAuthorityExtraMs
        : 0.f;
    const float RenderAuthoritativeTime = bRenderAuthoritativeTargeting
        ? FMath::Clamp(RenderAuthoritativeMs * 0.001f, 0.f, 0.25f)
        : 0.f;
    const float RenderAuthoritativeSlack = bRenderAuthoritativeTargeting
        ? FMath::Max(0.f, CVarRenderCreditSlack.GetValueOnGameThread())
        : 0.f;
    const float TargetSampleTime = bRenderAuthoritativeTargeting
        ? RenderAuthoritativeTime : ActualPredictionTime;
    const bool bRenderAuthorityDebug = bRenderAuthoritativeTargeting &&
        CVarHitAttribDebug.GetValueOnGameThread() > 0;

    // Perform the initial trace against world geometry. Weapon attachments are
    // client-only cosmetics (AUTCharacter never spawns them on a dedicated
    // server), so a Blueprint that leaves Mesh3P on BlockAll must not shorten
    // the client's ray or manufacture a client-only impact/claim/hitsound.
    // Retracing is intentionally bounded and happens only when an attachment is
    // actually the nearest blocker; normal shots still issue exactly one query.
    constexpr int32 MaxCosmeticAttachmentSkips = 16;
    int32 CosmeticAttachmentSkips = 0;
    for (;;)
    {
        const bool bWorldHit = (TraceRadius <= 0.0f)
            ? GetWorld()->LineTraceSingleByChannel(
                Hit, StartLocation, EndTrace, TraceChannel, QueryParams)
            : GetWorld()->SweepSingleByChannel(
                Hit, StartLocation, EndTrace, FQuat::Identity, TraceChannel,
                FCollisionShape::MakeSphere(TraceRadius), QueryParams);

        if (!bWorldHit || !Hit.bBlockingHit)
        {
            Hit.Location = EndTrace;
            break;
        }

        AUTWeaponAttachment* CosmeticAttachment = Cast<AUTWeaponAttachment>(Hit.GetActor());
        if (CosmeticAttachment == nullptr ||
            CosmeticAttachmentSkips >= MaxCosmeticAttachmentSkips)
        {
            break;
        }

        QueryParams.AddIgnoredActor(CosmeticAttachment);
        ++CosmeticAttachmentSkips;
    }


    // Now check against pawns
    AUTCharacter* BestTarget = NULL;
    FVector BestPoint(0.f);
    FVector BestCapsulePoint(0.f);
    float BestCollisionRadius = 0.f;
    float BestTargetEntryDistance = BIG_NUMBER;
    FVector UnverifiableBlockHitLocation(0.f);
    float UnverifiableBlockEntryDistance = BIG_NUMBER;

    const FVector PawnTraceVector = Hit.Location - StartLocation;
    const float PawnTraceLength = PawnTraceVector.Size();
    const FVector PawnTraceDirection = PawnTraceLength > KINDA_SMALL_NUMBER
        ? PawnTraceVector / PawnTraceLength
        : (EndTrace - StartLocation).GetSafeNormal();
    // Exact first entry of this finite ray segment into a vertical capsule.
    // The capsule is the union of its finite cylinder and endpoint spheres;
    // expanding Radius by the trace radius is the exact sphere-sweep Minkowski
    // sum. This avoids the closest-point/back-distance approximation, which is
    // late for rays angled along the capsule axis.
    auto RayCapsuleEntryDistance = [&](const FVector& CapsuleCenter,
        float AxisHalfLength, float CombinedRadius, float& OutEntryDistance)
    {
        OutEntryDistance = BIG_NUMBER;
        AxisHalfLength = FMath::Max(0.f, AxisHalfLength);
        CombinedRadius = FMath::Max(0.f, CombinedRadius);
        if (CombinedRadius <= 0.f)
        {
            return false;
        }

        FVector ClosestAxisPoint = CapsuleCenter;
        ClosestAxisPoint.Z += FMath::Clamp(
            StartLocation.Z - CapsuleCenter.Z, -AxisHalfLength, AxisHalfLength);
        if (FVector::DistSquared(StartLocation, ClosestAxisPoint) <
            FMath::Square(CombinedRadius))
        {
            OutEntryDistance = 0.f;
            return true;
        }
        if (PawnTraceLength <= KINDA_SMALL_NUMBER)
        {
            return false;
        }

        auto ConsiderEntry = [&](float EntryDistance)
        {
            if (EntryDistance >= 0.f && EntryDistance <= PawnTraceLength)
            {
                OutEntryDistance = FMath::Min(OutEntryDistance, EntryDistance);
            }
        };

        const FVector Offset = StartLocation - CapsuleCenter;
        const float CylinderA = FMath::Square(PawnTraceDirection.X) +
            FMath::Square(PawnTraceDirection.Y);
        if (AxisHalfLength > 0.f && CylinderA > SMALL_NUMBER)
        {
            const float CylinderHalfB = Offset.X * PawnTraceDirection.X +
                Offset.Y * PawnTraceDirection.Y;
            const float CylinderC = FMath::Square(Offset.X) +
                FMath::Square(Offset.Y) - FMath::Square(CombinedRadius);
            const float CylinderDisc = FMath::Square(CylinderHalfB) -
                CylinderA * CylinderC;
            if (CylinderDisc >= 0.f)
            {
                const float Root = FMath::Sqrt(CylinderDisc);
                const float Entries[2] = {
                    (-CylinderHalfB - Root) / CylinderA,
                    (-CylinderHalfB + Root) / CylinderA
                };
                for (int32 EntryIndex = 0; EntryIndex < 2; ++EntryIndex)
                {
                    const float EntryDistance = Entries[EntryIndex];
                    const float EntryZ = Offset.Z +
                        PawnTraceDirection.Z * EntryDistance;
                    if (FMath::Abs(EntryZ) <= AxisHalfLength)
                    {
                        ConsiderEntry(EntryDistance);
                    }
                }
            }
        }

        auto TestEndSphere = [&](const FVector& SphereCenter)
        {
            const FVector SphereOffset = StartLocation - SphereCenter;
            const float SphereHalfB = FVector::DotProduct(
                SphereOffset, PawnTraceDirection);
            const float SphereC = SphereOffset.SizeSquared() -
                FMath::Square(CombinedRadius);
            const float SphereDisc = FMath::Square(SphereHalfB) - SphereC;
            if (SphereDisc >= 0.f)
            {
                const float Root = FMath::Sqrt(SphereDisc);
                ConsiderEntry(-SphereHalfB - Root);
                ConsiderEntry(-SphereHalfB + Root);
            }
        };
        TestEndSphere(CapsuleCenter + FVector(0.f, 0.f, AxisHalfLength));
        if (AxisHalfLength > 0.f)
        {
            TestEndSphere(CapsuleCenter - FVector(0.f, 0.f, AxisHalfLength));
        }

        return OutEntryDistance < BIG_NUMBER;
    };

    // Stock UT does one extra visibility check only when client-claim padding,
    // rather than the real capsule, rescued the hit. A padded envelope can
    // overlap the shot ray around a wall corner even though no unobstructed
    // segment reaches the target's physical capsule. Use a separate hit result
    // here: stock writes this query into the main Hit, but retaining the
    // original world endpoint keeps later pawn ordering deterministic when an
    // outside-hit candidate is rejected.
    auto HasClearPathToCapsuleSurface = [&](const FVector& ClosestPoint,
        const FVector& ClosestCapsulePoint, float CapsuleRadius)
    {
        const FVector SurfaceDirection =
            (ClosestPoint - ClosestCapsulePoint).GetSafeNormal();
        if (CapsuleRadius <= 0.f || SurfaceDirection.IsNearlyZero())
        {
            return false;
        }

        const FVector PointToCheck = ClosestCapsulePoint +
            CapsuleRadius * SurfaceDirection;
        FHitResult OutsideHit;
        return !GetWorld()->LineTraceSingleByChannel(OutsideHit,
            StartLocation, PointToCheck, TraceChannel, QueryParams);
    };

    // If a live pawn lacks trustworthy render history, it cannot be
    // damaged in render-authoritative mode. Its known capsule positions must
    // still conservatively occlude the trace; otherwise "fail closed" for the
    // first target could become a hit on a pawn/projectile behind it.
    auto RecordUnverifiableBlocker = [&](AUTCharacter* Target,
        const FVector& CandidateLocation, float CandidateTime)
    {
        FVector BlockerLocation = CandidateLocation;
        float BlockerHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        ApplySlidePostureForValidation(Target, CandidateTime, BlockerLocation, BlockerHeight);
        const float BlockerRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
        const float AxisHalfLength = FMath::Max(0.f, BlockerHeight - BlockerRadius);
        const float EffectiveRadius = FMath::Min(BlockerRadius, BlockerHeight);
        const float CombinedRadius = EffectiveRadius + TraceRadius +
            RenderAuthoritativeSlack;
        float EntryDistance = BIG_NUMBER;
        if (RayCapsuleEntryDistance(BlockerLocation, AxisHalfLength,
            CombinedRadius, EntryDistance))
        {
            if (EntryDistance < UnverifiableBlockEntryDistance)
            {
                UnverifiableBlockEntryDistance = EntryDistance;
                UnverifiableBlockHitLocation = StartLocation +
                    PawnTraceDirection * EntryDistance;
            }
        }
    };

    for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
    {
        AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
        if (Target != UTOwner && IsLiveHitscanTarget(Target))
        {

            // Standard logic: Teammate checks, etc.
            if (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target))
            {
                if (bRenderAuthoritativeTargeting)
                {
                    int32 RenderOlderIndex = INDEX_NONE;
                    int32 RenderNewerIndex = INDEX_NONE;
                    const bool bHasContinuousRenderHistory = HasContinuousRenderHistory(
                        Target, RenderAuthoritativeTime, ActualPredictionTime,
                        RenderOlderIndex, RenderNewerIndex);
                    if (!bRenderAuthorityTimingValid || !bHasContinuousRenderHistory)
                    {
                        RecordUnverifiableBlocker(Target, Target->GetActorLocation(), 0.f);

                        // If the requested render epoch straddles a teleport,
                        // GetRewindLocation() may interpolate across it. Treat
                        // both real bracket endpoints as non-damageable
                        // blockers so a farther pawn cannot be hit through the
                        // target's unverifiable rendered position.
                        const float Now = GetWorld()->GetTimeSeconds();
                        if (Target->SavedPositions.IsValidIndex(RenderOlderIndex))
                        {
                            RecordUnverifiableBlocker(Target,
                                Target->SavedPositions[RenderOlderIndex].Position,
                                FMath::Max(0.f, Now - Target->SavedPositions[RenderOlderIndex].Time));
                        }
                        if (Target->SavedPositions.IsValidIndex(RenderNewerIndex))
                        {
                            RecordUnverifiableBlocker(Target,
                                Target->SavedPositions[RenderNewerIndex].Position,
                                FMath::Max(0.f, Now - Target->SavedPositions[RenderNewerIndex].Time));
                        }
                        continue;
                    }
                }

                float ExtraHitPadding = 0.f;

                // Only apply padding if the client explicitly claimed THIS target.
                // If client missed (ReceivedHitScanHitChar is null), this block is skipped (Padding = 0).
                if (bClaimCapableMode && Target == ReceivedHitScanHitChar)
                {
                    // Check velocity to decide WHICH padding to use
                    bool bIsMoving = !Target->GetVelocity().IsNearlyZero(1.0f);
					//ExtraHitPadding = bIsMoving ? HitScanPadding : HitScanPaddingStationary;
					if (bIsMoving)
					{
						// Server-live claimed-target primary allowance. Keep this
						// independent from the fallback-rung allowance so each can
						// be canaried and rolled back without rebuilding.
						ExtraHitPadding = FMath::Clamp(
							CVarHitscanPrimaryPadding.GetValueOnGameThread(), 0.0f, 100.0f);
				
					}
					else
					{
						// Stationary targets don't need velocity compensation
                        ExtraHitPadding = HitScanPaddingStationary;
					}
                }
                else if (bRenderAuthoritativeTargeting)
                {
                    ExtraHitPadding = RenderAuthoritativeSlack;
                }

                // Stock-style modes sample raw validation history. Opted-in
                // claimless modes substitute the one estimated render-time
                // sample here, so there is no raw OR render acceptance union.
                FVector TargetLocation = ((TargetSampleTime > 0.f) && (Role == ROLE_Authority))
                    ? Target->GetRewindLocation(TargetSampleTime)
                    : Target->GetActorLocation();

                if (bRenderAuthoritativeTargeting)
                {
                    const FVector ValidationLocation = (ActualPredictionTime > 0.f)
                        ? Target->GetRewindLocation(ActualPredictionTime)
                        : Target->GetActorLocation();
                    if (FVector::DistSquared(ValidationLocation, TargetLocation) >
                        FMath::Square(600.f))
                    {
                        RecordUnverifiableBlocker(Target, Target->GetActorLocation(), 0.f);
                        RecordUnverifiableBlocker(Target, TargetLocation,
                            RenderAuthoritativeTime);
                        continue;
                    }
                }

                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                ApplySlidePostureForValidation(Target,
                    ((TargetSampleTime > 0.f) && (Role == ROLE_Authority)) ? TargetSampleTime : 0.f,
                    TargetLocation, CollisionHeight);
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
                const float TargetEffectiveRadius =
                    FMath::Min(CollisionRadius, CollisionHeight);

                bool bCheckOutsideHit = false;
                bool bHitTarget = false;
                FVector ClosestPoint(0.f);
                FVector ClosestCapsulePoint = TargetLocation;
                if (CollisionRadius >= CollisionHeight)
                {
                    ClosestPoint = FMath::ClosestPointOnSegment(TargetLocation, StartLocation, Hit.Location);
                    const float DistanceSq =
                        (ClosestPoint - TargetLocation).SizeSquared();
                    const float UnpaddedRadius = TargetEffectiveRadius + TraceRadius;
                    bHitTarget = DistanceSq < FMath::Square(UnpaddedRadius);
                    if (!bHitTarget && ExtraHitPadding > 0.f)
                    {
                        bHitTarget = DistanceSq <
                            FMath::Square(UnpaddedRadius + ExtraHitPadding);
                        bCheckOutsideHit = bHitTarget;
                    }
                }
                else
                {
                    FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
                    FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
                    const float DistanceSq =
                        (ClosestPoint - ClosestCapsulePoint).SizeSquared();
                    const float UnpaddedRadius = TargetEffectiveRadius + TraceRadius;
                    bHitTarget = DistanceSq < FMath::Square(UnpaddedRadius);
                    if (!bHitTarget && ExtraHitPadding > 0.f)
                    {
                        bHitTarget = DistanceSq <
                            FMath::Square(UnpaddedRadius + ExtraHitPadding);
                        bCheckOutsideHit = bHitTarget;
                    }
                }

                if (bCheckOutsideHit)
                {
                    bHitTarget = HasClearPathToCapsuleSurface(ClosestPoint,
                        ClosestCapsulePoint, TargetEffectiveRadius);
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

                const float TargetAxisHalfLength =
                    FMath::Max(0.f, CollisionHeight - CollisionRadius);
                const float TargetSelectionRadius = TargetEffectiveRadius +
                    TraceRadius + ExtraHitPadding;
                float CandidateEntryDistance = BIG_NUMBER;
                const bool bHasCandidateEntry = bRenderAuthoritativeTargeting &&
                    bHitTarget &&
                    RayCapsuleEntryDistance(TargetLocation, TargetAxisHalfLength,
                        TargetSelectionRadius, CandidateEntryDistance);

                // Raw/claim-capable behavior retains its established closest-
                // axis ordering. Render-authoritative mode uses true capsule
                // entry ordering so different postures/radii cannot let a
                // farther surface beat a nearer one.
                const bool bShouldSelectTarget = bHitTarget &&
                    (bRenderAuthoritativeTargeting
                        ? (bHasCandidateEntry && CandidateEntryDistance < BestTargetEntryDistance)
                        : (!BestTarget || ((ClosestPoint - StartLocation).SizeSquared() <
                            (BestPoint - StartLocation).SizeSquared())));
                if (bShouldSelectTarget)
                {
                    BestTarget = Target;
                    BestPoint = ClosestPoint;
                    BestCapsulePoint = ClosestCapsulePoint;
                    BestCollisionRadius = TargetEffectiveRadius;
                    BestTargetEntryDistance = CandidateEntryDistance;
                    // Cache the total padded radius for ServerShield hitplot normalization
                    LastHitscanPaddedRadius = TargetEffectiveRadius +
                        TraceRadius + ExtraHitPadding;
                }
            }
        }
        // --- FIX END ---
    }

    const float WorldHitDistance = (Hit.Location - StartLocation).Size();
    if (bRenderAuthoritativeTargeting &&
        UnverifiableBlockEntryDistance <= BestTargetEntryDistance &&
        UnverifiableBlockEntryDistance < WorldHitDistance)
    {
        if (bRenderAuthorityDebug)
        {
            UE_LOG(LogUTWeaponFix, Verbose,
                TEXT("[RenderAuthority] fail-closed at unverifiable pawn history; rejected=%s"),
                BestTarget != nullptr ? *BestTarget->GetName() : TEXT("none"));
        }

        // Stop the full trace at this non-damageable endpoint. Merely clearing
        // BestTarget would expose the original world/projectile hit behind the
        // pawn, allowing damage or a beam visual to pass through history that
        // the server has explicitly deemed unverifiable.
        Hit = FHitResult(StartLocation, EndTrace);
        Hit.Location = UnverifiableBlockHitLocation;
        Hit.ImpactPoint = Hit.Location;
        Hit.Normal = -PawnTraceDirection;
        Hit.ImpactNormal = Hit.Normal;
        Hit.bBlockingHit = true;
        Hit.Time = (EndTrace - StartLocation).IsNearlyZero()
            ? 0.f
            : UnverifiableBlockEntryDistance / (EndTrace - StartLocation).Size();
        BestTarget = nullptr;
        BestPoint = FVector::ZeroVector;
        BestCapsulePoint = FVector::ZeroVector;
        BestCollisionRadius = 0.f;
        LastHitscanPaddedRadius = 0.f;
    }

	// ---- RESCUE LEAD GATE state (see cvar block; emitted on [HitAttrib]) ----
	bool bRescueLeadApplicable = false;          // remote-human claimed shot, gate or telemetry on
	bool bRescueLeadTimingValid = false;         // ACK RTT measured; render epoch trustworthy
	bool bRescueLeadSearchBlockedNoTiming = false; // enforce + no timing: whole search skipped
	bool bRescueLeadAcceptedValid = false;       // an accepted rung was lead-evaluated
	float RescueLeadAcceptedUU = 0.f;            // credited lead of the ACCEPTED rung
	int32 RescueLeadRungsSkipped = 0;            // enforce: geometrically-hitting rungs denied on lead
	bool bRescueLeadRayValid = false;            // ray-vs-render telemetry computed with a usable motion dir
	float RescueLeadRayAheadUU = 0.f;            // signed: shot ray ahead of render capsule along historical motion
	float RescueLeadRenderMissUU = 0.f;          // ray miss distance vs render-epoch capsule (unsigned-published)
	float RescueLeadHistDMag = 0.f;              // |validation - render| sample displacement (motion window)
	float RescueLeadCapApplied = 0.f;            // cap in force when evaluated (for the emit)

	// ============================================================
	// NEWNET-STYLE BIDIRECTIONAL TIME SEARCH
	// If client claimed a hit but we didn't find it, search through time
	// ============================================================
	// Mirror the main-loop team guard (~line 1896): never run the time-search for a CLIENT-NAMED teammate when
	// teammates don't block hitscan. ReceivedHitScanHitChar is fully client-controlled, so without this a client
	// could name a teammate to force a near-graze body hit (FF-gated at damage, but it shouldn't be considered).
	if (bClaimCapableMode && Role == ROLE_Authority &&
		IsLiveHitscanTarget(ReceivedHitScanHitChar) &&
		BestTarget != ReceivedHitScanHitChar &&
		(bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, ReceivedHitScanHitChar)))
	{
		AUTCharacter* ClaimedTarget = ReceivedHitScanHitChar;

		float CapRadius = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
		float CapHeight = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

		// RESCUE LEAD GATE (shadow/enforce — see cvar block). The gate judges
		// the capsule position each ACCEPTED RUNG credits, measured from the
		// render-epoch estimate along the target's HISTORICAL motion between
		// the two epochs (render->validation displacement direction — never
		// instantaneous velocity, which zeroes on stops/reversals and points
		// the wrong way on curves). The render sample and motion direction are
		// rung-independent and cached here; the per-rung credited lead differs
		// by the rung offset and is evaluated inside the acceptance branch, so
		// a deep positive rung (crediting a position CLOSER to what was
		// rendered) can pass where a shallow/negative rung is denied. Only
		// remote humans reach this block (ReceivedHitScanHitChar is set solely
		// by the client fire RPC); a claimed shot whose controller cannot be
		// resolved to a remote AUTPlayerController is treated as timing-
		// unavailable, and enforce mode fails closed like the unclaimed render
		// check. Ray-vs-render telemetry (signed along-motion offset + miss
		// distance) is computed once for the [HitAttrib] line so the shadow
		// corpus can separate rendered-body aim from leading aim before
		// enforcement is trusted.
		const int32 RescueLeadGate = CVarHitscanRescueLeadGate.GetValueOnGameThread();
		AController* const RescueRawController = UTOwner ? UTOwner->Controller : nullptr;
		const AUTPlayerController* RescueShooterPC = Cast<AUTPlayerController>(RescueRawController);
		FVector RescueRenderPos = FVector::ZeroVector;
		FVector RescueLeadHistDir = FVector::ZeroVector;
		bool bRescueLeadHistDirValid = false;
		bool bRescueLeadGateActive = false;   // enforce mode with usable timing
		if ((bHitAttrib || RescueLeadGate > 0) &&
			!(RescueRawController != nullptr && RescueRawController->IsLocalController()))
		{
			bRescueLeadApplicable = true;
			RescueLeadCapApplied = FMath::Max(0.f, CVarHitscanMaxRescueLeadUU.GetValueOnGameThread());
			float RescueRTTMs = 0.f;
			if (RescueShooterPC != nullptr && GetServerObservedRTTMs(RescueShooterPC, RescueRTTMs))
			{
				bRescueLeadTimingValid = true;
				const float RescueRenderT = FMath::Clamp(
					(RescueRTTMs * 0.5f +
						FMath::Max(0.f, CVarHitAttribRenderExtraMs.GetValueOnGameThread())) * 0.001f,
					0.f, 0.25f);
				const FVector RescueValPos = (ActualPredictionTime > 0.f)
					? ClaimedTarget->GetRewindLocation(ActualPredictionTime)
					: ClaimedTarget->GetActorLocation();
				RescueRenderPos = (RescueRenderT > 0.f)
					? ClaimedTarget->GetRewindLocation(RescueRenderT)
					: ClaimedTarget->GetActorLocation();
				const FVector HistDelta = RescueValPos - RescueRenderPos;
				RescueLeadHistDMag = HistDelta.Size();
				// Under ~2uu of window motion there is no meaningful direction;
				// fall back to unsigned distance-from-render for the rung lead
				// (conservative: a teleport-corrupted sample reads huge and is
				// visible in the shadow corpus rather than silently passing).
				bRescueLeadHistDirValid = RescueLeadHistDMag >= 2.0f;
				if (bRescueLeadHistDirValid)
				{
					RescueLeadHistDir = HistDelta / RescueLeadHistDMag;
				}
				bRescueLeadGateActive = (RescueLeadGate > 0);

				// Ray-vs-render telemetry (rung-independent): where did the shot
				// ray actually pass relative to the capsule the shooter was
				// estimated to have SEEN. Standing posture; the slide-adjusted
				// posture at render time is not reconstructable from position
				// history alone and this is telemetry, not the gate quantity.
				FVector RenderRayPoint(0.f), RenderCapPoint(RescueRenderPos);
				float RenderEffRadius;
				if (CapRadius >= CapHeight)
				{
					RenderRayPoint = FMath::ClosestPointOnSegment(RescueRenderPos, StartLocation, Hit.Location);
					RenderEffRadius = CapHeight;
				}
				else
				{
					const FVector RenderSeg(0.f, 0.f, CapHeight - CapRadius);
					FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location,
						RescueRenderPos - RenderSeg, RescueRenderPos + RenderSeg,
						RenderRayPoint, RenderCapPoint);
					RenderEffRadius = CapRadius;
				}
				RescueLeadRenderMissUU = FVector::Dist(RenderRayPoint, RenderCapPoint)
					- (RenderEffRadius + TraceRadius);
				if (bRescueLeadHistDirValid)
				{
					RescueLeadRayAheadUU = FVector::DotProduct(
						RenderRayPoint - RenderCapPoint, RescueLeadHistDir);
					bRescueLeadRayValid = true;
				}
			}
			else if (RescueLeadGate > 0)
			{
				// No trustworthy render epoch (unmeasured RTT, or a claimed shot
				// with a null/non-UT controller). Enforce fails closed; shadow
				// runs untouched and the emit reports no-timing — NEVER counted
				// as a measured over-cap rescue.
				bRescueLeadSearchBlockedNoTiming = true;
				UE_LOG(LogUTWeaponFix, Verbose,
					TEXT("[RescueLeadGate] BLOCKED-NO-TIMING %s -> %s: no server RTT measurement — time search skipped"),
					*GetName(), *ClaimedTarget->GetName());
			}
		}

		const float SearchStep = 0.015f;      // 15ms steps
		const float MaxSearchOffset = GetHitscanTimeSearchWindow(); // ±45ms max search (tries ±15, ±30, ±45 on fixed 15ms rungs; ±60 is the next rung but trades attacker recovery for "shot through my dodge" defender complaints — primary rewind still does the heavy lifting)
		float SearchOffset = SearchStep;

		while (!bRescueLeadSearchBlockedNoTiming && FMath::Abs(SearchOffset) <= MaxSearchOffset)
		{
			float AltRewindTime = ActualPredictionTime + SearchOffset;

			// Sanity bounds
			if (AltRewindTime > 0.0f && AltRewindTime < 0.25f)
			{
				FVector AltTargetLoc = ClaimedTarget->GetRewindLocation(AltRewindTime);
				// Raw (pre-posture) sample: the position this rung would CREDIT,
				// judged by the lead gate below. Posture adjustment only moves Z
				// for the collision test and must not perturb the lead metric.
				const FVector AltTargetLocRaw = AltTargetLoc;

				// Handle floor sliding at alternate time (grace-windowed posture:
				// AltRewindTime is this rung's effective claim age).
				float AltCapHeight = CapHeight;
				ApplySlidePostureForValidation(ClaimedTarget, AltRewindTime, AltTargetLoc, AltCapHeight);
				const float AltEffectiveRadius =
					FMath::Min(CapRadius, AltCapHeight);

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

				// Server-live padding for the alternate-time fallback. This is
				// deliberately independent from primary moving-target padding.
				const float SearchPadding = FMath::Clamp(
					CVarHitscanSearchPadding.GetValueOnGameThread(), 0.0f, 100.0f);
				float CombinedRadius = AltEffectiveRadius + TraceRadius + SearchPadding;
				const float SearchDistanceSq =
					(ClosestPoint - ClosestCapsulePoint).SizeSquared();
				const bool bSearchHit = SearchDistanceSq <
					FMath::Square(CombinedRadius);
				const bool bSearchPaddingOnly = bSearchHit &&
					SearchDistanceSq >=
						FMath::Square(AltEffectiveRadius + TraceRadius);
				const bool bSearchOutsideClear = !bSearchPaddingOnly ||
					HasClearPathToCapsuleSurface(ClosestPoint,
						ClosestCapsulePoint, AltEffectiveRadius);

				if (bSearchHit && bSearchOutsideClear)
				{
					// RESCUE LEAD GATE: judge the position THIS rung credits.
					// Evaluated only for geometrically-accepted rungs, so an
					// enforce-mode skip means precisely "this rung would have
					// been the rescue but for its lead". The walk continues —
					// a deeper rung crediting a position nearer the rendered
					// image may still pass.
					float RungLeadUU = 0.f;
					bool bRungLeadKnown = false;
					if (bRescueLeadApplicable && bRescueLeadTimingValid)
					{
						const FVector AltFromRender = AltTargetLocRaw - RescueRenderPos;
						RungLeadUU = bRescueLeadHistDirValid
							? FVector::DotProduct(AltFromRender, RescueLeadHistDir)
							: AltFromRender.Size();
						bRungLeadKnown = true;
					}
					if (bRungLeadKnown && bRescueLeadGateActive &&
						RungLeadUU > RescueLeadCapApplied)
					{
						RescueLeadRungsSkipped++;
						UE_LOG(LogUTWeaponFix, Verbose,
							TEXT("[RescueLeadGate] SKIPPED RUNG %+.0fms %s -> %s: credited lead %.1f > cap %.1f"),
							SearchOffset * 1000.f, *GetName(), *ClaimedTarget->GetName(),
							RungLeadUU, RescueLeadCapApplied);
						// fall through to the oscillator: do NOT accept this rung
					}
					else
					{
					if (bRungLeadKnown)
					{
						bRescueLeadAcceptedValid = true;
						RescueLeadAcceptedUU = RungLeadUU;
					}
					// Found the hit at alternate time
					BestTarget = ClaimedTarget;
					BestPoint = ClosestPoint;
					BestCapsulePoint = ClosestCapsulePoint;
					BestCollisionRadius = AltEffectiveRadius;

					if (bHitAttrib)
					{
						bAttribTimeSearchHit = true;
						AttribTimeSearchRungMs = SearchOffset * 1000.f;
						// vs the UNPADDED radius at the accepted rung: <=0 is a genuine
						// capsule hit at the alternate time, >0 rode the 45uu search pad.
						AttribTimeSearchMissBy =
							FVector::Dist(ClosestPoint, ClosestCapsulePoint) -
							(AltEffectiveRadius + TraceRadius);
					}

					UE_LOG(LogUTWeaponFix, Verbose,
						TEXT("TimeSearch: Found claimed hit at offset %.1fms (base %.1fms)"),
						SearchOffset * 1000.f, ActualPredictionTime * 1000.f);
					break;
					}
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
        bClaimCapableMode &&
        (bHitAttrib || UnclaimedRenderGate > 0) &&
        Role == ROLE_Authority && GetNetMode() != NM_Standalone &&
        UTOwner != nullptr && UTOwner->PlayerState != nullptr)
    {
        // Same applicability envelope as the claim generator in FireShot: the
        // shooter is a remote human and this mode COULD have claimed. Bots and
        // spread weapons never claim, so their unclaimed hits are legitimate.
        const AUTPlayerController* ShooterPC = Cast<AUTPlayerController>(UTOwner->Controller);
        if (ShooterPC != nullptr && !ShooterPC->IsLocalController())
        {
            bRenderChkApplicable = true;
            float ServerRTTMs = 0.f;
            const bool bServerTimingValid =
                GetServerObservedRTTMs(ShooterPC, ServerRTTMs);
            const float RenderMs = ServerRTTMs * 0.5f +
                FMath::Max(0.f, CVarHitAttribRenderExtraMs.GetValueOnGameThread());

            if (!bServerTimingValid)
            {
                // No server measurement means no trustworthy render epoch.
                // Fail closed rather than letting ExactPing select the sample.
                bRenderChkPass = false;
                RenderChkMissBy = BIG_NUMBER;
            }
            else
            {
                const float RenderT = FMath::Clamp(RenderMs * 0.001f, 0.f, 0.25f);
                const FVector RenderLoc = BestTarget->GetRewindLocation(RenderT);
                const float RenderColRadius = BestTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
                const float RenderColHeight = BestTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

                // Position history stores location only — not capsule posture. Test
                // the ray against BOTH plausible postures and take the best: the
                // full standing capsule always (it strictly contains the slide-
                // adjusted capsule at the same anchor, so it also covers a target
                // that WAS sliding at render time), plus the slide-adjusted capsule
                // when the target is currently sliding (its recorded anchor may
                // already reflect slide posture). A mandatory reject must not
                // hinge on posture we cannot reconstruct.
                auto RenderMissBy = [&](const FVector& CapsuleCentre, float HalfHeight) -> float
                {
                    FVector ClosestOnRay(0.f);
                    FVector ClosestOnCapsule = CapsuleCentre;
                    float EffRadius;
                    if (RenderColRadius >= HalfHeight)
                    {
                        ClosestOnRay = FMath::ClosestPointOnSegment(CapsuleCentre, StartLocation, Hit.Location);
                        EffRadius = HalfHeight;
                    }
                    else
                    {
                        const FVector Seg(0.f, 0.f, HalfHeight - RenderColRadius);
                        FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location,
                            CapsuleCentre - Seg, CapsuleCentre + Seg, ClosestOnRay, ClosestOnCapsule);
                        EffRadius = RenderColRadius;
                    }
                    return FVector::Dist(ClosestOnRay, ClosestOnCapsule) - (EffRadius + TraceRadius);
                };

                RenderChkMissBy = RenderMissBy(RenderLoc, RenderColHeight);
                if (BestTarget->UTCharacterMovement && BestTarget->UTCharacterMovement->bIsFloorSliding)
                {
                    FVector SlideLoc = RenderLoc;
                    SlideLoc.Z = SlideLoc.Z - RenderColHeight + BestTarget->SlideTargetHeight;
                    RenderChkMissBy = FMath::Min(RenderChkMissBy,
                        RenderMissBy(SlideLoc, BestTarget->SlideTargetHeight));
                }
                bRenderChkPass = RenderChkMissBy <=
                    FMath::Max(0.f, CVarUnclaimedRenderSlack.GetValueOnGameThread());
            }

            if (!bRenderChkPass && UnclaimedRenderGate > 0)
            {
                // Demote to the world impact computed above; beams and impact
                // effects still terminate correctly (same shape as the reverted
                // 2026-07-18 gate's rejection, but render-reconstructed instead
                // of claim-presence-based, and ping-independent).
                // [RenderGate] keeps its own tag so [HitAttrib] parsers never
                // double-count a demoted shot. Verbose since 2026-08-06 (was
                // Log): silent on live by default; the checklist blocker-5
                // verification pass re-enables it with `Log LogUTWeaponFix
                // Verbose` at the server console — no rebuild needed.
                UE_LOG(LogUTWeaponFix, Verbose,
                    TEXT("[RenderGate] DEMOTED %s: missed render-time capsule by %.1fuu (serverRTT %.0f, renderMs %.1f, timingValid=%d)"),
                    *BestTarget->GetName(), RenderChkMissBy, ServerRTTMs,
                    RenderMs, bServerTimingValid ? 1 : 0);
                RenderChkDemotedTarget = BestTarget;
                bRenderChkDemoted = true;
                bLastUnclaimedRenderDemoted = true;
                BestTarget = nullptr;
                BestPoint = FVector::ZeroVector;
                BestCapsulePoint = FVector::ZeroVector;
                BestCollisionRadius = 0.f;
                LastHitscanPaddedRadius = 0.f;
            }
        }
    }

    // The primary pawn loop above already substituted the render-time sample,
    // so no second pass is needed. Prevent a later raw-time head-sphere rescue
    // from violating render authority when no rendered capsule was selected.
    if (bRenderAuthoritativeTargeting)
    {
        bLastUnclaimedRenderDemoted = (BestTarget == nullptr);
        if (bRenderAuthorityDebug && BestTarget != nullptr)
        {
            UE_LOG(LogUTWeaponFix, Verbose,
                TEXT("[RenderAuthority] selected=%s (serverRTT %.0f, clientPing %.0f, extraMs %.1f, estimateMs %.1f, sampleMs %.1f, slack %.1f)"),
                *BestTarget->GetName(), RenderAuthorityRTTMs,
                UTOwner->PlayerState->ExactPing,
                RenderAuthorityExtraMs, RenderAuthoritativeMs,
                RenderAuthoritativeTime * 1000.f,
                RenderAuthoritativeSlack);
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

        const float ClientReportedPing = (UTOwner && UTOwner->PlayerState)
            ? UTOwner->PlayerState->ExactPing : 0.f;
        const AUTPlayerController* ShooterPC = UTOwner
            ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
        float ShooterRTTMs = 0.f;
        bool bShooterTimingValid = false;
        if (ShooterPC != nullptr && !ShooterPC->IsLocalController())
        {
            bShooterTimingValid =
                GetServerObservedRTTMs(ShooterPC, ShooterRTTMs);
        }
        else
        {
            // Listen host/bot timing is not supplied by a remote client.
            ShooterRTTMs = ClientReportedPing;
            bShooterTimingValid = true;
        }
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
        const float RenderEstMs = bShooterTimingValid
            ? ShooterRTTMs * 0.5f +
                FMath::Max(0.f, CVarHitAttribRenderExtraMs.GetValueOnGameThread())
            : 0.f;
        FString LeadStr(TEXT("na"));
        FString DeltaMagStr(TEXT("na"));
        if (AttribTarget != nullptr && bShooterTimingValid)
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

        // Rescue lead gate verdict (appended fields — existing parsers anchor on
        // earlier tokens and are unaffected). Taxonomy:
        //   pass        accepted rescue, credited lead within cap
        //   shadow-fail accepted rescue OVER the cap (shadow only — the
        //               enforcement headline; enforce never accepts one)
        //   blocked     enforce denied >=1 geometrically-hitting rung and no
        //               rung was accepted (rescueSkips carries the count)
        //   nohit       search found nothing on its own; gate irrelevant
        //   no-timing / blocked-no-timing   RTT unmeasured (never counted as a
        //               measured over-cap rescue; enforce fails closed)
        // rescueLeadUU is the ACCEPTED rung's credited lead (vs the render-epoch
        // estimate along historical motion) and describes the CLAIMED target —
        // it equals the legacy leadUU field only on route=timesearch-rescue
        // rows (rescueSame flags target identity for every other row).
        const FString RescueLeadStr = !bRescueLeadApplicable ? TEXT("na")
            : (!bRescueLeadTimingValid
                ? (bRescueLeadSearchBlockedNoTiming ? TEXT("blocked-no-timing") : TEXT("no-timing"))
                : (bRescueLeadAcceptedValid
                    ? ((RescueLeadAcceptedUU <= RescueLeadCapApplied) ? TEXT("pass") : TEXT("shadow-fail"))
                    : (RescueLeadRungsSkipped > 0 ? TEXT("blocked") : TEXT("nohit"))));
        const FString RescueLeadUUStr = bRescueLeadAcceptedValid
            ? FString::Printf(TEXT("%.1f"), RescueLeadAcceptedUU) : TEXT("na");
        const FString RescueRayAheadStr = bRescueLeadRayValid
            ? FString::Printf(TEXT("%.1f"), RescueLeadRayAheadUU) : TEXT("na");
        const FString RescueRenderMissStr = (bRescueLeadApplicable && bRescueLeadTimingValid)
            ? FString::Printf(TEXT("%.1f"), RescueLeadRenderMissUU) : TEXT("na");
        const FString RescueDMagStr = (bRescueLeadApplicable && bRescueLeadTimingValid)
            ? FString::Printf(TEXT("%.1f"), RescueLeadHistDMag) : TEXT("na");
        const int32 RescueSame = (AttribTarget != nullptr &&
            AttribTarget == ReceivedHitScanHitChar) ? 1 : 0;

        // UE4.15's FMsg::Logf_Internal overload set has a fixed argument-count
        // ceiling. Assemble the record in bounded chunks, then emit the exact
        // same single searchable line through a one-argument UE_LOG call.
        FString HitAttribLog = FString::Printf(
            TEXT("[HitAttrib] shooter=%s ping=%.0f clientPing=%.0f timingValid=%d wep=%s mode=%d rewindMs=%.1f claim=%s target=%s speed=%.0f route=%s"),
            *AttribName(UTOwner), ShooterRTTMs, ClientReportedPing,
            bShooterTimingValid ? 1 : 0, *GetClass()->GetName(), CurrentFireMode,
            ActualPredictionTime * 1000.f, *ClaimStr, *AttribName(AttribTarget),
            AttribTarget ? AttribTarget->GetVelocity().Size() : 0.f,
            Route);
        HitAttribLog += FString::Printf(
            TEXT(" claimMissBy=%s pad=%.0f tsRungMs=%s tsMissBy=%s renderEstMs=%.1f leadUU=%s dMag=%s renderChk=%s renderChkMissBy=%s"),
            *ClaimMissStr, AttribClaimPad, *TsRungStr, *TsMissStr,
            RenderEstMs, *LeadStr, *DeltaMagStr, *RenderChkStr, *RenderChkMissStr);
        HitAttribLog += FString::Printf(
            TEXT(" rescueLead=%s rescueLeadUU=%s rescueRayAheadUU=%s rescueRenderMissUU=%s rescueDMag=%s rescueSkips=%d rescueSame=%d"),
            *RescueLeadStr, *RescueLeadUUStr, *RescueRayAheadStr,
            *RescueRenderMissStr, *RescueDMagStr, RescueLeadRungsSkipped, RescueSame);
        UE_LOG(LogUTWeaponFix, Log, TEXT("%s"), *HitAttribLog);
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
    // Buffered Shock uses the explicit scope because exact ZeroRotator is a
    // legitimate +X aim. Other weapons retain the pre-existing transaction
    // gate and non-zero sentinel behavior.
    FRotator BaseAim;

    if (HasScopedTransactionalAim(this)
        || (Role == ROLE_Authority && bIsTransactionalFire && !CachedTransactionalRotation.IsZero()))
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
    // Buffered Shock uses scoped validity for exact ZeroRotator. Preserve the
    // existing non-zero cache behavior for every other logical shot.
    if (HasScopedTransactionalAim(this)
        || (Role == ROLE_Authority && bIsTransactionalFire && !CachedTransactionalRotation.IsZero())
        || (Role < ROLE_Authority && !CachedTransactionalRotation.IsZero()))
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

    // Buffered Shock uses the explicit scope; other projectiles retain the
    // existing transactional/non-zero gate.
    if (bIsProjectile && Role == ROLE_Authority
        && (HasScopedTransactionalAim(this)
            || (bIsTransactionalFire && !CachedTransactionalRotation.IsZero()))
        && UTOwner)
    {
        // Keep projectile-origin timing on the legacy per-weapon field. The
        // server-live hitscan cvar must not alter projectile presentation.
        float PredictionTime = GetPredictionTimeWithFudgeMs(FudgeFactorMs);

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
	// Legacy non-Flak path. Kept unchanged while ncp.RocketPrimaryDiag establishes
	// whether the M1 symptom is cosmetic prediction delay or authoritative cadence loss.
	if (RocketPrimaryDiagFor(this, 0, 2))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] FAKE_DELAY_CALLBACK frame=%u t=%.4f role=%d net=%d currentMode=%d class=%s timerActive=%d timerRate=%.4f timerRemain=%.4f pendingFakes=%d"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			(int32)Role, (int32)GetNetMode(), CurrentFireMode,
			NetcodeDelayedProjectile.ProjectileClass ? *NetcodeDelayedProjectile.ProjectileClass->GetName() : TEXT("null"),
			GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle) ? 1 : 0,
			GetWorldTimerManager().GetTimerRate(SpawnDelayedFakeProjHandle),
			GetWorldTimerManager().GetTimerRemaining(SpawnDelayedFakeProjHandle),
			PendingFakeProjectiles.Num());
	}
	if (NetcodeDelayedProjectile.ProjectileClass != nullptr)
	{
		SpawnNetPredictedProjectile(NetcodeDelayedProjectile.ProjectileClass, NetcodeDelayedProjectile.SpawnLocation, NetcodeDelayedProjectile.SpawnRotation);
	}
}

void AUTWeaponFix::SpawnDelayedFlakFakeProjectile(uint32 ReservationId)
{
    int32 RequestIndex = INDEX_NONE;
    for (int32 i = 0; i < DelayedFlakProjectiles.Num(); ++i)
    {
        if (DelayedFlakProjectiles[i].ReservationId == ReservationId)
        {
            RequestIndex = i;
            break;
        }
    }

    if (RequestIndex == INDEX_NONE)
    {
        return; // ACK/cleanup won the race and cancelled this request.
    }

    // Copy before RemoveAtSwap: timer delegates carry only the stable ID, never an array
    // element reference that could have been invalidated by another shard reservation.
    const FNetcodeDelayedFlakProjectile Request = DelayedFlakProjectiles[RequestIndex];
    DelayedFlakProjectiles.RemoveAtSwap(RequestIndex, 1, false);

    SpawnNetPredictedProjectileInternal(
        Request.ProjectileClass,
        Request.SpawnLocation,
        Request.SpawnRotation,
        Request.FireMode,
        Request.EventIndex,
        false); // direct spawn: the callback never re-enters the excess-ping decision
}

void AUTWeaponFix::ClearDelayedFlakFakeProjectiles()
{
    for (FNetcodeDelayedFlakProjectile& Request : DelayedFlakProjectiles)
    {
        GetWorldTimerManager().ClearTimer(Request.TimerHandle);
    }
    DelayedFlakProjectiles.Empty();
}


AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectile(
	TSubclassOf<AUTProjectile> ProjectileClass,
	FVector SpawnLocation,
	FRotator SpawnRotation)
{
	const uint8 CapturedFireMode = CurrentFireMode;
	const int32 CapturedEventIndex = ClientFireEventIndex.IsValidIndex(CapturedFireMode)
		? ClientFireEventIndex[CapturedFireMode]
		: INDEX_NONE;

	return SpawnNetPredictedProjectileInternal(
		ProjectileClass,
		SpawnLocation,
		SpawnRotation,
		CapturedFireMode,
		CapturedEventIndex,
		true);
}

AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectileInternal(
	TSubclassOf<AUTProjectile> ProjectileClass,
	FVector SpawnLocation,
	FRotator SpawnRotation,
	uint8 CapturedFireMode,
	int32 CapturedEventIndex,
	bool bAllowDelay)
{
	// Pitch clamp for shells/rockets firing straight down
	FRotator AdjustedRot = SpawnRotation;
	AdjustedRot.Normalize();
    bool bIsShockCore = ProjectileClass &&
        ProjectileClass->IsChildOf(AUTPlusProj_ShockBall::StaticClass());
    bool bIsFlakShell = ProjectileClass &&
        ProjectileClass->IsChildOf(AUTProj_FlakShell::StaticClass());
    const bool bIsPrimaryFlakShard = CapturedFireMode == 0 && ProjectileClass &&
        ProjectileClass->IsChildOf(AUTProj_FlakShard::StaticClass());

    // Preserve the existing Shock guard exactly. Flak shell duplicate prevention is
    // intentionally deferred until the actor is actually about to spawn below.
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
	if (bAllowDelay && (Role != ROLE_Authority) && OwningPlayer)
	{
		float ExcessPing = CurrentPing - FudgeFactorMs - ProjectilePredictionCapMs;

		if (ExcessPing > 10.0f)  // More than 10ms over cap
		{
			float SleepTime = ExcessPing * 0.001f;

			if (bIsPrimaryFlakShard || bIsFlakShell)
			{
				// One shell reservation per logical event; unlike the shell, every primary
				// shard is intentional and must receive its own request despite sharing the
				// same event index.
				if (bIsFlakShell)
				{
					for (const FNetcodeDelayedFlakProjectile& Existing : DelayedFlakProjectiles)
					{
						if (Existing.Kind == ENetcodeDelayedFlakKind::SecondaryShell
							&& Existing.FireMode == CapturedFireMode
							&& Existing.EventIndex == CapturedEventIndex
							&& Existing.ProjectileClass == ProjectileClass)
						{
							return nullptr;
						}
					}
				}

				int32 ProjectileOrdinal = 0;
				const ENetcodeDelayedFlakKind Kind = bIsFlakShell
					? ENetcodeDelayedFlakKind::SecondaryShell
					: ENetcodeDelayedFlakKind::PrimaryShard;
				for (const FNetcodeDelayedFlakProjectile& Existing : DelayedFlakProjectiles)
				{
					if (Existing.Kind == Kind
						&& Existing.FireMode == CapturedFireMode
						&& Existing.EventIndex == CapturedEventIndex)
					{
						ProjectileOrdinal = FMath::Max(ProjectileOrdinal, Existing.ProjectileOrdinal + 1);
					}
				}

				const int32 AddedIndex = DelayedFlakProjectiles.AddDefaulted();
				FNetcodeDelayedFlakProjectile& Request = DelayedFlakProjectiles[AddedIndex];
				Request.ProjectileClass = ProjectileClass;
				Request.SpawnLocation = SpawnLocation;
				Request.SpawnRotation = SpawnRotation;
				Request.FireMode = CapturedFireMode;
				Request.EventIndex = CapturedEventIndex;
				Request.ReservationId = NextDelayedFlakReservationId++;
				if (NextDelayedFlakReservationId == 0)
				{
					NextDelayedFlakReservationId = 1;
				}
				Request.ProjectileOrdinal = ProjectileOrdinal;
				Request.RequestTime = GetWorld()->GetTimeSeconds();
				Request.Kind = Kind;

				FTimerDelegate DelayedDelegate = FTimerDelegate::CreateUObject(
					this,
					&AUTWeaponFix::SpawnDelayedFlakFakeProjectile,
					Request.ReservationId);
				GetWorldTimerManager().SetTimer(Request.TimerHandle, DelayedDelegate, SleepTime, false);
				return nullptr;
			}

			// Legacy non-Flak behavior remains available for the rocket diagnostic run.
			const bool bLegacyTimerAlreadyActive = GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle);
			if (RocketPrimaryDiagFor(this, CapturedFireMode, 2))
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("[RocketM1Diag] FAKE_DELAY_DECISION frame=%u t=%.4f event=%d mode=%d ping=%.1f excess=%.1f sleep=%.4f action=%s existingClass=%s newClass=%s timerRate=%.4f timerRemain=%.4f"),
					(uint32)GFrameCounter, GetWorld()->GetTimeSeconds(), CapturedEventIndex, CapturedFireMode,
					CurrentPing, ExcessPing, SleepTime,
					bLegacyTimerAlreadyActive ? TEXT("SUPPRESS_SHARED_TIMER_BUSY") : TEXT("ARM_SHARED_TIMER"),
					NetcodeDelayedProjectile.ProjectileClass ? *NetcodeDelayedProjectile.ProjectileClass->GetName() : TEXT("null"),
					ProjectileClass ? *ProjectileClass->GetName() : TEXT("null"),
					GetWorldTimerManager().GetTimerRate(SpawnDelayedFakeProjHandle),
					GetWorldTimerManager().GetTimerRemaining(SpawnDelayedFakeProjHandle));
			}
			if (!bLegacyTimerAlreadyActive)
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

	// The reservation is the pre-spawn duplicate guard. Only an actual spawn may
	// consume the wall-clock guard; scheduling or a failed SpawnActor must not.
	if (bIsFlakShell)
	{
		const float CurrentTime = GetWorld()->GetTimeSeconds();
		const float TimeSinceLast = CurrentTime - LastFlakShellSpawnTime;
		if (LastFlakShellSpawnTime > 0.f && TimeSinceLast < 0.2f)
		{
			if (FireDbg())
			{
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("FlakShell anti-dup guard BLOCKED actual spawn. TimeSinceLast=%.4f Role=%d mode=%d event=%d"),
					TimeSinceLast, (int32)Role, CapturedFireMode, CapturedEventIndex);
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
		if (RocketPrimaryDiagFor(this, CapturedFireMode, 2))
		{
			UE_LOG(LogUTWeaponFix, Warning,
				TEXT("[RocketM1Diag] PROJECTILE_SPAWN_FAIL frame=%u t=%.4f wep=%p player=%s role=%d net=%d event=%d capturedEvent=%d mode=%d class=%s allowDelay=%d"),
				(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
				this, RocketPrimaryDiagPlayer(this), (int32)Role, (int32)GetNetMode(),
				Role == ROLE_Authority && AuthoritativeFireEventIndex.IsValidIndex(CapturedFireMode)
					? AuthoritativeFireEventIndex[CapturedFireMode] : CapturedEventIndex,
				CapturedEventIndex, CapturedFireMode,
				ProjectileClass ? *ProjectileClass->GetName() : TEXT("null"), bAllowDelay ? 1 : 0);
		}
		return nullptr;
	}
	if (RocketPrimaryDiagFor(this, CapturedFireMode, 2))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] PROJECTILE_SPAWN_OK frame=%u t=%.4f wep=%p player=%s role=%d net=%d local=%d event=%d capturedEvent=%d mode=%d projectile=%s class=%s allowDelay=%d ping=%.1f catchup=%.4f"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			this, RocketPrimaryDiagPlayer(this), (int32)Role, (int32)GetNetMode(),
			(UTOwner && UTOwner->IsLocallyControlled()) ? 1 : 0,
			Role == ROLE_Authority && AuthoritativeFireEventIndex.IsValidIndex(CapturedFireMode)
				? AuthoritativeFireEventIndex[CapturedFireMode] : CapturedEventIndex,
			CapturedEventIndex, CapturedFireMode, *NewProjectile->GetName(), *NewProjectile->GetClass()->GetName(),
			bAllowDelay ? 1 : 0, CurrentPing, CatchupTickDelta);
	}

	if (bIsFlakShell)
	{
		LastFlakShellSpawnTime = GetWorld()->GetTimeSeconds();
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

	// Record what this weapon fires, on whichever side we are. The claim route and the hitsound
	// prediction both need to get from a projectile back to the weapon that launched it, and
	// GetWeapon() cannot answer that once the shooter has switched weapons mid-flight.
	if (NewProjectile != nullptr)
	{
		NCPFiredProjClasses.AddUnique(NewProjectile->GetClass());
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
			ActiveServerProjectiles.Add(FActiveServerProjectile(NewProjectile, CapturedFireMode));

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

						// Posture at the REWOUND moment, not now: a target who started a
						// floor slide between the client's shot and this validation must be
						// tested with the same slide-grace envelope hitscan uses, or the
						// shrunken live capsule turns their rendered torso into a false
						// miss. Mutates RewoundLoc/CapHeight in place; no-op unless sliding.
						ApplySlidePostureForValidation(Target, SampleRewindTime, RewoundLoc, CapHeight);

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

						// Same slide-posture correction as the spawn sweep above.
						ApplySlidePostureForValidation(Target, RewindTime, RewoundLoc, CapHeight);

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

    // Track against the immutable logical shot captured before any high-ping delay.
    // CurrentFireMode and ClientFireEventIndex may have moved on while the timer slept.
    PendingFakeProjectiles.Add(FPendingFakeProjectile(NewProjectile, CapturedEventIndex, CapturedFireMode));

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
    // bLastUnclaimedRenderDemoted: HitScanTrace demoted this ray's pawn hit at
    // the render check — the head-sphere fallback must not resurrect it.
    if (UTPC && bCheckHeadSphere && !bLastUnclaimedRenderDemoted &&
        (Cast<AUTCharacter>(Hit.Actor.Get()) == nullptr) &&
        ((Spread.Num() <= GetCurrentFireMode()) || (Spread[GetCurrentFireMode()] == 0.f)) &&
        (UTOwner->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere))
    {
        AUTCharacter* AltTarget = Cast<AUTCharacter>(UUTGameplayStatics::ChooseBestAimTarget(
            UTPC, SpawnLocation, FireDir, 0.7f, (Hit.Location - SpawnLocation).Size(),
            150.f, AUTCharacter::StaticClass()));
        if (IsLiveHitscanTarget(AltTarget) &&
            (AltTarget->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere) &&
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
    AUTCharacter* HitCharacter = Cast<AUTCharacter>(Hit.Actor.Get());
    if (Hit.Actor != nullptr && Hit.Actor->bCanBeDamaged && bDealDamage &&
        (HitCharacter == nullptr || IsLiveHitscanTarget(HitCharacter)))
    {
        // Detonating a damageable projectile (your own shock core for a combo, or
        // shooting down an enemy core/rocket) still deals the damage below, but it
        // is NOT a landed hit on a player — counting it inflated shock beam
        // accuracy (2026-08-10). The same rule runs in every hitscan credit site
        // (cone sweep below, UTPlusSniper, link beam). Pawns keep counting.
        if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None)
            && Cast<AUTProjectile>(Hit.Actor.Get()) == nullptr)
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
	StopShockInputTrace();
    GetWorldTimerManager().ClearTimer(DeferredActiveStateHandle);
    GetWorldTimerManager().ClearTimer(DelayedPutDownHandle);
	ClearFireEventsFixed();
	ClearDelayedFlakFakeProjectiles();
    // Safety: Kill timers if the weapon is destroyed or dropped
    for (int32 i = 0; i < 2; i++)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
        bBufferedClickPending[i] = false;
    }
    ScopedTransactionalAimWeapons.Remove(this);
    CachedTransactionalRotation = FRotator::ZeroRotator;
    ClearPendingFakeProjectiles();
	DestroyFirstPersonHologramDepthMesh();
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
		StopShockInputTrace();
		// The original fixed fire RPCs are Reliable. Stop only NetcodePlus's
		// application-level retry copies at the outgoing equip boundary.
		ClearFireEventsFixed();

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
                    // Buffered clicks are spent input, not held intent — graduating
                    // one would fire a ghost shot on the next weapon.
                    if (!bCrossModeRetryArmed[i] && !bBufferedClickPending[i])
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
        // A) Kill any pending retry timers (a buffered click dies with the switch)
        for (int32 i = 0; i < 2; i++)
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
            bBufferedClickPending[i] = false;
        }
        ScopedTransactionalAimWeapons.Remove(this);
        CachedTransactionalRotation = FRotator::ZeroRotator;
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
        // Characters are validated exactly once by the rewind-aware pawn pass
        // below. In particular, never let a retained corpse's mesh/component
        // re-enter through this generic shootable-object overlap.
        if (Overlap.GetActor() != nullptr &&
            Cast<AUTCharacter>(Overlap.GetActor()) == nullptr)
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
        if (Target != UTOwner && IsLiveHitscanTarget(Target) &&
            (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target)))
        {
            // find appropriate rewind position, and test against trace from StartLocation to Hit.Location
            // NOTE: This uses GetRewindLocation, which in your Character override respects 'PredictionTime' on the server
            FVector TargetLocation = ((PredictionTime > 0.f) && (Role == ROLE_Authority)) ? Target->GetRewindLocation(PredictionTime) : Target->GetActorLocation();

            const FVector Diff = TargetLocation - SpawnLocation;
            if (Diff.Size() <= InstantHitInfo[CurrentFireMode].TraceRange && (Diff.GetSafeNormal() | FireDir) >= InstantHitInfo[CurrentFireMode].ConeDotAngle)
            {
                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                ApplySlidePostureForValidation(Target,
                    ((PredictionTime > 0.f) && (Role == ROLE_Authority)) ? PredictionTime : 0.f,
                    TargetLocation, CollisionHeight);
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
                const float CapsuleSurfaceRadius =
                    FMath::Min(CollisionRadius, CollisionHeight);

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
                float BackDist = FMath::Sqrt(FMath::Max(0.f,
                    CapsuleSurfaceRadius * CapsuleSurfaceRadius - ClosestDistSq));
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
        AUTCharacter* HitCharacter = Cast<AUTCharacter>(Hit.Actor.Get());
        if (UTOwner && Hit.Actor != NULL && Hit.Actor->bCanBeDamaged &&
            (HitCharacter == nullptr || IsLiveHitscanTarget(HitCharacter)))
        {
            // No accuracy credit for detonating projectiles — see FireInstantHit.
            if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None)
                && Cast<AUTProjectile>(Hit.Actor.Get()) == nullptr)
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
	//   Lightning Gun — the one-material FALLBACK is asymmetric. PinkLG 1P
	//     MAT_INS_LG_Pink_E0_1p uses the PartTWO
	//     textures (T_LightingGunTwo_*), which Lightning_Gun_1p carries on slot 0.
	//     PinkLG 3P MAT_INS_LG_Pink_E1_3p uses the PartONE textures
	//     (T_LightingGun_one_*), which Lightning_Gun_3p carries on slot 1. The authored
	//     E0/E1 names are the element indices -> 1P {0}, 3P {1}. Writing the other slot
	//     would paint that section with the wrong part's textures. The resolved-skin
	//     layer expands exact PinkLG to {0,1} and supplies the complementary material.
	//   Everything else keeps stock behaviour: slot 0 only.
	// Normal skins do not write slots outside this base mask, so ammo counters, decals,
	// glass, and SetupSpecialMaterials()' Shock Rifle screen keep their originals. Slot
	// NAMES on these meshes are unreliable (scrambled/generic), which is why these are
	// explicit verified indices.
	static const FName NAME_FlakCannonSkins(TEXT("FlakCannon_Skins"));
	// Accept both legacy/current spellings while existing cooked content is harmonised.
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

static bool UsesAuthenticInvisibilityMaterial(const UMaterialInterface* Material)
{
	static const FString InvisibilityBaseMaterialPath(
		TEXT("/Game/RestrictedAssets/Pickups/Powerups/Assets/M_Invis_Skin.M_Invis_Skin"));
	static const FString NCPInvisibilityBaseMaterialPath(
		TEXT("/Game/NetcodePlusOptional/M_Invis_SkinNCP.M_Invis_SkinNCP"));
	const UMaterial* const BaseMaterial = (Material != nullptr) ? Material->GetMaterial() : nullptr;
	if (BaseMaterial == nullptr)
	{
		return false;
	}

	const FString BaseMaterialPath = BaseMaterial->GetPathName();
	return BaseMaterialPath == InvisibilityBaseMaterialPath ||
		BaseMaterialPath == NCPInvisibilityBaseMaterialPath;
}

static bool UsesPickupHologramMaterial(const UMaterialInterface* Material)
{
	static const FString PickupHologramBaseMaterialPath(
		TEXT("/Game/RestrictedAssets/Weapons/Weapon_Base_Effects/Materials/M_HoloEffect.M_HoloEffect"));
	const UMaterial* const BaseMaterial = (Material != nullptr) ? Material->GetMaterial() : nullptr;
	return BaseMaterial != nullptr && BaseMaterial->GetPathName() == PickupHologramBaseMaterialPath;
}

static bool IsFirstPersonHologramWeaponSkin(const UUTWeaponSkin* Skin)
{
	if (Skin == nullptr)
	{
		return false;
	}

	// Exact allow-list: these DataAssets are clones of the established Invisible*
	// entries so their weapon class/tag compatibility and stock 3P material stay
	// intact. C++ replaces only their 1P material with the stock pickup hologram.
	const FString Path = Skin->GetPathName();
	return Path == TEXT("/Game/NetcodePlusOptional/GhostBio.GhostBio") ||
		Path == TEXT("/Game/NetcodePlusOptional/GhostFlak.GhostFlak") ||
		Path == TEXT("/Game/NetcodePlusOptional/GhostIGRifle.GhostIGRifle") ||
		Path == TEXT("/Game/NetcodePlusOptional/GhostLG.GhostLG") ||
		Path == TEXT("/Game/NetcodePlusOptional/GhostLinkElim.GhostLinkElim");
}

static UMaterialInterface* GetPickupHologramMaterial()
{
	static TWeakObjectPtr<UMaterialInterface> CachedMaterial;
	if (!CachedMaterial.IsValid())
	{
		// BP_UDamage supplies the inventory/mesh, while PowerupBase supplies this
		// lavender M_HoloEffect child for its unavailable pickup ghost. That is the
		// exact visual reference used for the five held-weapon variants.
		static const TCHAR* const MaterialPath =
			TEXT("/Game/RestrictedAssets/Weapons/Weapon_Base_Effects/Materials/M_HoloEffect_Powerup.M_HoloEffect_Powerup");
		CachedMaterial = LoadObject<UMaterialInterface>(nullptr, MaterialPath);
		if (!CachedMaterial.IsValid())
		{
			static bool bLoggedMissingMaterial = false;
			if (!bLoggedMissingMaterial)
			{
				bLoggedMissingMaterial = true;
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("First-person hologram material is missing: %s"), MaterialPath);
			}
		}
	}
	return CachedMaterial.Get();
}

static bool IsPinkLGWeaponSkin(const UUTWeaponSkin* Skin)
{
	static const FString PinkLGPath(TEXT("/Game/NetcodePlusOptional/PinkLG.PinkLG"));
	return Skin != nullptr && Skin->GetPathName() == PinkLGPath;
}

static uint32 MakeLowestMaterialSlotMask(int32 MaterialSlotCount)
{
	const int32 ClampedSlotCount = FMath::Clamp(MaterialSlotCount, 0,
		AUTWeaponFix::MaxWeaponSkinTargetSlots);
	return (ClampedSlotCount == AUTWeaponFix::MaxWeaponSkinTargetSlots)
		? MAX_uint32
		: ((ClampedSlotCount > 0) ? ((1u << ClampedSlotCount) - 1u) : 0u);
}

uint32 AUTWeaponFix::GetResolvedWeaponSkinTargetSlotMask(const UUTWeaponSkin* Skin,
	FName WeaponSkinCustomizationTag, bool bFirstPersonMesh, int32 MaterialSlotCount)
{
	if (IsFirstPersonHologramWeaponSkin(Skin))
	{
		// Ghost skins are deliberately first-person only. Returning zero for the
		// attachment path restores its captured stock materials, while every live
		// 1P slot participates in the hologram/depth pair.
		return (bFirstPersonMesh && GetPickupHologramMaterial() != nullptr)
			? MakeLowestMaterialSlotMask(MaterialSlotCount)
			: 0u;
	}

	const UMaterialInterface* const ViewMaterial = (Skin == nullptr)
		? nullptr
		: (bFirstPersonMesh ? Skin->FPSMaterial : Skin->Material);
	if ((UsesAuthenticInvisibilityMaterial(ViewMaterial) ||
		 (bFirstPersonMesh && UsesPickupHologramMaterial(ViewMaterial))) &&
		MaterialSlotCount > 0)
	{
		return MakeLowestMaterialSlotMask(MaterialSlotCount);
	}
	if (IsPinkLGWeaponSkin(Skin))
	{
		return MakeLowestMaterialSlotMask(FMath::Min(MaterialSlotCount, 2));
	}

	return GetWeaponSkinTargetSlotMask(WeaponSkinCustomizationTag, bFirstPersonMesh);
}

UMaterialInterface* AUTWeaponFix::GetResolvedWeaponSkinMaterialForSlot(
	const UUTWeaponSkin* Skin, bool bFirstPersonMesh, int32 MaterialSlot)
{
	if (IsFirstPersonHologramWeaponSkin(Skin))
	{
		return bFirstPersonMesh ? GetPickupHologramMaterial() : nullptr;
	}

	UMaterialInterface* const ViewMaterial = (Skin == nullptr)
		? nullptr
		: (bFirstPersonMesh ? Skin->FPSMaterial : Skin->Material);
	if (!IsPinkLGWeaponSkin(Skin) || UsesAuthenticInvisibilityMaterial(ViewMaterial))
	{
		return ViewMaterial;
	}

	// PinkLG's data asset supplies E0_1p and E1_3p. MutAnnouncers holds the cook
	// references for the opposite elements; load them only after mounted selection.
	if (bFirstPersonMesh && MaterialSlot == 1)
	{
		static const TCHAR* const MaterialPath =
			TEXT("/Game/Blueprints/UT+/UT+/UTPlusNew/MAT_INS_LG_Pink_E1_1p.MAT_INS_LG_Pink_E1_1p");
		UMaterialInstance* const Material =
			LoadObject<UMaterialInstance>(nullptr, MaterialPath);
		if (Material == nullptr)
		{
			static bool bLoggedMissingFirstPersonMaterial = false;
			if (!bLoggedMissingFirstPersonMaterial)
			{
				bLoggedMissingFirstPersonMaterial = true;
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("PinkLG missing E1_1p material: %s"), MaterialPath);
			}
		}
		return Material;
	}
	if (!bFirstPersonMesh && MaterialSlot == 0)
	{
		static const TCHAR* const MaterialPath =
			TEXT("/Game/Blueprints/UT+/UT+/UTPlusNew/MAT_INS_LG_Pink_E0_3p.MAT_INS_LG_Pink_E0_3p");
		UMaterialInstance* const Material =
			LoadObject<UMaterialInstance>(nullptr, MaterialPath);
		if (Material == nullptr)
		{
			static bool bLoggedMissingThirdPersonMaterial = false;
			if (!bLoggedMissingThirdPersonMaterial)
			{
				bLoggedMissingThirdPersonMaterial = true;
				UE_LOG(LogUTWeaponFix, Warning,
					TEXT("PinkLG missing E0_3p material: %s"), MaterialPath);
			}
		}
		return Material;
	}

	return ViewMaterial;
}

void AUTWeaponFix::ApplyResolvedWeaponSkin(UUTWeaponSkin* Skin)
{
	const bool bFirstPersonHologramSelection =
		IsFirstPersonHologramWeaponSkin(Skin);
	UMaterialInterface* const FirstPersonSkinMaterial =
		(GetNetMode() != NM_DedicatedServer)
		? (bFirstPersonHologramSelection
			? GetPickupHologramMaterial()
			: ((Skin != nullptr) ? Skin->FPSMaterial : nullptr))
		: nullptr;
	bFirstPersonHologramSkinActive = bSkinsEnabled && FirstPersonSkinMaterial != nullptr &&
		(bFirstPersonHologramSelection ||
		 UsesPickupHologramMaterial(FirstPersonSkinMaterial));

	// Only authority needs the weapon-level identity: it is copied to a dropped
	// pickup. Viewers resolve their materials from the replicated Character array.
	if (Role == ROLE_Authority)
	{
		// Ghost skins are a local 1P presentation. Do not copy their cloned
		// DataAsset material to a dropped pickup; dropped and remote weapons stay
		// stock, while the replicated Character selection re-applies 1P on equip.
		WeaponSkin = bFirstPersonHologramSelection ? nullptr : Skin;
	}
	if (!bSkinsEnabled || GetNetMode() == NM_DedicatedServer || Mesh == nullptr ||
		Mesh->GetNumMaterials() < 1)
	{
		UpdateFirstPersonHologramDepthMesh(false);
		if (GetNetMode() != NM_DedicatedServer && Mesh != nullptr && Mesh->IsRegistered())
		{
			UpdateOutline();
		}
		return;
	}

	const int32 MaterialSlotCount = FMath::Min(Mesh->GetNumMaterials(),
		MaxWeaponSkinTargetSlots);
	const uint32 TargetSlotMask = GetResolvedWeaponSkinTargetSlotMask(Skin,
		WeaponSkinCustomizationTag, /*bFirstPersonMesh=*/true, MaterialSlotCount);

	// Capture every original once. Ordinary skins still write only their family mask;
	// the full cache is needed so an invisibility skin can cover every mesh section and
	// later restore counters, decals, glass, screens, and multipart weapon sections.
	while (SavedMeshMaterials.Num() < Mesh->GetNumMaterials())
	{
		SavedMeshMaterials.Add(Mesh->GetMaterial(SavedMeshMaterials.Num()));
	}
	if (!bCapturedOriginalFPSMaterials ||
		OriginalFPSMaterials.Num() != MaterialSlotCount)
	{
		OriginalFPSMaterials.Empty(MaterialSlotCount);
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			UMaterialInterface* Original = SavedMeshMaterials.IsValidIndex(Slot)
				? SavedMeshMaterials[Slot]
				: Mesh->GetMaterial(Slot);
			if (Original != nullptr && Cast<UMaterialInstanceDynamic>(Original) == nullptr)
			{
				if (UMaterialInstanceDynamic* DefaultMID =
					UMaterialInstanceDynamic::Create(Original, Mesh))
				{
					Original = DefaultMID;
				}
			}
			OriginalFPSMaterials.Add(Original);
		}
		AppliedFPSMaterialInstances.Empty();
		AppliedFPSMaterialParents.Empty();
		AppliedFPSMaterialSlotMask = 0u;
		bCapturedOriginalFPSMaterials = true;
	}

	TArray<UMaterialInterface*> DesiredParents;
	DesiredParents.AddZeroed(MaterialSlotCount);
	if (Skin != nullptr)
	{
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u)
			{
				DesiredParents[Slot] = GetResolvedWeaponSkinMaterialForSlot(
					Skin, /*bFirstPersonMesh=*/true, Slot);
			}
		}
	}
	const uint32 PreviousSlotMask = AppliedFPSMaterialSlotMask;
	bool bMaterialParentsChanged =
		AppliedFPSMaterialParents.Num() != DesiredParents.Num();
	for (int32 Slot = 0; !bMaterialParentsChanged && Slot < MaterialSlotCount; ++Slot)
	{
		bMaterialParentsChanged =
			AppliedFPSMaterialParents[Slot] != DesiredParents[Slot];
	}
	if (bMaterialParentsChanged || TargetSlotMask != AppliedFPSMaterialSlotMask ||
		AppliedFPSMaterialInstances.Num() != MaterialSlotCount)
	{
		AppliedFPSMaterialParents = DesiredParents;
		AppliedFPSMaterialInstances.Empty(MaterialSlotCount);
		AppliedFPSMaterialInstances.AddZeroed(MaterialSlotCount);
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u &&
				DesiredParents[Slot] != nullptr)
			{
				AppliedFPSMaterialInstances[Slot] =
					UMaterialInstanceDynamic::Create(DesiredParents[Slot], Mesh);
				if (bFirstPersonHologramSkinActive &&
					AppliedFPSMaterialInstances[Slot] != nullptr)
				{
					static const FName NAME_Normal(TEXT("Normal"));
					UTexture* NormalTexture = nullptr;
					UMaterialInterface* const OriginalMaterial =
						OriginalFPSMaterials.IsValidIndex(Slot)
						? OriginalFPSMaterials[Slot]
						: nullptr;
					if (OriginalMaterial != nullptr &&
						OriginalMaterial->GetTextureParameterValue(NAME_Normal, NormalTexture))
					{
						AppliedFPSMaterialInstances[Slot]->SetTextureParameterValue(
							NAME_Normal, NormalTexture);
					}
				}
			}
		}
	}
	AppliedFPSMaterialSlotMask = TargetSlotMask;

	// Restore every slot owned by the previous skin but not the new one. This is what
	// makes invisibility -> PinkLG/ordinary/Default transitions lossless.
	const uint32 SlotsToUpdate = PreviousSlotMask | TargetSlotMask;
	const bool bBodyOverrideActive = (UTOwner != nullptr && UTOwner->GetSkin() != nullptr);
	static const FName NAME_Scale(TEXT("Scale"));
	for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
	{
		if (((SlotsToUpdate >> Slot) & 0x1u) == 0u)
		{
			continue;
		}
		const bool bTargetedByNewSkin = ((TargetSlotMask >> Slot) & 0x1u) != 0u;
		UMaterialInterface* const DesiredSlotParent = bTargetedByNewSkin
			? DesiredParents[Slot]
			: nullptr;
		UMaterialInstanceDynamic* const DesiredSlotMID =
			(DesiredSlotParent != nullptr &&
			 AppliedFPSMaterialInstances.IsValidIndex(Slot))
			? AppliedFPSMaterialInstances[Slot]
			: Cast<UMaterialInstanceDynamic>(OriginalFPSMaterials[Slot]);
		UMaterialInterface* const DesiredSlotMaterial =
			(DesiredSlotParent != nullptr)
			? ((DesiredSlotMID != nullptr)
				? Cast<UMaterialInterface>(DesiredSlotMID)
				: DesiredSlotParent)
			: OriginalFPSMaterials[Slot];
		if (SavedMeshMaterials.IsValidIndex(Slot))
		{
			SavedMeshMaterials[Slot] = DesiredSlotMaterial;
		}
		if (DesiredSlotMID != nullptr)
		{
			DesiredSlotMID->SetScalarParameterValue(NAME_Scale, WeaponRenderScale);
		}
		// Character body overrides own every visible weapon slot while active.
		if (bBodyOverrideActive)
		{
			continue;
		}
		Mesh->SetMaterial(Slot, DesiredSlotMaterial);
		// MeshMIDs is compact; index i is slot i's MID whenever slot i has a
		// material. Avoid touching it for the null-slot edge case.
		if (DesiredSlotMID != nullptr && MeshMIDs.IsValidIndex(Slot))
		{
			MeshMIDs[Slot] = DesiredSlotMID;
		}
	}
	// A screen/counter slot may have runtime state created by SetupSpecialMaterials().
	// Rebuild that state after an all-slot invisibility skin gives ownership back to an
	// ordinary family mask. Do not call this while applying invisibility: its purpose is
	// to cover those special sections too.
	if (!bBodyOverrideActive && (PreviousSlotMask & ~TargetSlotMask) != 0u)
	{
		SetupSpecialMaterials();
	}
	ApplyFirstPersonHologramProjectionParams();
	UpdateFirstPersonHologramDepthMesh(
		bFirstPersonHologramSkinActive && !bBodyOverrideActive);
	if (Mesh->IsRegistered())
	{
		UpdateOutline();
	}
	if (!bBodyOverrideActive && SkinTiming())
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[SkinTiming] %s applied skin=%s slot-mask=0x%x"), *GetName(),
			Skin != nullptr ? *Skin->GetName() : TEXT("Default"), TargetSlotMask);
	}
}

void AUTWeaponFix::UpdateOutline()
{
	if (bFirstPersonHologramSkinActive)
	{
		// The stock outline mesh would occupy the same custom-depth pixels with a
		// different stencil and Panini/material projection. Keep that 1P-only mesh
		// dormant while the Ghost depth slave owns the silhouette. Remote players'
		// stock 3P weapon attachment/outline is a separate actor and is untouched.
		if (CustomDepthMesh != nullptr && CustomDepthMesh->IsRegistered())
		{
			CustomDepthMesh->UnregisterComponent();
		}
		return;
	}

	Super::UpdateOutline();
}

void AUTWeaponFix::ApplyFirstPersonHologramProjectionParams()
{
	if (!bFirstPersonHologramSkinActive)
	{
		return;
	}

	// M_HoloEffect deliberately has no first-person Panini material graph. The
	// viewport nevertheless reads these names from slot zero when it projects
	// muzzle flashes and other weapon children. Store identity values in every
	// actor-local MID so those children stay aligned with the raw hologram mesh.
	// The Holo shader ignores the otherwise-unused scalar overrides.
	static const FName NAME_PaniniD(TEXT("d"));
	static const FName NAME_PaniniS(TEXT("s"));
	static const FName NAME_FOVMulti(TEXT("FOV Multi"));
	static const FName NAME_Scale(TEXT("Scale"));
	for (int32 Slot = 0; Slot < AppliedFPSMaterialInstances.Num(); ++Slot)
	{
		if (((AppliedFPSMaterialSlotMask >> Slot) & 0x1u) == 0u)
		{
			continue;
		}
		UMaterialInstanceDynamic* const MID = AppliedFPSMaterialInstances[Slot];
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(NAME_PaniniD, 0.f);
			MID->SetScalarParameterValue(NAME_PaniniS, 0.f);
			MID->SetScalarParameterValue(NAME_FOVMulti, 0.f);
			MID->SetScalarParameterValue(NAME_Scale, 1.f);
		}
	}
}

void AUTWeaponFix::DestroyFirstPersonHologramDepthMesh()
{
	if (FirstPersonHologramDepthMesh != nullptr)
	{
		FirstPersonHologramDepthMesh->SetMasterPoseComponent(nullptr);
		FirstPersonHologramDepthMesh->DestroyComponent(false);
		FirstPersonHologramDepthMesh = nullptr;
	}
}

void AUTWeaponFix::UpdateFirstPersonHologramDepthMesh(bool bEnable)
{
	if (!bEnable || GetNetMode() == NM_DedicatedServer || Mesh == nullptr ||
		Mesh->SkeletalMesh == nullptr)
	{
		DestroyFirstPersonHologramDepthMesh();
		return;
	}

	if (FirstPersonHologramDepthMesh == nullptr)
	{
		// Build a fresh slave instead of DuplicateObject(Mesh). Epic's stock helper
		// is not exported to plugins, and duplicating a live component also copies its
		// transient AttachChildren pointers; destroying that copy can then walk/log
		// the real weapon's muzzle and beam children.
		FirstPersonHologramDepthMesh = NewObject<USkeletalMeshComponent>(this);
		if (FirstPersonHologramDepthMesh == nullptr)
		{
			return;
		}

		FirstPersonHologramDepthMesh->SetSkeletalMesh(Mesh->SkeletalMesh);
		FirstPersonHologramDepthMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FirstPersonHologramDepthMesh->SetSimulatePhysics(false);
		FirstPersonHologramDepthMesh->SetCastShadow(false);
		FirstPersonHologramDepthMesh->SetOnlyOwnerSee(true);
		FirstPersonHologramDepthMesh->bRenderInMainPass = false;
		FirstPersonHologramDepthMesh->bRenderCustomDepth = true;
		// Stencil zero is the stock pickup-ghost contract: M_HoloEffect samples
		// depth, while TacCom/team outlines reserve nonzero stencil values.
		FirstPersonHologramDepthMesh->CustomDepthStencilValue = 0;
		FirstPersonHologramDepthMesh->bReceivesDecals = false;
		FirstPersonHologramDepthMesh->bShouldUpdatePhysicsVolume = false;
		FirstPersonHologramDepthMesh->bUseAttachParentBound = true;
		// The stock 1P originals include Panini projection, but M_HoloEffect does
		// not. Using those originals for depth would bend only the depth carrier
		// and produce doubled/broken hologram edges. A plain opaque depth material
		// keeps both passes in the same raw mesh space until a dedicated
		// Panini-aware NCP hologram master is authored.
		for (int32 MaterialIndex = 0;
			MaterialIndex < FirstPersonHologramDepthMesh->GetNumMaterials();
			++MaterialIndex)
		{
			FirstPersonHologramDepthMesh->SetMaterial(
				MaterialIndex, UMaterial::GetDefaultMaterial(MD_Surface));
		}
		FirstPersonHologramDepthMesh->BoundsScale = 15000.f;
		FirstPersonHologramDepthMesh->SetMasterPoseComponent(Mesh);
		FirstPersonHologramDepthMesh->UpdateMasterBoneMap();
		FirstPersonHologramDepthMesh->AttachToComponent(
			Mesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		FirstPersonHologramDepthMesh->RelativeLocation = FVector::ZeroVector;
		FirstPersonHologramDepthMesh->RelativeRotation = FRotator::ZeroRotator;
		FirstPersonHologramDepthMesh->RelativeScale3D = FVector(1.f);
	}

	// The main weapon mesh owns every muzzle/beam child. This depth-only slave has
	// none, so never propagate visibility beyond the component itself.
	FirstPersonHologramDepthMesh->SetVisibility(Mesh->bVisible, false);
	FirstPersonHologramDepthMesh->SetHiddenInGame(Mesh->bHiddenInGame, false);
	if (FirstPersonHologramDepthMesh->GetAttachParent() != Mesh)
	{
		FirstPersonHologramDepthMesh->SetMasterPoseComponent(Mesh);
		FirstPersonHologramDepthMesh->AttachToComponent(
			Mesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		FirstPersonHologramDepthMesh->RelativeLocation = FVector::ZeroVector;
		FirstPersonHologramDepthMesh->RelativeRotation = FRotator::ZeroRotator;
		FirstPersonHologramDepthMesh->RelativeScale3D = FVector(1.f);
	}
	if (Mesh->IsRegistered() && !FirstPersonHologramDepthMesh->IsRegistered())
	{
		FirstPersonHologramDepthMesh->RegisterComponent();
		FirstPersonHologramDepthMesh->LastRenderTime = Mesh->LastRenderTime;
		FirstPersonHologramDepthMesh->bRecentlyRendered = Mesh->bRecentlyRendered;
	}
}

TArray<UMeshComponent*> AUTWeaponFix::Get1PMeshes_Implementation() const
{
	TArray<UMeshComponent*> Result = Super::Get1PMeshes_Implementation();
	Result.Add(FirstPersonHologramDepthMesh);
	return Result;
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
#if !UE_SERVER
	RefreshShockInputTrace();
#endif
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
	// Super::BringUp/AttachToOwner writes WeaponRenderScale after material setup.
	// Restore the identity projection required by the non-Panini Holo material.
	ApplyFirstPersonHologramProjectionParams();
	// Refresh after the hide policy so the depth-only companion mirrors both
	// BP-parity bVisible propagation and classic bHiddenInGame on this frame.
	UpdateFirstPersonHologramDepthMesh(
		bFirstPersonHologramSkinActive && UTOwner != nullptr &&
		UTOwner->GetSkin() == nullptr);

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
	AUTWeaponFix* const FixWeapon = Cast<AUTWeaponFix>(Weapon);
	USkeletalMeshComponent* const HologramDepthMesh = (FixWeapon != nullptr)
		? FixWeapon->FirstPersonHologramDepthMesh
		: nullptr;
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
		if (HologramDepthMesh != nullptr)
		{
			// SetHiddenInGame does not propagate by default. Mirror classic hide on
			// the depth-only companion or it can leave a post-process silhouette.
			HologramDepthMesh->SetHiddenInGame(true, false);
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

	if (HologramDepthMesh != nullptr && WeapMesh != nullptr)
	{
		HologramDepthMesh->SetVisibility(WeapMesh->bVisible, false);
		HologramDepthMesh->SetHiddenInGame(WeapMesh->bHiddenInGame, false);
	}
}

void AUTWeaponFix::SetSkin(UMaterialInterface* NewSkin)
{
	const bool bLogSkinTiming = SkinTiming();
	const double SetSkinStartTime = bLogSkinTiming ? FPlatformTime::Seconds() : 0.0;
	const uint32 TargetSlotMask = bCapturedOriginalFPSMaterials
		? AppliedFPSMaterialSlotMask
		: 0u;
	UMaterialInterface* SlotZeroBefore = (Mesh != nullptr && Mesh->GetNumMaterials() > 0)
		? Mesh->GetMaterial(0)
		: nullptr;

	// Patch stock's restore array first so clearing a character-body override brings
	// back the configured weapon skin, including every slot of invisibility skins.
	if (bCapturedOriginalFPSMaterials)
	{
		for (int32 Slot = 0; Slot < OriginalFPSMaterials.Num(); ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u && SavedMeshMaterials.IsValidIndex(Slot))
			{
				UMaterialInstanceDynamic* const SelectedMID =
					AppliedFPSMaterialInstances.IsValidIndex(Slot)
					? AppliedFPSMaterialInstances[Slot]
					: nullptr;
				UMaterialInterface* const SelectedParent =
					AppliedFPSMaterialParents.IsValidIndex(Slot)
					? AppliedFPSMaterialParents[Slot]
					: nullptr;
				SavedMeshMaterials[Slot] = (SelectedParent != nullptr)
					? ((SelectedMID != nullptr)
						? Cast<UMaterialInterface>(SelectedMID)
						: SelectedParent)
					: OriginalFPSMaterials[Slot];
			}
		}
	}

	Super::SetSkin(NewSkin);

	bool bReassertedWeaponSkin = false;
	if (NewSkin == nullptr && bCapturedOriginalFPSMaterials && Mesh != nullptr)
	{
		// Stock's MeshMIDs rebuild should already preserve existing MIDs, but reassert
		// the exact actor-local selected/default instance for every targeted slot.
		static const FName NAME_Scale(TEXT("Scale"));
		const int32 MaterialSlotCount = FMath::Min(OriginalFPSMaterials.Num(),
			Mesh->GetNumMaterials());
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) == 0u)
			{
				continue;
			}
			UMaterialInstanceDynamic* const SelectedMID =
				AppliedFPSMaterialInstances.IsValidIndex(Slot)
				? AppliedFPSMaterialInstances[Slot]
				: nullptr;
			UMaterialInterface* const SelectedParent =
				AppliedFPSMaterialParents.IsValidIndex(Slot)
				? AppliedFPSMaterialParents[Slot]
				: nullptr;
			UMaterialInterface* const DesiredSlotMaterial = (SelectedParent != nullptr)
				? ((SelectedMID != nullptr)
					? Cast<UMaterialInterface>(SelectedMID)
					: SelectedParent)
				: OriginalFPSMaterials[Slot];
			UMaterialInstanceDynamic* const DesiredSlotMID =
				Cast<UMaterialInstanceDynamic>(DesiredSlotMaterial);
			Mesh->SetMaterial(Slot, DesiredSlotMaterial);
			if (SavedMeshMaterials.IsValidIndex(Slot))
			{
				SavedMeshMaterials[Slot] = DesiredSlotMaterial;
			}
			if (DesiredSlotMID != nullptr)
			{
				DesiredSlotMID->SetScalarParameterValue(NAME_Scale, WeaponRenderScale);
				if (MeshMIDs.IsValidIndex(Slot))
				{
					MeshMIDs[Slot] = DesiredSlotMID;
				}
			}
			bReassertedWeaponSkin = true;
		}
	}
	ApplyFirstPersonHologramProjectionParams();
	UpdateFirstPersonHologramDepthMesh(
		bFirstPersonHologramSkinActive && NewSkin == nullptr);

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
			bReassertedWeaponSkin ? 1 : 0,
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

	// Never let duplicate retries cross into another weapon's equip lifetime.
	if (!UTOwner || UTOwner->IsPendingKillPending() || UTOwner->GetWeapon() != this
		|| UTOwner->GetPendingWeapon() != nullptr
		|| CurrentState == UnequippingState || CurrentState == InactiveState)
	{
		return;
	}

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
	if (!UTOwner || UTOwner->IsPendingKillPending() || UTOwner->GetWeapon() != this
		|| UTOwner->GetPendingWeapon() != nullptr
		|| CurrentState == UnequippingState || CurrentState == InactiveState)
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
	if (RocketPrimaryDiagFor(this, FireModeNum))
	{
		UE_LOG(LogUTWeaponFix, Warning,
			TEXT("[RocketM1Diag] CLIENT_ACK frame=%u t=%.4f mode=%d ack=%d localEvent=%d state=%s currentMode=%d tracker=%d pending0=%d pendingFakes=%d resend=%d legacyDelayActive=%d"),
			(uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
			FireModeNum, InAuthorizedEventIndex,
			ClientFireEventIndex.IsValidIndex(FireModeNum) ? ClientFireEventIndex[FireModeNum] : -1,
			GetCurrentState() ? *GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			CurrentFireMode, CurrentlyFiringMode,
			(UTOwner && UTOwner->IsPendingFire(0)) ? 1 : 0,
			PendingFakeProjectiles.Num(), ResendFireEvents.Num(),
			GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle) ? 1 : 0);
	}

    // FIX 1: Do NOT rollback the local sequence generator.
    // Only update if server is AHEAD (rare resync case).
    if (ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        if (InAuthorizedEventIndex > ClientFireEventIndex[FireModeNum])
        {
            ClientFireEventIndex[FireModeNum] = InAuthorizedEventIndex;
        }
    }

	// An ACK means the server already spawned the authoritative projectile. Cancel any
	// Flak fake that is still sleeping instead of letting it appear late beside the real.
	// Each primary shard owns an independent request even though all nine share one event.
	for (int32 i = DelayedFlakProjectiles.Num() - 1; i >= 0; --i)
	{
		FNetcodeDelayedFlakProjectile& Request = DelayedFlakProjectiles[i];
		if (Request.FireMode == FireModeNum && Request.EventIndex <= InAuthorizedEventIndex)
		{
			GetWorldTimerManager().ClearTimer(Request.TimerHandle);
			DelayedFlakProjectiles.RemoveAtSwap(i, 1, false);
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
            // Keep the projectile types whose stock fake/real handoff deliberately leaves
            // the fake responsible for visuals. This is mandatory for Flak primary because
            // AUTProj_FlakShard is bNetTemporary: after pairing, the replicated real destroys
            // itself. Destroying the fake on ACK therefore erases the shard permanently.
            // Flak shells use the normal hidden-real handoff, so destroying their visible fake
            // during the ACK/replication race causes the same cosmetic disappearance.
            if (Pending.Projectile.IsValid())
            {
				AUTProjectile* Fake = Pending.Projectile.Get();
				const bool bExistingRetentionType = Fake->IsA(AUTPlusProj_ShockBall::StaticClass())
					|| Fake->IsA(AUTPlusProj_Rocket::StaticClass());
				const bool bFlakRetentionType =
					(Pending.FireMode == 0 && Fake->IsA(AUTProj_FlakShard::StaticClass()))
					|| (Pending.FireMode == 1 && Fake->IsA(AUTProj_FlakShell::StaticClass()));
				// Secondary impact fragments are AUTProj_FlakShard-derived too, but are spawned
				// by the authoritative shell explosion and never enter this weapon-pending list.
				// The exact ACK may arrive before replication/pairing, so preserve that fake.
				// For an older event, retain Flak only if pairing already proved it had a real;
				// otherwise a skipped/rejected old event must not become a permanent ghost.
				const bool bRetainVisualFake = bExistingRetentionType
					|| (bFlakRetentionType
						&& (Pending.EventIndex == InAuthorizedEventIndex || Fake->MasterProjectile != nullptr));
				if (!bRetainVisualFake)
                {
					Fake->Destroy();
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

void AUTWeaponFix::NotifyFakeProjectileHit(AUTCharacter* HitTarget, const FVector& HitLocation, uint8 FireModeNum,
	AUTProjectile* SourceProj)
{
	// During replay playback, skip all rewind/prediction logic
	UWorld* W = GetWorld();
	if (W && W->DemoNetDriver && W->DemoNetDriver->IsPlaying())
	{
		return;
	}

	if (!bEnableProjectileRewind || !HitTarget)
	{
		return;
	}

	// Client-side hitsound prediction for projectile weapons.
	// Deliberately AFTER the bEnableProjectileRewind gate: this is the claim
	// path, so a weapon with rewind disabled must not produce a predicted
	// hitsound for a hit it never claims (UTPlusProj_StingerShard's comment
	// already documented that contract — previously the block sat above the
	// gate and broke it).
	if (Role != ROLE_Authority && IsHighConfidencePredictedHitsoundTarget(HitTarget))
	{
		AClientHitsounds* HitsoundsMut = FindClientHitsoundsMutator();
		if (HitsoundsMut)
		{
			AUTGameState* HitsoundGS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
			const bool bFriendlyTarget = HitsoundGS && HitsoundGS->OnSameTeam(UTOwner, HitTarget);

			// Prefer the reporting projectile's OWN damage. `this` is the weapon the
			// shooter is holding right now, which is not necessarily the one that fired:
			// ProjClass[FireModeNum] on a swapped-to weapon estimates a completely
			// different projectile (rocket in flight + switch to flak => flak shard's
			// damage). The instance also carries Blueprint-authored overrides, which the
			// C++ CDO of a stock class does not.
			int32 EstDamage = 0;
			if (SourceProj != nullptr)
			{
				EstDamage = SourceProj->DamageParams.BaseDamage;
			}
			else if (ProjClass.IsValidIndex(FireModeNum) && ProjClass[FireModeNum])
			{
				if (AUTProjectile* DefProj = ProjClass[FireModeNum]->GetDefaultObject<AUTProjectile>())
				{
					EstDamage = DefProj->DamageParams.BaseDamage;
				}
			}
			// Mirror the server broadcast's amp scaling (see the hitscan site).
			if (UTOwner != nullptr)
			{
				EstDamage = FMath::TruncToInt(UTOwner->DamageScaling * (float)EstDamage);
			}
			if (bFriendlyTarget || EstDamage > 0)
			{
				HitsoundsMut->PlayClientPredictedHitsound(EstDamage, bFriendlyTarget);
			}
		}
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

	// 4. Capsule dims for the rewound target. Half-height is only a BASE here —
	// posture (floor slide) is re-applied PER REWIND SAMPLE below, because the
	// shape the target had at the claimed instant, not its current shape, is
	// what the shot was aimed at. Same helper + grace window as hitscan
	// validation (see the AltCapHeight pattern in the hitscan claim search).
	const float CapRadius = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float BaseCapHeight = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// 5. ClaimedHitLocation is a SEARCH ANCHOR only, never the damage origin (using it as the
	// origin would let a modified client convert a near-miss into a center-mass direct hit).
	// Walk the target's history across the window; find the instant its capsule passed
	// closest to the claimed point.
	float BestDelta = 0.f;
	float BestDistSq = BIG_NUMBER;
	FVector BestCenter = ClaimedTarget->GetActorLocation();
	float BestSegHalf = FMath::Max(0.f, BaseCapHeight - CapRadius);
	const float StepSec = 1.f / 240.f;
	for (float Delta = 0.f; Delta <= WindowSec + KINDA_SMALL_NUMBER; Delta += StepSec)
	{
		FVector Center = ClaimedTarget->GetRewindLocation(Delta);
		float SampleCapHeight = BaseCapHeight;
		ApplySlidePostureForValidation(ClaimedTarget, Delta, Center, SampleCapHeight);
		const float SegHalf = FMath::Max(0.f, SampleCapHeight - CapRadius);
		const FVector OnSeg = FMath::ClosestPointOnSegment(ClaimedHitLocation,
			Center - FVector(0.f, 0.f, SegHalf), Center + FVector(0.f, 0.f, SegHalf));
		const float DistSq = FVector::DistSquared(ClaimedHitLocation, OnSeg);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestDelta = Delta;
			BestCenter = Center;
			BestSegHalf = SegHalf;
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

	const FVector SegTop = BestCenter + FVector(0.f, 0.f, BestSegHalf);
	const FVector SegBot = BestCenter - FVector(0.f, 0.f, BestSegHalf);
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

AUTWeaponFix* AUTWeaponFix::FindFiringWeaponForProjectile(AUTCharacter* OwnerChar, AUTProjectile* Proj)
{
	if (OwnerChar == nullptr || Proj == nullptr)
	{
		return nullptr;
	}

	const TSubclassOf<AUTProjectile> ProjectileClass = Proj->GetClass();
	for (TInventoryIterator<AUTWeapon> It(OwnerChar); It; ++It)
	{
		// The inventory chain can hand back a stale entry while it is mid-mutation
		// (see NCPlusCTFScoreboard.cpp), so null-check every step rather than the cast alone.
		AUTWeaponFix* const Candidate = Cast<AUTWeaponFix>(*It);
		if (Candidate != nullptr && Candidate->NCPFiredProjClasses.Contains(ProjectileClass))
		{
			return Candidate;
		}
	}

	// Nothing recorded for this class: the shooter never spawned one locally (no fake), or the
	// projectile came from a path other than SpawnNetPredictedProjectileInternal. Fall back to
	// the pre-existing held-weapon route so behaviour degrades to exactly what it was before.
	return Cast<AUTWeaponFix>(OwnerChar->GetWeapon());
}

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

int32 AUTWeaponFix::GetPredictedHitsoundDamage(uint8 FireModeNum, bool bHeadshotClaimed)
{
	// Base weapons have no headshot mechanic: a head claim from this weapon is
	// positional data for the server, not a damage upgrade.
	return InstantHitInfo.IsValidIndex(FireModeNum) ? InstantHitInfo[FireModeNum].Damage : 0;
}
