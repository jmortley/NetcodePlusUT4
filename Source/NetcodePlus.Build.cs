// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class NetcodePlus : ModuleRules
	{
		public NetcodePlus(TargetInfo Target)
		{
			PrivateIncludePaths.Add("NetcodePlus/Private");
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PublicIncludePaths.AddRange(new string[] {
				"NetcodePlus/Public"
            });
			PrivateIncludePaths.AddRange(new string[] {
				"UnrealTournament/Private",
				"UnrealTournament/Classes"
			});

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealTournament",
				"InputCore"
			});

			// Add any additional UT dependencies
			PrivateDependencyModuleNames.AddRange(new string[] { 
                // Add specific modules if needed
            });
		}
	}
}
