// NCPawnDiagnostics.cpp - shipping-safe character/X-ray lifecycle diagnostics.

#include "UnrealTournament.h"
#include "TeamArenaCharacter.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCPPawnDbg, Log, All);

namespace
{
	void AddReason(FString& Reasons, const TCHAR* Reason)
	{
		if (!Reasons.IsEmpty())
		{
			Reasons += TEXT(",");
		}
		Reasons += Reason;
	}

	void DumpNCPPawns()
	{
		int32 WorldCount = 0;
		int32 CharacterCount = 0;
		int32 SuspectCount = 0;

		if (GEngine == nullptr)
		{
			UE_LOG(LogNCPPawnDbg, Warning, TEXT("[PawnDbg] no GEngine"));
			return;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld())
			{
				continue;
			}

			++WorldCount;
			AUTPlayerController* LocalPC = Cast<AUTPlayerController>(World->GetFirstPlayerController());
			AActor* ViewTarget = LocalPC != nullptr ? LocalPC->GetViewTarget() : nullptr;
			UE_LOG(LogNCPPawnDbg, Warning,
				TEXT("[PawnDbg] WORLD name=%s path=%s type=%d netMode=%d time=%.3f localPC=%s viewTarget=%s taccom=%d"),
				*World->GetName(), *World->GetPathName(), (int32)Context.WorldType, (int32)World->GetNetMode(),
				World->GetTimeSeconds(), LocalPC ? *LocalPC->GetName() : TEXT("none"),
				ViewTarget ? *ViewTarget->GetName() : TEXT("none"),
				(LocalPC && LocalPC->bTacComView) ? 1 : 0);

			for (TActorIterator<AUTCharacter> It(World); It; ++It)
			{
				AUTCharacter* Character = *It;
				if (Character == nullptr)
				{
					continue;
				}

				++CharacterCount;
				const USkeletalMeshComponent* BodyMesh = Character->GetMesh();
				const USkeletalMeshComponent* DepthMesh = Character->GetCustomDepthMesh();
				AController* Controller = Character->GetController();
				AUTPlayerState* PS = Cast<AUTPlayerState>(Character->PlayerState);
				ATeamArenaCharacter* TeamCharacter = Cast<ATeamArenaCharacter>(Character);

				const bool bBodyVisible = BodyMesh != nullptr && BodyMesh->IsVisible();
				const bool bBodyHiddenInGame = BodyMesh != nullptr && BodyMesh->bHiddenInGame;
				const bool bDepthRegistered = DepthMesh != nullptr && DepthMesh->IsRegistered();
				const bool bDepthVisible = DepthMesh != nullptr && DepthMesh->IsVisible();
				const bool bDepthHiddenInGame = DepthMesh != nullptr && DepthMesh->bHiddenInGame;
				const bool bDead = Character->IsDead();

				FString Reasons;
				if (bDepthRegistered && !bBodyVisible) { AddReason(Reasons, TEXT("hidden-body+depth")); }
				if (bDepthRegistered && bDead) { AddReason(Reasons, TEXT("dead+depth")); }
				if (Character->Health <= 0 && !bDead) { AddReason(Reasons, TEXT("health/dead-mismatch")); }
				if (PS != nullptr && PS->bOutOfLives && !bDead) { AddReason(Reasons, TEXT("out-of-lives/live-actor")); }
				if (Controller == nullptr && PS == nullptr && !bDead) { AddReason(Reasons, TEXT("unowned-live-pawn")); }
				if (Character->bHidden && bDepthRegistered) { AddReason(Reasons, TEXT("hidden-actor+depth")); }

				const bool bSuspect = !Reasons.IsEmpty();
				if (bSuspect)
				{
					++SuspectCount;
				}

				// UE4.15's FMsg::Logf_Internal overloads cannot accept this many
				// formatting arguments at once. Build the record in bounded chunks,
				// then emit one searchable log line.
				FString PawnLog = FString::Printf(
					TEXT("[PawnDbg]%s pawn=%s class=%s player=%s controller=%s owner=%s"),
					bSuspect ? TEXT("[SUSPECT]") : TEXT(""),
					*Character->GetName(), *Character->GetClass()->GetName(),
					PS ? *PS->PlayerName : TEXT("none"),
					Controller ? *Controller->GetName() : TEXT("none"),
					Character->GetOwner() ? *Character->GetOwner()->GetName() : TEXT("none"));
				PawnLog += FString::Printf(
					TEXT(" role=%d remoteRole=%d age=%.3f life=%.3f health=%d dead=%d tearOff=%d pendingKill=%d ragdoll=%d"),
					(int32)Character->Role, (int32)Character->GetRemoteRole(),
					Character->GetGameTimeSinceCreation(), Character->GetLifeSpan(), Character->Health,
					bDead ? 1 : 0, Character->bTearOff ? 1 : 0,
					Character->IsPendingKillPending() ? 1 : 0, Character->IsRagdoll() ? 1 : 0);
				PawnLog += FString::Printf(
					TEXT(" actorHidden=%d collision=%d loc=%s vel=%s"),
					Character->bHidden ? 1 : 0, Character->GetActorEnableCollision() ? 1 : 0,
					*Character->GetActorLocation().ToString(), *Character->GetVelocity().ToString());
				PawnLog += FString::Printf(
					TEXT(" body={registered=%d visible=%d hiddenInGame=%d} depth={exists=%d registered=%d visible=%d hiddenInGame=%d renderCustomDepth=%d stencil=%u}"),
					(BodyMesh && BodyMesh->IsRegistered()) ? 1 : 0, bBodyVisible ? 1 : 0,
					bBodyHiddenInGame ? 1 : 0,
					DepthMesh ? 1 : 0, bDepthRegistered ? 1 : 0, bDepthVisible ? 1 : 0,
					bDepthHiddenInGame ? 1 : 0, (DepthMesh && DepthMesh->bRenderCustomDepth) ? 1 : 0,
					DepthMesh ? (uint32)DepthMesh->CustomDepthStencilValue : 0u);
				PawnLog += FString::Printf(
					TEXT(" outline={outlined=%d unoccluded=%d forceOff=%d} ps={onlySpectator=%d outOfLives=%d inactive=%d} pingSpawnPending=%d reasons=%s"),
					Character->IsOutlined(255) ? 1 : 0, Character->GetOutlineWhenUnoccluded() ? 1 : 0,
					Character->bForceNoOutline ? 1 : 0,
					(PS && PS->bOnlySpectator) ? 1 : 0, (PS && PS->bOutOfLives) ? 1 : 0,
					(PS && PS->bIsInactive) ? 1 : 0,
					(TeamCharacter && TeamCharacter->bPingCompensatedSpawnPending) ? 1 : 0,
					Reasons.IsEmpty() ? TEXT("none") : *Reasons);
				UE_LOG(LogNCPPawnDbg, Warning, TEXT("%s"), *PawnLog);
			}
		}

		UE_LOG(LogNCPPawnDbg, Warning, TEXT("[PawnDbg] complete worlds=%d characters=%d suspects=%d"),
			WorldCount, CharacterCount, SuspectCount);
	}

	FAutoConsoleCommand GNCPPawnDumpCmd(
		TEXT("ncp.PawnDump"),
		TEXT("Dump character death, ownership, visibility, and CustomDepth state for spectator X-ray diagnosis."),
		FConsoleCommandDelegate::CreateStatic(&DumpNCPPawns));
}
