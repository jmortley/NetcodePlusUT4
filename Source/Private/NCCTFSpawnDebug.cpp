#include "NCCTFSpawnDebug.h"
#include "NCPlusCTFGameMode.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "Components/LineBatchComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

namespace
{
	bool IsSpawnDebugWarmup(UWorld* World)
	{
		AUTGameState* GS = World ? World->GetGameState<AUTGameState>() : nullptr;
		return GS && GS->GetMatchState() == MatchState::WaitingToStart;
	}

	bool CanViewSpawnDebug(APlayerController* PC)
	{
		return PC && (PC->GetNetMode() == NM_Standalone
			|| NCPlusReflection::GetBool(PC->PlayerState, TEXT("bIsRconAdmin")));
	}

	bool ParseSpawnDebugPlayerId(const FString& Text, int32& Out)
	{
		if (Text.IsEmpty()) { return false; }
		int32 Value = 0;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Ch = Text[Index];
			if (Ch < '0' || Ch > '9') { return false; }
			const int32 Digit = Ch - '0';
			if (Value > (MAX_int32 - Digit) / 10) { return false; }
			Value = Value * 10 + Digit;
		}
		Out = Value;
		return true;
	}

	void SpawnBlocksCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || World->GetNetMode() == NM_Client) { return; }
		if (!Cast<ANCPlusCTFGameMode>(World->GetAuthGameMode())) { return; }
		const FString Action = Args.Num() ? Args[0].ToLower() : TEXT("on");
		if (Action != TEXT("on") && Action != TEXT("1") && Action != TEXT("off") && Action != TEXT("0"))
		{
			UE_LOG(LogGameMode, Warning, TEXT("Usage: rconexec ncp.SpawnBlocks [on|off] [PlayerID]"));
			return;
		}
		int32 TargetId = -1;
		if (Args.Num() > 2 || (Args.Num() == 2 && !ParseSpawnDebugPlayerId(Args[1], TargetId)))
		{
			UE_LOG(LogGameMode, Warning, TEXT("ncp.SpawnBlocks: PlayerID must be a nonnegative integer."));
			return;
		}
		for (TActorIterator<ANCCTFSpawnDebug> It(World); It; ++It) { It->Destroy(); }
		if (Action == TEXT("off") || Action == TEXT("0") || !IsSpawnDebugWarmup(World)) { return; }
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (!CanViewSpawnDebug(PC)) { continue; }
			FActorSpawnParameters Params;
			Params.Owner = PC;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ANCCTFSpawnDebug* Debug = World->SpawnActor<ANCCTFSpawnDebug>(Params);
			if (Debug) { Debug->TargetPlayerId = TargetId; }
		}
	}

	FAutoConsoleCommandWithWorldAndArgs SpawnBlocksConsoleCommand(
		TEXT("ncp.SpawnBlocks"),
		TEXT("Warmup spawn markers for RCON admins. rconexec ncp.SpawnBlocks [on|off] [PlayerID].\n")
		TEXT("Default perspective: your spectated player or your own player. Ends when warmup ends."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnBlocksCommand), ECVF_Default);

	const TCHAR* SpawnBlockReason(int32 Reason)
	{
		switch (Reason)
		{
			case 1: return TEXT("enemy sightline");
			case 2: return TEXT("enemy proximity");
			case 3: return TEXT("teammate sightline");
			case 4: return TEXT("teammate proximity");
			case 5: return TEXT("enemy flag carrier");
			case 6: return TEXT("unheld flag proximity");
			case 8: return TEXT("last killer");
			case 9: return TEXT("flag-out base rotation");
			case 11: return TEXT("recent team spawn");
			case 12: return TEXT("previous spawn/location");
			case 13: return TEXT("safer starts available");
			case 14: return TEXT("stock selector: NewCTF filters inactive");
			case 15: return TEXT("legacy selector: NewCTF filters inactive");
			default: return TEXT("eligible");
		}
	}

	FColor SpawnMarkerColor(uint8 State)
	{
		switch (State)
		{
			case 1: return FColor(255, 180, 0);
			case 2: return FColor::Red;
			case 3: return FColor::Cyan;
			case 4: return FColor(180, 180, 180);
			default: return FColor::Green;
		}
	}
}

ANCCTFSpawnDebug::ANCCTFSpawnDebug(const FObjectInitializer& OI) : Super(OI)
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	bAlwaysRelevant = false;
	SetReplicateMovement(false);
	SetActorHiddenInGame(false);
	PrimaryActorTick.bCanEverTick = true;
	NetUpdateFrequency = 4.f;
	Lines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("SpawnMarkerLines"));
	RootComponent = Lines;
	Lines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Lines->bGenerateOverlapEvents = false;
}

void ANCCTFSpawnDebug::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCCTFSpawnDebug, Markers);
}

void ANCCTFSpawnDebug::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerController* Viewer = Cast<APlayerController>(GetOwner());
	if (!IsSpawnDebugWarmup(GetWorld()))
	{
		Lines->Flush();
		for (UTextRenderComponent* Label : Labels) { Label->SetVisibility(false); }
		if (HasAuthority()) { Destroy(); }
		return;
	}
	if (HasAuthority())
	{
		if (!CanViewSpawnDebug(Viewer)) { Destroy(); return; }
		RefreshRemaining -= DeltaSeconds;
		if (RefreshRemaining <= 0.f)
		{
			RefreshRemaining = .25f;
			RefreshMarkers(Viewer);
		}
	}
	if (Viewer && Viewer->IsLocalController() && GetNetMode() != NM_DedicatedServer)
	{
		RenderMarkers(Viewer);
	}
}

void ANCCTFSpawnDebug::RefreshMarkers(APlayerController* Viewer)
{
	Markers.Reset();
	ANCPlusCTFGameMode* Game = Cast<ANCPlusCTFGameMode>(GetWorld()->GetAuthGameMode());
	if (!Game) { return; }
	AController* Target = nullptr;
	if (TargetPlayerId >= 0)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* PC = It->Get();
			if (PC && PC->PlayerState && NCPlusReflection::GetInt(PC->PlayerState, TEXT("PlayerId")) == TargetPlayerId)
			{
				Target = PC;
				break;
			}
		}
	}
	else
	{
		APawn* Watched = Cast<APawn>(Viewer->GetViewTarget());
		Target = Watched && Watched->GetController() ? Watched->GetController() : Viewer;
	}
	AUTPlayerState* PS = Target ? Cast<AUTPlayerState>(Target->PlayerState) : nullptr;
	TArray<FNCCTFSpawnDebugEntry> Snapshot;
	Game->GetSpawnDebugSnapshot(Target, Snapshot);
	if (Snapshot.Num() == 0)
	{
		FVector Location; FRotator Rotation;
		Viewer->GetPlayerViewPoint(Location, Rotation);
		FNCCTFSpawnMarker Marker;
		Marker.Location = Location + Rotation.Vector() * 250.f;
		Marker.State = 4;
		Marker.Label = TEXT("Spawn markers: select an active team player\nrconexec ncp.SpawnBlocks on <PlayerID>");
		Markers.Add(Marker);
	}
	for (const FNCCTFSpawnDebugEntry& Entry : Snapshot)
	{
		if (!Entry.Start) { continue; }
		FNCCTFSpawnMarker Marker;
		Marker.Location = Entry.Start->GetActorLocation();
		Marker.State = Entry.State;
		const TCHAR* Status = Entry.State == 2 ? TEXT("BLOCKED")
			: Entry.State == 1 ? TEXT("PRIMARY BLOCKED (fallback possible)")
			: Entry.State == 3 ? TEXT("CHOSEN") : Entry.State == 4 ? TEXT("INACTIVE") : TEXT("OPEN");
		Marker.Label = FString::Printf(TEXT("%s | %s\n%s: %s"), PS ? *PS->PlayerName : TEXT("?"),
			*Entry.Start->GetName(), Status, SpawnBlockReason(Entry.Reason));
		Markers.Add(Marker);
	}
	ForceNetUpdate();
}

void ANCCTFSpawnDebug::RenderMarkers(APlayerController* Viewer)
{
	Lines->Flush();
	FVector Eye; FRotator Rotation;
	Viewer->GetPlayerViewPoint(Eye, Rotation);
	while (Labels.Num() < Markers.Num())
	{
		UTextRenderComponent* Label = NewObject<UTextRenderComponent>(this);
		Label->SetHorizontalAlignment(EHTA_Center);
		Label->SetWorldSize(18.f);
		Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Label->RegisterComponent();
		Labels.Add(Label);
	}
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UTextRenderComponent* Label = Labels[Index];
		const bool bVisible = Index < Markers.Num();
		Label->SetVisibility(bVisible);
		if (!bVisible) { continue; }
		const FNCCTFSpawnMarker& Marker = Markers[Index];
		const FColor Color = SpawnMarkerColor(Marker.State);
		const FVector Top = Marker.Location + FVector(0.f, 0.f, 110.f);
		if (Label->Text.ToString() != Marker.Label) { Label->SetText(FText::FromString(Marker.Label)); }
		if (Label->TextRenderColor != Color) { Label->SetTextRenderColor(Color); }
		Label->SetWorldLocation(Top);
		Label->SetWorldRotation((Eye - Top).Rotation());
		// Component drawing survives Shipping builds and is owned by this actor.
		// Clearing it never flushes another feature's debug geometry.
		Lines->DrawLine(Marker.Location, Top, FLinearColor(Color), 0, 3.f, .1f);
		const FVector X(35.f, 0.f, 0.f), Y(0.f, 35.f, 0.f);
		Lines->DrawLine(Marker.Location - X - Y, Marker.Location + X + Y, FLinearColor(Color), 0, 4.f, .1f);
		Lines->DrawLine(Marker.Location - X + Y, Marker.Location + X - Y, FLinearColor(Color), 0, 4.f, .1f);
	}
}
