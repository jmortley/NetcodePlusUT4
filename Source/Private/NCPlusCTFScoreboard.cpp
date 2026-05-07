// NCPlusCTFScoreboard.cpp — K/D/Eff/Acc/C/G/R/Ping columns for CTF.
#include "NCPlusCTFScoreboard.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTGameState.h"
#include "UTTeamInfo.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "CTFStatsReplicator.h"
#include "StatNames.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UTBot.h"
#include "UTHUD.h"
#include "UTArmor.h"
#include "Engine/Canvas.h"

UNCPlusCTFScoreboard::UNCPlusCTFScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CellWidth = 850.f; // Match Wipeout — plenty of room for 8 columns

	// Parent's PlayerX member is what our DrawPlayer override reads when
	// laying out the country flag, clan tag, and name. Both layouts use
	// 0.09 (past the flag), so set it once here.
	ColumnHeaderPlayerX = 0.09f;

	// Two layouts. Both push PlayerX past the country-flag glyph (FlagX=0.01,
	// ~6-7% of CellWidth) and use a tight name region so long auto-generated
	// names don't crash into the K/D column. Engine MaxNameWidth is hardcoded
	// at 0.42 — our DrawPlayer override re-computes it from PlayerX/KDX so
	// names get scaled into the actual available room.
	NormalLayout.PlayerX  = 0.09f;
	NormalLayout.KDX      = 0.30f;
	NormalLayout.AccX     = 0.37f;
	NormalLayout.CapsX    = 0.45f;
	NormalLayout.GrabsX   = 0.52f;
	NormalLayout.ReturnsX = 0.60f;
	NormalLayout.ArmorsX  = 0.72f;   // wide — 4 icons + counts
	NormalLayout.AmpX     = 0.85f;   // wide — count + MM:SS held
	NormalLayout.PingX    = 0.95f;

	InstagibLayout.PlayerX  = 0.09f;
	InstagibLayout.KDX      = 0.36f;   // pushed right — instagib has no Armors/Amp so the room shifts
	InstagibLayout.EffX     = 0.45f;
	InstagibLayout.AccX     = 0.55f;
	InstagibLayout.CapsX    = 0.65f;
	InstagibLayout.GrabsX   = 0.74f;
	InstagibLayout.ReturnsX = 0.83f;
	InstagibLayout.PingX    = 0.93f;

	// Column header texts
	CH_KD      = NSLOCTEXT("CTFScoreboard", "KDHeader",      "K/D");
	CH_Eff     = NSLOCTEXT("CTFScoreboard", "EffHeader",     "EFF");
	CH_Acc     = NSLOCTEXT("CTFScoreboard", "AccHeader",     "ACC");
	CH_Caps    = NSLOCTEXT("CTFScoreboard", "CapsHeader",    "Caps");
	CH_Grabs   = NSLOCTEXT("CTFScoreboard", "GrabsHeader",   "Grabs");
	CH_Returns = NSLOCTEXT("CTFScoreboard", "ReturnsHeader", "Returns");
	CH_Armors  = NSLOCTEXT("CTFScoreboard", "ArmorsHeader",  "Armors");
	CH_Amp     = NSLOCTEXT("CTFScoreboard", "AmpHeader",     "Amp");
}

// Local helper: does the character have any weapon whose class name contains
// "Instagib"? In instagib mode every player spawns with a weapon class named
// like UTWeap_InstagibRifle (stock) or UTPlusInstagibRifle (plugin variant) -
// the substring is reliable across both. Walks at most ~5-10 inventory items.
//
// Defensive: IsValid catches both null and pending-kill on the character (can
// happen mid-respawn or just after Destroy()) and on each inventory item
// (TInventoryIterator can yield a stale ptr if the inventory chain is mid-mutation).
static bool NCCharHasInstagibWeapon(AUTCharacter* Char)
{
	if (!IsValid(Char)) return false;
	for (TInventoryIterator<AUTWeapon> It(Char); It; ++It)
	{
		AUTWeapon* W = *It;
		if (!IsValid(W)) continue;
		UClass* WClass = W->GetClass();
		if (WClass && WClass->GetName().Contains(TEXT("Instagib")))
		{
			return true;
		}
	}
	return false;
}

const UNCPlusCTFScoreboard::FCtfColumnLayout& UNCPlusCTFScoreboard::GetActiveLayout()
{
	// Replicator-driven path - works once HandleMatchHasStarted has fired.
	// Trusted authoritative source: ACTFStatsReplicator::bIsInstagibMatch is
	// seeded from GM->bIsInstagib in BeginPlay.
	if (ACTFStatsReplicator* Rep = FindStatsReplicator())
	{
		return Rep->bIsInstagibMatch ? InstagibLayout : NormalLayout;
	}

	// Warmup fallback. Replicator MUST defer to HandleMatchHasStarted to avoid
	// client crashes (see feedback_replicator_spawn_timing.md), so during
	// warmup it doesn't exist yet. Sniff inventory instead - purely client-side,
	// no replication timing to worry about.
	//
	// Three layers of fallback for "no local pawn yet" cases:
	//   1. Local player has a pawn → check that pawn's inventory.
	//   2. No local pawn (joined as spectator, hasn't picked a team, or is
	//      dead mid-warmup) → walk world AUTCharacters for any with an
	//      instagib weapon.
	//   3. No characters in world yet (very first second of warmup, no one
	//      has spawned) → fall through to NormalLayout. The replicator will
	//      take over once HandleMatchHasStarted fires; this brief window is
	//      harmless.
	//
	// Each access is null-/IsValid-guarded; the helper handles pending-kill
	// pawns and stale inventory pointers.
	if (IsValid(UTHUDOwner) && IsValid(UTHUDOwner->UTPlayerOwner))
	{
		APawn* LocalPawn = UTHUDOwner->UTPlayerOwner->GetPawn();
		if (NCCharHasInstagibWeapon(Cast<AUTCharacter>(LocalPawn)))
		{
			return InstagibLayout;
		}
	}

	// Spectator / not-yet-joined fallback: walk world AUTCharacters. Cheap
	// (~6-8 chars max in CTF) and only reached during warmup before the
	// replicator exists. TActorIterator skips destroyed actors automatically;
	// the helper guards pending-kill ones.
	UWorld* World = IsValid(UTHUDOwner) ? UTHUDOwner->GetWorld() : nullptr;
	if (World)
	{
		for (TActorIterator<AUTCharacter> CharIt(World); CharIt; ++CharIt)
		{
			if (NCCharHasInstagibWeapon(*CharIt))
			{
				return InstagibLayout;
			}
		}
	}

	return NormalLayout;
}

void UNCPlusCTFScoreboard::PreDraw(float DeltaTime, AUTHUD* InUTHUDOwner, UCanvas* InCanvas, FVector2D InCanvasCenter)
{
	Super::PreDraw(DeltaTime, InUTHUDOwner, InCanvas, InCanvasCenter);
	// Parent CTFScoreboard::PreDraw sets bDrawMinimapInScoreboard = true every frame.
	// Force it off — we don't want the minimap on the scoreboard.
	bDrawMinimapInScoreboard = false;
}

ACTFStatsReplicator* UNCPlusCTFScoreboard::FindStatsReplicator()
{
	if (CachedStatsRep) return CachedStatsRep;

	for (TActorIterator<ACTFStatsReplicator> It(UTHUDOwner->GetWorld()); It; ++It)
	{
		CachedStatsRep = *It;
		return CachedStatsRep;
	}
	return nullptr;
}

void UNCPlusCTFScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		// Background
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height, 149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));
		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState)
		{
			// GetActiveLayout handles replicator-vs-warmup fallback in one place.
			// Identity-compare the returned reference to detect instagib without
			// re-querying the replicator (and without re-walking inventory).
			const FCtfColumnLayout& L = GetActiveLayout();
			const bool bInstagib = (&L == &InstagibLayout);

			DrawText(CH_KD,     XOffset + (ScaledCellWidth * L.KDX),      YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			if (bInstagib)
			{
				DrawText(CH_Eff, XOffset + (ScaledCellWidth * L.EffX),    YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			}
			DrawText(CH_Acc,    XOffset + (ScaledCellWidth * L.AccX),     YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Caps,   XOffset + (ScaledCellWidth * L.CapsX),    YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Grabs,  XOffset + (ScaledCellWidth * L.GrabsX),   YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Returns,XOffset + (ScaledCellWidth * L.ReturnsX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			if (!bInstagib)
			{
				DrawText(CH_Armors, XOffset + (ScaledCellWidth * L.ArmorsX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
				DrawText(CH_Amp,    XOffset + (ScaledCellWidth * L.AmpX),    YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			}

			// Keep parent's ColumnHeaderPingX in sync with our active layout
			// so the DrawPlayer override (which uses that member for the ping
			// value position) lines up with the header.
			ColumnHeaderPingX = L.PingX;
		}
		DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4;
}

void UNCPlusCTFScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	if (!PlayerState) return;

	ACTFStatsReplicator* Rep = FindStatsReplicator();
	const FString PlayerId = PlayerState->UniqueId.IsValid()
		? PlayerState->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *PlayerState->PlayerName);
	const FCtfColumnLayout& L = GetActiveLayout();
	const bool bInstagib = (&L == &InstagibLayout);

	// K/D (combined column — matches duel/elim/wipeout)
	{
		const int32 DisplayKills = PlayerState->Kills + PlayerState->KillAssists;
		const FString KDStr = FString::Printf(TEXT("%d/%d"), DisplayKills, PlayerState->Deaths);
		DrawText(FText::FromString(KDStr),
			XOffset + (Width * L.KDX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// EFF — instagib only. Kills / (Kills + Deaths) * 100.
	if (bInstagib)
	{
		const float EffKills = float(PlayerState->Kills);
		const float EffDeaths = float(PlayerState->Deaths);
		const float EffPct = (EffKills + EffDeaths > 0.f)
			? (EffKills / (EffKills + EffDeaths) * 100.f) : 0.f;
		const FLinearColor EffColor = (EffPct >= 60.f) ? FLinearColor(0.25f, 0.8f, 0.25f, 1.f)
		                            : (EffPct >= 40.f) ? FLinearColor(0.8f,  0.8f, 0.25f, 1.f)
		                            : FLinearColor(0.8f, 0.25f, 0.25f, 1.f);
		DrawText(FText::FromString(FString::Printf(TEXT("%.0f%%"), EffPct)),
			XOffset + (Width * L.EffX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, EffColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Accuracy (from replicator — LG-only or Instagib depending on match mode)
	{
		int32 Hits = 0, Shots = 0;
		if (Rep) Rep->GetAccuracyForPlayer(PlayerId, Hits, Shots);
		const float AccPct = (Shots > 0) ? (float(Hits) / float(Shots) * 100.f) : 0.f;
		const FLinearColor AccColor = (AccPct >= 40.f) ? FLinearColor(0.25f, 0.8f, 0.25f, 1.f)
		                            : (AccPct >= 25.f) ? FLinearColor(0.8f,  0.8f, 0.25f, 1.f)
		                            : FLinearColor(0.8f, 0.25f, 0.25f, 1.f);
		const FString AccStr = (Shots > 0) ? FString::Printf(TEXT("%.0f%%"), AccPct) : TEXT("-");
		DrawText(FText::FromString(AccStr),
			XOffset + (Width * L.AccX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, AccColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Caps
	DrawText(FText::AsNumber(PlayerState->FlagCaptures),
		XOffset + (Width * L.CapsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);

	// Grabs
	{
		int32 Grabs = (Rep) ? Rep->GetGrabsForPlayer(PlayerId) : 0;
		DrawText(FText::AsNumber(Grabs),
			XOffset + (Width * L.GrabsX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Returns
	DrawText(FText::AsNumber(PlayerState->FlagReturns),
		XOffset + (Width * L.ReturnsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);

	// Armors + Amp only in non-instagib play. In instagib these stats are
	// always 0 and the columns are header-suppressed.
	if (!bInstagib && Rep)
	{
		uint8 ArmorCounts[4] = { 0, 0, 0, 0 };
		Rep->GetArmorCountsForPlayer(PlayerId, ArmorCounts);
		DrawArmorIconRow(XOffset + (Width * L.ArmorsX), YOffset + ColumnY,
			ArmorCounts, FLinearColor::White);

		uint8 AmpCount = 0;
		int32 AmpTimeS = 0;
		Rep->GetAmpForPlayer(PlayerId, AmpCount, AmpTimeS);
		DrawAmpCell(XOffset + (Width * L.AmpX), YOffset + ColumnY,
			AmpCount, AmpTimeS, DrawColor);
	}
}

void UNCPlusCTFScoreboard::DrawPlayerScores(float RenderDelta, float& YOffset)
{
	// Copy of UUTTeamScoreboard::DrawPlayerScores with the engine's
	// "X Spectators Watching" count text replaced by a comma-separated
	// names list. Calling Super and then drawing our text is what we used
	// to do, but the engine paints its count at Y=765 — same Y as our
	// names list — and the two overlap into the "garbled" look.
	// Same pattern Elim/Wipeout scoreboards use.
	if (UTGameState == nullptr) return;

	int32 XOffset = ScaledEdgeSize;
	float MaxYOffset = 0.f;
	TArray<FString> SpectatorNames;

	for (int8 Team = 0; Team < 2; Team++)
	{
		int32 Place = 1;
		float DrawOffset = YOffset;
		const int32 NumPlayersToShow = ShouldDrawScoringStats() ? 5 : UTGameState->PlayerArray.Num();
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PlayerState = Cast<AUTPlayerState>(UTGameState->PlayerArray[i]);
			if (PlayerState)
			{
				if (!PlayerState->bOnlySpectator)
				{
					if (PlayerState->GetTeamNum() == Team)
					{
						DrawPlayer(Place, PlayerState, RenderDelta, XOffset, DrawOffset);
						Place++;
						DrawOffset += CellHeight * RenderScale;
						if (Place > NumPlayersToShow) break;
					}
				}
				else if (Team == 0 && !PlayerState->bIsDemoRecording && !PlayerState->PlayerName.IsEmpty())
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
			}
		}
		MaxYOffset = FMath::Max(DrawOffset, MaxYOffset);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset = MaxYOffset;

	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}

// =============================================================================
// Armor icon row + Amp cell helpers (non-instagib)
// =============================================================================
// Stock armor BP class paths. Each BP sets HUDIcon on its CDO with the
// proper UV slice. Loaded once + AddToRoot so GC doesn't reap the cached
// UClass underneath us. Mirrors the duel scoreboard's pattern.
namespace
{
	struct FCtfArmorSlot
	{
		const TCHAR* ClassPath;
		UClass*      CachedClass;
		FCanvasIcon  CachedIcon;
		bool         bResolved;

		// Explicit ctor for UE 4.15 brace-init compatibility — see the
		// FArmorIconSlot note in NCLeagueDuelScoreboard.cpp.
		explicit FCtfArmorSlot(const TCHAR* InPath)
			: ClassPath(InPath), CachedClass(nullptr), bResolved(false) {}
	};

	static FCtfArmorSlot& GetCtfBeltSlot()
	{
		static FCtfArmorSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_ShieldBelt.Armor_ShieldBelt_C") };
		return S;
	}
	static FCtfArmorSlot& GetCtfVestSlot()
	{
		static FCtfArmorSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Chest.Armor_Chest_C") };
		return S;
	}
	static FCtfArmorSlot& GetCtfPadsSlot()
	{
		static FCtfArmorSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_ThighPads.Armor_ThighPads_C") };
		return S;
	}
	static FCtfArmorSlot& GetCtfHelmetSlot()
	{
		static FCtfArmorSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Helmet.Armor_Helmet_C") };
		return S;
	}

	bool ResolveCtfArmorIcon(FCtfArmorSlot& Slot)
	{
		if (Slot.bResolved) return Slot.CachedIcon.Texture != nullptr;
		Slot.bResolved = true;
		Slot.CachedClass = LoadClass<AUTArmor>(nullptr, Slot.ClassPath);
		if (!Slot.CachedClass) return false;
		Slot.CachedClass->AddToRoot();
		if (AUTArmor* CDO = Slot.CachedClass->GetDefaultObject<AUTArmor>())
		{
			Slot.CachedIcon = CDO->HUDIcon;
		}
		return Slot.CachedIcon.Texture != nullptr;
	}
}

void UNCPlusCTFScoreboard::DrawArmorIconRow(float CenterX, float CenterY,
	const uint8* Counts, FLinearColor IconTint)
{
	FCtfArmorSlot* Slots[4] = { &GetCtfBeltSlot(), &GetCtfVestSlot(),
		&GetCtfPadsSlot(), &GetCtfHelmetSlot() };

	const float CellSize = 18.f * RenderScale;   // tighter than duel's row — CTF has more columns
	const float Gap      = 4.f  * RenderScale;
	const float TotalW   = 4.f * CellSize + 3.f * Gap;
	float X = CenterX - TotalW * 0.5f;
	const float IconY = CenterY - CellSize * 0.5f;

	for (int32 i = 0; i < 4; ++i)
	{
		if (ResolveCtfArmorIcon(*Slots[i]))
		{
			const FCanvasIcon& Icon = Slots[i]->CachedIcon;
			DrawTexture(Icon.Texture, X, IconY, CellSize, CellSize,
				Icon.U, Icon.V, Icon.UL, Icon.VL, 1.0f, IconTint);
		}
		// Count below the icon (or "—" when zero, dimmer)
		const uint8 C = Counts[i];
		const FLinearColor NumColor = (C > 0)
			? FLinearColor(1.f, 1.f, 1.f, 1.f)
			: FLinearColor(1.f, 1.f, 1.f, 0.35f);
		DrawText(FText::AsNumber(C),
			X + CellSize * 0.5f, CenterY + CellSize * 0.6f,
			UTHUDOwner->TinyFont, 0.85f * RenderScale, 1.0f, NumColor,
			ETextHorzPos::Center, ETextVertPos::Center);
		X += CellSize + Gap;
	}
}

void UNCPlusCTFScoreboard::DrawAmpCell(float CenterX, float CenterY,
	uint8 AmpCount, int32 AmpTimeS, FLinearColor DrawColor)
{
	// Format: "<count>×  <mm:ss>" — count and held-time on one line.
	// Dimmed when AmpCount is 0.
	const FLinearColor Color = (AmpCount > 0)
		? FLinearColor(1.f, 0.85f, 0.4f, 1.f)   // amp gold
		: FLinearColor(1.f, 1.f, 1.f, 0.35f);

	const int32 Mins = AmpTimeS / 60;
	const int32 Secs = AmpTimeS % 60;
	// ASCII hyphen, not em-dash — this .cpp lacks a UTF-8 BOM so MSVC reads
	// non-ASCII chars as Win-1252 and a "—" renders as "â€"" mojibake in
	// Slate. Same gotcha hit on SNCPlusHUDEditor.cpp / NCDuelRatingSystem.cpp.
	const FString Text = (AmpCount > 0)
		? FString::Printf(TEXT("%u  %02d:%02d"), AmpCount, Mins, Secs)
		: TEXT("-");

	DrawText(FText::FromString(Text),
		CenterX, CenterY, UTHUDOwner->TinyFont, RenderScale, 1.0f, Color,
		ETextHorzPos::Center, ETextVertPos::Center);
}

// =============================================================================
// DrawPlayer override
// =============================================================================
// Same shape as the duel / shaft DrawPlayer overrides:
//   - Copy of UUTScoreboard::DrawPlayer (UTScoreboard.cpp:661)
//   - Null-guard UTHUDOwner->UTPlayerOwner derefs (avoids the pre-match
//     standalone-PIE crash documented in feedback_scoreboard_drawplayer_null_guards.md)
//   - Mute block removed (matches elim/wipeout)
//   - Ping / bot-skill draw is CENTERED at ColumnHeaderPingX rather than
//     right-aligned at 0.995, so the value sits under the column header
//     instead of jamming against the row's right border.
void UNCPlusCTFScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
	float RenderDelta, float XOffset, float YOffset)
{
	if (PlayerState == NULL) return;

	float BarOpacity = 0.3f;
	bool bIsUnderCursor = false;

	if (bIsInteractive)
	{
		FVector4 Bounds = FVector4(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + ScaledCellWidth, RenderPosition.Y + YOffset + CellHeight*RenderScale);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
		bIsUnderCursor = (CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z
			&& CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W);
	}
	PlayerState->ScoreCorner = FVector(RenderPosition.X + XOffset, RenderPosition.Y + YOffset + 0.25f*CellHeight*RenderScale, 0.f);
	if (!PlayerState->Team || (PlayerState->Team->TeamIndex != 1))
	{
		PlayerState->ScoreCorner.X += ScaledCellWidth;
	}

	float NameXL, NameYL;
	float ClanXL = 0.f;
	FString DisplayName = PlayerState->PlayerName;
	FString ClanName = PlayerState->ClanName;
	if (!PlayerState->ClanName.IsEmpty())
	{
		ClanName = "[" + ClanName + "]";
		Canvas->TextSize(UTHUDOwner->SmallFont, ClanName, ClanXL, NameYL, 1.f, 1.f);
		ClanXL += 4.f;
	}
	// Tight name cap based on the active layout's K/D column — engine's
	// hardcoded 0.42 is too generous for our column-dense scoreboard and
	// long auto-generated names ("WIN11X3DPRO-3313") crashed into K/D.
	// Reserve a small padding before the K/D center.
	const FCtfColumnLayout& ActiveL = GetActiveLayout();
	const float NameWidthFraction = FMath::Max(0.18f, (ActiveL.KDX - ColumnHeaderPlayerX) - 0.03f);
	float MaxNameWidth = NameWidthFraction*ScaledCellWidth - (PlayerState->bIsFriend ? 30.f*RenderScale : 0.f);
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	UFont* NameFont = UTHUDOwner->SmallFont;
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);

	// Null-guarded local-owner check (see feedback_scoreboard_drawplayer_null_guards.md).
	int32 Ping = PlayerState->Ping * 4;
	const bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner
		&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner)
	{
		Ping = PlayerState->ExactPing;
		BarOpacity = 0.5f;
	}

	// Background border.
	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor) { BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0); FinalBarOpacity = 0.75f; }
	if (PlayerState == SelectedPlayer) { BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0); FinalBarOpacity = 0.75f; }

	DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth,
		0.9f*CellHeight*RenderScale, 149, 138, 32, 32, FinalBarOpacity, BarColor);

	if (PlayerState->KickCount > 0)
	{
		float NumPlayers = 0.0f;
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			if (!UTGameState->PlayerArray[i]->bIsSpectator
				&& !UTGameState->PlayerArray[i]->bOnlySpectator
				&& !UTGameState->PlayerArray[i]->bIsABot)
			{
				if (!UTGameState->bOnlyTeamCanVoteKick
					|| UTGameState->OnSameTeam(PlayerState, UTGameState->PlayerArray[i]))
				{
					NumPlayers += 1.0f;
				}
			}
		}
		if (NumPlayers > 0.0f)
		{
			float KickPercent = float(PlayerState->KickCount) / NumPlayers;
			float XL, SmallYL;
			Canvas->TextSize(UTHUDOwner->SmallFont, "Kick", XL, SmallYL, RenderScale, RenderScale);
			DrawText(NSLOCTEXT("UTScoreboard", "Kick", "Kick"), XOffset + (ScaledCellWidth * FlagX),
				YOffset + ColumnY - 0.27f*SmallYL, UTHUDOwner->TinyFont, RenderScale, 1.0f,
				DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
			FText Kick = FText::Format(NSLOCTEXT("Common", "PercFormat", "{0}%"),
				FText::AsNumber(int32(KickPercent * 100.0)));
			DrawText(Kick, XOffset + (ScaledCellWidth * FlagX), YOffset + ColumnY + 0.33f*SmallYL,
				UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
				ETextHorzPos::Left, ETextVertPos::Center);
		}
	}
	else
	{
		FTextureUVs FlagUV;
		UTexture2D* NewFlagAtlas = UTHUDOwner->ResolveFlag(PlayerState, FlagUV);
		DrawTexture(NewFlagAtlas, XOffset + (ScaledCellWidth * FlagX), YOffset + 14.f*RenderScale,
			FlagUV.UL*RenderScale, FlagUV.VL*RenderScale, FlagUV.U, FlagUV.V, 36, 26, 1.0,
			FLinearColor::White, FVector2D(0.0f, 0.5f));
	}

	FVector2D NameSize;
	float NameScaling = FMath::Min(RenderScale, MaxNameWidth / FMath::Max(NameXL + ClanXL, 1.f));
	if (!PlayerState->EpicAccountName.IsEmpty())
	{
		NameSize = DrawText(FText::FromString(ClanName),
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
		NameSize += DrawText(FText::FromString(DisplayName),
			XOffset + NameScaling*ClanXL + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, false, FVector2D(0.f, 0.f), FLinearColor::Black, true,
			GetPlayerHighlightColorFor(PlayerState), NameScaling, 1.0f, DrawColor,
			FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), ETextHorzPos::Left, ETextVertPos::Center);
	}
	else
	{
		NameSize = DrawText(FText::FromString(DisplayName),
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
	}

	if (PlayerState->bIsFriend)
	{
		DrawTexture(UTHUDOwner->ScoreboardAtlas,
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX) + NameSize.X*NameScaling + 5.f*RenderScale,
			YOffset + 18.f*RenderScale, 30.f*RenderScale, 24.f*RenderScale,
			236, 136, 30, 24, 1.0, FLinearColor::White, FVector2D(0.0f, 0.5f));
	}
	if (UTGameState && UTGameState->HasMatchStarted())
	{
		if (PlayerState->bPendingTeamSwitch && !PlayerState->bIsABot)
		{
			DrawText(TeamSwapText, XOffset + (ScaledCellWidth * ColumnHeaderScoreX),
				YOffset + ColumnY, UTHUDOwner->SmallFont, RenderScale, 1.0f,
				FLinearColor::White, ETextHorzPos::Center, ETextVertPos::Center);
		}
		else
		{
			DrawPlayerScore(PlayerState, XOffset, YOffset, ScaledCellWidth, DrawColor);
		}
	}
	else
	{
		DrawReadyText(PlayerState, XOffset, YOffset, ScaledCellWidth);
	}

	// Ping / bot-skill: draw CENTERED at ColumnHeaderPingX so the value sits
	// under the column header instead of right-aligning at the row's right
	// edge (engine default 0.995). Same TinyFont + 0.75 scale as engine.
	AUTBot* Bot = Cast<AUTBot>(PlayerState->GetOwner());
	if (Bot)
	{
		static const FNumberFormattingOptions SkillValueFormattingOptions = FNumberFormattingOptions()
			.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		DrawText(FText::AsNumber(Bot->Skill, &SkillValueFormattingOptions),
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, 0.75f*RenderScale, 1.f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}
	else if (GetWorld()->GetNetMode() != NM_Standalone)
	{
		FText PingText = FText::Format(PingFormatText, FText::AsNumber(Ping));
		DrawText(PingText, XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, 0.75f*RenderScale, 1.f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Strike out players that are out of lives.
	if (PlayerState->bOutOfLives)
	{
		float Height = 8.0f;
		float XL, YL;
		Canvas->TextSize(UTHUDOwner->SmallFont,
			(PlayerState->PlayerName + PlayerState->ClanName), XL, YL, RenderScale, RenderScale);
		float StrikeWidth = FMath::Min(0.475f*ScaledCellWidth, XL);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX),
			YOffset + ColumnY, StrikeWidth, Height, 185.f, 400.f, 4.f, 4.f, 1.0f, FLinearColor::Red);
	}

	// Mute block intentionally omitted (matches elim/wipeout DrawPlayer
	// overrides; IsPlayerGameMuted's UTPlayerOwner deref is the kind of
	// unguarded access we just spent a session debugging).
	if (PlayerState->bIsTalking)
	{
		bool bLeft = (XOffset < Canvas->ClipX * 0.5f);
		float TalkingXOffset = bLeft ? ScaledCellWidth + (10.0f * RenderScale) : (-36.0f * RenderScale);
		FTextureUVs ChatIconUVs = bLeft
			? FTextureUVs(497.0f, 965.0f, 35.0f, 31.0f)
			: FTextureUVs(532.0f, 965.0f, -35.0f, 31.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset,
			YOffset + ((CellHeight * 0.5f - 24.0f) * RenderScale),
			(26 * RenderScale), (23 * RenderScale),
			ChatIconUVs.U, ChatIconUVs.V, ChatIconUVs.UL, ChatIconUVs.VL, 1.0f);
	}
}
