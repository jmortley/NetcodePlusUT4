#include "UTWeaponStateZoomingFix.h"
#include "UTWeaponFix.h" 
#include "UTHUDWidget.h"
#include "UTPlayerController.h"

UUTWeaponStateZoomingFix::UUTWeaponStateZoomingFix(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UUTWeaponStateZoomingFix::DrawHUD(UUTHUDWidget* WeaponHudWidget)
{
    // 1. Standard Checks (Scope overlay, etc)
    if (GetOuterAUTWeapon()->ZoomState != EZoomState::EZS_NotZoomed && OverlayMat != NULL)
    {
        UCanvas* C = WeaponHudWidget->GetCanvas();
        AUTPlayerController* UTPC = GetUTOwner()->GetLocalViewer();
        if (UTPC && UTPC->IsBehindView())
        {
            return true;
        }

        // Draw Scope Overlay
        if (OverlayMI == NULL)
        {
            OverlayMI = UMaterialInstanceDynamic::Create(OverlayMat, this);
        }
        FCanvasTileItem Item(FVector2D(0.0f, 0.0f), OverlayMI->GetRenderProxy(false), FVector2D(C->ClipX, C->ClipY));
        float OrigSizeX = Item.Size.X;
        Item.Size.X = FMath::Max<float>(Item.Size.X, Item.Size.Y * 16.0f / 9.0f);
        Item.Position.X -= (Item.Size.X - OrigSizeX) * 0.5f;
        Item.UV0 = FVector2D(0.0f, 0.0f);
        Item.UV1 = FVector2D(1.0f, 1.0f);
        C->DrawItem(Item);

        // 2. Draw Heads (CLEANED UP)
        if (bDrawHeads && TargetIndicator != NULL)
        {
            float HeadScale = GetOuterAUTWeapon()->GetHeadshotScale(nullptr);
            if (HeadScale > 0.0f)
            {
                AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
                float WorldTime = GetWorld()->TimeSeconds;
                FVector FireStart = GetOuterAUTWeapon()->GetFireStartLoc();

                for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
                {
                    AUTCharacter* EnemyChar = Cast<AUTCharacter>(*It);

                    // Filter targets (Alive, Visible, Enemy)
                    if (EnemyChar != NULL && !EnemyChar->IsDead() && !EnemyChar->IsInvisible() &&
                        !EnemyChar->IsFeigningDeath() &&
                        (EnemyChar->GetMesh()->LastRenderTime > WorldTime - 0.25f) &&
                        EnemyChar != GetUTOwner() &&
                        (GS == NULL || !GS->OnSameTeam(EnemyChar, GetUTOwner())))
                    {
                        FVector HeadLoc = EnemyChar->GetHeadLocation();
                        static FName NAME_SniperZoom(TEXT("SniperZoom"));

                        // Visibility Check (Is the head behind a wall?)
                        if (!GetWorld()->LineTraceTestByChannel(FireStart, HeadLoc, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionQueryParams(NAME_SniperZoom, true, GetUTOwner())))
                        {
                            // --- CHANGED: REMOVED PING CALCULATION ---
                            // We trust the Rewind. We only draw what is visually there.

                            float HeadRadius = EnemyChar->HeadRadius * EnemyChar->HeadScale * HeadScale;
                            if (EnemyChar->UTCharacterMovement && EnemyChar->UTCharacterMovement->bIsFloorSliding)
                            {
                                HeadRadius = EnemyChar->HeadRadius * EnemyChar->HeadScale;
                            }

                            // --- CHANGED: REMOVED LOOP ---
                            // We only draw ONCE (i=0), which is the Red Circle.

                            FVector Perpendicular = (HeadLoc - FireStart).GetSafeNormal() ^ FVector(0.0f, 0.0f, 1.0f);
                            FVector PointA = C->Project(HeadLoc + Perpendicular * HeadRadius);
                            FVector PointB = C->Project(HeadLoc - Perpendicular * HeadRadius);
                            FVector2D UpperLeft(FMath::Min<float>(PointA.X, PointB.X), FMath::Min<float>(PointA.Y, PointB.Y));
                            FVector2D BottomRight(FMath::Max<float>(PointA.X, PointB.X), FMath::Max<float>(PointA.Y, PointB.Y));
                            float MidY = (UpperLeft.Y + BottomRight.Y) * 0.5f;

                            // Screen Bounds Check
                            if ((FMath::Abs(MidY - 0.5f * C->SizeY) < 0.11f * C->SizeY) && (FMath::Abs(0.5f * (UpperLeft.X + BottomRight.X) - 0.5f * C->SizeX) < 0.11f * C->SizeX))
                            {
                                float SizeY = FMath::Max<float>(MidY - UpperLeft.Y, (BottomRight.X - UpperLeft.X) * 0.5f);
                                UpperLeft.Y = MidY - SizeY;
                                BottomRight.Y = MidY + SizeY;

                                // FORCE RED COLOR (1.0, 0.0, 0.0)
                                FLinearColor TargetColor = FLinearColor(1.0f, 0.0f, 0.0f, 0.5f);
                                FCanvasTileItem HeadCircleItem(UpperLeft, TargetIndicator->Resource, BottomRight - UpperLeft, TargetColor);
                                HeadCircleItem.BlendMode = SE_BLEND_Translucent;
                                C->DrawItem(HeadCircleItem);
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
    return true;
}