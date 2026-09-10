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
