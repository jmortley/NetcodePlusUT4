#include "NCRocketVolley.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <functional>
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
    template<class T> static bool IsFinite(T V){return std::isfinite(V);}
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
using AController = Controller;
struct AUTProjectile;
struct APlayerController : Controller {bool Confirmed=true;TArray<AUTProjectile*> FakeProjectiles;};
using AUTPlayerController = APlayerController;
namespace NCPlusVersionGate {inline bool IsProtocolConfirmed(APlayerController* P){return P&&P->Confirmed;}}
struct AUTPlusWeap_RocketLauncher;
using AUTWeapon = AUTPlusWeap_RocketLauncher;
using AUTWeaponFix = AUTPlusWeap_RocketLauncher;
struct AUTCharacter;
struct World;
struct FTimerHandle {bool Active=false; float Delay=0;};
struct State : UObject {
    AUTPlusWeap_RocketLauncher* Outer=nullptr;
    virtual void BeginState(const State*);
    virtual void EndState(){}
};
using UUTWeaponState = State;
struct UUTWeaponStateFiringChargedRocket_Transactional : State {
    uint32 StateVolleyId=0,StateVolleyEpoch=0;
    AUTPlusWeap_RocketLauncher* RocketLauncher=nullptr;
    bool bCharging=false,bReleaseRequested=false,bReleaseCommitted=false;
    bool bCompletingLoadTimer=false,bDuplicateReleaseLogged=false;
    float ChargeTime=0;
    FTimerHandle RefireCheckHandle,LoadTimerHandle,GraceTimerHandle,FireLoadedRocketHandle,PutDownHandle;
    AUTWeapon* GetOuterAUTWeapon(){return Outer;}
    AUTCharacter* GetUTOwner();World* GetWorld();uint8 GetFireMode();
    void BeginState(const UUTWeaponState*) override;void EndState() override;void ClearAllTimers();
    void ToggleLoopingEffects(bool){}void LoadTimer(){}void FireLoadedRocket(){}void EndFiringSequence(uint8){}
};
struct AUTCharacter : UObject {
    bool Dead=false,Disabled=false,Local=true,Pending[2]={false,false};
    ::Controller* Controller=nullptr;
    AUTPlusWeap_RocketLauncher* Weapon=nullptr; AUTPlusWeap_RocketLauncher* PendingWeapon=nullptr;
    bool IsDead() const{return Dead;} bool IsFiringDisabled()const{return Disabled;}
    bool IsLocallyControlled()const{return Local;}
    AUTPlusWeap_RocketLauncher* GetWeapon()const{return Weapon;}
    AUTPlusWeap_RocketLauncher* GetPendingWeapon()const{return PendingWeapon;}
    void SetPendingFire(uint8 M,bool B){Pending[M]=B;}
    bool IsPendingFire(uint8 M)const{return Pending[M];}void ClearFiringInfo(){}
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
struct FPlatformTime {static double Now;static double Seconds(){return Now;}};
double FPlatformTime::Now = 1000;
struct Timers {
    int ClearCalls=0;
    void ClearTimer(FTimerHandle& H){++ClearCalls;H.Active=false;}
    template<class T> void ClearAllTimersForObject(T*){}
    bool IsTimerActive(const FTimerHandle& H)const{return H.Active;}
    template<class T,class F> void SetTimer(FTimerHandle& H,T*,F,float D,bool){H.Active=true;H.Delay=D;}
};
struct World {UNetDriver Driver;UNetDriver* GetNetDriver(){return &Driver;}float Now=10; AUTGameState GS;float GetTimeSeconds()const{return Now;} template<class T>T* GetGameState(){return &GS;}Timers TimerManager;Timers& GetTimerManager(){return TimerManager;}};
using UWorld = World;
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
struct FTestLoadedInputRPC {uint32 Epoch=0,Id=0;AUTCharacter* Pawn=nullptr;uint8 Mode=0,Count=0;};
struct RocketMode {bool ProjClass=true;};
struct CVar {int GetValueOnGameThread()const{return 0;}} CVarRocketVolleyDebug;
namespace EEndPlayReason {enum Type {Destroyed,LevelTransition};}
struct FakeBase {
    void GivenTo(AUTCharacter*,bool);void Removed();void StartFire(uint8);void StopFire(uint8);
    bool PutDown();void DetachFromOwner_Implementation();void ClientGivenTo_Internal(bool);void Destroyed();void StateChanged(){}
    void EndPlay(EEndPlayReason::Type);
    AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile>,FVector,FRotator){return nullptr;}
};
struct AUTPlusWeap_RocketLauncher : FakeBase {
    using Super = FakeBase;
    World W;World* TestWorld=&W;int Role=ROLE_Authority; AUTCharacter* UTOwner=nullptr;
    State Idle,Inactive,Unequip,Equip; UUTWeaponStateFiringChargedRocket_Transactional Charge;
    State* ActiveState=&Idle;State* InactiveState=&Inactive;State* UnequippingState=&Unequip;State* CurrentState=&Idle;
    State* EquippingState=&Equip;
    TArray<State*> FiringState{ };TArray<RocketMode> RocketFireModes;
    bool bDisableAltLoading=false,bAllowGrenades=true,bAllowAltModes=true,SpiralRocketClass=true;
    bool bDrawRocketModeString=false,bHandlingRetry=false;
    bool bFireHeldByPlayer[2]={false,false}; // AUTWeaponFix's GhostFix held flags.
    uint8 CurrentFireMode=1;int32 CurrentRocketFireMode=0,NumLoadedRockets=0,NumLoadedBarrels=0,Ammo=9;
    uint8 CurrentlyFiringMode=255;TArray<uint8> FireModeActiveState;
    int BeginCalls=0,EndCalls=0,GotoCalls=0,Refund=0;float EarliestFireTime=0,TestRemaining=0;
    NCRocketVolley::FProgress LoadedVolley;
    uint32 LoadedOwnershipEpoch=1,LoadedVolleyEpoch=0,LastClientLoadedVolleyId=0,LastServerLoadedVolleyId=0;
    bool bPendingLoadedVolleyInput=false,bPendingLoadedVolleyRelease=false;
    double PendingLoadedVolleyInputAt=0;
    TWeakObjectPtr<AUTCharacter> PendingLoadedVolleyPawn;
    TWeakObjectPtr<UWorld> PendingLoadedVolleyWorld;
    TWeakObjectPtr<AController> PendingLoadedVolleyController;
    FTimerHandle PendingLoadedVolleyInputHandle;
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
    TArray<FTestLoadedInputRPC> SentBegins,SentReleases;TArray<uint8> InputEvents;
    int StockStartCalls=0,StockStopCalls=0;
    bool TestPutDownResult=true,BaseSawPendingInput=false,TestDestroyed=false;
    std::function<void()> TestOnBegin;
    // Opt in to stock-style synchronous state callbacks for switch regressions.
    // Other lifecycle tests keep their existing charge/animation boundary stubs.
    bool TestUseStateCallbacks=false;
    int TestStateDepth=0,TestMaxStateDepth=0,TestChargedEntries=0,TestLoadStarts=0;
    TArray<bool> TestPendingAtGotoActive;
    TArray<float> LastFireTime{ };
    std::function<void()> TestOnActive;
    EEndPlayReason::Type TestEndPlayReason=EEndPlayReason::Destroyed;
    int LifecycleCalls=0;
    FTimerHandle UpdateLockHandle;float LockCheckTime=0.1f;
    FTimerHandle LoadedRocketReconcileHandle,LoadedVolleyBeginHandle;
    AUTPlusWeap_RocketLauncher(){FiringState.Add(&Idle);FiringState.Add(&Charge);RocketFireModes.Add({});RocketFireModes.Add({});RocketFireModes.Add({});Idle.Outer=Charge.Outer=this;LastFireTime.Add(0);LastFireTime.Add(0);}
    World* GetWorld(){return TestWorld;}Timers& GetWorldTimerManager(){return GetWorld()->GetTimerManager();}
    bool HasAmmo(uint8)const{return Ammo>0;}bool IsFiring()const{return CurrentState==&Charge;}
    bool Is329FireProtocolReady()const{APlayerController* PC=UTOwner?Cast<APlayerController>(UTOwner->Controller):nullptr;return PC&&PC->Confirmed;}
    void ForceNetUpdate(){}
    bool IsPendingKillPending()const{return TestDestroyed;}
    void UpdateLock(){}void ClearLoadedRockets(){}
    void ClearDeferredActiveState(){}
    bool BeginFiringSequence(uint8 M,bool){++BeginCalls;InputEvents.Add(2);CurrentFireMode=M;UTOwner->SetPendingFire(M,true);if(TestUseStateCallbacks)GotoState(&Charge);else{CurrentState=&Charge;bLoadedVolleyEnteredState=true;}if(TestOnBegin)TestOnBegin();return true;}
    void EndFiringSequence(uint8 M){++EndCalls;InputEvents.Add(4);if(UTOwner)UTOwner->SetPendingFire(M,false);}
    void GotoActiveState(){++GotoCalls;TestPendingAtGotoActive.Add(UTOwner&&UTOwner->IsPendingFire(1));if(TestUseStateCallbacks)GotoState(ActiveState);else CurrentState=ActiveState;}
    void GotoState(State* NewState);
    State* GetCurrentState(){return CurrentState;}uint8 GetCurrentFireMode(){return CurrentFireMode;}
    AUTCharacter* GetUTOwner(){return UTOwner;}
    void OnStartedFiring(){}void DeactivateSpawnProtection(){}void OnStoppedFiring(){}void StopFiringEffects(){}
    void BeginLoadRocket(){++TestLoadStarts;}float GetLoadTime(int32)const{return 0.4f;}
    uint32 GetLoadedVolleyId()const{return LoadedVolley.Id;}uint32 GetLoadedVolleyEpoch()const{return LoadedVolleyEpoch;}
    bool IsLoadedVolleyReleasePending()const{return bLoadedVolleyReleaseSent||bLoadedVolleyReleaseReceived;}
    void AddAmmo(int32 A){Ammo+=A;Refund+=A;}
    void SetRocketFlashExtra(uint8,int32,int32,bool){}
    bool HasLoadedVolley()const{return LoadedVolley.Id!=0&&!LoadedVolley.Terminal;}
    void ClientLoadedVolleyResult(uint32,uint32 Id,AUTCharacter*,uint8 R,uint8 C,uint8 M){FNCLoadedVolleyReceipt X;X.VolleyId=Id;X.Result=R;X.Count=C;X.SpawnedMask=M;SentVolleys.Add(X);}
    void ClientLoadedRocketResult(uint32,uint32,AUTCharacter*,uint8 Ordinal,uint8 R,AUTProjectile*,uint32){SentRockets.Add(Ordinal);SentRocketResults.Add(R);}
    void ServerBeginLoadedVolley(uint32 Epoch,uint32 Id,AUTCharacter* Pawn){SentBegins.Add({Epoch,Id,Pawn,0,0});InputEvents.Add(1);}
    void ServerReleaseLoadedVolley(uint32 Epoch,uint32 Id,AUTCharacter* Pawn,uint8 Mode,uint8 Count){SentReleases.Add({Epoch,Id,Pawn,Mode,Count});InputEvents.Add(3);}
    AUTProjectile* SpawnNetPredictedProjectileInternal(TSubclassOf<AUTProjectile>,FVector,FRotator,uint8,int32,bool AllowDelay){
        ++TestSpawnCalls;TestAllowDelay=AllowDelay;
        CaptureLoadedRocketSpawn(TestNextProjectile);
        if(Role<ROLE_Authority&&TestNextProjectile){FPendingFakeProjectile P;P.Projectile=TestNextProjectile;PendingFakeProjectiles.Add(P);Cast<APlayerController>(UTOwner->Controller)->FakeProjectiles.Add(TestNextProjectile);}
        if(TestImmediateImpact&&TestNextProjectile)TestNextProjectile->bExploded=true;
        return TestImmediateImpact?nullptr:TestNextProjectile;
    }
    bool CanBeginLoadedVolley();bool IsLoadedVolleyModeValid(uint8 Mode)const;void ResetLoadedVolley(uint32 Id);
    void StartFire(uint8);void StopFire(uint8);void NotifyLoadedVolleyRelease();
    bool CanBeginLoadedVolleyInput();void BufferLoadedVolleyInput();void TryDrainLoadedVolleyInput();void ClearLoadedVolleyInput();
    void TryBeginLoadedVolley();bool BeginLoadedVolleyState();void ContinueLoadedVolley();void ServerBeginLoadedVolley_Implementation(uint32,uint32,AUTCharacter*);
    void ServerReleaseLoadedVolley_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8);
    void ServerSetLoadedRocketMode_Implementation(uint32,uint32,AUTCharacter*,uint8);
    bool CommitLoadedVolley();void SendLoadedVolleyReceipt(NCRocketVolley::EResult);void CompleteLoadedVolley(bool);
    FNCLoadedRocketPrediction& FindOrAddLoadedRocket(uint32,uint32,uint8);
    void ClientLoadedRocketResult_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8,AUTProjectile*,uint32);
    void ClientLoadedVolleyResult_Implementation(uint32,uint32,AUTCharacter*,uint8,uint8,uint8);
    void ReconcileLoadedRockets();
    bool ObserveLoadedRocketActor(uint32,uint32,uint8,AUTProjectile*);
    void ResetLoadedOwnershipState(bool bPreservePendingInput=false);void GivenTo(AUTCharacter*,bool);void Removed();void OnRep_LoadedOwnershipEpoch();
    bool PutDown();void DetachFromOwner_Implementation();void ClientGivenTo_Internal(bool);void Destroyed();void StateChanged();
    void EndPlay(EEndPlayReason::Type);
    void CaptureLoadedRocketSpawn(AUTProjectile*);
    AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile>,FVector,FRotator);
};
namespace NCClientFireTiming {inline float MaxRemaining(AUTPlusWeap_RocketLauncher* W){return W->TestRemaining;}inline void Record(AUTWeaponFix*,uint8){}}
namespace NCFireDiagnostics {template<class... T>void Record(T...){}inline void ChargeEnded(AUTWeapon*,uint8){}}
inline bool RocketPrimaryChargedDiag(AUTWeapon*){return false;}
inline AUTCharacter* UUTWeaponStateFiringChargedRocket_Transactional::GetUTOwner(){return Outer->UTOwner;}
inline World* UUTWeaponStateFiringChargedRocket_Transactional::GetWorld(){return Outer->GetWorld();}
inline uint8 UUTWeaponStateFiringChargedRocket_Transactional::GetFireMode(){return Outer->GetCurrentFireMode();}

// Stock AUTWeapon::GotoState callback ordering, including nested End/Begin and
// the CurrentState guard. Assets, rendering and engine timers remain adapters.
inline void AUTPlusWeap_RocketLauncher::GotoState(State* NewState)
{
    assert(NewState && UTOwner);
    assert(++TestStateDepth <= 8); // A recovery loop fails before overflowing.
    TestMaxStateDepth=std::max(TestMaxStateDepth,TestStateDepth);
    if(CurrentState!=NewState)
    {
        State* PrevState=CurrentState;
        if(CurrentState)CurrentState->EndState();
        if(CurrentState==PrevState)
        {
            CurrentState=NewState;
            if(NewState==&Charge)++TestChargedEntries;
            CurrentState->BeginState(PrevState);
            StateChanged();
        }
    }
    --TestStateDepth;
}

// Stock ActiveState scans pending bits and goes DIRECTLY to FiringState; it
// does not call the launcher's StartFire override. Preserve that failure trigger.
inline void State::BeginState(const State*)
{
    if(!Outer || this!=Outer->ActiveState)return;
    if(Outer->TestOnActive)Outer->TestOnActive();
    if(!Outer->UTOwner->GetPendingWeapon() || !Outer->PutDown())
        for(uint8 M=0;M<Outer->FiringState.Num();++M)
            if(Outer->UTOwner->IsPendingFire(M)&&Outer->HasAmmo(M))
            {
                Outer->CurrentFireMode=M;
                Outer->GotoState(Outer->FiringState[M]);
                return;
            }
}

inline void FakeBase::GivenTo(AUTCharacter* Pawn,bool){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;W->UTOwner=Pawn;}
inline void FakeBase::Removed(){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;W->UTOwner=nullptr;}
inline void FakeBase::StartFire(uint8){++static_cast<AUTPlusWeap_RocketLauncher*>(this)->StockStartCalls;}
inline void FakeBase::StopFire(uint8){++static_cast<AUTPlusWeap_RocketLauncher*>(this)->StockStopCalls;}
inline bool FakeBase::PutDown(){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;return W->TestPutDownResult;}
inline void FakeBase::DetachFromOwner_Implementation(){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;W->UTOwner=nullptr;}
inline void FakeBase::ClientGivenTo_Internal(bool){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;}
inline void FakeBase::Destroyed(){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;W->TestDestroyed=true;}
inline void FakeBase::EndPlay(EEndPlayReason::Type Reason){auto* W=static_cast<AUTPlusWeap_RocketLauncher*>(this);W->BaseSawPendingInput=W->bPendingLoadedVolleyInput;++W->LifecycleCalls;W->TestEndPlayReason=Reason;}
