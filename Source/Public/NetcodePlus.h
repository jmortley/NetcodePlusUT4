#pragma once

#include "CoreMinimal.h"
#include "UnrealTournament.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"


class FNetcodePlus : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

