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
				"NetcodePlus/Public",
				// Vendored Glicko2 (github.com/tronunator/Glicko2). Cross-includes
				// like #include "TeamGlickoRating.h" resolve here without editing
				// the vendored files. Used by ElimPlus rating system.
				"NetcodePlus/Public/Glicko2"
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
				"InputCore",
				"Slate",
				"SlateCore"
			});

			PrivateDependencyModuleNames.AddRange(new string[] {
				"AssetRegistry",
				"AppFramework",   // SColorPicker (used by SNCPlusHUDEditor color swatches)
				"Http",
				"Json",
				"JsonUtilities",
				"RenderCore"      // GWhiteTexture (QuickStats DrawArc canvas fallback)
            });
		}
	}
}
