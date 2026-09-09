#include "NCPickupBaseMutator.h"
#include "UTGameMode.h"
#include "UTPickupWeapon.h"
#include "UTWorldSettings.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/World.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	const TCHAR* const SourceClasses[] =
	{
		TEXT("/Game/RestrictedAssets/Weapons/WeaponBase.WeaponBase_C"),
		TEXT("/Game/RestrictedAssets/Pickups/Powerups/PowerupBase.PowerupBase_C"),
		TEXT("/Game/Blueprints/Netcode/NCPowerupBase_test.NCPowerupBase_test_C")
	};
	const TCHAR* const CopyClasses[] =
	{
		TEXT("/Game/Blueprints/Netcode/Performance/NCWeaponBase.NCWeaponBase_C"),
		TEXT("/Game/Blueprints/Netcode/Performance/NCPowerupBase.NCPowerupBase_C"),
		TEXT("/Game/Blueprints/Netcode/Performance/NCPowerupBaseTimer.NCPowerupBaseTimer_C")
	};

	int32 FindSourceClass(const UClass* Class)
	{
		const FString Path = Class->GetPathName();
		for (int32 Index = 0; Index < ARRAY_COUNT(SourceClasses); ++Index)
		{
			if (Path == SourceClasses[Index])
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool HasBoundActorDelegates(AActor* Actor, FString& Reason)
	{
		// In particular, preserve WeaponBase's map-authored PickedUpWeapon bindings.
		for (TFieldIterator<UMulticastDelegateProperty> It(Actor->GetClass()); It; ++It)
		{
			const FMulticastScriptDelegate* Delegate = It->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Actor);
			if (Delegate->IsBound())
			{
				if (Actor->bNetStartup && It->GetOwnerClass() == AActor::StaticClass()
					&& It->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, OnDestroyed))
				{
					const AUTWorldSettings* WorldSettings = Actor->GetWorld() != nullptr
						? Cast<AUTWorldSettings>(Actor->GetWorld()->GetWorldSettings()) : nullptr;
					if (WorldSettings != nullptr)
					{
						// UT installs this listener before each level actor's BeginPlay.
						// Exclude only that binding from the preservation check, using a
						// copy: the real callback must record the original's destruction
						// for replays. Any additional map listener still keeps the actor.
						FMulticastScriptDelegate MapBindings = *Delegate;
						MapBindings.Remove(WorldSettings, FName(TEXT("LevelActorDestroyed")));
						if (!MapBindings.IsBound())
						{
							continue;
						}
					}
				}
				Reason = FString::Printf(TEXT("bound actor delegate %s"), *It->GetName());
				return true;
			}
		}
		return false;
	}

	bool HasMatchingBodySettings(const FBodyInstance& Body, const FBodyInstance& Defaults)
	{
		// Old map packages can retain the former angular-velocity default. When
		// neither body overrides it, GetMaxAngularVelocity() uses PhysicsSettings.
		// Compare the other reflected fields without copying a live physics body.
		for (TFieldIterator<UProperty> It(FBodyInstance::StaticStruct()); It; ++It)
		{
			if (!Body.bOverrideMaxAngularVelocity && !Defaults.bOverrideMaxAngularVelocity
				&& It->GetFName() == GET_MEMBER_NAME_CHECKED(FBodyInstance, MaxAngularVelocity))
			{
				continue;
			}
			for (int32 Index = 0; Index < It->ArrayDim; ++Index)
			{
				if (!It->Identical_InContainer(&Body, &Defaults, Index))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool HasCustomPresentation(AUTPickupInventory* Pickup, FString& Reason)
	{
		// Nonreplicated map overrides would be lost on clients of a newly spawned actor.
		// Keep those authored actors. InventoryType/RespawnTime replicate; WeaponType
		// is synchronized by UTPickupWeapon::InventoryTypeUpdated on clients.
		const AUTPickupInventory* Defaults = Pickup->GetClass()->GetDefaultObject<AUTPickupInventory>();
		const bool bLogDetails = UE_LOG_ACTIVE(LogGameMode, VeryVerbose);
		bool bHasOverrides = false;
		if (Pickup->FloatHeight != Defaults->FloatHeight
			|| Pickup->RotationOffset != Defaults->RotationOffset
			|| Pickup->bAllowRotatingPickup != Defaults->bAllowRotatingPickup
			|| Pickup->TakenParticles != Defaults->TakenParticles
			|| !Pickup->TakenEffectTransform.Equals(Defaults->TakenEffectTransform)
			|| Pickup->RespawnParticles != Defaults->RespawnParticles
			|| !Pickup->RespawnEffectTransform.Equals(Defaults->RespawnEffectTransform)
			|| Pickup->BeaconDist != Defaults->BeaconDist
			|| Pickup->bBeaconThroughWalls != Defaults->bBeaconThroughWalls
			|| Pickup->Camera != Defaults->Camera
			|| Pickup->bOverride_TeamSide != Defaults->bOverride_TeamSide
			|| Pickup->TeamSide != Defaults->TeamSide
			|| Pickup->Tags != Defaults->Tags
			|| Pickup->GetIsReplicated() != Defaults->GetIsReplicated()
			|| Pickup->bHidden != Defaults->bHidden
			|| Pickup->GetActorEnableCollision() != Defaults->GetActorEnableCollision())
		{
			Reason = TEXT("pickup presentation settings differ from class defaults");
			if (!bLogDetails)
			{
				return true;
			}
			bHasOverrides = true;
			UE_LOG(LogGameMode, VeryVerbose, TEXT("[PickupBase] %s: %s"), *Pickup->GetPathName(), *Reason);
		}

		TInlineComponentArray<UActorComponent*> Components(Pickup);
		for (UActorComponent* Component : Components)
		{
			const UObject* Archetype = Component->GetArchetype();
			// Runtime inventory meshes/effects have component CDO archetypes. The
			// native/SCS base templates belong to the pickup CDO or generated class.
			if (Archetype == nullptr || (Archetype->GetOuter() != Defaults
				&& Archetype->GetOuter() != Pickup->GetClass()))
			{
				continue;
			}
			for (TFieldIterator<UProperty> It(Component->GetClass()); It; ++It)
			{
				UProperty* Property = *It;
				if (!Property->HasAnyPropertyFlags(CPF_Edit)
					|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst
						| CPF_InstancedReference | CPF_ContainsInstancedReference))
				{
					continue;
				}
				const FName Name = Property->GetFName();
				if (Component == Pickup->TimerEffect && Pickup->IsA(AUTPickupWeapon::StaticClass())
					&& Name == FName(TEXT("Template")) && Pickup->TimerEffect->Template != nullptr
					&& Pickup->TimerEffect->Template->GetPathName() == TEXT("/Game/RestrictedAssets/Weapons/Weapon_Base_Effects/Particles/P_Weapon_timer_01b.P_Weapon_timer_01b"))
				{
					continue; // stock WeaponBase construction sets this; its CDO template is null
				}
				if (Component == Pickup->TimerEffect && Pickup->IsA(AUTPickupWeapon::StaticClass())
					&& Name == GET_MEMBER_NAME_CHECKED(USceneComponent, bVisible))
				{
					// Stock editor preview hides ordinary weapon timers. BeginPlay
					// sets visibility true after relevance; HiddenInGame controls play.
					continue;
				}
				if (Component == Pickup->GetRootComponent()
					&& (Name == FName(TEXT("RelativeLocation")) || Name == FName(TEXT("RelativeRotation"))
						|| Name == FName(TEXT("RelativeScale3D"))))
				{
					continue; // preserved by the spawn transform
				}
				if (!Property->Identical_InContainer(Component, Archetype))
				{
					if (Property->GetOwnerClass() == UPrimitiveComponent::StaticClass()
						&& Name == GET_MEMBER_NAME_CHECKED(UPrimitiveComponent, BodyInstance))
					{
						const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
						const UPrimitiveComponent* DefaultPrimitive = Cast<UPrimitiveComponent>(Archetype);
						if (Primitive != nullptr && DefaultPrimitive != nullptr
							&& HasMatchingBodySettings(Primitive->BodyInstance, DefaultPrimitive->BodyInstance))
						{
							continue;
						}
					}
					const FString Difference = FString::Printf(TEXT("component override %s.%s"), *Component->GetName(), *Property->GetName());
					if (!bHasOverrides)
					{
						Reason = Difference;
						bHasOverrides = true;
					}
					if (bLogDetails)
					{
						FString InstanceValue, DefaultValue;
						Property->ExportText_InContainer(0, InstanceValue, Component, nullptr, Component, PPF_None);
						Property->ExportText_InContainer(0, DefaultValue, Archetype, nullptr, Component, PPF_None);
						UE_LOG(LogGameMode, VeryVerbose, TEXT("[PickupBase] %s %s: instance=%s; archetype=%s"),
							*Pickup->GetPathName(), *Difference, *InstanceValue, *DefaultValue);
					}
					else
					{
						return true;
					}
				}
			}
		}
		return bHasOverrides;
	}

	bool HasLevelScriptReference(AActor* Actor)
	{
		ALevelScriptActor* Script = Actor->GetLevel()->GetLevelScriptActor();
		if (Script == nullptr)
		{
			return false;
		}
		TArray<UObject*> References;
		FReferenceFinder Finder(References, nullptr, false, true, false, true);
		Finder.FindReferences(Script);
		return References.Contains(Actor);
	}

	bool ShouldPreservePickup(AUTPickupInventory* Source, AUTGameMode* Game, FString& Reason)
	{
		bool bPreventModify = false;
		if (Game && Game->BaseMutator && Game->BaseMutator->AlwaysKeep(Source, bPreventModify))
		{
			Reason = TEXT("AlwaysKeep rule");
			return true;
		}
		if (Source->GetOwner() != nullptr)
		{
			Reason = TEXT("actor has an owner");
			return true;
		}
		if (Source->GetAttachParentActor() != nullptr)
		{
			Reason = TEXT("actor is attached");
			return true;
		}
		if (HasBoundActorDelegates(Source, Reason))
		{
			return true;
		}
		if (HasLevelScriptReference(Source))
		{
			Reason = TEXT("level script references actor");
			return true;
		}
		return HasCustomPresentation(Source, Reason);
	}

	void CopyPickupSettings(AUTPickupInventory* Source, AUTPickupInventory* Target)
	{
		// Only native pickup configuration, including protected InventoryType. Do
		// not copy actor identity, tick settings, component pointers, timers, state,
		// customers or Blueprint instance storage into an unrelated generated class.
		UClass* NativeClass = Source->IsA(AUTPickupWeapon::StaticClass())
			? AUTPickupWeapon::StaticClass() : AUTPickupInventory::StaticClass();
		for (TFieldIterator<UProperty> It(NativeClass); It; ++It)
		{
			UProperty* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if (Owner != nullptr && Owner->IsChildOf(AUTPickup::StaticClass())
				&& Property->HasAnyPropertyFlags(CPF_Edit)
				&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient
					| CPF_InstancedReference | CPF_ContainsInstancedReference))
			{
				Property->CopyCompleteValue_InContainer(Target, Source);
			}
		}
	}
}

ANCPickupBaseMutator::ANCPickupBaseMutator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool ANCPickupBaseMutator::CheckRelevance_Implementation(AActor* Other)
{
	// Downstream mutators see the authored class and can substitute its inventory
	// or reject it before a base is copied. A rejected pickup stays rejected.
	const bool bRelevant = Super::CheckRelevance_Implementation(Other);
	AUTPickupInventory* Source = Cast<AUTPickupInventory>(Other);
	if (!bRelevant || Source == nullptr || Source->IsPendingKillPending()
		|| !HasAuthority() || GetWorld() == nullptr || !GetWorld()->IsGameWorld()
		|| Source->GetWorld() != GetWorld()
		|| !Source->IsActorBeginningPlay())
	{
		return bRelevant;
	}

	const int32 Index = FindSourceClass(Source->GetClass());
	if (Index == INDEX_NONE)
	{
		return true; // exact originals only; copies and specialized children cannot recurse
	}
	UClass* ReplacementClasses[] = { *NCWeaponBaseClass, *NCPowerupBaseClass, *NCPowerupTimerBaseClass };
	UClass* ReplacementClass = ReplacementClasses[Index];
	if (ReplacementClass == nullptr || ReplacementClass->GetPathName() != CopyClasses[Index]
		|| ReplacementClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| (Index == 0) != ReplacementClass->IsChildOf(AUTPickupWeapon::StaticClass()))
	{
		UE_LOG(LogGameMode, Verbose, TEXT("[PickupBase] keeping %s: copy class is not configured"), *Source->GetPathName());
		return true; // incomplete configuration/content rollout keeps the original
	}

	AUTGameMode* Game = GetWorld()->GetAuthGameMode<AUTGameMode>();
	FString KeepReason;
	if (ShouldPreservePickup(Source, Game, KeepReason))
	{
		UE_LOG(LogGameMode, Verbose, TEXT("[PickupBase] keeping %s: %s"), *Source->GetPathName(), *KeepReason);
		return true; // retain explicit keep rules, bound callbacks and map overrides
	}

	const FTransform Transform = Source->GetActorTransform();
	FActorSpawnParameters Params;
	Params.OverrideLevel = Source->GetLevel();
	Params.Instigator = Source->Instigator;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.bDeferConstruction = true;
	AUTPickupInventory* Replacement = GetWorld()->SpawnActor<AUTPickupInventory>(ReplacementClass, Transform, Params);
	if (Replacement == nullptr || Replacement->IsPendingKillPending())
	{
		return true;
	}

	CopyPickupSettings(Source, Replacement);
	Replacement->FinishSpawning(Transform);
	// The replacement goes through construction and the engine's normal BeginPlay /
	// relevance scheduling. Even if another mutator rejects it, do not resurrect
	// its original. GameMode destroys the original when we return false.
	UE_LOG(LogGameMode, Verbose, TEXT("[PickupBase] %s -> %s (%s)"),
		*Source->GetPathName(), *Replacement->GetPathName(),
		Replacement->IsPendingKillPending() ? TEXT("removed by relevance") : TEXT("copied base"));
	return false;
}
