#include "timing_adapter.h"
#include "NCClientFireTiming.h"
#include <iostream>
#include <string>
CleanupDelegate FWorldDelegates::OnWorldCleanup;
void Check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
void Near(double a,double b,double tol,const char*m){Check(std::abs(a-b)<=tol,m);}
int main(){
 try {
  NCClientFireTiming::Startup();
  int scenarios=0;
  // Deliberately leave worlds alive: the adapter stubs weak-pointer serials, so
  // addresses must not be reused. UE supplies serial-aware weak identity.
  for(int fps:{60,144,240,360,720,1000})for(float start:{10.f,120.f,900.f,1800.f}){
   auto* w=new UWorld;auto* p=new AUTCharacter;auto* gun=new AUTWeaponFix(w,p);w->WorldTime=start;
   w->Timers.BeginFrame();NCClientFireTiming::Record(gun,0,true);w->Timers.Tick(1.f/fps);
   // Anchor and pending first-refire both start at end of the first tick.
   const double origin=w->Timers.Internal;
   Near(NCClientFireTiming::Remaining(gun,0),1.,1e-7,"pending first shot phase");
   double last=0.;double worst=0.;int shots=1;
   for(int frame=1;frame<=fps*120;++frame){
    w->Step(1.f/fps);const double now=w->Timers.Internal-origin;
    if(now>shots){NCClientFireTiming::Record(gun,0,true);last=std::min(last+1.,now);++shots;}
    const double r=NCClientFireTiming::Remaining(gun,0);
    worst=std::max(worst,std::abs(r-(last+1.-now)));
    Check(r<=1.000001,"future debt accumulated");
   }
   Check(worst<0.000002,"anchor accumulated float drift");
   std::cout<<"PASS hold fps="<<fps<<" world="<<start<<" maxResidualError="<<worst<<"\n";++scenarios;
  }
  // Jittered frame deltas and long hitches cross several anchor epochs. The
  // reference is TimerManager's double clock, never float WorldTime.
  {
   auto*w=new UWorld;auto*p=new AUTCharacter;auto*g=new AUTWeaponFix(w,p);
   NCClientFireTiming::Record(g,0);w->Step(.002f);const double origin=w->Timers.Internal;
   double last=0.;int shots=1;
   for(int frame=1;frame<=120000;++frame){
    float dt=frame%7919==0?.075f:(frame%7==0?.004f:.0008f);
    w->Step(dt);double now=w->Timers.Internal-origin;
    if(now>shots){NCClientFireTiming::Record(g,0,true);last=std::min(last+1.,now);++shots;}
    Near(NCClientFireTiming::Remaining(g,0),last+1.-now,2e-6,"jitter clock diverged");
   }++scenarios;
  }
  // A different due callback can observe the anchor before or after its epoch
  // callback. Both must report the same absolute timer clock.
  for(bool observerFirst:{false,true}){
   auto*w=new UWorld;auto*p=new AUTCharacter;auto*g=new AUTWeaponFix(w,p);FTimerHandle observer;
   float inCallback=-10.f;
   auto arm=[&](){w->Timers.SetTimer(observer,FTimerDelegate::CreateLambda([&](){inCallback=NCClientFireTiming::Remaining(g,0);}),1.f,false);};
   if(observerFirst)arm();NCClientFireTiming::Record(g,0);if(!observerFirst)arm();
   w->Step(.001f);w->Step(1.001f);
   Near(inCallback,NCClientFireTiming::Remaining(g,0),2e-7,"epoch callback order changed clock");++scenarios;
  }
  auto*w=new UWorld;auto*p=new AUTCharacter;auto*g=new AUTWeaponFix(w,p);
  NCClientFireTiming::Record(g,0);w->Step(.01f);w->Step(.2f);
  float before=NCClientFireTiming::Remaining(g,0);for(int i=0;i<100;++i)w->Step(0.f);
  Near(NCClientFireTiming::Remaining(g,0),before,1e-7,"pause changed cooldown");
  for(int i=0;i<100;++i)w->Step(.001f*.5f);
  Near(NCClientFireTiming::Remaining(g,0),before-.05,2e-7,"time dilation diverged");++scenarios;
  w->Step(3.75f);Near(NCClientFireTiming::Remaining(g,0),-.0-3.0,2e-6,"multi epoch catchup");++scenarios;
  // Mode and rate changes must use the same clock, with independently stored shots.
  NCClientFireTiming::Record(g,0);w->Step(.1f);NCClientFireTiming::Record(g,1);g->Refire[1]=.2f;
  Near(NCClientFireTiming::Remaining(g,0),.9,2e-7,"mode0 overwritten");Near(NCClientFireTiming::Remaining(g,1),.2,2e-7,"mode1 wrong");
  g->Refire[0]=.5f;Near(NCClientFireTiming::Remaining(g,0),.4,2e-7,"rate change wrong");++scenarios;
  p->Local=false;g->LastFireTime[0]=w->WorldTime-.1f;Near(NCClientFireTiming::Remaining(g,0),.4,2e-6,"remote authority fallback changed");
  p->Local=true;UDemoNetDriver demo;demo.Playing=true;w->DemoNetDriver=&demo;Near(NCClientFireTiming::Remaining(g,0),.4,2e-6,"replay fallback changed");w->DemoNetDriver=nullptr;++scenarios;
  auto* other=new AUTCharacter;g->Owner=other;g->LastFireTime[0]=-1.f;Near(NCClientFireTiming::Remaining(g,0),0.,1e-7,"old owner retained");
  NCClientFireTiming::Record(g,0);Near(NCClientFireTiming::Remaining(g,0),.5,1e-7,"new owner clock bad");NCClientFireTiming::Forget(g);Near(NCClientFireTiming::Remaining(g,0),0.,1e-7,"forget failed");++scenarios;
  FWorldDelegates::OnWorldCleanup.Broadcast(w);Check(w->Timers.Timers.empty(),"world anchor not cleared");NCClientFireTiming::Shutdown();Check(!FWorldDelegates::OnWorldCleanup.Fn,"shutdown delegate retained");++scenarios;
  std::cout<<"TOTAL "<<scenarios<<" scenarios passed\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
