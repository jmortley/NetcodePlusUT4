#pragma once

#include "CoreMinimal.h"

class AUTCharacter;

// Game-thread-only, synchronous observation of one GetFireStartLoc query. No
// actor fields, movement flags or network messages are changed by this scope.
struct FNCShotOriginScope
{
    explicit FNCShotOriginScope(const AUTCharacter* InOwner);
    ~FNCShotOriginScope();
    FNCShotOriginScope(const FNCShotOriginScope&) = delete;
    FNCShotOriginScope& operator=(const FNCShotOriginScope&) = delete;

    static void ObserveStockLookup(const AUTCharacter* Character, const FVector& Result);

    const AUTCharacter* Owner;
    int32 LookupCount = 0;
    int32 SavedCount = INDEX_NONE;
    int32 MarkerIndex = INDEX_NONE;
    bool bResultMatches = false;
    bool bReachedAgeCutoff = false;
    bool bNewerTeleport = false;
    bool bMarkerTeleported = false;
    bool bMarkerOverAge = false;
    float MarkerServerTime = -1.f;
    float MarkerMoveStamp = -1.f;
    float MarkerAgeMs = -1.f;
    FVector LookupPosition = FVector::ZeroVector;

private:
    static FNCShotOriginScope* Active;
    FNCShotOriginScope* Previous;
};
