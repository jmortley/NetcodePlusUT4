// NCStatsUploader.cpp — STUB implementation.
// Logs the POST sequence we would send to each backend. Future PR will replace
// each UE_LOG with FHttpModule::Get().CreateRequest() + JSON payload.

#include "NCStatsUploader.h"
#include "UnrealTournament.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCStatsUploader, Log, All);

namespace
{
	void LogMatchSummaryHeader(const FNCMatchSummary& S, const TCHAR* Backend)
	{
		UE_LOG(LogNCStatsUploader, Log,
			TEXT("[%s] Match: gamemode=%s map=%s server=%s players=%d red=%d blue=%d redDmg=%d blueDmg=%d"),
			Backend, *S.GameMode, *S.MapName, *S.ServerName,
			S.Players.Num(), S.RedScore, S.BlueScore, S.RedDamage, S.BlueDamage);
	}

	void LogPlayer(const FNCPlayerSummary& P, const TCHAR* Backend)
	{
		UE_LOG(LogNCStatsUploader, Log,
			TEXT("[%s]   Player %s (%s): score=%d K/D=%d/%d ping=%d team=%d dmg=%d ELO %d -> %d"),
			Backend, *P.PlayerName, *P.UniqueId, P.Score, P.Kills, P.Deaths,
			P.Ping, P.Team, P.Damage, P.PreMatchElo, P.PostMatchElo);

		for (const TPair<FName, FIntPoint>& WA : P.WeaponAccuracy)
		{
			const int32 Shots = WA.Value.X;
			const int32 Hits  = WA.Value.Y;
			const float Pct   = (Shots > 0) ? (float(Hits) / float(Shots) * 100.f) : 0.f;
			UE_LOG(LogNCStatsUploader, Log,
				TEXT("[%s]     %s: %d/%d (%.1f%%)"),
				Backend, *WA.Key.ToString(), Hits, Shots, Pct);
		}
	}
}

void FNCStatsUploader::PostToUT4Stats(UWorld* World, const FNCMatchSummary& Summary)
{
	const TCHAR* Backend = TEXT("NCStatsUploader/ut4stats");
	LogMatchSummaryHeader(Summary, Backend);

	// Per api_endpoints.md (43 days old at plan time — verify before HTTP code).
	UE_LOG(LogNCStatsUploader, Log,
		TEXT("[%s] TODO: would POST /json_entry insert_match (gamemode=%s, gamemap=%s, servername=%s)"),
		Backend, *Summary.GameMode, *Summary.MapName, *Summary.ServerName);

	for (const FNCPlayerSummary& P : Summary.Players)
	{
		UE_LOG(LogNCStatsUploader, Log,
			TEXT("[%s] TODO: would POST /json_entry insert_player (playerid=%s, playername=%s)"),
			Backend, *P.UniqueId, *P.PlayerName);

		UE_LOG(LogNCStatsUploader, Log,
			TEXT("[%s] TODO: would POST /json_entry insert_matchstats ")
			TEXT("(playerid=%s, score=%d, kills=%d, deaths=%d, ping=%d, team=%d, damage=%d)"),
			Backend, *P.UniqueId, P.Score, P.Kills, P.Deaths, P.Ping, P.Team, P.Damage);

		UE_LOG(LogNCStatsUploader, Log,
			TEXT("[%s] TODO: would POST /weapon_entry insert_accuracy (playerid=%s, weapons=%d)"),
			Backend, *P.UniqueId, P.WeaponAccuracy.Num());

		LogPlayer(P, Backend);
	}

	UE_LOG(LogNCStatsUploader, Log,
		TEXT("[%s] TODO: would POST /json_entry update_match ")
		TEXT("(redkills=%d, bluekills=%d, redscore=%d, bluescore=%d, reddamage=%d, bluedamage=%d, gameoptions=%s, replayid=%s)"),
		Backend, Summary.RedScore, Summary.BlueScore, Summary.RedScore, Summary.BlueScore,
		Summary.RedDamage, Summary.BlueDamage, *Summary.GameOptions, *Summary.ReplayId);
}

void FNCStatsUploader::PostToStatSQL(UWorld* World, const FNCMatchSummary& Summary)
{
	const TCHAR* Backend = TEXT("NCStatsUploader/StatSQL");
	LogMatchSummaryHeader(Summary, Backend);

	UE_LOG(LogNCStatsUploader, Log,
		TEXT("[%s] TODO: would POST match payload to StatSQL Django backend"), Backend);

	for (const FNCPlayerSummary& P : Summary.Players)
	{
		LogPlayer(P, Backend);
	}
}
