// NCAmpRespawnFix.cpp — server-side amp respawn-interval correction. No UObject, no UHT, no
// replicated state: a pure module-level hook, so shipping this is a server-DLL-only deploy
// (no client roll, no NETCODE_PLUGIN_VERSION bump).
//
// WHY: the community BP mutators (NCWepMut / NCStockWeapons — standalone BP children of stock
// UTMutator, no C++ base to hook) swap the map amp via CheckRelevance. In affected modes outside
// CTF/iCTF they erroneously spawn the CTF amp pickup, whose BP RespawnTime is tuned for CTF
// pacing (1:50) instead of the intended 90s. The swap itself can't be prevented BP-side without
// a pak recook, so this follows up from C++: shortly after each server world initialises, sweep
// amp pickups and force RespawnTime to the target in DM, TDM, and FlagRun/Blitz only.
//
// MECHANICS (verified in stock source): AUTPickup reads RespawnTime when each new sleep cycle
// begins. One property write per pickup INSTANCE (never the class default; a CDO write could leak
// across worlds in one process) therefore retunes subsequent respawns. An already-running initial
// sleep keeps its old deadline; deliberately do not re-enter BlueprintNativeEvent StartSleeping()
// here, because public game-mode/pickup Blueprints may destroy or replace pickups from that path.
//
// ATTACHMENT: FWorldDelegates::OnPostWorldInitialization (module-level; precedent — this module
// already hooks FCoreUObjectDelegates::PreLoadMap). The GameMode does not exist yet at world-init
// time, so the sweep runs on a short timer and retries briefly until the mode is up; by then the
// BP mutators' CheckRelevance swap has long since happened (it runs during level actor init).
// The sweep is positively restricted to the stock native DM, TDM, and FlagRun/Blitz roots plus
// Blueprint-only descendants rooted directly in them. Native derivatives such as Duel, Showdown,
// Shaft Arena, and FlagRun PvE/Siege are not treated as those three modes. In particular, using
// AUTTeamGameMode as the gate would be far too broad.
//
// IDENTITY: pickup is an AUTPickupInventory whose InventoryType derives AUTTimedPowerup AND either
// the inventory CDO's StatsNameCount == "UDamageCount" (the stock amp stat identity, inherited by
// any BP child of the stock UDamage) or the inventory class path contains a configured token —
// the fallback for a from-scratch amp inventory that never set the stat names. Matching stock 90s
// amps is expected and harmless (the write is a no-op skip).
//
// Mod.ini ([NetcodePlus]):
//   AmpRespawnFix=1              kill-switch; 0 = do nothing (default 1)
//   AmpRespawnSeconds=90         target interval (default 90)
//   AmpMatchTokens=UDamage,Amp   CSV, case-insensitive, matched vs the inventory class PATH
//
// BUILD NOTE: NetcodePlus.h MUST be the first include. PCH mode is UseExplicitOrSharedPCHs, and
// NetcodePlus.h is the module PCH anchor (it pulls CoreMinimal.h then UnrealTournament.h in order).
// The original of this file opened with "UnrealTournament.h" directly, which skipped that anchor and
// collapsed the stock UT public headers with an "undefined type" cascade (UFont / ULocalMessage /
// ConstructorHelpers) — the feedback_pch_first_include failure mode. Every other plugin .cpp leads
// with its own header, which leads with NetcodePlus.h; this headerless file leads with it directly.
#include "NetcodePlus.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "UTPickupInventory.h"
#include "UTInventory.h"
#include "UTTimedPowerup.h"
#include "UTDMGameMode.h"
#include "UTTeamDMGameMode.h"
#include "UTFlagRunGame.h"
#include "GameFramework/GameModeBase.h"

namespace
{

void AmpFixReadConfig(int32& bOutEnabled, float& OutTarget, TArray<FString>& OutTokens)
{
	bOutEnabled = 1;
	OutTarget = 90.f;
	FString TokensCsv = TEXT("UDamage,Amp");
	// Same pattern as ElimPlusGame/MutBotEvents: absent keys leave the defaults untouched.
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->GetInt(TEXT("NetcodePlus"), TEXT("AmpRespawnFix"), bOutEnabled, ModIni);
		GConfig->GetFloat(TEXT("NetcodePlus"), TEXT("AmpRespawnSeconds"), OutTarget, ModIni);
		GConfig->GetString(TEXT("NetcodePlus"), TEXT("AmpMatchTokens"), TokensCsv, ModIni);
	}
	TokensCsv.ParseIntoArray(OutTokens, TEXT(","), true);
	for (FString& Token : OutTokens)
	{
		Token.Trim();          // UE4.15: mutates in place (no TrimStartAndEnd here)
		Token.TrimTrailing();
	}
}

bool AmpFixIsTargetMode(const AGameModeBase* GameMode)
{
	// IsA(AUTTeamDMGameMode) also accepts Duel/Showdown and IsA(AUTFlagRunGame) accepts Siege.
	// Strip only Blueprint-generated layers, then require one of the three exact native roots.
	const UClass* NativeModeClass = GameMode != nullptr ? GameMode->GetClass() : nullptr;
	while (NativeModeClass != nullptr && NativeModeClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		NativeModeClass = NativeModeClass->GetSuperClass();
	}
	return NativeModeClass == AUTDMGameMode::StaticClass()
		|| NativeModeClass == AUTTeamDMGameMode::StaticClass()
		|| NativeModeClass == AUTFlagRunGame::StaticClass();
}

bool AmpFixIsAmpPickup(const AUTPickupInventory* Pickup, const TArray<FString>& Tokens)
{
	UClass* InvClass = Pickup->GetInventoryType();
	if (InvClass == nullptr || !InvClass->IsChildOf(AUTTimedPowerup::StaticClass()))
	{
		return false;
	}
	// Primary identity: the stock amp's pickup-count stat, inherited by BP children of UDamage.
	const AUTInventory* InvCDO = InvClass->GetDefaultObject<AUTInventory>();
	if (InvCDO != nullptr && InvCDO->StatsNameCount == FName(TEXT("UDamageCount")))
	{
		return true;
	}
	// Fallback: configured path tokens (case-insensitive), for amp inventories with no stat names.
	const FString InvPath = InvClass->GetPathName();
	for (const FString& Token : Tokens)
	{
		if (!Token.IsEmpty() && InvPath.Contains(Token))
		{
			return true;
		}
	}
	return false;
}

void AmpFixSweep(TWeakObjectPtr<UWorld> WeakWorld, int32 Attempt)
{
	UWorld* World = WeakWorld.Get();
	if (World == nullptr)
	{
		return; // GameInstance timers can outlive a world; the weak pointer makes that callback a no-op.
	}

	AGameModeBase* GameMode = World->GetAuthGameMode();
	if (GameMode == nullptr)
	{
		// World initialised but the mode hasn't spawned yet — retry briefly (10s cap), then give up quietly.
		if (Attempt < 20)
		{
			FTimerHandle Unused;
			World->GetTimerManager().SetTimer(Unused,
				FTimerDelegate::CreateStatic(&AmpFixSweep, WeakWorld, Attempt + 1), 0.5f, false);
		}
		return;
	}

	if (!AmpFixIsTargetMode(GameMode))
	{
		UE_LOG(LogGameMode, Verbose, TEXT("[AmpFix] mode %s is not DM, TDM, or FlagRun — skipping"),
			*GameMode->GetClass()->GetName());
		return;
	}

	int32 bEnabled = 1;
	float Target = 90.f;
	TArray<FString> Tokens;
	AmpFixReadConfig(bEnabled, Target, Tokens);
	if (bEnabled == 0 || Target <= 0.f)
	{
		return;
	}

	for (TActorIterator<AUTPickupInventory> It(World); It; ++It)
	{
		AUTPickupInventory* Pickup = *It;
		if (Pickup == nullptr || Pickup->IsPendingKillPending() || !AmpFixIsAmpPickup(Pickup, Tokens))
		{
			continue;
		}
		if (Pickup->RespawnTime <= 0.f || FMath::IsNearlyEqual(Pickup->RespawnTime, Target, 0.01f))
		{
			continue; // already correct (stock amps land here) — quiet no-op
		}

		const float OldTime = Pickup->RespawnTime;
		Pickup->RespawnTime = Target; // instance only — never the class default

		// Property-only correction: an active sleep keeps its current deadline. Avoid dispatching the
		// BlueprintNativeEvent StartSleeping(), which can destroy/replace pickups in public game modes.
		UE_LOG(LogGameMode, Log, TEXT("[AmpFix] %s inv=%s RespawnTime %.0f -> %.0f (mode %s)"),
			*Pickup->GetName(), *GetNameSafe(Pickup->GetInventoryType()), OldTime, Target,
			*GameMode->GetClass()->GetName());
	}
}

void AmpFixOnPostWorldInit(UWorld* World, const UWorld::InitializationValues)
{
	// Server-side only (dedicated, listen, or offline standalone). Clients and editor asset worlds
	// bail here — the module loads everywhere, the hook acts nowhere it shouldn't.
	if (World == nullptr || !World->IsGameWorld() || World->GetNetMode() == NM_Client)
	{
		return;
	}
	// TimerManager exists from world construction; the 2s delay lands well after level actor init
	// (and therefore after the BP mutators' CheckRelevance amp swap) on any synchronous server load.
	FTimerHandle Unused;
	World->GetTimerManager().SetTimer(Unused,
		FTimerDelegate::CreateStatic(&AmpFixSweep, TWeakObjectPtr<UWorld>(World), 0), 2.0f, false);
}

FDelegateHandle GAmpFixWorldInitHandle;

} // anonymous namespace

// Called from FNetcodePlus::StartupModule / ShutdownModule (extern-declared there).
void RegisterNCAmpRespawnFix()
{
	GAmpFixWorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(&AmpFixOnPostWorldInit);
}

void UnregisterNCAmpRespawnFix()
{
	if (GAmpFixWorldInitHandle.IsValid())
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(GAmpFixWorldInitHandle);
		GAmpFixWorldInitHandle.Reset();
	}
}
