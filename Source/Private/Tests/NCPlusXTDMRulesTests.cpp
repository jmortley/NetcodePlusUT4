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
	TestFalse(TEXT("A missing yellow player prevents starting"), IsFullRoster(Reserved, 2));
	const int32 Sixteen[4] = { 4, 4, 4, 4 };
	TestTrue(TEXT("Four per team is supported"), IsFullRoster(Sixteen, 4));
	TestFalse(TEXT("Sixteen players do not fit a two-per-team configuration"), IsFullRoster(Sixteen, 2));
	const int32 Overfull[4] = { 3, 2, 2, 1 };
	TestFalse(TEXT("Equal total population cannot hide an overfull team"), IsFullRoster(Overfull, 2));
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
