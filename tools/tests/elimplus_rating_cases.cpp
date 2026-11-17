// Drives the real FElimPlusRatingSystem (ElimPlusRatingSystem.cpp compiled against elimplus_rating_adapter.h) from
// commands on stdin, one per line, fields separated by 0x1f. It calls the public API in the order AElimPlusGame
// does and reports what comes out; it holds no rating logic of its own. test_elimplus_rating_flow.py is the other
// end of the pipe.
//
//   new                         a map load: a fresh rating system, as the game mode makes one per map
//   cvar NAME VALUE             set a console variable
//   now UNIX                    set the clock
//   init                        InitDatabase
//   load ID [NAME]              PostLogin: LoadPlayerFromDB then CaptureLateJoinBaseline. The game mode runs both
//                               for every login, a reconnect in the middle of a match included.
//   snapshot                    HandleMatchHasStarted: SnapshotMatchStart
//   round DRAW WINNERS LOSERS   one round; each side is "id:kills:deaths:damage:team;..." (an id starting with
//                               BOT: is a bot). RecordRoundPPR for each human, then ProcessRound. A side may be
//                               empty: the game mode still counts the round for the players who were in it, and
//                               the rating system rejects it. In a draw the game mode passes team 0 as WINNERS.
//   replicator 0|1              whether the game mode has its stats actor. Without one (the spawn failed) it skips
//                               the RecordRoundPPR loop altogether and hands FlushAtMatchEnd a null pointer.
//   flush                       HandleMatchHasEnded: FlushAtMatchEnd
//   payload WINNER RED BLUE ROSTER   BuildResultPayload; ROSTER is "id:name:team:kills:deaths:damage;..." and
//                               holds whoever is still connected at match end (a leaver is not in it)
//   elo ID / rank ID            GetCachedElo / GetPlayerGlobalRank
//
// At login and at match start the game mode also pushes each cached rating and rank to its stats actor. Those
// calls only read, so they are not replayed here; the elo and rank commands ask the same questions.
//
// Every command ends with a DONE line. In between come SQL (answered by the test), LOG, ELO, RANK and OUT lines.
#include "CoreMinimal.h"
#include "ElimPlusRatingSystem.h"

namespace
{
	std::vector<std::string> Split(const std::string& In, char Separator)
	{
		std::vector<std::string> Parts;
		size_t Start = 0;
		for (;;)
		{
			const size_t End = In.find(Separator, Start);
			Parts.push_back(In.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
			if (End == std::string::npos)
			{
				return Parts;
			}
			Start = End + 1;
		}
	}

	TArray<FElimPlusPlayerRoundPerf> ParseSide(const std::string& In)
	{
		TArray<FElimPlusPlayerRoundPerf> Side;
		if (In.empty())
		{
			return Side;
		}
		for (const std::string& Entry : Split(In, ';'))
		{
			const std::vector<std::string> Parts = Split(Entry, ':');
			FElimPlusPlayerRoundPerf Perf;
			// A bot's key is "BOT:<name>", so the id may itself contain the separator: the last four are numbers.
			const size_t Numbers = Parts.size() - 4;
			std::string Id = Parts[0];
			for (size_t i = 1; i < Numbers; ++i)
			{
				Id += ":" + Parts[i];
			}
			Perf.UniqueId = FString(Id);
			Perf.Kills = std::atoi(Parts[Numbers].c_str());
			Perf.Deaths = std::atoi(Parts[Numbers + 1].c_str());
			Perf.Damage = static_cast<float>(std::atof(Parts[Numbers + 2].c_str()));
			Perf.TeamIndex = std::atoi(Parts[Numbers + 3].c_str());
			Side.Add(Perf);
		}
		return Side;
	}
}

int main()
{
	UUTGameInstance GameInstance;
	UWorld World;
	World.GameInstance = &GameInstance;
	World.MapName = FString("DM-StandIn");
	AElimPlusStatsReplicator Replicator;
	bool bHasReplicator = true;
	TUniquePtr<FElimPlusRatingSystem> System;

	std::string Line;
	while (std::getline(std::cin, Line))
	{
		const std::vector<std::string> Fields = Split(Line, kStandInSeparator);
		const std::string& Command = Fields[0];
		if (Command == "new")
		{
			System = MakeUnique<FElimPlusRatingSystem>();
		}
		else if (Command == "cvar")
		{
			void* Found = StandInConsoleVariables().count(Fields[1]) ? StandInConsoleVariables()[Fields[1]] : nullptr;
			if (!Found)
			{
				StandInEmit("OUT", {"unknown console variable"});
			}
			else
			{
				static_cast<TAutoConsoleVariable<int32>*>(Found)->Value = std::atoi(Fields[2].c_str());
			}
		}
		else if (Command == "now")
		{
			StandInUnixNow() = std::atoll(Fields[1].c_str());
		}
		else if (Command == "init")
		{
			StandInEmit("OUT", {System->InitDatabase(&World) ? "1" : "0"});
		}
		else if (Command == "load")
		{
			const FString Id(Fields[1]);
			System->LoadPlayerFromDB(&World, Id, Fields.size() > 2 ? FString(Fields[2]) : FString());
			System->CaptureLateJoinBaseline(Id);
		}
		else if (Command == "snapshot")
		{
			System->SnapshotMatchStart();
		}
		else if (Command == "round")
		{
			FElimPlusRoundResult Result;
			Result.bIsDraw = Fields[1] == "1";
			Result.WinnerTeam = ParseSide(Fields[2]);
			Result.LoserTeam = ParseSide(Fields[3]);
			// AElimPlusGame runs this loop only "if (GS && StatsReplicator)".
			for (const TArray<FElimPlusPlayerRoundPerf>* Side : {&Result.WinnerTeam, &Result.LoserTeam})
			{
				for (const FElimPlusPlayerRoundPerf& Perf : *Side)
				{
					if (bHasReplicator && !Perf.UniqueId.StartsWith(FString("BOT:")))
					{
						System->RecordRoundPPR(Perf.UniqueId, static_cast<float>(Perf.Kills) + Perf.Damage * 0.01f,
							Perf.Damage, Perf.UniqueId);
					}
				}
			}
			System->ProcessRound(Result);
		}
		else if (Command == "replicator")
		{
			bHasReplicator = Fields[1] == "1";
		}
		else if (Command == "flush")
		{
			System->FlushAtMatchEnd(&World, bHasReplicator ? &Replicator : nullptr);
		}
		else if (Command == "payload")
		{
			FNCElimPlusMatchInput In;
			In.WinnerTeamIndex = std::atoi(Fields[1].c_str());
			In.RedScore = std::atoi(Fields[2].c_str());
			In.BlueScore = std::atoi(Fields[3].c_str());
			if (Fields.size() > 4 && !Fields[4].empty())
			{
				for (const std::string& Entry : Split(Fields[4], ';'))
				{
					const std::vector<std::string> Parts = Split(Entry, ':');
					FNCElimPlusPlayerInput Player;
					Player.UniqueId = FString(Parts[0]);
					Player.PlayerName = FString(Parts[1]);
					Player.TeamIndex = std::atoi(Parts[2].c_str());
					Player.Kills = std::atoi(Parts[3].c_str());
					Player.Deaths = std::atoi(Parts[4].c_str());
					Player.Damage = std::atoi(Parts[5].c_str());
					In.Players.Add(Player);
				}
			}
			StandInEmit("OUT", {System->BuildResultPayload(&World, In)});
		}
		else if (Command == "elo")
		{
			StandInEmit("OUT", {std::to_string(System->GetCachedElo(FString(Fields[1])))});
		}
		else if (Command == "rank")
		{
			StandInEmit("OUT", {std::to_string(System->GetPlayerGlobalRank(&World, FString(Fields[1])))});
		}
		else
		{
			StandInEmit("OUT", {"unknown command"});
		}
		StandInEmit("DONE", {});
	}
	return 0;
}
