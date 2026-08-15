// NCPlusHUDWidget_AutoPause.cpp - see header.
#include "NCPlusHUDWidget_AutoPause.h"

#include "NCAutoPauseState.h"
#include "UTHUD.h"
#include "Engine/Canvas.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformTime.h"

UNCPlusHUDWidget_AutoPause::UNCPlusHUDWidget_AutoPause(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, LastObservedStateRevision(INDEX_NONE)
	, LastObservedCountdownSecond(INDEX_NONE)
	, LastObservedCountdownStartRealTime(-1.0f)
	, LocalCountdownEndRealTime(0.0)
{
	// Compact top-center panel: below the normal score bar, but above the first
	// scoreboard rows.
	Position = FVector2D(0.f, 52.f);
	Size = FVector2D(700.f, 90.f);
	ScreenPosition = FVector2D(0.5f, 0.f);
	Origin = FVector2D(0.5f, 0.f);
	DesignedResolution = 1080.f;
	bShouldKickBack = false;

	CountdownSounds.SetNumZeroed(8);
	if (IsRunningDedicatedServer())
	{
		return;
	}

	// These are stock, always-cooked assets. Resolve them once while the client
	// widget CDO is constructed, rather than synchronously loading on CD7.
	static ConstructorHelpers::FObjectFinder<USoundBase> CD1(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD1.A_AnnouncerF_CD1'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD2(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD2.A_AnnouncerF_CD2'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD3(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD3.A_AnnouncerF_CD3'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD4(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD4.A_AnnouncerF_CD4'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD5(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD5.A_AnnouncerF_CD5'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD6(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD6.A_AnnouncerF_CD6'"));
	static ConstructorHelpers::FObjectFinder<USoundBase> CD7(
		TEXT("SoundWave'/Game/RestrictedAssets/Audio/AnnouncerStatus/A_AnnouncerF_CD7.A_AnnouncerF_CD7'"));
	CountdownSounds[1] = CD1.Object;
	CountdownSounds[2] = CD2.Object;
	CountdownSounds[3] = CD3.Object;
	CountdownSounds[4] = CD4.Object;
	CountdownSounds[5] = CD5.Object;
	CountdownSounds[6] = CD6.Object;
	CountdownSounds[7] = CD7.Object;
}

void UNCPlusHUDWidget_AutoPause::InitializeWidget(AUTHUD* Hud)
{
	Super::InitializeWidget(Hud);
	if (!IsRunningDedicatedServer() && !AudioTicker.IsValid())
	{
		AudioTicker = FTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(
				this, &UNCPlusHUDWidget_AutoPause::TickPausePresentation), 0.1f);
	}
}

void UNCPlusHUDWidget_AutoPause::BeginDestroy()
{
	if (AudioTicker.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AudioTicker);
		AudioTicker.Reset();
	}
	Super::BeginDestroy();
}

bool UNCPlusHUDWidget_AutoPause::TickPausePresentation(float /*DeltaTime*/)
{
	UpdateCountdownAudio(GetAutoPauseState());
	return true;
}

ANCAutoPauseState* UNCPlusHUDWidget_AutoPause::GetAutoPauseState()
{
	UWorld* World = GetWorld();
	ANCAutoPauseState* State = CachedAutoPauseState.Get();
	if (State == nullptr || State->GetWorld() != World)
	{
		ANCAutoPauseState* Found = ANCAutoPauseState::Find(World);
		if (Found != State)
		{
			// A newly replicated carrier belongs to a fresh presentation stream.
			LastObservedStateRevision = INDEX_NONE;
			LastObservedCountdownSecond = INDEX_NONE;
			LastObservedCountdownStartRealTime = -1.0f;
		}
		CachedAutoPauseState = Found;
		State = Found;
	}
	return State;
}

void UNCPlusHUDWidget_AutoPause::UpdateCountdownAudio(ANCAutoPauseState* State)
{
	if (State == nullptr)
	{
		return;
	}

	const FNCAutoPauseSnapshot& Snapshot = State->Snapshot;
	const double Now = FPlatformTime::Seconds();

	if (Snapshot.Phase != ENCAutoPausePhase::Resuming)
	{
		// Re-arm even when replication coalesces a later countdown back to the
		// same starting number as the previous one.
		LastObservedCountdownSecond = INDEX_NONE;
		LastObservedCountdownStartRealTime = -1.0f;
		LocalCountdownEndRealTime = 0.0;
		LastObservedStateRevision = Snapshot.StateRevision;
		return;
	}

	if (Snapshot.CountdownStartServerRealTime != LastObservedCountdownStartRealTime)
	{
		LastObservedCountdownStartRealTime = Snapshot.CountdownStartServerRealTime;
		LastObservedCountdownSecond = INDEX_NONE;
		LocalCountdownEndRealTime = Now
			+ double(FMath::Max(0, Snapshot.CountdownSecondsRemaining));
	}
	else if (Snapshot.StateRevision != LastObservedStateRevision)
	{
		const int32 LocallyPredicted = FMath::Max(0, FMath::CeilToInt(
			float(LocalCountdownEndRealTime - Now)));
		if (FMath::Abs(LocallyPredicted - Snapshot.CountdownSecondsRemaining) > 1)
		{
			LocalCountdownEndRealTime = Now
				+ double(FMath::Max(0, Snapshot.CountdownSecondsRemaining));
		}
	}
	LastObservedStateRevision = Snapshot.StateRevision;

	const int32 Remaining = FMath::Clamp(FMath::CeilToInt(
		float(LocalCountdownEndRealTime - Now)), 0, Snapshot.CountdownDurationSeconds);
	if (Remaining == LastObservedCountdownSecond)
	{
		return;
	}
	LastObservedCountdownSecond = Remaining;

	if (Remaining >= 1 && Remaining <= 7
		&& CountdownSounds.IsValidIndex(Remaining)
		&& CountdownSounds[Remaining] != nullptr)
	{
		UGameplayStatics::PlaySound2D(GetWorld(), CountdownSounds[Remaining]);
	}
}

bool UNCPlusHUDWidget_AutoPause::ShouldDraw_Implementation(bool bShowScores)
{
	// Do not call Super: its stock policy rejects dead/spectating players and all
	// widgets while scores are open. Pause status must remain universal.
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner))
	{
		return false;
	}

	ANCAutoPauseState* State = GetAutoPauseState();
	UpdateCountdownAudio(State);
	return State != nullptr && State->Snapshot.Phase != ENCAutoPausePhase::Inactive;
}

void UNCPlusHUDWidget_AutoPause::Draw_Implementation(float DeltaTime)
{
	if (!Canvas || !IsValid(UTHUDOwner))
	{
		return;
	}

	ANCAutoPauseState* State = GetAutoPauseState();
	if (State == nullptr || State->Snapshot.Phase == ENCAutoPausePhase::Inactive)
	{
		return;
	}
	UpdateCountdownAudio(State);

	const FNCAutoPauseSnapshot& Snapshot = State->Snapshot;
	const bool bResuming = Snapshot.Phase == ENCAutoPausePhase::Resuming;
	const int32 DisplayRemaining = bResuming && LocalCountdownEndRealTime > 0.0
		? FMath::Clamp(FMath::CeilToInt(float(
			LocalCountdownEndRealTime - FPlatformTime::Seconds())),
			0, Snapshot.CountdownDurationSeconds)
		: FMath::Max(0, Snapshot.CountdownSecondsRemaining);
	const FString Title = bResuming
		? FString::Printf(TEXT("RESUMING IN %d"),
			DisplayRemaining)
		: TEXT("MATCH PAUSED");

	FString Detail = Snapshot.PauseReason;
	if (Snapshot.AwaitedPlayerIds.Num() > 0)
	{
		const int32 AwaitedCount = Snapshot.AwaitedPlayerIds.Num();
		const FString AwaitedText = bResuming
			? FString::Printf(TEXT("%d PLAYER%s STILL DISCONNECTED"),
				AwaitedCount, AwaitedCount == 1 ? TEXT("") : TEXT("S"))
			: FString::Printf(TEXT("WAITING FOR %d PLAYER%s"),
				AwaitedCount, AwaitedCount == 1 ? TEXT("") : TEXT("S"));
		Detail = Detail.IsEmpty() ? AwaitedText : Detail + TEXT(" - ") + AwaitedText;
	}

	UFont* TitleFont = UTHUDOwner->MediumFont != nullptr
		? UTHUDOwner->MediumFont
		: UTHUDOwner->SmallFont;
	UFont* DetailFont = UTHUDOwner->SmallFont != nullptr
		? UTHUDOwner->SmallFont
		: UTHUDOwner->TinyFont;
	if (TitleFont == nullptr)
	{
		return;
	}

	const FLinearColor PanelColor(0.015f, 0.02f, 0.025f, 0.88f);
	const FLinearColor AccentColor = bResuming
		? FLinearColor(0.f, 0.88f, 1.f, 1.f)
		: FLinearColor(1.f, 0.72f, 0.08f, 1.f);
	const FLinearColor Shadow(0.f, 0.f, 0.f, 0.9f);
	const float CenterX = Size.X * 0.5f;

	if (Canvas->DefaultTexture != nullptr)
	{
		DrawTexture(Canvas->DefaultTexture, 0.f, 0.f, Size.X, Size.Y,
			0.f, 0.f, 1.f, 1.f, PanelColor.A, PanelColor);
		DrawTexture(Canvas->DefaultTexture, 0.f, 0.f, Size.X, 4.f,
			0.f, 0.f, 1.f, 1.f, AccentColor.A, AccentColor);
	}

	DrawText(FText::FromString(Title), CenterX + 1.f, 12.f,
		TitleFont, 1.2f, 1.f, Shadow,
		ETextHorzPos::Center, ETextVertPos::Top);
	DrawText(FText::FromString(Title), CenterX, 11.f,
		TitleFont, 1.2f, 1.f, AccentColor,
		ETextHorzPos::Center, ETextVertPos::Top);

	if (DetailFont != nullptr && !Detail.IsEmpty())
	{
		const FLinearColor DetailColor(1.f, 1.f, 1.f, 0.9f);
		DrawText(FText::FromString(Detail), CenterX + 1.f, 54.f,
			DetailFont, 0.9f, 1.f, Shadow,
			ETextHorzPos::Center, ETextVertPos::Top);
		DrawText(FText::FromString(Detail), CenterX, 53.f,
			DetailFont, 0.9f, 1.f, DetailColor,
			ETextHorzPos::Center, ETextVertPos::Top);
	}
}
