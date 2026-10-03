#include "NCRocketVolley.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include <map>
using uint8 = uint8_t; using uint32 = uint32_t; using int32 = int32_t;
#define TEXT(x) x
#define UE_LOG(...) ((void)0)
constexpr int ROLE_Authority = 3;
constexpr float KINDA_SMALL_NUMBER = 0.0001f;
constexpr int INDEX_NONE = -1;
struct FVector {};
template<class T> struct TSubclassOf {};
struct FMath {
    template<class T> static T Min(T A,T B){return std::min(A,B);}
    template<class T> static T Max(T A,T B){return std::max(A,B);}
    template<class T> static T Clamp(T A,T L,T H){return Max(L,Min(A,H));}
};
template<class T> struct TArray : std::vector<T> {
    bool IsValidIndex(int32 I) const{return I>=0&&I<Num();}
    int32 Num() const{return static_cast<int32>(this->size());}
    void Add(const T& V){this->push_back(V);} void Empty(){this->clear();}
    T& Last(){return this->back();}
    void RemoveAt(int32 I){this->erase(this->begin()+I);}
    void Remove(const T& V){this->erase(std::remove(this->begin(),this->end(),V),this->end());}
};
template<class T> struct TWeakObjectPtr {
    T* Value=nullptr;
    TWeakObjectPtr()=default; TWeakObjectPtr(T* P):Value(P){}
    TWeakObjectPtr& operator=(T* P){Value=P;return *this;}
    T* Get() const{return Value;} void Reset(){Value=nullptr;}
};
template<class T> struct TGuardValue {T& V;T Old;TGuardValue(T& R,T N):V(R),Old(R){V=N;}~TGuardValue(){V=Old;}};
struct UObject {virtual ~UObject()=default;};
template<class T,class U> T* Cast(U* P){return dynamic_cast<T*>(P);}
struct FRotator { bool Nan=false; bool ContainsNaN()const{return Nan;} void Normalize(){} FRotator GetNormalized()const{return *this;} static const FRotator ZeroRotator;};
const FRotator FRotator::ZeroRotator;
struct Controller : UObject {};
struct AUTProjectile;
struct APlayerController : Controller {bool Confirmed=true;TArray<AUTProjectile*> FakeProjectiles;};
using AUTPlayerController = APlayerController;
namespace NCPlusVersionGate {inline bool IsProtocolConfirmed(APlayerController* P){return P&&P->Confirmed;}}
struct State : UObject {};
struct UUTWeaponStateFiringChargedRocket_Transactional : State {uint32 StateVolleyId=0,StateVolleyEpoch=0;void FireLoadedRocket(){}void EndFiringSequence(uint8){}};
struct AUTPlusWeap_RocketLauncher;
struct AUTCharacter : UObject {
    bool Dead=false,Disabled=false,Pending[2]={false,false};
    ::Controller* Controller=nullptr;
    AUTPlusWeap_RocketLauncher* Weapon=nullptr; AUTPlusWeap_RocketLauncher* PendingWeapon=nullptr;
    bool IsDead() const{return Dead;} bool IsFiringDisabled()const{return Disabled;}
    AUTPlusWeap_RocketLauncher* GetWeapon()const{return Weapon;}
    AUTPlusWeap_RocketLauncher* GetPendingWeapon()const{return PendingWeapon;}
    void SetPendingFire(uint8 M,bool B){Pending[M]=B;}
};
struct AUTGameState {bool Block=false; bool PreventWeaponFire(){return Block;}};
struct FNetworkGUID {uint32 Value;FNetworkGUID(uint32 V):Value(V){}};
struct FNetGUIDCache {
    std::map<uint32,UObject*> Objects;uint32 Next=2;
    FNetworkGUID GetOrAssignNetGUID(UObject* P){for(auto E:Objects)if(E.second==P)return E.first;uint32 Id=Next;Next+=2;Objects[Id]=P;return Id;}
    UObject* GetObjectFromNetGUID(FNetworkGUID Id,bool){auto I=Objects.find(Id.Value);return I==Objects.end()?nullptr:I->second;}
};
struct GuidPointer {FNetGUIDCache Cache;bool IsValid()const{return true;}FNetGUIDCache* operator->(){return &Cache;}};
struct UNetDriver {GuidPointer GuidCache;};
struct World {UNetDriver Driver;UNetDriver* GetNetDriver(){return &Driver;}float Now=10; AUTGameState GS;float GetTimeSeconds()const{return Now;} template<class T>T* GetGameState(){return &GS;}};
struct FTimerHandle {bool Active=false; float Delay=0;};
struct Timers {
    void ClearTimer(FTimerHandle& H){H.Active=false;}
    bool IsTimerActive(const FTimerHandle& H)const{return H.Active;}
    template<class T,class F> void SetTimer(FTimerHandle& H,T*,F,float D,bool){H.Active=true;H.Delay=D;}
};
struct AUTProjectile : UObject {
    bool Dead=false,bExploded=false; int DestroyCalls=0,PairCalls=0; int Class=1;
    AUTCharacter* Instigator=nullptr; AUTProjectile* MasterProjectile=nullptr;AUTProjectile* MyFakeProjectile=nullptr;
    bool IsPendingKillPending()const{return Dead;} int GetClass()const{return Class;}
    void Destroy(){Dead=true;++DestroyCalls;}
    void BeginFakeProjectileSynch(AUTProjectile* Fake){++PairCalls;MyFakeProjectile=Fake;Fake->MasterProjectile=this;}
};
struct AUTPlusProj_Rocket : AUTProjectile {uint32 LoadedOwnershipEpoch=0,LoadedVolleyId=0;uint8 LoadedRocketOrdinal=0;AUTPlusWeap_RocketLauncher* LoadedVolleyWeapon=nullptr;};
struct FPendingFakeProjectile {TWeakObjectPtr<AUTProjectile> Projectile;};
struct FNCLoadedRocketPrediction {
    uint32 OwnershipEpoch=0,ProjectileNetGUID=0,VolleyId=0;uint8 Ordinal=0,Outcome=255;float CreatedAt=0;
    TWeakObjectPtr<AUTProjectile> Fake,Real;
};
struct FNCLoadedVolleyReceipt {uint32 VolleyId=0;uint8 Result=0,Count=0,SpawnedMask=0;};
struct RocketMode {bool ProjClass=true;};
struct CVar {int GetValueOnGameThread()const{return 0;}} CVarRocketVolleyDebug;
struct FakeBase {void GivenTo(AUTCharacter*,bool);void Removed();AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile>,FVector,FRotator){return nullptr;}};
struct AUTPlusWeap_RocketLauncher : FakeBase {
    using Super = FakeBase;
    World W;Timers TM;int Role=ROLE_Authority; AUTCharacter* UTOwner=nullptr;
    State Idle,Inactive,Unequip; UUTWeaponStateFiringChargedRocket_Transactional Charge;
    State* ActiveState=&Idle;State* InactiveState=&Inactive;State* UnequippingState=&Unequip;State* CurrentState=&Idle;
    TArray<State*> FiringState{ };TArray<RocketMode> RocketFireModes;
    bool bDisableAltLoading=false,bAllowGrenades=true,bAllowAltModes=true,SpiralRocketClass=true;
    bool bDrawRocketModeString=false;
    uint8 CurrentFireMode=1;int32 CurrentRocketFireMode=0,NumLoadedRockets=0,NumLoadedBarrels=0,Ammo=9;
    uint8 CurrentlyFiringMode=255;TArray<uint8> FireModeActiveState;
    int BeginCalls=0,EndCalls=0,GotoCalls=0,Refund=0;float EarliestFireTime=0,TestRemaining=0;
    NCRocketVolley::FProgress LoadedVolley;
    uint32 LoadedOwnershipEpoch=1,LoadedVolleyEpoch=0,LastClientLoadedVolleyId=0,LastServerLoadedVolleyId=0;
    uint8 LoadedVolleyRequestedCount=0,LoadedVolleySelectedMode=0,LoadedVolleyNextOrdinal=0;
    int32 LoadedVolleyAmmoSpent=0;bool bLoadedVolleyEnteredState=false;float LoadedVolleyBeginRequestedAt=0;
    bool bLoadedVolleyReleaseSent=false,bLoadedVolleyReleaseReceived=false,bLoadedVolleyApplyingResult=false;
    bool bLoadedVolleySpawnInProgress=false,bLoadedVolleySpawnSucceeded=false;
    TWeakObjectPtr<AUTProjectile> LoadedVolleySpawnedProjectile;
    TArray<FPendingFakeProjectile> PendingFakeProjectiles;
    AUTProjectile* TestNextProjectile=nullptr;bool TestImmediateImpact=false,TestAllowDelay=true;int TestSpawnCalls=0;
    TWeakObjectPtr<AUTCharacter> LoadedVolleyPawn;
    TArray<FNCLoadedRocketPrediction> LoadedRocketPredictions;TArray<FNCLoadedVolleyReceipt> LoadedVolleyReceipts;
    TArray<FNCLoadedVolleyReceipt> SentVolleys;TArray<uint8> SentRockets,SentRocketResults;
    FTimerHandle LoadedRocketReconcileHandle,LoadedVolleyBeginHandle;
    AUTPlusWeap_RocketLauncher(){FiringState.Add(&Idle);FiringState.Add(&Charge);RocketFireModes.Add({});RocketFireModes.Add({});RocketFireModes.Add({});}
    World* GetWorld(){return &W;}Timers& GetWorldTimerManager(){return TM;}
    bool HasAmmo(uint8)const{return Ammo>0;}bool IsFiring()const{return CurrentState==&Charge;}
    bool Is329FireProtocolReady()const{APlayerController* PC=UTOwner?Cast<APlayerController>(UTOwner->Controller):nullptr;return PC&&PC->Confirmed;}
    void ForceNetUpdate(){}
    void ClearDeferredActiveState(){}
    bool BeginFiringSequence(uint8,bool){++BeginCalls;CurrentState=&Charge;bLoadedVolleyEnteredState=true;UTOwner->SetPendingFire(1,true);return true;}
    void EndFiringSequence(uint8 M){++EndCalls;UTOwner->SetPendingFire(M,false);}
    void GotoActiveState(){++GotoCalls;CurrentState=ActiveState;}
    void AddAmmo(int32 A){Ammo+=A;Refund+=A;}
    void SetRocketFlashExtra(uint8,int32,uint8,bool){}
    bool HasLoadedVolley()const{return LoadedVolley.Id!=0&&!LoadedVolley.Terminal;}
    void ClientLoadedVolleyResult(uint32,uint32 Id,AUTCharacter*,uint8 R,uint8 C,uint8 M){FNCLoadedVolleyReceipt X;X.VolleyId=Id;X.Result=R;X.Count=C;X.SpawnedMask=M;SentVolleys.Add(X);}
    void ClientLoadedRocketResult(uint32,uint32,AUTCharacter*,uint8 Ordinal,uint8 R,AUTProjectile*,uint32){SentRockets.Add(Ordinal);SentRocketResults.Add(R);}
    AUTProjectile* SpawnNetPredictedProjectileInternal(TSubclassOf<AUTProjectile>,FVector,FRotator,uint8,int32,bool AllowDelay){
        ++TestSpawnCalls;TestAllowDelay=AllowDelay;
        CaptureLoadedRocketSpawn(TestNextProjectile);
        if(Role<ROLE_Authority&&TestNextProjectile){FPendingFakeProjectile P;P.Projectile=TestNextProjectile;PendingFakeProjectiles.Add(P);Cast<APlayerController>(UTOwner->Controller)->FakeProjectiles.Add(TestNextProjectile);}
        if(TestImmediateImpact&&TestNextProjectile)TestNextProjectile->bExploded=true;
        return TestImmediateImpact?nullptr:TestNextProjectile;
    }
    bool CanBeginLoadedVolley();bool IsLoadedVolleyModeValid(uint8 Mode)const;void ResetLoadedVolley(uint32 Id);
    void TryBeginLoadedVolley();void ServerBeginLoadedVolley_Implementation(uint32,uint32,AUTCharacter*);
    void ServerReleaseLoadedVolley_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8);
    void ServerSetLoadedRocketMode_Implementation(uint32,uint32,AUTCharacter*,uint8);
    bool CommitLoadedVolley();void SendLoadedVolleyReceipt(NCRocketVolley::EResult);void CompleteLoadedVolley(bool);
    FNCLoadedRocketPrediction& FindOrAddLoadedRocket(uint32,uint32,uint8);
    void ClientLoadedRocketResult_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8,AUTProjectile*,uint32);
    void ClientLoadedVolleyResult_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8,uint8);
    void ReconcileLoadedRockets();
    bool ObserveLoadedRocketActor(uint32,uint32,uint8,AUTProjectile*);
    void ResetLoadedOwnershipState();void GivenTo(AUTCharacter*,bool);void Removed();void OnRep_LoadedOwnershipEpoch();
    void CaptureLoadedRocketSpawn(AUTProjectile*);
    AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile>,FVector,FRotator);
};
namespace NCClientFireTiming {inline float MaxRemaining(AUTPlusWeap_RocketLauncher* W){return W->TestRemaining;}}

inline void FakeBase::GivenTo(AUTCharacter* Pawn,bool){static_cast<AUTPlusWeap_RocketLauncher*>(this)->UTOwner=Pawn;}
inline void FakeBase::Removed(){static_cast<AUTPlusWeap_RocketLauncher*>(this)->UTOwner=nullptr;}
