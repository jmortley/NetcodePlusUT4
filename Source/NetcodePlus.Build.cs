// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class NetcodePlus : ModuleRules
	{
		public NetcodePlus(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PublicIncludePaths.AddRange(new string[] {
				Path.Combine(ModuleDirectory, "Public"),
				// Vendored Glicko2 (github.com/tronunator/Glicko2). Cross-includes
				// like #include "TeamGlickoRating.h" resolve here without editing
				// the vendored files. Used by ElimPlus rating system.
				Path.Combine(ModuleDirectory, "Public/Glicko2")
            });
			PrivateIncludePaths.AddRange(new string[] {
				Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../Source/UnrealTournament/Private")),
				Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../Source/UnrealTournament/Public"))
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
				"PhysicsCore",   // 4.27 UPhysicalMaterial::DetermineSurfaceType
				"AppFramework",   // SColorPicker (used by SNCPlusHUDEditor color swatches)
				"Http",
				"Json",
				"JsonUtilities",
				"Projects",       // IPluginManager (FNCPlusHUDLayout::PluginResourcesDir)
				"RenderCore"      // GWhiteTexture (QuickStats DrawArc canvas fallback)
            });

			// Recovered Clutch HUD PNGs are decoded only by rendered clients/editors.
			// Dedicated Linux servers do not ship the libPNG archive required by
			// ImageWrapper, and the decoding code is compiled out under UE_SERVER.
			if (Target.Type != TargetRules.TargetType.Server)
			{
				PrivateDependencyModuleNames.Add("ImageWrapper");
			}
		}
	}
}
