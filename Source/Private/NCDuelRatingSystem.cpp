// NCDuelRatingSystem.cpp — Mods.db persistence + 1v1 Glicko2 ProcessMatch glue.
// Tron's TeamGlicko2 headers (and their <vector>/<cmath> baggage) stay confined
// to this translation unit thanks to the Pimpl in the header.

#include "NCDuelRatingSystem.h"
#include "UnrealTournament.h"
#include "UTGameInstance.h"            // FDatabaseRow + ExecDatabaseCommand
#include "UTGameState.h"               // ServerName for BuildResultPayload
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Vendored TeamGlicko2 — included only here, never via the .h.
#include "TeamGlickoRating.h"
#include "TeamGlicko2System.h"
#include "TeamGlicko2Config.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCDuelRating, Log, All);

// =============================================================================
// Pimpl: cache. TeamGlicko2 types only visible here.
// =============================================================================
struct FNCDuelRatingSystemImpl
{
	/** UniqueId -> mutable PlayerRating. Updated by ProcessMatchResult. */
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;

	/** Win/loss/draw counters per cached player — DB-persisted. */
	TMap<FString, FNCDuelPlayerEntry> StatsCache;

	/** UniqueId -> rating-rounded-int captured by SnapshotMatchStart. Used by
	 *  GetPreMatchElo to report (NewElo - StartElo) deltas in the uploader payload. */
	TMap<FString, int32> RatingAtMatchStart;
};

// Unity-build aware: anonymous-namespace symbols can collide across .cpp files
// bundled into one TU. Use file-scoped prefixed names so these stay distinct
// from ElimPlusRatingSystem.cpp's and NCShaftArenaRatingSystem.cpp's identical
// helpers.
namespace
{
	FString NCDuel_SqlEscape(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	bool NCDuel_ExecSql(UWorld* World, const FString& Sql, TArray<FDatabaseRow>& OutRows)
	{
		if (!World) return false;
		UUTGameInstance* GI = Cast<UUTGameInstance>(World->GetGameInstance());
		if (!GI) return false;
		return GI->ExecDatabaseCommand(Sql, OutRows);
	}

	bool NCDuel_ExecSqlNoRows(UWorld* World, const FString& Sql)
	{
		TArray<FDatabaseRow> Discard;
		return NCDuel_ExecSql(World, Sql, Discard);
	}
}

// =============================================================================
// FNCDuelRatingSystem
// =============================================================================

FNCDuelRatingSystem::FNCDuelRatingSystem()
	: Impl(MakeUnique<FNCDuelRatingSystemImpl>())
{}

FNCDuelRatingSystem::~FNCDuelRatingSystem() = default;

bool FNCDuelRatingSystem::InitDatabase(UWorld* World)
{
	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingDuel (")
		TEXT("  UniqueId      TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating        REAL    NOT NULL DEFAULT 1400.0,")
		TEXT("  RD            REAL    NOT NULL DEFAULT 200.0,")
		TEXT("  Sigma         REAL    NOT NULL DEFAULT 0.06,")
		TEXT("  GamesPlayed   INTEGER NOT NULL DEFAULT 0,")
		TEXT("  Wins          INTEGER NOT NULL DEFAULT 0,")
		TEXT("  Losses        INTEGER NOT NULL DEFAULT 0,")
		TEXT("  Draws         INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc   INTEGER NOT NULL DEFAULT 0,")
		TEXT("  SchemaVersion INTEGER NOT NULL DEFAULT 1")
		TEXT(");");

	const bool bOk = NCDuel_ExecSqlNoRows(World, Sql);
	UE_LOG(LogNCDuelRating, Log, TEXT("InitDatabase: NCRatingDuel %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));
	return bOk;
}

void FNCDuelRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = NCDuel_SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, GamesPlayed, Wins, Losses, Draws FROM NCRatingDuel WHERE UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = NCDuel_ExecSql(World, Sql, Rows);

	FNCDuelPlayerEntry Entry;
	Entry.UniqueId = UniqueId;

	if (bOk && Rows.Num() > 0 && Rows[0].Text.Num() >= 7)
	{
		Entry.Rating      = FCString::Atod(*Rows[0].Text[0]);
		Entry.RD          = FCString::Atod(*Rows[0].Text[1]);
		Entry.Sigma       = FCString::Atod(*Rows[0].Text[2]);
		Entry.GamesPlayed = FCString::Atoi(*Rows[0].Text[3]);
		Entry.Wins        = FCString::Atoi(*Rows[0].Text[4]);
		Entry.Losses      = FCString::Atoi(*Rows[0].Text[5]);
		Entry.Draws       = FCString::Atoi(*Rows[0].Text[6]);

		TeamGlicko2::PlayerRating PR(Entry.Rating, Entry.RD, Entry.Sigma);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->StatsCache.Add(UniqueId, Entry);

		UE_LOG(LogNCDuelRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f W/L/D=%d/%d/%d"),
			*UniqueId, Entry.Rating, Entry.RD, Entry.Wins, Entry.Losses, Entry.Draws);
	}
	else
	{
		Entry.Rating = TeamGlicko2::kDefaultRating;
		Entry.RD     = TeamGlicko2::kDefaultRD;
		Entry.Sigma  = TeamGlicko2::kDefaultVolatility;

		TeamGlicko2::PlayerRating PR(Entry.Rating, Entry.RD, Entry.Sigma);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->StatsCache.Add(UniqueId, Entry);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingDuel ")
			TEXT("(UniqueId, Rating, RD, Sigma, GamesPlayed, Wins, Losses, Draws, LastSeenUtc) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0, 0, 0, 0, %lld);"),
			*Esc,
			Entry.Rating, Entry.RD, Entry.Sigma,
			static_cast<long long>(NowUtc));
		NCDuel_ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogNCDuelRating, Log, TEXT("New duel player %s — defaults cached + INSERT OR IGNORE"), *UniqueId);
	}
}

int32 FNCDuelRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FNCDuelRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		Impl->RatingAtMatchStart.Add(Pair.Key, FMath::RoundToInt(Pair.Value.GetRating()));
	}
}

int32 FNCDuelRatingSystem::GetPreMatchElo(const FString& UniqueId) const
{
	if (const int32* Snapshot = Impl->RatingAtMatchStart.Find(UniqueId))
	{
		return *Snapshot;
	}
	return GetCachedElo(UniqueId);
}

void FNCDuelRatingSystem::ProcessMatchResult(const FString& WinnerId, const FString& LoserId, bool bDraw)
{
	using namespace TeamGlicko2;

	if (WinnerId.IsEmpty() || LoserId.IsEmpty() || WinnerId == LoserId) return;

	PlayerRating* WinnerPR = Impl->RatingCache.Find(WinnerId);
	PlayerRating* LoserPR  = Impl->RatingCache.Find(LoserId);
	if (!WinnerPR || !LoserPR)
	{
		UE_LOG(LogNCDuelRating, Warning, TEXT("ProcessMatchResult: missing cache entry W=%s L=%s — skipping"),
			*WinnerId, *LoserId);
		return;
	}

	// Build a 1v1 MatchResult. Each team has exactly one MatchPlayer; performance
	// score is 0 for both since 1v1 has no relative-to-teammate component.
	MatchResult Match;
	Match.teamA.push_back(MatchPlayer(*WinnerPR, 0.0));
	Match.teamB.push_back(MatchPlayer(*LoserPR,  0.0));
	Match.scoreA = bDraw ? kDrawScore : kWinScore;
	Match.scoreB = bDraw ? kDrawScore : kLossScore;

	TeamGlicko2System::ProcessMatch(Match);

	// Write back updated ratings.
	*WinnerPR = Match.teamA[0].rating;
	*LoserPR  = Match.teamB[0].rating;

	// Update W/L/D counters.
	if (FNCDuelPlayerEntry* WinnerStats = Impl->StatsCache.Find(WinnerId))
	{
		++WinnerStats->GamesPlayed;
		if (bDraw) ++WinnerStats->Draws; else ++WinnerStats->Wins;
	}
	if (FNCDuelPlayerEntry* LoserStats = Impl->StatsCache.Find(LoserId))
	{
		++LoserStats->GamesPlayed;
		if (bDraw) ++LoserStats->Draws; else ++LoserStats->Losses;
	}

	UE_LOG(LogNCDuelRating, Log, TEXT("Match: %s (%.0f -> %.0f) %s %s (%.0f -> %.0f)"),
		*WinnerId,
		Impl->RatingAtMatchStart.FindRef(WinnerId) > 0 ? float(Impl->RatingAtMatchStart.FindRef(WinnerId)) : float(WinnerPR->GetRating()),
		WinnerPR->GetRating(),
		bDraw ? TEXT("drew") : TEXT("beat"),
		*LoserId,
		Impl->RatingAtMatchStart.FindRef(LoserId) > 0 ? float(Impl->RatingAtMatchStart.FindRef(LoserId)) : float(LoserPR->GetRating()),
		LoserPR->GetRating());
}

void FNCDuelRatingSystem::Flush(UWorld* World)
{
	if (!World) return;
	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();

	int32 PersistedCount = 0;
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		const TeamGlicko2::PlayerRating& PR = Pair.Value;
		const FNCDuelPlayerEntry* Entry = Impl->StatsCache.Find(UniqueId);
		if (!Entry) continue;

		const FString Esc = NCDuel_SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingDuel ")
			TEXT("(UniqueId, Rating, RD, Sigma, GamesPlayed, Wins, Losses, Draws, LastSeenUtc, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %d, %d, %d, %d, %lld, 1);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			Entry->GamesPlayed, Entry->Wins, Entry->Losses, Entry->Draws,
			static_cast<long long>(NowUtc));
		NCDuel_ExecSqlNoRows(World, Sql);
		++PersistedCount;
	}
	UE_LOG(LogNCDuelRating, Log, TEXT("Flush: persisted %d duel ratings"), PersistedCount);
}

void FNCDuelRatingSystem::Forget(const FString& UniqueId)
{
	Impl->RatingCache.Remove(UniqueId);
	Impl->StatsCache.Remove(UniqueId);
	Impl->RatingAtMatchStart.Remove(UniqueId);
}

FString FNCDuelRatingSystem::BuildResultPayload(UWorld* World, const FNCDuelMatchInput& In) const
{
	using namespace TeamGlicko2;

	const PlayerRating* WinnerPR = Impl->RatingCache.Find(In.WinnerId);
	const PlayerRating* LoserPR  = Impl->RatingCache.Find(In.LoserId);
	if (!WinnerPR || !LoserPR)
	{
		UE_LOG(LogNCDuelRating, Warning,
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
	Writer->WriteValue(TEXT("mode"),          FString(TEXT("Duel")));
	Writer->WriteValue(TEXT("server"),        ServerName);
	Writer->WriteValue(TEXT("map"),           MapName);
	Writer->WriteValue(TEXT("played_at_utc"), PlayedAtUtc);
	Writer->WriteArrayStart(TEXT("players"));

	auto WritePlayer = [&Writer](const FString& Id, const FString& Name, const TCHAR* Result,
	                              int32 Score, int32 PreElo, const PlayerRating& PR)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),     Id);
		Writer->WriteValue(TEXT("name"),   Name);
		Writer->WriteValue(TEXT("result"), FString(Result));
		Writer->WriteValue(TEXT("score"),  Score);
		Writer->WriteValue(TEXT("pre"),    static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),   PR.GetRating());
		Writer->WriteValue(TEXT("rd"),     PR.GetRD());
		Writer->WriteValue(TEXT("sigma"),  PR.GetSigma());
		Writer->WriteObjectEnd();
	};

	const TCHAR* WinResult  = In.bDraw ? TEXT("draw") : TEXT("win");
	const TCHAR* LossResult = In.bDraw ? TEXT("draw") : TEXT("loss");

	WritePlayer(In.WinnerId, In.WinnerName, WinResult,  In.WinnerScore, WinnerPre, *WinnerPR);
	WritePlayer(In.LoserId,  In.LoserName,  LossResult, In.LoserScore,  LoserPre,  *LoserPR);

	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	return Out;
}
