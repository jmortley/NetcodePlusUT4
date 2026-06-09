// NCPlusScoreboardHost.cpp — see header.
#include "NCPlusScoreboardHost.h"
#include "NCPlusHostInfo.h"
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "EngineUtils.h"

namespace NCPlusScoreboardHost
{
	/** Cached lookup of the per-match ANCHostInfo. The scoreboard calls IsHost
	 *  per row per frame, so the actor-list walk is throttled to 1/s and the
	 *  hit path is a weak-pointer deref. Static state is single-client safe;
	 *  the world compare invalidates it across map travel. */
	static ANCHostInfo* FindHostInfo(UWorld* World)
	{
		static TWeakObjectPtr<ANCHostInfo> Cached;
		static float NextScanTime = -1.f;

		ANCHostInfo* Info = Cached.Get();
		if (Info != nullptr && Info->GetWorld() == World)
		{
			return Info;
		}
		const float Now = World->GetTimeSeconds();
		if (Now < NextScanTime - 1.5f)
		{
			NextScanTime = -1.f;   // world time went backwards = new match; rescan now
		}
		if (Now < NextScanTime)
		{
			return nullptr;
		}
		NextScanTime = Now + 1.f;
		for (TActorIterator<ANCHostInfo> It(World); It; ++It)
		{
			Cached = *It;
			return *It;
		}
		Cached = nullptr;
		return nullptr;
	}

	bool IsHost(AUTPlayerState* PS, AUTGameState* GS)
	{
		if (PS == nullptr)
		{
			return false;
		}
		// Warmup-only by design: the badge is most useful before the match starts
		// (so latecomers know who's about to press Enter); once InProgress hits, it
		// just clutters the scoreboard. Drop after WaitingToStart so it disappears
		// for the live match, halves, OT, and end-of-match.
		if (GS == nullptr || GS->GetMatchState() != MatchState::WaitingToStart)
		{
			return false;
		}
		// Plugin-replicated host identity first. The engine fields below ride the
		// AUTPlayerState/AUTGameState replication lists, which differ between the
		// patched server builds and the stock shipping client (IntroClass /
		// LineUpHelper list skew) — whether they arrive intact depends on which
		// engine-binary vintage a server box runs (the UK-vs-NYC badge mystery,
		// Jun 2026). ANCHostInfo is a plugin class, version-gated identical on
		// both ends, so when it's present and resolved it is authoritative.
		if (ANCHostInfo* Info = FindHostInfo(GS->GetWorld()))
		{
			if (Info->HostPS != nullptr)
			{
				return PS == Info->HostPS;
			}
			// Present but unresolved (no host configured, host not joined yet, or
			// the first-second replication gap) — fall through to the engine
			// fields so behavior is never worse than the old path.
		}
		// Engine fallback (old servers without ANCHostInfo): flag set when the
		// host (HostId player) is present.
		if (PS->bIsMatchHost)
		{
			return true;
		}
		// Fallback: the replicated HostIdString covers the warmup window before
		// ReadyToStartMatch assigns bIsMatchHost, and any mode that doesn't run
		// that path. Bots have no UniqueId so they never match.
		if (!GS->HostIdString.IsEmpty() && PS->UniqueId.IsValid())
		{
			return GS->HostIdString.Equals(PS->UniqueId.ToString(), ESearchCase::IgnoreCase);
		}
		return false;
	}

	void DrawHostMarker(UUTHUDWidget* Scoreboard, AUTHUD* HUD, AUTPlayerState* PS,
		AUTGameState* GS, float X, float Y, float RenderScale)
	{
		if (Scoreboard == nullptr || HUD == nullptr || !IsHost(PS, GS))
		{
			return;
		}
		static const FText HostText = NSLOCTEXT("NCPlusScoreboard", "HostTag", "HOST");
		// Gold badge so it reads as special — identical on every NetcodePlus scoreboard.
		Scoreboard->DrawText(HostText, X, Y, HUD->TinyFont, RenderScale, 1.0f,
			FLinearColor(1.f, 0.84f, 0.1f, 1.f), ETextHorzPos::Left, ETextVertPos::Center);
	}
}
