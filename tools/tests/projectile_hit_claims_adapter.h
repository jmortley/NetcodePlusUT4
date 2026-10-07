#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include "NCRocketVolley.h"
using uint8 = uint8_t;
using uint32 = uint32_t;
using int32 = int32_t;
#define UPROPERTY(...)
#define GENERATED_BODY()
#define TEXT(value) value
template<class... T> void TraceLog(const char*, const T&...) {}
#define UE_LOG(category, level, ...) TraceLog(__VA_ARGS__)
constexpr int ROLE_Authority = 3, COLLISION_TRACE_WEAPON = 1;
constexpr float BIG_NUMBER = 1.e30f, KINDA_SMALL_NUMBER = .0001f;
struct FVector {
    float X, Y, Z;
    FVector(float x=0.f, float y=0.f, float z=0.f) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& b) const { return FVector(X+b.X,Y+b.Y,Z+b.Z); }
    FVector operator-(const FVector& b) const { return FVector(X-b.X,Y-b.Y,Z-b.Z); }
    FVector operator*(float a) const { return FVector(X*a,Y*a,Z*a); }
    float SizeSquared() const { return X*X+Y*Y+Z*Z; }
    float Size() const { return std::sqrt(SizeSquared()); }
    bool ContainsNaN() const { return !std::isfinite(X)||!std::isfinite(Y)||!std::isfinite(Z); }
    FVector GetSafeNormal() const { const float size=Size(); return size>0.f ? *this*(1.f/size) : FVector(); }
    static float DistSquared(FVector a,FVector b) { return (a-b).SizeSquared(); }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
struct FMath {
    static float Max(float a,float b) { return std::max(a,b); }
    static float Clamp(float a,float lo,float hi) { return std::max(lo,std::min(a,hi)); }
    static float Square(float a) { return a*a; }
    static float Sqrt(float a) { return std::sqrt(a); }
    static bool IsFinite(float a) { return std::isfinite(a); }
    static FVector ClosestPointOnSegment(FVector point,FVector a,FVector b) {
        const FVector span=b-a, offset=point-a;
        const float t=span.SizeSquared()>0.f ? Clamp((offset.X*span.X+offset.Y*span.Y+offset.Z*span.Z)/span.SizeSquared(),0.f,1.f) : 0.f;
        return a+span*t;
    }
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::operator=;
    int32 Num() const { return static_cast<int32>(this->size()); }
    bool IsValidIndex(int32 i) const { return i>=0&&i<Num(); }
    void Add(const T& value) { this->push_back(value); }
    bool Contains(const T& value) const { return std::find(this->begin(),this->end(),value)!=this->end(); }
    void AddUnique(const T& value) { if(!Contains(value)) Add(value); }
    void RemoveAt(int32 i) { this->erase(this->begin()+i); }
    T& Last() { return this->back(); }
};
template<class T> void* operator new(std::size_t,TArray<T>& array) { array.emplace_back(); return &array.back(); }
template<class T> void operator delete(void*,TArray<T>&) noexcept {}
template<class T> struct TWeakObjectPtr {
    T* Value=nullptr;
    TWeakObjectPtr()=default;
    TWeakObjectPtr(T* value):Value(value){}
    TWeakObjectPtr& operator=(T* value) { Value=value;return *this; }
    T* Get() const { return Value; }
    bool IsValid() const { return Value!=nullptr; }
    bool operator==(const TWeakObjectPtr& other) const { return Value==other.Value; }
    void Reset() { Value=nullptr; }
};
struct UObject { virtual ~UObject()=default; };
template<class T,class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct UDamageType { static void* StaticClass() { static int type;return &type; } };
template<class T> struct TSubclassOf {
    void* Value=nullptr;
    TSubclassOf()=default;
    TSubclassOf(std::nullptr_t){}
    TSubclassOf(void* value):Value(value){}
    explicit operator bool() const { return Value!=nullptr; }
};
struct AController : UObject {};
struct AUTBot : AController {};
struct APlayerController : AController {
    bool Local=false,Confirmed=true;
    bool IsLocalController() const { return Local; }
};
namespace NCPlusVersionGate {
    bool IsProtocolConfirmed(APlayerController* pc) { return pc&&pc->Confirmed; }
}
struct AUTPlayerState { float ExactPing=40.f; };
struct UCapsuleComponent {
    float Radius=40.f,HalfHeight=108.f;
    float GetScaledCapsuleRadius() const { return Radius; }
    float GetScaledCapsuleHalfHeight() const { return HalfHeight; }
};
class AUTCharacter;
struct FHitResult {
    FVector TraceStart,TraceEnd;
    FHitResult()=default;
    FHitResult(AUTCharacter*,UCapsuleComponent*,FVector,FVector){}
};
struct FRadialDamageParams {
    float BaseDamage=0.f,MinimumDamage=0.f,OuterRadius=200.f;
    FRadialDamageParams()=default;
    FRadialDamageParams(float damage,float):BaseDamage(damage){}
};
struct FUTRadialDamageEvent {
    float BaseMomentumMag=0.f;
    FRadialDamageParams Params;
    TSubclassOf<UDamageType> DamageTypeClass;
    FVector Origin,ShotDirection;
    TArray<FHitResult> ComponentHits;
};
struct AUTWeaponFix;
class AUTCharacter : public UObject {
public:
    bool Dead=false;
    AUTPlayerState State;
    AUTPlayerState* PlayerState=&State;
    APlayerController OwnController;
    AController* Controller=&OwnController;
    AUTWeaponFix* Weapon=nullptr;
    FVector Position=FVector(100.f,0.f,0.f),History=Position;
    UCapsuleComponent Capsule;
    int DamageCalls=0,HistoryQueries=0;
    float DamageTotal=0.f,MaxRewind=0.f;
    FVector DamageOrigin;
    std::function<void()> OnDamage;
    bool IsDead() const { return Dead; }
    UCapsuleComponent* GetCapsuleComponent() { return &Capsule; }
    FVector GetActorLocation() const { return Position; }
    FVector GetRewindLocation(float delta) { ++HistoryQueries;MaxRewind=std::max(MaxRewind,delta);return History; }
    const char* GetName() const { return "target"; }
    AController* GetController() { return Controller; }
    AUTWeaponFix* GetWeapon() { return Weapon; }
    float TakeDamage(float damage,const FUTRadialDamageEvent& event,AController*,AUTWeaponFix*) {
        ++DamageCalls;DamageTotal+=damage;DamageOrigin=event.Origin;
        if(OnDamage) { auto callback=OnDamage;OnDamage=nullptr;callback(); }
        return damage;
    }
};
struct USphereComponent { float Radius=10.f;float GetScaledSphereRadius() const { return Radius; } };
struct UProjectileMovement { float Gravity=0.f;float GetGravityZ() const { return Gravity; } };
struct AUTProjectile : UObject {
    bool bExploded=false,PendingKill=false;
    int ProcessHits=0;
    FVector Position=FVector(100.f,0.f,0.f),Velocity;
    AUTCharacter* HitTarget=nullptr;
    std::function<void()> OnHit;
    UProjectileMovement Movement;
    UProjectileMovement* ProjectileMovement=&Movement;
    USphereComponent Collision,Overlap;
    USphereComponent* CollisionComp=&Collision;
    USphereComponent* PawnOverlapSphere=&Overlap;
    FRadialDamageParams DamageParams=FRadialDamageParams(100.f,1.f);
    AUTProjectile* MasterProjectile=nullptr;
    UObject* ImpactedActor=nullptr;
    int DamageParamQueries=0;
    FVector LastDamageParamLocation;
    float Momentum=1000.f;
    TSubclassOf<UDamageType> MyDamageType=UDamageType::StaticClass();
    bool IsPendingKillPending() const { return PendingKill; }
    FVector GetActorLocation() const { return Position; }
    FVector GetVelocity() const { return Velocity; }
    FRadialDamageParams GetDamageParams(UObject*,FVector location,float&) {
        ++DamageParamQueries;LastDamageParamLocation=location;return DamageParams;
    }
    void ProcessHit(AUTCharacter* target,UCapsuleComponent*,FVector,FVector) {
        ++ProcessHits;HitTarget=target;
        if(OnHit) { auto callback=OnHit;OnHit=nullptr;callback(); }
        bExploded=true;
    }
};
class AUTPlusProj_Rocket : public AUTProjectile {
public:
    bool bFakeClientProjectile=false;
    uint32 LoadedVolleyId=41;
};
// TRACKED_ENTRY
struct AUTGameState {
    bool SameTeam=false;
    bool OnSameTeam(AUTCharacter*,AUTCharacter*) const { return SameTeam; }
};
struct FCollisionQueryParams {
    FCollisionQueryParams(const char*,bool,AUTProjectile*){}
    void AddIgnoredActor(AUTCharacter*){}
};
struct FQuat { static const FQuat Identity; };
const FQuat FQuat::Identity;
struct FCollisionShape {
    float Radius;
    static FCollisionShape MakeSphere(float radius) { return {radius}; }
};
struct FOverlapResult {
    UObject* Actor;
    FOverlapResult(UObject* actor=nullptr):Actor(actor){}
    UObject* GetActor() const { return Actor; }
};
struct World {
    float Now=10.f;
    bool Wall=false;
    int WallQueries=0;
    bool OverlapBlocking=false;
    int OverlapQueries=0,LastOverlapChannel=-1;
    float LastOverlapRadius=0.f;
    FVector LastOverlapOrigin;
    TArray<FOverlapResult> OverlapResults;
    AUTGameState GS;
    float GetTimeSeconds() const { return Now; }
    template<class T> T* GetGameState() { return &GS; }
    bool LineTraceTestByChannel(FVector,FVector,int,const FCollisionQueryParams&) { ++WallQueries;return Wall; }
    bool OverlapMultiByChannel(TArray<FOverlapResult>& results,FVector origin,FQuat,int channel,
                              FCollisionShape shape,const FCollisionQueryParams&) {
        ++OverlapQueries;LastOverlapOrigin=origin;LastOverlapRadius=shape.Radius;
        LastOverlapChannel=channel;results=OverlapResults;return OverlapBlocking;
    }
};
struct TestCVar { float Value;float GetValueOnGameThread() const { return Value; } };
TestCVar CVarRocketLagComp{1.f},CVarRocketLagCompMaxPingMs{150.f};
TestCVar CVarRocketLagCompMaxWindowMs{200.f},CVarRocketLagCompGraceMs{200.f};
bool RocketLagCompDbg() { return false; }
void ApplySlidePostureForValidation(AUTCharacter*,float,FVector&,float&) {}
struct AUTWeaponFix : UObject {
    int Role=ROLE_Authority;
    bool bEnableProjectileRewind=true,PendingKill=false;
    AUTCharacter* UTOwner=nullptr;
    TArray<FActiveServerProjectile> ActiveServerProjectiles;
    World TheWorld;
    World* GetWorld() { return &TheWorld; }
    AUTCharacter* GetUTOwner() { return UTOwner; }
    TWeakObjectPtr<AController> FireProtocolController;
    bool Is329FireProtocolReady() const;
    bool IsPendingKillPending() const { return PendingKill; }
    void OnTrackedProjectileResolved(AUTProjectile*,AUTCharacter*);
    void OnTrackedRocketExploding(AUTPlusProj_Rocket*,const FVector&,const FVector&);
    void PruneTrackedProjectiles(float);
    void ServerProjectileHitClaim_Implementation(AUTCharacter*,FVector,uint8);
    void ServerLoadedRocketHitClaim_Implementation(AUTCharacter*,FVector,uint32,uint32,uint8);
    void ProcessProjectileHitClaim(AUTCharacter*,FVector,uint8,uint32,uint32,uint8);
};
struct AUTPlusWeap_RocketLauncher : AUTWeaponFix {
    uint32 LoadedOwnershipEpoch=7;
};
void Require(bool condition,const char* message) {
    if(!condition) { std::cerr<<message<<'\n';std::exit(1); }
}
