#pragma once

#include "CoreMinimal.h"
#include "UnrealTournament.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"


// CHANGE THIS NUMBER WHEN YOU RELEASE AN UPDATE
#define NETCODE_PLUGIN_VERSION 308

class FNetcodePlus : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// Helper to access version globally
	static int32 GetPluginVersion() { return NETCODE_PLUGIN_VERSION; }
};

