// NCPlusHUDWidget_CTFFlagStatus - implementation.
#include "NCPlusHUDWidget_CTFFlagStatus.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTCTFGameState.h"
#include "UTCTFFlagBase.h"
#include "UTCarriedObject.h"
#include "UTFlag.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"

UNCPlusHUDWidget_CTFFlagStatus::UNCPlusHUDWidget_CTFFlagStatus(const FObjectInitializer& OI)
	: Super(OI)
	, EnemyAnimationAlpha(0.f)
	, YouHaveFlagAppearedAt(-1.f)
	, EnemyHasFlagAppearedAt(-1.f)
{
}

// =============================================================================
// Pop-in scale helper.
//
// Returns 1.5 at AppearedAt, eases down to 1.0 over 200ms (quad ease-out),
// stays at 1.0 thereafter. -1 sentinel = "not appeared yet" => no pop, return 1.
// =============================================================================
float UNCPlusHUDWidget_CTFFlagStatus::ComputePopInScale(float AppearedAt) const
{
	if (AppearedAt < 0.f) return 1.f;
	UWorld* W = GetWorld();
	if (!W) return 1.f;

	const float Now = W->GetTimeSeconds();
	const float Elapsed = Now - AppearedAt;
	const float PopDuration = 0.20f;
	if (Elapsed >= PopDuration) return 1.f;

	const float T = FMath::Clamp(Elapsed / PopDuration, 0.f, 1.f);
	// Quad ease-out: out = 1 - (1 - t)^2
	const float EaseOut = 1.f - FMath::Square(1.f - T);
	return FMath::Lerp(1.5f, 1.0f, EaseOut);
}

// =============================================================================
// DrawFlagWorld - world-projected carrier / dropped-flag / pickable indicator.
//
// Re-implementation of the engine version because we need to (1) inject an
// offset_x/y into the screen position after world projection and (2) multiply
// our layout-driven scale into every per-element size. Modifying the inherited
// UPROPERTYs (MaxIconScale, InWorldAlpha) handles scale + opacity but the
// engine subtracts/re-adds RenderPosition such that modifying it cancels out,
// so offset has to be added at the RenderObj_TextureAt call sites directly.
//
// Reading from layout: alias `ctf_carrier_indicator` -> scale, offset_x/y
// (screen-pixel deltas, design-pixel domain), hidden, opacity.
// =============================================================================
void UNCPlusHUDWidget_CTFFlagStatus::DrawFlagWorld(AUTCTFGameState* GameState,
	FVector PlayerViewPoint, FRotator PlayerViewRotation, uint8 TeamNum,
	AUTCTFFlagBase* FlagBase, AUTFlag* Flag, AUTPlayerState* FlagHolder)
{
	if (!Flag) return;

	static const FName CarrierAlias(TEXT("ctf_carrier_indicator"));
	const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(CarrierAlias);
	if (Elem && Elem->bHidden) return;

	const float LayoutScale   = Elem ? FMath::Max(Elem->Scale, 0.01f) : 1.f;
	const float LayoutOpacity = Elem ? FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;
	const float OffsetX_Px    = (Elem ? Elem->Offset.X : 0.f) * RenderScale;
	const float OffsetY_Px    = (Elem ? Elem->Offset.Y : 0.f) * RenderScale;

	bScaleByDesignedResolution = false;

	const bool bSpectating = UTPlayerOwner->PlayerState && UTPlayerOwner->PlayerState->bOnlySpectator;
	const bool bIsEnemyFlag = !GameState->OnSameTeam(Flag, UTPlayerOwner);

	FlagIconTemplate.RenderColor = GameState->Teams.IsValidIndex(TeamNum)
		? GameState->Teams[TeamNum]->TeamColor : FLinearColor::Green;
	CameraIconTemplate.RenderColor = FlagIconTemplate.RenderColor;

	const float Dist = (Flag->GetActorLocation() - PlayerViewPoint).Size();
	// Apply layout scale into the world render scale so EVERY child size below
	// inherits it (icon, circle, border, taken overlay, dropped overlay).
	float WorldRenderScale = RenderScale * MaxIconScale * LayoutScale;

	AUTCharacter* Holder = nullptr;
	bool bDrawInWorld = false;
	bool bDrawEdgeArrow = false;
	FVector DrawScreenPosition(0.f);

	FVector WorldPosition = Flag->GetActorLocation()
		+ Flag->GetActorRotation().RotateVector(Flag->HomeBaseOffset)
		+ FVector(0.f, 0.f, Flag->Collision->GetUnscaledCapsuleHalfHeight() * 3.f);

	const float OldFlagAlpha = FlagIconTemplate.RenderOpacity;
	float CurrentWorldAlpha = InWorldAlpha;

	const FVector ViewDir = PlayerViewRotation.Vector();
	const float Edge = CircleTemplate.GetWidth() * WorldRenderScale;
	const bool bShouldDrawFlagIcon = ShouldDrawFlag(Flag, bIsEnemyFlag);

	if ((bSpectating || bShouldDrawFlagIcon)
		&& (Flag->Holder != UTHUDOwner->GetScorerPlayerState())
		&& (Flag->ObjectState != CarriedObjectState::Home))
	{
		WorldPosition = Flag->GetActorLocation();
		if (Flag->ObjectState == CarriedObjectState::Held)
		{
			Holder = Cast<AUTCharacter>(Flag->GetAttachmentReplication().AttachParent);
			if (Holder)
			{
				WorldPosition = Holder->GetMesh()->GetComponentLocation()
					+ FVector(0.f, 0.f, Holder->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() * 2.25f);
			}
		}
		else
		{
			WorldPosition += FVector(0.f, 0.f, Flag->Collision->GetUnscaledCapsuleHalfHeight() * 0.75f);
		}
		bDrawInWorld = true;
		DrawScreenPosition = GetAdjustedScreenPosition(WorldPosition, PlayerViewPoint, ViewDir, Dist, Edge, bDrawEdgeArrow, TeamNum);
	}
	else
	{
		DrawScreenPosition = GetAdjustedScreenPosition(WorldPosition, PlayerViewPoint, ViewDir, Dist, Edge, bDrawEdgeArrow, TeamNum);
	}

	// "Just appeared" scale pulse for the enemy carrier indicator.
	const float CurrentWorldTime = GameState->GetWorld()->GetTimeSeconds();
	if (bIsEnemyFlag)
	{
		if (bDrawInWorld && !bEnemyFlagWasDrawn)
		{
			EnemyFlagStartDrawTime = CurrentWorldTime;
		}
		if (CurrentWorldTime - EnemyFlagStartDrawTime < 0.3f)
		{
			WorldRenderScale *= (1.f + 3.f * (0.3f - CurrentWorldTime + EnemyFlagStartDrawTime));
		}
		bEnemyFlagWasDrawn = bDrawInWorld;
	}

	if (bDrawInWorld)
	{
		const float PctFromCenter = (DrawScreenPosition - FVector(0.5f * GetCanvas()->ClipX, 0.5f * GetCanvas()->ClipY, 0.f)).Size() / GetCanvas()->ClipX;
		CurrentWorldAlpha = InWorldAlpha * FMath::Min(6.f * PctFromCenter, 1.f);

		const float ViewDist = (PlayerViewPoint - WorldPosition).Size();

		// don't overlap player beacon
		UFont* TinyFont = AUTHUD::StaticClass()->GetDefaultObject<AUTHUD>()->TinyFont;
		float X, Y;
		const float Scale = Canvas->ClipX / 1920.f;
		Canvas->TextSize(TinyFont, FString("+999   A999"), X, Y, Scale, Scale);
		if (!bDrawEdgeArrow)
		{
			const float MinDistSq = FMath::Square(0.06f * GetCanvas()->ClipX);
			const float ActualDistSq = FMath::Square(DrawScreenPosition.X - 0.5f * GetCanvas()->ClipX)
				+ FMath::Square(DrawScreenPosition.Y - 0.5f * GetCanvas()->ClipY);
			if (ActualDistSq > MinDistSq)
			{
				if (!Holder || (ViewDist < Holder->TeamPlayerIndicatorMaxDistance))
				{
					DrawScreenPosition.Y -= 3.5f * Y;
				}
				else
				{
					DrawScreenPosition.Y -= (ViewDist < Holder->SpectatorIndicatorMaxDistance) ? 2.5f * Y : 1.5f * Y;
				}
			}
		}

		// Engine-required widget-root subtraction so RenderObj_TextureAt re-adds
		// it correctly internally.
		DrawScreenPosition.X -= RenderPosition.X;
		DrawScreenPosition.Y -= RenderPosition.Y;

		// Layout offset: apply AFTER the projection + RenderPosition normalization
		// so it shifts the final on-screen position by the requested pixel delta.
		DrawScreenPosition.X += OffsetX_Px;
		DrawScreenPosition.Y += OffsetY_Px;

		// Layout opacity multiplies the per-distance alpha.
		CurrentWorldAlpha *= LayoutOpacity;

		FlagIconTemplate.RenderOpacity = CurrentWorldAlpha;
		CircleTemplate.RenderOpacity = CurrentWorldAlpha;
		CircleBorderTemplate.RenderOpacity = CurrentWorldAlpha;

		const float InWorldFlagScale = WorldRenderScale;
		RenderObj_TextureAt(CircleTemplate, DrawScreenPosition.X, DrawScreenPosition.Y,
			CircleTemplate.GetWidth() * InWorldFlagScale, CircleTemplate.GetHeight() * InWorldFlagScale);
		RenderObj_TextureAt(CircleBorderTemplate, DrawScreenPosition.X, DrawScreenPosition.Y,
			CircleBorderTemplate.GetWidth() * InWorldFlagScale, CircleBorderTemplate.GetHeight() * InWorldFlagScale);

		if (bDrawEdgeArrow)
		{
			DrawEdgeArrow(WorldPosition, PlayerViewPoint, PlayerViewRotation,
				DrawScreenPosition, CurrentWorldAlpha, WorldRenderScale, TeamNum);
		}
		else
		{
			const FText FlagStatusMessage = Flag->GetHUDStatusMessage(UTHUDOwner);
			if (!FlagStatusMessage.IsEmpty())
			{
				DrawText(FlagStatusMessage, DrawScreenPosition.X,
					DrawScreenPosition.Y - ((CircleTemplate.GetHeight() + 40) * WorldRenderScale),
					AUTHUD::StaticClass()->GetDefaultObject<AUTHUD>()->TinyFont,
					true, FVector2D(1.f, 1.f), FLinearColor::Black, false, FLinearColor::Black,
					1.5f * WorldRenderScale, 0.5f + 0.5f * CurrentWorldAlpha,
					FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f),
					ETextHorzPos::Center, ETextVertPos::Center);
			}
		}

		if (Flag && Flag->ObjectState == CarriedObjectState::Held)
		{
			TakenIconTemplate.RenderOpacity = CurrentWorldAlpha;
			RenderObj_TextureAt(TakenIconTemplate, DrawScreenPosition.X, DrawScreenPosition.Y,
				1.1f * TakenIconTemplate.GetWidth() * InWorldFlagScale,
				1.1f * TakenIconTemplate.GetHeight() * InWorldFlagScale);
			RenderObj_TextureAt(FlagIconTemplate,
				DrawScreenPosition.X - 0.25f * FlagIconTemplate.GetWidth() * InWorldFlagScale,
				DrawScreenPosition.Y - 0.25f * FlagIconTemplate.GetHeight() * InWorldFlagScale,
				FlagIconTemplate.GetWidth() * InWorldFlagScale,
				FlagIconTemplate.GetHeight() * InWorldFlagScale);
		}
		else
		{
			RenderObj_TextureAt(FlagIconTemplate, DrawScreenPosition.X, DrawScreenPosition.Y,
				1.25f * FlagIconTemplate.GetWidth() * WorldRenderScale,
				1.25f * FlagIconTemplate.GetHeight() * WorldRenderScale);

			if (Flag->ObjectState == CarriedObjectState::Dropped)
			{
				const float DroppedAlpha = DroppedIconTemplate.RenderOpacity;
				DroppedIconTemplate.RenderOpacity = CurrentWorldAlpha;
				RenderObj_TextureAt(DroppedIconTemplate, DrawScreenPosition.X, DrawScreenPosition.Y,
					1.25f * DroppedIconTemplate.GetWidth() * InWorldFlagScale,
					1.25f * DroppedIconTemplate.GetHeight() * InWorldFlagScale);
				DroppedIconTemplate.RenderOpacity = DroppedAlpha;
				DrawText(GetFlagReturnTime(Flag), DrawScreenPosition.X, DrawScreenPosition.Y,
					TinyFont, true, FVector2D(1.f, 1.f), FLinearColor::Black, false, FLinearColor::Black,
					1.5f * InWorldFlagScale, 0.5f + 0.5f * CurrentWorldAlpha,
					FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f),
					ETextHorzPos::Center, ETextVertPos::Center);
			}
		}
	}

	FlagIconTemplate.RenderOpacity = OldFlagAlpha;
	CircleTemplate.RenderOpacity = 1.f;
	CircleBorderTemplate.RenderOpacity = 1.f;

	bScaleByDesignedResolution = true;
}

// =============================================================================
// DrawStatusMessage - both banners.
//
// Engine version only ever rendered YouHaveFlagText. We also render
// EnemyHasFlagText when an enemy is carrying our team's flag (the UT99-style
// alarm). Each banner has its own alias for nchud control.
//
// State detection:
//   - "you have flag" = OwnerPS->CarriedObject != null  (engine's existing check)
//   - "enemy has flag" = GS->GetFlagHolder(MyTeam) is a non-teammate
//
// Both can fire in the same frame (double-carry scenario).
// =============================================================================
void UNCPlusHUDWidget_CTFFlagStatus::DrawStatusMessage(float DeltaTime)
{
	AUTCTFGameState* GS = Cast<AUTCTFGameState>(UTGameState);
	if (bSuppressMessage || GS == nullptr) return;
	if (!GS->IsMatchInProgress() || UTHUDOwner == nullptr || UTHUDOwner->PlayerOwner == nullptr) return;

	APawn* ViewedPawn = Cast<APawn>(UTHUDOwner->UTPlayerOwner->GetViewTarget());
	AUTPlayerState* ViewedPS = ViewedPawn ? Cast<AUTPlayerState>(ViewedPawn->PlayerState) : nullptr;
	AUTPlayerState* OwnerPS = ViewedPS ? ViewedPS : UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (OwnerPS == nullptr || OwnerPS->Team == nullptr) return;

	const uint8 MyTeam = OwnerPS->GetTeamNum();

	// --- Two animation alphas (yellow uses parent's AnimationAlpha; red uses our own). ---
	AnimationAlpha       += DeltaTime * 3.f;
	EnemyAnimationAlpha  += DeltaTime * 3.f;
	const float YouPulse   = FMath::Abs(FMath::Sin(AnimationAlpha));
	const float EnemyPulse = FMath::Abs(FMath::Sin(EnemyAnimationAlpha));

	const float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// --- State 1: you carry an enemy flag. ---
	const bool bYouHaveFlag = (OwnerPS->CarriedObject != nullptr);
	if (bYouHaveFlag)
	{
		if (YouHaveFlagAppearedAt < 0.f) YouHaveFlagAppearedAt = CurrentTime;
		const float Pop = ComputePopInScale(YouHaveFlagAppearedAt);
		DrawOneBanner(TEXT("ctf_you_have_flag"), YouHaveFlagText, FLinearColor::Yellow, YouPulse, Pop);
	}
	else
	{
		YouHaveFlagAppearedAt = -1.f;
		AnimationAlpha = 0.f;
	}

	// --- State 2: enemy is carrying our team's flag (NEW). ---
	AUTPlayerState* OurFlagHolder = GS->GetFlagHolder(MyTeam);
	const bool bEnemyHasFlag = (OurFlagHolder != nullptr
		&& OurFlagHolder->Team != nullptr
		&& OurFlagHolder->Team->TeamIndex != MyTeam);
	if (bEnemyHasFlag)
	{
		if (EnemyHasFlagAppearedAt < 0.f) EnemyHasFlagAppearedAt = CurrentTime;
		const float Pop = ComputePopInScale(EnemyHasFlagAppearedAt);
		// Stock UT4 red, slightly saturated so it pops against dark HUDs.
		const FLinearColor DefaultRed(1.f, 0.18f, 0.18f, 1.f);
		DrawOneBanner(TEXT("ctf_enemy_has_flag"), EnemyHasFlagText, DefaultRed, EnemyPulse, Pop);
	}
	else
	{
		EnemyHasFlagAppearedAt = -1.f;
		EnemyAnimationAlpha = 0.f;
	}
}

// =============================================================================
// DrawOneBanner - layout-aware single-banner render.
//
// Reads the alias's anchor/offset/scale/opacity/hidden and a color_text extra,
// computes the absolute on-screen position, and renders via the inherited
// FlagStatusText (save/restore so the parent's UPROPERTY state isn't leaked).
// =============================================================================
void UNCPlusHUDWidget_CTFFlagStatus::DrawOneBanner(FName Alias, const FText& Text,
	const FLinearColor& DefaultColor, float PulseAlpha, float PopInScale)
{
	const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(Alias);
	if (Elem && Elem->bHidden) return;
	if (Text.IsEmpty()) return;
	if (!Canvas) return;

	// Anchor + offset -> absolute screen pixels.
	// Default anchors come from the alias table (you_have_flag = BottomCenter,
	// enemy_has_flag = TopCenter). Default offsets are also in the alias table.
	const ENCPlusHUDAnchor Anchor = Elem ? Elem->Anchor : NCPlusHUDAliases::GetStockAnchor(Alias);
	const FVector2D StockOffset   = NCPlusHUDAliases::GetStockOffset(Alias);
	const FVector2D Offset        = Elem ? Elem->Offset : StockOffset;
	const float LayoutScale       = Elem ? FMath::Max(Elem->Scale, 0.01f) : 1.f;
	const float LayoutOpacity     = Elem ? FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;
	const FLinearColor TextColor  = Elem ? Elem->GetExtraColor(TEXT("color_text"), DefaultColor) : DefaultColor;

	const FVector2D AnchorScreen = FNCPlusHUDLayout::AnchorToScreenCoords(Anchor);
	const float ScreenX = AnchorScreen.X * Canvas->ClipX + Offset.X * RenderScale;
	const float ScreenY = AnchorScreen.Y * Canvas->ClipY + Offset.Y * RenderScale;

	// Switch FlagStatusText to raw-screen-pixel mode for this draw so the position
	// is interpreted as absolute screen pixels (matches our anchor math).
	const bool bOldScaleMode = bScaleByDesignedResolution;
	bScaleByDesignedResolution = false;

	// Save inherited FlagStatusText state so we restore parent invariants on the
	// way out. This is the same UPROPERTY the engine's DrawStatusMessage mutates.
	const FVector2D SavedPos       = FlagStatusText.Position;
	const FLinearColor SavedColor  = FlagStatusText.RenderColor;
	const float SavedScale         = FlagStatusText.RenderScale;
	const float SavedOpacity       = FlagStatusText.RenderOpacity;
	const FText SavedText          = FlagStatusText.Text;
	const bool SavedShadow         = FlagStatusText.bDrawShadow;
	const FVector2D SavedShadowDir = FlagStatusText.ShadowDirection;
	const FLinearColor SavedShadow_C = FlagStatusText.ShadowColor;

	// Position relative to widget root (RenderObj_Text re-adds RenderPosition).
	FlagStatusText.Position = FVector2D(ScreenX - RenderPosition.X, ScreenY - RenderPosition.Y);
	FlagStatusText.RenderColor    = TextColor;
	FlagStatusText.RenderScale    = SavedScale * LayoutScale * PopInScale;
	FlagStatusText.RenderOpacity  = PulseAlpha * LayoutOpacity;
	FlagStatusText.Text           = Text;
	FlagStatusText.bDrawShadow    = true;
	FlagStatusText.ShadowDirection = FVector2D(1.f, 2.f);
	FlagStatusText.ShadowColor    = FLinearColor::Black;
	RenderObj_Text(FlagStatusText);

	// Restore.
	FlagStatusText.Position       = SavedPos;
	FlagStatusText.RenderColor    = SavedColor;
	FlagStatusText.RenderScale    = SavedScale;
	FlagStatusText.RenderOpacity  = SavedOpacity;
	FlagStatusText.Text           = SavedText;
	FlagStatusText.bDrawShadow    = SavedShadow;
	FlagStatusText.ShadowDirection = SavedShadowDir;
	FlagStatusText.ShadowColor    = SavedShadow_C;
	bScaleByDesignedResolution    = bOldScaleMode;
}
