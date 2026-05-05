// NCPlusHUDWidget_HealAbility.cpp — heal/boost ability indicator.
//
// Reads:
//   AUTPlayerState::BoostClass        -> inventory CDO -> HUDIcon  (preferred icon source)
//   AUTPlayerState::GetRemainingBoosts() -> grey out gate          (when used up)
//   UUTPlayerInput::CustomBinds[]     -> match Command             (key label)
//
// Layout extras honoured:
//   "bind_command" (string) — the input Command to look up. Default
//      "StartActivatePowerup" (engine standard for boost). Override to
//      "ToggleTranslocator" for the user's custom heal binding.
//   "icon_size"    (float)  — design-pixel edge length, default 64.
//
// Hidden by default. Enable + reposition via the nchud editor.
//
// Warmup behavior: WipeoutGame::RestartPlayer early-returns on
// MatchState::WaitingToStart (WipeoutGame.cpp:1218-1222), so the boost
// grant block at line 1300+ never runs and PS->BoostClass stays null. The
// widget keeps rendering anyway with a placeholder green-cross icon so
// the user can still see the bound key during warm-up. Once the match
// starts and the spawn block runs, the icon swaps to the real heal
// inventory's HUDIcon.

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
	/** Scan CustomBinds + LocalBinds for the first key bound to Command.
	 *  Returns "?" if the action isn't bound — better visual than empty so
	 *  the user immediately sees they need to bind a key.  */
	FString FindKeyForCommand(AUTPlayerController* PC, const FString& Command)
	{
		if (!PC || !PC->PlayerInput) return TEXT("?");
		UUTPlayerInput* Input = Cast<UUTPlayerInput>(PC->PlayerInput);
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

		// FName already comes through as a clean key name (e.g. "Q", "MouseX").
		// Strip the verbose engine prefixes the player would never want to see.
		FString Out = Key.ToString();
		if (Out.StartsWith(TEXT("Gamepad_"))) Out = Out.RightChop(8);
		return Out;
	}

	/** Draw a green plus-sign placeholder when no inventory icon is available
	 *  (warm-up before BoostClass is granted, or BoostClass with empty
	 *  HUDIcon.Texture). Universally readable as "heal", visually distinct
	 *  from the real inventory icon so the user knows it's a placeholder. */
	void DrawPlaceholderHealIcon(UCanvas* Canvas, float X, float Y, float Sz, const FLinearColor& Tint)
	{
		if (!Canvas || !Canvas->DefaultTexture) return;
		const float Cx = X + Sz * 0.5f;
		const float Cy = Y + Sz * 0.5f;
		const float Thick = Sz * 0.22f;
		const float Len   = Sz * 0.78f;
		const FLinearColor Green(0.35f, 0.95f, 0.45f, 1.f);
		const FLinearColor Final = Green * Tint;
		// Vertical bar
		Canvas->SetLinearDrawColor(Final);
		Canvas->DrawTile(Canvas->DefaultTexture, Cx - Thick * 0.5f, Cy - Len * 0.5f,
			Thick, Len, 0, 0, 1, 1);
		// Horizontal bar
		Canvas->DrawTile(Canvas->DefaultTexture, Cx - Len * 0.5f, Cy - Thick * 0.5f,
			Len, Thick, 0, 0, 1, 1);
	}
}

UNCPlusHUDWidget_HealAbility::UNCPlusHUDWidget_HealAbility(const FObjectInitializer& OI)
	: Super(OI)
{
	// Stock position: bottom-center, lifted a bit so it doesn't overlap the
	// HP/Armor cluster. Diabotical places it to the right of the player
	// model — but for arbitrary HUDs the safe default is dead-center bottom
	// where it's easy to spot during combat. User can drag elsewhere via nchud.
	Position           = FVector2D(0.f, -180.f);
	Size               = FVector2D(96.f, 96.f);
	ScreenPosition     = FVector2D(0.5f, 1.0f);   // BottomCenter anchor
	Origin             = FVector2D(0.5f, 1.0f);   // pivot bottom-center
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

bool UNCPlusHUDWidget_HealAbility::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!IsValid(PS) || PS->bOnlySpectator) return false;

	// Render whenever the user has placed the widget via the nchud editor
	// (no implicit gating on BoostClass — see warmup note in file header).
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
	if (!IsValid(PS)) return;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("heal_ability"));
	const float IconSize = (E ? E->GetExtraFloat(TEXT("icon_size"), 64.f) : 64.f);
	const FString BindCmd = (E ? E->GetExtra(TEXT("bind_command")) : FString());
	const FString EffectiveCmd = BindCmd.IsEmpty() ? FString(TEXT("StartActivatePowerup")) : BindCmd;

	// Charge state. Three cases:
	//   1. BoostClass null (warmup, pre-spawn)                   → show placeholder, treat as "available" (no charge known yet)
	//   2. BoostClass set + RemainingBoosts > 0 (active, ready)  → show real icon, bright
	//   3. BoostClass set + RemainingBoosts == 0 (just used)     → show real icon, greyed
	const bool bHasBoostInfo = (PS->BoostClass != nullptr);
	const bool bUsed         = bHasBoostInfo && PS->GetRemainingBoosts() == 0;

	const FLinearColor IconTint = bUsed
		? FLinearColor(0.35f, 0.35f, 0.35f, 0.85f)
		: FLinearColor(1.f, 1.f, 1.f, 1.f);

	// Centered icon. Size.X * 0.5f puts the X anchor at our box midpoint;
	// Origin already places the box bottom-center at the screen anchor.
	const float IconX = (Size.X - IconSize) * 0.5f;
	const float IconY = (Size.Y - IconSize) * 0.5f - 8.f;   // lift to leave room for keybind text

	bool bDrewRealIcon = false;
	if (bHasBoostInfo)
	{
		AUTInventory* InvCDO = PS->BoostClass->GetDefaultObject<AUTInventory>();
		if (InvCDO && InvCDO->HUDIcon.Texture)
		{
			const FCanvasIcon& Icon = InvCDO->HUDIcon;
			DrawTexture(Icon.Texture, IconX, IconY, IconSize, IconSize,
				Icon.U, Icon.V, Icon.UL, Icon.VL, 1.f, IconTint);
			bDrewRealIcon = true;
		}
	}
	if (!bDrewRealIcon)
	{
		// Warmup pre-grant OR the configured ability inventory has no
		// HUDIcon set. Drop in a placeholder so the user still sees a
		// recognizable heal indicator + their bind. One-shot diagnostic
		// log helps debug "why is this a placeholder" without spamming.
		static bool bLoggedPlaceholder = false;
		if (!bLoggedPlaceholder)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[NCPlusHUDWidget_HealAbility] Drawing placeholder icon: BoostClass=%s, HUDIcon.Texture=%s. ")
				TEXT("Expected during warmup (WipeoutGame::RestartPlayer skips boost grant in WaitingToStart). ")
				TEXT("If still placeholder mid-match, the ability inventory class needs HUDIcon set."),
				bHasBoostInfo ? *PS->BoostClass->GetName() : TEXT("(null)"),
				(bHasBoostInfo && PS->BoostClass->GetDefaultObject<AUTInventory>() && PS->BoostClass->GetDefaultObject<AUTInventory>()->HUDIcon.Texture)
					? TEXT("set") : TEXT("null"));
			bLoggedPlaceholder = true;
		}
		DrawPlaceholderHealIcon(Canvas, IconX, IconY, IconSize, IconTint);
	}

	// Keybind label below the icon.
	UFont* LabelFont = UTHUDOwner->MediumFont ? UTHUDOwner->MediumFont : UTHUDOwner->SmallFont;
	// Honor per-element font override (Phase 3.8).
	LabelFont = NCPlusHUDFonts::Resolve(TEXT("heal_ability"), UTHUDOwner, LabelFont);
	if (!LabelFont) return;

	const FString KeyLabel = FindKeyForCommand(PC, EffectiveCmd);
	const FLinearColor LabelColor = bUsed
		? FLinearColor(0.6f, 0.6f, 0.6f, 0.85f)
		: FLinearColor(1.f, 1.f, 1.f, 1.f);

	DrawText(FText::FromString(KeyLabel), Size.X * 0.5f, IconY + IconSize + 2.f,
		LabelFont, RenderScale, 1.0f, LabelColor,
		ETextHorzPos::Center, ETextVertPos::Top);
}
