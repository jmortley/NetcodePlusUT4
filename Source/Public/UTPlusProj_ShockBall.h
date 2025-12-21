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
};
