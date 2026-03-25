// ClientHitsounds.cpp
#include "ClientHitsounds.h"
#include "HitsoundPack.h"
#include "UTHitsoundMessage.h"
#include "UnrealTournament.h"
#include "UTGameMode.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTGameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "AssetRegistryModule.h"
#include "IAssetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogClientHitsounds, Log, All);

const FString AClientHitsounds::ConfigSection = TEXT("ClientHitsounds");

AClientHitsounds::AClientHitsounds(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FlakHitMinAge = 0.035f;
	MedDamageThreshold = 30;
	HighDamageThreshold = 60;
	HitsoundsDefaults.bPlayZeroFriendly = false;
	HitsoundsDefaults.UserMultiplier = 1.0f;
	Config = HitsoundsDefaults;

	// Client-side prediction defaults
	LastClientHitsoundTime = 0.0f;
	bClientSideHitsoundsEnabled = true;
	ClientHitsoundDedupWindow = 0.25f;
	ClientHitsoundMinInterval = 0.05f;

	// Critical for networking
	bReplicates = true;
	bAlwaysRelevant = true;

	// Default message class
	HitsoundMessageClass = UUTHitsoundMessage::StaticClass();
}

void AClientHitsounds::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() != NM_DedicatedServer)
	{
		ReadConfig();
	}
	BuildHitsounds();
	UE_LOG(LogClientHitsounds, Log, TEXT("ClientHitsounds initialized"));
}

void AClientHitsounds::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (FlakTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(FlakTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void AClientHitsounds::Init_Implementation(const FString& Options)
{
	Super::Init_Implementation(Options);
	UE_LOG(LogClientHitsounds, Log, TEXT("ClientHitsounds::Init with options: %s"), *Options);
}

void AClientHitsounds::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	Super::Mutate_Implementation(MutateString, Sender);
	if (HasAuthority())
	{
		EventMutateClientSide(MutateString, Sender);
	}
}

void AClientHitsounds::EventMutateClientSide_Implementation(const FString& MutateString, APlayerController* Sender)
{
	APlayerController* LocalPC = UGameplayStatics::GetPlayerController(this, 0);
	if (LocalPC && LocalPC == Sender)
	{
		ShowMenu(MutateString, Sender);
	}
}

bool AClientHitsounds::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
	AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser,
	TSubclassOf<UDamageType> DamageType)
{
	bool bResult = Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
	if (!HasAuthority()) return bResult;

	int32 ScaledDamage = GetScaledDamage(InstigatedBy, Damage);
	HandleDamage(InstigatedBy, Injured, ScaledDamage, DamageType);
	return bResult;
}

int32 AClientHitsounds::GetScaledDamage(AController* InstigatorController, int32 BaseDamage)
{
	return BaseDamage;
}

void AClientHitsounds::HandleDamage(AController* CausedBy, APawn* Victim, int32 Damage, TSubclassOf<UDamageType> Type)
{
	if (!CausedBy || !Victim) return;

	if (IsFlakDamage(Type))
	{
		AppendFlakQueue(Damage, CausedBy, Victim);
		return;
	}
	NotifyDamage(Damage, CausedBy, Victim);
}

void AClientHitsounds::NotifyDamage(int32 Damage, AController* CausedBy, APawn* Victim)
{
	if (!CausedBy || !HasAuthority()) return;

	AUTPlayerController* AttackerPC = Cast<AUTPlayerController>(CausedBy);
	if (!AttackerPC) return;

	if (!HitsoundMessageClass) return;

	bool bIsFriendly = IsFriendlyFire(CausedBy, Victim);
	int32 PackedMessage = (Damage << 1) | (bIsFriendly ? 1 : 0);

	AttackerPC->ClientReceiveLocalizedMessage(
		HitsoundMessageClass,
		PackedMessage,
		AttackerPC->PlayerState,
		nullptr,
		this
	);
}

void AClientHitsounds::AppendFlakQueue(int32 Damage, AController* CausedBy, APawn* Victim)
{
	float CurrentTime = GetWorld()->GetTimeSeconds();
	FlakHitQueue.Add(FFlakHitEvent(Damage, CurrentTime, CausedBy, Victim));

	if (!FlakTimer.IsValid())
	{
		GetWorld()->GetTimerManager().SetTimer(
			FlakTimer,
			this,
			&AClientHitsounds::ProcessFlakQueue,
			0.01f,
			true
		);
	}
}

void AClientHitsounds::ProcessFlakQueue()
{
	if (FlakHitQueue.Num() == 0)
	{
		GetWorld()->GetTimerManager().ClearTimer(FlakTimer);
		return;
	}

	float CurrentTime = GetWorld()->GetTimeSeconds();
	for (int32 i = FlakHitQueue.Num() - 1; i >= 0; i--)
	{
		const FFlakHitEvent& HitEvent = FlakHitQueue[i];
		if ((CurrentTime - HitEvent.WorldTime) >= FlakHitMinAge)
		{
			NotifyDamage(HitEvent.Damage, HitEvent.CausedBy, HitEvent.Victim);
			FlakHitQueue.RemoveAt(i);
		}
	}
}

void AClientHitsounds::PlayHitsound(int32 Damage, bool bIsFriendly, TSubclassOf<UDamageType> Type, AActor* HitsoundPlayer)
{
	// Removed Interface Logic - Direct Playback Only
	const FHitsound& HitsoundPreset = bIsFriendly ? Config.Friendly : Config.Enemy;

	if (bIsFriendly && Damage == 0 && !Config.bPlayZeroFriendly)
	{
		return;
	}

	USoundBase* SoundToPlay = SelectSoundForDamage(HitsoundPreset, Damage);
	if (SoundToPlay)
	{
		float FinalVolume = HitsoundPreset.Volume * Config.UserMultiplier;
		UGameplayStatics::PlaySound2D(this, SoundToPlay, FinalVolume, HitsoundPreset.Pitch);
	}
}

USoundBase* AClientHitsounds::SelectSoundForDamage(const FHitsound& Hitsound, int32 Damage) const
{
	if (Damage >= HighDamageThreshold && Hitsound.High) return Hitsound.High;
	else if (Damage >= MedDamageThreshold && Hitsound.Med) return Hitsound.Med;
	else if (Hitsound.Low) return Hitsound.Low;
	return Hitsound.Med;
}

void AClientHitsounds::PlaySampleHitsound()
{
	USoundBase* SoundToPlay = SelectSoundForDamage(Config.Enemy, MedDamageThreshold);
	if (SoundToPlay)
	{
		float FinalVolume = Config.Enemy.Volume * Config.UserMultiplier;
		UGameplayStatics::PlaySound2D(this, SoundToPlay, FinalVolume, Config.Enemy.Pitch);
	}
}

// ============================================================
// Client-Side Hitsound Prediction (Task 4)
// ============================================================

void AClientHitsounds::PlayClientPredictedHitsound(int32 EstimatedDamage)
{
	if (!bClientSideHitsoundsEnabled)
	{
		return;
	}

	// Only on clients, not dedicated server
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	float CurrentTime = World->GetTimeSeconds();

	// Check minimum interval (flak shard batching)
	if ((CurrentTime - LastClientHitsoundTime) < ClientHitsoundMinInterval)
	{
		return;
	}

	// Use Enemy preset for client-predicted hitsounds
	USoundBase* SoundToPlay = SelectSoundForDamage(Config.Enemy, EstimatedDamage);
	if (SoundToPlay)
	{
		float FinalVolume = Config.Enemy.Volume * Config.UserMultiplier;
		UGameplayStatics::PlaySound2D(this, SoundToPlay, FinalVolume, Config.Enemy.Pitch);
		LastClientHitsoundTime = CurrentTime;
	}
}

bool AClientHitsounds::ShouldSuppressServerHitsound() const
{
	UWorld* World = GetWorld();
	if (!World || !bClientSideHitsoundsEnabled)
	{
		return false;
	}

	float CurrentTime = World->GetTimeSeconds();
	return (CurrentTime - LastClientHitsoundTime) < ClientHitsoundDedupWindow;
}

// ============================================================
// Configuration
// ============================================================

void AClientHitsounds::ReadConfig()
{
	BuildHitsounds();
	Config.Enemy = ReadConfigSection(TEXT("Hitsounds.Enemy"), HitsoundsDefaults.Enemy);
	Config.Friendly = ReadConfigSection(TEXT("Hitsounds.Friendly"), HitsoundsDefaults.Friendly);

	float UserMult = 1.0f;
	UUTGameplayStatics::GetModConfigFloat(ConfigSection, TEXT("UserMultiplier"), UserMult);
	Config.UserMultiplier = UserMult;

	int32 PlayZeroFriendlyInt = 0;
	UUTGameplayStatics::GetModConfigInt(ConfigSection, TEXT("PlayZeroFriendly"), PlayZeroFriendlyInt);
	Config.bPlayZeroFriendly = (PlayZeroFriendlyInt != 0);

	// Read client-side hitsound prediction setting
	int32 ClientSideHitsoundsInt = 1;
	UUTGameplayStatics::GetModConfigInt(ConfigSection, TEXT("ClientSideHitsounds"), ClientSideHitsoundsInt);
	bClientSideHitsoundsEnabled = (ClientSideHitsoundsInt != 0);
}

FHitsound AClientHitsounds::ReadConfigSection(const FString& Section, const FHitsound& Default)
{
	FHitsound Result = Default;
	FString SoundID;
	if (UUTGameplayStatics::GetModConfigString(Section, TEXT("SoundID"), SoundID))
	{
		Result = GetHitsoundByName(SoundID);
	}
	float Pitch = Default.Pitch;
	float Volume = Default.Volume;
	UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Pitch"), Pitch);
	UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Volume"), Volume);
	Result.Pitch = Pitch;
	Result.Volume = Volume;
	return Result;
}

void AClientHitsounds::WriteConfig()
{
	WriteConfigSection(TEXT("Hitsounds.Enemy"), Config.Enemy);
	WriteConfigSection(TEXT("Hitsounds.Friendly"), Config.Friendly);
	UUTGameplayStatics::SetModConfigFloat(ConfigSection, TEXT("UserMultiplier"), Config.UserMultiplier);
	UUTGameplayStatics::SetModConfigInt(ConfigSection, TEXT("PlayZeroFriendly"), Config.bPlayZeroFriendly ? 1 : 0);

	// Write client-side hitsound prediction setting
	UUTGameplayStatics::SetModConfigInt(ConfigSection, TEXT("ClientSideHitsounds"), bClientSideHitsoundsEnabled ? 1 : 0);

	UUTGameplayStatics::SaveModConfig();
}

void AClientHitsounds::WriteConfigSection(const FString& Section, const FHitsound& Hitsound)
{
	UUTGameplayStatics::SetModConfigString(Section, TEXT("SoundID"), Hitsound.DisplayName);
	UUTGameplayStatics::SetModConfigFloat(Section, TEXT("Pitch"), Hitsound.Pitch);
	UUTGameplayStatics::SetModConfigFloat(Section, TEXT("Volume"), Hitsound.Volume);
}

void AClientHitsounds::BuildHitsounds_Implementation()
{
	AllHitsounds.Empty();

	// Discover all UHitsoundPack data assets via the Asset Registry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	TArray<FAssetData> AssetList;
	AR.GetAssetsByClass(UHitsoundPack::StaticClass()->GetFName(), AssetList, true);

	for (const FAssetData& Asset : AssetList)
	{
		UHitsoundPack* Pack = Cast<UHitsoundPack>(Asset.GetAsset());
		if (!Pack) continue;

		FHitsound HS;
		HS.DisplayName = Pack->DisplayName;
		HS.Low = Pack->Low;
		HS.Med = Pack->Med;
		HS.High = Pack->High;
		HS.Volume = Pack->DefaultVolume;
		HS.Pitch = Pack->DefaultPitch;
		AllHitsounds.Add(HS);
	}

	// Sort alphabetically by display name
	AllHitsounds.Sort([](const FHitsound& A, const FHitsound& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	// Set defaults from first entries
	if (AllHitsounds.Num() > 0)
	{
		HitsoundsDefaults.Enemy = AllHitsounds[0];
		HitsoundsDefaults.Friendly = AllHitsounds.Num() > 1 ? AllHitsounds[1] : AllHitsounds[0];
	}
}

FHitsound AClientHitsounds::GetHitsoundByName(const FString& Name)
{
	for (const FHitsound& Preset : AllHitsounds)
	{
		if (Preset.DisplayName.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Preset;
		}
	}
	if (AllHitsounds.Num() > 0) return AllHitsounds[0];
	return FHitsound();
}

FHitsound AClientHitsounds::MakeHitsound(float Volume, float Pitch, USoundBase* Low, USoundBase* Med, USoundBase* High, const FString& InDisplayName)
{
	FHitsound Result;
	Result.Volume = Volume;
	Result.Pitch = Pitch;
	Result.Low = Low;
	Result.Med = Med;
	Result.High = High;
	Result.DisplayName = InDisplayName;
	return Result;
}

void AClientHitsounds::ShowMenu(const FString& Command, APlayerController* InPlayerOwner)
{
	// Stub implementation for now to satisfy linker
	if (Command.Contains(TEXT("test")))
	{
		PlaySampleHitsound();
	}
}

bool AClientHitsounds::IsFlakDamage(TSubclassOf<UDamageType> DamageType) const
{
	if (!DamageType) return false;
	FString DamageTypeName = DamageType->GetName();
	return DamageTypeName.Contains(TEXT("Flak")) || DamageTypeName.Contains(TEXT("Shard"));
}

bool AClientHitsounds::ShouldPlayHitsound(TSubclassOf<UDamageType> DamageType) const
{
	return true;
}

bool AClientHitsounds::IsFriendlyFire(AController* Attacker, APawn* Victim) const
{
	if (!Attacker || !Victim) return false;
	AUTPlayerState* AttackerPS = Cast<AUTPlayerState>(Attacker->PlayerState);
	AController* VictimController = Victim->GetController();
	AUTPlayerState* VictimPS = VictimController ? Cast<AUTPlayerState>(VictimController->PlayerState) : nullptr;
	if (!AttackerPS || !VictimPS) return false;
	return AttackerPS->GetTeamNum() == VictimPS->GetTeamNum() && AttackerPS->GetTeamNum() != 255;
}
