#include "NCToTCollector.h"
#include "UTPlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCToT, Log, All);

// Tunables (compile-time; mirror the originals from MutBotEvents).
static const int32 kLowDwellMs        = 16;
static const int32 kMinSamplesForFlag = 12;

// Per-server match counter, bumped on Reset() so CSV filenames are unique across rematches.
static int32 GToTMatchCounter = 0;

// Dwell histogram buckets (ms): [0,8) [8,16) [16,32) [32,64) [64,128) [128,256)
// [256,512) [512,inf). Log-ish spacing so the low end (flick / trigger-bot band) is
// fine-grained and the long tracking tail collapses into a few buckets.
static int32 ToTHistBucket(int32 Ms)
{
	if (Ms < 8)   return 0;
	if (Ms < 16)  return 1;
	if (Ms < 32)  return 2;
	if (Ms < 64)  return 3;
	if (Ms < 128) return 4;
	if (Ms < 256) return 5;
	if (Ms < 512) return 6;
	return 7;
}

FNCToTCollector& FNCToTCollector::Get()
{
	static FNCToTCollector Instance;
	return Instance;
}

void FNCToTCollector::Reset()
{
	Stats.Reset();
	bReportedSinceReset = false;
	++GToTMatchCounter;
}

void FNCToTCollector::ReportOnce(UWorld* World)
{
	if (bReportedSinceReset) return;
	bReportedSinceReset = true;
	Report(World);
}

void FNCToTCollector::Record(UWorld* World, AUTPlayerState* Shooter, int32 DwellMs, uint8 FrameMs, bool bClaimedHit)
{
	if (Shooter == nullptr) return;

	// Stable identity, most-stable first: StatsID (logged-in account, survives a
	// reconnect) -> session UniqueId (stable even for a guest, immune to a mid-match
	// `setname`) -> name (last resort).
	FString Key;
	if (!Shooter->StatsID.IsEmpty())      Key = TEXT("id:")   + Shooter->StatsID;
	else if (Shooter->UniqueId.IsValid()) Key = TEXT("net:")  + Shooter->UniqueId.ToString();
	else                                  Key = TEXT("name:") + Shooter->PlayerName;

	AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	const float Now = GS ? GS->GetServerWorldTimeSeconds()
	                     : (World ? World->GetTimeSeconds() : 0.0f);

	FStat& S = Stats.FindOrAdd(Key);
	S.PlayerName = Shooter->PlayerName; // keep the latest display name for this identity
	FSample Smp;
	Smp.DwellMs     = FMath::Clamp(DwellMs, 0, 60000);
	Smp.ServerTime  = Now;
	Smp.FrameMs     = FrameMs;
	Smp.bClaimedHit = bClaimedHit;
	S.Samples.Add(Smp);
}

FNCToTCollector::FSummary FNCToTCollector::ComputeSummary(const FStat& S) const
{
	FSummary R;
	FMemory::Memzero(&R, sizeof(R)); // POD-only struct (no FString) — safe to zero
	const int32 N = S.Samples.Num();
	R.N = N;
	if (N == 0) return R;

	TArray<int32> D; D.Reserve(N);
	double Sum = 0.0, FrameSum = 0.0; int32 FrameCnt = 0, LowCnt = 0, FirstFrameCnt = 0;
	for (const FSample& Smp : S.Samples)
	{
		D.Add(Smp.DwellMs);
		Sum += Smp.DwellMs;
		R.Hist[ToTHistBucket(Smp.DwellMs)]++;
		if (Smp.DwellMs <= kLowDwellMs) LowCnt++;
		if (Smp.bClaimedHit) R.HitN++;
		if (Smp.FrameMs > 0) { FrameSum += Smp.FrameMs; FrameCnt++; }
		// "First-frame fire": dwell within ~1 of the PLAYER'S OWN frame time — fps-FAIR.
		const int32 FrameRef = (Smp.FrameMs > 0) ? (int32)Smp.FrameMs : kLowDwellMs;
		if (Smp.DwellMs <= FrameRef) FirstFrameCnt++;
	}
	const double PreciseMean = Sum / N;   // keep full precision for the variance pass
	R.MeanMs        = (float)PreciseMean;
	R.LowDwellPct   = 100.0f * (float)LowCnt / (float)N;
	R.FirstFramePct = 100.0f * (float)FirstFrameCnt / (float)N;
	R.MeanFrameMs   = FrameCnt ? (float)(FrameSum / FrameCnt) : 0.0f;

	double Var = 0.0;
	for (int32 X : D) { const double d = (double)X - PreciseMean; Var += d * d; }
	R.StdDevMs = (N > 1) ? (float)FMath::Sqrt(Var / (N - 1)) : 0.0f;
	R.CV       = (R.MeanMs > KINDA_SMALL_NUMBER) ? (R.StdDevMs / R.MeanMs) : 0.0f;

	if (R.StdDevMs > 0.0f)
	{
		for (int32 X : D)
			if (FMath::Abs((float)X - R.MeanMs) > 3.0f * R.StdDevMs) R.OutlierCount++;
	}

	D.Sort();
	R.MinMs = D[0];
	R.MaxMs = D[N - 1];
	R.P10 = D[FMath::Clamp(FMath::RoundToInt(0.10f * (N - 1)), 0, N - 1)];
	R.P50 = D[FMath::Clamp(FMath::RoundToInt(0.50f * (N - 1)), 0, N - 1)];
	R.P90 = D[FMath::Clamp(FMath::RoundToInt(0.90f * (N - 1)), 0, N - 1)];

	R.bSuspect = false; // peer-relative; decided in Report once the lobby is known
	return R;
}

void FNCToTCollector::Report(UWorld* World) const
{
	if (Stats.Num() == 0) return;

	const int32 LowDwellMs = kLowDwellMs; // local copy (avoid static-const ODR-use in varargs)

	// Pass 1 — summarize every player.
	struct FRow { FString Name; FSummary R; };
	TArray<FRow> Rows;
	for (const TPair<FString, FStat>& Pair : Stats)
	{
		if (Pair.Value.Samples.Num() == 0) continue;
		FRow Row; Row.Name = Pair.Value.PlayerName; Row.R = ComputeSummary(Pair.Value);
		Rows.Add(Row);
	}
	if (Rows.Num() == 0) return;

	// Lobby baseline: a real cheat is the clear high outlier of its OWN lobby (self-
	// calibrates across fps and modes — instagib lobbies run fast across the board).
	TArray<float> FF, CVs;
	for (const FRow& Row : Rows)
		if (Row.R.N >= kMinSamplesForFlag) { FF.Add(Row.R.FirstFramePct); CVs.Add(Row.R.CV); }
	float LobbyMedianFF = 0.0f, LobbyMedianCV = 0.0f;
	if (FF.Num()  > 0) { FF.Sort();  LobbyMedianFF = FF[FF.Num() / 2]; }
	if (CVs.Num() > 0) { CVs.Sort(); LobbyMedianCV = CVs[CVs.Num() / 2]; }

	// Pass 2 — relative flag + emit. Thresholds PROVISIONAL; the strong signal is the
	// SAME player flagging across MANY matches.
	for (FRow& Row : Rows)
	{
		FSummary& R = Row.R;
		const bool bEnough = (R.N >= kMinSamplesForFlag) && (FF.Num() >= 3);

		// Signature A — "fast": elevated fast-hit fraction vs peers (instant trigger).
		const bool bFastOutlier = bEnough && (R.CV < 0.5f) && (R.FirstFramePct >= 12.0f)
			&& (R.FirstFramePct >= 2.0f * FMath::Max(LobbyMedianFF, 1.0f));

		// Signature B — "regular": tight spread + no human tracking tail (compressed
		// p90/p50) clearly below the lobby variance. Catches a delay-randomised bot.
		const float TailRatio = (float)R.P90 / (float)FMath::Max(R.P50, 1);
		const bool bTooRegular = bEnough && (R.CV < 0.45f) && (TailRatio < 2.5f)
			&& (R.CV <= 0.6f * FMath::Max(LobbyMedianCV, KINDA_SMALL_NUMBER));

		R.bSuspect = bFastOutlier || bTooRegular;
		const TCHAR* Reason = bFastOutlier ? TEXT("fast") : (bTooRegular ? TEXT("regular") : TEXT(""));
		const FString Flag = R.bSuspect ? FString::Printf(TEXT("  [REVIEW: %s]"), Reason) : FString();

		const float Fps = (R.MeanFrameMs > 0.0f) ? (1000.0f / R.MeanFrameMs) : 0.0f;

		// Warning verbosity so it survives Shipping (it's a review trail).
		UE_LOG(LogNCToT, Warning,
			TEXT("[ToT] %s: n=%d hits=%d mean=%.0f sd=%.0f cv=%.2f p10=%d p50=%d p90=%d min=%d max=%d low<=%dms=%.0f%% firstframe=%.0f%%(lobby~%.0f%%) fps~%.0f outliers=%d%s"),
			*Row.Name, R.N, R.HitN, R.MeanMs, R.StdDevMs, R.CV, R.P10, R.P50, R.P90, R.MinMs, R.MaxMs,
			LowDwellMs, R.LowDwellPct, R.FirstFramePct, LobbyMedianFF, Fps, R.OutlierCount, *Flag);
	}

	WriteCsv(World);
}

void FNCToTCollector::WriteCsv(UWorld* World) const
{
	// Flatten every player's samples into one match-clock-sorted timeline (the review
	// trail an admin scrubs against a server demo / the replay overlay reads).
	struct FRow { float T; FString Name; int32 Dwell; uint8 Frame; bool bHit; };
	TArray<FRow> Rows;
	for (const TPair<FString, FStat>& Pair : Stats)
		for (const FSample& Smp : Pair.Value.Samples)
		{
			FRow Rw; Rw.T = Smp.ServerTime; Rw.Name = Pair.Value.PlayerName;
			Rw.Dwell = Smp.DwellMs; Rw.Frame = Smp.FrameMs; Rw.bHit = Smp.bClaimedHit;
			Rows.Add(Rw);
		}
	if (Rows.Num() == 0) return;
	Rows.Sort([](const FRow& A, const FRow& B) { return A.T < B.T; });

	FString Csv = TEXT("server_time,player,dwell_ms,frame_ms,hit\n");
	for (const FRow& Rw : Rows)
	{
		const FString Name = Rw.Name.Replace(TEXT("\""), TEXT("'")).Replace(TEXT(","), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
		Csv += FString::Printf(TEXT("%.3f,%s,%d,%d,%d\n"), Rw.T, *Name, Rw.Dwell, (int32)Rw.Frame, Rw.bHit ? 1 : 0);
	}

	const FString MapTag = World ? FPaths::GetBaseFilename(World->URL.Map) : FString(TEXT("match"));
	const FString Path = FPaths::GameSavedDir() / TEXT("Logs")
		/ FString::Printf(TEXT("ToT_%s_%d.csv"), *MapTag, GToTMatchCounter);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (FFileHelper::SaveStringToFile(Csv, *Path))
	{
		UE_LOG(LogNCToT, Warning, TEXT("[ToT] timeline written: %s (%d samples)"), *Path, Rows.Num());
	}
	else
	{
		UE_LOG(LogNCToT, Warning, TEXT("[ToT] FAILED to write timeline: %s"), *Path);
	}
}

// ── On-demand dump: `ncp.ToTDump` (server console / rcon) — print + write the table
//    mid-match without waiting for match end. Fast in-game test loop. ────────────────
static void NCToTDumpCmd(UWorld* World)
{
	FNCToTCollector::Get().Report(World);
}
static FAutoConsoleCommandWithWorld GNCToTDumpCmd(
	TEXT("ncp.ToTDump"),
	TEXT("Dump the current time-on-target table (server log [ToT] lines + Saved/Logs CSV) on demand."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&NCToTDumpCmd));
