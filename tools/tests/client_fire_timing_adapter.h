#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
using uint8=uint8_t; using int32=int32_t;
namespace FMath { template<class T>T Max(T a,T b){return std::max(a,b);} template<class T>T Min(T a,T b){return std::min(a,b);} }
template<class T>struct TWeakObjectPtr { T* P=nullptr; TWeakObjectPtr(){} TWeakObjectPtr(T* p):P(p){} T* Get()const{return P;} bool IsValid()const{return P!=nullptr;} bool operator<(const TWeakObjectPtr& b)const{return P<b.P;} };
template<class T>using TSharedPtr=std::shared_ptr<T>;
template<class T>TSharedPtr<T> MakeShareable(T* p){return TSharedPtr<T>(p);}
template<class T>struct TArray:std::vector<T>{ void Init(T v,int n){this->assign(n,v);} bool IsValidIndex(int n)const{return n>=0 && size_t(n)<this->size();} int Num()const{return int(this->size());} };
template<class K,class V>struct TMap {
 std::map<K,V> Data;
 V* Find(K k){auto i=Data.find(k);return i==Data.end()?nullptr:&i->second;}
 void Empty(){Data.clear();} void Add(K k,V v){Data[k]=v;} void Remove(K k){Data.erase(k);}
 struct Iter {TMap* M;typename std::map<K,V>::iterator I;bool Removed=false;explicit operator bool()const{return I!=M->Data.end();} K Key(){return I->first;} V& Value(){return I->second;} void RemoveCurrent(){I=M->Data.erase(I);Removed=true;} void operator++(){if(Removed)Removed=false;else ++I;} };
 Iter CreateIterator(){return {this,Data.begin(),false};}
};
struct FTimerHandle{int Id=0;};
struct FTimerDelegate{std::function<void()> Fn;template<class F>static FTimerDelegate CreateLambda(F f){return {f};}};
struct FTimerManager {
 struct Timer{float Rate;double Expire;bool Loop;int Status;std::function<void()> Fn;}; // 0 pending,1 active,2 executing
 std::map<int,Timer> Timers;double Internal=0.;bool Ticked=false;int Next=1;
 void SetTimer(FTimerHandle& h,FTimerDelegate d,float r,bool loop){if(!h.Id)h.Id=Next++;Timers[h.Id]={r,Ticked?Internal+r:r,loop,Ticked?1:0,d.Fn};}
 float GetTimerElapsed(FTimerHandle h){auto i=Timers.find(h.Id);if(i==Timers.end())return -1.f;auto& t=i->second;return float(t.Rate-(t.Expire-(t.Status?Internal:0.)));}
 void ClearTimer(FTimerHandle& h){Timers.erase(h.Id);h.Id=0;} void BeginFrame(){Ticked=false;}
 void Tick(float dt){Internal+=dt; for(;;){auto next=Timers.end();for(auto i=Timers.begin();i!=Timers.end();++i)if(i->second.Status==1 && Internal>i->second.Expire && (next==Timers.end()||i->second.Expire<next->second.Expire))next=i;if(next==Timers.end())break;auto& t=next->second;t.Status=2;int n=t.Loop?int((Internal-t.Expire)/t.Rate)+1:1;for(int c=0;c<n;++c)t.Fn();if(t.Loop){t.Expire+=float(n*t.Rate);t.Status=1;}else Timers.erase(next);} Ticked=true;for(auto& kv:Timers)if(kv.second.Status==0){kv.second.Expire+=Internal;kv.second.Status=1;} }
};
struct UDemoNetDriver{bool Playing=false;bool IsPlaying(){return Playing;}};
struct UWorld {FTimerManager Timers;float WorldTime=0.f;UDemoNetDriver* DemoNetDriver=nullptr;FTimerManager& GetTimerManager(){return Timers;}float GetTimeSeconds(){return WorldTime;}void Step(float dt){Timers.BeginFrame();WorldTime+=dt;Timers.Tick(dt);}};
struct AUTCharacter{bool Local=true;bool IsLocallyControlled(){return Local;}};
class AUTWeaponFix{public:UWorld* World;AUTCharacter* Owner;TArray<float> LastFireTime;float Refire[2]={1.f,1.f};float EarliestFireTime=0.f;AUTWeaponFix(UWorld*w,AUTCharacter*o):World(w),Owner(o){LastFireTime.Init(-1.f,2);}UWorld* GetWorld(){return World;}AUTCharacter* GetUTOwner(){return Owner;}float GetRefireTime(uint8 m){return Refire[m];}};

struct FDelegateHandle{bool Valid=false;bool IsValid()const{return Valid;}void Reset(){Valid=false;}};
struct CleanupDelegate{std::function<void(UWorld*,bool,bool)> Fn;FDelegateHandle AddStatic(void(*f)(UWorld*,bool,bool)){Fn=f;return {true};}void Remove(FDelegateHandle){Fn=nullptr;}void Broadcast(UWorld*w){if(Fn)Fn(w,false,true);}};
struct FWorldDelegates{static CleanupDelegate OnWorldCleanup;};
inline bool UObjectInitialized(){return true;}
