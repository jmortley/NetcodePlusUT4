// NetcodePlus.cpp
#include "NetcodePlus.h"
#include "Modules/ModuleManager.h"




IMPLEMENT_MODULE(FNetcodePlus, NetcodePlus)

void FNetcodePlus::StartupModule()
{
	UE_LOG(LogLoad, Log, TEXT("netcodeplus loaded"));
}

void FNetcodePlus::ShutdownModule()
{
	UE_LOG(LogLoad, Log, TEXT("netcodeplus unloaded"));
}
