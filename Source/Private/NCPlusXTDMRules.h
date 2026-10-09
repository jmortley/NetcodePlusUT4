#pragma once
#include "CoreMinimal.h"

namespace NCPlusXTDMRules
{
	inline uint8 PickSmallestTeam(const int32 (&Seats)[4], int32 TeamSize, uint8 Requested)
	{
		uint8 Best = 255;
		int32 Smallest = TeamSize;
		for (uint8 Team = 0; Team < 4; ++Team)
		{
			if (Seats[Team] < Smallest || (Seats[Team] == Smallest && Seats[Team] < TeamSize && Team == Requested))
			{
				Smallest = Seats[Team];
				Best = Team;
			}
		}
		return Best;
	}

	inline bool IsFullRoster(const int32 (&Seats)[4], int32 TeamSize)
	{
		return TeamSize >= 2 && TeamSize <= 4 && Seats[0] == TeamSize && Seats[1] == TeamSize &&
			Seats[2] == TeamSize && Seats[3] == TeamSize;
	}

	/** Seats include reservations; only an actual removable bot can make room on a full team. */
	inline uint8 PickHumanTeam(const int32 (&Seats)[4], const int32 (&RemovableBots)[4], int32 TeamSize, uint8 Requested)
	{
		if (TeamSize < 2 || TeamSize > 4) return 255;
		uint8 Best = 255;
		int32 Smallest = TeamSize + 1;
		for (uint8 Team = 0; Team < 4; ++Team)
		{
			const bool bCanJoin = Seats[Team] >= 0 && (Seats[Team] < TeamSize ||
				(Seats[Team] == TeamSize && RemovableBots[Team] > 0));
			if (bCanJoin && (Seats[Team] < Smallest || (Seats[Team] == Smallest && Team == Requested)))
			{
				Smallest = Seats[Team];
				Best = Team;
			}
		}
		return Best;
	}

	/** Bot readiness never participates; callers count only active human players. */
	inline bool CanStartRoster(bool bAllowIncompleteTeams, bool bFullRoster, int32 HumanCount, bool bAllHumansReady)
	{
		return HumanCount > 0 && bAllHumansReady && (bAllowIncompleteTeams || bFullRoster);
	}

	inline int32 ClampBotFillTarget(int32 Requested, int32 TeamSize, bool bForceNoBots)
	{
		return bForceNoBots || TeamSize < 2 || TeamSize > 4 ? 0 : FMath::Clamp(Requested, 0, 4 * TeamSize);
	}

	/** Draft seats remain human-only; casual rosters may refill bots after countdown begins. */
	inline bool CanAddBot(bool bRosterLocked, bool bAllowIncompleteTeams, bool bHasDraft)
	{
		return !bHasDraft && (!bRosterLocked || bAllowIncompleteTeams);
	}

	/** Return a leading index plus a tie flag. Only equality with the highest score causes overtime. */
	inline uint8 FindLeader(const int32 (&Scores)[4], bool& bTied)
	{
		uint8 Best = 0;
		bTied = false;
		for (uint8 Team = 1; Team < 4; ++Team)
		{
			if (Scores[Team] > Scores[Best]) { Best = Team; bTied = false; }
			else if (Scores[Team] == Scores[Best]) bTied = true;
		}
		return Best;
	}
}
