#include "NCPBlueprintCompatLibrary.h"
#include "Engine/NetDriver.h"
#include "Engine/Player.h"
#include "GameFramework/GameMode.h"
#include "UObject/UnrealType.h"
#include "GameFramework/PlayerController.h"
#include "UTGameMode.h"
#include "UTWeapon.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacterMovement.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

void UNCPBlueprintCompatLibrary::ClientPlaySoundAtLocation(APlayerController* Target, USoundBase* Sound, FVector Location, float VolumeMultiplier, float PitchMultiplier)
{
    if (Target) { Target->ClientPlaySoundAtLocation(Sound, Location, VolumeMultiplier, PitchMultiplier); }
}
void UNCPBlueprintCompatLibrary::ClientPlaySoundAtLocationMany(const TArray<APlayerController*>& Targets, USoundBase* Sound, FVector Location, float VolumeMultiplier, float PitchMultiplier)
{
    for (APlayerController* Target : Targets) { ClientPlaySoundAtLocation(Target, Sound, Location, VolumeMultiplier, PitchMultiplier); }
}
void UNCPBlueprintCompatLibrary::UTClientPlaySound(AUTPlayerController* Target, USoundBase* Sound)
{
    if (Target) { Target->UTClientPlaySound(Sound); }
}
void UNCPBlueprintCompatLibrary::NotifyLegacyMatchState(AGameMode* Target, FName NewState)
{
    if (!Target) { return; }
    // K2_OnSetMatchState is a protected BlueprintImplementableEvent. Dispatch the
    // one validated event through its supported UObject interface; do not alter
    // function flags or bypass SetMatchState's native state machine.
    UFunction* Event = Target->FindFunction(TEXT("K2_OnSetMatchState"));
    if (!Event || Event->NumParms != 1 || !FindFProperty<FNameProperty>(Event, TEXT("NewState"))) { return; }
    struct FParams { FName NewState; } Params{NewState};
    if (Event->ParmsSize == sizeof(FParams)) { Target->ProcessEvent(Event, &Params); }
}
void UNCPBlueprintCompatLibrary::ServerViewNextPlayer(APlayerController* Target)
{
    if (Target) { Target->ServerViewNextPlayer(); }
}
void UNCPBlueprintCompatLibrary::ServerNegotiatePredictionPing(AUTPlayerController* Target, float NewPredictionPing)
{
    if (Target) { Target->ServerNegotiatePredictionPing(NewPredictionPing); }
}
int32 UNCPBlueprintCompatLibrary::SetMaxClientRate(UNetDriver* Target, int32 Value)
{
    if (Target) { Target->MaxClientRate = FMath::Max(1, Value); return Target->MaxClientRate; } return 0;
}
int32 UNCPBlueprintCompatLibrary::SetCurrentNetSpeed(UPlayer* Target, int32 Value)
{
    if (Target) { Target->CurrentNetSpeed = FMath::Max(1, Value); return Target->CurrentNetSpeed; } return 0;
}
bool UNCPBlueprintCompatLibrary::SetRecordReplays(AUTGameMode* Target, bool Value)
{
    if (Target) { Target->bRecordReplays = Value; return Target->bRecordReplays; } return false;
}
APlayerStart* UNCPBlueprintCompatLibrary::SetRespawnChoiceA(AUTPlayerState* Target, APlayerStart* Value)
{
    if (Target) { Target->RespawnChoiceA = Value; return Target->RespawnChoiceA; } return nullptr;
}
APlayerStart* UNCPBlueprintCompatLibrary::SetRespawnChoiceB(AUTPlayerState* Target, APlayerStart* Value)
{
    if (Target) { Target->RespawnChoiceB = Value; return Target->RespawnChoiceB; } return nullptr;
}
bool UNCPBlueprintCompatLibrary::SetWantsFloorSlide(UUTCharacterMovement* Target, bool Value)
{
    if (Target) { Target->UpdateFloorSlide(Value); return Target->WantsFloorSlide(); } return false;
}
void UNCPBlueprintCompatLibrary::MakeSimpleJson(const TArray<FString>& Keys, const TArray<FString>& Values, FString& Json)
{
    Json.Reset();
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    Writer->WriteObjectStart();
    for (int32 Index = 0; Index < Keys.Num(); ++Index)
    {
        Writer->WriteValue(Keys[Index], Values.IsValidIndex(Index) ? Values[Index] : FString());
    }
    Writer->WriteObjectEnd(); Writer->Close();
}
FLinearColor UNCPBlueprintCompatLibrary::GetWeaponIconColor(TSubclassOf<AUTWeapon> WeaponClass)
{
    const AUTWeapon* Weapon = WeaponClass ? WeaponClass->GetDefaultObject<AUTWeapon>() : nullptr;
    return Weapon ? Weapon->IconColor : FLinearColor::White;
}
