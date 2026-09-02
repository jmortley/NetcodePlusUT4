#include "NCHighPollingMouseInput.h"

#if PLATFORM_WINDOWS && !UE_SERVER

#include "Engine/Engine.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "UnrealEngine.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/InputSettings.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarNCPHighPollingMouseCoalesce(
		TEXT("ncp.HighPollingMouseCoalesce"),
		0,
		TEXT("Coalesce captured high-precision relative mouse motion before Slate routes it.\n")
		TEXT("0: stock UE4.15 input (default)\n")
		TEXT("1: coalesce gameplay motion once per rendered frame"),
		ECVF_Default);

	/**
	 * UE4.15 routes every WM_INPUT packet through Slate, then FSceneViewport
	 * accumulates those deltas and submits MouseX/MouseY once at the end of the
	 * input phase. At 4-8 kHz, the intermediate hit testing and widget routing is
	 * pure overhead while the game viewport has captured and hidden the cursor.
	 *
	 * This preprocessor performs the same final InputAxis calls at the start of
	 * FinishedInputThisFrame(), preserving the summed integer deltas and original
	 * sample count. It deliberately declines all UI, editor, smoothing, focus-loss,
	 * player-cursor, and uncaptured cases so those retain stock Slate semantics.
	 */
	class FNCHighPollingMouseInput final : public IInputProcessor
	{
	public:
		FNCHighPollingMouseInput()
			: PendingX(0)
			, PendingY(0)
			, PendingSamples(0)
			, BatchMode(EBatchMode::Undecided)
			, BatchSceneViewport(nullptr)
		{
		}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp,
			TSharedRef<ICursor> Cursor) override
		{
			FlushPending();
			ResetBatch();
		}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp,
			const FKeyEvent& InKeyEvent) override
		{
			return false;
		}

		virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp,
			const FKeyEvent& InKeyEvent) override
		{
			return false;
		}

		virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp,
			const FAnalogInputEvent& InAnalogInputEvent) override
		{
			return false;
		}

		virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp,
			const FPointerEvent& MouseEvent) override
		{
			if (BatchMode == EBatchMode::Undecided)
			{
				UGameViewportClient* GameViewport = nullptr;
				FSceneViewport* SceneViewport = nullptr;
				if (IsGameplayCaptureEligible(SlateApp, MouseEvent,
					GameViewport, SceneViewport))
				{
					BatchMode = EBatchMode::Consume;
					BatchGameViewport = GameViewport;
					BatchSceneViewport = SceneViewport;
				}
				else
				{
					BatchMode = EBatchMode::PassThrough;
				}
			}

			// Lock the first event's eligibility choice for the OS-message batch.
			// The source-safety check below may demote Consume to PassThrough, but
			// never promotes a batch that initially failed the gameplay gates.
			if (BatchMode != EBatchMode::Consume)
			{
				return false;
			}

			// FSlateApplication::OnMouseMove constructs CursorDelta from these two
			// positions, while OnRawMouseMove supplies an explicit relative delta.
			// Absolute RAWINPUT is forwarded through OnMouseMove even while high-
			// precision mode is active. Never consume that position-derived event:
			// stock routing must advance Slate's last-position anchor or subsequent
			// absolute reports can repeatedly rotate from a stale origin.
			if (IsPositionDerivedMouseMove(MouseEvent))
			{
				// Keep any relative deltas already accepted earlier in this batch. Tick
				// flushes them before FSceneViewport flushes this and later stock events.
				BatchMode = EBatchMode::PassThrough;
				return false;
			}

			const FVector2D Delta = MouseEvent.GetCursorDelta();
			PendingX += static_cast<int64>(FMath::TruncToInt(Delta.X));
			// FSceneViewport applies this sign flip before submitting MouseY.
			PendingY -= static_cast<int64>(FMath::TruncToInt(Delta.Y));
			if (PendingSamples < MAX_int32)
			{
				++PendingSamples;
			}
			return true;
		}

		void DiscardPending()
		{
			ResetBatch();
		}

	private:
		enum class EBatchMode : uint8
		{
			Undecided,
			PassThrough,
			Consume
		};

		static bool IsPositionDerivedMouseMove(const FPointerEvent& MouseEvent)
		{
			const FVector2D PositionDelta = MouseEvent.GetScreenSpacePosition()
				- MouseEvent.GetLastScreenSpacePosition();
			// A relative raw delta can coincidentally match this value. Falling back
			// to stock for that batch is conservative and does not change input.
			return MouseEvent.GetCursorDelta() == PositionDelta;
		}

		static bool IsGameplayCaptureEligible(FSlateApplication& SlateApp,
			const FPointerEvent& MouseEvent, UGameViewportClient*& OutGameViewport,
			FSceneViewport*& OutSceneViewport)
		{
			if (CVarNCPHighPollingMouseCoalesce.GetValueOnGameThread() == 0
				|| !FApp::IsGame() || GIsEditor || MouseEvent.IsTouchEvent()
				|| !SlateApp.IsActive() || SlateApp.AnyMenusVisible()
				|| !SlateApp.IsUsingHighPrecisionMouseMovment()
				|| GetDefault<UInputSettings>()->bEnableMouseSmoothing
				|| GEngine == nullptr || GEngine->GameViewport == nullptr)
			{
				return false;
			}

			UGameViewportClient* const GameViewport = GEngine->GameViewport;
			UWorld* const World = GameViewport->GetWorld();
			FSceneViewport* const SceneViewport = GameViewport->GetGameViewport();
			const TSharedPtr<SViewport> ViewportWidget =
				GameViewport->GetGameViewportWidget();
			APlayerController* const PlayerController =
				World != nullptr ? World->GetFirstPlayerController() : nullptr;

			const bool bEligible = World != nullptr
				&& World->WorldType == EWorldType::Game
				&& !GameViewport->IgnoreInput()
				&& (GameViewport->ViewportConsole == nullptr
					|| !GameViewport->ViewportConsole->ConsoleActive())
				&& PlayerController != nullptr
				&& PlayerController->IsLocalController()
				&& !PlayerController->ShouldShowMouseCursor()
				&& SceneViewport != nullptr
				&& SceneViewport->HasMouseCapture()
				&& ViewportWidget.IsValid()
				&& ViewportWidget->HasMouseCapture();

			if (bEligible)
			{
				OutGameViewport = GameViewport;
				OutSceneViewport = SceneViewport;
			}
			return bEligible;
		}

		void FlushPending()
		{
			if (PendingSamples <= 0)
			{
				return;
			}

			UGameViewportClient* const GameViewport = BatchGameViewport.Get();
			FSceneViewport* const SceneViewport = BatchSceneViewport;

			// A gate may change later in the same message pump (for example F5 opens),
			// but these deltas occurred while gameplay owned capture. Flush them as
			// stock would, provided the exact viewport still exists.
			if (GameViewport != nullptr && SceneViewport != nullptr
				&& GameViewport->GetGameViewport() == SceneViewport
				&& GameViewport->GetWorld() != nullptr)
			{
				const float X = static_cast<float>(FMath::Clamp<int64>(
					PendingX, static_cast<int64>(MIN_int32), static_cast<int64>(MAX_int32)));
				const float Y = static_cast<float>(FMath::Clamp<int64>(
					PendingY, static_cast<int64>(MIN_int32), static_cast<int64>(MAX_int32)));
				const float FrameDelta = FApp::GetDeltaTime();
				FScopedConditionalWorldSwitcher WorldSwitcher(GameViewport);

				// This is exactly where FSceneViewport::OnFinishedPointerInput would
				// submit its accumulated movement later in this same function.
				GameViewport->InputAxis(SceneViewport, 0, EKeys::MouseX,
					X, FrameDelta, PendingSamples, false);
				GameViewport->InputAxis(SceneViewport, 0, EKeys::MouseY,
					Y, FrameDelta, PendingSamples, false);
			}
		}

		void ResetBatch()
		{
			PendingX = 0;
			PendingY = 0;
			PendingSamples = 0;
			BatchMode = EBatchMode::Undecided;
			BatchGameViewport.Reset();
			BatchSceneViewport = nullptr;
		}

		int64 PendingX;
		int64 PendingY;
		int32 PendingSamples;
		EBatchMode BatchMode;
		TWeakObjectPtr<UGameViewportClient> BatchGameViewport;
		FSceneViewport* BatchSceneViewport;
	};

	static TSharedPtr<FNCHighPollingMouseInput> GHighPollingMouseInput;
	static FDelegateHandle GHighPollingMousePreExitHandle;

	static void ReleaseHighPollingMouseInput()
	{
		if (!GHighPollingMouseInput.IsValid())
		{
			return;
		}

		GHighPollingMouseInput->DiscardPending();
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetInputPreProcessor(false);
		}
		GHighPollingMouseInput.Reset();
	}

	static void HandleHighPollingMousePreExit()
	{
		if (GHighPollingMousePreExitHandle.IsValid())
		{
			FCoreDelegates::OnPreExit.Remove(GHighPollingMousePreExitHandle);
			GHighPollingMousePreExitHandle.Reset();
		}
		ReleaseHighPollingMouseInput();
	}
}

#endif // PLATFORM_WINDOWS && !UE_SERVER

void RegisterNCHighPollingMouseInput()
{
#if PLATFORM_WINDOWS && !UE_SERVER
	if (GIsEditor || GHighPollingMouseInput.IsValid()
		|| !FSlateApplication::IsInitialized())
	{
		return;
	}

	GHighPollingMouseInput = MakeShareable(new FNCHighPollingMouseInput());
	FSlateApplication::Get().SetInputPreProcessor(true, GHighPollingMouseInput);
	GHighPollingMousePreExitHandle = FCoreDelegates::OnPreExit.AddStatic(
		&HandleHighPollingMousePreExit);
#endif
}

void UnregisterNCHighPollingMouseInput()
{
#if PLATFORM_WINDOWS && !UE_SERVER
	if (GHighPollingMousePreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(GHighPollingMousePreExitHandle);
		GHighPollingMousePreExitHandle.Reset();
	}
	ReleaseHighPollingMouseInput();
#endif
}
