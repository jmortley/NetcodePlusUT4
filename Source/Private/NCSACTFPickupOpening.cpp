#include "NCSACTFPickupOpening.h"
#include "NetcodePlus.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "UTArmor.h"
#include "UTGameMode.h"
#include "UTPickup.h"
#include "UTPickupInventory.h"
#include "UObject/UnrealType.h"

namespace
{
	const float OpeningAmpDelay = 30.f;

	bool HasClassPath(const UClass* Class, const TCHAR* Path)
	{
		for (; Class != nullptr; Class = Class->GetSuperClass())
		{
			if (Class->GetPathName() == Path) { return true; }
		}
		return false;
	}

	UObject* ReadObject(UObject* Object, const TCHAR* Name)
	{
		UObjectPropertyBase* Property = Object ? FindField<UObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
		return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
	}

	bool IsSACTF(AUTGameMode* GameMode)
	{
		if (!GameMode || !GameMode->HasAuthority()) { return false; }
		if (HasClassPath(GameMode->GetClass(), TEXT("/Game/Blueprints/Netcode/SACTF+.SACTF+_C"))) { return true; }
		for (UObject* Mutator = ReadObject(GameMode, TEXT("BaseMutator")); Mutator;
			Mutator = ReadObject(Mutator, TEXT("NextMutator")))
		{
			if (HasClassPath(Mutator->GetClass(), TEXT("/Game/Blueprints/Netcode/MutSaCTF.MutSaCTF_C"))) { return true; }
		}
		return false;
	}

	struct FOpeningAmp
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AUTPickup> Pickup;
		float RespawnTime;
		bool bDelayedSpawn;
		FTimerHandle OpeningHandle;
	};

	struct FOpeningState
	{
		TArray<FOpeningAmp> Amps;
		FDelegateHandle CleanupHandle;

		FOpeningState()
		{
			CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FOpeningState::WorldCleanup);
		}
		~FOpeningState()
		{
			FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
		}
		void WorldCleanup(UWorld* World, bool, bool)
		{
			Amps.RemoveAll([World](const FOpeningAmp& Amp) { return !Amp.World.IsValid() || Amp.World.Get() == World; });
		}
	};

	FOpeningState& OpeningState()
	{
		static FOpeningState State;
		return State;
	}

	void RestoreAmp(const FOpeningAmp& Amp)
	{
		AUTPickup* Pickup = Amp.Pickup.Get();
		if (!Amp.World.IsValid() || !Pickup || Pickup->IsPendingKillPending()) { return; }
		// Reflected properties avoid depending on the retail AUTPickup member layout.
		if (UFloatProperty* Respawn = FindField<UFloatProperty>(Pickup->GetClass(), TEXT("RespawnTime")))
		{
			Respawn->SetPropertyValue_InContainer(Pickup, Amp.RespawnTime);
		}
		if (UBoolProperty* Delayed = FindField<UBoolProperty>(Pickup->GetClass(), TEXT("bDelayedSpawn")))
		{
			Delayed->SetPropertyValue_InContainer(Pickup, Amp.bDelayedSpawn);
		}
		Pickup->ForceNetUpdate();
	}

	void WakeOpeningAmp(TWeakObjectPtr<AUTPickup> WeakPickup)
	{
		TArray<FOpeningAmp>& Amps = OpeningState().Amps;
		for (int32 Index = Amps.Num() - 1; Index >= 0; --Index)
		{
			if (Amps[Index].Pickup == WeakPickup)
			{
				const FOpeningAmp Amp = Amps[Index];
				Amps.RemoveAtSwap(Index);
				RestoreAmp(Amp);
				if (AUTPickup* Pickup = Amp.World.IsValid() ? WeakPickup.Get() : nullptr)
				{
					// WakeUp enables collision last and can immediately give the amp to a
					// touching player. Restore the ordinary interval BEFORE that happens.
					if (!Pickup->IsPendingKillPending()) { Pickup->AUTPickup::WakeUpTimer(); }
				}
				return;
			}
		}
	}
}

void NCSACTFPickupOpening::Cancel(UWorld* World)
{
	TArray<FOpeningAmp>& Amps = OpeningState().Amps;
	for (int32 Index = Amps.Num() - 1; Index >= 0; --Index)
	{
		FOpeningAmp Amp = Amps[Index];
		if (!Amp.World.IsValid() || !Amp.Pickup.IsValid() || Amp.World.Get() == World)
		{
			Amps.RemoveAtSwap(Index);
			RestoreAmp(Amp);
			if (Amp.World.IsValid()) { Amp.World->GetTimerManager().ClearTimer(Amp.OpeningHandle); }
		}
	}
}

void NCSACTFPickupOpening::Prepare(AUTGameMode* GameMode)
{
	if (!IsSACTF(GameMode) || !GameMode->GetWorld()) { return; }
	UWorld* World = GameMode->GetWorld();
	Cancel(World);
	for (TActorIterator<AUTPickupInventory> It(World); It; ++It)
	{
		AUTPickupInventory* Pickup = *It;
		if (!Pickup || Pickup->IsPendingKillPending()) { continue; }
		const UClass* Inventory = Cast<UClass>(ReadObject(Pickup, TEXT("InventoryType")));
		if (!Inventory) { continue; }
		UBoolProperty* Delayed = FindField<UBoolProperty>(Pickup->GetClass(), TEXT("bDelayedSpawn"));
		if (!Delayed) { continue; }
		if (Inventory->IsChildOf(AUTArmor::StaticClass()))
		{
			// Armour (including belt) is ready at the opening reset, and on later
			// round resets. Pickup-to-pickup intervals remain the map's values.
			Delayed->SetPropertyValue_InContainer(Pickup, false);
		}
		else if (HasClassPath(Inventory, TEXT("/Game/Blueprints/UT+/UT+/UTPlusNew/UT+SAmp.UT+SAmp_C")))
		{
			UFloatProperty* Respawn = FindField<UFloatProperty>(Pickup->GetClass(), TEXT("RespawnTime"));
			UBoolProperty* Fixed = FindField<UBoolProperty>(Pickup->GetClass(), TEXT("bFixedRespawnInterval"));
			if (!Respawn || !Fixed || Fixed->GetPropertyValue_InContainer(Pickup)
				|| Respawn->GetPropertyValue_InContainer(Pickup) <= 0.f) { continue; }
			FOpeningAmp Amp;
			Amp.World = World;
			Amp.Pickup = Pickup;
			Amp.RespawnTime = Respawn->GetPropertyValue_InContainer(Pickup);
			Amp.bDelayedSpawn = Delayed->GetPropertyValue_InContainer(Pickup);
			OpeningState().Amps.Add(Amp);
			// Stock Reset/StartSleeping publishes this interval with its sleeping
			// State and drives the pre-spawn effect/countdown on existing clients.
			Respawn->SetPropertyValue_InContainer(Pickup, OpeningAmpDelay);
			Delayed->SetPropertyValue_InContainer(Pickup, true);
		}
	}
}

void NCSACTFPickupOpening::Finish(AUTGameMode* GameMode)
{
	if (!GameMode || !GameMode->HasAuthority()) { return; }
	UWorld* World = GameMode->GetWorld();
	TArray<FOpeningAmp>& Amps = OpeningState().Amps;
	for (int32 Index = Amps.Num() - 1; Index >= 0; --Index)
	{
		FOpeningAmp& Amp = Amps[Index];
		if (!Amp.World.IsValid() || !Amp.Pickup.IsValid()) { Amps.RemoveAtSwap(Index); continue; }
		if (Amp.World.Get() != World) { continue; }
		AUTPickup* Pickup = Amp.Pickup.Get();
		// AUTPickup's unreflected timer handle is already used by WipeoutGame's
		// shipped pickup scheduling. Keep the SAME handle so Reset, PreReplication,
		// AI time queries and stock pre-spawn effects see the actual deadline.
		const float Remaining = World->GetTimerManager().GetTimerRemaining(Pickup->WakeUpTimerHandle);
		if (Remaining <= 0.f || Pickup->IsPendingKillPending())
		{
			RestoreAmp(Amp);
			Amps.RemoveAtSwap(Index);
			continue; // Blueprint reset destroyed/replaced/woke this pickup.
		}
		World->GetTimerManager().SetTimer(Pickup->WakeUpTimerHandle,
			FTimerDelegate::CreateStatic(&WakeOpeningAmp, Amp.Pickup), Remaining, false);
		Amp.OpeningHandle = Pickup->WakeUpTimerHandle;
	}
}
