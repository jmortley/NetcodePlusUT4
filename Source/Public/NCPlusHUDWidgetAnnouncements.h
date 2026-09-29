// NCPlusHUDWidgetAnnouncements - stock announcements widget plus the opt-in
// "one name per kill message" presentation toggle.
//
// Stock UUTKillerMessage is unique with bCombineEmphasisText: every kill within
// its 3 s lifetime appends the victim's name to the live "You killed ..." entry,
// so killing the same player repeatedly reads "You killed ToX & ToX & ToX".
// With NCPlusDisplaySettings::GetCollapseRepeatedKillNames() on, a combined
// entry lists each victim player state once, joined with the message class's own
// localized CombineEmphasisText, and its measured Text matches what is drawn.
//
// The message class, grouping, prefix, lifetime and slot are all stock; with the
// toggle off AddMessage leaves every entry exactly as stock built it. Installed in
// place of /Script/UnrealTournament.UTHUDWidgetAnnouncements by every NetcodePlus
// HUD, and mapped to the nchud "announcements" alias (NCPlusHUDAliases::GetAliasForClass).
#pragma once

#include "NetcodePlus.h"
#include "UTHUDWidgetAnnouncements.h"
#include "NCPlusHUDWidgetAnnouncements.generated.h"

/** One victim of the live combined killer entry. */
struct FNCPlusKillVictim
{
	TWeakObjectPtr<APlayerState> PlayerState;
	FString PlayerName;   // as stock captured it when the kill arrived
};

/**
 * Victims of the live killer entry, in arrival order and unique by player state.
 * Kept whether or not the toggle is on, so switching it on mid-window applies to
 * the next kill. Only one killer entry of a class is live at a time (bIsUnique),
 * and queue entries shift as others expire, so the record is matched to the live
 * entry by class and emphasis text, never by queue index.
 */
struct FNCPlusKillChain
{
	const UClass* MessageClass = nullptr;  // identity only, never dereferenced
	TArray<FNCPlusKillVictim> Victims;
	FString LastEmphasis;                 // the entry's EmphasisText after our last AddMessage
	bool bTracked = false;                // false = entry contents unknown; leave it stock
};

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidgetAnnouncements : public UUTHUDWidgetAnnouncements
{
	GENERATED_UCLASS_BODY()

public:
	virtual void AddMessage(int32 QueueIndex, TSubclassOf<class UUTLocalMessage> MessageClass, uint32 MessageIndex, FText LocalMessageText, int32 MessageCount, APlayerState* RelatedPlayerState_1, APlayerState* RelatedPlayerState_2, UObject* OptionalObject) override;

private:
	FNCPlusKillChain KillChain;
};
