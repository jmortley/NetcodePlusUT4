#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>
using int32 = int32_t;
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#define TEXT(value) value
#define ECVF_Default 0
#define DEFINE_LOG_CATEGORY_STATIC(...)
#define UE_LOG(category,verbosity,...) AdapterLog(__VA_ARGS__)
#define ANSI_TO_TCHAR(value) value
template<class... Args> void AdapterLog(const char*, Args...) {}
struct FString { const char* Value; const char* operator*() const { return Value; } };
struct UObject {
    bool Alive = true;
    int Serial = 1;
    virtual ~UObject() = default;
};
template<class T> struct TWeakObjectPtr {
    T* Pointer = nullptr;
    int Serial = 0;
    TWeakObjectPtr() = default;
    TWeakObjectPtr(T* p) : Pointer(p), Serial(p ? p->Serial : 0) {}
    T* Get() const { return Pointer && Pointer->Alive && Pointer->Serial == Serial ? Pointer : nullptr; }
    bool IsValid() const { return Get() != nullptr; }
    T* operator->() const { return Get(); }
    void Reset() { Pointer=nullptr; Serial=0; }
    bool operator==(const TWeakObjectPtr& other) const { return Pointer==other.Pointer && Serial==other.Serial; }
    bool operator<(const TWeakObjectPtr& other) const { return std::tie(Pointer,Serial)<std::tie(other.Pointer,other.Serial); }
};
template<class K, class V> struct TMap {
    struct Pair { K Key; V Value; };
    std::map<K, Pair> Values;
    V* Find(const K& key) { auto i=Values.find(key); return i==Values.end() ? nullptr : &i->second.Value; }
    V& FindOrAdd(const K& key) { auto i=Values.emplace(key,Pair{key,V()}); return i.first->second.Value; }
    void Remove(const K& key) { Values.erase(key); }
    void Empty() { Values.clear(); }
    struct Iterator {
        TMap& Owner;
        typename std::map<K,Pair>::iterator It;
        bool Removed=false;
        explicit Iterator(TMap& owner) : Owner(owner), It(owner.Values.begin()) {}
        explicit operator bool() const { return It != Owner.Values.end(); }
        const K& Key() const { return It->first; }
        V& Value() { return It->second.Value; }
        void RemoveCurrent() { It=Owner.Values.erase(It); Removed=true; }
        void operator++() { if (!Removed) ++It; Removed=false; }
    };
    Iterator CreateIterator() { return Iterator(*this); }
    struct RangeIterator {
        typename std::map<K,Pair>::iterator It;
        Pair& operator*() { return It->second; }
        void operator++() { ++It; }
        bool operator!=(const RangeIterator& other) const { return It != other.It; }
    };
    RangeIterator begin() { return {Values.begin()}; }
    RangeIterator end() { return {Values.end()}; }
};
struct FVector {
    float X=0.f,Y=0.f,Z=0.f;
    FVector() = default;
    FVector(float x,float y,float z) : X(x),Y(y),Z(z) {}
    static const FVector ZeroVector;
    bool ContainsNaN() const { return !std::isfinite(X)||!std::isfinite(Y)||!std::isfinite(Z); }
    static float Dist2D(const FVector& a,const FVector& b) { const float x=a.X-b.X,y=a.Y-b.Y;return std::sqrt(x*x+y*y); }
};
const FVector FVector::ZeroVector;
struct FRotator {
    float Pitch=0.f,Yaw=0.f,Roll=0.f;
    FRotator() = default;
    FRotator(float p,float y,float r) : Pitch(p),Yaw(y),Roll(r) {}
    static const FRotator ZeroRotator;
    bool ContainsNaN() const { return !std::isfinite(Pitch)||!std::isfinite(Yaw)||!std::isfinite(Roll); }
};
const FRotator FRotator::ZeroRotator;
struct FMath {
    template<class T> static T Min(T a,T b) { return std::min(a,b); }
    template<class T> static T Max(T a,T b) { return std::max(a,b); }
    template<class T> static T Clamp(T a,T lo,T hi) { return std::max(lo,std::min(a,hi)); }
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Abs(float value) { return std::fabs(value); }
    static float FindDeltaAngleDegrees(float a,float b) { return std::remainder(b-a,360.f); }
};
template<class T> struct TAutoConsoleVariable {
    T Value;
    TAutoConsoleVariable(const char*,T value,const char*,int) : Value(value) {}
    T GetValueOnGameThread() const { return Value; }
};
struct FName { explicit FName(const char*) {} };
struct FCollisionQueryParams { FCollisionQueryParams(FName,bool,UObject*) {} };
struct FHitResult {};
enum { ECC_Visibility=1,ROLE_Authority=3,ROLE_AutonomousProxy=2,USOCK_Open=1,USOCK_Closed=2 };
struct UNetDriver { double Time=100.0; };
class UNetConnection : public UObject { public: UNetDriver* Driver=nullptr; float LastRecvAckTime=100.f; int State=USOCK_Open; };
struct UWorld : UObject {
    float Time=10.f;
    int Traces=0;
    bool Obstructed=false;
    float GetTimeSeconds() const { return Time; }
    bool LineTraceSingleByChannel(FHitResult&,const FVector& from,const FVector& to,int,const FCollisionQueryParams&) {
        ++Traces;
        assert(!from.ContainsNaN() && !to.ContainsNaN());
        return Obstructed;
    }
};
struct FDelegateHandle {
    bool Bound=false;
    bool IsValid() const { return Bound; }
    void Reset() { Bound=false; }
};
struct FWorldDelegates {
    struct CleanupEvent {
        using Callback=void(*)(UWorld*,bool,bool);
        Callback Fn=nullptr;
        int Adds=0,Removes=0;
        FDelegateHandle AddStatic(Callback fn) { Fn=fn;++Adds;return {true}; }
        void Remove(FDelegateHandle) { Fn=nullptr;++Removes; }
        void Broadcast(UWorld* world) { if(Fn) Fn(world,false,false); }
    };
    static CleanupEvent OnWorldCleanup;
};
FWorldDelegates::CleanupEvent FWorldDelegates::OnWorldCleanup;
struct AActor : UObject {
    UWorld* World=nullptr;
    bool PendingKill=false;
    UWorld* GetWorld() const { return World; }
    bool IsPendingKillPending() const { return PendingKill; }
};
struct AController : AActor {};
class APlayerController : public AController { public:
    bool Local=false;
    UNetConnection* Connection=nullptr;
    bool IsLocalController() const { return Local; }
    UNetConnection* GetNetConnection() const { return Connection; }
};
class AUTCharacter;
class AUTWeapon : public AActor { public:
    AUTCharacter* Owner=nullptr;
    AUTCharacter* GetUTOwner() const { return Owner; }
    FString GetName() const { return {"AdapterWeapon"}; }
};
struct UUTCharacterMovement { bool bJustTeleported=false; };
struct FSavedPosition { bool bTeleported=false;float Time=0.f; };
class AUTCharacter : public AActor { public:
    int Role=ROLE_Authority;
    bool Dead=false;
    float Stamp=1.f;
    FVector Eye=FVector(100.f,100.f,100.f);
    FRotator Aim=FRotator::ZeroRotator;
    UUTCharacterMovement* UTCharacterMovement=nullptr;
    AUTWeapon* Weapon=nullptr;
    AController* Controller=nullptr;
    std::vector<FSavedPosition> SavedPositions;
    bool IsDead() const { return Dead; }
    float GetCurrentSynchTime(bool) const { return Stamp; }
    AController* GetController() const { return Controller; }
    AUTWeapon* GetWeapon() const { return Weapon; }
    FVector GetPawnViewLocation() const { return Eye; }
    FRotator GetViewRotation() const { return Aim; }
};
template<class T,class U> T* Cast(U* object) { return dynamic_cast<T*>(object); }
