// Custom split WeaponBar — implementation.
#include "NCPlusHUDWidget_WeaponBar.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTInventory.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

namespace NCPlusWB
{
	// Slot dimensions (1080p design pixels)
	static const float SlotW       = 76.f;
	static const float SlotH       = 60.f;
	static const float SlotGap     = 6.f;
	static const float SlotPad     = 6.f;       // inner padding for icon
	static const float AmmoBarH    = 4.f;
	static const float GroupNumPad = 3.f;

	// Default colors (overridable per-element via Extras keys in Phase 3.3).
	// Lower alphas + lighter inactive bg to match stock UT4 weapon bar's translucent feel.
	static const FLinearColor SlotBgInactive(0.04f, 0.04f, 0.04f, 0.30f);
	static const FLinearColor SlotBgActive  (0.10f, 0.10f, 0.10f, 0.55f);
	static const FLinearColor ActiveOutline (0.95f, 0.83f, 0.34f, 1.f); // amber accent
	static const FLinearColor AmmoFillFull  (0.4f,  0.95f, 0.48f, 1.f);
	static const FLinearColor AmmoFillWarn  (1.0f,  0.85f, 0.30f, 1.f);
	static const FLinearColor AmmoFillDanger(1.0f,  0.32f, 0.28f, 1.f);

	struct FSharedInventoryCache
	{
		TWeakObjectPtr<AUTCharacter> Character;
		uint32 LayoutRevision = 0;
		uint64 SampledFrame = MAX_uint64;
		TArray<TWeakObjectPtr<AUTWeapon>> Observed;
		TArray<TWeakObjectPtr<AUTWeapon>> ClassifiedObserved;
		TArray<int32> ClassifiedGroups;
		TArray<TWeakObjectPtr<AUTWeapon>> Left;
		TArray<TWeakObjectPtr<AUTWeapon>> Right;
	};

	static FSharedInventoryCache GInventoryCache;

	static const TArray<TWeakObjectPtr<AUTWeapon>>& GetWeaponsForSide(
		AUTCharacter* Character, const FNCPlusHUDLayout& Layout, uint32 LayoutRevision, int32 SideIndex)
	{
		FSharedInventoryCache& Cache = GInventoryCache;
		if (Cache.SampledFrame != GFrameCounter || Cache.Character.Get() != Character
			|| Cache.LayoutRevision != LayoutRevision)
		{
			Cache.SampledFrame = GFrameCounter;
			Cache.Observed.Reset();
			for (TInventoryIterator<AUTWeapon> It(Character); It; ++It)
			{
				AUTWeapon* W = *It;
				if (!W || W->IsPendingKill() || !W->GetClass()) continue;
				Cache.Observed.Add(W);
			}
			bool bInventoryChanged = Cache.ClassifiedObserved.Num() != Cache.Observed.Num();
			for (int32 i = 0; !bInventoryChanged && i < Cache.Observed.Num(); ++i)
			{
				AUTWeapon* W = Cache.Observed[i].Get();
				bInventoryChanged = Cache.ClassifiedObserved[i] != Cache.Observed[i]
					|| !Cache.ClassifiedGroups.IsValidIndex(i) || !W || Cache.ClassifiedGroups[i] != W->Group;
			}
			if (Cache.Character.Get() != Character || Cache.LayoutRevision != LayoutRevision
				|| bInventoryChanged)
			{
				Cache.Character = Character;
				Cache.LayoutRevision = LayoutRevision;
				Cache.ClassifiedObserved = Cache.Observed;
				Cache.ClassifiedGroups.Reset(Cache.Observed.Num());
				for (const TWeakObjectPtr<AUTWeapon>& WeakWeapon : Cache.Observed)
				{
					AUTWeapon* W = WeakWeapon.Get();
					Cache.ClassifiedGroups.Add(W ? W->Group : MIN_int32);
				}
				Cache.Left.Reset();
				Cache.Right.Reset();
				static const FName NAME_Left(TEXT("left"));
				for (const TWeakObjectPtr<AUTWeapon>& WeakWeapon : Cache.Observed)
				{
					AUTWeapon* W = WeakWeapon.Get();
					if (!W) continue;
					(Layout.GetWeaponSide(W->GetClass()) == NAME_Left ? Cache.Left : Cache.Right).Add(W);
				}
				auto SortByGroup = [](const TWeakObjectPtr<AUTWeapon>& A, const TWeakObjectPtr<AUTWeapon>& B)
				{
					const AUTWeapon* WA = A.Get();
					const AUTWeapon* WB = B.Get();
					return WA && WB ? WA->Group < WB->Group : WB != nullptr;
				};
				Cache.Left.Sort(SortByGroup);
				Cache.Right.Sort(SortByGroup);
			}
		}
		return SideIndex == 0 ? Cache.Left : Cache.Right;
	}
}

// =============================================================================
// Constructors
// =============================================================================

UNCPlusHUDWidget_WeaponBar::UNCPlusHUDWidget_WeaponBar(const FObjectInitializer& OI)
	: Super(OI)
	, SideIndex(0)
	, WeaponIconAtlas(nullptr)
	, CachedStyleRevision(0)
	, bCachedVertical(true)
	, CachedOpacity(1.f)
	, CachedSlotBgInactive(NCPlusWB::SlotBgInactive)
	, CachedSlotBgActive(NCPlusWB::SlotBgActive)
	, CachedActiveOutline(NCPlusWB::ActiveOutline)
	, CachedAmmoFillFull(NCPlusWB::AmmoFillFull)
	, CachedAmmoFillWarn(NCPlusWB::AmmoFillWarn)
	, CachedAmmoFillDanger(NCPlusWB::AmmoFillDanger)
{
	// Narrow vertical box — slots stack downward from the top-left of the widget.
	// Size matches a single column so anchoring (e.g. CenterRight) lands the
	// strip flush against the screen edge instead of leaving a gap.
	Position           = FVector2D(0.f, 0.f);
	Size               = FVector2D(80.f, 540.f);
	ScreenPosition     = FVector2D(0.5f, 1.0f);
	Origin             = FVector2D(0.5f, 1.0f);
	DesignedResolution = 1080.f;

	// Don't apply HUDImpulse to the weapon bar. Stock UUTHUDWidget::PreDraw shifts
	// Origin by AUTHUD::CurrentHUDImpulse whenever bShouldKickBack is true (default),
	// causing the bar to jolt sideways on every fire. The impulse magnitude is
	// per-weapon: sniper uses base UTWeapon's default (0.03, 0.1); flak overrides
	// to (0, 0.2) — both noticeable on a tall side-anchored strip. Crosshair,
	// scoreboard, and radial menu all opt out for the same reason.
	bShouldKickBack = false;

	// Stock weapon-icon atlas — WeaponBarSelectedUVs are pixel coords into THIS,
	// not HUDAtlas. Match stock UTHUDWidget_WeaponBar's exact path format
	// (Texture2D'...' wrapper). Falls back to a lazy LoadObject in Draw if this
	// CDO-time load returns null (FObjectFinder can be flaky in some load paths).
	static ConstructorHelpers::FObjectFinder<UTexture2D> WeaponAtlasFinder(TEXT("Texture2D'/Game/RestrictedAssets/UI/WeaponAtlas01.WeaponAtlas01'"));
	WeaponIconAtlas = WeaponAtlasFinder.Object;
}

UNCPlusHUDWidget_WeaponBar_Left::UNCPlusHUDWidget_WeaponBar_Left(const FObjectInitializer& OI)
	: Super(OI)
{
	SideIndex = 0;
	// Different default placement so the two strips don't overlap out-of-the-box.
	ScreenPosition = FVector2D(0.0f, 1.0f);   // BottomLeft
	Origin         = FVector2D(0.0f, 1.0f);
	Position       = FVector2D(20.f, -20.f);
}

UNCPlusHUDWidget_WeaponBar_Right::UNCPlusHUDWidget_WeaponBar_Right(const FObjectInitializer& OI)
	: Super(OI)
{
	SideIndex = 1;
	ScreenPosition = FVector2D(1.0f, 1.0f);   // BottomRight
	Origin         = FVector2D(1.0f, 1.0f);
	Position       = FVector2D(-20.f, -20.f);
}

// =============================================================================
// ShouldDraw / Draw
// =============================================================================

float UNCPlusHUDWidget_WeaponBar::GetDrawScaleOverride()
{
	// Per-side alias: weapon_bar_left for SideIndex 0, weapon_bar_right for 1.
	const FName Alias = (SideIndex == 0) ? FName(TEXT("weapon_bar_left")) : FName(TEXT("weapon_bar_right"));
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	const float UserScale = E ? FMath::Clamp(E->Scale, 0.25f, 4.f) : 1.f;
	return Super::GetDrawScaleOverride() * UserScale;
}

bool UNCPlusHUDWidget_WeaponBar::ShouldDraw_Implementation(bool bShowScores)
{
	if (!Super::ShouldDraw_Implementation(bShowScores)) return false;
	if (!UTHUDOwner || !UTHUDOwner->UTPlayerOwner) return false;
	if (UTHUDOwner->bShowComsMenu || UTHUDOwner->bShowWeaponWheel) return false;

	// Don't draw during the post-match lineup. Characters are being spawned
	// and inventories populated mid-tick — TInventoryIterator can hit stale
	// pointers and crash. Wait until the new round is fully running.
	if (UTGameState && UTGameState->IsLineUpActive()) return false;

	return true;
}

void UNCPlusHUDWidget_WeaponBar::Draw_Implementation(float DeltaTime)
{
	using namespace NCPlusWB;

	// Hard validity gate — alt-tab / world transitions can leave pointers stale.
	if (!UTHUDOwner || UTHUDOwner->IsPendingKill()) return;
	if (!UTHUDOwner->UTPlayerOwner || UTHUDOwner->UTPlayerOwner->IsPendingKill()) return;
	if (!Canvas) return;

	AActor* ViewTarget = UTHUDOwner->UTPlayerOwner->GetViewTarget();
	if (!ViewTarget || ViewTarget->IsPendingKill()) return;
	AUTCharacter* Char = Cast<AUTCharacter>(ViewTarget);
	if (!Char || Char->IsDead()) return;

	// Lazy-load the weapon atlas if the CDO-time FObjectFinder failed
	// (happens occasionally — see feedback_editorplus_crash memory note).
	if (!WeaponIconAtlas)
	{
		WeaponIconAtlas = LoadObject<UTexture2D>(nullptr, TEXT("/Game/RestrictedAssets/UI/WeaponAtlas01.WeaponAtlas01"));
	}

	// One-shot diagnostic so we can see what state the atlas is in.
	static bool bLoggedAtlas = false;
	if (!bLoggedAtlas)
	{
		bLoggedAtlas = true;
		if (!WeaponIconAtlas)
		{
			UE_LOG(LogTemp, Warning, TEXT("[NCPlusWB] WeaponIconAtlas LOAD FAILED — both FObjectFinder and LoadObject returned null."));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[NCPlusWB] WeaponIconAtlas loaded OK (%dx%d). Side=%d"),
				int32(WeaponIconAtlas->GetSurfaceWidth()), int32(WeaponIconAtlas->GetSurfaceHeight()), SideIndex);
		}
	}

	const FNCPlusHUDLayout& Layout = FNCPlusHUDLayout::GetLive();
	const uint32 LayoutRevision = FNCPlusHUDLayout::GetLiveRevision();

	if (CachedStyleRevision != LayoutRevision)
	{
		const FName MyAlias = (SideIndex == 0) ? FName(TEXT("weapon_bar_left")) : FName(TEXT("weapon_bar_right"));
		const FNCPlusHUDElement* Elem = Layout.Find(MyAlias);
		CachedStyleRevision = LayoutRevision;
		bCachedVertical = true;
		CachedOpacity = Elem ? FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;
		if (Elem)
		{
			const FString OrientStr = Elem->GetExtra(TEXT("orientation"));
			bCachedVertical = !OrientStr.Equals(TEXT("Horizontal"), ESearchCase::IgnoreCase);
		}
		auto ResolveColor = [Elem, this](FName Key, const FLinearColor& Default) -> FLinearColor
		{
			FLinearColor Out = Elem ? Elem->GetExtraColor(Key, Default) : Default;
			Out.A *= CachedOpacity;
			return Out;
		};
		CachedSlotBgInactive = ResolveColor(TEXT("color_slot_bg_inactive"), SlotBgInactive);
		CachedSlotBgActive   = ResolveColor(TEXT("color_slot_bg_active"), SlotBgActive);
		CachedActiveOutline  = ResolveColor(TEXT("color_outline"), ActiveOutline);
		CachedAmmoFillFull   = ResolveColor(TEXT("color_ammo_full"), AmmoFillFull);
		CachedAmmoFillWarn   = ResolveColor(TEXT("color_ammo_warn"), AmmoFillWarn);
		CachedAmmoFillDanger = ResolveColor(TEXT("color_ammo_danger"), AmmoFillDanger);
	}
	const bool bVertical = bCachedVertical;
	const float Opacity = CachedOpacity;
	const FLinearColor SlotBgInactiveCol = CachedSlotBgInactive;
	const FLinearColor SlotBgActiveCol   = CachedSlotBgActive;
	const FLinearColor ActiveOutlineCol  = CachedActiveOutline;
	const FLinearColor AmmoFillFullCol   = CachedAmmoFillFull;
	const FLinearColor AmmoFillWarnCol   = CachedAmmoFillWarn;
	const FLinearColor AmmoFillDangerCol = CachedAmmoFillDanger;
	if (Opacity <= 0.001f) return;

	// Active weapon — pending swap takes priority over current.
	AUTWeapon* CurrentWeapon = Char->GetPendingWeapon();
	if (!CurrentWeapon) CurrentWeapon = Char->GetWeapon();

	// The two side widgets share one inventory sample per frame. Classification
	// and sorting rebuild only when the pawn, inventory signature, or layout
	// revision changes.
	const TArray<TWeakObjectPtr<AUTWeapon>>& MyWeapons =
		GetWeaponsForSide(Char, Layout, LayoutRevision, SideIndex);
	if (MyWeapons.Num() == 0) return;

	UFont* GroupFont = UTHUDOwner->TinyFont;
	UFont* AmmoFont  = UTHUDOwner->SmallFont;

	// Draw cells
	for (int32 i = 0; i < MyWeapons.Num(); i++)
	{
		AUTWeapon* W = MyWeapons[i].Get();
		if (!W || W->IsPendingKill()) continue;

		const float SlotX = bVertical ? 0.f : i * (SlotW + SlotGap);
		const float SlotY = bVertical ? i * (SlotH + SlotGap) : 0.f;
		const bool  bActive = (W == CurrentWeapon);

		// Background cell — alpha goes through DrawOpacity (10th arg).
		const FLinearColor Bg = bActive ? SlotBgActiveCol : SlotBgInactiveCol;
		DrawTexture(Canvas->DefaultTexture, SlotX, SlotY, SlotW, SlotH,
			0.f, 0.f, 1.f, 1.f, Bg.A, Bg);

		// Active outline (1px frame)
		if (bActive)
		{
			const float OL = ActiveOutlineCol.A;
			DrawTexture(Canvas->DefaultTexture, SlotX, SlotY, SlotW, 1.5f,            0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, SlotX, SlotY + SlotH - 1.5f, SlotW, 1.5f, 0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, SlotX, SlotY, 1.5f, SlotH,            0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, SlotX + SlotW - 1.5f, SlotY, 1.5f, SlotH, 0,0,1,1, OL, ActiveOutlineCol);
		}

		// Weapon icon — UVs from WeaponBarSelectedUVs are pixel coords against
		// WeaponIconAtlas. Note: UUTHUDWidget::DrawTexture normalizes the U/V/UL/VL
		// arguments INTERNALLY by texture size, so we pass the raw pixel values
		// (NOT pre-normalized) — pre-dividing here would double-divide and shrink
		// the sample region to a single texel (= invisible).
		if (WeaponIconAtlas && !WeaponIconAtlas->IsPendingKill())
		{
			const FTextureUVs& UV = W->WeaponBarSelectedUVs;
			FLinearColor IconCol = (UTHUDOwner->GetUseWeaponColors())
				? W->IconColor
				: FLinearColor::White;

			// Dim if not active and dim further if no ammo.
			float Alpha = bActive ? 1.0f : 0.55f;
			if (W->NeedsAmmoDisplay() && W->Ammo <= 0)
			{
				Alpha *= 0.45f;
			}
			IconCol.A = Alpha * Opacity;

			// Fit icon into slot (preserve aspect ratio, leave room for ammo bar).
			const float MaxIconW = SlotW - SlotPad * 2.f;
			const float MaxIconH = SlotH - SlotPad * 2.f - AmmoBarH - 2.f;
			float IconW = UV.UL;
			float IconH = UV.VL;
			if (IconW > 0.f && IconH > 0.f)
			{
				const float ScaleW = MaxIconW / IconW;
				const float ScaleH = MaxIconH / IconH;
				const float Scale  = FMath::Min(ScaleW, ScaleH);
				IconW *= Scale;
				IconH *= Scale;
			}
			else
			{
				IconW = MaxIconW;
				IconH = MaxIconH;
			}

			const float IconX = SlotX + (SlotW - IconW) * 0.5f;
			const float IconY = SlotY + SlotPad;
			DrawTexture(WeaponIconAtlas, IconX, IconY, IconW, IconH,
				UV.U, UV.V, UV.UL, UV.VL,   // raw pixel coords
				IconCol.A, IconCol);
		}

		// Group number top-left
		if (GroupFont && W->Group > 0)
		{
			DrawText(FText::AsNumber(W->Group), SlotX + GroupNumPad, SlotY + GroupNumPad,
				GroupFont, FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.6f),
				1.0f, (bActive ? 1.0f : 0.7f) * Opacity, FLinearColor::White,
				ETextHorzPos::Left, ETextVertPos::Top);
		}

		// Ammo bar + count (only for weapons that display ammo)
		if (W->NeedsAmmoDisplay() && W->MaxAmmo > 0)
		{
			const float AmmoBarY = SlotY + SlotH - AmmoBarH - 2.f;
			const float AmmoBarW = SlotW - SlotPad * 2.f;
			const float AmmoBarX = SlotX + SlotPad;

			// Track
			DrawTexture(Canvas->DefaultTexture, AmmoBarX, AmmoBarY, AmmoBarW, AmmoBarH,
				0,0,1,1, 0.8f * Opacity, FLinearColor(0.05f, 0.05f, 0.05f, 1.f));

			// Fill
			const float AmmoFrac = FMath::Clamp(float(W->Ammo) / float(W->MaxAmmo), 0.f, 1.f);
			FLinearColor FillCol = AmmoFillFullCol;
			if (W->Ammo <= W->AmmoDangerAmount)       FillCol = AmmoFillDangerCol;
			else if (W->Ammo <= W->AmmoWarningAmount) FillCol = AmmoFillWarnCol;
			DrawTexture(Canvas->DefaultTexture, AmmoBarX, AmmoBarY, AmmoBarW * AmmoFrac, AmmoBarH,
				0,0,1,1, FillCol.A, FillCol);

			// Ammo count text top-right
			if (AmmoFont)
			{
				DrawText(FText::AsNumber(W->Ammo), SlotX + SlotW - GroupNumPad, SlotY + GroupNumPad,
					AmmoFont, FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.6f),
					0.85f, (bActive ? 1.0f : 0.75f) * Opacity, FillCol,
					ETextHorzPos::Right, ETextVertPos::Top);
			}
		}
	}
}
