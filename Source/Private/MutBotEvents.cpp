// MutBotEvents.cpp - HTTP event poster for UT4IGBot Discord bot
//
// Posts match lifecycle events and flag captures to the bot's FastAPI server.
// Uses FHttpModule (same pattern as StatSQL plugin).
//
// UE4 4.15 quirks observed:
//   - No FString::TrimStartAndEnd(), use .Trim() (mutates in place)
//   - PCH mode UseExplicitOrSharedPCHs: include UnrealTournament.h before UT headers
//   - Must include own header first (UHT requirement)

#include "MutBotEvents.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCarriedObject.h"
#include "UTCTFFlag.h"
#include "UTCTFFlagBase.h"
#include "UTCTFGameState.h"
#include "Json.h"
#include "GameFramework/GameStateBase.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY(LogBotEvents);

AMutBotEvents::AMutBotEvents(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PugId = -1;
	bFlagEventsBound = false;
}

void AMutBotEvents::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_Client) return;

	// Read config from URL options (set by bot in server launch command)
	UWorld* World = GetWorld();
	if (World)
	{
		BotApiUrl = World->URL.GetOption(TEXT("BotApiUrl="), TEXT(""));
		BotApiToken = World->URL.GetOption(TEXT("BotApiToken="), TEXT(""));

		FString PugIdStr = World->URL.GetOption(TEXT("PugId="), TEXT("-1"));
		PugId = FCString::Atoi(*PugIdStr);
	}

	// Trim any trailing slashes from URL
	while (BotApiUrl.EndsWith(TEXT("/")))
	{
		BotApiUrl = BotApiUrl.Left(BotApiUrl.Len() - 1);
	}

	// Fallback: read from Mod.ini
	if (BotApiUrl.IsEmpty())
	{
		FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		GConfig->GetString(TEXT("BOT_EVENTS"), TEXT("BotApiUrl"), BotApiUrl, ConfigPath);
		GConfig->GetString(TEXT("BOT_EVENTS"), TEXT("BotApiToken"), BotApiToken, ConfigPath);
	}

	if (BotApiUrl.IsEmpty())
	{
		UE_LOG(LogBotEvents, Warning, TEXT("No BotApiUrl configured - MutBotEvents will not post events"));
		return;
	}

	UE_LOG(LogBotEvents, Log, TEXT("MutBotEvents initialized: URL=%s PugId=%d"), *BotApiUrl, PugId);
}

void AMutBotEvents::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopReadyPolling();
	Super::EndPlay(EndPlayReason);
}

// ────────────────────────────────────────────────────────────────────
// Match State Changes
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::NotifyMatchStateChange_Implementation(FName NewState)
{
	// MutBotEvents is the chain head in bot-hosted games — forward before any
	// early return, or every downstream mutator (StatSQL submission, etc.) is starved.
	Super::NotifyMatchStateChange_Implementation(NewState);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;

	FString StateStr = NewState.ToString();
	UE_LOG(LogBotEvents, Log, TEXT("Match state changed: %s (PugId=%d)"), *StateStr, PugId);

	// Map UT4 match states to bot event names
	if (NewState == MatchState::WaitingToStart)
	{
		// Start polling player readiness every 5 seconds
		GetWorldTimerManager().SetTimer(ReadyCheckTimer, this,
			&AMutBotEvents::PollPlayerReadiness, 5.0f, true, 0.0f);
	}
	else if (NewState == MatchState::CountdownToBegin)
	{
		StopReadyPolling();
		PostStateChangeWithPlayers(TEXT("CountdownToBegin"));
	}
	else if (NewState == MatchState::InProgress)
	{
		StopReadyPolling();
		TryBindFlagEvents();
		PostStateChangeWithPlayers(TEXT("InProgress"));
	}
	else if (NewState == MatchState::WaitingPostMatch)
	{
		StopReadyPolling();
		PostMatchEnded();
		PostToTReport();
	}
	else if (NewState == MatchState::MapVoteHappening)
	{
		// Don't post anything for map vote
	}
}

// ────────────────────────────────────────────────────────────────────
// Mutate Commands (in-game console: mutate joinpug ictf)
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	// Forward first (chain head) — without this, mutate commands handled by
	// downstream mutators never fire in bot-hosted games.
	Super::Mutate_Implementation(MutateString, Sender);

	if (BotApiUrl.IsEmpty() || !Sender) return;

	AUTPlayerState* UTPS = Cast<AUTPlayerState>(Sender->PlayerState);
	if (!UTPS) return;

	TArray<FString> Parts;
	MutateString.ParseIntoArray(Parts, TEXT(" "), true);

	if (Parts.Num() < 1) return;

	FString Command = Parts[0].ToLower();

	if (Command == TEXT("joinpug") || Command == TEXT("leavepug") || Command == TEXT("listpug"))
	{
		// Only allow joinpug after the match is over (WaitingPostMatch or MapVoteHappening)
		if (Command == TEXT("joinpug"))
		{
			AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
			if (GS)
			{
				FName CurrentState = GS->GetMatchState();
				bool bMatchOver = (CurrentState == MatchState::WaitingPostMatch ||
				                   CurrentState == MatchState::MapVoteHappening);
				if (!bMatchOver)
				{
					// Tell the player they need to wait
					AUTPlayerController* PC = Cast<AUTPlayerController>(Sender);
					if (PC)
					{
						PC->ClientSay(UTPS, TEXT("Cannot join PUG queue while match is in progress. Wait until the match ends."), ChatDestinations::System);
					}
					UE_LOG(LogBotEvents, Log, TEXT("joinpug blocked for %s — match still in progress (state: %s)"),
						*UTPS->PlayerName, *CurrentState.ToString());
					return;
				}
			}
		}

		FString Mode = Parts.Num() > 1 ? Parts[1] : TEXT("ictf");
		FString PlayerName = UTPS->PlayerName;

		// Get UT4 player ID from UniqueId
		FString Ut4Id;
		if (UTPS->UniqueId.IsValid())
		{
			Ut4Id = UTPS->UniqueId.GetUniqueNetId()->ToString();
		}

		// Build JSON
		TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
		Json->SetStringField(TEXT("action"), Command);
		Json->SetStringField(TEXT("mode"), Mode);
		Json->SetStringField(TEXT("ut4_id"), Ut4Id);
		Json->SetStringField(TEXT("player_name"), PlayerName);

		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Json, Writer);

		SendPost(TEXT("/pug_action"), Output);

		UE_LOG(LogBotEvents, Log, TEXT("PUG action: %s mode=%s player=%s ut4_id=%s"),
			*Command, *Mode, *PlayerName, *Ut4Id);
	}
}

// ────────────────────────────────────────────────────────────────────
// Flag Captures
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::ScoreObject_Implementation(
	AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
	AUTPlayerState* Holder, FName Reason)
{
	// Forward first (chain head) — StatSQL's flag/carry tracking lives downstream.
	Super::ScoreObject_Implementation(GameObject, HolderPawn, Holder, Reason);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;

	if (Reason == FName(TEXT("FlagCapture")) && Holder)
	{
		PostFlagCapture(Holder);
	}
}

// ────────────────────────────────────────────────────────────────────
// Cover Kills
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::ScoreKill_Implementation(AController* Killer, AController* Other,
	TSubclassOf<UDamageType> DamageType)
{
	// Forward down the mutator chain first — MutBotEvents runs ahead of
	// MutStatSQL/MutServerShield, which both rely on ScoreKill.
	Super::ScoreKill_Implementation(Killer, Other, DamageType);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;
	if (!Killer || !Other || Killer == Other) return; // ignore suicides/environment

	AUTPlayerState* KillerPS = Cast<AUTPlayerState>(Killer->PlayerState);
	AUTPlayerState* VictimPS = Cast<AUTPlayerState>(Other->PlayerState);
	if (!KillerPS || !VictimPS || KillerPS == VictimPS) return;

	// Kill-streak highlights (monster kills + sprees) — fire BEFORE the
	// team-CTF gate below so FFA / Duel / ShaftArena (no valid teams) still
	// post. The engine's IncrementKills already excluded team-kills from
	// MultiKillLevel/Spree by the time we get here.
	ScoreKill_PostHighlights(KillerPS);

	const int32 KillerTeam = KillerPS->GetTeamNum();
	const int32 VictimTeam = VictimPS->GetTeamNum();

	// A cover kill is a frag of an enemy by a teammate of the flag carrier while
	// that team is carrying. Both must be on valid, opposing teams (no team kills).
	if (KillerTeam > 1 || VictimTeam > 1 || KillerTeam == VictimTeam) return;

	FCoverCarryWindow& Window = CarryWindows[KillerTeam];
	if (!Window.bOpen) return;

	// The carrier isn't their own cover.
	const FString KillerName = KillerPS->PlayerName;
	if (KillerName == Window.CarrierName) return;

	Window.CoverKills.AddUnique(KillerName);
}

// ────────────────────────────────────────────────────────────────────
// Kill-streak highlights (multi-kills + sprees)
// ────────────────────────────────────────────────────────────────────
//
// Hooked into ScoreKill_Implementation below, fires AFTER Super:: has resolved
// (so AUTPlayerState::MultiKillLevel + Spree are already updated by the engine's
// IncrementKills path). Sends to /reward; the bot turns it into a Discord chat
// highlight. Skips in-game bots so a low-pop bot match doesn't flood chat.
//
// MultiKill rungs (UTMultiKillMessage.h):
//   1=Double, 2=Multi, 3=ULTRA, 4=MONSTER (engine caps at 4 for the announcement).
// We only post the Monster threshold (level 4) — exact equality so the kill
// AFTER the monster (level=5,6,...) doesn't re-fire.
//
// Spree rungs (UTSpreeMessage.h, engine bumps every Spree % 5 == 0):
//   1=Killing Spree, 2=Rampage, 3=Dominating, 4=Unstoppable, 5=Godlike (capped).
// We only post Dominating and above (3..5). The %5==0 gate naturally fires once
// per milestone (no spam between 15 and 20).
void AMutBotEvents::ScoreKill_PostHighlights(AUTPlayerState* KillerPS)
{
	if (KillerPS == nullptr || BotApiUrl.IsEmpty()) return;
	if (KillerPS->bIsABot) return;       // don't flood chat in bot-heavy matches

	// MONSTER KILL — fire for level 4 AND every subsequent kill in the same
	// engine multikill window (5, 6, ...). Multiplier turns those continuations
	// into "MONSTER KILL x2 / x3 / ..." on the bot side via Discord message edit
	// instead of fresh embeds. Level 4 -> Multiplier 1 -> new embed; >4 -> edit.
	if (KillerPS->MultiKillLevel >= 4)
	{
		const int32 Multiplier = KillerPS->MultiKillLevel - 3;     // 4->1, 5->2, 6->3, ...
		PostReward(KillerPS, TEXT("monster"), KillerPS->MultiKillLevel, Multiplier);
	}

	// DOMINATING and above (level 3..5). The %5==0 gate is the engine's own
	// milestone check; matches AUTGameMode::ScoreKill at UTGameMode.cpp:2846.
	// Each spree milestone is its own event — multiplier always 1.
	if (KillerPS->Spree > 0 && (KillerPS->Spree % 5) == 0)
	{
		const int32 SpreeLevel = KillerPS->Spree / 5;
		if (SpreeLevel >= 3 && SpreeLevel <= 5)
		{
			PostReward(KillerPS, TEXT("spree"), SpreeLevel, 1);
		}
	}
}

void AMutBotEvents::TryBindFlagEvents()
{
	if (bFlagEventsBound) return;

	AUTCTFGameState* CTFGS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (!CTFGS) return; // not a CTF mode — no flags to track

	// Use GetFlagBase() accessor — never access FlagBases directly (ABI mismatch).
	bool bBoundAny = false;
	for (int32 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		AUTCTFFlagBase* Base = CTFGS->GetFlagBase(TeamIdx);
		if (Base && Base->GetCarriedObject())
		{
			Base->GetCarriedObject()->OnCarriedObjectHolderChangedDelegate.AddDynamic(
				this, &AMutBotEvents::OnFlagHolderChanged);
			bBoundAny = true;
			UE_LOG(LogBotEvents, Log, TEXT("Bound cover-kill tracker to flag holder-changed delegate for team %d"), TeamIdx);
		}
	}

	if (bBoundAny)
	{
		bFlagEventsBound = true;
		CarryWindows[0] = FCoverCarryWindow();
		CarryWindows[1] = FCoverCarryWindow();
	}
}

void AMutBotEvents::OnFlagHolderChanged(AUTCarriedObject* Flag)
{
	if (!Flag) return;

	AUTPlayerState* Holder = Flag->Holder;
	if (Holder)
	{
		// Flag grabbed — open a fresh carry window for the carrier's team.
		const int32 CarrierTeam = Holder->GetTeamNum();
		if (CarrierTeam == 0 || CarrierTeam == 1)
		{
			FCoverCarryWindow& Window = CarryWindows[CarrierTeam];
			Window.bOpen = true;
			Window.CarrierName = Holder->PlayerName;
			Window.CoverKills.Empty();
		}
	}
	else
	{
		// Flag dropped or returned — stop counting covers for the carrying team.
		// A flag's carrier is always on the team opposite its home team. Leave the
		// list intact so a capture firing in the same frame can still read it.
		const int32 FlagTeam = Flag->GetTeamNum();
		if (FlagTeam == 0 || FlagTeam == 1)
		{
			CarryWindows[1 - FlagTeam].bOpen = false;
		}
	}
}

// ────────────────────────────────────────────────────────────────────
// Event Posting Functions
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::PostStateChange(const FString& State)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), State);
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);
}

void AMutBotEvents::PostStateChangeWithPlayers(const FString& State)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), State);
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());
	Json->SetStringField(TEXT("match_id"), GetMatchId());

	// Parse player list JSON array and attach
	FString PlayerListStr = BuildPlayerListJson();
	TArray<TSharedPtr<FJsonValue>> PlayersArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PlayerListStr);
	FJsonSerializer::Deserialize(Reader, PlayersArray);
	Json->SetArrayField(TEXT("Players"), PlayersArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);
}

void AMutBotEvents::PostFlagCapture(AUTPlayerState* Scorer)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	int32 TeamIndex = Scorer->GetTeamNum();
	FString PlayerName = Scorer->PlayerName;

	// Get team scores
	int32 ScoreRed = 0, ScoreBlue = 0;
	if (GS->Teams.IsValidIndex(0) && GS->Teams[0]) ScoreRed = GS->Teams[0]->Score;
	if (GS->Teams.IsValidIndex(1) && GS->Teams[1]) ScoreBlue = GS->Teams[1]->Score;

	// Remaining time
	float RemainingTime = GS->GetRemainingTime();

	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("score_type"), TEXT("FlagCapture"));
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetStringField(TEXT("player_name"), PlayerName);
	Json->SetNumberField(TEXT("player_team"), TeamIndex);
	Json->SetStringField(TEXT("match_id"), GetMatchId());
	Json->SetNumberField(TEXT("match_remaining_time"), RemainingTime);

	// Cover kills: teammates who fragged enemies while this team carried the flag.
	// Snapshot the open carry window for the capper's team, then reset it.
	TArray<TSharedPtr<FJsonValue>> CoverKillsArray;
	int32 NumCovers = 0;
	if (TeamIndex == 0 || TeamIndex == 1)
	{
		FCoverCarryWindow& Window = CarryWindows[TeamIndex];
		for (const FString& Name : Window.CoverKills)
		{
			CoverKillsArray.Add(MakeShareable(new FJsonValueString(Name)));
		}
		NumCovers = Window.CoverKills.Num();
		Window = FCoverCarryWindow();
	}
	Json->SetArrayField(TEXT("cover_kills"), CoverKillsArray);

	// Team scores array
	TArray<TSharedPtr<FJsonValue>> TeamsArray;

	TSharedRef<FJsonObject> RedTeam = MakeShareable(new FJsonObject());
	RedTeam->SetNumberField(TEXT("id"), 0);
	RedTeam->SetNumberField(TEXT("score"), ScoreRed);
	TeamsArray.Add(MakeShareable(new FJsonValueObject(RedTeam)));

	TSharedRef<FJsonObject> BlueTeam = MakeShareable(new FJsonObject());
	BlueTeam->SetNumberField(TEXT("id"), 1);
	BlueTeam->SetNumberField(TEXT("score"), ScoreBlue);
	TeamsArray.Add(MakeShareable(new FJsonValueObject(BlueTeam)));

	Json->SetArrayField(TEXT("teams"), TeamsArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/score"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Flag capture: %s (team %d) | Score: %d-%d | Covers: %d"),
		*PlayerName, TeamIndex, ScoreRed, ScoreBlue, NumCovers);
}

void AMutBotEvents::PostReward(AUTPlayerState* Scorer, const FString& Type, int32 Level, int32 Multiplier)
{
	if (Scorer == nullptr || BotApiUrl.IsEmpty()) return;

	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("type"),         Type);          // "monster" | "spree"
	Json->SetNumberField(TEXT("level"),        Level);         // raw engine value
	Json->SetNumberField(TEXT("multiplier"),   Multiplier);    // 1 = new embed; >1 = edit existing
	Json->SetStringField(TEXT("player_name"),  Scorer->PlayerName);
	Json->SetNumberField(TEXT("player_team"),  Scorer->GetTeamNum());
	Json->SetNumberField(TEXT("pug_id"),       PugId);
	Json->SetStringField(TEXT("match_id"),     GetMatchId());
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/reward"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Reward: %s lvl=%d x%d by %s (team %d)"),
		*Type, Level, Multiplier, *Scorer->PlayerName, Scorer->GetTeamNum());
}

// ────────────────────────────────────────────────────────────────────
// Trigger-bot review — time-on-target at fire (ToT)
// ────────────────────────────────────────────────────────────────────

// Dwell histogram buckets (ms): [0,8) [8,16) [16,32) [32,64) [64,128) [128,256)
// [256,512) [512,inf). Log-ish spacing so the low end (flick / trigger-bot band)
// has fine resolution and the long tracking tail collapses into a few buckets.
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

void AMutBotEvents::RecordFireToT(AUTPlayerState* Shooter, int32 DwellMs, uint8 FrameMs, bool bClaimedHit)
{
	if (Shooter == nullptr) return;

	// Stable identity, most-stable first: StatsID (logged-in account, survives a
	// reconnect) -> session UniqueId (stable for the connection even for a guest,
	// and immune to a mid-match `setname`) -> name (last resort). A drop+rejoin
	// (what the auto-pause feature exists to handle) keeps the same StatsID; a guest
	// renaming mid-match keeps the same UniqueId. Name alone is the least stable key.
	FString Key;
	if (!Shooter->StatsID.IsEmpty())      Key = TEXT("id:")   + Shooter->StatsID;
	else if (Shooter->UniqueId.IsValid()) Key = TEXT("net:")  + Shooter->UniqueId.ToString();
	else                                  Key = TEXT("name:") + Shooter->PlayerName;

	UWorld* World = GetWorld();
	AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	const float Now = GS ? GS->GetServerWorldTimeSeconds()
	                     : (World ? World->GetTimeSeconds() : 0.0f);

	FToTStat& S = ToTStats.FindOrAdd(Key);
	S.PlayerName = Shooter->PlayerName; // keep the latest display name for this identity
	S.Samples.Emplace(FMath::Clamp(DwellMs, 0, 60000), Now, FrameMs, bClaimedHit);
}

AMutBotEvents::FToTSummary AMutBotEvents::ComputeToTSummary(const FToTStat& S) const
{
	FToTSummary R;
	FMemory::Memzero(&R, sizeof(R)); // POD-only struct (no FString) — safe to zero
	const int32 N = S.Samples.Num();
	R.N = N;
	if (N == 0) return R;

	TArray<int32> D; D.Reserve(N);
	double Sum = 0.0, FrameSum = 0.0; int32 FrameCnt = 0, LowCnt = 0, FirstFrameCnt = 0;
	for (const FToTSample& Smp : S.Samples)
	{
		D.Add(Smp.DwellMs);
		Sum += Smp.DwellMs;
		R.Hist[ToTHistBucket(Smp.DwellMs)]++;
		if (Smp.DwellMs <= ToTLowDwellMs) LowCnt++;
		if (Smp.bClaimedHit) R.HitN++;
		if (Smp.FrameMs > 0) { FrameSum += Smp.FrameMs; FrameCnt++; }
		// "First-frame fire": dwell within ~1 of the PLAYER'S OWN frame time. Unlike
		// the fixed-ms low-dwell band this is fps-FAIR by construction — "did you
		// fire within a frame of acquiring", the same question at 60 or 480 fps.
		// Falls back to the fixed band when the client didn't report a frame time.
		const int32 FrameRef = (Smp.FrameMs > 0) ? (int32)Smp.FrameMs : ToTLowDwellMs;
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

	// The "suspect" hint is RELATIVE to the lobby and is set in PostToTReport once
	// every player's summary is known. Ground truth (UT99 'meep'): a real cheat sits
	// at an ELEVATED fast-hit fraction vs peers WITH low variance — NOT at some fixed
	// absolute %. A lone absolute threshold either misses the cheat or floods on fast
	// instagib flickers; peer-relative self-calibrates per lobby and per fps.
	R.bSuspectConsistent = false;
	return R;
}

void AMutBotEvents::PostToTReport()
{
	if (ToTStats.Num() == 0) return;

	const int32 LowDwellMs = ToTLowDwellMs; // local copy (avoid static-const ODR-use in varargs)

	// Pass 1 — summarize every player.
	struct FRow { FString Name; FToTSummary R; };
	TArray<FRow> Rows;
	for (const TPair<FString, FToTStat>& Pair : ToTStats)
	{
		if (Pair.Value.Samples.Num() == 0) continue;
		FRow Row; Row.Name = Pair.Value.PlayerName; Row.R = ComputeToTSummary(Pair.Value);
		Rows.Add(Row);
	}
	if (Rows.Num() == 0) return;

	// Lobby baseline: median first-frame fraction over players with enough samples.
	// The "suspect" hint is a RELATIVE outlier test against this — a real cheat is the
	// clear high outlier of its OWN lobby, which self-calibrates across fps and across
	// modes (instagib lobbies run fast across the board, so an absolute cut is wrong).
	TArray<float> FF, CVs;
	for (const FRow& Row : Rows)
		if (Row.R.N >= ToTMinSamplesForFlag) { FF.Add(Row.R.FirstFramePct); CVs.Add(Row.R.CV); }
	float LobbyMedianFF = 0.0f, LobbyMedianCV = 0.0f;
	if (FF.Num()  > 0) { FF.Sort();  LobbyMedianFF = FF[FF.Num() / 2]; }
	if (CVs.Num() > 0) { CVs.Sort(); LobbyMedianCV = CVs[CVs.Num() / 2]; }

	// Pass 2 — relative flag + emit. Thresholds are PROVISIONAL; tune from gathered
	// data (the operator's UT99 'meep' set shows a cheat ~2-4x peers AND consistent
	// across matches). A single-match flag is a soft hint; the strong signal is the
	// SAME player flagging across many matches — persist these rows to get that.
	FString PlayersJson;
	for (FRow& Row : Rows)
	{
		FToTSummary& R = Row.R;
		const bool bEnough = (R.N >= ToTMinSamplesForFlag) && (FF.Num() >= 3);

		// Signature A — "fast": elevated fast-hit fraction vs peers (instant/naive
		// trigger that fires the moment the crosshair is on target).
		const bool bFastOutlier = bEnough && (R.CV < 0.5f) && (R.FirstFramePct >= 12.0f)
			&& (R.FirstFramePct >= 2.0f * FMath::Max(LobbyMedianFF, 1.0f));

		// Signature B — "regular": tight spread AND no human tracking tail (compressed
		// p90/p50), clearly below the lobby's variance. Catches a DELAY-RANDOMISED
		// trigger bot whose dwell sits mid-range (e.g. 90-200ms): a fixed delay band is
		// itself unnaturally consistent (CV ~0.2) and, crucially, never produces the
		// long tracking/holding dwells a real player accrues — the one shape a trigger
		// bot can't fake without throwing away its advantage (not shooting open targets).
		const float TailRatio = (float)R.P90 / (float)FMath::Max(R.P50, 1);
		const bool bTooRegular = bEnough && (R.CV < 0.45f) && (TailRatio < 2.5f)
			&& (R.CV <= 0.6f * FMath::Max(LobbyMedianCV, KINDA_SMALL_NUMBER));

		R.bSuspectConsistent = bFastOutlier || bTooRegular;
		const TCHAR* Reason = bFastOutlier ? TEXT("fast") : (bTooRegular ? TEXT("regular") : TEXT(""));
		const FString Flag = R.bSuspectConsistent
			? FString::Printf(TEXT("  [REVIEW: %s]"), Reason) : FString();

		const float Fps = (R.MeanFrameMs > 0.0f) ? (1000.0f / R.MeanFrameMs) : 0.0f;

		// Server log — Warning so it survives Shipping builds (>= verbosity threshold),
		// giving a review trail even on servers with no bot URL configured.
		UE_LOG(LogBotEvents, Warning,
			TEXT("[ToT] %s: n=%d hits=%d mean=%.0f sd=%.0f cv=%.2f p10=%d p50=%d p90=%d min=%d max=%d low<=%dms=%.0f%% firstframe=%.0f%%(lobby~%.0f%%) fps~%.0f outliers=%d%s"),
			*Row.Name, R.N, R.HitN, R.MeanMs, R.StdDevMs, R.CV, R.P10, R.P50, R.P90, R.MinMs, R.MaxMs,
			LowDwellMs, R.LowDwellPct, R.FirstFramePct, LobbyMedianFF, Fps, R.OutlierCount, *Flag);

		// Minimal JSON-safety on the name (names can contain quotes/backslashes).
		const FString SafeName = Row.Name.Replace(TEXT("\\"), TEXT("")).Replace(TEXT("\""), TEXT("'"));
		FString HistJson;
		for (int32 i = 0; i < 8; ++i) { if (i) HistJson += TEXT(","); HistJson += FString::FromInt(R.Hist[i]); }

		if (!PlayersJson.IsEmpty()) PlayersJson += TEXT(",");
		PlayersJson += FString::Printf(
			TEXT("{\"name\":\"%s\",\"n\":%d,\"claimed_hits\":%d,\"mean_ms\":%.1f,\"sd_ms\":%.1f,\"cv\":%.3f,")
			TEXT("\"p10\":%d,\"p50\":%d,\"p90\":%d,\"min\":%d,\"max\":%d,\"low_dwell_pct\":%.1f,\"first_frame_pct\":%.1f,")
			TEXT("\"mean_frame_ms\":%.1f,\"outliers\":%d,\"suspect_outlier\":%s,\"suspect_reason\":\"%s\",\"hist\":[%s]}"),
			*SafeName, R.N, R.HitN, R.MeanMs, R.StdDevMs, R.CV,
			R.P10, R.P50, R.P90, R.MinMs, R.MaxMs, R.LowDwellPct, R.FirstFramePct,
			R.MeanFrameMs, R.OutlierCount, R.bSuspectConsistent ? TEXT("true") : TEXT("false"), Reason, *HistJson);
	}

	// Always write the per-sample timeline (independent of bot connectivity) — it
	// is the review trail an admin scrubs against a server demo.
	WriteToTTimelineCsv();

	if (BotApiUrl.IsEmpty()) return; // already logged + CSV written; nothing to POST

	const FString Body = FString::Printf(
		TEXT("{\"pug_id\":%d,\"match_id\":\"%s\",\"low_dwell_ms\":%d,\"lobby_first_frame_pct\":%.1f,\"players\":[%s]}"),
		PugId, *GetMatchId(), LowDwellMs, LobbyMedianFF, *PlayersJson);
	SendPost(TEXT("/triggerbot_report"), Body);
}

void AMutBotEvents::WriteToTTimelineCsv() const
{
	// Flatten every player's samples into one match-clock-sorted timeline. An admin
	// reviewing a SERVER demo scrubs to server_time and sees who fired with what
	// dwell. (Client->server telemetry RPCs are not carried in a demo stream, so a
	// side-file keyed on the match clock is the realistic "diagnosis in replay" —
	// true in-demo overlay would need the per-shot data replicated to clients.)
	struct FRow { float T; FString Name; int32 Dwell; uint8 Frame; bool bHit; };
	TArray<FRow> Rows;
	for (const TPair<FString, FToTStat>& Pair : ToTStats)
		for (const FToTSample& Smp : Pair.Value.Samples)
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
		// CSV-safety: strip the separators/quotes a UT name can contain.
		const FString Name = Rw.Name.Replace(TEXT("\""), TEXT("'")).Replace(TEXT(","), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
		Csv += FString::Printf(TEXT("%.3f,%s,%d,%d,%d\n"), Rw.T, *Name, Rw.Dwell, (int32)Rw.Frame, Rw.bHit ? 1 : 0);
	}

	FString SafeMatch = GetMatchId();
	SafeMatch = SafeMatch.Replace(TEXT("/"), TEXT("_")).Replace(TEXT("\\"), TEXT("_")).Replace(TEXT(":"), TEXT("_"));
	const FString Path = FPaths::GameSavedDir() / TEXT("Logs") / (TEXT("ToT_") + SafeMatch + TEXT(".csv"));
	// SaveStringToFile does NOT create the directory tree — ensure Logs/ exists first
	// (normally yes, the engine log lives there, but be defensive on a fresh box).
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (FFileHelper::SaveStringToFile(Csv, *Path))
	{
		UE_LOG(LogBotEvents, Warning, TEXT("[ToT] timeline written: %s (%d samples)"), *Path, Rows.Num());
	}
	else
	{
		UE_LOG(LogBotEvents, Warning, TEXT("[ToT] FAILED to write timeline: %s"), *Path);
	}
}

void AMutBotEvents::PostMatchEnded()
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), TEXT("WaitingPostMatch"));
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());
	Json->SetStringField(TEXT("match_id"), GetMatchId());

	// Team scores array
	FString TeamsStr = BuildTeamScoresJson();
	TArray<TSharedPtr<FJsonValue>> TeamsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TeamsStr);
	FJsonSerializer::Deserialize(Reader, TeamsArray);
	Json->SetArrayField(TEXT("teams"), TeamsArray);

	// Player list
	FString PlayerListStr = BuildPlayerListJson();
	TArray<TSharedPtr<FJsonValue>> PlayersArray;
	TSharedRef<TJsonReader<>> PlayerReader = TJsonReaderFactory<>::Create(PlayerListStr);
	FJsonSerializer::Deserialize(PlayerReader, PlayersArray);
	Json->SetArrayField(TEXT("Players"), PlayersArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Match ended posted for PugId=%d"), PugId);
}

// ────────────────────────────────────────────────────────────────────
// Player Readiness Polling
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::PollPlayerReadiness()
{
	PostStateChangeWithPlayers(TEXT("WaitingToStart"));
}

void AMutBotEvents::StopReadyPolling()
{
	GetWorldTimerManager().ClearTimer(ReadyCheckTimer);
}

// ────────────────────────────────────────────────────────────────────
// HTTP Sending (StatSQL pattern)
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::SendPost(const FString& Endpoint, const FString& JsonBody, int32 RetryCount)
{
	if (BotApiUrl.IsEmpty()) return;

	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BotApiUrl + Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	if (!BotApiToken.IsEmpty())
	{
		Request->SetHeader(TEXT("api-token"), BotApiToken);
	}

	Request->SetContentAsString(JsonBody);

	TWeakObjectPtr<AMutBotEvents> WeakThis(this);
	FString CapturedEndpoint = Endpoint;
	TSharedRef<FString> CapturedBody = MakeShareable(new FString(JsonBody));

	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, RetryCount, CapturedEndpoint, CapturedBody](
			FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		if (!WeakThis.IsValid()) return;

		bool bSuccess = bConnected && Resp.IsValid() &&
			EHttpResponseCodes::IsOk(Resp->GetResponseCode());

		if (!bSuccess && RetryCount < WeakThis->MaxRetries)
		{
			float Delay = FMath::Pow(2.f, (float)RetryCount);
			UE_LOG(LogBotEvents, Warning, TEXT("POST %s failed (attempt %d/%d), retrying in %.0fs"),
				*CapturedEndpoint, RetryCount + 1, WeakThis->MaxRetries, Delay);

			FTimerHandle RetryHandle;
			WeakThis->GetWorldTimerManager().SetTimer(RetryHandle,
				[WeakThis, CapturedEndpoint, CapturedBody, RetryCount]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->SendPost(CapturedEndpoint, *CapturedBody, RetryCount + 1);
				}
			}, Delay, false);
			return;
		}

		if (!bSuccess)
		{
			int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			UE_LOG(LogBotEvents, Error, TEXT("POST %s FAILED after %d attempts (HTTP %d)"),
				*CapturedEndpoint, WeakThis->MaxRetries, Code);
		}
		else
		{
			UE_LOG(LogBotEvents, Verbose, TEXT("POST %s succeeded"), *CapturedEndpoint);
		}
	});

	Request->ProcessRequest();
}

// ────────────────────────────────────────────────────────────────────
// JSON Helpers
// ────────────────────────────────────────────────────────────────────

FString AMutBotEvents::BuildPlayerListJson() const
{
	TArray<TSharedPtr<FJsonValue>> PlayersArray;

	UWorld* World = GetWorld();
	if (!World) return TEXT("[]");

	AUTGameState* GS = World->GetGameState<AUTGameState>();
	if (!GS) return TEXT("[]");

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		// With host-controlled matches, "ready" = connected and on a team.
		// GetTeamNum() returns 255 if not on a team yet.
		bool bReady = (UTPS->GetTeamNum() < 2);

		TSharedRef<FJsonObject> PlayerObj = MakeShareable(new FJsonObject());
		PlayerObj->SetStringField(TEXT("Name"), UTPS->PlayerName);
		PlayerObj->SetNumberField(TEXT("Index"), UTPS->PlayerId);
		PlayerObj->SetNumberField(TEXT("Id"), UTPS->PlayerId);
		PlayerObj->SetBoolField(TEXT("Ready"), bReady);
		PlayerObj->SetStringField(TEXT("Password"), TEXT("")); // Not available server-side
		PlayerObj->SetNumberField(TEXT("Team"), UTPS->GetTeamNum());
		PlayerObj->SetStringField(TEXT("match_id"), GetMatchId());

		PlayersArray.Add(MakeShareable(new FJsonValueObject(PlayerObj)));
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(PlayersArray, Writer);
	return Output;
}

FString AMutBotEvents::BuildTeamScoresJson() const
{
	TArray<TSharedPtr<FJsonValue>> TeamsArray;

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS) return TEXT("[]");

	for (int32 i = 0; i < 2; i++)
	{
		int32 Score = 0;
		if (GS->Teams.IsValidIndex(i) && GS->Teams[i])
		{
			Score = GS->Teams[i]->Score;
		}

		TSharedRef<FJsonObject> TeamObj = MakeShareable(new FJsonObject());
		TeamObj->SetNumberField(TEXT("id"), i);
		TeamObj->SetNumberField(TEXT("score"), Score);
		TeamsArray.Add(MakeShareable(new FJsonValueObject(TeamObj)));
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(TeamsArray, Writer);
	return Output;
}

FString AMutBotEvents::GetMatchId() const
{
	// Use the game's session ID as a unique match identifier
	UWorld* World = GetWorld();
	if (World && World->GetGameInstance())
	{
		return World->URL.Map + TEXT("_") + FString::FromInt(PugId);
	}
	return FString::Printf(TEXT("pug_%d"), PugId);
}

float AMutBotEvents::GetTimeSeconds() const
{
	UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.f;
}
