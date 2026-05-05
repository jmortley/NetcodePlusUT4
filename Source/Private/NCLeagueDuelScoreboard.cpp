// NCLeagueDuelScoreboard.cpp — duel-specific scoreboard columns:
//   - "Acc" (combined hitscan accuracy) replaces engine "B/A" column
//   - 4 armor-pickup icons + counts (Belt / Vest / Pads / Helmet) replace
//     the "Eff%" column - duel cares more about armor map control than the
//     kill/death efficiency derived metric

#include "NCLeagueDuelScoreboard.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTArmor.h"
#include "UTInventory.h"
#include "StatNames.h"
#include "EngineUtils.h"
#include "NCLeagueDuelStatsReplicator.h"

namespace
{
	/** Locate the duel stats replicator on this client. Cached on a weak ptr -
	 *  the replicator is bAlwaysRelevant and spawned once per match (same
	 *  pattern as ElimPlus), so re-iterating actors every scoreboard frame is
	 *  wasted work. Cache invalidates on world swap (PIE stop / map travel). */
	ANCLeagueDuelStatsReplicator* FindNCLeagueDuelStatsReplicator(UWorld* World)
	{
		if (!World) return nullptr;
		static TWeakObjectPtr<UWorld> CachedWorld;
		static TWeakObjectPtr<ANCLeagueDuelStatsReplicator> CachedRep;
		if (CachedWorld.Get() == World && CachedRep.IsValid())
		{
			return CachedRep.Get();
		}
		for (TActorIterator<ANCLeagueDuelStatsReplicator> It(World); It; ++It)
		{
			CachedWorld = World;
			CachedRep   = *It;
			return *It;
		}
		return nullptr;
	}

	/** Stock armor BP class paths. Each BP sets HUDIcon on its CDO with the
	 *  proper UV slice into HUDAtlas01 / pickup atlas. We load each once and
	 *  cache the resolved icon so the scoreboard doesn't pay a class-load
	 *  per frame. AddToRoot so GC doesn't reap the loaded UClass underneath
	 *  our raw pointer (same fix pattern as the font cache). */
	struct FArmorIconSlot
	{
		const TCHAR* ClassPath;
		UClass*       CachedClass;
		FCanvasIcon   CachedIcon;
		bool          bResolved;

		// Explicit constructor: UE4 4.15 / MSVC C++14-mode doesn't treat
		// structs with default member initializers as aggregates, so brace-
		// init `FArmorIconSlot{ TEXT("...") }` fails. Same gotcha noted on
		// FAliasEntry in NCPlusHUDLayout.cpp.
		explicit FArmorIconSlot(const TCHAR* InPath)
			: ClassPath(InPath), CachedClass(nullptr), bResolved(false)
		{}
	};

	static FArmorIconSlot& GetBeltSlot()
	{
		static FArmorIconSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_ShieldBelt.Armor_ShieldBelt_C") };
		return S;
	}
	static FArmorIconSlot& GetVestSlot()
	{
		static FArmorIconSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Chest.Armor_Chest_C") };
		return S;
	}
	static FArmorIconSlot& GetPadsSlot()
	{
		static FArmorIconSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_ThighPads.Armor_ThighPads_C") };
		return S;
	}
	static FArmorIconSlot& GetHelmetSlot()
	{
		static FArmorIconSlot S{ TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Helmet.Armor_Helmet_C") };
		return S;
	}

	/** Resolve a slot lazily. Returns true if the icon has a valid texture. */
	bool ResolveArmorIcon(FArmorIconSlot& Slot)
	{
		if (Slot.bResolved) return Slot.CachedIcon.Texture != nullptr;
		Slot.bResolved = true;

		Slot.CachedClass = LoadClass<AUTArmor>(nullptr, Slot.ClassPath);
		if (!Slot.CachedClass) return false;
		Slot.CachedClass->AddToRoot();   // pin against GC

		if (AUTArmor* CDO = Slot.CachedClass->GetDefaultObject<AUTArmor>())
		{
			Slot.CachedIcon = CDO->HUDIcon;
		}
		return Slot.CachedIcon.Texture != nullptr;
	}
}

UNCLeagueDuelScoreboard::UNCLeagueDuelScoreboard(const FObjectInitializer& OI)
	: Super(OI)
{
	CH_Accuracy = NSLOCTEXT("NCLeagueDuelScoreboard", "ColumnHeader_Accuracy", "Acc");

	// Redistribute column X positions for the 5-column duel layout:
	// Player(left-aligned) | K/D | Acc | DMG | Armors(4 icons)
	// - DmgPerLife and Skill/Ping columns are removed (DrawPlayer override
	//   skips the inherited skill/ping draw)
	// - Armors column gets more space than the others because it's a 4-cell
	//   icon strip ~150px wide at typical resolutions
	ColumnHeaderKDX         = 0.42f;
	ColumnHeaderBeltAmpX    = 0.54f;   // Acc
	ColumnHeaderDamageX     = 0.66f;
	ColumnHeaderEfficiencyX = 0.84f;   // Armor row (icon strip)
	// DmgPerLife / Ping positions left at parent defaults; nothing draws them now.
}

void UNCLeagueDuelScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	const float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height,
			149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));

		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_KD, XOffset + (ScaledCellWidth * ColumnHeaderKDX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			// Replace "B/A" with "Acc" - duel cares about shooting precision, not pickups.
			DrawText(CH_Accuracy, XOffset + (ScaledCellWidth * ColumnHeaderBeltAmpX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			// Armors column header: simple text label. Tried 4 icons here but
			// at header-row scale they read as illegible dots, and the icons
			// are tinted black for header parity which makes them invisible
			// against the light header bar. Text is the right call.
			static const FText CH_Armors = NSLOCTEXT("NCLeagueDuelScoreboard", "ColumnHeader_Armors", "Armors");
			DrawText(CH_Armors, XOffset + (ScaledCellWidth * ColumnHeaderEfficiencyX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			// CH_DmgPerLife and CH_Skill/CH_Ping headers intentionally removed
			// per user request - duel scoreboard ends with the armor column.
		}

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4.f;
}

void UNCLeagueDuelScoreboard::DrawPlayerScore(AUTPlayerState* PS, float XOffset,
	float YOffset, float Width, FLinearColor DrawColor)
{
	if (!PS) return;

	// K/D (same as Wipeout)
	const int32 DisplayKills = PS->Kills + PS->KillAssists;
	const FString KDStr = FString::Printf(TEXT("%d/%d"), DisplayKills, PS->Deaths);
	DrawText(FText::FromString(KDStr), XOffset + (Width * ColumnHeaderKDX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Combined hitscan accuracy via the replicator. Reading PS->GetStatsValue
	// directly only works on the authority — AUTPlayerState::StatsData is
	// UPROPERTY() with no Replicated specifier, so on remote clients every
	// per-weapon hit/shot stat reads 0. ANCLeagueDuelStatsReplicator snapshots
	// the values server-side at 1Hz and replicates the percentage.
	const FString PlayerId = PS->UniqueId.IsValid()
		? PS->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
	float Pct = 0.f;
	if (ANCLeagueDuelStatsReplicator* Rep = FindNCLeagueDuelStatsReplicator(GetWorld()))
	{
		Pct = Rep->GetAccuracyForPlayer(PlayerId);
	}
	// Authority fallback — listen-server / standalone where no replication
	// has happened yet. Keeps the local player's row populated from frame 1
	// instead of waiting on the 1Hz tick.
	if (Pct == 0.f && GetWorld() && GetWorld()->GetNetMode() != NM_Client)
	{
		const int32 Hits  = PS->GetStatsValue(NAME_LinkHits)
		                  + PS->GetStatsValue(NAME_ShockRifleHits)
		                  + PS->GetStatsValue(NAME_SniperHits);
		const int32 Shots = PS->GetStatsValue(NAME_LinkShots)
		                  + PS->GetStatsValue(NAME_ShockRifleShots)
		                  + PS->GetStatsValue(NAME_SniperShots);
		Pct = (Shots > 0) ? float(Hits) / float(Shots) * 100.f : 0.f;
	}
	const FLinearColor AccColor = (Pct >= 35.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
	                            : (Pct >= 20.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
	                            : FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	const FString AccStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct));
	DrawText(FText::FromString(AccStr), XOffset + (Width * ColumnHeaderBeltAmpX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, AccColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage
	const int32 Damage = int32(PS->DamageDone);
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PS->GetUTCharacter()) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Armor pickup row (replaces the Eff% column). Reads counts from the
	// stats replicator with the same authority-fallback the accuracy column
	// uses so listen-server / standalone don't wait on the 1Hz tick.
	uint8 Counts[4] = { 0, 0, 0, 0 };
	if (ANCLeagueDuelStatsReplicator* Rep = FindNCLeagueDuelStatsReplicator(GetWorld()))
	{
		Counts[0] = Rep->GetBeltCountForPlayer(PlayerId);
		Counts[1] = Rep->GetVestCountForPlayer(PlayerId);
		Counts[2] = Rep->GetPadsCountForPlayer(PlayerId);
		Counts[3] = Rep->GetHelmetCountForPlayer(PlayerId);
	}
	if (Counts[0] == 0 && Counts[1] == 0 && Counts[2] == 0 && Counts[3] == 0
		&& GetWorld() && GetWorld()->GetNetMode() != NM_Client)
	{
		const auto Clamp255 = [](float V) -> uint8 {
			return uint8(FMath::Clamp(FMath::RoundToInt(V), 0, 255));
		};
		Counts[0] = Clamp255(PS->GetStatsValue(NAME_ShieldBeltCount));
		Counts[1] = Clamp255(PS->GetStatsValue(NAME_ArmorVestCount));
		Counts[2] = Clamp255(PS->GetStatsValue(NAME_ArmorPadsCount));
		Counts[3] = Clamp255(PS->GetStatsValue(NAME_HelmetCount));
	}
	// Per-player armor icons: tint WHITE so the artwork's natural colors come
	// through. The previous code passed DrawColor (the row's team color) which
	// multiplied against the icon RGB and turned the icons into dim blobs.
	DrawArmorIconRow(XOffset + (Width * ColumnHeaderEfficiencyX), YOffset + ColumnY,
		Counts, FLinearColor::White);
	// DmgPerLife column intentionally removed per user request.
}

void UNCLeagueDuelScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
	float RenderDelta, float XOffset, float YOffset)
{
	// Slim version of UUTScoreboard::DrawPlayer that omits the right-edge
	// Skill/Ping draw. Everything else (background, name, friend icon,
	// kick %, DrawPlayerScore call, strike-out, mute/talk indicators) is
	// inherited from Super by calling it. Then we paint over the right-
	// edge ping/skill text with the same row background color so it
	// disappears from view.
	//
	// Why not just override DrawPlayer wholesale: the parent body is ~120
	// lines that touch private state (SelectedPlayer, BarOpacity, FlagX,
	// etc.) and gets engine updates (mute UI, kick voting) we want to
	// inherit for free. Cover-rect over the unwanted last column is the
	// minimum-disruption hook.
	Super::DrawPlayer(Index, PlayerState, RenderDelta, XOffset, YOffset);

	if (!PlayerState || !UTHUDOwner) return;

	// Cover the right ~5% of the cell where the parent drew skill/ping.
	// Use the same row background color so the cover blends seamlessly.
	// BarOpacity in the parent's DrawPlayer is a local (not a class member)
	// so we can't read it; hardcode 0.7 which matches the engine's default
	// row opacity. If it ever doesn't match exactly the seam will only be
	// visible at the very right edge where there's nothing else to compare.
	const FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	const float CoverW = 0.06f * ScaledCellWidth;
	const float CoverX = XOffset + ScaledCellWidth - CoverW;
	const float CoverH = 0.9f * CellHeight * RenderScale;
	DrawTexture(UTHUDOwner->ScoreboardAtlas, CoverX, YOffset, CoverW, CoverH,
		149, 138, 32, 32, 0.7f, BarColor);
}

void UNCLeagueDuelScoreboard::DrawArmorIconRow(float CenterX, float CenterY,
	const uint8* Counts, FLinearColor IconTint)
{
	if (!Canvas) return;

	// Each armor cell: icon + (optional) count to the right. Arrange 4 cells
	// in a tight row centered on CenterX. Bumped to 26px icons + 18px count
	// width so the artwork is recognizable at typical scoreboard scale; was
	// 18px before (read as black dots).
	const float IconSize = 26.f * RenderScale;
	const float CountW   = 18.f * RenderScale;
	const float CellW    = IconSize + CountW + 4.f * RenderScale;
	const float TotalW   = CellW * 4.f;
	const float StartX   = CenterX - TotalW * 0.5f;
	const float IconY    = CenterY - IconSize * 0.5f;

	FArmorIconSlot* Slots[4] = {
		&GetBeltSlot(),
		&GetVestSlot(),
		&GetPadsSlot(),
		&GetHelmetSlot(),
	};

	for (int32 i = 0; i < 4; ++i)
	{
		FArmorIconSlot* S = Slots[i];
		if (!S || !ResolveArmorIcon(*S))
		{
			// Couldn't resolve the BP class (e.g. asset moved). Fall back to
			// drawing the count alone so the column isn't blank.
			if (Counts)
			{
				const FString Txt = FString::Printf(TEXT("%u"), Counts[i]);
				DrawText(FText::FromString(Txt), StartX + i * CellW + IconSize * 0.5f, CenterY,
					UTHUDOwner->TinyFont, 1.0f, 1.0f, IconTint,
					ETextHorzPos::Center, ETextVertPos::Center);
			}
			continue;
		}

		const FCanvasIcon& Icon = S->CachedIcon;
		const float CellX = StartX + i * CellW;
		DrawTexture(Icon.Texture, CellX, IconY, IconSize, IconSize,
			Icon.U, Icon.V, Icon.UL, Icon.VL, 1.f, IconTint);

		// Count text: only on per-player rows (Counts != null). Drawn to the
		// right of the icon so it doesn't crowd the icon at small sizes.
		if (Counts)
		{
			// Color: dim if 0 picks, brighter as count grows (caps at 5+).
			const uint8 C = Counts[i];
			const float Tier = FMath::Min(float(C) / 5.f, 1.f);
			FLinearColor CountColor = FMath::Lerp(
				FLinearColor(0.55f, 0.55f, 0.55f, 1.f),
				FLinearColor(1.0f,  1.0f,  1.0f,  1.f),
				Tier);
			if (C == 0) CountColor.A = 0.5f;

			const FString Txt = FString::Printf(TEXT("%u"), C);
			DrawText(FText::FromString(Txt),
				CellX + IconSize + 1.f * RenderScale, CenterY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, CountColor,
				ETextHorzPos::Left, ETextVertPos::Center);
		}
	}
}
