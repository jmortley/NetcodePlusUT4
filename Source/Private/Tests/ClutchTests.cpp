#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ClutchGameMode.h"
#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "Engine/Texture2D.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"


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
	TestEqual(TEXT("Attack-order picker allows fifteen seconds"),
		GameMode->AttackOrderSelectionSeconds, 15.0f);
	TestTrue(TEXT("Round state replicates"), RoundState->GetIsReplicated());
	TestTrue(TEXT("Round state is always relevant"), RoundState->bAlwaysRelevant);
	TestEqual(TEXT("Attack orders begin unlocked"),
		static_cast<int32>(RoundState->AttackOrderLockedMask), 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchRosterIdentityTest,
	"NetcodePlus.Clutch.Rules.RosterIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchRosterIdentityTest::RunTest(const FString& Parameters)
{
	AClutchRoundState* RoundState = GetMutableDefault<AClutchRoundState>();
	TestNotNull(TEXT("Round-state CDO exists"), RoundState);
	if (!RoundState)
	{
		return false;
	}

	// Identity lookups must only ever reclaim VACATED rows (PlayerState == nullptr,
	// as DetachPlayer leaves them). Hub bots can collide on session PlayerIds, and a
	// lookup that matched a LIVE row let one bot hijack another's entry, cross-linking
	// the roster until a team had no valid slots and selection wedged.
	const TArray<FClutchRosterEntry> SavedRoster = RoundState->Roster;
	RoundState->Roster.Reset();

	FClutchRosterEntry LiveRow;
	// Any non-null marker works; identity lookups never dereference it.
	LiveRow.PlayerState = GetMutableDefault<AUTPlayerState>();
	LiveRow.StablePlayerId = TEXT("name:barktooth");
	LiveRow.PlayerIdFallback = 7;
	LiveRow.TeamIndex = 0;
	RoundState->Roster.Add(LiveRow);

	TestNull(TEXT("A live row is never matched by stable id"),
		RoundState->FindEntryByIdentity(TEXT("name:barktooth"), INDEX_NONE));
	TestNull(TEXT("A live row is never matched by a colliding PlayerId"),
		RoundState->FindEntryByIdentity(TEXT("name:kali"), 7));

	// Vacate the row the way DetachPlayer does and identity reclaim opens up.
	RoundState->Roster[0].PlayerState = nullptr;

	TestNotNull(TEXT("A vacated row is reclaimed by the same stable id"),
		RoundState->FindEntryByIdentity(TEXT("name:barktooth"), INDEX_NONE));
	TestNotNull(TEXT("A vacated row is reclaimed via the PlayerId fallback"),
		RoundState->FindEntryByIdentity(TEXT("name:kali"), 7));
	TestNull(TEXT("An unmatched online uid never falls back to PlayerId reuse"),
		RoundState->FindEntryByIdentity(TEXT("uid:12345"), 7));

	RoundState->Roster = SavedRoster;
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchSpectatorRulesTest,
	"NetcodePlus.Clutch.Rules.Spectating",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchSpectatorRulesTest::RunTest(const FString& Parameters)
{
	FClutchRosterEntry AliveTeammate;
	AliveTeammate.TeamIndex = 1;
	AliveTeammate.RosterSlot = 0;
	AliveTeammate.PlayerRole = EClutchRole::Defender;
	AliveTeammate.PlayerStatus = EClutchStatus::Active;

	TestTrue(TEXT("Dead defender may view an alive active teammate"),
		AClutchRoundState::CanSpectateRosterEntry(1, AliveTeammate, true));

	FClutchRosterEntry SecondAliveTeammate = AliveTeammate;
	SecondAliveTeammate.RosterSlot = 1;
	TestTrue(TEXT("Spectator cycling may select another alive teammate"),
		AClutchRoundState::CanSpectateRosterEntry(1, SecondAliveTeammate, true));

	TestFalse(TEXT("A dead teammate is not a valid camera target"),
		AClutchRoundState::CanSpectateRosterEntry(1, AliveTeammate, false));
	TestFalse(TEXT("An enemy may not be spectated by a playing-team viewer"),
		AClutchRoundState::CanSpectateRosterEntry(0, AliveTeammate, true));

	FClutchRosterEntry BenchedTeammate = AliveTeammate;
	BenchedTeammate.PlayerStatus = EClutchStatus::Benched;
	TestFalse(TEXT("A benched teammate is not a live camera target"),
		AClutchRoundState::CanSpectateRosterEntry(1, BenchedTeammate, true));

	FClutchRosterEntry EliminatedTeammate = AliveTeammate;
	EliminatedTeammate.PlayerStatus = EClutchStatus::Eliminated;
	TestFalse(TEXT("An eliminated teammate is not a live camera target"),
		AClutchRoundState::CanSpectateRosterEntry(1, EliminatedTeammate, true));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchAttackOrderTest,
	"NetcodePlus.Clutch.Rules.AttackOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchAttackOrderTest::RunTest(const FString& Parameters)
{
	TArray<int32> EligibleSlots;
	EligibleSlots.Add(0);
	EligibleSlots.Add(1);
	EligibleSlots.Add(2);

	TArray<int32> ChosenOrder;
	ChosenOrder.Add(2);
	ChosenOrder.Add(0);
	ChosenOrder.Add(1);
	TestTrue(TEXT("A complete teammate permutation is accepted"),
		AClutchRoundState::IsValidAttackOrder(EligibleSlots, ChosenOrder));

	TArray<int32> DuplicateOrder;
	DuplicateOrder.Add(2);
	DuplicateOrder.Add(2);
	DuplicateOrder.Add(1);
	TestFalse(TEXT("Duplicate teammates are rejected"),
		AClutchRoundState::IsValidAttackOrder(EligibleSlots, DuplicateOrder));

	TArray<int32> MissingOrder;
	MissingOrder.Add(2);
	MissingOrder.Add(0);
	TestFalse(TEXT("A missing teammate is rejected"),
		AClutchRoundState::IsValidAttackOrder(EligibleSlots, MissingOrder));

	TArray<int32> ForeignOrder;
	ForeignOrder.Add(2);
	ForeignOrder.Add(0);
	ForeignOrder.Add(5);
	TestFalse(TEXT("A foreign roster slot is rejected"),
		AClutchRoundState::IsValidAttackOrder(EligibleSlots, ForeignOrder));

	// Happy path: nobody has disconnected, so the full order and the connected set are
	// identical and the rotation cycles the chosen permutation.
	int32 Previous = INDEX_NONE;
	Previous = AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ChosenOrder, Previous);
	TestEqual(TEXT("Chosen first attacker goes first"), Previous, 2);
	Previous = AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ChosenOrder, Previous);
	TestEqual(TEXT("Chosen second attacker goes second"), Previous, 0);
	Previous = AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ChosenOrder, Previous);
	TestEqual(TEXT("Chosen third attacker goes third"), Previous, 1);
	Previous = AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ChosenOrder, Previous);
	TestEqual(TEXT("Chosen order wraps"), Previous, 2);

	// Regression: the previous attacker (slot 0) disconnects mid-match. The full locked
	// order still lists slot 0, so the rotation must advance PAST it to the next teammate
	// (slot 1) instead of collapsing back to the front (slot 2) — which would give slot 2
	// consecutive team turns while skipping slot 1.
	TArray<int32> ConnectedAfterDrop;
	ConnectedAfterDrop.Add(2);
	ConnectedAfterDrop.Add(1);
	TestEqual(TEXT("Rotation advances past a disconnected previous attacker"),
		AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ConnectedAfterDrop, 0), 1);

	// After slot 1 takes its turn the sequence keeps wrapping over the connected slots.
	TestEqual(TEXT("Rotation continues after the skip"),
		AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ConnectedAfterDrop, 1), 2);

	// A fresh selection (no previous) starts at the first connected slot even when the
	// front of the locked order has disconnected.
	TArray<int32> ConnectedFrontGone;
	ConnectedFrontGone.Add(0);
	ConnectedFrontGone.Add(1);
	TestEqual(TEXT("Fresh selection skips a disconnected order front"),
		AClutchRoundState::SelectNextOrderedSlot(ChosenOrder, ConnectedFrontGone, INDEX_NONE), 0);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchPoleProgressTest,
	"NetcodePlus.Clutch.Rules.PoleProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchPoleProgressTest::RunTest(const FString& Parameters)
{
	const float Capturing = AClutchRoundState::AdvancePoleProgress(
		20.0f, 1.0f, true, false, 5.0f, 10.0f);
	TestTrue(TEXT("Uncontested attacker advances the pole"),
		FMath::IsNearlyEqual(Capturing, 40.0f));

	const float Contested = AClutchRoundState::AdvancePoleProgress(
		Capturing, 1.0f, true, true, 5.0f, 10.0f);
	TestTrue(TEXT("Attacker and defender freeze progress"),
		FMath::IsNearlyEqual(Contested, Capturing));

	const float DefenderOnly = AClutchRoundState::AdvancePoleProgress(
		Contested, 1.0f, false, true, 5.0f, 10.0f);
	TestTrue(TEXT("Defender-only pole decays"),
		FMath::IsNearlyEqual(DefenderOnly, 30.0f));

	const float EmptyPole = AClutchRoundState::AdvancePoleProgress(
		DefenderOnly, 1.0f, false, false, 5.0f, 10.0f);
	TestTrue(TEXT("Unattended pole decays"),
		FMath::IsNearlyEqual(EmptyPole, 20.0f));

	TestTrue(TEXT("Capture clamps at one hundred"), FMath::IsNearlyEqual(
		AClutchRoundState::AdvancePoleProgress(95.0f, 1.0f, true, false, 5.0f, 10.0f),
		100.0f));
	TestTrue(TEXT("Decay clamps at zero"), FMath::IsNearlyEqual(
		AClutchRoundState::AdvancePoleProgress(5.0f, 1.0f, false, false, 5.0f, 10.0f),
		0.0f));
	TestTrue(TEXT("Non-positive delta does not move the pole"), FMath::IsNearlyEqual(
		AClutchRoundState::AdvancePoleProgress(55.0f, 0.0f, true, false, 5.0f, 10.0f),
		55.0f));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchRoleDamageTest,
	"NetcodePlus.Clutch.Rules.RoleDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchRoleDamageTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Defender hit removes one attacker armor segment"),
		AClutchRoundState::ResolveRoleDamage(
			EClutchRole::Defender, EClutchRole::Attacker, 100, 1000),
		100);
	TestEqual(TEXT("Attacker hit is lethal to a defender"),
		AClutchRoundState::ResolveRoleDamage(
			EClutchRole::Attacker, EClutchRole::Defender, 100, 1000),
		1000);
	TestEqual(TEXT("Defender friendly fire is suppressed"),
		AClutchRoundState::ResolveRoleDamage(
			EClutchRole::Defender, EClutchRole::Defender, 100, 1000),
		0);
	TestEqual(TEXT("Attacker self-role damage is suppressed"),
		AClutchRoundState::ResolveRoleDamage(
			EClutchRole::Attacker, EClutchRole::Attacker, 100, 1000),
		0);
	TestEqual(TEXT("Invalid negative damage is clamped away"),
		AClutchRoundState::ResolveRoleDamage(
			EClutchRole::Defender, EClutchRole::Attacker, -100, 1000),
		0);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchAmmoRegenTest,
	"NetcodePlus.Clutch.Rules.AmmoRegen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchAmmoRegenTest::RunTest(const FString& Parameters)
{
	// A full clip never regenerates and parks a stale carry back to zero.
	float Accumulator = 1.0f;
	TestEqual(TEXT("Full magazine grants nothing"),
		AClutchRoundState::AdvanceAmmoRegen(Accumulator, 1.5f, 4, 4, 1.5f), 0);
	TestTrue(TEXT("Full magazine parks the timer at zero"),
		FMath::IsNearlyEqual(Accumulator, 0.0f));

	// Below capacity, nothing arrives until a whole interval has elapsed.
	Accumulator = 0.0f;
	TestEqual(TEXT("Sub-interval grants nothing"),
		AClutchRoundState::AdvanceAmmoRegen(Accumulator, 0.05f, 1, 4, 1.5f), 0);

	// From one round, refills exactly one per 1.5s until the four-round clip is full.
	// A 0.5s tick divides 1.5 cleanly, so the step count is deterministic.
	Accumulator = 0.0f;
	int32 Ammo = 1;
	int32 Steps = 0;
	while (Ammo < 4 && Steps < 100)
	{
		Ammo += AClutchRoundState::AdvanceAmmoRegen(Accumulator, 0.5f, Ammo, 4, 1.5f);
		++Steps;
	}
	TestEqual(TEXT("Refills to a full four-round clip"), Ammo, 4);
	TestEqual(TEXT("Three rounds at three 0.5s ticks each"), Steps, 9);

	// A long hitch grants several rounds at once but never overfills the clip.
	Accumulator = 0.0f;
	TestEqual(TEXT("Catch-up is capped at the remaining deficit"),
		AClutchRoundState::AdvanceAmmoRegen(Accumulator, 10.0f, 2, 4, 1.5f), 2);
	TestTrue(TEXT("Hitting the cap re-parks the timer"),
		FMath::IsNearlyEqual(Accumulator, 0.0f));

	// A disabled feature (zero magazine) is inert regardless of elapsed time.
	Accumulator = 3.0f;
	TestEqual(TEXT("Disabled magazine grants nothing"),
		AClutchRoundState::AdvanceAmmoRegen(Accumulator, 1.5f, 0, 0, 1.5f), 0);

	// SM-style empty penalty: arming a negative carry (-pause) delays the first round
	// by pause + interval. -0.5 -> +1.5 is 2.0s; at a 0.5s tick that is four ticks.
	Accumulator = -0.5f;
	int32 PausedAmmo = 0;
	int32 PausedSteps = 0;
	while (PausedAmmo < 1 && PausedSteps < 200)
	{
		PausedAmmo += AClutchRoundState::AdvanceAmmoRegen(Accumulator, 0.5f, PausedAmmo, 4, 1.5f);
		++PausedSteps;
	}
	TestEqual(TEXT("Empty pause delays the first round by pause + interval"), PausedSteps, 4);

	// A negative carry grants nothing mid-pause and just keeps counting up.
	Accumulator = -0.5f;
	TestEqual(TEXT("Armed pause grants nothing mid-pause"),
		AClutchRoundState::AdvanceAmmoRegen(Accumulator, 0.25f, 0, 4, 1.5f), 0);
	TestTrue(TEXT("Armed pause keeps counting up from negative"),
		FMath::IsNearlyEqual(Accumulator, -0.25f));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClutchHUDAssetsTest,
	"NetcodePlus.Clutch.Assets.HUD",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClutchHUDAssetsTest::RunTest(const FString& Parameters)
{
	const TCHAR* const LegacyTextures[] = {
		TEXT("clutch_attack_progress"),
		TEXT("clutch_attack_rail_back"),
		TEXT("clutch_attack_rail_front"),
		TEXT("clutch_attacking"),
		TEXT("clutch_circle"),
		TEXT("clutch_defend_progress"),
		TEXT("clutch_defend_rail_back"),
		TEXT("clutch_defend_rail_front"),
		TEXT("clutch_inner_circle"),
		TEXT("clutch_insta"),
		TEXT("clutch_item_placeholder"),
		TEXT("clutch_left_gadget"),
		TEXT("clutch_right_gadget"),
		TEXT("clutch_rocket"),
		TEXT("clutch_shield")
	};

	for (const TCHAR* TextureName : LegacyTextures)
	{
		const FString ObjectPath = FString::Printf(
			TEXT("/NetcodePlus/Clutch/HUD/Textures/Legacy/%s.%s"),
			TextureName, TextureName);
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		TestNotNull(FString::Printf(TEXT("Plugin HUD texture loads: %s"), TextureName), Texture);
	}

	const TCHAR* const ResourceFiles[] = {
		TEXT("hud_top_bottom2.png"),
		TEXT("hud_bottom.png"),
		TEXT("hud_bottom_Blue.png")
	};
	const FString GamePluginsDir = FPaths::GamePluginsDir();
	const FString ResourceDir = FPaths::Combine(
		*GamePluginsDir, TEXT("NetcodePlus/Resources/ClutchHUD"));
	for (const TCHAR* FileName : ResourceFiles)
	{
		const FString FilePath = FPaths::Combine(*ResourceDir, FileName);
		TestTrue(FString::Printf(TEXT("Recovered HUD resource exists: %s"), FileName),
			FPaths::FileExists(FilePath));
	}
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
