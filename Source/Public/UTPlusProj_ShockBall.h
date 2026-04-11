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

	/** Time the projectile has been near-zero velocity on the server.
	 *  If it exceeds StuckExplodeDelay, force-explode to prevent stuck balls. */
	float StuckTime;
	static constexpr float StuckExplodeDelay = 0.05f;

public:
	virtual bool CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const override;

	/** Re-cache the fire direction after external velocity enforcement */
	void SetOriginalFireDirection(const FVector& Dir);
};
