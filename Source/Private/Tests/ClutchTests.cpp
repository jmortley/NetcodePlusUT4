#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ClutchGameMode.h"
#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Misc/AutomationTest.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchDefaultsTest,
	"NetcodePlus.Clutch.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchDefaultsTest::RunTest(const FString& Parameters)
{
	const AClutchGameMode* GameMode = GetDefault<AClutchGameMode>();
	const AClutchRoundState* RoundState = GetDefault<AClutchRoundState>();

	TestNotNull(TEXT("Clutch game mode CDO exists"), GameMode);
	TestNotNull(TEXT("Clutch round-state CDO exists"), RoundState);
	if (!GameMode || !RoundState)
	{
		return false;
	}

	TestTrue(TEXT("Uses stock AUTPlayerState"),
		GameMode->PlayerStateClass == AUTPlayerState::StaticClass());
	TestTrue(TEXT("Uses stock AUTPlayerController"),
		GameMode->PlayerControllerClass == AUTPlayerController::StaticClass());
	TestTrue(TEXT("Uses native Clutch HUD"),
		GameMode->HUDClass == AClutchHUD::StaticClass());
	TestEqual(TEXT("First to nine"), GameMode->GoalScore, 9);
	TestFalse(TEXT("One-vs-one testing does not require a full six-player lobby"),
		GameMode->bRequireFull);
	TestEqual(TEXT("Three attacker hits"), GameMode->MaxAttackerHits, 3);
	TestEqual(TEXT("Sixty second round"), GameMode->RoundDurationSeconds, 60.0f);
	TestEqual(TEXT("Pole unlocks at 45 seconds"), GameMode->PoleUnlockDelaySeconds, 45.0f);
	TestTrue(TEXT("Round state replicates"), RoundState->GetIsReplicated());
	TestTrue(TEXT("Round state is always relevant"), RoundState->bAlwaysRelevant);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchRotationTest,
	"NetcodePlus.Clutch.Rules.Rotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchRotationTest::RunTest(const FString& Parameters)
{
	TArray<int32> Slots;
	Slots.Add(2);
	Slots.Add(0);
	Slots.Add(1);

	int32 Previous = INDEX_NONE;
	Previous = AClutchRoundState::SelectNextRotationSlot(Slots, Previous);
	TestEqual(TEXT("First attacker slot"), Previous, 0);
	Previous = AClutchRoundState::SelectNextRotationSlot(Slots, Previous);
	TestEqual(TEXT("Second attacker slot"), Previous, 1);
	Previous = AClutchRoundState::SelectNextRotationSlot(Slots, Previous);
	TestEqual(TEXT("Third attacker slot"), Previous, 2);
	Previous = AClutchRoundState::SelectNextRotationSlot(Slots, Previous);
	TestEqual(TEXT("Rotation wraps"), Previous, 0);

	TArray<int32> SparseSlots;
	SparseSlots.Add(0);
	SparseSlots.Add(2);
	TestEqual(TEXT("Rotation skips a missing slot"),
		AClutchRoundState::SelectNextRotationSlot(SparseSlots, 0), 2);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchRoundResolutionTest,
	"NetcodePlus.Clutch.Rules.RoundResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchRoundResolutionTest::RunTest(const FString& Parameters)
{
	const uint8 Attackers = 0;
	const uint8 Defenders = 1;
	const uint8 NoWinner = AClutchRoundState::NoTeam;

	TestEqual(TEXT("Live unresolved round"),
		AClutchRoundState::ResolveRoundWinner(Attackers, false, 3, true, false), NoWinner);
	TestEqual(TEXT("Timeout awards defenders"),
		AClutchRoundState::ResolveRoundWinner(Attackers, true, 0, false, true), Defenders);
	TestEqual(TEXT("All defenders eliminated awards attackers"),
		AClutchRoundState::ResolveRoundWinner(Attackers, false, 0, true, false), Attackers);
	TestEqual(TEXT("Same-frame trade awards attackers after final defender dies"),
		AClutchRoundState::ResolveRoundWinner(Attackers, false, 0, false, false), Attackers);
	TestEqual(TEXT("Attacker eliminated awards defenders"),
		AClutchRoundState::ResolveRoundWinner(Attackers, false, 1, false, false), Defenders);
	TestEqual(TEXT("Pole capture awards attackers"),
		AClutchRoundState::ResolveRoundWinner(Attackers, false, 1, true, true), Attackers);
	TestFalse(TEXT("Eight rounds is not enough"),
		AClutchRoundState::HasWonMatch(8, 8, 9, false, 2));
	TestTrue(TEXT("Literal first-to-nine ends at 9-8"),
		AClutchRoundState::HasWonMatch(9, 8, 9, false, 2));
	TestFalse(TEXT("Optional win-by-two keeps 9-8 alive"),
		AClutchRoundState::HasWonMatch(9, 8, 9, true, 2));
	TestTrue(TEXT("Optional win-by-two ends at 10-8"),
		AClutchRoundState::HasWonMatch(10, 8, 9, true, 2));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchDerivedRosterTest,
	"NetcodePlus.Clutch.Rules.DerivedRosterState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchDerivedRosterTest::RunTest(const FString& Parameters)
{
	AClutchRoundState* RoundState = GetMutableDefault<AClutchRoundState>();
	TestNotNull(TEXT("Round-state CDO exists"), RoundState);
	if (!RoundState)
	{
		return false;
	}

	const EClutchRoundPhase SavedPhase = RoundState->Phase;
	const uint8 SavedMaxHits = RoundState->MaxAttackerHits;
	RoundState->Phase = EClutchRoundPhase::Combat;
	RoundState->MaxAttackerHits = 3;

	FClutchRosterEntry Attacker;
	Attacker.PlayerRole = EClutchRole::Attacker;
	Attacker.PlayerStatus = EClutchStatus::Active;
	Attacker.HitsTaken = 1;
	TestTrue(TEXT("Active attacker participates"), RoundState->IsEntryRoundActive(Attacker));
	TestFalse(TEXT("Active attacker should not spectate"), RoundState->ShouldEntrySpectate(Attacker));
	TestTrue(TEXT("Attacker owns attacker loadout"), RoundState->EntryUsesAttackerWeapon(Attacker));
	TestEqual(TEXT("One hit leaves two armor pips"), RoundState->GetEntryArmorRemaining(Attacker), 2);

	Attacker.PlayerStatus = EClutchStatus::Benched;
	TestTrue(TEXT("Benched player spectates"), RoundState->ShouldEntrySpectate(Attacker));
	TestFalse(TEXT("Benched player owns no active weapon"), RoundState->EntryUsesAttackerWeapon(Attacker));

	RoundState->Phase = SavedPhase;
	RoundState->MaxAttackerHits = SavedMaxHits;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
