#pragma once

// Persisted scenario IDs: append new presets without renumbering saved boards.
namespace NCAimTrainerScenarioId
{
    enum
    {
        LinkTracking = 0,
        Headshots = 1,
        InstagibPopup = 2,
        PrecisionPopup = 3,
        SACTFHeadshots = 4,
        SACTFPopup = 5,
        LinkTrackingHard = 6,
        AirborneInstagib = 7,
        AirbornePrecision = 8,
        AirborneSACTF = 9,
        AirborneRockets = 10,
        ScenarioCount = 11,
        LeaderboardCount = ScenarioCount * 4
    };
}
