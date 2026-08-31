#include "NCShockInputTrace.h"

#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTWeaponState.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS && !UE_SERVER
#include "Framework/Application/SlateApplication.h"
#include "Windows/WindowsApplication.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogNCShockInputTrace, Log, All);

namespace
{
	static TAutoConsoleVariable<int32> CVarShockInputTrace(
		TEXT("ncp.ShockInputTrace"),
		0,
		TEXT("Shock-primary client input-path diagnostics. No input is consumed and firing is unchanged.\n")
		TEXT("0: off (default)\n")
		TEXT("1: retain a bounded trace and log only broken input chains\n")
		TEXT("2: also log every observed stage; use only for short captures"),
		ECVF_Default);

	enum class ENCShockTraceStage : uint8
	{
		NativeDown,
		NativeUp,
		PlayerInputDown,
		PlayerInputUp,
		ActionStart,
		ActionStop,
		QueueStart,
		QueueStop,
		WeaponStart,
		WeaponStop,
		FireShot,
		Anomaly
	};

	enum class ENCShockTraceAnomaly : uint8
	{
		None,
		NativeNoPlayerInput,
		PlayerInputNoNative,
		PlayerInputNoAction,
		ActionNoDeferredQueue,
		DeferredQueueNoWeapon,
		WeaponStartNoShot
	};

	enum ENCShockTraceFlags : uint16
	{
		TraceEligible = 1 << 0,
		TraceFocused = 1 << 1,
		TraceSyntheticPress = 1 << 2,
		TraceQueueMatched = 1 << 3,
		TraceRetry = 1 << 4,
		TraceExpectedImmediate = 1 << 5,
		TraceInternalStop = 1 << 6
	};

	struct FNCShockTraceRecord
	{
		uint32 TimeMs;
		uint32 Frame;
		uint32 PressId;
		ENCShockTraceStage Stage;
		ENCShockTraceAnomaly Anomaly;
		uint16 Flags;
		int16 QueueDepth;
		float ReadyInMs;
		FName WeaponState;

		FNCShockTraceRecord()
			: TimeMs(0)
			, Frame(0)
			, PressId(0)
			, Stage(ENCShockTraceStage::NativeDown)
			, Anomaly(ENCShockTraceAnomaly::None)
			, Flags(0)
			, QueueDepth(-1)
			, ReadyInMs(0.f)
			, WeaponState(NAME_None)
		{
		}
	};

	struct FNCShockTracePress
	{
		uint32 PressId;
		uint32 FirstTimeMs;
		uint32 NativeDownMs;
		uint32 PlayerDownMs;
		uint32 ActionStartMs;
		uint32 QueueStartMs;
		uint32 WeaponStartMs;
		uint32 ShotMs;
		uint16 ReportedMask;
		bool bEligible;
		bool bNativeDown;
		bool bPlayerDown;
		bool bActionStart;
		bool bQueueStart;
		bool bQueueEvidenceConclusive;
		bool bWeaponStart;
		bool bShot;
		bool bExpectedImmediateShot;

		FNCShockTracePress()
			: PressId(0)
			, FirstTimeMs(0)
			, NativeDownMs(0)
			, PlayerDownMs(0)
			, ActionStartMs(0)
			, QueueStartMs(0)
			, WeaponStartMs(0)
			, ShotMs(0)
			, ReportedMask(0)
			, bEligible(false)
			, bNativeDown(false)
			, bPlayerDown(false)
			, bActionStart(false)
			, bQueueStart(false)
			, bQueueEvidenceConclusive(false)
			, bWeaponStart(false)
			, bShot(false)
			, bExpectedImmediateShot(false)
		{
		}
	};

#if PLATFORM_WINDOWS && !UE_SERVER
	struct FNCShockNativeEvent
	{
		uint32 TimeMs;
		uint32 PressId;
		bool bPressed;
		bool bFocused;
	};
#endif

	static const TCHAR* StageName(ENCShockTraceStage Stage)
	{
		switch (Stage)
		{
		case ENCShockTraceStage::NativeDown: return TEXT("native-down");
		case ENCShockTraceStage::NativeUp: return TEXT("native-up");
		case ENCShockTraceStage::PlayerInputDown: return TEXT("playerinput-down");
		case ENCShockTraceStage::PlayerInputUp: return TEXT("playerinput-up");
		case ENCShockTraceStage::ActionStart: return TEXT("action-start");
		case ENCShockTraceStage::ActionStop: return TEXT("action-stop");
		case ENCShockTraceStage::QueueStart: return TEXT("queue-start");
		case ENCShockTraceStage::QueueStop: return TEXT("queue-stop");
		case ENCShockTraceStage::WeaponStart: return TEXT("weapon-start");
		case ENCShockTraceStage::WeaponStop: return TEXT("weapon-stop");
		case ENCShockTraceStage::FireShot: return TEXT("fire-shot");
		case ENCShockTraceStage::Anomaly: return TEXT("anomaly");
		default: return TEXT("unknown");
		}
	}

	static const TCHAR* AnomalyName(ENCShockTraceAnomaly Anomaly)
	{
		switch (Anomaly)
		{
		case ENCShockTraceAnomaly::NativeNoPlayerInput: return TEXT("native-no-playerinput");
		case ENCShockTraceAnomaly::PlayerInputNoNative: return TEXT("playerinput-no-native");
		case ENCShockTraceAnomaly::PlayerInputNoAction: return TEXT("playerinput-no-action-or-mapping");
		case ENCShockTraceAnomaly::ActionNoDeferredQueue: return TEXT("action-no-deferred-queue");
		case ENCShockTraceAnomaly::DeferredQueueNoWeapon: return TEXT("deferred-queue-no-weapon");
		case ENCShockTraceAnomaly::WeaponStartNoShot: return TEXT("weapon-start-no-shot");
		default: return TEXT("none");
		}
	}

	static uint32 MonotonicMilliseconds()
	{
		// UE4.15 Windows biases Seconds() by a large epoch. Narrow through uint64
		// so the intended modulo-2^32 clock is defined C++.
		return static_cast<uint32>(static_cast<uint64>(
			FPlatformTime::Seconds() * 1000.0));
	}

	static int32 ElapsedMilliseconds(uint32 Now, uint32 Then)
	{
		return static_cast<int32>(Now - Then);
	}

	class FNCShockInputTraceManager
#if PLATFORM_WINDOWS && !UE_SERVER
		: public IWindowsMessageHandler
#endif
	{
	public:
		FNCShockInputTraceManager()
			: RingHead(0)
			, RingCount(0)
			, ActivePressId(0)
			, SyntheticPressCounter(0)
			, NativeDownCount(0)
			, PlayerDownCount(0)
			, ActionStartCount(0)
			, QueueStartCount(0)
			, WeaponStartCount(0)
			, ShotCount(0)
			, AnomalyCount(0)
			, DroppedNativeEvents(0)
			, LastSweepMs(0)
			, bNativeObserverAvailable(false)
#if PLATFORM_WINDOWS && !UE_SERVER
			, NextNativePressId(0)
			, HeldNativePressId(0)
			, bNativeHandlerRegistered(false)
#endif
		{
			Ring.SetNum(RingCapacity);
			Presses.Reserve(MaxPresses);
#if PLATFORM_WINDOWS && !UE_SERVER
			NativeEvents.Reserve(MaxNativeEvents);
#endif
		}

		~FNCShockInputTraceManager()
		{
			UnregisterNativeHandler();
		}

		void Start(AUTWeaponFix* Weapon)
		{
			if (Target.Get() == Weapon)
			{
				return;
			}
			if (Target.IsValid())
			{
				Stop(Target.Get());
			}
			else
			{
				// Heal an abnormal UObject teardown that bypassed the weapon lifecycle.
				// A stale native handler must never carry events into a new owner.
				UnregisterNativeHandler();
				ResetSession();
			}

			Target = Weapon;
			ResetSession();
			bNativeObserverAvailable = RegisterNativeHandler();
			UE_LOG(LogNCShockInputTrace, Warning,
				TEXT("[ShockInputTrace] attached weapon=%s native=%d mode=%d; observational only"),
				Weapon ? *Weapon->GetName() : TEXT("null"),
				bNativeObserverAvailable ? 1 : 0,
				NCShockInputTrace::GetMode());
		}

		void Stop(AUTWeaponFix* Weapon)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}

			// Freeze the producer before draining and summarizing. Otherwise a window
			// message can land between the final drain and handler removal and either
			// escape the summary or race the drop counter reset.
			UnregisterNativeHandler();
			DrainNativeEvents();
			if (RingCount > 0)
			{
				UE_LOG(LogNCShockInputTrace, Warning,
					TEXT("[ShockInputTrace] summary native=%u player=%u action=%u queue=%u weapon=%u shot=%u anomalies=%u nativeDrops=%u"),
					NativeDownCount, PlayerDownCount, ActionStartCount,
					QueueStartCount, WeaponStartCount, ShotCount,
					AnomalyCount, DroppedNativeEvents);
			}

			Target.Reset();
			ResetSession();
		}

		void Tick(AUTWeaponFix* Weapon)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}

			const uint32 Now = MonotonicMilliseconds();
			if (LastSweepMs != 0 && ElapsedMilliseconds(Now, LastSweepMs) < 20)
			{
				return;
			}
			LastSweepMs = Now;
			// Player/action callbacks drain immediately for exact same-frame
			// correlation. This fallback runs at 50 Hz only, so an enabled but idle
			// trace does not take a native-queue lock at the player's render rate.
			DrainNativeEvents();
			for (FNCShockTracePress& Press : Presses)
			{
				if (!Press.bEligible)
				{
					continue;
				}
				// Context can change between the edge and the delayed diagnosis (alt-tab,
				// console/menu, death/taunt/feign). Prefer missing a sample over turning
				// an intentional gameplay-input suppression into a broken-chain report.
				if (!IsGameplayEligible(Weapon))
				{
					Press.bEligible = false;
					continue;
				}

				if (Press.bNativeDown && !Press.bPlayerDown
					&& ElapsedMilliseconds(Now, Press.NativeDownMs) >= 100)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::NativeNoPlayerInput);
				}
				if (bNativeObserverAvailable && Press.bPlayerDown && !Press.bNativeDown
					&& ElapsedMilliseconds(Now, Press.PlayerDownMs) >= 100)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::PlayerInputNoNative);
				}
				if (Press.bPlayerDown && !Press.bActionStart
					&& ElapsedMilliseconds(Now, Press.PlayerDownMs) >= 100)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::PlayerInputNoAction);
				}
				if (Press.bActionStart && Press.bQueueEvidenceConclusive
					&& !Press.bQueueStart
					&& ElapsedMilliseconds(Now, Press.ActionStartMs) >= 50)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::ActionNoDeferredQueue);
				}
				if (Press.bQueueStart && !Press.bWeaponStart
					&& ElapsedMilliseconds(Now, Press.QueueStartMs) >= 150)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::DeferredQueueNoWeapon);
				}
				if (Press.bWeaponStart && Press.bExpectedImmediateShot && !Press.bShot
					&& ElapsedMilliseconds(Now, Press.WeaponStartMs) >= 125)
				{
					ReportAnomaly(Press, ENCShockTraceAnomaly::WeaponStartNoShot);
				}
			}

			for (int32 Index = Presses.Num() - 1; Index >= 0; --Index)
			{
				if (ElapsedMilliseconds(Now, Presses[Index].FirstTimeMs) >= PressLifetimeMs)
				{
					if (ActivePressId == Presses[Index].PressId)
					{
						ActivePressId = 0;
					}
					Presses.RemoveAt(Index, 1, false);
				}
			}
		}

		void RecordPlayerInput(AUTWeaponFix* Weapon, bool bPressed)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}
			DrainNativeEvents();
			const uint32 Now = MonotonicMilliseconds();

			FNCShockTracePress* Press = nullptr;
			uint16 Flags = 0;
			if (bPressed)
			{
				// UE dispatches multiple same-frame edges in OS event order. Native
				// events are all drained before the first delegate, so pair with the
				// oldest unmatched down rather than the newest one.
				for (int32 Index = 0; Index < Presses.Num(); ++Index)
				{
					FNCShockTracePress& Candidate = Presses[Index];
					if (Candidate.bNativeDown && Candidate.bEligible
						&& !Candidate.bPlayerDown
						&& ElapsedMilliseconds(Now, Candidate.NativeDownMs) <= CorrelationWindowMs)
					{
						Press = &Candidate;
						break;
					}
				}
				if (Press == nullptr)
				{
					Press = &AddSyntheticPress(Now);
					Press->bEligible = IsGameplayEligible(Weapon);
					Flags |= TraceSyntheticPress;
				}
				Press->bPlayerDown = true;
				Press->PlayerDownMs = Now;
				ActivePressId = Press->PressId;
				++PlayerDownCount;
			}
			else
			{
				Press = FindPress(ActivePressId);
				if (Press == nullptr)
				{
					Press = FindMostRecentPlayerPress();
				}
			}

			const uint32 PressId = Press ? Press->PressId : 0;
			if (Press && Press->bEligible)
			{
				Flags |= TraceEligible;
			}
			AddRecord(PressId,
				bPressed ? ENCShockTraceStage::PlayerInputDown
					: ENCShockTraceStage::PlayerInputUp,
				ENCShockTraceAnomaly::None, Flags, -1, 0.f,
				WeaponStateName(Weapon));
		}

		void RecordAction(AUTWeaponFix* Weapon, bool bPressed,
			bool bQueueTailMatched, bool bQueueEvidenceConclusive,
			int32 QueueDepth)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}
			DrainNativeEvents();
			const uint32 Now = MonotonicMilliseconds();
			FNCShockTracePress* Press = FindPress(ActivePressId);
			if (Press == nullptr)
			{
				Press = FindMostRecentPlayerPress();
			}
			if (bPressed && Press == nullptr)
			{
				Press = &AddSyntheticPress(Now);
				Press->bEligible = IsGameplayEligible(Weapon);
				ActivePressId = Press->PressId;
			}

			uint16 Flags = (Press && Press->bEligible) ? TraceEligible : 0;
			if (Press && !Press->bNativeDown)
			{
				Flags |= TraceSyntheticPress;
			}
			if (bQueueTailMatched)
			{
				Flags |= TraceQueueMatched;
			}

			const uint32 PressId = Press ? Press->PressId : 0;
			AddRecord(PressId,
				bPressed ? ENCShockTraceStage::ActionStart : ENCShockTraceStage::ActionStop,
				ENCShockTraceAnomaly::None, Flags, static_cast<int16>(
					FMath::Clamp(QueueDepth, -1, static_cast<int32>(MAX_int16))),
				0.f, WeaponStateName(Weapon));

			if (Press)
			{
				if (bPressed)
				{
					Press->bActionStart = true;
					Press->ActionStartMs = Now;
					Press->bQueueEvidenceConclusive = bQueueEvidenceConclusive;
					++ActionStartCount;
				}
				if (bQueueTailMatched)
				{
					if (bPressed)
					{
						Press->bQueueStart = true;
						Press->QueueStartMs = Now;
						++QueueStartCount;
					}
					AddRecord(PressId,
						bPressed ? ENCShockTraceStage::QueueStart
							: ENCShockTraceStage::QueueStop,
						ENCShockTraceAnomaly::None, Flags,
						static_cast<int16>(FMath::Clamp(QueueDepth, -1,
							static_cast<int32>(MAX_int16))),
						0.f, WeaponStateName(Weapon));
				}
			}

			if (!bPressed)
			{
				ActivePressId = 0;
			}
		}

		void RecordWeaponStart(AUTWeaponFix* Weapon, bool bRetry,
			bool bExpectedImmediateShot, float ReadyInMs, FName StateName)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}
			DrainNativeEvents();
			const uint32 Now = MonotonicMilliseconds();
			FNCShockTracePress* Press = nullptr;
			for (int32 Index = Presses.Num() - 1; Index >= 0; --Index)
			{
				FNCShockTracePress& Candidate = Presses[Index];
				if (Candidate.bQueueStart && !Candidate.bShot
					&& (!Candidate.bWeaponStart || bRetry)
					&& ElapsedMilliseconds(Now, Candidate.QueueStartMs) <= PressLifetimeMs)
				{
					Press = &Candidate;
					break;
				}
			}
			if (Press == nullptr)
			{
				Press = FindPress(ActivePressId);
			}

			uint16 Flags = (Press && Press->bEligible) ? TraceEligible : 0;
			Flags |= bRetry ? TraceRetry : 0;
			Flags |= bExpectedImmediateShot ? TraceExpectedImmediate : 0;
			AddRecord(Press ? Press->PressId : 0, ENCShockTraceStage::WeaponStart,
				ENCShockTraceAnomaly::None, Flags, -1, ReadyInMs, StateName);

			if (Press)
			{
				if (!Press->bWeaponStart)
				{
					Press->WeaponStartMs = Now;
					++WeaponStartCount;
				}
				Press->bWeaponStart = true;
				Press->bExpectedImmediateShot = Press->bExpectedImmediateShot
					|| bExpectedImmediateShot;
			}
		}

		void RecordWeaponStop(AUTWeaponFix* Weapon, bool bInternal, FName StateName)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}
			uint16 Flags = bInternal ? TraceInternalStop : 0;
			FNCShockTracePress* Press = FindPress(ActivePressId);
			if (Press && Press->bEligible)
			{
				Flags |= TraceEligible;
			}
			AddRecord(Press ? Press->PressId : 0, ENCShockTraceStage::WeaponStop,
				ENCShockTraceAnomaly::None, Flags, -1, 0.f, StateName);
		}

		void RecordFireShot(AUTWeaponFix* Weapon, FName StateName)
		{
			if (Target.Get() != Weapon)
			{
				return;
			}
			const uint32 Now = MonotonicMilliseconds();
			FNCShockTracePress* Press = nullptr;
			for (int32 Index = Presses.Num() - 1; Index >= 0; --Index)
			{
				if (Presses[Index].bWeaponStart && !Presses[Index].bShot
					&& ElapsedMilliseconds(Now, Presses[Index].WeaponStartMs) <= PressLifetimeMs)
				{
					Press = &Presses[Index];
					break;
				}
			}
			uint16 Flags = (Press && Press->bEligible) ? TraceEligible : 0;
			AddRecord(Press ? Press->PressId : 0, ENCShockTraceStage::FireShot,
				ENCShockTraceAnomaly::None, Flags, -1, 0.f, StateName);
			if (Press)
			{
				Press->bShot = true;
				Press->ShotMs = Now;
				++ShotCount;
			}
		}

#if PLATFORM_WINDOWS && !UE_SERVER
		virtual bool ProcessMessage(HWND Hwnd, uint32 Message, WPARAM WParam,
			LPARAM LParam, int32& OutResult) override
		{
			if (Message != WM_LBUTTONDOWN && Message != WM_LBUTTONDBLCLK
				&& Message != WM_LBUTTONUP)
			{
				return false;
			}

			FNCShockNativeEvent Event;
			Event.TimeMs = MonotonicMilliseconds();
			Event.bPressed = Message != WM_LBUTTONUP;
			Event.bFocused = ::GetForegroundWindow() == Hwnd;
			{
				FScopeLock Lock(&NativeQueueLock);
				if (Event.bPressed)
				{
					++NextNativePressId;
					if (NextNativePressId == 0 || (NextNativePressId & 0x80000000u) != 0)
					{
						NextNativePressId = 1;
					}
					HeldNativePressId = NextNativePressId;
					Event.PressId = HeldNativePressId;
				}
				else
				{
					Event.PressId = HeldNativePressId;
					HeldNativePressId = 0;
				}

				if (NativeEvents.Num() < MaxNativeEvents)
				{
					NativeEvents.Add(Event);
				}
				else
				{
					++DroppedNativeEvents;
				}
			}

			// This handler is a tap only. Slate and the game always receive the message.
			return false;
		}
#endif

	private:
		enum
		{
			RingCapacity = 128,
			MaxPresses = 16,
			MaxNativeEvents = 32,
				CorrelationWindowMs = 500,
			PressLifetimeMs = 3000
		};

		TWeakObjectPtr<AUTWeaponFix> Target;
		TArray<FNCShockTraceRecord> Ring;
		TArray<FNCShockTracePress> Presses;
		int32 RingHead;
		int32 RingCount;
		uint32 ActivePressId;
		uint32 SyntheticPressCounter;
		uint32 NativeDownCount;
		uint32 PlayerDownCount;
		uint32 ActionStartCount;
		uint32 QueueStartCount;
		uint32 WeaponStartCount;
		uint32 ShotCount;
		uint32 AnomalyCount;
		uint32 DroppedNativeEvents;
		uint32 LastSweepMs;
		bool bNativeObserverAvailable;

#if PLATFORM_WINDOWS && !UE_SERVER
		FCriticalSection NativeQueueLock;
		TArray<FNCShockNativeEvent> NativeEvents;
		TWeakPtr<GenericApplication> WeakPlatformApplication;
		uint32 NextNativePressId;
		uint32 HeldNativePressId;
		bool bNativeHandlerRegistered;
#endif

		void ResetSession()
		{
			RingHead = 0;
			RingCount = 0;
			Presses.Reset();
			ActivePressId = 0;
			SyntheticPressCounter = 0;
			NativeDownCount = 0;
			PlayerDownCount = 0;
			ActionStartCount = 0;
			QueueStartCount = 0;
			WeaponStartCount = 0;
			ShotCount = 0;
			AnomalyCount = 0;
			DroppedNativeEvents = 0;
			LastSweepMs = 0;
#if PLATFORM_WINDOWS && !UE_SERVER
			FScopeLock Lock(&NativeQueueLock);
			NativeEvents.Reset();
			NextNativePressId = 0;
			HeldNativePressId = 0;
#endif
		}

		static FName WeaponStateName(AUTWeaponFix* Weapon)
		{
			return Weapon && Weapon->GetCurrentState()
				? Weapon->GetCurrentState()->GetFName() : NAME_None;
		}

		static bool IsGameplayEligible(AUTWeaponFix* Weapon)
		{
			AUTCharacter* Owner = Weapon ? Weapon->GetUTOwner() : nullptr;
			AUTPlayerController* PC = Owner
				? Cast<AUTPlayerController>(Owner->Controller) : nullptr;
			if (Owner == nullptr || PC == nullptr || !PC->IsLocalController()
				|| PC->GetPawn() != Owner || Owner->GetWeapon() != Weapon
				|| Owner->IsDead() || Owner->IsPendingKillPending()
				|| Owner->TauntCount != 0 || Owner->IsFeigningDeath()
				|| !PC->IsInState(NAME_Playing) || PC->IsMoveInputIgnored()
				|| PC->ShouldShowMouseCursor())
			{
				return false;
			}

			UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
			return Viewport != nullptr && !Viewport->IgnoreInput()
				&& (Viewport->ViewportConsole == nullptr
					|| !Viewport->ViewportConsole->ConsoleActive());
		}

		FNCShockTracePress& AddSyntheticPress(uint32 Now)
		{
			++SyntheticPressCounter;
			uint32 PressId = 0x80000000u | SyntheticPressCounter;
			if (PressId == 0x80000000u)
			{
				SyntheticPressCounter = 1;
				PressId = 0x80000001u;
			}
			return AddPress(PressId, Now);
		}

		FNCShockTracePress& AddPress(uint32 PressId, uint32 Now)
		{
			if (Presses.Num() >= MaxPresses)
			{
				if (ActivePressId == Presses[0].PressId)
				{
					ActivePressId = 0;
				}
				Presses.RemoveAt(0, 1, false);
			}
			FNCShockTracePress Press;
			Press.PressId = PressId;
			Press.FirstTimeMs = Now;
			Presses.Add(Press);
			return Presses.Last();
		}

		FNCShockTracePress* FindPress(uint32 PressId)
		{
			if (PressId == 0)
			{
				return nullptr;
			}
			for (FNCShockTracePress& Press : Presses)
			{
				if (Press.PressId == PressId)
				{
					return &Press;
				}
			}
			return nullptr;
		}

		FNCShockTracePress* FindMostRecentPlayerPress()
		{
			for (int32 Index = Presses.Num() - 1; Index >= 0; --Index)
			{
				if (Presses[Index].bPlayerDown && !Presses[Index].bShot)
				{
					return &Presses[Index];
				}
			}
			return nullptr;
		}

		void AddRecord(uint32 PressId, ENCShockTraceStage Stage,
			ENCShockTraceAnomaly Anomaly, uint16 Flags, int16 QueueDepth,
			float ReadyInMs, FName StateName)
		{
			FNCShockTraceRecord& Record = Ring[RingHead];
			Record.TimeMs = MonotonicMilliseconds();
			Record.Frame = static_cast<uint32>(GFrameCounter);
			Record.PressId = PressId;
			Record.Stage = Stage;
			Record.Anomaly = Anomaly;
			Record.Flags = Flags;
			Record.QueueDepth = QueueDepth;
			Record.ReadyInMs = ReadyInMs;
			Record.WeaponState = StateName;
			RingHead = (RingHead + 1) % RingCapacity;
			RingCount = FMath::Min<int32>(RingCount + 1,
				static_cast<int32>(RingCapacity));

			if (NCShockInputTrace::GetMode() >= 2)
			{
				LogRecord(Record, TEXT("stage"));
			}
		}

		static void LogRecord(const FNCShockTraceRecord& Record, const TCHAR* Prefix)
		{
			UE_LOG(LogNCShockInputTrace, Warning,
				TEXT("[ShockInputTrace] %s t=%u frame=%u press=%u stage=%s anomaly=%s eligible=%d focused=%d synthetic=%d queueMatch=%d retry=%d expectedNow=%d internalStop=%d queueDepth=%d readyMs=%.2f state=%s"),
				Prefix, Record.TimeMs, Record.Frame, Record.PressId,
				StageName(Record.Stage), AnomalyName(Record.Anomaly),
				(Record.Flags & TraceEligible) ? 1 : 0,
				(Record.Flags & TraceFocused) ? 1 : 0,
				(Record.Flags & TraceSyntheticPress) ? 1 : 0,
				(Record.Flags & TraceQueueMatched) ? 1 : 0,
				(Record.Flags & TraceRetry) ? 1 : 0,
				(Record.Flags & TraceExpectedImmediate) ? 1 : 0,
				(Record.Flags & TraceInternalStop) ? 1 : 0,
				static_cast<int32>(Record.QueueDepth), Record.ReadyInMs,
				Record.WeaponState.IsNone() ? TEXT("none") : *Record.WeaponState.ToString());
		}

		void ReportAnomaly(FNCShockTracePress& Press, ENCShockTraceAnomaly Anomaly)
		{
			const uint16 Bit = static_cast<uint16>(1u << (static_cast<uint8>(Anomaly) - 1u));
			if ((Press.ReportedMask & Bit) != 0)
			{
				return;
			}
			Press.ReportedMask |= Bit;
			++AnomalyCount;
			AddRecord(Press.PressId, ENCShockTraceStage::Anomaly, Anomaly,
				TraceEligible, -1, 0.f, WeaponStateName(Target.Get()));

			UE_LOG(LogNCShockInputTrace, Warning,
				TEXT("[ShockInputTrace] CHAIN_GAP press=%u reason=%s native=%d player=%d action=%d queue=%d weapon=%d shot=%d; diagnostic evidence, not a verdict; dumping retained chain"),
				Press.PressId, AnomalyName(Anomaly), Press.bNativeDown ? 1 : 0,
				Press.bPlayerDown ? 1 : 0, Press.bActionStart ? 1 : 0,
				Press.bQueueStart ? 1 : 0, Press.bWeaponStart ? 1 : 0,
				Press.bShot ? 1 : 0);

			const int32 First = (RingHead - RingCount + RingCapacity) % RingCapacity;
			for (int32 Offset = 0; Offset < RingCount; ++Offset)
			{
				const FNCShockTraceRecord& Record = Ring[(First + Offset) % RingCapacity];
				if (Record.PressId == Press.PressId)
				{
					LogRecord(Record, TEXT("chain"));
				}
			}
		}

		void DrainNativeEvents()
		{
#if PLATFORM_WINDOWS && !UE_SERVER
			FNCShockNativeEvent LocalEvents[MaxNativeEvents];
			int32 LocalCount = 0;
			{
				FScopeLock Lock(&NativeQueueLock);
				LocalCount = FMath::Min<int32>(NativeEvents.Num(),
					static_cast<int32>(MaxNativeEvents));
				for (int32 Index = 0; Index < LocalCount; ++Index)
				{
					LocalEvents[Index] = NativeEvents[Index];
				}
				NativeEvents.Reset();
			}

			AUTWeaponFix* Weapon = Target.Get();
			for (int32 Index = 0; Index < LocalCount; ++Index)
			{
				const FNCShockNativeEvent& Event = LocalEvents[Index];
				FNCShockTracePress* Press = FindPress(Event.PressId);
				if (Event.bPressed)
				{
					if (Press == nullptr)
					{
						Press = &AddPress(Event.PressId, Event.TimeMs);
					}
					Press->bNativeDown = true;
					Press->NativeDownMs = Event.TimeMs;
					Press->bEligible = Event.bFocused
						&& IsGameplayEligible(Weapon);
					++NativeDownCount;
				}
				uint16 Flags = (Press && Press->bEligible) ? TraceEligible : 0;
				Flags |= Event.bFocused ? TraceFocused : 0;
				AddRecord(Event.PressId,
					Event.bPressed ? ENCShockTraceStage::NativeDown
						: ENCShockTraceStage::NativeUp,
					ENCShockTraceAnomaly::None, Flags, -1, 0.f,
					WeaponStateName(Weapon));
			}
#endif
		}

		bool RegisterNativeHandler()
		{
#if PLATFORM_WINDOWS && !UE_SERVER
			if (bNativeHandlerRegistered)
			{
				return true;
			}
			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}
			TSharedPtr<GenericApplication> PlatformApplication =
				FSlateApplication::Get().GetPlatformApplication();
			if (!PlatformApplication.IsValid())
			{
				return false;
			}
			WeakPlatformApplication = PlatformApplication;
			static_cast<FWindowsApplication*>(PlatformApplication.Get())
				->AddMessageHandler(*this);
			bNativeHandlerRegistered = true;
			return true;
#else
			return false;
#endif
		}

		void UnregisterNativeHandler()
		{
#if PLATFORM_WINDOWS && !UE_SERVER
			if (!bNativeHandlerRegistered)
			{
				return;
			}
			TSharedPtr<GenericApplication> PlatformApplication = WeakPlatformApplication.Pin();
			if (PlatformApplication.IsValid())
			{
				static_cast<FWindowsApplication*>(PlatformApplication.Get())
					->RemoveMessageHandler(*this);
			}
			WeakPlatformApplication.Reset();
			bNativeHandlerRegistered = false;
#endif
			bNativeObserverAvailable = false;
		}
	};

	static FNCShockInputTraceManager& TraceManager()
	{
		static FNCShockInputTraceManager Manager;
		return Manager;
	}
}

namespace NCShockInputTrace
{
	int32 GetMode()
	{
		return FMath::Clamp(CVarShockInputTrace.GetValueOnGameThread(), 0, 2);
	}

	void Start(AUTWeaponFix* Weapon)
	{
		TraceManager().Start(Weapon);
	}

	void Stop(AUTWeaponFix* Weapon)
	{
		TraceManager().Stop(Weapon);
	}

	void Tick(AUTWeaponFix* Weapon)
	{
		TraceManager().Tick(Weapon);
	}

	void RecordPlayerInput(AUTWeaponFix* Weapon, bool bPressed)
	{
		TraceManager().RecordPlayerInput(Weapon, bPressed);
	}

	void RecordAction(AUTWeaponFix* Weapon, bool bPressed,
		bool bQueueTailMatched, bool bQueueEvidenceConclusive,
		int32 QueueDepth)
	{
		TraceManager().RecordAction(Weapon, bPressed, bQueueTailMatched,
			bQueueEvidenceConclusive, QueueDepth);
	}

	void RecordWeaponStart(AUTWeaponFix* Weapon, bool bRetry,
		bool bExpectedImmediateShot, float ReadyInMs, FName StateName)
	{
		TraceManager().RecordWeaponStart(Weapon, bRetry,
			bExpectedImmediateShot, ReadyInMs, StateName);
	}

	void RecordWeaponStop(AUTWeaponFix* Weapon, bool bInternal, FName StateName)
	{
		TraceManager().RecordWeaponStop(Weapon, bInternal, StateName);
	}

	void RecordFireShot(AUTWeaponFix* Weapon, FName StateName)
	{
		TraceManager().RecordFireShot(Weapon, StateName);
	}
}
