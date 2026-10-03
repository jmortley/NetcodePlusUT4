int main()
{
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
