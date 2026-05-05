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
#include "UTBot.h"
#include "UTHUD.h"
#include "Engine/Canvas.h"
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
	// Direct copy of UUTScoreboard::DrawPlayer, with the bot-skill / ping
	// right-edge draw block (parent's UTScoreboard.cpp:785-798) OMITTED.
	// Duel scoreboard ends with the Armors column - no skill/ping at the
	// far right. Earlier attempt was a cover-rect over the parent's draw
	// but that double-blended opacities and showed up as a black smear on
	// non-local players' rows; the only clean solution is to skip the
	// inherited draws outright.
	//
	// Everything else is preserved: background tile, kick %, flag, name,
	// friend icon, DrawPlayerScore, ready-text, strike-out, mute/talk
	// indicators. Engine fixes to those land for free via parent edits.
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
	float MaxNameWidth = 0.42f*ScaledCellWidth - (PlayerState->bIsFriend ? 30.f*RenderScale : 0.f);
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	UFont* NameFont = UTHUDOwner->SmallFont;
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);

	if (UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState)
	{
		BarOpacity = 0.5f;
	}

	// Background border.
	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor)
	{
		BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0);
		FinalBarOpacity = 0.75f;
	}
	if (PlayerState == SelectedPlayer)
	{
		BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0);
		FinalBarOpacity = 0.75f;
	}

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

	// >>> Skill / Ping right-edge draw OMITTED on purpose. <<<

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

	// Muted indicator.
	if (UTHUDOwner->UTPlayerOwner->IsPlayerGameMuted(PlayerState))
	{
		bool bLeft = (XOffset < Canvas->ClipX * 0.5f);
		float TalkingXOffset = bLeft ? ScaledCellWidth + (10.0f *RenderScale) : (-36.0f * RenderScale);
		FTextureUVs ChatIconUVs = bLeft
			? FTextureUVs(497.0f, 965.0f, 35.0f, 31.0f)
			: FTextureUVs(532.0f, 965.0f, -35.0f, 31.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset,
			YOffset + ((CellHeight * 0.5f - 24.0f) * RenderScale),
			(26 * RenderScale), (23 * RenderScale),
			ChatIconUVs.U, ChatIconUVs.V, ChatIconUVs.UL, ChatIconUVs.VL, 1.0f);

		FTextureUVs MuteIconUVs = FTextureUVs(410.0f, 942.0f, 64.0f, 64.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset - 3.f,
			YOffset + ((CellHeight * 0.5f - 30.0f) * RenderScale),
			(32 * RenderScale), (32 * RenderScale),
			MuteIconUVs.U, MuteIconUVs.V, MuteIconUVs.UL, MuteIconUVs.VL,
			1.0f, FLinearColor::Red);
	}
	// Talking indicator.
	else if (PlayerState->bIsTalking)
	{
		bool bLeft = (XOffset < Canvas->ClipX * 0.5f);
		float TalkingXOffset = bLeft ? ScaledCellWidth + (10.0f *RenderScale) : (-36.0f * RenderScale);
		FTextureUVs ChatIconUVs = bLeft
			? FTextureUVs(497.0f, 965.0f, 35.0f, 31.0f)
			: FTextureUVs(532.0f, 965.0f, -35.0f, 31.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset,
			YOffset + ((CellHeight * 0.5f - 24.0f) * RenderScale),
			(26 * RenderScale), (23 * RenderScale),
			ChatIconUVs.U, ChatIconUVs.V, ChatIconUVs.UL, ChatIconUVs.VL, 1.0f);
	}
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
