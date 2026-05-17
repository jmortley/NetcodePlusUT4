// NCShaftArenaRatingSystem.cpp — Mods.db persistence + 1v1 Glicko2 + awards.

#include "NCShaftArenaRatingSystem.h"
#include "UnrealTournament.h"
#include "UTGameInstance.h"
#include "UTGameState.h"               // ServerName for BuildResultPayload
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "TeamGlickoRating.h"
#include "TeamGlicko2System.h"
#include "TeamGlicko2Config.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCShaftArenaRating, Log, All);

struct FNCShaftArenaRatingSystemImpl
{
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;
	TMap<FString, FNCShaftArenaPlayerEntry>  EntryCache;
	TMap<FString, int32>                     RatingAtMatchStart;
};

// Unique-name helpers — see NCDuelRatingSystem.cpp banner for unity-build collision rationale.
namespace
{
	FString NCShaft_SqlEscape(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	bool NCShaft_ExecSql(UWorld* World, const FString& Sql, TArray<FDatabaseRow>& OutRows)
	{
		if (!World) return false;
		UUTGameInstance* GI = Cast<UUTGameInstance>(World->GetGameInstance());
		if (!GI) return false;
		return GI->ExecDatabaseCommand(Sql, OutRows);
	}

	bool NCShaft_ExecSqlNoRows(UWorld* World, const FString& Sql)
	{
		TArray<FDatabaseRow> Discard;
		return NCShaft_ExecSql(World, Sql, Discard);
	}
}

FNCShaftArenaRatingSystem::FNCShaftArenaRatingSystem()
	: Impl(MakeUnique<FNCShaftArenaRatingSystemImpl>())
{}

FNCShaftArenaRatingSystem::~FNCShaftArenaRatingSystem() = default;

bool FNCShaftArenaRatingSystem::InitDatabase(UWorld* World)
{
	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingShaftArena (")
		TEXT("  UniqueId            TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating              REAL    NOT NULL DEFAULT 1400.0,")
		TEXT("  RD                  REAL    NOT NULL DEFAULT 200.0,")
		TEXT("  Sigma               REAL    NOT NULL DEFAULT 0.03,")
		TEXT("  BestSessionAccuracy REAL    NOT NULL DEFAULT 0.0,")
		TEXT("  BestStreak          INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalWins           INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalLosses         INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalKills          INTEGER NOT NULL DEFAULT 0,")
		TEXT("  GamesPlayed         INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc         INTEGER NOT NULL DEFAULT 0,")
		TEXT("  SchemaVersion       INTEGER NOT NULL DEFAULT 1")
		TEXT(");");

	const bool bOk = NCShaft_ExecSqlNoRows(World, Sql);
	UE_LOG(LogNCShaftArenaRating, Log, TEXT("InitDatabase: NCRatingShaftArena %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));
	return bOk;
}

void FNCShaftArenaRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = NCShaft_SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, BestSessionAccuracy, BestStreak, ")
		TEXT("TotalWins, TotalLosses, TotalKills, GamesPlayed ")
		TEXT("FROM NCRatingShaftArena WHERE UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = NCShaft_ExecSql(World, Sql, Rows);

	FNCShaftArenaPlayerEntry Entry;
	Entry.UniqueId = UniqueId;

	if (bOk && Rows.Num() > 0 && Rows[0].Text.Num() >= 9)
	{
		Entry.Rating              = FCString::Atod(*Rows[0].Text[0]);
		Entry.RD                  = FCString::Atod(*Rows[0].Text[1]);
		Entry.Sigma               = FCString::Atod(*Rows[0].Text[2]);
		Entry.BestSessionAccuracy = FCString::Atof(*Rows[0].Text[3]);
		Entry.BestStreak          = FCString::Atoi(*Rows[0].Text[4]);
		Entry.TotalWins           = FCString::Atoi(*Rows[0].Text[5]);
		Entry.TotalLosses         = FCString::Atoi(*Rows[0].Text[6]);
		Entry.TotalKills          = FCString::Atoi(*Rows[0].Text[7]);
		Entry.GamesPlayed         = FCString::Atoi(*Rows[0].Text[8]);

		TeamGlicko2::PlayerRating PR(Entry.Rating, Entry.RD, Entry.Sigma);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->EntryCache.Add(UniqueId, Entry);

		UE_LOG(LogNCShaftArenaRating, Log,
			TEXT("Loaded %s: Rating=%.1f BestAcc=%.1f%% BestStreak=%d W/L=%d/%d Kills=%d"),
			*UniqueId, Entry.Rating, Entry.BestSessionAccuracy, Entry.BestStreak,
			Entry.TotalWins, Entry.TotalLosses, Entry.TotalKills);
	}
	else
	{
		Entry.Rating = TeamGlicko2::kDefaultRating;
		Entry.RD     = TeamGlicko2::kDefaultRD;
		Entry.Sigma  = TeamGlicko2::kDefaultVolatility;

		TeamGlicko2::PlayerRating PR(Entry.Rating, Entry.RD, Entry.Sigma);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->EntryCache.Add(UniqueId, Entry);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingShaftArena ")
			TEXT("(UniqueId, Rating, RD, Sigma, LastSeenUtc) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %lld);"),
			*Esc, Entry.Rating, Entry.RD, Entry.Sigma,
			static_cast<long long>(NowUtc));
		NCShaft_ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogNCShaftArenaRating, Log, TEXT("New shaft player %s — defaults cached + INSERT OR IGNORE"), *UniqueId);
	}
}

int32 FNCShaftArenaRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FNCShaftArenaRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		Impl->RatingAtMatchStart.Add(Pair.Key, FMath::RoundToInt(Pair.Value.GetRating()));
	}
}

int32 FNCShaftArenaRatingSystem::GetPreMatchElo(const FString& UniqueId) const
{
	if (const int32* Snapshot = Impl->RatingAtMatchStart.Find(UniqueId))
	{
		return *Snapshot;
	}
	return GetCachedElo(UniqueId);
}

const FNCShaftArenaPlayerEntry* FNCShaftArenaRatingSystem::GetEntry(const FString& UniqueId) const
{
	return Impl->EntryCache.Find(UniqueId);
}

void FNCShaftArenaRatingSystem::RecordMatchResult(
	const FString& WinnerId, const FString& LoserId,
	int32 WinnerKills, int32 LoserKills,
	float WinnerAccuracy, float LoserAccuracy,
	int32 WinnerBestStreak, int32 LoserBestStreak)
{
	using namespace TeamGlicko2;

	if (WinnerId.IsEmpty() || LoserId.IsEmpty() || WinnerId == LoserId) return;

	PlayerRating* WinnerPR = Impl->RatingCache.Find(WinnerId);
	PlayerRating* LoserPR  = Impl->RatingCache.Find(LoserId);
	if (!WinnerPR || !LoserPR)
	{
		UE_LOG(LogNCShaftArenaRating, Warning,
			TEXT("RecordMatchResult: missing cache entry W=%s L=%s — skipping"),
			*WinnerId, *LoserId);
		return;
	}

	MatchResult Match;
	Match.teamA.push_back(MatchPlayer(*WinnerPR, 0.0));
	Match.teamB.push_back(MatchPlayer(*LoserPR,  0.0));
	Match.scoreA = kWinScore;
	Match.scoreB = kLossScore;
	TeamGlicko2System::ProcessMatch(Match);

	*WinnerPR = Match.teamA[0].rating;
	*LoserPR  = Match.teamB[0].rating;

	if (FNCShaftArenaPlayerEntry* W = Impl->EntryCache.Find(WinnerId))
	{
		++W->GamesPlayed;
		++W->TotalWins;
		W->TotalKills += WinnerKills;
		W->BestStreak  = FMath::Max(W->BestStreak, WinnerBestStreak);
		W->BestSessionAccuracy = FMath::Max(W->BestSessionAccuracy, WinnerAccuracy);
	}
	if (FNCShaftArenaPlayerEntry* L = Impl->EntryCache.Find(LoserId))
	{
		++L->GamesPlayed;
		++L->TotalLosses;
		L->TotalKills += LoserKills;
		L->BestStreak  = FMath::Max(L->BestStreak, LoserBestStreak);
		L->BestSessionAccuracy = FMath::Max(L->BestSessionAccuracy, LoserAccuracy);
	}
}

void FNCShaftArenaRatingSystem::Flush(UWorld* World)
{
	if (!World) return;
	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();

	int32 PersistedCount = 0;
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		const TeamGlicko2::PlayerRating& PR = Pair.Value;
		const FNCShaftArenaPlayerEntry* Entry = Impl->EntryCache.Find(UniqueId);
		if (!Entry) continue;

		const FString Esc = NCShaft_SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingShaftArena ")
			TEXT("(UniqueId, Rating, RD, Sigma, BestSessionAccuracy, BestStreak, ")
			TEXT("TotalWins, TotalLosses, TotalKills, GamesPlayed, LastSeenUtc, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.4f, %d, %d, %d, %d, %d, %lld, 1);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			Entry->BestSessionAccuracy, Entry->BestStreak,
			Entry->TotalWins, Entry->TotalLosses, Entry->TotalKills,
			Entry->GamesPlayed,
			static_cast<long long>(NowUtc));
		NCShaft_ExecSqlNoRows(World, Sql);
		++PersistedCount;
	}
	UE_LOG(LogNCShaftArenaRating, Log, TEXT("Flush: persisted %d shaft entries"), PersistedCount);
}

void FNCShaftArenaRatingSystem::Forget(const FString& UniqueId)
{
	Impl->RatingCache.Remove(UniqueId);
	Impl->EntryCache.Remove(UniqueId);
	Impl->RatingAtMatchStart.Remove(UniqueId);
}

FString FNCShaftArenaRatingSystem::BuildResultPayload(UWorld* World, const FNCShaftArenaMatchInput& In) const
{
	using namespace TeamGlicko2;

	const PlayerRating* WinnerPR = Impl->RatingCache.Find(In.WinnerId);
	const PlayerRating* LoserPR  = Impl->RatingCache.Find(In.LoserId);
	if (!WinnerPR || !LoserPR)
	{
		UE_LOG(LogNCShaftArenaRating, Warning,
			TEXT("BuildResultPayload: cache miss for W=%s L=%s — skipping upload"),
			*In.WinnerId, *In.LoserId);
		return FString();
	}

	const int32 WinnerPre = Impl->RatingAtMatchStart.FindRef(In.WinnerId);
	const int32 LoserPre  = Impl->RatingAtMatchStart.FindRef(In.LoserId);

	FString ServerName;
	if (World)
	{
		if (AUTGameState* GS = World->GetGameState<AUTGameState>())
		{
			ServerName = GS->ServerName;
		}
	}
	FString MapName;
	if (World)
	{
		MapName = World->GetMapName();
		MapName.RemoveFromStart(World->StreamingLevelsPrefix);
	}
	const FString PlayedAtUtc = FDateTime::UtcNow().ToIso8601();

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("request"),       FString(TEXT("elo_match_result")));
	Writer->WriteValue(TEXT("mode"),          FString(TEXT("ShaftArena")));
	Writer->WriteValue(TEXT("server"),        ServerName);
	Writer->WriteValue(TEXT("map"),           MapName);
	Writer->WriteValue(TEXT("played_at_utc"), PlayedAtUtc);
	Writer->WriteArrayStart(TEXT("players"));

	auto WritePlayer = [&Writer](const FString& Id, const FString& Name, const TCHAR* Result,
	                              int32 Score, int32 Streak, float AccuracyPct,
	                              int32 PreElo, const PlayerRating& PR)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),           Id);
		Writer->WriteValue(TEXT("name"),         Name);
		Writer->WriteValue(TEXT("result"),       FString(Result));
		Writer->WriteValue(TEXT("score"),        Score);
		Writer->WriteValue(TEXT("best_streak"),  Streak);
		Writer->WriteValue(TEXT("accuracy_pct"), static_cast<double>(AccuracyPct));
		Writer->WriteValue(TEXT("pre"),          static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),         PR.GetRating());
		Writer->WriteValue(TEXT("rd"),           PR.GetRD());
		Writer->WriteValue(TEXT("sigma"),        PR.GetSigma());
		Writer->WriteObjectEnd();
	};

	WritePlayer(In.WinnerId, In.WinnerName, TEXT("win"),
	            In.WinnerScore, In.WinnerStreak, In.WinnerAccuracy, WinnerPre, *WinnerPR);
	WritePlayer(In.LoserId, In.LoserName, TEXT("loss"),
	            In.LoserScore, In.LoserStreak, In.LoserAccuracy, LoserPre, *LoserPR);

	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	return Out;
}

TArray<FNCShaftArenaPlayerEntry> FNCShaftArenaRatingSystem::GetTopPlayers(UWorld* World, int32 N)
{
	TArray<FNCShaftArenaPlayerEntry> Result;
	if (!World || N <= 0) return Result;

	const FString Sql = FString::Printf(
		TEXT("SELECT UniqueId, Rating, RD, Sigma, BestSessionAccuracy, BestStreak, ")
		TEXT("TotalWins, TotalLosses, TotalKills, GamesPlayed ")
		TEXT("FROM NCRatingShaftArena ORDER BY Rating DESC LIMIT %d;"),
		N);

	TArray<FDatabaseRow> Rows;
	if (!NCShaft_ExecSql(World, Sql, Rows)) return Result;

	for (const FDatabaseRow& Row : Rows)
	{
		if (Row.Text.Num() < 10) continue;
		FNCShaftArenaPlayerEntry Entry;
		Entry.UniqueId            = Row.Text[0];
		Entry.Rating              = FCString::Atod(*Row.Text[1]);
		Entry.RD                  = FCString::Atod(*Row.Text[2]);
		Entry.Sigma               = FCString::Atod(*Row.Text[3]);
		Entry.BestSessionAccuracy = FCString::Atof(*Row.Text[4]);
		Entry.BestStreak          = FCString::Atoi(*Row.Text[5]);
		Entry.TotalWins           = FCString::Atoi(*Row.Text[6]);
		Entry.TotalLosses         = FCString::Atoi(*Row.Text[7]);
		Entry.TotalKills          = FCString::Atoi(*Row.Text[8]);
		Entry.GamesPlayed         = FCString::Atoi(*Row.Text[9]);
		Result.Add(Entry);
	}
	return Result;
}
