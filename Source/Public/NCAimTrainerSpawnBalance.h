#pragma once

// Authority-owned recent confirmed hits, reset for every run. Looking around or
// missing deliberately cannot restore a farmed lane's spawn frequency.
struct FNCAimTrainerSpawnBalance
{
    enum { Left = 0, Center = 1, Right = 2, HistorySize = 8 };
    int RecentSides[HistorySize] = {};
    int SideHits[3] = {};
    int Count = 0;
    int Next = 0;

    void Reset() { *this = FNCAimTrainerSpawnBalance(); }

    static int Side(float RelativeY)
    {
        return RelativeY < -250.f ? Left : RelativeY > 250.f ? Right : Center;
    }

    static int DropSide(int Slot) { return Slot < 3 ? Left : Slot == 3 ? Center : Right; }

    void RecordHit(float RelativeY)
    {
        if (Count == HistorySize) { --SideHits[RecentSides[Next]]; }
        else { ++Count; }
        RecentSides[Next] = Side(RelativeY);
        ++SideHits[RecentSides[Next]];
        Next = (Next + 1) % HistorySize;
    }

    float Weight(int SideIndex) const
    {
        if (Count < 4 || SideIndex < Left || SideIndex > Right) { return 1.f; }
        const float Share = float(SideHits[SideIndex]) / float(Count);
        // Only a majority is penalized. Even an entirely farmed lane retains
        // one fifth of its normal opportunities; balanced play restores it.
        return Share > 0.5f ? 1.f - 1.6f * (Share - 0.5f) : 1.f;
    }
};
