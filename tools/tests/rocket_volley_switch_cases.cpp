// Runs the production charged BeginState/EndState and volley methods together.
// The adapter supplies stock synchronous state dispatch, not UE input transport.
struct FHeldAltSwitchFixture
{
    APlayerController PC;
    AUTCharacter Pawn;
    AUTPlusWeap_RocketLauncher Weapon;

    FHeldAltSwitchFixture()
    {
        FPlatformTime::Now = 1000;
        Pawn.Controller = &PC;
        Pawn.Weapon = &Weapon;
        Weapon.UTOwner = &Pawn;
        Weapon.Role = 1;
        Weapon.TestUseStateCallbacks = true;
        Weapon.LastClientLoadedVolleyId = 40;
    }

    void FinishRaise(bool Held = true)
    {
        Pawn.SetPendingFire(1, Held);
        Weapon.CurrentState = Weapon.EquippingState;
        Weapon.GotoState(Weapon.ActiveState);
    }

    void AssertOneClientCharge(uint32 Id = 41, uint32 Epoch = 1) const
    {
        assert(Weapon.HasLoadedVolley() && Weapon.LoadedVolley.Id == Id);
        assert(Weapon.LoadedVolleyEpoch == Epoch && Weapon.LoadedVolleyPawn.Get() == &Pawn);
        assert(Weapon.SentBegins.Num() == 1 && Weapon.SentReleases.Num() == 0);
        assert(Weapon.SentBegins[0].Id == Id && Weapon.SentBegins[0].Epoch == Epoch);
        assert(Weapon.SentBegins[0].Pawn == &Pawn);
        assert(Weapon.BeginCalls == 1 && Weapon.TestLoadStarts == 1);
        assert(Weapon.CurrentState == &Weapon.Charge && Weapon.bLoadedVolleyEnteredState);
        assert(Weapon.Charge.StateVolleyId == Id && Weapon.Charge.StateVolleyEpoch == Epoch);
        assert(Weapon.Charge.bCharging && Weapon.Charge.LoadTimerHandle.Active);
        assert(Weapon.Charge.LoadTimerHandle.Delay == 0.4f);
        assert(!Weapon.Charge.bReleaseRequested && !Weapon.Charge.bReleaseCommitted);
        assert(Weapon.TestStateDepth == 0 && Weapon.TestMaxStateDepth <= 4);
        assert(Weapon.Ammo == 9 && Weapon.NumLoadedRockets == 0);
    }

    void AssertNoCharge() const
    {
        assert(!Weapon.HasLoadedVolley() && Weapon.SentBegins.Num() == 0);
        assert(Weapon.SentReleases.Num() == 0 && Weapon.TestLoadStarts == 0);
        assert(Weapon.BeginCalls == 0 && !Pawn.IsPendingFire(1));
        assert(!Weapon.Charge.bCharging && !Weapon.Charge.LoadTimerHandle.Active);
        assert(Weapon.TestStateDepth == 0 && Weapon.Ammo == 9);
    }
};

static void TestHeldAltSwitchRecovery()
{
    // The report's trigger: Active sees a carried held bit and bypasses StartFire.
    // Recovery must leave the failed charge before starting exactly one real one.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.TestOnBegin = [&F]() { F.Weapon.StartFire(1); };
        F.FinishRaise();
        F.AssertOneClientCharge();
        assert(F.Weapon.TestChargedEntries == 2);
        assert(F.Weapon.TestPendingAtGotoActive == std::vector<bool>({false}));
        assert(F.Weapon.GotoCalls == 1 && F.Pawn.IsPendingFire(1));
        F.Weapon.StartFire(1);
        F.Weapon.TryBeginLoadedVolley();
        F.AssertOneClientCharge(); // Nested/repeated starts cannot allocate ID 42.
        F.Weapon.StopFire(1);
        assert(F.Weapon.SentReleases.Num() == 1 && F.Weapon.SentReleases[0].Id == 41);
        assert(F.Weapon.SentReleases[0].Count == 1); // First load still owns a tap.
    }

    // Fully released taps never enter charge from stock Active. A direct failed
    // entry with no held bit also cannot manufacture a Begin.
    {
        FHeldAltSwitchFixture F;
        F.FinishRaise(false);
        F.AssertNoCharge();
        assert(F.Weapon.TestChargedEntries == 0);
        F.Weapon.GotoState(&F.Weapon.Charge);
        F.AssertNoCharge();
        assert(F.Weapon.CurrentState == F.Weapon.ActiveState);
    }

    // Remote authority still refuses to turn stock pending input into a load.
    // The normal version-gated RPC remains the only way to start that charge.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.Role = ROLE_Authority;
        F.Pawn.Local = false;
        F.FinishRaise();
        F.AssertNoCharge();
        assert(F.Weapon.TestChargedEntries == 1 && F.Weapon.LastClientLoadedVolleyId == 40);
        assert(F.Weapon.TestPendingAtGotoActive == std::vector<bool>({false}));
        F.PC.Confirmed = false;
        F.Weapon.ServerBeginLoadedVolley_Implementation(1, 41, &F.Pawn);
        F.AssertNoCharge();
        F.PC.Confirmed = true;
        F.Weapon.ServerBeginLoadedVolley_Implementation(1, 41, &F.Pawn);
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolley.Id == 41);
        assert(F.Weapon.TestLoadStarts == 1 && F.Weapon.SentBegins.Num() == 0);
    }

    // Revalidate after leaving the charged state. Active may have accepted a
    // pending switch, or this launcher may no longer be the current weapon.
    for (bool PendingSwitch : {false, true})
    {
        FHeldAltSwitchFixture F;
        AUTPlusWeap_RocketLauncher Other;
        F.Weapon.TestOnActive = [&F, &Other, PendingSwitch]() {
            if (PendingSwitch) F.Pawn.PendingWeapon = &Other;
            else F.Pawn.Weapon = &Other;
        };
        F.Pawn.SetPendingFire(1, true);
        F.Weapon.GotoState(&F.Weapon.Charge);
        F.AssertNoCharge();
        assert(F.Weapon.LastClientLoadedVolleyId == 40);
        assert(F.Weapon.TestPendingAtGotoActive == std::vector<bool>({false}));
    }

    // Other normal StartFire gates remain in force, including match/death checks.
    for (int Gate = 0; Gate != 4; ++Gate)
    {
        FHeldAltSwitchFixture F;
        if (Gate == 0) F.Pawn.Dead = true;
        if (Gate == 1) F.Pawn.Disabled = true;
        if (Gate == 2) F.Weapon.bDisableAltLoading = true;
        if (Gate == 3) F.Weapon.W.GS.Block = true;
        F.FinishRaise();
        F.AssertNoCharge();
    }

    // Neither weapon cooldown nor inherited switch debt can be bypassed by the
    // stock held bit. Queue an identity, then wait on both normal timing gates.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.EarliestFireTime = F.Weapon.W.Now + 0.2f;
        F.Weapon.TestRemaining = 0.3f;
        F.FinishRaise();
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.SentBegins.Num() == 1);
        assert(F.Weapon.BeginCalls == 0 && !F.Pawn.IsPendingFire(1));
        assert(F.Weapon.LoadedVolleyBeginHandle.Active && F.Weapon.TestLoadStarts == 0);
        assert(F.Weapon.LoadedVolleyBeginHandle.Delay == 0.3f);
        F.Weapon.TestRemaining = 0;
        F.Weapon.TryBeginLoadedVolley();
        assert(F.Weapon.BeginCalls == 0); // EarliestFireTime still blocks.
        F.Weapon.W.Now += 0.21f;
        F.Weapon.TryBeginLoadedVolley();
        F.AssertOneClientCharge();
    }

    // Ownership not replicated yet: no identity/RPC/loading before the epoch.
    // A late epoch outside 250 ms must not resurrect the carried held input.
    for (bool Expire : {false, true})
    {
        FHeldAltSwitchFixture F;
        F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.LastClientLoadedVolleyId = 0;
        F.FinishRaise();
        F.AssertNoCharge();
        assert(F.Weapon.bPendingLoadedVolleyInput && F.Weapon.LastClientLoadedVolleyId == 0);
        assert(F.Weapon.PendingLoadedVolleyInputHandle.Active);
        assert(F.Weapon.TestPendingAtGotoActive == std::vector<bool>({false}));
        FPlatformTime::Now += Expire ? NCRocketVolley::OwnershipInputWindowSeconds : 0.05;
        F.Weapon.LoadedOwnershipEpoch = 9;
        F.Weapon.OnRep_LoadedOwnershipEpoch();
        assert(!F.Weapon.bPendingLoadedVolleyInput);
        if (Expire) F.AssertNoCharge();
        else
        {
            F.AssertOneClientCharge(1, 9);
            F.Weapon.OnRep_LoadedOwnershipEpoch();
            F.Weapon.TryDrainLoadedVolleyInput();
            F.AssertOneClientCharge(1, 9);
        }
    }

    // Reused charged state: cleanup of its previous completed volley must finish
    // before nested BeginState sets the new identity, flags and first-load timer.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.ResetLoadedVolley(40);
        F.Weapon.CompleteLoadedVolley(false);
        F.Weapon.Charge.StateVolleyId = 40;
        F.Weapon.Charge.StateVolleyEpoch = 1;
        F.Weapon.Charge.bReleaseRequested = F.Weapon.Charge.bReleaseCommitted = true;
        F.Weapon.Charge.GraceTimerHandle.Active = true;
        F.Weapon.Charge.FireLoadedRocketHandle.Active = true;
        F.FinishRaise();
        F.AssertOneClientCharge();
        assert(!F.Weapon.Charge.GraceTimerHandle.Active);
        assert(!F.Weapon.Charge.FireLoadedRocketHandle.Active);
        assert(F.Weapon.TestChargedEntries == 2);
    }

    // A spawn-held replay/direct press while raising already has an identity;
    // completing equip must not create a second Begin or enter charge early.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.CurrentState = F.Weapon.EquippingState;
        F.Weapon.StartFire(1);
        assert(F.Weapon.SentBegins.Num() == 1 && F.Weapon.TestLoadStarts == 0);
        assert(!F.Pawn.IsPendingFire(1) && F.Weapon.LoadedVolleyBeginHandle.Active);
        F.Weapon.GotoState(F.Weapon.ActiveState);
        assert(F.Weapon.TestLoadStarts == 0);
        F.Weapon.TryBeginLoadedVolley();
        F.AssertOneClientCharge();
        assert(F.Weapon.TestChargedEntries == 1 && F.Weapon.GotoCalls == 0);
    }

    // Existing listen-host and bot direct-entry behavior is untouched.
    for (bool Bot : {false, true})
    {
        FHeldAltSwitchFixture F;
        Controller BotController;
        F.Weapon.Role = ROLE_Authority;
        F.Pawn.Local = !Bot;
        if (Bot) F.Pawn.Controller = &BotController;
        F.FinishRaise();
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.LoadedVolley.Id == 41);
        assert(F.Weapon.TestLoadStarts == 1 && F.Weapon.TestChargedEntries == 1);
        assert(F.Weapon.GotoCalls == 0 && F.Weapon.SentBegins.Num() == 0);
    }
}

// Alt pressed while the launcher itself is being put away belongs to the
// incoming weapon. The launcher hands it to AUTWeaponFix's swap-away latch
// (counted here as StockStartCalls) and never opens a loaded volley for it.
static void TestOutgoingLauncherAltHandoff()
{
    // Lowering, a requested switch (incl. a deferred mid-burst put-down), and a
    // switch before ownership: all hand off; none buffer, allocate or send.
    for (int Switch = 0; Switch != 3; ++Switch)
    {
        FHeldAltSwitchFixture F;
        AUTPlusWeap_RocketLauncher Incoming;
        if (Switch == 0) F.Weapon.CurrentState = F.Weapon.UnequippingState;
        else F.Pawn.PendingWeapon = &Incoming;
        if (Switch == 2) F.Weapon.LoadedOwnershipEpoch = 0;
        F.Weapon.StartFire(1);
        assert(F.Weapon.StockStartCalls == 1 && F.Weapon.StockStopCalls == 0);
        assert(!F.Weapon.HasLoadedVolley() && F.Weapon.SentBegins.Num() == 0);
        assert(!F.Weapon.bPendingLoadedVolleyInput && F.Weapon.LastClientLoadedVolleyId == 40);
        assert(F.Weapon.BeginCalls == 0 && F.Weapon.TestLoadStarts == 0);
    }

    // Switching back before the launcher is down: the latched bit (AUTWeaponFix's
    // swap branch, simulated) re-enters through the held-switch recovery.
    {
        FHeldAltSwitchFixture F;
        AUTPlusWeap_RocketLauncher Incoming;
        F.Weapon.CurrentState = F.Weapon.UnequippingState;
        F.Pawn.PendingWeapon = &Incoming;
        F.Weapon.StartFire(1);
        assert(F.Weapon.StockStartCalls == 1 && F.Weapon.SentBegins.Num() == 0);
        F.Pawn.PendingWeapon = nullptr; // stock LocalSwitchWeapon switch-back
        F.FinishRaise();                // latched bit still held through BringUp
        F.AssertOneClientCharge();
    }

    // A genuine release during the put-down clears that latch and the GhostFix
    // flag, with or without an older queued volley, and sends no stock Stop. A
    // queued volley still receives its existing numbered release.
    for (bool QueuedVolley : {false, true})
    {
        FHeldAltSwitchFixture F;
        AUTPlusWeap_RocketLauncher Incoming;
        if (QueuedVolley) F.Weapon.ResetLoadedVolley(41);
        F.Weapon.CurrentState = F.Weapon.UnequippingState;
        F.Pawn.PendingWeapon = &Incoming;
        F.Weapon.StartFire(1);
        F.Pawn.SetPendingFire(1, true);       // AUTWeaponFix swap-away latch
        F.Weapon.bFireHeldByPlayer[1] = true; // its flag when ncp.GhostFix=1
        F.Weapon.StopFire(1);
        assert(!F.Pawn.IsPendingFire(1) && !F.Weapon.bFireHeldByPlayer[1]);
        assert(F.Weapon.StockStartCalls == 1 && F.Weapon.StockStopCalls == 0);
        assert(F.Weapon.SentReleases.Num() == (QueuedVolley ? 1 : 0));
        assert(F.Weapon.EndCalls == (QueuedVolley ? 0 : 1));
    }

    // Outside a switch a release keeps the 329 queued-volley rule: the held bit
    // was already cleared when the Begin queued, and the release is retained.
    {
        FHeldAltSwitchFixture F;
        F.Weapon.TestRemaining = 0.2f;
        F.Weapon.StartFire(1);
        assert(F.Weapon.StockStartCalls == 0 && F.Weapon.SentBegins.Num() == 1);
        assert(!F.Pawn.IsPendingFire(1) && F.Weapon.LoadedVolleyBeginHandle.Active);
        F.Weapon.StopFire(1);
        assert(F.Weapon.SentReleases.Num() == 1 && F.Weapon.EndCalls == 0);
        assert(F.Weapon.HasLoadedVolley() && F.Weapon.StockStopCalls == 0);
    }
}
