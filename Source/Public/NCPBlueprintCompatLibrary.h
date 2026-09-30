#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NCPBlueprintCompatLibrary.generated.h"

class APlayerController;
class AGameMode;
class AUTWeapon;
class USoundBase;
class UNetDriver;
class UPlayer;
class AUTGameMode;
class AUTPlayerController;
class AUTPlayerState;
class APlayerStart;
class UUTCharacterMovement;

/** Explicit runtime APIs for legacy NetcodePlus graphs authored with UTEditorPlus.
 *  These delegate to the real game/engine operations; no reflection flags are patched.
 */
UCLASS()
class NETCODEPLUS_API UNCPBlueprintCompatLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static void ClientPlaySoundAtLocation(APlayerController* Target, USoundBase* Sound, FVector Location, float VolumeMultiplier=1.f, float PitchMultiplier=1.f);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility")
    static void ClientPlaySoundAtLocationMany(const TArray<APlayerController*>& Targets, USoundBase* Sound, FVector Location, float VolumeMultiplier=1.f, float PitchMultiplier=1.f);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static void UTClientPlaySound(AUTPlayerController* Target, USoundBase* Sound);

    /** Preserves legacy notification-only calls; this does not change the native match state. */
    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static void NotifyLegacyMatchState(AGameMode* Target, FName NewState);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static void ServerViewNextPlayer(APlayerController* Target);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static void ServerNegotiatePredictionPing(AUTPlayerController* Target, float NewPredictionPing);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static int32 SetMaxClientRate(UNetDriver* Target, int32 Value);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static int32 SetCurrentNetSpeed(UPlayer* Target, int32 Value);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static bool SetRecordReplays(AUTGameMode* Target, bool Value);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static APlayerStart* SetRespawnChoiceA(AUTPlayerState* Target, APlayerStart* Value);

    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static APlayerStart* SetRespawnChoiceB(AUTPlayerState* Target, APlayerStart* Value);

    /** Use the existing movement API so the slide tap timestamp is updated too. */
    UFUNCTION(BlueprintCallable, Category="NetcodePlus|Compatibility", meta=(DefaultToSelf="Target"))
    static bool SetWantsFloorSlide(UUTCharacterMovement* Target, bool Value);

    UFUNCTION(BlueprintPure, Category="NetcodePlus|Compatibility")
    static FLinearColor GetWeaponIconColor(TSubclassOf<AUTWeapon> WeaponClass);

    /** Legacy BlueprintHttp MakeSimpleJson semantics: string values; missing values are empty. No HTTP request. */
    UFUNCTION(BlueprintPure, Category="NetcodePlus|Compatibility")
    static void MakeSimpleJson(const TArray<FString>& Keys, const TArray<FString>& Values, FString& Json);
};