#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "../NCPlusXTDMRules.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCPlusXTDMSeatsTest, "NetcodePlus.XTDM.SeatPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCPlusXTDMSeatsTest::RunTest(const FString& Parameters)
{
	using namespace NCPlusXTDMRules;
	const int32 Empty[4] = { 0, 0, 0, 0 };
	TestEqual(TEXT("Requested yellow wins an equal-size choice"), int32(PickSmallestTeam(Empty, 2, 3)), 3);
	const int32 Reserved[4] = { 2, 1, 2, 0 };
	TestEqual(TEXT("Reservations consume seats and smallest team wins"), int32(PickSmallestTeam(Reserved, 2, 0)), 3);
	const int32 Full[4] = { 2, 2, 2, 2 };
	TestEqual(TEXT("Full roster has no spare seat"), int32(PickSmallestTeam(Full, 2, 0)), 255);
	TestTrue(TEXT("Default eight-player roster is complete"), IsFullRoster(Full, 2));
	TestFalse(TEXT("A missing yellow player is not a full roster"), IsFullRoster(Reserved, 2));
	const int32 Sixteen[4] = { 4, 4, 4, 4 };
	TestTrue(TEXT("Four per team is supported"), IsFullRoster(Sixteen, 4));
	TestFalse(TEXT("Sixteen players do not fit a two-per-team configuration"), IsFullRoster(Sixteen, 2));
	const int32 Overfull[4] = { 3, 2, 2, 1 };
	TestFalse(TEXT("Equal total population cannot hide an overfull team"), IsFullRoster(Overfull, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCPlusXTDMHumanSeatsTest, "NetcodePlus.XTDM.HumanSeatPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCPlusXTDMHumanSeatsTest::RunTest(const FString& Parameters)
{
	using namespace NCPlusXTDMRules;
	const int32 NoBots[4] = { 0, 0, 0, 0 };
	const int32 Full[4] = { 2, 2, 2, 2 };
	TestEqual(TEXT("Full humans or reservations cannot be displaced"), int32(PickHumanTeam(Full, NoBots, 2, 0)), 255);
	const int32 Bots[4] = { 0, 1, 0, 1 };
	TestEqual(TEXT("An actual bot makes a full team replaceable"), int32(PickHumanTeam(Full, Bots, 2, 0)), 1);
	TestEqual(TEXT("Preferred yellow wins among equally full replaceable teams"), int32(PickHumanTeam(Full, Bots, 2, 3)), 3);
	const int32 Spare[4] = { 2, 2, 1, 2 };
	TestEqual(TEXT("A free seat wins before replacing a bot on a larger team"), int32(PickHumanTeam(Spare, Bots, 2, 3)), 2);
	const int32 TiedSpare[4] = { 1, 1, 2, 2 };
	TestEqual(TEXT("Requested preference only breaks equal-size choices"), int32(PickHumanTeam(TiedSpare, NoBots, 2, 1)), 1);
	TestEqual(TEXT("Invalid preference still finds a valid open seat"), int32(PickHumanTeam(TiedSpare, NoBots, 2, 255)), 0);
	const int32 Overfull[4] = { 3, 2, 2, 2 };
	const int32 OnlyOverfullBot[4] = { 1, 0, 0, 0 };
	TestEqual(TEXT("Replacing one bot does not repair an overfull team"), int32(PickHumanTeam(Overfull, OnlyOverfullBot, 2, 0)), 255);
	const int32 Invalid[4] = { -1, 2, 2, 2 };
	TestEqual(TEXT("Invalid negative occupancy fails closed"), int32(PickHumanTeam(Invalid, NoBots, 2, 0)), 255);
	TestEqual(TEXT("Unsupported team size fails closed"), int32(PickHumanTeam(Full, Bots, 1, 0)), 255);
	const int32 Large[4] = { 4, 4, 4, 4 };
	TestEqual(TEXT("Replacement supports four-player teams"), int32(PickHumanTeam(Large, Bots, 4, 3)), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCPlusXTDMReadyRosterTest, "NetcodePlus.XTDM.ReadyRoster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCPlusXTDMReadyRosterTest::RunTest(const FString& Parameters)
{
	using namespace NCPlusXTDMRules;
	TestTrue(TEXT("One ready human may start an incomplete casual roster"), CanStartRoster(true, false, 1, true));
	TestFalse(TEXT("An unready human still blocks an incomplete roster"), CanStartRoster(true, false, 1, false));
	TestFalse(TEXT("A second unready human still blocks a full roster"), CanStartRoster(true, true, 2, false));
	TestFalse(TEXT("A bot-only full roster cannot start"), CanStartRoster(true, true, 0, true));
	TestFalse(TEXT("An empty roster cannot start"), CanStartRoster(true, false, 0, true));
	TestFalse(TEXT("Strict mode requires a full live roster"), CanStartRoster(false, false, 7, true));
	const int32 HumanAndBots[4] = { 2, 2, 2, 2 };
	TestTrue(TEXT("Strict mode counts live bots in occupancy but only the human needs ready-up"),
		CanStartRoster(false, IsFullRoster(HumanAndBots, 2), 1, true));
	const int32 OneMissing[4] = { 2, 2, 2, 1 };
	TestFalse(TEXT("Losing one occupant invalidates the strict countdown"),
		CanStartRoster(false, IsFullRoster(OneMissing, 2), 1, true));
	TestTrue(TEXT("Losing one bot does not invalidate a ready casual countdown"),
		CanStartRoster(true, IsFullRoster(OneMissing, 2), 1, true));
	TestFalse(TEXT("The last human leaving cancels even a full bot roster"), CanStartRoster(false, true, 0, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCPlusXTDMBotPolicyTest, "NetcodePlus.XTDM.BotPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCPlusXTDMBotPolicyTest::RunTest(const FString& Parameters)
{
	using namespace NCPlusXTDMRules;
	TestEqual(TEXT("Negative bot fill is clamped to zero"), ClampBotFillTarget(-1, 2, false), 0);
	TestEqual(TEXT("Zero bot fill stays zero"), ClampBotFillTarget(0, 2, false), 0);
	TestEqual(TEXT("A smaller explicit fill is retained"), ClampBotFillTarget(5, 2, false), 5);
	TestEqual(TEXT("Fill cannot exceed eight seats"), ClampBotFillTarget(1000, 2, false), 8);
	TestEqual(TEXT("Three-player teams support twelve seats"), ClampBotFillTarget(1000, 3, false), 12);
	TestEqual(TEXT("Four-player teams support sixteen seats"), ClampBotFillTarget(1000, 4, false), 16);
	TestEqual(TEXT("ForceNoBots overrides the requested fill"), ClampBotFillTarget(8, 2, true), 0);
	TestEqual(TEXT("Invalid small teams fail closed"), ClampBotFillTarget(8, 1, false), 0);
	TestEqual(TEXT("Invalid large teams fail closed"), ClampBotFillTarget(20, 5, false), 0);
	TestTrue(TEXT("Strict games admit bots before the roster locks"), CanAddBot(false, false, false));
	TestTrue(TEXT("Casual games admit bots before the roster locks"), CanAddBot(false, true, false));
	TestTrue(TEXT("Casual games may refill bots after the roster locks"), CanAddBot(true, true, false));
	TestFalse(TEXT("Strict locked games reject new bot seats"), CanAddBot(true, false, false));
	for (int32 Locked = 0; Locked < 2; ++Locked)
	{
		for (int32 Incomplete = 0; Incomplete < 2; ++Incomplete)
		{
			TestFalse(TEXT("An explicit draft never admits an unlisted bot"), CanAddBot(Locked != 0, Incomplete != 0, true));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCPlusXTDMWinnerTest, "NetcodePlus.XTDM.TeamWinner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCPlusXTDMWinnerTest::RunTest(const FString& Parameters)
{
	using namespace NCPlusXTDMRules;
	bool bTied = false;
	const int32 SecondPlaceTie[4] = { 12, 12, 20, 3 };
	TestEqual(TEXT("Green can win"), int32(FindLeader(SecondPlaceTie, bTied)), 2);
	TestFalse(TEXT("A second-place tie does not extend the match"), bTied);
	const int32 TopTie[4] = { 20, 12, 20, 3 };
	FindLeader(TopTie, bTied);
	TestTrue(TEXT("A first-place tie needs overtime"), bTied);
	const int32 Negative[4] = { -4, -2, -3, -1 };
	TestEqual(TEXT("Negative scores still have a valid yellow winner"), int32(FindLeader(Negative, bTied)), 3);
	TestFalse(TEXT("Negative scores are not automatically tied"), bTied);
	const int32 NewLeader[4] = { 2, 2, 3, 1 };
	TestEqual(TEXT("A later higher score replaces an earlier tie"), int32(FindLeader(NewLeader, bTied)), 2);
	TestFalse(TEXT("Earlier lower-score ties are discarded"), bTied);
	return true;
}
#endif
