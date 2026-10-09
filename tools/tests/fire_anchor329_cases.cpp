#include "NCFireAnchor.cpp"
#include <limits>

struct Fixture {
    UWorld World,NextWorld;
    UNetDriver Driver;
    UNetConnection Connection,NextConnection;
    APlayerController Controller,NextController;
    UUTCharacterMovement Movement;
    AUTWeapon Weapon,NextWeapon;
    AUTCharacter Pawn;
    Fixture() {
        NCFireAnchor::Shutdown();
        NCFireAnchor::Startup();
        CVarAnchorMode.Value=1;
        Controller.World=NextController.World=&World;
        Connection.Driver=NextConnection.Driver=&Driver;
        Controller.Connection=&Connection;
        NextController.Connection=&NextConnection;
        Pawn.World=Weapon.World=NextWeapon.World=&World;
        Pawn.Controller=&Controller;
        Pawn.UTCharacterMovement=&Movement;
        Pawn.Weapon=&Weapon;
        Weapon.Owner=NextWeapon.Owner=&Pawn;
    }
    void Mark() { NCFireAnchor::RecordMove(&Pawn,true); }
    FNCFireAnchor Resolve() {
        return NCFireAnchor::Resolve(&Weapon,&Pawn,0,1,Pawn.Stamp,Pawn.Eye,Pawn.Aim,0.04f,80.f,0.125f);
    }
};
static int Passed=0;
#define CHECK(value) do { if(!(value)) {std::cerr<<"FAILED "<<__LINE__<<": " #value "\n";return 1;} ++Passed; } while(false)
static bool Near(float a,float b) { return std::fabs(a-b)<0.00001f; }
int main() {
    const float NaN=std::numeric_limits<float>::quiet_NaN();
    const float Inf=std::numeric_limits<float>::infinity();
    {
        Fixture f;
        CHECK(NCFireAnchor::Mode()==1);
        const int Adds=FWorldDelegates::OnWorldCleanup.Adds;
        NCFireAnchor::Startup();
        CHECK(FWorldDelegates::OnWorldCleanup.Adds==Adds);
        CHECK(!f.Resolve().bValid && f.World.Traces==0);
        f.Mark();
        f.World.Time+=0.04f;
        FNCFireAnchor a=f.Resolve();
        CHECK(a.bValid && !a.bEnforce && Near(a.ExtraAtAccept,0.04f));
        CHECK(a.World.Get()==&f.World && a.Controller.Get()==&f.Controller && a.Connection.Get()==&f.Connection);
        float extra=0.f;
        CHECK(NCFireAnchor::Dispatch(a,f.World.Time+0.02f,extra) && Near(extra,0.06f));
        CHECK(!f.Resolve().bValid && f.World.Traces==1);
        f.Pawn.Stamp+=0.01f;
        f.Mark();
        CHECK(f.Resolve().bValid); // New marker available, old accepted shot remains immutable.
        CHECK(NCFireAnchor::Dispatch(a,f.World.Time+0.02f,extra));
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time+0.06f,extra)); // Total cap is unchanged.
    }
    {
        Fixture f;f.Mark();
        FNCFireAnchor a=NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,FVector(NaN,0,0),f.Pawn.Aim,0.04f,80.f,0.125f);
        CHECK(!a.bValid && f.World.Traces==0);
        CHECK(!f.Resolve().bValid && f.World.Traces==0); // Bad matched claim consumed once.
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,f.Pawn.Eye,FRotator(Inf,0,0),0.04f,80.f,0.125f).bValid);
        CHECK(f.World.Traces==0);
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,f.Pawn.Eye,f.Pawn.Aim,NaN,80.f,0.125f).bValid);
        CHECK(f.World.Traces==0);
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,f.Pawn.Eye,f.Pawn.Aim,0.04f,80.f,Inf).bValid);
        CHECK(f.World.Traces==0);
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,FVector(121.f,100.f,100.f),f.Pawn.Aim,0.04f,80.f,0.125f).bValid);
        CHECK(f.World.Traces==0);
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp,f.Pawn.Eye,FRotator(0,2.1f,0),0.04f,80.f,0.125f).bValid);
        CHECK(f.World.Traces==0);
    }
    {
        Fixture f;f.Mark();f.World.Obstructed=true;
        CHECK(!f.Resolve().bValid && f.World.Traces==1);
    }
    {
        Fixture f;f.Mark();
        CHECK(!NCFireAnchor::Resolve(&f.Weapon,&f.Pawn,0,1,f.Pawn.Stamp-0.001f,f.Pawn.Eye,f.Pawn.Aim,0.04f,80.f,0.125f).bValid);
        CHECK(f.Resolve().bValid); // Wrong stamp cannot select a nearby sample.
    }
    {
        Fixture f;f.Mark();f.World.Time+=0.081f;
        CHECK(!f.Resolve().bValid && f.World.Traces==0);
    }
    {
        Fixture f;CVarAnchorMode.Value=2;f.Mark();
        FNCFireAnchor a=f.Resolve();float extra=0.f;
        CHECK(a.bValid && a.bEnforce);
        f.Controller.Connection=&f.NextConnection;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        f.Mark();
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        CHECK(f.Resolve().bValid); // New connection identity has its own generation.
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.Controller=&f.NextController;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        f.Mark();
        CHECK(f.Resolve().bValid && !NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.World=f.Weapon.World=f.Controller.World=&f.NextWorld;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        f.Mark();CHECK(f.Resolve().bValid);
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        FWorldDelegates::OnWorldCleanup.Broadcast(&f.World);
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        CHECK(!f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.Stamp=0.5f;NCFireAnchor::RecordMove(&f.Pawn,false);
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        f.Pawn.Stamp+=0.01f;f.Mark();CHECK(f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Movement.bJustTeleported=true;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        NCFireAnchor::RecordMove(&f.Pawn,false);
        f.Movement.bJustTeleported=false;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.SavedPositions.push_back({true,f.World.Time+0.01f});
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time+0.02f,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.Dead=true;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        NCFireAnchor::RecordMove(&f.Pawn,false);
        f.Pawn.Dead=false;f.Mark();
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.Weapon=&f.NextWeapon;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        NCFireAnchor::RecordMove(&f.Pawn,false);
        f.Pawn.Weapon=&f.Weapon;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        NCFireAnchor::InvalidateWeapon(&f.Weapon);
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        CHECK(!f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.UTCharacterMovement=nullptr;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
        CHECK(!f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();f.Connection.LastRecvAckTime=97.9f;
        CHECK(!f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Connection.State=USOCK_Closed;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        f.Pawn.Serial++;
        CHECK(!NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    {
        Fixture f;CVarAnchorMode.Value=0;f.Mark();
        CHECK(!f.Resolve().bValid && f.World.Traces==0);
    }
    {
        Fixture f;f.Controller.Local=true;f.Mark();
        CHECK(!f.Resolve().bValid);
    }
    {
        Fixture f;f.Mark();FNCFireAnchor a=f.Resolve();float extra=0.f;
        NCFireAnchor::Shutdown();
        CHECK(!FWorldDelegates::OnWorldCleanup.Fn && !NCFireAnchor::Dispatch(a,f.World.Time,extra));
    }
    CHECK(NCFireAnchorPolicy::AckIsRecent(100.0,98.0));
    CHECK(!NCFireAnchorPolicy::AckIsRecent(100.0,97.99));
    CHECK(!NCFireAnchorPolicy::AckIsRecent(100.0,100.01));
    CHECK(!NCFireAnchorPolicy::AckIsRecent(Inf,100.0));
    CHECK(NCFireAnchorPolicy::AckIsRecent(1000000.032,static_cast<float>(1000000.032)));
    CHECK(!NCFireAnchorPolicy::AckIsRecent(1000000.032,static_cast<float>(1000003.0)));
    std::cout<<"PASS "<<Passed<<" anchor lifecycle and policy checks\n";
}
