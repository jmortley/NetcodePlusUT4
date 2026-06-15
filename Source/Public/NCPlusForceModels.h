// NCPlusForceModels.h
// Client-side "force team models" config + resolvers — C++ port of the MutForceModels
// Blueprint (phase 1: types, Mod.ini IO, model/colour resolution, installed-character
// enumeration, the shotgun-recolour param set, and a server-policy gate stub).
// Applied per-pawn by ATeamArenaCharacter::NotifyTeamChanged. Purely client-local:
// nothing here replicates, and it is a no-op on a dedicated server.
#pragma once
#include "NetcodePlus.h"

class AUTCharacterContent;

/** Style dropdown — how a pawn's model/colour is chosen. Values match the Mod.ini int. */
enum class ENCPlusSkinStyle : uint8
{
	TeamEnemy = 0,  // relative to the local viewer (friends vs foes)
	RedBlue   = 1,  // absolute by team
	EnemyOnly = 2,  // only enemies reskinned; teammates left untouched
};

/** Armour recolour mode (render-only). */
enum class ENCPlusArmourMode : uint8
{
	MatchSkin     = 0,
	Complimentary = 1,
};

/** One side's settings (Enemy/Team/Red/Blue), from [ForceModels.Model.<side>]. */
struct FNCPlusModelSettings
{
	FString           ContentPath;                 // AUTCharacterContent class path; empty = don't force this side
	float             H = 0.f;                      // hue in degrees (0-360)
	float             S = 1.f;
	float             V = 1.f;                       // HSV value = base brightness within the normal 0-1 range
	float             Brightness = 1.f;             // overbright multiplier (1 = off) for a capped highlight glow; see GetEmissiveColour
	bool              bComplimentary = false;
	ENCPlusArmourMode ArmourMode = ENCPlusArmourMode::MatchSkin;
};

/** Whole-feature config: [ForceModels] flags + the four side sections. */
struct FNCPlusForceModelsConfig
{
	bool             bEnabled      = false;
	bool             bModels       = true;
	bool             bHUD          = true;
	bool             bArmour       = true;
	bool             bFlags        = true;
	bool             bDarkenBodies = false;
	bool             bCosmetics    = true;
	ENCPlusSkinStyle Style         = ENCPlusSkinStyle::TeamEnemy;
	FNCPlusModelSettings Enemy, Team, Red, Blue;

	/** Optional override of which material params get team-coloured, from
	 *  [ForceModels] RecolorParams=Name1,Name2,... (param names may contain spaces).
	 *  Empty = the built-in default (TeamColourParamNames). Use this to e.g. colour
	 *  only armour params and leave body/face alone. */
	TArray<FName>    RecolorParams;

	/** Material-name substrings to SKIP when recolouring, so face/eyes/hair stay natural,
	 *  from [ForceModels] SkipMaterials=head,eye,hair,... Empty = built-in default
	 *  (head/face/eye/hair/teeth/...). Matched case-insensitively against each material name
	 *  (e.g. "M_Skaarj_Head_Inst" matches "head"). */
	TArray<FString>  SkipMaterialSubstrings;
};

namespace NCPlusForceModels
{
	/** Lazy-loaded singleton config (reads Mod.ini on first access). */
	NETCODEPLUS_API const FNCPlusForceModelsConfig& Get();
	/** Mutable accessor for the menu (ensures loaded). */
	NETCODEPLUS_API FNCPlusForceModelsConfig& Mutable();
	/** Force a re-read from Mod.ini. */
	NETCODEPLUS_API void Reload();
	/** Persist the current config to Mod.ini. */
	NETCODEPLUS_API void Save();

	/** Master gate: feature on AND model-forcing enabled. */
	NETCODEPLUS_API bool IsEnabled();

	/** Resolve which side's settings apply to a pawn under the active Style. */
	NETCODEPLUS_API const FNCPlusModelSettings& GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly);

	/** HSV(degrees) -> FLinearColor for a side (base albedo tint; V is the normal 0-1 brightness). */
	NETCODEPLUS_API FLinearColor GetSkinColour(const FNCPlusModelSettings& Side);

	/** The side's colour scaled by its Brightness (clamped to MaxBrightness / a hard ceiling).
	 *  Fed to the emissive/overlay params only, so a >1 boost reads as a capped highlight glow
	 *  rather than blown-out / hue-shifted albedo. */
	NETCODEPLUS_API FLinearColor GetEmissiveColour(const FNCPlusModelSettings& Side);

	/** True if a team-colour param is an emissive/overlay (glow) channel — gets GetEmissiveColour. */
	NETCODEPLUS_API bool IsEmissiveParam(FName Param);

	/** Resolve + GC-pin + cache a side's AUTCharacterContent class (nullptr if none/unloadable). */
	NETCODEPLUS_API TSubclassOf<AUTCharacterContent> GetModelClass(const FNCPlusModelSettings& Side);

	/** Future server-policy gate. Returns true (no-op) until policing lands; the server-side
	 *  AMutForceModels will own a replicated allow/deny policy this consults. */
	NETCODEPLUS_API bool IsModelAllowed(TSubclassOf<AUTCharacterContent> Content);

	/** A selectable installed character (for the picker). */
	struct FContentEntry { FString DisplayName; FString ClassPath; };
	/** Enumerate every installed AUTCharacterContent (on-disk only -> self-limiting to renderable). */
	NETCODEPLUS_API void EnumerateContent(TArray<FContentEntry>& Out);

	/** Union of known UT team-colour vector param names — shotgunned onto every model's MIDs.
	 *  SetVectorParameterValue no-ops names a material lacks, so this colours any UT-framework
	 *  model and harmlessly skips the rest. */
	NETCODEPLUS_API const TArray<FName>& TeamColourParamNames();

	/** True if a material's name matches the skip list (face/eyes/hair) — don't recolour it. */
	NETCODEPLUS_API bool IsRecolorSkippedMaterial(const FString& MaterialName);

	/** Diagnostic (backs the `forcemodels_dumpmats` console command): log every AUTCharacter's body
	 *  materials + their vector/scalar parameter names, so we can see what a custom model actually
	 *  exposes. Client-side, on-demand, Shipping-safe (Warning verbosity). */
	NETCODEPLUS_API void DumpAllCharacterMaterials(class UWorld* World);
}
