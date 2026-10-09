#include "NCShotOriginDiagnostics.h"
#include "UTCharacter.h"
#include "Engine/World.h"

FNCShotOriginScope* FNCShotOriginScope::Active = nullptr;

FNCShotOriginScope::FNCShotOriginScope(const AUTCharacter* InOwner)
    : Owner(InOwner), Previous(Active)
{
    // Even an inactive nested query masks its parent's observation.
    Active = this;
}

FNCShotOriginScope::~FNCShotOriginScope()
{
    Active = Previous;
}

void FNCShotOriginScope::ObserveStockLookup(const AUTCharacter* Character, const FVector& Result)
{
    if (!Active || !Character || Active->Owner != Character) return;
    FNCShotOriginScope& Observation = *Active;
    // Stock makes one lookup per origin query. Keep the first snapshot and
    // expose multiple calls as unresolved instead of combining their fields.
    if (++Observation.LookupCount != 1) return;
    Observation.LookupPosition = Result;
    Observation.SavedCount = Character->SavedPositions.Num();
    const float Now = Character->GetWorld()->GetTimeSeconds();

    // Describe the stock call which already returned Result. Do not replace it.
    // Preserve its flag-before-age ordering, including the first over-age sample
    // and the lack of a teleport boundary. Native tests compare against stock.
    for (int32 i = Character->SavedPositions.Num() - 1; i >= 0; --i)
    {
        const FSavedPosition& Position = Character->SavedPositions[i];
        if (Position.bShotSpawned)
        {
            Observation.MarkerIndex = i;
            Observation.MarkerServerTime = Position.Time;
            Observation.MarkerMoveStamp = Position.TimeStamp;
            Observation.MarkerAgeMs = 1000.f * (Now - Position.Time);
            Observation.bMarkerOverAge = Now - Position.Time > Character->MaxShotSynchDelay;
            Observation.bMarkerTeleported = Position.bTeleported;
            Observation.bResultMatches = Result == Position.Position;
            return;
        }
        if (Now - Position.Time > Character->MaxShotSynchDelay)
        {
            Observation.bReachedAgeCutoff = true;
            break;
        }
        Observation.bNewerTeleport |= Position.bTeleported;
    }
    Observation.bResultMatches = Result == Character->GetActorLocation();
}
