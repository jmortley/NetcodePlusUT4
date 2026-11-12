#pragma once

class AUTGameMode;
class UWorld;

// Server-only, instance-only opening timing. Bracket the first live match reset;
// later resets retain each amp's ordinary respawn interval.
namespace NCSACTFPickupOpening
{
	void Prepare(AUTGameMode* GameMode);
	void Finish(AUTGameMode* GameMode);
	void Cancel(UWorld* World);
}
