// These tests execute the production launcher Start/Stop, admission and mode
// callbacks. Timers are dispatched explicitly, RPCs are recorded and server
// actor references are mapped by the fixture; no network/Unreal runtime claim.
static void TestQueuedPrimaryCyclesWithoutDispatch()
{
    for (bool Authority : {false, true})
    {
        for (int Gate = 0; Gate < 4; ++Gate)
        {
            if (Authority && Gate == 3) continue; // Epoch zero is client-only.
            FHeldAltSwitchFixture F;
            if (Authority) F.Weapon.Role = ROLE_Authority;
            F.Weapon.CurrentFireMode = 0; // Previous mode remains visible while queued.
            if (Gate == 0) F.Weapon.CurrentState = F.Weapon.EquippingState;
            if (Gate == 1) F.Weapon.EarliestFireTime = F.Weapon.W.Now + .2f;
            if (Gate == 2) F.Weapon.TestRemaining = .2f;
            if (Gate == 3) F.Weapon.LoadedOwnershipEpoch = 0;
            F.Weapon.StartFire(1);
            assert(F.Weapon.TestLoadStarts == 0 && F.Weapon.SentBegins.Num() == 0);
            const double InputAt = F.Weapon.PendingLoadedVolleyInputAt;
            const float RequestAt = F.Weapon.LoadedVolleyBeginRequestedAt;
            for (uint8 Expected : {uint8(1), uint8(2), uint8(0), uint8(1)})
            {
                F.Weapon.RetryFireHandle[0].Active = true;
                F.Weapon.bBufferedClickPending[0] = F.Weapon.bCrossModeRetryArmed[0] = true;
                F.Pawn.SetPendingFire(0, true);
                F.Weapon.StartFire(0);
                const uint8 Selected = Gate == 3 ? F.Weapon.PendingLoadedVolleyMode : F.Weapon.LoadedVolleySelectedMode;
                assert(Selected == Expected && F.Weapon.CurrentRocketFireMode == Expected);
                assert(F.Weapon.StockStartCalls == 0 && !F.Pawn.IsPendingFire(0));
                assert(!F.Weapon.RetryFireHandle[0].Active && !F.Weapon.bBufferedClickPending[0]);
                assert(!F.Weapon.bCrossModeRetryArmed[0]);
                assert(F.Weapon.SentBegins.Num() == 0 && F.Weapon.SentModes.Num() == 0);
                assert(F.Weapon.TestLoadStarts == 0 && F.Weapon.Ammo == 9);
                assert(F.Weapon.PendingLoadedVolleyInputAt == InputAt);
                assert(F.Weapon.LoadedVolleyBeginRequestedAt == RequestAt);
            }
            F.Weapon.CurrentRocketFireMode = 0; // Old-state cleanup can reset visuals.
            F.Weapon.TestRemaining = 0;
            F.Weapon.W.Now += .21f;
            F.Weapon.CurrentState = F.Weapon.ActiveState;
            if (Gate == 3)
            {
                FPlatformTime::Now += .05;
                F.Weapon.LoadedOwnershipEpoch = 7;
                F.Weapon.OnRep_LoadedOwnershipEpoch();
            }
            else F.Weapon.TryBeginLoadedVolley();
            assert(F.Weapon.TestLoadStarts == 1 && F.Weapon.CurrentRocketFireMode == 1);
            assert(F.Weapon.bLoadedVolleyEnteredState && F.Weapon.Charge.bCharging);
            assert(F.Weapon.SentBegins.Num() == (Authority ? 0 : 1));
            assert(F.Weapon.SentModes.Num() == (Authority ? 0 : 1));
            if (!Authority)
            {
                assert(F.Weapon.InputEvents == std::vector<uint8>({2, 1, 5}));
                assert(F.Weapon.SentModes[0].Id == F.Weapon.SentBegins[0].Id);
                assert(F.Weapon.SentModes[0].Epoch == F.Weapon.SentBegins[0].Epoch);
                assert(F.Weapon.SentModes[0].Mode == 1);
            }
            // A retry is never a new mode click or a duplicate Begin.
            F.Weapon.TryBeginLoadedVolley();
            F.Weapon.StartFire(1);
            assert(F.Weapon.TestLoadStarts == 1 && F.Weapon.CurrentRocketFireMode == 1);
        }
    }
}

static void TestQueuedTapCancellationAndFreshIdentity()
{
    for (int OwnerKind = 0; OwnerKind < 3; ++OwnerKind)
    {
        for (bool Equip : {false, true})
        {
            FHeldAltSwitchFixture F;
            AUTBot Bot;
            if (OwnerKind != 0) F.Weapon.Role = ROLE_Authority;
            if (OwnerKind == 2) {F.Pawn.Local = false; F.Pawn.Controller = &Bot;}
            if (Equip) F.Weapon.CurrentState = F.Weapon.EquippingState;
            else F.Weapon.TestRemaining = .2f;
            F.Weapon.StartFire(1);
            const uint32 CancelledId = F.Weapon.LoadedVolley.Id;
            assert(CancelledId != 0 && F.Weapon.HasLoadedVolley());
            assert(F.Weapon.SentBegins.Num() == 0 && F.Weapon.TestLoadStarts == 0);
            F.Weapon.NotifyLoadedVolleyRelease(); // A generic notification cannot send an unpaired Release.
            assert(F.Weapon.SentReleases.Num() == 0 && !F.Weapon.bLoadedVolleyReleaseSent);
            F.Weapon.StopFire(1);
            assert(!F.Weapon.HasLoadedVolley() && !F.Weapon.LoadedVolleyBeginHandle.Active);
            assert(F.Weapon.SentReleases.Num() == 0 && F.Weapon.TestLoadStarts == 0);
            assert(F.Weapon.Ammo == 9 && F.Weapon.Refund == 0 && !F.Pawn.IsPendingFire(1));
            F.Weapon.StopFire(1);
            F.Weapon.TestRemaining = 0;
            F.Weapon.W.Now += 6.f;
            F.Weapon.CurrentState = F.Weapon.ActiveState;
            F.Weapon.TryBeginLoadedVolley();
            assert(F.Weapon.TestLoadStarts == 0 && F.Weapon.SentBegins.Num() == 0);
            F.Weapon.StartFire(1);
            assert(F.Weapon.LoadedVolley.Id != CancelledId && F.Weapon.TestLoadStarts == 1);
            assert(F.Weapon.Charge.bCharging && F.Weapon.SentReleases.Num() == 0);
            const uint32 NewId = F.Weapon.LoadedVolley.Id;
            if (OwnerKind == 0)
            {
                F.Weapon.ClientLoadedVolleyResult_Implementation(1, CancelledId, &F.Pawn,
                    uint8(NCRocketVolley::EResult::Cancelled), 0, 0);
                assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolley.Id == NewId);
                assert(F.Weapon.CurrentState == &F.Weapon.Charge && F.Pawn.IsPendingFire(1));
            }
        }
    }
}

static void TestEnteredReleaseStillWaitsForRemoteServer()
{
    FHeldAltSwitchFixture Client;
    Client.Weapon.CurrentState = Client.Weapon.EquippingState;
    Client.Weapon.StartFire(1);
    Client.Weapon.StartFire(0);
    assert(Client.Weapon.SentBegins.Num() == 0);
    Client.Weapon.CurrentState = Client.Weapon.ActiveState;
    Client.Weapon.TryBeginLoadedVolley();
    Client.Weapon.StopFire(1); // Actual charge entered, first rocket has not completed.
    assert(Client.Weapon.SentBegins.Num() == 1 && Client.Weapon.SentModes.Num() == 1);
    assert(Client.Weapon.SentReleases.Num() == 1 && Client.Weapon.SentReleases[0].Count == 1);
    assert(Client.Weapon.InputEvents == std::vector<uint8>({2, 1, 5, 3, 4}));
    assert(Client.Weapon.HasLoadedVolley() && Client.Weapon.Charge.LoadTimerHandle.Active);

    FPendingServerRocketFixture Server;
    Server.Begin();
    Server.Weapon.ServerSetLoadedRocketMode_Implementation(1, 41, &Server.Pawn, 1);
    Server.Weapon.ServerReleaseLoadedVolley_Implementation(1, 41, &Server.Pawn, 1, 1);
    Server.AssertWaiting();
    // A server-side Stop while switching must not locally cancel a client-started request.
    Server.Weapon.StopFire(1);
    assert(Server.Weapon.HasLoadedVolley() && Server.Weapon.bLoadedVolleyReleaseReceived);
    Server.BeginRaise();
    Server.FinishServerRaise();
    assert(Server.Weapon.EndCalls == 1 && Server.Weapon.CurrentRocketFireMode == 1);
    // Native load callback is outside this adapter: provide one completed load.
    Server.Weapon.NumLoadedRockets = Server.Weapon.LoadedVolleyAmmoSpent = 1;
    Server.Weapon.Ammo = 8;
    assert(Server.Weapon.CommitLoadedVolley());
    assert(Server.Weapon.LoadedVolley.Count == 1 && Server.Weapon.Refund == 0);
}

static void TestChargingModeGuardsAndOneBegin()
{
    FHeldAltSwitchFixture F;
    F.Weapon.TestOnServerBegin = [&F]() {
        assert(F.Weapon.bLoadedVolleyEnteredState);
        assert(F.Weapon.BeginLoadedVolleyState()); // Same identity re-entry cannot resend Begin.
    };
    F.Weapon.StartFire(1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.TestLoadStarts == 1);
    F.Weapon.StartFire(0);
    assert(F.Weapon.CurrentRocketFireMode == 1 && F.Weapon.SentModes.Num() == 1);
    assert(F.Weapon.StockStartCalls == 0 && !F.Pawn.IsPendingFire(0));
    F.Weapon.Charge.GraceTimerHandle.Active = true;
    F.Weapon.Charge.GraceTimerHandle.Delay = .01f;
    F.Weapon.StartFire(0);
    assert(F.Weapon.CurrentRocketFireMode == 1 && F.Weapon.SentModes.Num() == 1);
    assert(F.Weapon.StockStartCalls == 0); // Unsafe cycle is consumed, never a primary shot.
    F.Weapon.Charge.GraceTimerHandle.Active = false;
    F.Weapon.SpawnDelayedFakeProjHandle.Active = true;
    F.Weapon.StartFire(0);
    assert(F.Weapon.CurrentRocketFireMode == 1 && F.Weapon.SentModes.Num() == 1);
    F.Weapon.SpawnDelayedFakeProjHandle.Active = false;
    F.Weapon.bHandlingRetry = true;
    F.Weapon.StartFire(0);
    assert(F.Weapon.CurrentRocketFireMode == 1 && F.Weapon.SentModes.Num() == 1);
    assert(F.Weapon.StockStartCalls == 1);
    F.Weapon.bHandlingRetry = false;
    F.Weapon.StopFire(1);
    F.Weapon.StartFire(0);
    assert(F.Weapon.StockStartCalls == 2 && F.Weapon.SentModes.Num() == 1);
    assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.SentReleases.Num() == 1);

    FHeldAltSwitchFixture GrenadesOnly;
    GrenadesOnly.Weapon.bAllowAltModes = false;
    GrenadesOnly.Weapon.TestRemaining = .2f;
    GrenadesOnly.Weapon.StartFire(1);
    GrenadesOnly.Weapon.StartFire(0);
    assert(GrenadesOnly.Weapon.LoadedVolleySelectedMode == 1);
    GrenadesOnly.Weapon.StartFire(0);
    assert(GrenadesOnly.Weapon.LoadedVolleySelectedMode == 0); // Skip unsupported spiral.
    assert(GrenadesOnly.Weapon.SentModes.Num() == 0 && GrenadesOnly.Weapon.StockStartCalls == 0);
}

static void TestModeInputCannotFollowInvalidRequest()
{
    for (int Change = 0; Change < 10; ++Change)
    {
        FHeldAltSwitchFixture F;
        AUTCharacter OtherPawn;
        AUTPlusWeap_RocketLauncher OtherWeapon;
        F.Weapon.TestRemaining = .2f;
        F.Weapon.StartFire(1);
        switch (Change)
        {
            case 0: F.Pawn.Dead = true; break;
            case 1: F.Pawn.Disabled = true; break;
            case 2: F.Pawn.Weapon = &OtherWeapon; break;
            case 3: F.Pawn.PendingWeapon = &OtherWeapon; break;
            case 4: ++F.Weapon.LoadedOwnershipEpoch; break;
            case 5: F.Weapon.LoadedVolleyPawn = &OtherPawn; break;
            case 6: F.Weapon.Role = ROLE_Authority; F.Pawn.Local = false; break;
            case 7: F.Weapon.bLoadedVolleyReleaseSent = true; break;
            case 8: assert(F.Weapon.LoadedVolley.Release(41, 1)); break;
            case 9: F.Weapon.CompleteLoadedVolley(true); break;
        }
        assert(!F.Weapon.TryCycleLoadedVolleyMode());
        assert(F.Weapon.LoadedVolleySelectedMode == 0 && F.Weapon.SentModes.Num() == 0);
        assert(F.Weapon.SentBegins.Num() == 0 && F.Weapon.TestLoadStarts == 0);
    }
    FHeldAltSwitchFixture Expired;
    Expired.Weapon.LoadedOwnershipEpoch = 0;
    Expired.Weapon.StartFire(1);
    FPlatformTime::Now += NCRocketVolley::OwnershipInputWindowSeconds;
    Expired.Weapon.StartFire(0);
    assert(!Expired.Weapon.bPendingLoadedVolleyInput && Expired.Weapon.PendingLoadedVolleyMode == 0);
    assert(Expired.Weapon.StockStartCalls == 1 && Expired.Weapon.SentBegins.Num() == 0);

    // Authority-local ActiveState can enter charging without StartFire first.
    // Preserve its chosen bot/host mode when that admission creates the identity.
    for (bool Bot : {false, true})
    {
        FHeldAltSwitchFixture F;
        AUTBot BotController;
        F.Weapon.Role = ROLE_Authority;
        if (Bot) {F.Pawn.Local = false; F.Pawn.Controller = &BotController;}
        F.Weapon.CurrentRocketFireMode = 2;
        F.FinishRaise();
        assert(F.Weapon.CurrentRocketFireMode == 2 && F.Weapon.LoadedVolleySelectedMode == 2);
        assert(F.Weapon.TestLoadStarts == 1 && F.Weapon.SentBegins.Num() == 0);
    }
}

static void TestQuickRocketInputs()
{
    TestQueuedPrimaryCyclesWithoutDispatch();
    TestQueuedTapCancellationAndFreshIdentity();
    TestEnteredReleaseStillWaitsForRemoteServer();
    TestChargingModeGuardsAndOneBegin();
    TestModeInputCannotFollowInvalidRequest();
}
