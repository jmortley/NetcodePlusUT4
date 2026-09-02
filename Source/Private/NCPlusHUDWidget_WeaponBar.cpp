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
#include "CanvasItem.h"
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
	static const FLinearColor TextShadow(0.f, 0.f, 0.f, 0.6f);
	static const FLinearColor AmmoTrack(0.05f, 0.05f, 0.05f, 1.f);
	static const FName NAME_Left(TEXT("left"));
	static const FName NAME_WeaponBarLeft(TEXT("weapon_bar_left"));
	static const FName NAME_WeaponBarRight(TEXT("weapon_bar_right"));
	static const float InventorySampleInterval = 0.05f; // 20 Hz; pickups tolerate one HUD-frame bucket.

	// FText::AsNumber allocates formatting state. Weapon groups and ammo values
	// repeat for thousands of render frames, so retain one immutable FText per
	// observed integer instead of rebuilding it at render rate.
	struct FNumberTextCache
	{
		FText Text;
		FString String;
		TMap<UFont*, FVector2D> Sizes;
	};

	static FNumberTextCache& NumberText(int32 Value)
	{
		static TMap<int32, FNumberTextCache> Cache;
		if (FNumberTextCache* Found = Cache.Find(Value))
		{
			return *Found;
		}
		FNumberTextCache Entry;
		Entry.Text = FText::AsNumber(Value);
		Entry.String = Entry.Text.ToString();
		return Cache.Add(Value, MoveTemp(Entry));
	}

	struct FSharedInventoryCache
	{
		TWeakObjectPtr<AUTCharacter> Character;
		uint32 LayoutRevision = 0;
		uint64 SampledFrame = MAX_uint64;
		float NextSampleTime = 0.f;
		TArray<TWeakObjectPtr<AUTWeapon>> Observed;
		TArray<TWeakObjectPtr<AUTWeapon>> ClassifiedObserved;
		TArray<int32> ClassifiedGroups;
		TArray<TWeakObjectPtr<AUTWeapon>> Left;
		TArray<TWeakObjectPtr<AUTWeapon>> Right;
	};

	static FSharedInventoryCache GInventoryCache;

	// Snapshot the render-rate state for one weapon before submitting any Canvas
	// items. Slots are separated by SlotGap and their icon/text/bar bounds do not
	// overlap, so drawing compatible resources in layers preserves the pixels and
	// intra-slot ordering while avoiding a texture/font state cycle per weapon.
	struct FSlotDrawRecord
	{
		float SlotX;
		float SlotY;
		FLinearColor BackgroundColor;
		bool bDrawOutline;

		bool bDrawIcon;
		float IconX;
		float IconY;
		float IconW;
		float IconH;
		float IconU;
		float IconV;
		float IconUL;
		float IconVL;
		FLinearColor IconColor;

		bool bDrawGroup;
		int32 Group;
		float GroupOpacity;

		bool bDrawAmmo;
		float AmmoBarX;
		float AmmoBarY;
		float AmmoBarW;
		float AmmoFillW;
		FLinearColor AmmoFillColor;
		int32 Ammo;
		float AmmoTextOpacity;

		FSlotDrawRecord()
			: SlotX(0.f)
			, SlotY(0.f)
			, BackgroundColor(0.f, 0.f, 0.f, 0.f)
			, bDrawOutline(false)
			, bDrawIcon(false)
			, IconX(0.f)
			, IconY(0.f)
			, IconW(0.f)
			, IconH(0.f)
			, IconU(0.f)
			, IconV(0.f)
			, IconUL(0.f)
			, IconVL(0.f)
			, IconColor(0.f, 0.f, 0.f, 0.f)
			, bDrawGroup(false)
			, Group(0)
			, GroupOpacity(0.f)
			, bDrawAmmo(false)
			, AmmoBarX(0.f)
			, AmmoBarY(0.f)
			, AmmoBarW(0.f)
			, AmmoFillW(0.f)
			, AmmoFillColor(0.f, 0.f, 0.f, 0.f)
			, Ammo(0)
			, AmmoTextOpacity(0.f)
		{
		}
	};

	static const TArray<TWeakObjectPtr<AUTWeapon>>& GetWeaponsForSide(
		AUTCharacter* Character, const FNCPlusHUDLayout& Layout, uint32 LayoutRevision,
		int32 SideIndex, float Now)
	{
		FSharedInventoryCache& Cache = GInventoryCache;
		const bool bCacheIdentityChanged = Cache.Character.Get() != Character
			|| Cache.LayoutRevision != LayoutRevision;
		if (bCacheIdentityChanged || (Cache.SampledFrame != GFrameCounter && Now >= Cache.NextSampleTime))
		{
			Cache.SampledFrame = GFrameCounter;
			Cache.NextSampleTime = Now + InventorySampleInterval;
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
	, CachedScaleRevision(MAX_uint32)
	, CachedUserScale(1.f)
	, CachedStyleRevision(MAX_uint32)
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
	const uint32 Revision = FNCPlusHUDLayout::GetLiveRevision();
	if (CachedScaleRevision != Revision)
	{
		const FName Alias = (SideIndex == 0) ? NCPlusWB::NAME_WeaponBarLeft : NCPlusWB::NAME_WeaponBarRight;
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		CachedUserScale = E ? FMath::Clamp(E->Scale, 0.25f, 4.f) : 1.f;
		CachedScaleRevision = Revision;
	}
	return Super::GetDrawScaleOverride() * CachedUserScale;
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

void UNCPlusHUDWidget_WeaponBar::DrawCachedNumber(int32 Value, float X, float Y,
	UFont* Font, float TextScale, float DrawOpacity, const FLinearColor& DrawColor,
	bool bRightAligned)
{
	if (!Canvas || !Font) return;
	NCPlusWB::FNumberTextCache& Entry = NCPlusWB::NumberText(Value);
	FVector2D* Size = Entry.Sizes.Find(Font);
	if (!Size)
	{
		float XL = 0.f;
		float YL = 0.f;
		Canvas->StrLen(Font, Entry.String, XL, YL);
		Size = &Entry.Sizes.Add(Font, FVector2D(XL, YL));
	}

	if (bScaleByDesignedResolution)
	{
		X *= RenderScale;
		Y *= RenderScale;
	}
	const float FinalScale = bScaleByDesignedResolution ? RenderScale * TextScale : TextScale;
	FVector2D DrawPos(RenderPosition.X + X, RenderPosition.Y + Y);
	if (bRightAligned)
	{
		DrawPos.X -= Size->X * FinalScale;
	}

	FLinearColor Color = DrawColor;
	Color.A = Opacity * DrawOpacity * (bIgnoreHUDOpacity ? 1.f : UTHUDOwner->WidgetOpacity);
	// FUTCanvasTextItem is not exported by the UT module; plugins must use the
	// engine canvas item or the client DLL fails to link on Linux/Windows.
	FCanvasTextItem TextItem(DrawPos, Entry.Text, Font, Color);
	FLinearColor Shadow = NCPlusWB::TextShadow;
	Shadow.A *= DrawOpacity * (bIgnoreHUDOpacity ? 1.f : UTHUDOwner->WidgetOpacity);
	TextItem.EnableShadow(Shadow, FVector2D(1.f, 1.f));
	TextItem.Scale = FVector2D(FinalScale, FinalScale);
	Canvas->DrawItem(TextItem);
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
		const FName MyAlias = (SideIndex == 0) ? NAME_WeaponBarLeft : NAME_WeaponBarRight;
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
	const bool bUseWeaponColors = UTHUDOwner->GetUseWeaponColors();

	// The two side widgets share a 20 Hz inventory sample. Classification and
	// sorting rebuild only when the pawn, inventory signature, or layout revision
	// changes; active weapon/ammo values remain render-rate reads below.
	const TArray<TWeakObjectPtr<AUTWeapon>>& MyWeapons =
		GetWeaponsForSide(Char, Layout, LayoutRevision, SideIndex, GetWorld()->GetTimeSeconds());
	if (MyWeapons.Num() == 0) return;

	UFont* GroupFont = UTHUDOwner->TinyFont;
	UFont* AmmoFont  = UTHUDOwner->SmallFont;

	// Gather all render-rate state before drawing. The inline allocator covers
	// the normal UT inventory without adding a per-frame heap allocation.
	TArray<FSlotDrawRecord, TInlineAllocator<16>> DrawRecords;
	DrawRecords.Reserve(MyWeapons.Num());
	for (int32 i = 0; i < MyWeapons.Num(); i++)
	{
		AUTWeapon* W = MyWeapons[i].Get();
		if (!W || W->IsPendingKill()) continue;

		const float SlotX = bVertical ? 0.f : i * (SlotW + SlotGap);
		const float SlotY = bVertical ? i * (SlotH + SlotGap) : 0.f;
		const bool  bActive = (W == CurrentWeapon);
		const bool  bNeedsAmmo = W->NeedsAmmoDisplay();

		FSlotDrawRecord Record;
		Record.SlotX = SlotX;
		Record.SlotY = SlotY;
		Record.BackgroundColor = bActive ? SlotBgActiveCol : SlotBgInactiveCol;
		Record.bDrawOutline = bActive;
		Record.bDrawIcon = false;
		Record.bDrawGroup = GroupFont && W->Group > 0;
		Record.Group = W->Group;
		Record.GroupOpacity = (bActive ? 1.0f : 0.7f) * Opacity;
		Record.bDrawAmmo = bNeedsAmmo && W->MaxAmmo > 0;
		Record.Ammo = W->Ammo;
		Record.AmmoTextOpacity = (bActive ? 1.0f : 0.75f) * Opacity;

		// Weapon icon — UVs from WeaponBarSelectedUVs are pixel coords against
		// WeaponIconAtlas. Note: UUTHUDWidget::DrawTexture normalizes the U/V/UL/VL
		// arguments INTERNALLY by texture size, so we pass the raw pixel values
		// (NOT pre-normalized) — pre-dividing here would double-divide and shrink
		// the sample region to a single texel (= invisible).
		if (WeaponIconAtlas && !WeaponIconAtlas->IsPendingKill())
		{
			const FTextureUVs& UV = W->WeaponBarSelectedUVs;
			FLinearColor IconCol = bUseWeaponColors
				? W->IconColor
				: FLinearColor::White;

			// Dim if not active and dim further if no ammo.
			float Alpha = bActive ? 1.0f : 0.55f;
			if (bNeedsAmmo && W->Ammo <= 0)
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
			Record.bDrawIcon = true;
			Record.IconX = IconX;
			Record.IconY = IconY;
			Record.IconW = IconW;
			Record.IconH = IconH;
			Record.IconU = UV.U;
			Record.IconV = UV.V;
			Record.IconUL = UV.UL;
			Record.IconVL = UV.VL;
			Record.IconColor = IconCol;
		}

		if (Record.bDrawAmmo)
		{
			Record.AmmoBarY = SlotY + SlotH - AmmoBarH - 2.f;
			Record.AmmoBarW = SlotW - SlotPad * 2.f;
			Record.AmmoBarX = SlotX + SlotPad;
			const float AmmoFrac = FMath::Clamp(float(W->Ammo) / float(W->MaxAmmo), 0.f, 1.f);
			FLinearColor FillCol = AmmoFillFullCol;
			if (W->Ammo <= W->AmmoDangerAmount)       FillCol = AmmoFillDangerCol;
			else if (W->Ammo <= W->AmmoWarningAmount) FillCol = AmmoFillWarnCol;
			Record.AmmoFillW = Record.AmmoBarW * AmmoFrac;
			Record.AmmoFillColor = FillCol;
		}

		DrawRecords.Add(Record);
	}

	// Pass 1: every DefaultTexture primitive. Within a slot the background is
	// still below its outline and ammo track/fill. Those regions do not overlap
	// the padded icon or either top-corner number.
	for (const FSlotDrawRecord& Record : DrawRecords)
	{
		const FLinearColor& Bg = Record.BackgroundColor;
		DrawTexture(Canvas->DefaultTexture, Record.SlotX, Record.SlotY, SlotW, SlotH,
			0.f, 0.f, 1.f, 1.f, Bg.A, Bg);

		if (Record.bDrawOutline)
		{
			const float OL = ActiveOutlineCol.A;
			DrawTexture(Canvas->DefaultTexture, Record.SlotX, Record.SlotY, SlotW, 1.5f,
				0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, Record.SlotX, Record.SlotY + SlotH - 1.5f, SlotW, 1.5f,
				0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, Record.SlotX, Record.SlotY, 1.5f, SlotH,
				0,0,1,1, OL, ActiveOutlineCol);
			DrawTexture(Canvas->DefaultTexture, Record.SlotX + SlotW - 1.5f, Record.SlotY, 1.5f, SlotH,
				0,0,1,1, OL, ActiveOutlineCol);
		}

		if (Record.bDrawAmmo)
		{
			DrawTexture(Canvas->DefaultTexture, Record.AmmoBarX, Record.AmmoBarY,
				Record.AmmoBarW, AmmoBarH, 0,0,1,1, 0.8f * Opacity, AmmoTrack);
			DrawTexture(Canvas->DefaultTexture, Record.AmmoBarX, Record.AmmoBarY,
				Record.AmmoFillW, AmmoBarH, 0,0,1,1,
				Record.AmmoFillColor.A, Record.AmmoFillColor);
		}
	}

	// Pass 2: atlas icons.
	for (const FSlotDrawRecord& Record : DrawRecords)
	{
		if (Record.bDrawIcon)
		{
			DrawTexture(WeaponIconAtlas, Record.IconX, Record.IconY, Record.IconW, Record.IconH,
				Record.IconU, Record.IconV, Record.IconUL, Record.IconVL,
				Record.IconColor.A, Record.IconColor);
		}
	}

	// Pass 3: TinyFont group numbers remain above the icons.
	for (const FSlotDrawRecord& Record : DrawRecords)
	{
		if (Record.bDrawGroup)
		{
			DrawCachedNumber(Record.Group,
				Record.SlotX + GroupNumPad, Record.SlotY + GroupNumPad,
				GroupFont, 1.0f, Record.GroupOpacity, FLinearColor::White, false);
		}
	}

	// Pass 4: SmallFont ammo counts remain above their track/fill.
	if (AmmoFont)
	{
		for (const FSlotDrawRecord& Record : DrawRecords)
		{
			if (Record.bDrawAmmo)
			{
				DrawCachedNumber(Record.Ammo,
					Record.SlotX + SlotW - GroupNumPad, Record.SlotY + GroupNumPad,
					AmmoFont, 0.85f, Record.AmmoTextOpacity,
					Record.AmmoFillColor, true);
			}
		}
	}
}
