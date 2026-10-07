struct Fixture {
    AUTPlusWeap_RocketLauncher Gun;
    AUTCharacter Shooter,Target,OtherPawn;
    AUTPlusProj_Rocket Rockets[16];
    Fixture() {
        Gun.UTOwner=&Shooter;Shooter.Weapon=&Gun;
        CVarRocketLagComp.Value=1.f;CVarRocketLagCompGraceMs.Value=200.f;
        CVarRocketLagCompMaxPingMs.Value=150.f;CVarRocketLagCompMaxWindowMs.Value=200.f;
        for(int i=0;i<3;++i) Add(i,41,uint8(i));
    }
    void Add(int rocket,uint32 volley,uint8 ordinal,uint8 mode=1,uint32 epoch=7) {
        FActiveServerProjectile entry(&Rockets[rocket],mode);
        entry.FiringPawn=&Shooter;entry.LoadedOwnershipEpoch=epoch;
        entry.LoadedVolleyId=volley;entry.LoadedRocketOrdinal=ordinal;
        Rockets[rocket].LoadedVolleyId=volley;
        Gun.ActiveServerProjectiles.Add(entry);
    }
    void Claim(uint8 ordinal,uint32 volley=41,uint32 epoch=7) {
        Gun.ServerLoadedRocketHitClaim_Implementation(&Target,Target.History,epoch,volley,ordinal);
    }
    void Resolve(int rocket,AUTCharacter* damaged=nullptr,bool trusted=true) {
        Rockets[rocket].ImpactedActor=damaged;
        if(trusted&&Rockets[rocket].LoadedVolleyId!=0)
            Gun.OnTrackedRocketExploding(&Rockets[rocket],Rockets[rocket].Position,FVector(0.f,0.f,1.f));
        else Gun.OnTrackedProjectileResolved(&Rockets[rocket],damaged);
        Rockets[rocket].bExploded=true;Rockets[rocket].PendingKill=true;
        // Expired engine weak handles no longer yield the destroyed actor.
        for(auto& entry:Gun.ActiveServerProjectiles)
            if(entry.Projectile.Get()==&Rockets[rocket]) entry.Projectile.Reset();
    }
    int Hits() const { int hits=0;for(const auto& rocket:Rockets)hits+=rocket.ProcessHits;return hits; }
};
struct FlakFixture {
    AUTWeaponFix Gun;
    AUTCharacter Shooter,Target,OtherPawn;
    AUTPlusProj_FlakShell Shells[8];
    FlakFixture() {
        Gun.UTOwner=&Shooter;Shooter.Weapon=&Gun;
        CVarRocketLagComp.Value=1.f;CVarRocketLagCompGraceMs.Value=200.f;
        CVarRocketLagCompMaxPingMs.Value=150.f;CVarRocketLagCompMaxWindowMs.Value=200.f;
        Gun.NextFlakShotId=100;
        for(int i=0;i<3;++i) Add(i);
    }
    void Add(int index) {
        auto& shell=Shells[index];shell.Instigator=&Shooter;
        Gun.CaptureFlakShellSpawn(&shell);
        FActiveServerProjectile entry(&shell,1);
        entry.FiringPawn=&Shooter;entry.FlakShotId=shell.ShotId;
        Gun.ActiveServerProjectiles.Add(entry);
    }
    void Claim(uint32 id) { Gun.ServerFlakShellHitClaim_Implementation(&Target,Target.History,id); }
    void Resolve(int index,UObject* impacted=nullptr,bool trusted=true) {
        auto& shell=Shells[index];shell.ImpactedActor=impacted;
        if(trusted) Gun.OnTrackedFlakExploding(&shell,shell.Position,FVector(0.f,0.f,1.f));
        else Gun.OnTrackedProjectileResolved(&shell,Cast<AUTCharacter>(impacted));
        shell.bExploded=true;shell.PendingKill=true;
        for(auto& entry:Gun.ActiveServerProjectiles)
            if(entry.Projectile.Get()==&shell) entry.Projectile.Reset();
    }
    int Hits() const { int count=0;for(const auto& shell:Shells)count+=shell.ProcessHits;return count; }
};
void FlakCases(const std::string& name) {
    if(name=="flak_exact") {
        FlakFixture f;
        // Authored child shards are still handled by the live native hit path.
        f.Shells[2].ShardClass=UDamageType::StaticClass();f.Shells[2].ShardSpawnCount=9;
        f.Claim(103);
        Require(f.Shells[2].ProcessHits==1&&f.Shells[0].ProcessHits==0&&f.Shells[1].ProcessHits==0,
                "exact Flak ID selected the older overlapping shell");
        Require(f.Target.DamageCalls==0&&f.Gun.ActiveServerProjectiles.Num()==2,
                "live Flak claim bypassed native ProcessHit or removed another shell");
        f.Claim(103);f.Claim(101);f.Claim(101);f.Claim(102);f.Claim(102);
        Require(f.Hits()==3&&f.Gun.ActiveServerProjectiles.Num()==0&&f.Target.DamageCalls==0,
                "out-of-order or duplicate Flak claim selected another shell or applied damage twice");
    } else if(name=="flak_guards") {
        for(int guard=0;guard<12;++guard) {
            FlakFixture f;uint32 id=103;
            if(guard==0) id=0;
            if(guard==1) id=99;
            if(guard==2) f.Gun.Role=1;
            if(guard==3) f.Shooter.OwnController.Confirmed=false;
            if(guard==4) f.Gun.UTOwner=nullptr;
            if(guard==5) f.Gun.UTOwner=&f.OtherPawn;
            if(guard==6) f.Gun.ActiveServerProjectiles[2].FiringPawn=&f.OtherPawn;
            if(guard==7) f.Gun.ActiveServerProjectiles[2].FiringPawn.Reset();
            if(guard==8) f.Gun.ActiveServerProjectiles[2].FireMode=0;
            if(guard==9) f.Gun.ActiveServerProjectiles[2].LoadedVolleyId=41;
            if(guard==10) f.Target.Dead=true;
            if(guard==11) {
                AUTWeaponFix wrongWeapon;wrongWeapon.UTOwner=&f.Shooter;
                wrongWeapon.ServerFlakShellHitClaim_Implementation(&f.Target,f.Target.History,id);
            } else f.Claim(id);
            Require(f.Hits()==0&&f.Target.DamageCalls==0&&f.Gun.ActiveServerProjectiles.Num()==3,
                    "invalid/stale Flak ID, weapon, owner, protocol or fire mode accepted a claim");
        }
        FlakFixture switched;AUTWeaponFix otherGun;switched.Shooter.Weapon=&otherGun;switched.Claim(103);
        Require(switched.Shells[2].ProcessHits==1,"ordinary weapon switch invalidated in-flight Flak claim");
    } else if(name=="flak_isolation") {
        FlakFixture f;
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,0);
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,1);
        Require(f.Hits()==0,"legacy claim consumed identified Flak shell");
        f.Gun.ActiveServerProjectiles[2].FlakShotId=0;
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,1);
        Require(f.Hits()==0,"missing Flak identity fell back to legacy FIFO matching");
        f.Gun.ActiveServerProjectiles[2].FlakShotId=103;
        f.Gun.ActiveServerProjectiles[2].LoadedVolleyId=41;
        f.Gun.ActiveServerProjectiles[2].LoadedOwnershipEpoch=7;
        f.Gun.ActiveServerProjectiles[2].LoadedRocketOrdinal=2;
        f.Gun.ProcessProjectileHitClaim(&f.Target,f.Target.History,1,7,41,2);
        f.Claim(103);
        Require(f.Hits()==0,"dual-tagged tracking entry matched Flak or loaded rocket claim");
        Fixture rockets;rockets.Gun.ServerFlakShellHitClaim_Implementation(&rockets.Target,rockets.Target.History,41);
        Require(rockets.Hits()==0,"Flak claim consumed loaded rocket sharing a numeric ID");
        AUTProjectile legacy;
        FActiveServerProjectile legacyEntry(&legacy,1);legacyEntry.FiringPawn=&f.Shooter;
        f.Gun.ActiveServerProjectiles.Add(legacyEntry);
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,1);
        Require(legacy.ProcessHits==1&&f.Hits()==0,"tag isolation broke ordinary non-Flak legacy projectile");
    } else if(name=="flak_grace") {
        FlakFixture f;f.Shells[2].UseAdjustedDamage=true;
        f.Shells[2].AdjustedDamageParams.BaseDamage=125.f;
        f.Resolve(2);f.Gun.TheWorld.Now+=.05f;f.Add(3);f.Gun.PruneTrackedProjectiles(f.Gun.TheWorld.Now);
        Require(f.Gun.ActiveServerProjectiles.Num()==4,"Flak grace snapshot was discarded by a new spawn");
        f.Claim(103);
        Require(f.Target.DamageCalls==1&&f.Target.DamageTotal==125.f&&f.Hits()==0
                &&f.Gun.ActiveServerProjectiles.Num()==3,
                "exact Flak grace consumed unrelated live shell or ignored adjusted damage snapshot");
        f.Claim(103);Require(f.Target.DamageCalls==1&&f.Hits()==0,"Flak grace duplicate consumed live shell");
        f.Claim(104);Require(f.Shells[3].ProcessHits==1,"Flak grace damaged the replacement shell identity");
    } else if(name=="flak_terminal_guards") {
        for(int risk=0;risk<4;++risk) {
            FlakFixture f;APawn genericPawn;UObject* impacted=nullptr;
            if(risk==0) {f.Shells[2].ShardClass=UDamageType::StaticClass();f.Shells[2].ShardSpawnCount=9;}
            if(risk==1) impacted=&f.Target;
            if(risk==2) impacted=&f.OtherPawn;
            if(risk==3) impacted=&genericPawn;
            f.Resolve(2,impacted);f.Claim(103);f.Claim(103);
            Require(f.Gun.ActiveServerProjectiles[2].bFlakExplosionObserved
                    &&!f.Gun.ActiveServerProjectiles[2].bFlakGraceEligible
                    &&f.Target.DamageCalls==0&&f.Hits()==0&&f.Gun.TheWorld.OverlapQueries==0,
                    "Flak terminal with authored shards or pawn impact accepted a top-up");
        }
        for(bool hasClass:{false,true}) {
            FlakFixture f;
            if(hasClass) f.Shells[2].ShardClass=UDamageType::StaticClass();
            f.Shells[2].ShardSpawnCount=hasClass?0:9;
            f.Resolve(2);f.Claim(103);
            Require(f.Target.DamageCalls==1,"inactive shard configuration prevented otherwise valid shardless grace");
        }
    } else if(name=="flak_splash") {
        for(bool wall:{false,true}) {
            FlakFixture f;UObject scenery;
            f.Gun.TheWorld.Wall=wall;f.Gun.TheWorld.OverlapBlocking=false;
            f.Gun.TheWorld.OverlapResults={&f.Target,&f.Target,&scenery};
            f.Shells[2].UseAdjustedDamage=true;f.Shells[2].AdjustedDamageParams.OuterRadius=321.f;
            f.Resolve(2);
            const auto& entry=f.Gun.ActiveServerProjectiles[2];
            Require(entry.bFlakExplosionObserved&&entry.bFlakGraceEligible
                    &&entry.PossibleSplashTargets.Num()==1&&entry.PossibleSplashTargets.Contains(&f.Target)
                    &&entry.BaseDamage==125.f&&entry.Momentum==222.f
                    &&f.Gun.TheWorld.LastOverlapRadius==321.f
                    &&f.Gun.TheWorld.LastOverlapOrigin.Z==1.f,
                    "Flak explosion query lost adjusted parameters or nonblocking duplicate candidate handling");
            f.Claim(103);
            Require(f.Target.DamageCalls==0&&f.Hits()==0&&f.Gun.TheWorld.WallQueries==0,
                    "possible Flak splash victim received a grace top-up or depended on LOS");
        }
        FlakFixture unknown;unknown.Resolve(2,nullptr,false);unknown.Claim(103);
        Require(unknown.Target.DamageCalls==0&&unknown.Hits()==0,"generic untrusted Flak snapshot accepted grace");
        for(int invalid=0;invalid<7;++invalid) {
            FlakFixture f;auto& shell=f.Shells[2];shell.UseAdjustedDamage=true;
            if(invalid==0) shell.AdjustedDamageParams.OuterRadius=-1.f;
            if(invalid==1) shell.AdjustedDamageParams.OuterRadius=std::numeric_limits<float>::quiet_NaN();
            if(invalid==2) shell.Position.Z=std::numeric_limits<float>::infinity();
            if(invalid==3) shell.AdjustedDamageParams.BaseDamage=-1.f;
            if(invalid==4) shell.AdjustedDamageParams.BaseDamage=std::numeric_limits<float>::quiet_NaN();
            if(invalid==5) shell.AdjustedMomentum=std::numeric_limits<float>::infinity();
            if(invalid==6) shell.AdjustedMomentum=std::numeric_limits<float>::quiet_NaN();
            f.Resolve(2);f.Claim(103);
            Require(f.Target.DamageCalls==0&&!f.Gun.ActiveServerProjectiles[2].bFlakGraceEligible,
                    "invalid Flak explosion geometry or damage snapshot allowed grace");
        }
    } else if(name=="flak_low_ping") {
        for(float ping:{0.f,10.f,20.f,40.f}) for(bool grace:{false,true}) for(bool miss:{false,true}) {
            FlakFixture f;f.Shooter.State.ExactPing=ping;
            const float window=std::max(.016f,ping*.0011f),step=1.f/240.f;
            const float delta=std::floor(window/step)*step,age=grace?ping*.0005f:0.f;
            const float back=delta-age;
            f.Target.HistoryVelocity=FVector(0.f,1000.f,0.f);
            const FVector anchor=f.Target.History-f.Target.HistoryVelocity*delta;
            auto& shell=f.Shells[2];shell.Velocity=FVector(2000.f,0.f,300.f);shell.Movement.Gravity=-980.f;
            shell.Position=anchor+shell.Velocity*back-FVector(0.f,0.f,.5f*shell.Movement.Gravity*back*back);
            if(miss) shell.Position.X+=200.f;
            shell.DamageParams.OuterRadius=0.f; // Exercise contact reconstruction without possible native splash.
            if(grace) { f.Gun.TheWorld.Now=10.f-age;f.Resolve(2);f.Gun.TheWorld.Now=10.f; }
            f.Gun.ServerFlakShellHitClaim_Implementation(&f.Target,anchor,103);
            Require(miss ? f.Hits()==0&&f.Target.DamageCalls==0
                         : grace ? f.Target.DamageCalls==1&&f.Hits()==0 : shell.ProcessHits==1&&f.Target.DamageCalls==0,
                    "low-ping Flak moving-history/gravity contact decision was incorrect");
            Require(f.Target.MaxRewind<=window+.0001f&&f.Target.MaxRewind>=window-step-.0001f,
                    "Flak exact claims changed bounded low-ping history window");
        }
        for(bool grace:{false,true}) for(int guard=0;guard<3;++guard) {
            FlakFixture f;if(grace)f.Resolve(2);
            if(guard==0) f.Gun.TheWorld.Wall=true;
            if(guard==1) f.Shooter.State.ExactPing=151.f;
            if(guard==2) f.Target.History.Y=1000.f;
            f.Gun.ServerFlakShellHitClaim_Implementation(&f.Target,FVector(100.f,0.f,0.f),103);
            Require(f.Hits()==0&&f.Target.DamageCalls==0,"Flak exact identity bypassed LOS/ping/history rejection");
        }
    } else if(name=="flak_explosion_guards") {
        for(int guard=0;guard<7;++guard) {
            FlakFixture f;auto& shell=f.Shells[2];
            if(guard==0) f.Gun.Role=1;
            if(guard==1) shell.bFakeClientProjectile=true;
            if(guard==2) shell.bExploded=true;
            if(guard==3) shell.ShotId=0;
            if(guard==4) shell.FiringWeapon=nullptr;
            if(guard==5) f.Gun.ActiveServerProjectiles[2].Projectile.Reset();
            f.Gun.OnTrackedFlakExploding(guard==6?nullptr:&shell,f.Target.History,FVector());
            const auto& entry=f.Gun.ActiveServerProjectiles[2];
            Require(entry.ExpireTime==-1.f&&!entry.bFlakExplosionObserved&&!entry.bFlakGraceEligible
                    &&f.Gun.TheWorld.OverlapQueries==0,
                    "invalid Flak terminal callback created trusted grace state");
        }
        FlakFixture f;f.Shells[2].Position=FVector(999.f,999.f,999.f);
        f.Gun.OnTrackedFlakExploding(&f.Shells[2],FVector(100.f,0.f,0.f),FVector(0.f,0.f,1.f));
        const auto& entry=f.Gun.ActiveServerProjectiles[2];
        Require(entry.FinalLoc.X==100.f&&entry.FinalLoc.Y==0.f&&entry.bFlakGraceEligible,
                "Flak terminal used actor position instead of the explosion hit location");
        f.Gun.TheWorld.Now=11.f;f.Gun.TheWorld.OverlapResults={&f.Target};
        f.Gun.OnTrackedFlakExploding(&f.Shells[2],FVector(888.f,888.f,888.f),FVector());
        Require(entry.ExpireTime==10.f&&entry.FinalLoc.X==100.f&&entry.PossibleSplashTargets.Num()==0
                &&f.Gun.TheWorld.OverlapQueries==1,
                "repeated Flak terminal callback rewrote its original snapshot");
    } else if(name=="flak_drop_repick") {
        for(bool samePawn:{false,true}) {
            FlakFixture f;f.Resolve(2);
            AUTProjectile legacy;AUTPlusProj_Rocket rocket;
            FActiveServerProjectile legacyEntry(&legacy,0),rocketEntry(&rocket,1);
            rocketEntry.LoadedVolleyId=41;rocketEntry.LoadedOwnershipEpoch=7;
            f.Gun.ActiveServerProjectiles.Add(legacyEntry);f.Gun.ActiveServerProjectiles.Add(rocketEntry);
            f.Gun.ClearFlakShellClaims();
            Require(f.Gun.ActiveServerProjectiles.Num()==2&&f.Gun.NextFlakShotId==103
                    &&f.Gun.ActiveServerProjectiles[0].Projectile.Get()==&legacy
                    &&f.Gun.ActiveServerProjectiles[1].Projectile.Get()==&rocket,
                    "drop cleanup removed unrelated entries or reset the Flak identity counter");
            f.Gun.UTOwner=nullptr;f.Claim(103);
            f.Gun.UTOwner=samePawn?&f.Shooter:&f.OtherPawn;f.Claim(103);
            auto& replacement=f.Shells[3];replacement.Instigator=f.Gun.UTOwner;
            f.Gun.CaptureFlakShellSpawn(&replacement);
            FActiveServerProjectile fresh(&replacement,1);fresh.FiringPawn=f.Gun.UTOwner;fresh.FlakShotId=replacement.ShotId;
            f.Gun.ActiveServerProjectiles.Add(fresh);f.Claim(103);
            Require(replacement.ShotId==104&&f.Gun.NextFlakShotId==104&&f.Hits()==0&&f.Target.DamageCalls==0,
                    "old dropped Flak claim followed the cannon into a new ownership lifetime");
            f.Claim(104);
            Require(replacement.ProcessHits==1&&f.Gun.ActiveServerProjectiles.Num()==2,
                    "repicked cannon could not accept its fresh Flak identity");
        }
    } else if(name=="flak_client_route") {
        FlakFixture f;AUTWeaponFix held;held.UTOwner=&f.Shooter;
        f.Shooter.Weapon=&held;f.Shooter.Inventory.Add(&held);
        held.NCPFiredProjClasses.Add(TSubclassOf<AUTProjectile>(f.Shells[2].GetClass()));
        Require(AUTWeaponFix::FindFiringWeaponForProjectile(&f.Shooter,&f.Shells[2])==&f.Gun,
                "Flak route used held/inventory class fallback after an ordinary weapon switch");
        f.Gun.Role=1;f.Shooter.DamageScaling=2.f;
        f.Gun.NotifyFakeProjectileHit(&f.Target,f.Target.History,0,&f.Shells[2]);
        Require(f.Gun.FlakRPCs==1&&f.Gun.LastClaimId==103&&f.Gun.LoadedRPCs==0&&f.Gun.LegacyRPCs==0
                &&f.Gun.ClientSounds.Predictions==1&&f.Gun.ClientSounds.LastDamage==200,
                "valid client Flak report did not use exact RPC and source projectile predicted damage");
        for(int guard=0;guard<10;++guard) {
            FlakFixture bad;bad.Gun.Role=1;AUTWeaponFix wrong;wrong.UTOwner=&bad.OtherPawn;
            DemoDriver replay;replay.Playing=true;
            auto& shell=bad.Shells[2];
            if(guard==0) shell.ShotId=0;
            if(guard==1) shell.FiringWeapon=nullptr;
            if(guard==2) shell.FiringWeapon=&wrong;
            if(guard==3) shell.Instigator=&bad.OtherPawn;
            if(guard==4) bad.Gun.UTOwner=&bad.OtherPawn;
            if(guard==5) bad.Gun.PendingKill=true;
            if(guard==6) shell.bFakeClientProjectile=true;
            if(guard==7) bad.Gun.bEnableProjectileRewind=false;
            if(guard==8) bad.Gun.TheWorld.DemoNetDriver=&replay;
            if(guard<=5)
                Require(AUTWeaponFix::FindFiringWeaponForProjectile(&bad.Shooter,&shell)==nullptr,
                        "unmapped/invalid Flak source found another weapon by class or held fallback");
            // A pending-kill source is stopped by FindFiringWeapon before Notify.
            if(guard!=5) bad.Gun.NotifyFakeProjectileHit(guard==9?nullptr:&bad.Target,bad.Target.History,0,&shell);
            Require(bad.Gun.FlakRPCs==0&&bad.Gun.LoadedRPCs==0&&bad.Gun.LegacyRPCs==0
                    &&bad.Gun.ClientSounds.Predictions==0,
                    "rejected Flak report downgraded to FIFO or played a predicted hitsound");
        }
    } else if(name=="flak_capture") {
        FlakFixture f;
        Require(f.Shells[0].ShotId==101&&f.Shells[1].ShotId==102&&f.Shells[2].ShotId==103
                &&f.Shells[2].FiringWeapon==&f.Gun&&f.Gun.NextFlakShotId==103,
                "authority did not assign distinct per-weapon Flak IDs and exact firing weapon");
        f.Gun.CaptureFlakShellSpawn(&f.Shells[2]);
        Require(f.Shells[2].ShotId==103&&f.Gun.NextFlakShotId==103,"repeated spawn capture changed Flak identity");
        for(int guard=0;guard<5;++guard) {
            FlakFixture rejected;auto& shell=rejected.Shells[3];shell.Instigator=&rejected.Shooter;
            if(guard==0) rejected.Gun.Role=1;
            if(guard==1) rejected.Gun.UTOwner=nullptr;
            if(guard==2) shell.bFakeClientProjectile=true;
            if(guard==3) shell.Instigator=&rejected.OtherPawn;
            rejected.Gun.CaptureFlakShellSpawn(guard==4?nullptr:&shell);
            Require(shell.ShotId==0&&shell.FiringWeapon==nullptr&&rejected.Gun.NextFlakShotId==103,
                    "invalid/fake/wrong-owner spawn received an authoritative Flak identity");
        }
        f.Gun.NextFlakShotId=MAX_uint32-1;f.Add(3);f.Add(4);
        Require(f.Shells[3].ShotId==MAX_uint32&&f.Shells[4].ShotId==0&&f.Shells[4].FiringWeapon==nullptr
                &&f.Gun.NextFlakShotId==MAX_uint32,"Flak identity counter wrapped and aliased an earlier shell");
    } else Require(false,"unknown Flak case");
}
int main(int argc,char** argv) {
    Require(argc==2,"case required");const std::string name(argv[1]);
    if(name.find("flak_")==0) { FlakCases(name);return 0; }
    if(name=="exact_sibling") {
        Fixture f;f.Claim(2);
        Require(f.Rockets[2].ProcessHits==1&&f.Rockets[0].ProcessHits==0&&f.Rockets[1].ProcessHits==0,
                "third loaded rocket claim consumed oldest sibling");
        Require(f.Gun.ActiveServerProjectiles.Num()==2&&f.Rockets[2].HitTarget==&f.Target,
                "exact live claim did not consume just its own tracked projectile");
    } else if(name=="out_of_order") {
        for(bool reverse:{false,true}) {
            Fixture f;
            const uint8 order[3]={uint8(reverse?1:2),uint8(reverse?2:0),uint8(reverse?0:1)};
            for(int count=0;count<3;++count) {
                f.Claim(order[count]);f.Claim(order[count]);
                Require(f.Hits()==count+1&&f.Gun.ActiveServerProjectiles.Num()==2-count,
                        "out-of-order or duplicate identity consumed another rocket");
            }
            for(uint8 ordinal:{uint8(0),uint8(1),uint8(2)})
                Require(f.Rockets[ordinal].ProcessHits==1,"one sibling was skipped or damaged twice");
        }
    } else if(name=="identity_guards") {
        for(int guard=0;guard<8;++guard) {
            Fixture f;
            if(guard==0) f.Claim(2,42);
            if(guard==1) f.Claim(2,41,6);
            if(guard==2) f.Claim(3);
            if(guard==3) { f.Gun.ActiveServerProjectiles[2].LoadedOwnershipEpoch=6;f.Claim(2); }
            if(guard==4) { f.Gun.ActiveServerProjectiles[2].FiringPawn=&f.OtherPawn;f.Claim(2); }
            if(guard==5) { f.Gun.ActiveServerProjectiles[2].FiringPawn.Reset();f.Claim(2); }
            if(guard==6) { f.Gun.ActiveServerProjectiles[2].FireMode=0;f.Claim(2); }
            if(guard==7) {
                AUTPlusWeap_RocketLauncher wrong;wrong.UTOwner=&f.Shooter;
                wrong.ServerLoadedRocketHitClaim_Implementation(&f.Target,f.Target.History,7,41,2);
                Require(wrong.ActiveServerProjectiles.Num()==0,"wrong weapon invented tracking state");
            }
            Require(f.Hits()==0&&f.Target.DamageCalls==0&&f.Gun.ActiveServerProjectiles.Num()==3,
                    "unmatched epoch/id/ordinal/owner/fire-mode/weapon was accepted or consumed another entry");
        }
        Fixture transferred;transferred.Gun.UTOwner=&transferred.OtherPawn;transferred.Claim(2);
        Require(transferred.Hits()==0,"owner transfer accepted the previous pawn's in-flight claim");
    } else if(name=="rpc_guards") {
        for(int guard=0;guard<7;++guard) {
            Fixture f;
            if(guard==0) f.Gun.Role=1;
            if(guard==1) f.Shooter.OwnController.Confirmed=false;
            if(guard==2) f.Shooter.Controller=nullptr;
            if(guard==3) f.Gun.UTOwner=nullptr;
            if(guard==4) { f.Claim(2,41,0); }
            if(guard==5) { f.Claim(2,0,7); }
            if(guard==6) {
                AUTWeaponFix wrongType;wrongType.UTOwner=&f.Shooter;
                wrongType.ActiveServerProjectiles=f.Gun.ActiveServerProjectiles;
                wrongType.ServerLoadedRocketHitClaim_Implementation(&f.Target,f.Target.History,7,41,2);
            }
            if(guard<4) f.Claim(2);
            Require(f.Hits()==0&&f.Target.DamageCalls==0,"loaded RPC escaped protocol/authority/weapon/identity gate");
        }
        for(bool local:{false,true}) {
            Fixture f;f.Shooter.OwnController.Local=local;f.Shooter.OwnController.Confirmed=!local;
            AUTWeaponFix equippedOther;f.Shooter.Weapon=&equippedOther;
            f.Claim(2);
            Require(f.Rockets[2].ProcessHits==1,
                    "valid local/confirmed owner lost in-flight claim after ordinary weapon switch");
        }
    } else if(name=="legacy") {
        Fixture f;
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,0);
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,1);
        Require(f.Hits()==0&&f.Gun.ActiveServerProjectiles.Num()==3,
                "legacy claim consumed an identified loaded rocket");
        f.Add(3,0,0,0,0);f.Add(4,0,0,0,0);
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,0);
        Require(f.Rockets[3].ProcessHits==1&&f.Rockets[4].ProcessHits==0&&f.Hits()==1,
                "legacy primary stopped honoring its own FIFO path");
        f.Add(5,0,0,1,0);
        f.Gun.ServerProjectileHitClaim_Implementation(&f.Target,f.Target.History,1);
        Require(f.Rockets[5].ProcessHits==1&&f.Rockets[0].ProcessHits==0,
                "legacy alt selected a loaded sibling instead of the untagged projectile");
        f.Claim(2);Require(f.Rockets[2].ProcessHits==1,"legacy processing damaged exact loaded identity lookup");
    } else if(name=="grace_spawn") {
        Fixture f;f.Resolve(2);f.Gun.TheWorld.Now+=.05f;
        f.Add(3,42,0);f.Gun.PruneTrackedProjectiles(f.Gun.TheWorld.Now);
        Require(f.Gun.ActiveServerProjectiles.Num()==4,"new spawn discarded resolved rocket's unexpired grace snapshot");
        f.Claim(2);
        Require(f.Target.DamageCalls==1&&f.Target.DamageTotal==100.f&&f.Hits()==0
                &&f.Gun.ActiveServerProjectiles.Num()==3,
                "exact resolved sibling fell back to a live sibling or lost grace rescue");
        f.Claim(2);Require(f.Target.DamageCalls==1&&f.Hits()==0,"grace duplicate consumed another rocket");
        f.Claim(0,42);Require(f.Rockets[3].ProcessHits==1,"grace claim consumed the newly spawned volley");
    } else if(name=="direct_hit") {
        Fixture f;f.Resolve(2,&f.Target);f.Gun.TheWorld.Now+=.05f;
        f.Claim(2);f.Claim(2);
        Require(f.Target.DamageCalls==0&&f.Hits()==0&&f.Gun.ActiveServerProjectiles.Num()==3,
                "claim reapplied authoritative direct hit or stole a live sibling");
        f.Claim(0);Require(f.Rockets[0].ProcessHits==1,"direct hit guard broke another valid sibling");
    } else if(name=="prune") {
        Fixture f;f.Resolve(0);f.Gun.ActiveServerProjectiles[1].Projectile.Reset();
        f.Gun.TheWorld.Now+=.201f;f.Gun.PruneTrackedProjectiles(f.Gun.TheWorld.Now);
        Require(f.Gun.ActiveServerProjectiles.Num()==1&&f.Gun.ActiveServerProjectiles[0].LoadedRocketOrdinal==2,
                "pruning kept expired/unresolved entries or discarded live projectile");
        f.Claim(0);Require(f.Hits()==0&&f.Target.DamageCalls==0,"expired grace was rescued");
        for(int i=3;i<16;++i) f.Add(i,uint32(i),0);
        f.Gun.PruneTrackedProjectiles(f.Gun.TheWorld.Now);
        Require(f.Gun.ActiveServerProjectiles.Num()==10
                &&f.Gun.ActiveServerProjectiles[0].Projectile.Get()==&f.Rockets[6],
                "tracking cap did not retain the newest ten entries");
        Fixture disabled;disabled.Resolve(2);CVarRocketLagCompGraceMs.Value=0.f;
        disabled.Gun.PruneTrackedProjectiles(disabled.Gun.TheWorld.Now);
        Require(disabled.Gun.ActiveServerProjectiles.Num()==2,"disabled grace retained resolved entry");
    } else if(name=="validation") {
        for(bool grace:{false,true}) for(int guard=0;guard<10;++guard) {
            Fixture f;
            if(guard==0) f.Rockets[2].Position=FVector(100.f,1000.f,0.f);
            if(grace) f.Resolve(2);
            FVector anchor=f.Target.History;
            if(guard==1) anchor=FVector(100.f,1000.f,0.f);
            if(guard==2) f.Gun.TheWorld.Wall=true;
            if(guard==3) f.Shooter.State.ExactPing=151.f;
            if(guard==4) f.Target.Dead=true;
            if(guard==5) f.Gun.TheWorld.GS.SameTeam=true;
            if(guard==6) f.Gun.bEnableProjectileRewind=false;
            if(guard==7) CVarRocketLagComp.Value=0.f;
            if(guard==8) anchor.X=std::numeric_limits<float>::quiet_NaN();
            if(guard==9) f.Shooter.PlayerState=nullptr;
            f.Gun.ServerLoadedRocketHitClaim_Implementation(&f.Target,anchor,7,41,2);
            Require(f.Hits()==0&&f.Target.DamageCalls==0&&f.Gun.ActiveServerProjectiles.Num()==3,
                    "identity-matched claim bypassed contact/history/LOS/ping/target/team/feature validation");
        }
    } else if(name=="damage_origin") {
        Fixture f;f.Rockets[2].Position=FVector(130.f,0.f,20.f);f.Resolve(2);
        const FVector anchor(140.f,0.f,20.f);
        f.Gun.ServerLoadedRocketHitClaim_Implementation(&f.Target,anchor,7,41,2);
        Require(f.Target.DamageCalls==1&&f.Target.DamageOrigin.X==100.f&&f.Target.DamageOrigin.Z==20.f
                &&FVector::DistSquared(f.Target.DamageOrigin,anchor)>0.f,
                "client anchor replaced server-computed capsule contact for grace damage");
    } else if(name=="reentrant") {
        for(bool grace:{false,true}) {
            Fixture f;if(grace) f.Resolve(2);
            int callbacks=0;
            auto reenter=[&]() {
                ++callbacks;
                Require(f.Gun.ActiveServerProjectiles.Num()==2,"tracking entry was still present inside damage callback");
                f.Add(3,42,0);
                f.Claim(2);
            };
            if(grace) f.Target.OnDamage=reenter;else f.Rockets[2].OnHit=reenter;
            f.Claim(2);
            Require(callbacks==1&&f.Gun.ActiveServerProjectiles.Num()==3
                    &&(grace?f.Target.DamageCalls==1:f.Rockets[2].ProcessHits==1),
                    "reentrant callback reused identity or removed another tracking entry");
            f.Claim(0,42);Require(f.Rockets[3].ProcessHits==1,"callback-created tracking entry was lost");
        }
    } else if(name=="explosion_snapshot") {
        Fixture f;AUTProjectile master;UObject scenery;
        master.DamageParams.OuterRadius=321.f;
        f.Rockets[2].MasterProjectile=&master;
        f.Rockets[2].DamageParams.OuterRadius=99.f;
        f.Rockets[2].Position=FVector(999.f,999.f,999.f);
        f.Rockets[2].Velocity=FVector(10.f,20.f,30.f);
        f.Rockets[2].Movement.Gravity=-980.f;
        f.Rockets[2].ImpactedActor=&f.OtherPawn;
        f.Gun.TheWorld.OverlapBlocking=false;
        f.Gun.TheWorld.OverlapResults={&f.Target,&f.Target,&scenery,&f.OtherPawn,nullptr};
        const FVector location(100.f,0.f,0.f),normal(0.f,0.f,1.f);
        f.Gun.OnTrackedRocketExploding(&f.Rockets[2],location,normal);
        const auto& entry=f.Gun.ActiveServerProjectiles[2];
        Require(entry.bLoadedExplosionObserved&&entry.ExpireTime==10.f&&entry.DamagedTarget.Get()==&f.OtherPawn
                &&FVector::DistSquared(entry.FinalLoc,location)==0.f
                &&entry.FinalVel.Y==20.f&&entry.FinalGravityZ==-980.f,
                "actual terminal hook did not snapshot explosion position, movement and direct victim");
        Require(f.Gun.TheWorld.OverlapQueries==1&&f.Gun.TheWorld.LastOverlapChannel==COLLISION_TRACE_WEAPON
                &&f.Gun.TheWorld.LastOverlapRadius==321.f
                &&FVector::DistSquared(f.Gun.TheWorld.LastOverlapOrigin,location+normal)==0.f
                &&master.DamageParamQueries==1&&f.Rockets[2].DamageParamQueries==0
                &&FVector::DistSquared(master.LastDamageParamLocation,location)==0.f,
                "splash guard did not use stock master damage radius and exact explosion query origin");
        Require(entry.PossibleSplashTargets.Num()==2&&entry.PossibleSplashTargets.Contains(&f.Target)
                &&entry.PossibleSplashTargets.Contains(&f.OtherPawn),
                "nonblocking overlaps were discarded, noncharacters accepted, or duplicates retained");
        f.Gun.TheWorld.Now=10.1f;f.Gun.TheWorld.OverlapResults.clear();
        f.Gun.OnTrackedRocketExploding(&f.Rockets[2],FVector(888.f,888.f,888.f),normal);
        Require(entry.ExpireTime==10.f&&entry.PossibleSplashTargets.Num()==2
                &&entry.FinalLoc.X==100.f&&f.Gun.TheWorld.OverlapQueries==1,
                "repeated terminal callback replaced the original splash snapshot");
    } else if(name=="explosion_guards") {
        for(int guard=0;guard<6;++guard) {
            Fixture f;
            if(guard==0) f.Gun.Role=1;
            if(guard==1) f.Rockets[2].bFakeClientProjectile=true;
            if(guard==2) f.Rockets[2].bExploded=true;
            if(guard==3) f.Rockets[2].LoadedVolleyId=0;
            if(guard==4) f.Gun.ActiveServerProjectiles[2].Projectile.Reset();
            f.Gun.OnTrackedRocketExploding(guard==5?nullptr:&f.Rockets[2],f.Target.History,FVector());
            Require(f.Gun.ActiveServerProjectiles[2].ExpireTime==-1.f
                    &&!f.Gun.ActiveServerProjectiles[2].bLoadedExplosionObserved
                    &&f.Gun.TheWorld.OverlapQueries==0,
                    "non-authoritative/fake/already-exploded/untagged/untracked/null rocket created trusted snapshot");
        }
    } else if(name=="explosion_geometry") {
        for(int invalid=0;invalid<5;++invalid) {
            Fixture f;FVector location=f.Target.History,normal;
            if(invalid==0) f.Rockets[2].DamageParams.OuterRadius=-1.f;
            if(invalid==1) f.Rockets[2].DamageParams.OuterRadius=std::numeric_limits<float>::quiet_NaN();
            if(invalid==2) f.Rockets[2].DamageParams.OuterRadius=std::numeric_limits<float>::infinity();
            if(invalid==3) location.X=std::numeric_limits<float>::quiet_NaN();
            if(invalid==4) normal.Z=std::numeric_limits<float>::infinity();
            f.Gun.OnTrackedRocketExploding(&f.Rockets[2],location,normal);
            Require(f.Gun.ActiveServerProjectiles[2].ExpireTime==10.f
                    &&!f.Gun.ActiveServerProjectiles[2].bLoadedExplosionObserved
                    &&f.Gun.TheWorld.OverlapQueries==0,
                    "invalid explosion origin/radius became trusted or ran invalid overlap query");
            f.Rockets[2].bExploded=true;f.Gun.ActiveServerProjectiles[2].Projectile.Reset();f.Claim(2);
            Require(f.Target.DamageCalls==0&&f.Hits()==0,"untrusted explosion geometry accepted exact grace");
        }
        Fixture zero;zero.Rockets[2].DamageParams.OuterRadius=0.f;zero.Resolve(2);
        Require(zero.Gun.ActiveServerProjectiles[2].bLoadedExplosionObserved
                &&zero.Gun.TheWorld.OverlapQueries==0,
                "valid zero-radius explosion invented splash candidates or lost trusted terminal snapshot");
        zero.Claim(2);Require(zero.Target.DamageCalls==1,"zero-radius terminal prevented otherwise valid exact grace");
    } else if(name=="splash_guard") {
        for(bool wall:{false,true}) {
            Fixture f;f.Gun.TheWorld.Wall=wall;
            f.Gun.TheWorld.OverlapBlocking=false;f.Gun.TheWorld.OverlapResults={&f.Target};
            f.Resolve(2);f.Claim(2);f.Claim(2);
            Require(f.Target.DamageCalls==0&&f.Hits()==0&&f.Gun.ActiveServerProjectiles.Num()==3
                    &&f.Target.HistoryQueries==0&&f.Gun.TheWorld.WallQueries==0,
                    "possible splash victim received full-damage top-up or guard depended on LOS");
        }
        Fixture outside;outside.Target.Position=FVector(1000.f,0.f,0.f);
        outside.Gun.TheWorld.OverlapResults={&outside.OtherPawn};outside.Resolve(2);outside.Claim(2);
        Require(outside.Target.DamageCalls==1&&outside.Target.DamageTotal==100.f,
                "outside-candidate target lost valid exact grace rescue");
    } else if(name=="low_ping") {
        for(float ping:{0.f,10.f,20.f,40.f,150.f}) for(bool grace:{false,true}) {
            Fixture f;f.Shooter.State.ExactPing=ping;
            if(grace) {
                f.Resolve(2);
                // Model a terminal callback just before the delayed claim,
                // including the zero-ping same-frame close-range race.
                f.Gun.TheWorld.Now+=ping*.0005f;
            }
            f.Claim(2);
            Require(grace ? f.Target.DamageCalls==1&&f.Hits()==0
                          : f.Rockets[2].ProcessHits==1&&f.Target.DamageCalls==0,
                    "valid low-ping or cutoff-boundary exact live/grace claim was rejected");
            Require(f.Rockets[0].ProcessHits==0&&f.Rockets[1].ProcessHits==0
                    &&f.Gun.ActiveServerProjectiles.Num()==2,
                    "low-ping claim consumed another loaded sibling");
            const float expectedWindow=std::min(.2f,std::max(.016f,ping*.0011f));
            const float step=1.f/240.f;
            Require(f.Target.HistoryQueries>1&&f.Target.MaxRewind<=expectedWindow+.0001f
                    &&f.Target.MaxRewind>=expectedWindow-step-.0001f,
                    "low-ping rewind did not retain the 16 ms floor, full-RTT slack and bounded history search");
            f.Claim(2);
            Require(grace ? f.Target.DamageCalls==1 : f.Rockets[2].ProcessHits==1,
                    "low-ping duplicate reapplied exact claim damage");
        }
        Fixture cap;cap.Shooter.State.ExactPing=250.f;CVarRocketLagCompMaxPingMs.Value=300.f;
        cap.Claim(2);
        Require(cap.Rockets[2].ProcessHits==1&&cap.Target.MaxRewind<=.2001f
                &&cap.Target.MaxRewind>=.2f-1.f/240.f-.0001f,
                "configured ping allowance expanded rewind past its independent 200 ms window cap");
    } else if(name=="unknown_snapshot") {
        Fixture loaded;loaded.Resolve(2,nullptr,false);loaded.Claim(2);
        Require(loaded.Target.DamageCalls==0&&loaded.Hits()==0,
                "old generic resolution snapshot bypassed the loaded explosion splash guard");
        Fixture legacy;legacy.Add(3,0,0,0,0);legacy.Resolve(3);
        Require(!legacy.Gun.ActiveServerProjectiles[3].bLoadedExplosionObserved,
                "fixture accidentally created a loaded explosion snapshot for primary fire");
        legacy.Gun.ServerProjectileHitClaim_Implementation(&legacy.Target,legacy.Target.History,0);
        Require(legacy.Target.DamageCalls==1&&legacy.Target.DamageTotal==100.f,
                "loaded grace trust requirement leaked into legacy primary grace");
    } else Require(false,"unknown case");
}
