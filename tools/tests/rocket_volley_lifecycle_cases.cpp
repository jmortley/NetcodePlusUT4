struct FLocalLoadedInputFixture
{
    APlayerController PC;
    AUTCharacter Pawn;
    AUTPlusWeap_RocketLauncher Weapon;

    FLocalLoadedInputFixture()
    {
        FPlatformTime::Now = 1000;
        Pawn.Controller = &PC;
        Pawn.Weapon = &Weapon;
        Weapon.UTOwner = &Pawn;
        Weapon.Role = 1;
    }
};

static void AssertNoLoadedInputWork(const FLocalLoadedInputFixture& F)
{
    assert(!F.Weapon.HasLoadedVolley() && F.Weapon.LastClientLoadedVolleyId == 0);
    assert(F.Weapon.SentBegins.Num() == 0 && F.Weapon.SentReleases.Num() == 0);
    assert(F.Weapon.BeginCalls == 0 && F.Weapon.Ammo == 9);
    assert(F.Weapon.NumLoadedRockets == 0 && !F.Pawn.Pending[1]);
    assert(F.Weapon.StockStartCalls == 0 && F.Weapon.StockStopCalls == 0);
}

static void AssertLoadedInputCleared(const FLocalLoadedInputFixture& F)
{
    assert(!F.Weapon.bPendingLoadedVolleyInput && !F.Weapon.bPendingLoadedVolleyRelease);
    assert(!F.Weapon.PendingLoadedVolleyInputHandle.Active);
    assert(F.Weapon.PendingLoadedVolleyPawn.Get() == nullptr);
    assert(F.Weapon.PendingLoadedVolleyWorld.Get() == nullptr);
    assert(F.Weapon.PendingLoadedVolleyController.Get() == nullptr);
}

static void TestFirstInputWaitsForOwnership()
{
    FLocalLoadedInputFixture F;
    F.Weapon.LoadedOwnershipEpoch = 0;
    F.Weapon.StartFire(1);
    AssertNoLoadedInputWork(F);
    assert(F.Weapon.bPendingLoadedVolleyInput && !F.Weapon.bPendingLoadedVolleyRelease);
    assert(F.Weapon.PendingLoadedVolleyPawn.Get() == &F.Pawn);
    assert(F.Weapon.PendingLoadedVolleyWorld.Get() == &F.Weapon.W);
    assert(F.Weapon.PendingLoadedVolleyController.Get() == &F.PC);
    assert(F.Weapon.PendingLoadedVolleyInputHandle.Active);
    const double RequestedAt = F.Weapon.PendingLoadedVolleyInputAt;
    FPlatformTime::Now += 0.05;
    F.Weapon.StartFire(1);
    F.Weapon.TryDrainLoadedVolleyInput();
    assert(F.Weapon.PendingLoadedVolleyInputAt == RequestedAt);
    AssertNoLoadedInputWork(F);

    F.Weapon.LoadedOwnershipEpoch = 7;
    F.Weapon.OnRep_LoadedOwnershipEpoch();
    AssertLoadedInputCleared(F);
    assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolleyEpoch == 7);
    assert(F.Weapon.LastClientLoadedVolleyId == 1 && F.Weapon.BeginCalls == 1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentReleases.Num() == 0);
    assert(F.Weapon.SentBegins[0].Epoch == 7 && F.Weapon.SentBegins[0].Id == 1);
    assert(F.Weapon.SentBegins[0].Pawn == &F.Pawn);
    F.Weapon.OnRep_LoadedOwnershipEpoch();
    F.Weapon.TryDrainLoadedVolleyInput();
    F.Weapon.StartFire(1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.BeginCalls == 1);
    F.Weapon.StopFire(1);
    assert(F.Weapon.SentReleases.Num() == 1 && F.Weapon.EndCalls == 1);
}

static void TestReleaseBeforeOwnershipIsRetained()
{
    for (bool Cooldown : {false, true})
    {
        FLocalLoadedInputFixture F;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        FPlatformTime::Now += 0.05;
        F.Weapon.StopFire(1);
        F.Weapon.StopFire(1);
        assert(F.Weapon.bPendingLoadedVolleyInput && F.Weapon.bPendingLoadedVolleyRelease);
        assert(F.Weapon.EndCalls == 0);
        AssertNoLoadedInputWork(F);
        const double RequestedAt = F.Weapon.PendingLoadedVolleyInputAt;
        F.Weapon.StartFire(1); // Repeated input does not erase the earlier release.
        assert(F.Weapon.bPendingLoadedVolleyRelease && F.Weapon.PendingLoadedVolleyInputAt == RequestedAt);
        F.Weapon.TestRemaining = Cooldown ? 0.1f : 0;
        F.Weapon.LoadedOwnershipEpoch = 8;
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        AssertLoadedInputCleared(F);
        assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentReleases.Num() == 1);
        const FTestLoadedInputRPC& Begin = F.Weapon.SentBegins[0];
        const FTestLoadedInputRPC& Release = F.Weapon.SentReleases[0];
        assert(Begin.Epoch == 8 && Begin.Id == 1 && Begin.Pawn == &F.Pawn);
        assert(Release.Epoch == Begin.Epoch && Release.Id == Begin.Id && Release.Pawn == Begin.Pawn);
        assert(Release.Count == 1 && Release.Mode == 0);
        assert(F.Weapon.bLoadedVolleyReleaseSent);
        if (Cooldown)
        {
            assert(F.Weapon.BeginCalls == 0 && F.Weapon.EndCalls == 0 && !F.Pawn.Pending[1]);
            assert(F.Weapon.LoadedVolleyBeginHandle.Active);
            assert(F.Weapon.InputEvents == std::vector<uint8>({1, 3}));
            F.Weapon.TestRemaining = 0;
            F.Weapon.TryBeginLoadedVolley();
        }
        else
        {
            assert(F.Weapon.EndCalls == 1 && !F.Pawn.Pending[1]);
            assert(F.Weapon.InputEvents == std::vector<uint8>({1, 2, 3, 4}));
        }
        F.Weapon.TryDrainLoadedVolleyInput();
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentReleases.Num() == 1);
        assert(F.Weapon.BeginCalls == 1 && F.Weapon.LastClientLoadedVolleyId == 1);
    }
}

static void TestOwnershipInputWindowDoesNotRefresh()
{
    FLocalLoadedInputFixture F;
    F.Weapon.LoadedOwnershipEpoch = 0;
    F.Weapon.StartFire(1);
    const double RequestedAt = F.Weapon.PendingLoadedVolleyInputAt;
    FPlatformTime::Now = RequestedAt + NCRocketVolley::OwnershipInputWindowSeconds - 0.01;
    F.Weapon.StartFire(1);
    F.Weapon.StopFire(1);
    assert(F.Weapon.bPendingLoadedVolleyInput && F.Weapon.PendingLoadedVolleyInputAt == RequestedAt);
    AssertNoLoadedInputWork(F);
    // Wall time expires while game time is paused; another press cannot extend it.
    FPlatformTime::Now = RequestedAt + NCRocketVolley::OwnershipInputWindowSeconds;
    F.Weapon.TryDrainLoadedVolleyInput();
    AssertLoadedInputCleared(F);
    AssertNoLoadedInputWork(F);
    F.Weapon.LoadedOwnershipEpoch = 9;
    F.Weapon.OnRep_LoadedOwnershipEpoch();
    F.Weapon.TryDrainLoadedVolleyInput();
    AssertNoLoadedInputWork(F);
    F.Weapon.StartFire(1); // Expiration does not poison the next real input.
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.BeginCalls == 1);
}

static void TestEpochReadyPollBeforeNotify()
{
    FLocalLoadedInputFixture F;
    F.Weapon.LoadedOwnershipEpoch = 0;
    F.Weapon.StartFire(1);
    F.Weapon.StopFire(1);
    F.Weapon.LoadedOwnershipEpoch = 9;
    F.Weapon.TryDrainLoadedVolleyInput();
    F.Weapon.OnRep_LoadedOwnershipEpoch();
    F.Weapon.TryDrainLoadedVolleyInput();
    AssertLoadedInputCleared(F);
    assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolleyEpoch == 9);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentReleases.Num() == 1);
    assert(F.Weapon.BeginCalls == 1 && F.Weapon.EndCalls == 1);
    assert(F.Weapon.LastClientLoadedVolleyId == 1);
}

static void TestRetryInputDoesNotReplaceFreshIntent()
{
    FLocalLoadedInputFixture F;
    F.Weapon.LoadedOwnershipEpoch = 0;
    F.Weapon.bHandlingRetry = true;
    F.Weapon.StartFire(1);
    AssertLoadedInputCleared(F);
    AssertNoLoadedInputWork(F);
    F.Weapon.bHandlingRetry = false;
    F.Weapon.StartFire(1);
    F.Weapon.StopFire(1);
    const double RequestedAt = F.Weapon.PendingLoadedVolleyInputAt;
    FPlatformTime::Now += 0.05;
    F.Weapon.bHandlingRetry = true;
    F.Weapon.StartFire(0); // An older primary retry cannot erase this later alt input.
    assert(F.Weapon.StockStartCalls == 1);
    assert(F.Weapon.bPendingLoadedVolleyInput && F.Weapon.bPendingLoadedVolleyRelease);
    assert(F.Weapon.PendingLoadedVolleyInputAt == RequestedAt);
    assert(F.Weapon.PendingLoadedVolleyInputHandle.Active && F.Weapon.SentBegins.Num() == 0);
    F.Weapon.bHandlingRetry = false;
    F.Weapon.StartFire(0); // A new physical primary press does replace the intent.
    AssertLoadedInputCleared(F);
    assert(F.Weapon.StockStartCalls == 2);
    F.Weapon.LoadedOwnershipEpoch = 14;
    F.Weapon.OnRep_LoadedOwnershipEpoch();
    assert(F.Weapon.SentBegins.Num() == 0 && F.Weapon.BeginCalls == 0);
}

static void TestFreshPressAfterExpiredInput()
{
    for (bool Ready : {false, true})
    {
        FLocalLoadedInputFixture F;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        F.Weapon.StopFire(1);
        FPlatformTime::Now += NCRocketVolley::OwnershipInputWindowSeconds;
        if (Ready) F.Weapon.LoadedOwnershipEpoch = 15;
        F.Weapon.StartFire(1); // This is a new press, with the old expiry still queued.
        if (!Ready)
        {
            assert(F.Weapon.bPendingLoadedVolleyInput && !F.Weapon.bPendingLoadedVolleyRelease);
            assert(F.Weapon.PendingLoadedVolleyInputAt == FPlatformTime::Now);
            AssertNoLoadedInputWork(F);
            F.Weapon.LoadedOwnershipEpoch = 15;
            F.Weapon.OnRep_LoadedOwnershipEpoch();
        }
        AssertLoadedInputCleared(F);
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolleyEpoch == 15);
        assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.BeginCalls == 1);
        assert(F.Weapon.SentReleases.Num() == 0 && F.Weapon.EndCalls == 0);
        assert(!F.Weapon.bLoadedVolleyReleaseSent && F.Pawn.Pending[1]);
    }
}

static void TestBufferedReleaseDoesNotFollowBeginReentry()
{
    for (int Replacement = 0; Replacement < 10; ++Replacement)
    {
        FLocalLoadedInputFixture F;
        AUTCharacter OtherPawn;
        APlayerController OtherController;
        AUTPlusWeap_RocketLauncher OtherWeapon;
        UWorld OtherWorld;
        OtherPawn.Controller = &OtherController;
        OtherPawn.Weapon = &F.Weapon;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        F.Weapon.StopFire(1);
        // Inject at state dispatch, after TryBegin's checks, to exercise the
        // drain's post-dispatch identity guard rather than earlier validation.
        F.Weapon.TestOnBegin = [&]()
        {
            assert(!F.Weapon.bPendingLoadedVolleyInput);
            switch (Replacement)
            {
                case 0: F.Weapon.LoadedVolley.Id = 40; break;
                case 1: F.Weapon.LoadedVolleyEpoch = 40; break;
                case 2: F.Weapon.LoadedOwnershipEpoch = 40; break;
                case 3:
                    F.Weapon.UTOwner = &OtherPawn;
                    F.Weapon.LoadedVolleyPawn = &OtherPawn;
                    break;
                case 4: F.Pawn.Weapon = &OtherWeapon; break;
                case 5: F.Pawn.PendingWeapon = &OtherWeapon; break;
                case 6: F.Pawn.Controller = &OtherController; break;
                case 7: F.Weapon.TestWorld = &OtherWorld; break;
                case 8: F.Pawn.Dead = true; break;
                case 9: F.Pawn.Disabled = true; break;
            }
        };
        F.Weapon.LoadedOwnershipEpoch = 16;
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        AssertLoadedInputCleared(F);
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.BeginCalls == 1);
        assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentBegins[0].Id == 1);
        assert(F.Weapon.SentReleases.Num() == 0 && F.Weapon.EndCalls == 0);
        assert(!F.Weapon.bLoadedVolleyReleaseSent);
    }
}

static void TestPendingInputInvalidation()
{
    for (int Change = 0; Change < 18; ++Change)
    {
        FLocalLoadedInputFixture F;
        AUTCharacter OtherPawn;
        APlayerController OtherController;
        AUTPlusWeap_RocketLauncher OtherWeapon;
        UWorld OtherWorld;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        assert(F.Weapon.bPendingLoadedVolleyInput);
        switch (Change)
        {
            case 0: F.Pawn.Dead = true; break;
            case 1: F.Pawn.Disabled = true; break;
            case 2: F.Pawn.Weapon = &OtherWeapon; break;
            case 3: F.Pawn.PendingWeapon = &OtherWeapon; break;
            case 4: F.Weapon.Ammo = 0; break;
            case 5: F.Weapon.CurrentState = F.Weapon.InactiveState; break;
            case 6: F.Weapon.CurrentState = F.Weapon.UnequippingState; break;
            case 7: F.Weapon.TestWorld = nullptr; break;
            case 8: F.Weapon.TestWorld = &OtherWorld; break;
            case 9: F.Weapon.UTOwner = nullptr; break;
            case 10: F.Weapon.UTOwner = &OtherPawn; break;
            case 11: F.Pawn.Controller = &OtherController; break;
            case 12: F.Pawn.Controller = nullptr; break;
            case 13: F.Pawn.Local = false; break;
            case 14: F.Weapon.Role = ROLE_Authority; break;
            case 15: F.Weapon.W.GS.Block = true; break;
            case 16: F.Weapon.bDisableAltLoading = true; break;
            case 17: F.Weapon.CurrentState = nullptr; break;
        }
        const int OriginalWorldClears = F.Weapon.W.TimerManager.ClearCalls;
        F.Weapon.TryDrainLoadedVolleyInput();
        AssertLoadedInputCleared(F);
        assert(F.Weapon.W.TimerManager.ClearCalls == OriginalWorldClears + 1);
        assert(OtherWorld.TimerManager.ClearCalls == 0);
        // Restore prerequisites before epoch arrival: cancelled input stays cancelled.
        F.Pawn.Dead = F.Pawn.Disabled = false;
        F.Pawn.Weapon = &F.Weapon;
        F.Pawn.PendingWeapon = nullptr;
        F.Pawn.Controller = &F.PC;
        F.Pawn.Local = true;
        F.Weapon.Ammo = 9;
        F.Weapon.CurrentState = F.Weapon.ActiveState;
        F.Weapon.TestWorld = &F.Weapon.W;
        F.Weapon.UTOwner = &F.Pawn;
        F.Weapon.Role = 1;
        F.Weapon.W.GS.Block = false;
        F.Weapon.bDisableAltLoading = false;
        F.Weapon.LoadedOwnershipEpoch = 10;
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        AssertNoLoadedInputWork(F);
    }
}

static void TestOnlyLocalClientBuffersInput()
{
    for (int Kind = 0; Kind < 4; ++Kind)
    {
        FLocalLoadedInputFixture F;
        F.Weapon.LoadedOwnershipEpoch = 0;
        if (Kind == 0) F.Weapon.Role = ROLE_Authority; // Listen host.
        if (Kind == 1) {F.Weapon.Role = ROLE_Authority; F.Pawn.Local = false;} // Remote server pawn.
        if (Kind == 2) F.Pawn.Local = false; // Simulated proxy.
        if (Kind == 3) F.Pawn.Controller = nullptr; // Unpossessed owner.
        F.Weapon.StartFire(1);
        AssertLoadedInputCleared(F);
        AssertNoLoadedInputWork(F);
    }
}

static void TestPendingInputLifecycleCancellation()
{
    for (int Hook = 0; Hook < 10; ++Hook)
    {
        FLocalLoadedInputFixture F;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        F.Weapon.StopFire(1);
        assert(F.Weapon.bPendingLoadedVolleyInput && F.Weapon.bPendingLoadedVolleyRelease);
        switch (Hook)
        {
            case 0: F.Weapon.Removed(); break;
            case 1: F.Weapon.GivenTo(&F.Pawn, true); break;
            case 2: F.Weapon.ClientGivenTo_Internal(true); break;
            case 3: assert(F.Weapon.PutDown()); break;
            case 4: F.Weapon.DetachFromOwner_Implementation(); break;
            case 5: F.Weapon.Destroyed(); break;
            case 6: F.Weapon.ResetLoadedOwnershipState(); break;
            case 7:
                F.Weapon.CurrentState = F.Weapon.InactiveState;
                F.Weapon.StateChanged();
                break;
            case 8:
                F.Weapon.CurrentState = F.Weapon.UnequippingState;
                F.Weapon.StateChanged();
                break;
            case 9:
                F.Weapon.EndPlay(EEndPlayReason::LevelTransition);
                assert(F.Weapon.TestEndPlayReason == EEndPlayReason::LevelTransition);
                break;
        }
        AssertLoadedInputCleared(F);
        assert(!F.Weapon.BaseSawPendingInput);
        if (Hook <= 5 || Hook == 9) assert(F.Weapon.LifecycleCalls == 1);
        F.Weapon.UTOwner = &F.Pawn;
        F.Weapon.CurrentState = F.Weapon.ActiveState;
        F.Weapon.LoadedOwnershipEpoch = 11;
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        F.Weapon.TryDrainLoadedVolleyInput();
        AssertNoLoadedInputWork(F);
    }

    FLocalLoadedInputFixture Primary;
    Primary.Weapon.LoadedOwnershipEpoch = 0;
    Primary.Weapon.StartFire(1);
    Primary.Weapon.StartFire(0);
    AssertLoadedInputCleared(Primary);
    assert(Primary.Weapon.StockStartCalls == 1);
    Primary.Weapon.LoadedOwnershipEpoch = 12;
    Primary.Weapon.OnRep_LoadedOwnershipEpoch();
    assert(Primary.Weapon.SentBegins.Num() == 0 && Primary.Weapon.BeginCalls == 0);

    // An epoch replacement only preserves intent for the first zero-to-ready transition.
    FLocalLoadedInputFixture Replacement;
    Replacement.Weapon.LoadedVolleyEpoch = 6;
    Replacement.Weapon.LoadedOwnershipEpoch = 0;
    Replacement.Weapon.StartFire(1);
    Replacement.Weapon.LoadedOwnershipEpoch = 7;
    Replacement.Weapon.OnRep_LoadedOwnershipEpoch();
    AssertLoadedInputCleared(Replacement);
    AssertNoLoadedInputWork(Replacement);
}

static void TestReadyInputPath()
{
    FLocalLoadedInputFixture F;
    AssertNoLoadedInputWork(F);
    F.Weapon.LoadedOwnershipEpoch = 7;
    F.Weapon.StartFire(1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.BeginCalls == 1);
    assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolleyEpoch == 7);
    assert(F.Weapon.SentBegins[0].Epoch == 7 && F.Weapon.SentBegins[0].Id == 1);
    assert(F.Weapon.SentBegins[0].Pawn == &F.Pawn && F.Pawn.Pending[1]);
    F.Weapon.StartFire(1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.BeginCalls == 1);
    F.Weapon.StopFire(1);
    assert(F.Weapon.SentReleases.Num() == 1 && F.Weapon.EndCalls == 1);
    assert(F.Weapon.SentReleases[0].Epoch == 7 && F.Weapon.SentReleases[0].Id == 1);
    assert(F.Weapon.SentReleases[0].Count == 1 && !F.Pawn.Pending[1]);
    assert(F.Weapon.InputEvents == std::vector<uint8>({1, 2, 3, 4}));
    F.Weapon.StartFire(0);
    F.Weapon.StopFire(0);
    assert(F.Weapon.StockStartCalls == 1 && F.Weapon.StockStopCalls == 1);

    FLocalLoadedInputFixture Delayed;
    Delayed.Weapon.TestRemaining = 0.2f;
    Delayed.Weapon.StartFire(1);
    Delayed.Weapon.StopFire(1);
    assert(Delayed.Weapon.SentBegins.Num() == 1 && Delayed.Weapon.SentReleases.Num() == 1);
    assert(Delayed.Weapon.BeginCalls == 0 && Delayed.Weapon.EndCalls == 0);
    assert(Delayed.Weapon.LoadedVolleyBeginHandle.Active && !Delayed.Pawn.Pending[1]);
    Delayed.Weapon.TestRemaining = 0;
    Delayed.Weapon.TryBeginLoadedVolley();
    assert(Delayed.Weapon.BeginCalls == 1 && Delayed.Weapon.bLoadedVolleyReleaseSent);
}

static void TestThirdLoadReleaseCounts()
{
    // These are completed-load snapshots on either side of the third callback,
    // not a simulated network/timer scheduler. Exercise the actual release,
    // count commit, spawn, receipt and reconciliation methods for every pairing.
    for (uint8 ClientCount = 2; ClientCount <= 3; ++ClientCount)
    {
        for (uint8 ServerCount = 2; ServerCount <= 3; ++ServerCount)
        {
            APlayerController PC;
            AUTCharacter Pawn;
            Pawn.Controller = &PC;
            AUTPlusWeap_RocketLauncher Server, Client;
            Server.UTOwner = Client.UTOwner = &Pawn;
            Client.Role = 1;
            Server.CurrentState = &Server.Charge;
            Client.CurrentState = &Client.Charge;
            Server.ResetLoadedVolley(10);
            Client.ResetLoadedVolley(10);
            Server.NumLoadedRockets = ServerCount;
            Client.NumLoadedRockets = ClientCount;
            Server.Ammo = 9 - ServerCount;
            Server.LoadedVolleyAmmoSpent = ServerCount;
            Pawn.Weapon = &Server;
            Server.ServerReleaseLoadedVolley_Implementation(1, 10, &Pawn, 0, ClientCount);
            assert(Server.CommitLoadedVolley());
            const uint8 ExpectedCount = std::min(ClientCount, ServerCount);
            assert(Server.LoadedVolley.Count == ExpectedCount);
            assert(Server.NumLoadedRockets == ExpectedCount);
            assert(Server.Refund == ServerCount - ExpectedCount);
            assert(Server.Ammo == 9 - ExpectedCount);
            const FNCLoadedVolleyReceipt Accepted = Server.SentVolleys.Last();
            assert(Accepted.Result == uint8(NCRocketVolley::EResult::Accepted));
            assert(Accepted.Count == ExpectedCount && Accepted.SpawnedMask == 0);

            Pawn.Weapon = &Client;
            assert(Client.CommitLoadedVolley());
            AUTPlusProj_Rocket Fakes[3], Reals[3];
            for (uint8 Ordinal = 0; Ordinal < ClientCount; ++Ordinal)
            {
                Fakes[Ordinal].Instigator = &Pawn;
                Client.TestNextProjectile = &Fakes[Ordinal];
                assert(Client.SpawnNetPredictedProjectile({}, {}, {}) == &Fakes[Ordinal]);
                assert(Fakes[Ordinal].LoadedRocketOrdinal == Ordinal);
                assert(!Client.TestAllowDelay && !Fakes[Ordinal].Dead);
            }
            Client.ClientLoadedVolleyResult_Implementation(1, 10, &Pawn,
                Accepted.Result, Accepted.Count, Accepted.SpawnedMask);
            for (uint8 Ordinal = 0; Ordinal < ClientCount; ++Ordinal)
                assert(Fakes[Ordinal].Dead == (Ordinal >= ExpectedCount));

            Pawn.Weapon = &Server;
            for (uint8 Ordinal = 0; Ordinal < ExpectedCount; ++Ordinal)
            {
                Reals[Ordinal].Instigator = &Pawn;
                Server.TestNextProjectile = &Reals[Ordinal];
                assert(Server.SpawnNetPredictedProjectile({}, {}, {}) == &Reals[Ordinal]);
                assert(Server.SentRockets[Ordinal] == Ordinal);
                assert(Server.SentRocketResults[Ordinal] == uint8(NCRocketVolley::ERocketResult::Spawned));
            }
            assert(Server.SpawnNetPredictedProjectile({}, {}, {}) == nullptr);
            assert(Server.TestSpawnCalls == ExpectedCount);
            Server.CompleteLoadedVolley(false);
            const FNCLoadedVolleyReceipt Completed = Server.SentVolleys.Last();
            assert(Completed.Count == ExpectedCount);
            assert(Completed.SpawnedMask == NCRocketVolley::CountMask(ExpectedCount));

            // Completion can arrive before any of the actor references map.
            Client.ClientLoadedVolleyResult_Implementation(1, 10, &Pawn,
                Completed.Result, Completed.Count, Completed.SpawnedMask);
            for (int Ordinal = ExpectedCount - 1; Ordinal >= 0; --Ordinal)
            {
                assert(!Fakes[Ordinal].Dead && !Fakes[Ordinal].MasterProjectile);
                Client.ClientLoadedRocketResult_Implementation(1, 10, &Pawn, uint8(Ordinal),
                    uint8(NCRocketVolley::ERocketResult::Spawned), &Reals[Ordinal], 0);
                assert(Fakes[Ordinal].MasterProjectile == &Reals[Ordinal]);
                assert(Reals[Ordinal].MyFakeProjectile == &Fakes[Ordinal]);
                assert(Reals[Ordinal].PairCalls == 1 && Fakes[Ordinal].DestroyCalls == 0);
            }
            if (ClientCount > ExpectedCount)
                assert(Fakes[2].DestroyCalls == 1); // Only the unauthorized third retires.
        }
    }
}

static void TestMixedCloseRangeOutcomes()
{
    const uint8 Orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    for (const auto& Order : Orders)
    {
        APlayerController PC;
        AUTCharacter Pawn;
        Pawn.Controller = &PC;
        AUTPlusWeap_RocketLauncher Client;
        Client.Role = 1;
        Client.UTOwner = &Pawn;
        Pawn.Weapon = &Client;
        Client.ResetLoadedVolley(20);
        assert(Client.LoadedVolley.Release(20, 3));
        AUTPlusProj_Rocket Fakes[3], Real;
        Real.Instigator = &Pawn;
        for (uint8 Ordinal = 0; Ordinal < 3; ++Ordinal)
        {
            Fakes[Ordinal].Instigator = &Pawn;
            Client.TestNextProjectile = &Fakes[Ordinal];
            assert(Client.SpawnNetPredictedProjectile({}, {}, {}) == &Fakes[Ordinal]);
        }
        // Rocket 0 already impacted, 1 failed to spawn, and 2 is still flying.
        // A resolved close-range shot was spawned, so it remains in mask 0b101.
        Client.ClientLoadedVolleyResult_Implementation(1, 20, &Pawn,
            uint8(NCRocketVolley::EResult::Completed), 3, 5);
        assert(!Fakes[0].Dead && Fakes[1].Dead && !Fakes[2].Dead);
        const NCRocketVolley::ERocketResult Outcomes[3] = {
            NCRocketVolley::ERocketResult::Resolved,
            NCRocketVolley::ERocketResult::Rejected,
            NCRocketVolley::ERocketResult::Spawned
        };
        for (uint8 Ordinal : Order)
        {
            for (int Duplicate = 0; Duplicate < 2; ++Duplicate)
                Client.ClientLoadedRocketResult_Implementation(1, 20, &Pawn, Ordinal,
                    uint8(Outcomes[Ordinal]), Ordinal == 2 ? &Real : nullptr, 0);
            assert(!Fakes[2].Dead); // Another ordinal's impact cannot retire this visual.
        }
        assert(Fakes[0].DestroyCalls == 1 && Fakes[1].DestroyCalls == 1);
        assert(Fakes[2].DestroyCalls == 0 && Real.PairCalls == 1);
        assert(Fakes[2].MasterProjectile == &Real && Real.MyFakeProjectile == &Fakes[2]);
    }
}

int main()
{
    TestFirstInputWaitsForOwnership();
    TestReleaseBeforeOwnershipIsRetained();
    TestOwnershipInputWindowDoesNotRefresh();
    TestEpochReadyPollBeforeNotify();
    TestRetryInputDoesNotReplaceFreshIntent();
    TestFreshPressAfterExpiredInput();
    TestBufferedReleaseDoesNotFollowBeginReentry();
    TestPendingInputInvalidation();
    TestOnlyLocalClientBuffersInput();
    TestPendingInputLifecycleCancellation();
    TestReadyInputPath();
    TestThirdLoadReleaseCounts();
    TestMixedCloseRangeOutcomes();
    APlayerController PC;AUTCharacter Pawn;Pawn.Controller=&PC;
    AUTPlusWeap_RocketLauncher Server;Server.UTOwner=&Pawn;Pawn.Weapon=&Server;
    Server.LoadedOwnershipEpoch=0;
    assert(!Server.CanBeginLoadedVolley()); // No prediction/charge before ownership identity is known.
    Server.LoadedOwnershipEpoch=1;
    PC.Confirmed=false;Server.ServerBeginLoadedVolley_Implementation(1,1,&Pawn);
    assert(!Server.HasLoadedVolley());
    PC.Confirmed=true;Server.Ammo=0;Server.ServerBeginLoadedVolley_Implementation(1,1,&Pawn);
    assert(!Server.HasLoadedVolley()&&Server.SentVolleys.Last().Result==uint8(NCRocketVolley::EResult::Rejected));
    Server.Ammo=9;Server.TestRemaining=0.2f;Server.ServerBeginLoadedVolley_Implementation(1,2,&Pawn);
    assert(Server.HasLoadedVolley()&&Server.BeginCalls==0&&Server.LoadedVolleyBeginHandle.Active);
    assert(!Pawn.Pending[1]); // Generic Active-state cannot start early.
    const float OriginalRequestTime=Server.LoadedVolleyBeginRequestedAt;
    Server.W.Now+=0.05f;Server.ServerBeginLoadedVolley_Implementation(1,2,&Pawn);
    assert(Server.LoadedVolleyBeginRequestedAt==OriginalRequestTime&&Server.BeginCalls==0);
    Server.ServerReleaseLoadedVolley_Implementation(1,2,&Pawn,1,2);
    assert(Server.bLoadedVolleyReleaseReceived&&Server.EndCalls==0); // Release before queued state entry.
    Server.TestRemaining=0;Server.W.Now+=0.2f;Server.TryBeginLoadedVolley();
    assert(Server.BeginCalls==1&&Server.bLoadedVolleyEnteredState);
    Server.NumLoadedRockets=3;Server.LoadedVolleyAmmoSpent=3;Server.CurrentRocketFireMode=1;
    assert(Server.CommitLoadedVolley());
    assert(Server.NumLoadedRockets==2&&Server.Refund==1&&Server.LoadedVolley.Count==2);
    Server.ServerReleaseLoadedVolley_Implementation(1,2,&Pawn,0,3);
    assert(Server.CurrentRocketFireMode==1&&Server.LoadedVolley.Count==2); // Immutable release.
    assert(Server.LoadedVolley.Resolve(2,0,true));Server.CompleteLoadedVolley(true);
    assert(Server.SentRockets.Num()==1&&Server.SentRockets[0]==1);
    assert(Server.SentVolleys.Last().SpawnedMask==1); // Preserve already fired ordinal on cancellation.
    Server.CurrentState=Server.ActiveState;Server.ServerBeginLoadedVolley_Implementation(1,3,&Pawn);
    AUTCharacter OldPawn;
    Server.ServerSetLoadedRocketMode_Implementation(1,3,&OldPawn,1);
    assert(!Server.bLoadedVolleyReleaseReceived);
    Server.ServerReleaseLoadedVolley_Implementation(1,3,&OldPawn,1,3);
    assert(!Server.bLoadedVolleyReleaseReceived); // Same ID from another pawn cannot release.
    Server.ServerReleaseLoadedVolley_Implementation(1,2,&Pawn,0,1);
    assert(Server.LoadedVolley.Id==3&&!Server.bLoadedVolleyReleaseReceived);

    AUTPlusWeap_RocketLauncher Client;Client.Role=1;Client.UTOwner=&Pawn;Pawn.Weapon=&Client;
    Client.ResetLoadedVolley(100);Client.CurrentState=&Client.Charge;
    AUTProjectile Fakes[3],Real;Real.Instigator=&Pawn;
    for(uint8 i=0;i<3;++i){Fakes[i].Instigator=&Pawn;Client.FindOrAddLoadedRocket(1,100,i).Fake=&Fakes[i];}
    Client.ClientLoadedVolleyResult_Implementation(1,100,&Pawn,uint8(NCRocketVolley::EResult::Accepted),3,0);
    assert(!Fakes[0].Dead&&!Fakes[1].Dead&&!Fakes[2].Dead);
    Client.ClientLoadedRocketResult_Implementation(1,100,&Pawn,1,uint8(NCRocketVolley::ERocketResult::Spawned),&Real,0);
    assert(Real.MyFakeProjectile==&Fakes[1]&&Fakes[1].MasterProjectile==&Real&&Real.PairCalls==1);
    Client.ClientLoadedVolleyResult_Implementation(1,100,&Pawn,uint8(NCRocketVolley::EResult::Completed),3,7);
    assert(!Fakes[0].Dead&&!Fakes[1].Dead&&!Fakes[2].Dead); // Completion is not visual deletion.
    Client.LoadedVolley.Finish(100);Client.ResetLoadedVolley(101);
    Client.ClientLoadedVolleyResult_Implementation(1,100,&Pawn,uint8(NCRocketVolley::EResult::Rejected),0,0);
    assert(Client.HasLoadedVolley()&&Client.LoadedVolley.Id==101&&Client.GotoCalls==0);
    assert(!Fakes[1].Dead); // Even conflicting old terminal data cannot erase paired visuals.
    AUTProjectile LateFake,LateReal;LateFake.Instigator=LateReal.Instigator=&Pawn;
    Client.ClientLoadedRocketResult_Implementation(1,101,&Pawn,0,uint8(NCRocketVolley::ERocketResult::Spawned),&LateReal,0);
    Client.FindOrAddLoadedRocket(1,101,0).Fake=&LateFake;Client.ReconcileLoadedRockets();
    assert(LateReal.MyFakeProjectile==&LateFake); // Receipt before predicted spawn.
    AUTProjectile UnknownFake;UnknownFake.Instigator=&Pawn;Client.FindOrAddLoadedRocket(1,101,1).Fake=&UnknownFake;
    Client.ClientLoadedRocketResult_Implementation(1,101,&Pawn,1,uint8(NCRocketVolley::ERocketResult::Spawned),nullptr,0);
    assert(!UnknownFake.Dead); // Unresolved actor reference is not rejection.
    AUTProjectile LaterMapped;LaterMapped.Instigator=&Pawn;
    Client.ClientLoadedRocketResult_Implementation(1,101,&Pawn,1,uint8(NCRocketVolley::ERocketResult::Spawned),&LaterMapped,0);
    assert(UnknownFake.MasterProjectile==&LaterMapped); // Identity resolves it later without another deletion.
    Client.ClientLoadedRocketResult_Implementation(1,101,&Pawn,2,uint8(NCRocketVolley::ERocketResult::Cancelled),nullptr,0);
    AUTProjectile CancelledFake;Client.FindOrAddLoadedRocket(1,101,2).Fake=&CancelledFake;Client.ReconcileLoadedRockets();
    assert(CancelledFake.Dead); // Exact cancellation received before fake spawn still applies.
    Client.ClientLoadedVolleyResult_Implementation(1,101,&OldPawn,uint8(NCRocketVolley::EResult::Rejected),0,0);
    assert(Client.HasLoadedVolley());

    AUTPlusWeap_RocketLauncher BurstClient;BurstClient.Role=1;BurstClient.UTOwner=&Pawn;Pawn.Weapon=&BurstClient;
    BurstClient.ResetLoadedVolley(1);assert(BurstClient.LoadedVolley.Release(1,3));
    AUTPlusProj_Rocket BurstFakes[3];
    for(uint8 i=0;i<3;++i){
        BurstFakes[i].Instigator=&Pawn;BurstClient.TestNextProjectile=&BurstFakes[i];
        assert(BurstClient.SpawnNetPredictedProjectile({}, {}, {})==&BurstFakes[i]);
        assert(BurstFakes[i].LoadedVolleyId==1&&BurstFakes[i].LoadedRocketOrdinal==i);
        assert(!BurstClient.TestAllowDelay); // Three rockets never overwrite the old one-slot delay.
        assert(BurstClient.PendingFakeProjectiles.Num()==0&&PC.FakeProjectiles.Num()==0);
    }
    assert(BurstClient.LoadedRocketPredictions.Num()==3&&BurstClient.LoadedVolley.SpawnedMask==7);
    assert(BurstClient.SpawnNetPredictedProjectile({}, {}, {})==nullptr&&BurstClient.TestSpawnCalls==3);

    AUTPlusWeap_RocketLauncher BurstServer;BurstServer.UTOwner=&Pawn;Pawn.Weapon=&BurstServer;
    BurstServer.ResetLoadedVolley(9);assert(BurstServer.LoadedVolley.Release(9,3));
    AUTPlusProj_Rocket RealRocket,ImmediateImpact;
    BurstServer.TestNextProjectile=&RealRocket;BurstServer.SpawnNetPredictedProjectile({}, {}, {});
    BurstServer.TestNextProjectile=&ImmediateImpact;BurstServer.TestImmediateImpact=true;
    assert(BurstServer.SpawnNetPredictedProjectile({}, {}, {})==nullptr);
    BurstServer.TestNextProjectile=nullptr;BurstServer.SpawnNetPredictedProjectile({}, {}, {});
    assert(BurstServer.SentRocketResults.Num()==3);
    assert(BurstServer.SentRocketResults[0]==uint8(NCRocketVolley::ERocketResult::Spawned));
    assert(BurstServer.SentRocketResults[1]==uint8(NCRocketVolley::ERocketResult::Resolved));
    assert(BurstServer.SentRocketResults[2]==uint8(NCRocketVolley::ERocketResult::Rejected));
    assert(BurstServer.LoadedVolley.SpawnedMask==3&&BurstServer.LoadedVolley.ResolvedMask==7);
    AUTPlusWeap_RocketLauncher Revoked;Revoked.UTOwner=&Pawn;Pawn.Weapon=&Revoked;Revoked.TestRemaining=0.3f;
    Revoked.ServerBeginLoadedVolley_Implementation(1,1,&Pawn);
    assert(Revoked.HasLoadedVolley()&&Revoked.BeginCalls==0);
    PC.Confirmed=false;Revoked.TestRemaining=0;Revoked.TryBeginLoadedVolley();
    assert(!Revoked.HasLoadedVolley()&&Revoked.BeginCalls==0);
    assert(Revoked.SentVolleys.Last().Result==uint8(NCRocketVolley::EResult::Cancelled));
    // The same server inventory survives a same-pawn drop/repick, but its client
    // actor may be recreated with id 1. Only a new server epoch permits that id.
    PC.Confirmed=true;
    AUTPlusWeap_RocketLauncher Repick;Repick.UTOwner=&Pawn;Pawn.Weapon=&Repick;
    Repick.ServerBeginLoadedVolley_Implementation(1,70,&Pawn);
    assert(Repick.HasLoadedVolley());
    Repick.Removed();assert(Repick.LoadedOwnershipEpoch==2&&!Repick.UTOwner);
    Repick.CurrentState=Repick.ActiveState;Repick.GivenTo(&Pawn,true);
    assert(Repick.LoadedOwnershipEpoch==3&&Repick.LastServerLoadedVolleyId==0);
    Repick.ServerBeginLoadedVolley_Implementation(1,71,&Pawn);
    assert(!Repick.HasLoadedVolley()); // Late old begin cannot consume the new sequence.
    Repick.ServerBeginLoadedVolley_Implementation(3,1,&Pawn);
    assert(Repick.HasLoadedVolley()&&Repick.LoadedVolleyEpoch==3&&Repick.LoadedVolley.Id==1);
    Repick.ServerReleaseLoadedVolley_Implementation(1,1,&Pawn,1,3);
    Repick.ServerSetLoadedRocketMode_Implementation(1,1,&Pawn,1);
    assert(!Repick.bLoadedVolleyReleaseReceived&&Repick.CurrentRocketFireMode==0);

    AUTPlusWeap_RocketLauncher Recreated;Recreated.Role=1;Recreated.UTOwner=&Pawn;Pawn.Weapon=&Recreated;
    Recreated.LoadedOwnershipEpoch=3;Recreated.OnRep_LoadedOwnershipEpoch();Recreated.ResetLoadedVolley(1);
    Recreated.ClientLoadedVolleyResult_Implementation(1,1,&Pawn,uint8(NCRocketVolley::EResult::Rejected),0,0);
    assert(Recreated.HasLoadedVolley()); // Same pawn + same id from old epoch cannot cancel.
    AUTProjectile EpochFake;EpochFake.Instigator=&Pawn;Recreated.FindOrAddLoadedRocket(3,1,0).Fake=&EpochFake;
    Recreated.ClientLoadedRocketResult_Implementation(1,1,&Pawn,0,uint8(NCRocketVolley::ERocketResult::Cancelled),nullptr,0);
    assert(!EpochFake.Dead);

    // Generic grenade/spiral actors have no new native identity fields. The
    // receipt preserves their server NetGUID even when Actor* arrives unmapped.
    AUTProjectile GrenadeFake,GrenadeReal;GrenadeFake.Instigator=GrenadeReal.Instigator=&Pawn;
    GrenadeFake.Class=GrenadeReal.Class=2;
    Recreated.FindOrAddLoadedRocket(3,1,1).Fake=&GrenadeFake;
    Recreated.ClientLoadedRocketResult_Implementation(3,1,&Pawn,1,uint8(NCRocketVolley::ERocketResult::Spawned),nullptr,42);
    assert(!GrenadeFake.Dead&&!GrenadeFake.MasterProjectile);
    Recreated.W.Driver.GuidCache->Objects[42]=&GrenadeReal;
    Recreated.ReconcileLoadedRockets();
    assert(GrenadeFake.MasterProjectile==&GrenadeReal&&GrenadeReal.PairCalls==1);
    AUTProjectile WrongOwnerReal;WrongOwnerReal.Instigator=&OldPawn;
    Recreated.FindOrAddLoadedRocket(3,1,2).Fake=&EpochFake;
    Recreated.ClientLoadedRocketResult_Implementation(3,1,&Pawn,2,uint8(NCRocketVolley::ERocketResult::Spawned),nullptr,44);
    Recreated.W.Driver.GuidCache->Objects[44]=&WrongOwnerReal;Recreated.ReconcileLoadedRockets();
    assert(!EpochFake.MasterProjectile&&!EpochFake.Dead);
    Recreated.LoadedVolley.Finish(1);
    assert(Recreated.SpawnNetPredictedProjectile({}, {}, {})==nullptr&&Recreated.TestSpawnCalls==0);
    return 0;
}
