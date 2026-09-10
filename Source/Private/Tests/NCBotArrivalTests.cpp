#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MutBotEvents.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCBotArrivalIdentityTest,
	"NetcodePlus.BotArrival.Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCBotArrivalIdentityTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Account IDs normalize case"),
		FBotArrivalLedger::CanonicalId(TEXT("ABCDEF0123456789ABCDEF0123456789")),
		FString(TEXT("abcdef0123456789abcdef0123456789")));
	TestTrue(TEXT("Nil account IDs are not presence evidence"),
		FBotArrivalLedger::CanonicalId(TEXT("00000000000000000000000000000000")).IsEmpty());
	TestTrue(TEXT("Shared passwords are not account IDs"), FBotArrivalLedger::CanonicalId(TEXT("pug123")).IsEmpty());
	TestTrue(TEXT("Non-hex IDs are rejected"),
		FBotArrivalLedger::CanonicalId(TEXT("zbcdef0123456789abcdef0123456789")).IsEmpty());
	TestTrue(TEXT("Blank IDs are rejected"), FBotArrivalLedger::CanonicalId(TEXT("")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCBotArrivalFirstSeenTest,
	"NetcodePlus.BotArrival.FirstSeen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCBotArrivalFirstSeenTest::RunTest(const FString& Parameters)
{
	FBotArrivalLedger Ledger;
	const FString First(TEXT("abcdef0123456789abcdef0123456789"));
	Ledger.RecordJoin(First.ToUpper(), 0.0);
	Ledger.RecordJoin(First, 361.0);
	TestEqual(TEXT("Reconnection preserves the initial visit across the six-minute boundary"), Ledger.Joined.FindChecked(First), 0.0);
	TestEqual(TEXT("Reconnection is not another account"), Ledger.Joined.Num(), 1);
	TestTrue(TEXT("A valid cumulative ledger is complete"), Ledger.bComplete);
	// A delayed UniqueId resolves with the login-hook time, not the polling time.
	const FString Second(TEXT("1234567890abcdef1234567890abcdef"));
	Ledger.RecordJoin(Second, 359.75);
	TestEqual(TEXT("Fractional first-seen time is preserved"), Ledger.Joined.FindChecked(Second), 359.75);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCBotArrivalBoundsTest,
	"NetcodePlus.BotArrival.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCBotArrivalBoundsTest::RunTest(const FString& Parameters)
{
	FBotArrivalLedger Ledger;
	for (int32 Index = 1; Index <= FBotArrivalLedger::MaxPlayers; ++Index)
	{
		Ledger.RecordJoin(FString::Printf(TEXT("%032x"), Index), double(Index));
	}
	TestTrue(TEXT("A full ledger is still complete"), Ledger.bComplete);
	Ledger.RecordJoin(TEXT("00000000000000000000000000000100"), 300.0);
	TestFalse(TEXT("Overflow permanently makes absence unknown"), Ledger.bComplete);
	TestEqual(TEXT("Overflow stays bounded"), Ledger.Joined.Num(), FBotArrivalLedger::MaxPlayers);
	Ledger.RecordJoin(TEXT("00000000000000000000000000000001"), 400.0);
	TestFalse(TEXT("A later valid join cannot clear lost evidence"), Ledger.bComplete);
	TestEqual(TEXT("Earlier evidence survives overflow"), Ledger.Joined.FindChecked(TEXT("00000000000000000000000000000001")), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCBotArrivalInvalidEvidenceTest,
	"NetcodePlus.BotArrival.InvalidEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCBotArrivalInvalidEvidenceTest::RunTest(const FString& Parameters)
{
	const FString Id(TEXT("abcdef0123456789abcdef0123456789"));
	FBotArrivalLedger Blank;
	Blank.RecordJoin(TEXT(""), 10.0);
	TestFalse(TEXT("Unresolved evidence cannot assert complete absence"), Blank.bComplete);
	FBotArrivalLedger Negative;
	Negative.RecordJoin(Id, -1.0);
	TestFalse(TEXT("Negative times are rejected"), Negative.bComplete);
	FBotArrivalLedger NonFinite;
	NonFinite.RecordJoin(Id, std::numeric_limits<double>::infinity());
	TestFalse(TEXT("Nonfinite times are rejected"), NonFinite.bComplete);
	TestEqual(TEXT("Invalid evidence adds no player"), NonFinite.Joined.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCBotArrivalBootstrapTest,
	"NetcodePlus.BotArrival.Bootstrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCBotArrivalBootstrapTest::RunTest(const FString& Parameters)
{
	FBotArrivalLedger Ledger;
	TestFalse(TEXT("No GameState cannot qualify an initial heartbeat"), Ledger.QualifyState(NAME_None));
	TestFalse(TEXT("EnteringMap cannot start the trial clock"), Ledger.QualifyState(FName(TEXT("EnteringMap"))));
	TestFalse(TEXT("An unobserved countdown cannot substitute for readiness"), Ledger.QualifyState(FName(TEXT("CountdownToBegin"))));
	TestFalse(TEXT("Joining observation in a live match cannot substitute for readiness"), Ledger.QualifyState(FName(TEXT("InProgress"))));
	const FString EarlyId(TEXT("abcdef0123456789abcdef0123456789"));
	Ledger.RecordJoin(EarlyId, 0.5);
	TestFalse(TEXT("A login before warmup does not start the trial clock"), Ledger.bWarmupSeen);
	TestTrue(TEXT("Actual WaitingToStart qualifies the first heartbeat"), Ledger.QualifyState(FName(TEXT("WaitingToStart"))));
	TestEqual(TEXT("Pre-warmup arrivals survive readiness qualification"), Ledger.Joined.FindChecked(EarlyId), 0.5);
	TestTrue(TEXT("Countdown continues an observed trial"), Ledger.QualifyState(FName(TEXT("CountdownToBegin"))));
	TestTrue(TEXT("Live state continues an observed trial"), Ledger.QualifyState(FName(TEXT("InProgress"))));
	TestTrue(TEXT("Terminal state can close an observed trial"), Ledger.QualifyState(FName(TEXT("WaitingPostMatch"))));
	FBotArrivalLedger NoWarmup;
	TestFalse(TEXT("A terminal event cannot start an unobserved trial"), NoWarmup.QualifyState(FName(TEXT("WaitingPostMatch"))));
	return true;
}

#endif
