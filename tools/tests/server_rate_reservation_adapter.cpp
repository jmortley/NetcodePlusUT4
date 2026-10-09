
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using uint8=uint8_t; using uint32=uint32_t; using int32=int32_t; using int64=int64_t;
constexpr int INDEX_NONE=-1; constexpr float SMALL_NUMBER=1.e-8f;
#define TEXT(x) x
#define UE_LOG(...)
struct FString {std::string S;FString(const char*s):S(s){}FString&operator=(const std::string&s){S=s;return*this;}const char*operator*()const{return S.c_str();} template<class...T> static std::string Printf(const char*s,T...){return s;} };
namespace FPlatformTime{inline double Seconds(){return 0;}}
namespace FMath { template<class T>T Min(T a,T b){return std::min(a,b);} template<class T>T Max(T a,T b){return std::max(a,b);} template<class T>T Clamp(T x,T a,T b){return Max(a,Min(x,b));} inline bool IsFinite(float x){return std::isfinite(x);} inline float Abs(float x){return std::abs(x);} }
struct Config{float Value=100;float GetValueOnGameThread(){return Value;}};
static Config CVarServerRateQueueMs;
template<class T> struct TWeakObjectPtr { T*p=nullptr; TWeakObjectPtr(){} TWeakObjectPtr(T*x):p(x){} T*Get()const{return p;} bool operator<(const TWeakObjectPtr&o)const{return p<o.p;} };
template<class K,class V>struct TMap:std::map<K,V> { V*Find(const K&k){auto i=this->find(k);return i==this->end()?nullptr:&i->second;} const V*Find(const K&k)const{auto i=this->find(k);return i==this->end()?nullptr:&i->second;} void Add(const K&k,const V&v){(*this)[k]=v;} void Remove(const K&k){this->erase(k);} bool Contains(const K&k)const{return this->count(k)!=0;} };
template<class T>struct TArray:std::vector<T>{bool IsValidIndex(int n)const{return n>=0&&n<int(this->size());} int Num()const{return int(this->size());} void SetNumZeroed(int n){this->resize(n);} };
struct FVector{float X=0,Y=0,Z=0; bool ContainsNaN()const{return !std::isfinite(X)||!std::isfinite(Y)||!std::isfinite(Z);} };
struct AUTWeaponFix; struct AUTGameState{bool Prevent=false; bool PreventWeaponFire(){return Prevent;}};
struct UUTWeaponState{virtual~UUTWeaponState(){} AUTWeaponFix*W=nullptr;};
struct UUTWeaponStateFiring_Transactional:UUTWeaponState{void TransactionalFire();};
template<class T,class U>T*Cast(U*x){return dynamic_cast<T*>(x);}
struct FSavedPosition{float Time=0;bool bTeleported=false;};
struct AController{virtual~AController(){}};struct APlayerState{std::string PlayerName="test";std::string GetPlayerName()const{return PlayerName;}};using AUTPlayerState=APlayerState;struct AUTPlayerController:AController{APlayerState*PlayerState=nullptr;template<class T>T*GetPlayerState(){return PlayerState;}};
struct AUTCharacter{AController*Controller=nullptr;bool Dead=false,Kill=false,Disabled=false; AUTWeaponFix*Weapon=nullptr,*PendingWeapon=nullptr; bool Pending[2]={false,false}; std::vector<FSavedPosition>SavedPositions; bool IsDead(){return Dead;} bool IsPendingKillPending(){return Kill;} AUTWeaponFix*GetWeapon(){return Weapon;} AUTWeaponFix*GetPendingWeapon(){return PendingWeapon;} bool IsFiringDisabled(){return Disabled;} void SetPendingFire(uint8 m,bool b){Pending[m]=b;} bool IsPendingFire(uint8 m){return Pending[m];}};
struct FTimerHandle{int ID=0;};
struct FTimerDelegate{std::function<void()>Fn;template<class W,class F,class...A>void BindUObject(W*w,F f,A...a){Fn=[=]{(w->*f)(a...);};}};
struct Timers{struct Entry{int ID;float Due;std::function<void()>Fn;};float*Now; int Next=1; std::vector<Entry>E;void SetTimer(FTimerHandle&h,FTimerDelegate d,float delay,bool){ClearTimer(h);h.ID=Next++;E.push_back({h.ID,*Now+delay,d.Fn});}void SetTimerForNextTick(FTimerDelegate d){FTimerHandle h;SetTimer(h,d,.001f,false);}void ClearTimer(FTimerHandle&h){E.erase(std::remove_if(E.begin(),E.end(),[&](const Entry&e){return e.ID==h.ID;}),E.end());h.ID=0;}void Drain(){for(int n=0;n<100;++n){auto i=std::find_if(E.begin(),E.end(),[&](const Entry&e){return e.Due<=*Now;});if(i==E.end())return;auto f=i->Fn;E.erase(i);f();}throw std::runtime_error("timer loop");}};
struct UWorld{float Now=10.f;Timers Timer{&Now};AUTGameState GS;float GetTimeSeconds(){return Now;}template<class T>T*GetGameState(){return &GS;}};
struct FDeferredEquipFireContext{bool bShotDispatched=false,bReleaseSeen=false,bDeferredRate=true,bDeferredEquip=false,bDeferredState=false,bDispatchReady=false;uint8 FireMode=0;int32 FireEventIndex=1;uint32 Generation=1;float ServerAcceptTime=10,RateDueTime=10.05f,RateMaxAge=.25f,RatePredictionTime=.02f,RateObservedRTTMs=40;FVector RateFireOrigin;FTimerHandle RateTimer;TWeakObjectPtr<AUTCharacter>Owner;TWeakObjectPtr<UUTWeaponState>ExpectedFiringState;};
static TMap<TWeakObjectPtr<AUTWeaponFix>,FDeferredEquipFireContext>DeferredEquipFireContexts,FollowingRateFireContexts;
static TMap<TWeakObjectPtr<AUTWeaponFix>,uint32>DeferredEquipFireGenerations;
static TMap<TWeakObjectPtr<AUTWeaponFix>,TArray<float>>ServerActualFireTimes;
namespace NCFireDiagnostics{inline bool Enabled(){return false;}template<class...T>void Record(T...){}struct FRequestScope{template<class...T>FRequestScope(T...){}};}
static bool FireProvenance(){return false;}static bool RocketPrimaryDiagFor(AUTWeaponFix*,uint8){return false;}
struct AUTWeaponFix{
 UWorld World;AUTCharacter Pawn;AUTCharacter*UTOwner=&Pawn;bool Remote=true,Kill=false,Ammo[2]={true,true},Allow[2]={true,true};
 UUTWeaponState Active;UUTWeaponStateFiring_Transactional Mode[2];UUTWeaponState*ActiveState=&Active,*CurrentState=&Active;
 TArray<UUTWeaponState*>FiringState;TArray<float>LastFireTime;TArray<int>FireModeActiveState,AuthoritativeFireEventIndex,LastProcessedStopEventIndex;TArray<void*>ProjClass;
 float Refire[2]={1.f,.1f};float MaxRewindMs=250,ProjectilePredictionCapMs=250,BasePrediction=.02f;uint8 CurrentFireMode=0,CurrentlyFiringMode=255;FTimerHandle DeferredActiveStateHandle;
 std::vector<int>Shots,Acks;std::vector<std::string>Cancels;int Cleanups=0;
 AUTWeaponFix(){Pawn.Weapon=this;for(int i=0;i<2;++i){Mode[i].W=this;FiringState.push_back(&Mode[i]);LastFireTime.push_back(i?9.9f:9.05f);FireModeActiveState.push_back(0);AuthoritativeFireEventIndex.push_back(0);LastProcessedStopEventIndex.push_back(0);ProjClass.push_back(nullptr);}}
 ~AUTWeaponFix(){auto k=TWeakObjectPtr<AUTWeaponFix>(this);DeferredEquipFireContexts.Remove(k);FollowingRateFireContexts.Remove(k);DeferredEquipFireGenerations.Remove(k);ServerActualFireTimes.Remove(k);}
 UWorld*GetWorld(){return &World;}Timers&GetWorldTimerManager(){return World.Timer;}bool RequiresTransactionalRequest(){return Remote;}bool IsPendingKillPending(){return Kill;}bool HasAmmo(uint8 m){return Ammo[m];}bool AllowServerFireMode(uint8 m){return Allow[m];}float GetRefireTime(uint8 m){return Refire[m];}float GetHitValidationPredictionTime(){return BasePrediction;}static bool GetServerObservedRTTMs(AUTPlayerController*,float&out){out=40;return true;}
 void ClearDeferredEquipFireContext(bool invalidate=true,const char*reason="clear") {auto k=TWeakObjectPtr<AUTWeaponFix>(this);auto*c=DeferredEquipFireContexts.Find(k);if(c){if(!c->bShotDispatched)Cancels.push_back(reason);World.Timer.ClearTimer(c->RateTimer);}DeferredEquipFireContexts.Remove(k);if(invalidate){DeferredEquipFireGenerations.Remove(k);FollowingRateFireContexts.Remove(k);}}
 void ClientConfirmFireEvent(uint8,int e){Acks.push_back(e);}
 void GotoState(UUTWeaponState*s){CurrentState=s;CommitShot();}
 void CommitShot(){auto k=TWeakObjectPtr<AUTWeaponFix>(this);auto*c=DeferredEquipFireContexts.Find(k);if(!c||!c->bDispatchReady||c->bShotDispatched||CurrentState!=c->ExpectedFiringState.Get())throw std::runtime_error("unauthorized dispatch");c->bShotDispatched=true;Shots.push_back(c->FireEventIndex);auto&t=ServerActualFireTimes[k];t.resize(2);t[c->FireMode]=World.Now;LastFireTime[c->FireMode]=World.Now;}
 void ReconcileDeferredEquipRelease(uint8 m,int,uint32 g,TWeakObjectPtr<AUTCharacter>o){auto*live=DeferredEquipFireGenerations.Find(TWeakObjectPtr<AUTWeaponFix>(this));if(live&&*live==g&&UTOwner==o.Get()&&CurrentState==FiringState[m]){Pawn.Pending[m]=false;++Cleanups;}}
 void ClearDeferredActiveState(){World.Timer.ClearTimer(DeferredActiveStateHandle);}
 void CompleteAcceptedRateFire(uint8,uint32);void PromoteFollowingRateFire();void ApplyStop(uint8,int32);bool CanReserveServerRateFire(uint8,float);bool IsFireEventSequenceValid(uint8,int32);bool ValidateFireRequest(uint8,int32,float,float*,float);
 void Reserve(int event=1,int mode=0,bool following=false){auto k=TWeakObjectPtr<AUTWeaponFix>(this);FDeferredEquipFireContext c;c.Owner=&Pawn;c.ExpectedFiringState=FiringState[mode];c.FireMode=uint8(mode);c.FireEventIndex=event;c.Generation=uint32(event+10);c.ServerAcceptTime=World.Now;c.RateDueTime=World.Now+.05f;if(following)FollowingRateFireContexts.Add(k,c);else{DeferredEquipFireContexts.Add(k,c);DeferredEquipFireGenerations.Add(k,c.Generation);}AuthoritativeFireEventIndex[mode]=event;}
};
void UUTWeaponStateFiring_Transactional::TransactionalFire(){if(W->Pawn.Pending[W->CurrentFireMode])W->CommitShot();}
static void Check(bool b,const char*m){if(!b)throw std::runtime_error(m);}

// NATIVE_METHODS

int main(){try{
 Check(ServerRateReservationDelay(10,9.05f,9.05f,1,100)>.049f,"ordinary compression");
 Check(ServerRateReservationDelay(10,9.05f,9.2f,1,100)==0,"actual dispatch bound");
 Check(ServerRateReservationDelay(10,9.952f,9.952f,.1f,100)==0,"half-refire bound");
 Check(ServerRateReservationDelay(10,9.05f,9.05f,1,0)==0,"rollback");
 Check(ServerRateReservationDelay(10,9.2f,9.2f,1,1000)==0,"hard cap");
 {AUTWeaponFix w;float d=0;Check(w.ValidateFireRequest(0,1,10,&d,0)&&d>.049f&&w.AuthoritativeFireEventIndex[0]==1,"actual admission advances once");Check(!w.ValidateFireRequest(0,1,10,&d,0),"duplicate admission");}
 {AUTWeaponFix w;w.BasePrediction=.12f;float d=0;Check(!w.ValidateFireRequest(0,1,10,&d,0)&&w.AuthoritativeFireEventIndex[0]==0,"high RTT remains retryable");w.World.Now=10.07f;Check(w.ValidateFireRequest(0,1,10,&d,0)&&d==0,"rejected high RTT retry recovers");}
 {AUTWeaponFix w;w.BasePrediction=.075f;float d=0;Check(!w.ValidateFireRequest(0,1,10,&d,0)&&w.AuthoritativeFireEventIndex[0]==0,"exact history budget rejects before consume");}
 {AUTWeaponFix w;w.Ammo[1]=false;float d=0;Check(!w.ValidateFireRequest(1,1,10,&d,.05f)&&w.AuthoritativeFireEventIndex[1]==0,"following admission policy before consume");}
 {AUTWeaponFix w;w.AuthoritativeFireEventIndex[0]=2147483645;Check(w.IsFireEventSequenceValid(0,2147483647),"sequence no signed overflow");}
 {AUTWeaponFix w;w.Reserve();w.ApplyStop(0,1);w.World.Now=10.051f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots==std::vector<int>{1}&&w.Acks==w.Shots&&!w.Pawn.Pending[0],"released reserved tap");w.CompleteAcceptedRateFire(0,11);Check(w.Shots.size()==1,"duplicate callback");}
 {AUTWeaponFix w;w.Reserve();w.CompleteAcceptedRateFire(0,99);Check(w.Shots.empty()&&w.Acks.empty(),"stale generation");}
 {AUTWeaponFix w;w.Reserve();w.Pawn.Pending[0]=true;w.World.Now=10.051f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots.size()==1&&w.Pawn.Pending[0],"held request");w.World.Now=12;w.World.Timer.Drain();Check(w.Shots.size()==1,"no invented auto shots");}
 {AUTWeaponFix w;w.Reserve();w.Reserve(2,1,true);w.ApplyStop(0,1);w.ApplyStop(1,2);w.World.Now=10.051f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots==std::vector<int>{1},"FIFO first");w.World.Now=10.054f;w.World.Timer.Drain();Check(w.Shots==std::vector<int>({1,2})&&w.Acks==w.Shots,"FIFO opposite mode");Check(!w.Pawn.Pending[0]&&!w.Pawn.Pending[1],"FIFO release");}
 for(int reason=0;reason<7;++reason){AUTWeaponFix w;w.Reserve();w.World.Now=10.051f;if(reason==0)w.Pawn.Dead=true;if(reason==1)w.Pawn.Weapon=nullptr;if(reason==2)w.Ammo[0]=false;if(reason==3)w.Allow[0]=false;if(reason==4)w.World.GS.Prevent=true;if(reason==5)w.Pawn.SavedPositions.push_back({10.01f,true});if(reason==6)w.World.Now=10.30f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots.empty()&&w.Acks.empty()&&!w.Cancels.empty(),"lifecycle/policy cancellation");}
 {AUTWeaponFix w;w.Reserve();w.World.Now=10.110f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots.empty()&&!w.Cancels.empty(),"no newer clamped target epoch");}
 {AUTWeaponFix w;w.Reserve();w.ApplyStop(0,0);auto*c=DeferredEquipFireContexts.Find(TWeakObjectPtr<AUTWeaponFix>(&w));Check(c&&!c->bReleaseSeen,"old Stop ownership");w.ApplyStop(0,100);Check(!c->bReleaseSeen,"jump Stop ownership");w.ApplyStop(0,1);Check(c->bReleaseSeen,"matching Stop");w.ApplyStop(0,1);Check(w.LastProcessedStopEventIndex[0]==1,"idempotent Stop");}
 {AUTWeaponFix w;w.Reserve();auto&t=ServerActualFireTimes[TWeakObjectPtr<AUTWeaponFix>(&w)];t.resize(2);t[0]=9.08f;w.World.Now=10.051f;w.CompleteAcceptedRateFire(0,11);Check(w.Shots.empty(),"actual cadence defers");w.World.Now=10.081f;w.World.Timer.Drain();Check(w.Shots.size()==1,"actual cadence commits once");}
 std::cout<<"PASS: actual admission, reservation delay, callback, promoter, Stop ownership; 23 scenario groups\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}return 0;}
