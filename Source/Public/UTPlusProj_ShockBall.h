#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTProj_ShockBall.h"
#include "UTPlusProj_ShockBall.generated.h"

/**
 * Custom shock ball projectile for UTPlusShockRifle.
 * Same as stock shock ball but references UTPlusShockRifle instead of UTWeap_ShockRifle.
 */
UCLASS()
class NETCODEPLUS_API AUTPlusProj_ShockBall : public AUTProj_ShockBall
{
	GENERATED_BODY()

public:
	AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer);
	virtual void Tick(float DeltaTime) override;
	virtual void BeginPlay() override;
	virtual void NotifyClientSideHit(AUTPlayerController* InstigatedBy, FVector HitLocation, AActor* DamageCauser, int32 Damage) override;
	virtual void OnRep_Slomo() override;
	virtual void PostNetReceiveVelocity(const FVector& NewVelocity) override;
	virtual void Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal, UPrimitiveComponent* HitComp = nullptr) override;
	virtual bool ShouldIgnoreHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp) override;
	virtual void DamageImpactedActor_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp, const FVector& HitLocation, const FVector& HitNormal) override;

	// Diagnostic hooks (ncp.ShockDebug) — Super-passthrough + event-gated logging only, no behaviour change.
	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp, const FVector& HitLocation, const FVector& HitNormal) override;
	virtual void PostNetReceiveLocationAndRotation() override;

private:
	// Forward declaration for safety
	class UParticleSystemComponent* FlightEffectComponent;

protected:
	/**
	 * Override to reference UTPlusShockRifle instead of UTWeap_ShockRifle.
	 * This ensures combo detection works with our custom weapon.
	 */
	virtual void PerformCombo(class AController* InstigatedBy, class AActor* DamageCauser) override;
	
	/** The visual component added in Blueprint (e.g., FlightEffect) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Visuals", meta = (AllowPrivateAccess = "true"))
	UParticleSystemComponent* FlightEffectVisual;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	float VisualInterpSpeed;

	// Store the offset so the ball doesn't snap to the center of the actor
	FVector InitialVisualOffset;
	bool bVisualInitialized;

	/** Original fire direction — used to correct floating point drift at high fps */
	FVector OriginalFireDirection;
	bool bHasCachedFireDirection;

	/** Time the projectile has been embedded in static geometry while not travelling.
	 *  If it exceeds StuckExplodeDelay, force-explode to clear a pinned/embedded core. */
	float StuckTime;
	static constexpr float StuckExplodeDelay = 0.05f;

	/** Last server location sampled for the stuck-progress test (see Tick). */
	FVector LastStuckProgressLoc;
	/** Max net movement (units) over the debounce window that still counts as "not travelling". */
	static constexpr float StuckProgressThreshold = 6.f;

	// ---- Behavioural pairing state (NOT diagnostics — consumed by CanMatchFake / handoff) ----
	/** Fake spawn origin. Consumed by CanMatchFake gate 3b (displacement-from-spawn) AND the curve
	 *  diagnostics. Written unconditionally in BeginPlay, so behaviour is identical with ncp.ShockDebug 0/1. */
	FVector FireLineOrigin;
	/** Fake-only: integral of |velocity|*dt over the fake's life (the actor tick delta is already
	 *  scaled by CustomTimeDilation). CanMatchFake gate 3b's expected-displacement term — correct under
	 *  Slomo and it freezes on a PMC stop, unlike currentSpeed*wallAge. Accumulated every Tick, ungated. */
	float ExpectedDispAccum;
	/** Real-only: set true once a replicated velocity ~0 (server-confirmed stop) has been received.
	 *  Sticky. Log-only in this build; commit 2 gates the reveal on it (replicated vs local-only stop). */
	bool bServerConfirmedStop;

	// ---- Curve diagnostics (ncp.ShockDebug) — logging state only, zero behaviour change ----
	// The open-air mid-flight bend ("swoosh") has never been captured because every existing
	// log line is tied to a stop/hit/reveal event. These track the flight itself, event-gated
	// by doubling thresholds so a straight core logs nothing. See Tick/PostNetReceiveVelocity.
	// Initialised in the CONSTRUCTOR, not BeginPlay: on a replicated real PostNetReceiveVelocity
	// fires before BeginPlay, so BeginPlay-init would wipe the FirstRepVelDir baseline it records.
	/** Next lateral-offset-from-fire-line (units) that triggers a CURVE-LAT log; doubles each log. */
	float NextCurveLatLog;
	/** Next velocity-heading deviation (degrees) that triggers a CURVE-VEL log; doubles each log. */
	float NextCurveVelDegLog;
	/** Vector sum of convergence corrections applied to the fake (lives on the REAL instance). */
	FVector ConvergePullAccum;
	/** Next accumulated-pull magnitude (units) that triggers a CONVERGE-PULL log; doubles each log. */
	float NextConvergePullLog;
	/** First non-stop replicated velocity heading (REAL instance) — the server's true fire heading;
	 *  baseline for the deferred PNRV paired-cmp (emitted in BeginPlay) + mid-flight heading changes. */
	FVector FirstRepVelDir;
	bool bLoggedFirstRepVel;

public:
	virtual bool CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const override;

	/** Re-cache the fire direction after external velocity enforcement */
	void SetOriginalFireDirection(const FVector& Dir);
};
