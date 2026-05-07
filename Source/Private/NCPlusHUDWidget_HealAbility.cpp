// NCPlusHUDWidget_HealAbility.cpp — heal/boost ability indicator.
//
// Reads:
//   AUTPlayerState::BoostClass        -> inventory CDO -> HUDIcon  (what to draw)
//   AUTPlayerState::GetRemainingBoosts() -> grey out gate           (when used up)
//   UUTPlayerInput::CustomBinds[]     -> match Command              (key label)
//
// Layout extras honoured:
//   "bind_command" (string) — the input Command to look up. Default tries
//      a small candidate list ("ActivateSpecial" -> "StartActivatePowerup"
//      -> "ToggleTranslocator") because UT4's stock keybind is registered
//      as "ActivateSpecial" (UTProfileSettings.cpp:256), but custom server
//      configs sometimes use the older boost or translocator names.
//      Set this extra to skip the fallback chain and look up exactly one.
//   "icon_size"    (float)  — design-pixel edge length, default 64.
//
// Hidden by default. Strictly Wipeout-only: hides whenever PS->BoostClass
// is null (warmup before WipeoutGame::RestartPlayer's spawn block at
// line 1300 has run, OR any mode that doesn't use the boost system —
// Duel, DM, CTF, ShockDom, NCLeagueDuel, NCShaftArena, etc). Inheriting
// HUDs (NCLeagueDuel etc) get the widget registered via subclass but
// it's a no-op there because BoostClass is never set.

#include "NCPlusHUDWidget_HealAbility.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTPlayerInput.h"
#include "UTInventory.h"
#include "Engine/Canvas.h"
#include "NCPlusHUDLayout.h"

namespace
{
	/** Scan all known input-binding tables for the first key bound to Command.
	 *  UE4 has THREE places a keybind can live, and "Q -> Activate Powerup" is
	 *  in the one we weren't checking:
	 *    1. UPlayerInput::ActionMappings    — engine-standard actions loaded
	 *       from DefaultInput.ini (StartActivatePowerup lives here)
	 *    2. UUTPlayerInput::CustomBinds     — UT4 plugin-defined commands
	 *       (ToggleTranslocator etc.)
	 *    3. UUTPlayerInput::LocalBinds      — per-machine override layer
	 *  Earlier code only checked #2 and #3, so action-mapped binds always
	 *  came back as "?". Action-mapped is the more common path for
	 *  combat/ability binds — checked first now. Returns "?" if none found,
	 *  which is still a better visual than empty (user sees they need to
	 *  bind a key OR set bind_command extras to a different action name).  */
	FString FindKeyForCommand(AUTPlayerController* PC, const FString& Command)
	{
		if (!PC || !PC->PlayerInput) return TEXT("?");
		UPlayerInput* BaseInput = PC->PlayerInput;

		// 1. ActionMappings — engine-canonical actions. ActionName must
		//    match exactly (no fuzzy StartsWith — these are clean FNames).
		const FName ActionName(*Command);
		for (const FInputActionKeyMapping& M : BaseInput->ActionMappings)
		{
			if (M.ActionName == ActionName)
			{
				FString Out = M.Key.GetFName().ToString();
				if (Out.StartsWith(TEXT("Gamepad_"))) Out = Out.RightChop(8);
				return Out;
			}
		}

		// 2 + 3. UT4 CustomBinds / LocalBinds.
		UUTPlayerInput* Input = Cast<UUTPlayerInput>(BaseInput);
		if (!Input) return TEXT("?");

		auto Search = [&Command](const TArray<FCustomKeyBinding>& Binds) -> FName
		{
			for (const FCustomKeyBinding& B : Binds)
			{
				// Loose match: handles "StartActivatePowerup" vs trailing args
				// like "StartActivatePowerup | OnReleaseActivatePowerup" some
				// configs embed via pipe.
				if (B.Command.StartsWith(Command, ESearchCase::IgnoreCase))
				{
					return B.KeyName;
				}
			}
			return NAME_None;
		};

		FName Key = Search(Input->CustomBinds);
		if (Key == NAME_None) Key = Search(Input->LocalBinds);
		if (Key == NAME_None) return TEXT("?");

		FString Out = Key.ToString();
		if (Out.StartsWith(TEXT("Gamepad_"))) Out = Out.RightChop(8);
		return Out;
	}
}

UNCPlusHUDWidget_HealAbility::UNCPlusHUDWidget_HealAbility(const FObjectInitializer& OI)
	: Super(OI)
{
	// Stock position: bottom-center, offset right of crosshair so it sits next
	// to the HP/Armor cluster without overlapping (matches the user's preferred
	// Diabotical-ish layout — heal indicator near the player, not above it).
	// Mirrored in NCPlusHUDLayout.cpp's alias StockOffset; keep them in sync
	// so "no layout entry" renders identically to a freshly-created entry.
	Position           = FVector2D(635.f, -52.f);
	Size               = FVector2D(96.f, 96.f);
	ScreenPosition     = FVector2D(0.5f, 1.0f);   // BottomCenter anchor
	Origin             = FVector2D(0.5f, 1.0f);   // pivot bottom-center
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

float UNCPlusHUDWidget_HealAbility::GetDrawScaleOverride()
{
	// Honor per-element Scale override (same pattern as QuickStats / AmmoCounter).
	// Without this the editor's Sc field has no effect on this widget.
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("heal_ability"));
	const float UserScale = E ? FMath::Clamp(E->Scale, 0.25f, 4.f) : 1.f;
	return Super::GetDrawScaleOverride() * UserScale;
}

bool UNCPlusHUDWidget_HealAbility::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!IsValid(PS) || PS->bOnlySpectator) return false;

	// Strict Wipeout-only gate. BoostClass is set in WipeoutGame::RestartPlayer
	// (WipeoutGame.cpp:1306) and replicates to clients. In any other mode
	// (Duel, DM, CTF, ShockDom, etc) it stays null and the widget hides.
	// Same for Wipeout warmup before the boost grant block runs — the player
	// will see the widget appear once the match starts.
	if (!PS->BoostClass) return false;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("heal_ability"));
	if (!E) return false;
	if (E->bHidden) return false;
	return true;
}

void UNCPlusHUDWidget_HealAbility::Draw_Implementation(float DeltaTime)
{
	if (!Canvas) return;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return;
	AUTPlayerController* PC = UTHUDOwner->UTPlayerOwner;
	AUTPlayerState* PS = PC->UTPlayerState;
	if (!IsValid(PS) || !PS->BoostClass) return;

	// Resolve icon from the boost inventory's CDO. If HUDIcon.Texture is null
	// the inventory class hasn't set one — bail rather than render garbage.
	// One-shot diagnostic so the modder knows what's missing.
	AUTInventory* InvCDO = PS->BoostClass->GetDefaultObject<AUTInventory>();
	if (!InvCDO) return;
	const FCanvasIcon& Icon = InvCDO->HUDIcon;
	if (Icon.Texture == nullptr)
	{
		static bool bLoggedNoIcon = false;
		if (!bLoggedNoIcon)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[NCPlusHUDWidget_HealAbility] %s has no HUDIcon set on its CDO — widget hidden. ")
				TEXT("Set the HUDIcon FCanvasIcon on the inventory class to render an icon."),
				*PS->BoostClass->GetName());
			bLoggedNoIcon = true;
		}
		return;
	}

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("heal_ability"));
	const float IconSize = (E ? E->GetExtraFloat(TEXT("icon_size"), 64.f) : 64.f);

	// Honor per-element opacity (0..1) — applied to icon tint AND label color.
	// UUTHUDWidget::DrawTexture's color.A path is the right one to feed into:
	// passing alpha pre-multiplied via the FLinearColor's .A is the canonical
	// way to dim a draw call. Same approach AmmoCounter / QuickStats use.
	const float Opacity = E ? FMath::Clamp(E->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;

	// Resolve the keybind. If the user pinned a specific command via the
	// layout's "bind_command" extra, look up only that. Otherwise walk the
	// canonical candidates: ActivateSpecial (stock UT4 default — see
	// UTProfileSettings.cpp:256), StartActivatePowerup (older boost name),
	// ToggleTranslocator (translocator-style heal binding). First non-"?"
	// hit wins.
	const FString BindOverride = (E ? E->GetExtra(TEXT("bind_command")) : FString());
	FString KeyLabel;
	if (!BindOverride.IsEmpty())
	{
		KeyLabel = FindKeyForCommand(PC, BindOverride);
	}
	else
	{
		static const TCHAR* Candidates[] = {
			TEXT("ActivateSpecial"),
			TEXT("StartActivatePowerup"),
			TEXT("ToggleTranslocator"),
		};
		KeyLabel = TEXT("?");
		for (const TCHAR* Cand : Candidates)
		{
			FString Found = FindKeyForCommand(PC, Cand);
			if (Found != TEXT("?")) { KeyLabel = Found; break; }
		}
	}

	const uint8 Charges = PS->GetRemainingBoosts();
	const bool bAvailable = Charges > 0;

	// Greyed when consumed: desaturate via tint RGB. Bright tint when ready.
	// IMPORTANT: UUTHUDWidget::DrawTexture overwrites color.A with the
	// explicit DrawOpacity argument — multiplying IconTint.A here would be
	// silently dropped. Combine the consumed-state dim factor with the
	// layout's opacity extra into a single DrawOpacity scalar.
	// (Same gotcha noted in hud_editor_session_may3_2026.md.)
	FLinearColor IconTint = bAvailable
		? FLinearColor(1.f, 1.f, 1.f, 1.f)
		: FLinearColor(0.35f, 0.35f, 0.35f, 1.f);
	const float ConsumedDim = bAvailable ? 1.f : 0.85f;
	const float DrawOpacity = Opacity * ConsumedDim;

	// Centered icon in widget-relative space — DrawTexture transforms by
	// the widget's origin/screen position automatically (unlike raw
	// Canvas->DrawTile which bypasses the transform and draws at absolute
	// canvas coords — the bug that put a green plus at top-left in DM).
	const float IconX = (Size.X - IconSize) * 0.5f;
	const float IconY = (Size.Y - IconSize) * 0.5f - 8.f;   // lift to leave room for keybind text

	DrawTexture(Icon.Texture, IconX, IconY, IconSize, IconSize,
		Icon.U, Icon.V, Icon.UL, Icon.VL, DrawOpacity, IconTint);

	// Keybind label below the icon. Larger font when available, dimmer
	// when the icon is greyed (mirrors the icon's state visually).
	UFont* LabelFont = UTHUDOwner->MediumFont ? UTHUDOwner->MediumFont : UTHUDOwner->SmallFont;
	// Honor per-element font override (Phase 3.8).
	LabelFont = NCPlusHUDFonts::Resolve(TEXT("heal_ability"), UTHUDOwner, LabelFont);
	if (!LabelFont) return;

	// KeyLabel was resolved above (candidate-list fallback or extras override).
	FLinearColor LabelColor = bAvailable
		? FLinearColor(1.f, 1.f, 1.f, 1.f)
		: FLinearColor(0.6f, 0.6f, 0.6f, 0.85f);
	LabelColor.A *= Opacity;

	// Prefix the keybind with "Heal:" so the label is self-explanatory at a
	// glance instead of reading as a stray letter under the icon. Even with
	// "?" as the keybind, the "Heal: ?" form makes it obvious what's missing.
	const FString DisplayLabel = FString::Printf(TEXT("Heal: %s"), *KeyLabel);
	DrawText(FText::FromString(DisplayLabel), Size.X * 0.5f, IconY + IconSize + 2.f,
		LabelFont, RenderScale, 1.0f, LabelColor,
		ETextHorzPos::Center, ETextVertPos::Top);
}
