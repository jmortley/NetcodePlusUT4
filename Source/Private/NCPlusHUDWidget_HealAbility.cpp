// NCPlusHUDWidget_HealAbility.cpp — heal/boost ability indicator.
//
// Reads:
//   AUTPlayerState::BoostClass        -> inventory CDO -> HUDIcon  (what to draw)
//   AUTPlayerState::GetRemainingBoosts() -> grey out gate           (when used up)
//   UUTPlayerInput::CustomBinds[]     -> match Command              (key label)
//
// Layout extras honoured:
//   "bind_command" (string) — the input Command to look up. Default
//      "StartActivatePowerup" (engine standard for boost). Override to
//      "ToggleTranslocator" for the user's custom heal binding.
//   "icon_size"    (float)  — design-pixel edge length, default 64.
//
// Hidden by default. Enable + reposition via the nchud editor.

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

	// No-op for modes that don't grant a boost (gracefully hides outside
	// Wipeout). Checked AFTER layout entry so the editor's eye-toggle still
	// works the user expects.
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

	AUTInventory* InvCDO = PS->BoostClass->GetDefaultObject<AUTInventory>();
	if (!InvCDO) return;
	const FCanvasIcon& Icon = InvCDO->HUDIcon;
	if (Icon.Texture == nullptr) return;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("heal_ability"));
	const float IconSize = (E ? E->GetExtraFloat(TEXT("icon_size"), 64.f) : 64.f);
	const FString BindCmd = (E ? E->GetExtra(TEXT("bind_command")) : FString());
	const FString EffectiveCmd = BindCmd.IsEmpty() ? FString(TEXT("StartActivatePowerup")) : BindCmd;

	const uint8 Charges = PS->GetRemainingBoosts();
	const bool bAvailable = Charges > 0;

	// Greyed when consumed: keep at full opacity so the icon shape stays
	// recognizable, but desaturate and dim. Bright tint when ready.
	const FLinearColor IconTint = bAvailable
		? FLinearColor(1.f, 1.f, 1.f, 1.f)
		: FLinearColor(0.35f, 0.35f, 0.35f, 0.85f);

	// Centered icon. Size.X * 0.5f puts the X anchor at our box midpoint;
	// Origin already places the box bottom-center at the screen anchor.
	const float IconX = (Size.X - IconSize) * 0.5f;
	const float IconY = (Size.Y - IconSize) * 0.5f - 8.f;   // lift to leave room for keybind text

	DrawTexture(Icon.Texture, IconX, IconY, IconSize, IconSize,
		Icon.U, Icon.V, Icon.UL, Icon.VL, 1.f, IconTint);

	// Keybind label below the icon. Larger font when available, dimmer
	// when the icon is greyed (mirrors the icon's state visually).
	UFont* LabelFont = UTHUDOwner->MediumFont ? UTHUDOwner->MediumFont : UTHUDOwner->SmallFont;
	if (!LabelFont) return;

	const FString KeyLabel = FindKeyForCommand(PC, EffectiveCmd);
	const FLinearColor LabelColor = bAvailable
		? FLinearColor(1.f, 1.f, 1.f, 1.f)
		: FLinearColor(0.6f, 0.6f, 0.6f, 0.85f);

	DrawText(FText::FromString(KeyLabel), Size.X * 0.5f, IconY + IconSize + 2.f,
		LabelFont, RenderScale, 1.0f, LabelColor,
		ETextHorzPos::Center, ETextVertPos::Top);
}
