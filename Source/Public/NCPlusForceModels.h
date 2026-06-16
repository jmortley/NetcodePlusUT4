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
	float             Brightness = 1.f;             // "Glow": brightness multiplier 1-5 (1 = normal); overbrights the recolour albedo (+ emissive) in ApplyForcedModel
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

	/** Material-name substrings that mark a model NON-recolourable ("baked-texture" models whose colour
	 *  params are inert — e.g. the community Robot). From [ForceModels] BakedMaterials=Robot,... Empty by
	 *  default; param-LESS models are auto-detected at apply time, so this is only for the inert case
	 *  (params exist but don't drive albedo — indistinguishable via the material API). Matched models
	 *  fall back to their baked red/blue team skin instead of the futile recolour. */
	TArray<FString>  BakedMaterialSubstrings;
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

	/** Re-apply forced models to every ATeamArenaCharacter in World (e.g. after the menu saves a
	 *  config change). Iterator-safe (collect-then-apply) and routes through each pawn's
	 *  NotifyTeamChanged, so a now-disabled side correctly un-forces. No-op on a dedicated server. */
	NETCODEPLUS_API void ReapplyAll(class UWorld* World);

	/** HUD recolour (the "HUD" flag): overwrite each team's TeamColor with the configured skin colour
	 *  (relative to the local viewer) so the beacon / HUD / weapons / chat match the forced models.
	 *  Client-side, re-asserted to survive replication; restores the real colours when HUD is off.
	 *  Mirrors the BP's "Update team colour" timer — call ~4x/sec from a client ticker. No-op on a
	 *  dedicated server. */
	NETCODEPLUS_API void SyncHudTeamColours(class UWorld* World);

	/** Recolour the CTF flag cloth to each team's configured skin colour (relative to the local viewer),
	 *  gated by the "Flags" flag. Client-side, re-asserted from the same ticker (survives flag respawns);
	 *  restores the flag's real colour when off. No-op outside CTF / on a dedicated server. */
	NETCODEPLUS_API void SyncFlagColours(class UWorld* World);

	/** Drive a single client-local AWindDirectionalSource so dc's FlagMesh cloth waves — a C++ port of
	 *  dc's FlagWind BP (random-walk speed within [Min,Max], random-walk angular velocity capped at
	 *  MaxAngularSpeed, integrate the actor's rotation). Spawns the wind when the cloth recolour is
	 *  active (CTF + Flags), destroys it when off. Call EVERY frame (real DeltaTime) from the client
	 *  ticker. Tuning via [ForceModels.FlagWind] Min/MaxSpeed, MaxAcceleration, Max{Angular}Speed,
	 *  MaxAngularAcceleration. No-op on a dedicated server / outside CTF. */
	NETCODEPLUS_API void TickFlagWind(class UWorld* World, float DeltaTime);

	/** Suppress the stock through-walls flag-carrier outline (CTF only — matches dc's flag BP). Forces
	 *  bForceNoOutline on each carrier and refreshes, dropping the body/weapon/flag CustomDepth stencil;
	 *  restores former carriers. Always on (it's a fix, not a setting). Client-side, re-asserted from the
	 *  same ticker as SyncFlagColours. No-op on a dedicated server or outside CTF (incl. Blitz). */
	NETCODEPLUS_API void SuppressFlagCarrierOutlines(class UWorld* World);

	/** The local viewer's effective team (0/1) for friend/enemy bucketing under the Team-vs-Enemy and
	 *  Enemy-Only styles. Returns the local PC's real team when playing; for a teamless spectator it
	 *  defaults to red (team 0) so Team = red and Enemy = blue. Use TheirTeam == GetViewerTeam(World)
	 *  as the "friendly" test instead of OnSameTeam(), which returns false for a spectator. */
	NETCODEPLUS_API int32 GetViewerTeam(class UWorld* World);

	/** Resolve which side's settings apply to a pawn under the active Style. */
	NETCODEPLUS_API const FNCPlusModelSettings& GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly);

	/** HSV(degrees) -> FLinearColor for a side (base albedo tint; V is the normal 0-1 brightness). */
	NETCODEPLUS_API FLinearColor GetSkinColour(const FNCPlusModelSettings& Side);

	/** Armour-overlay colour for a side: the skin colour (ArmourMode=MatchSkin) or its complement
	 *  (hue + 180, ArmourMode=Complimentary). Used to override the stock yellow armour overlay. */
	NETCODEPLUS_API FLinearColor GetArmourColour(const FNCPlusModelSettings& Side);

	/** Resolve + GC-pin + cache a side's AUTCharacterContent class (nullptr if none/unloadable). */
	NETCODEPLUS_API TSubclassOf<AUTCharacterContent> GetModelClass(const FNCPlusModelSettings& Side);

	/** Future server-policy gate. Returns true (no-op) until policing lands; the server-side
	 *  AMutForceModels will own a replicated allow/deny policy this consults. */
	NETCODEPLUS_API bool IsModelAllowed(TSubclassOf<AUTCharacterContent> Content);

	/** A selectable installed character (for the picker). bHidden = matches the [ForceModels]
	 *  HiddenModels curation denylist (kept out of the picker; surfaced by forcemodels_list). */
	struct FContentEntry { FString DisplayName; FString ClassPath; bool bHidden = false; };
	/** Enumerate every installed AUTCharacterContent (on-disk only -> self-limiting to renderable).
	 *  Curated by [ForceModels] HiddenModels= (comma-separated name substrings) — those are excluded
	 *  unless bIncludeHidden (the audit path) is true, where they're returned with bHidden=true. */
	NETCODEPLUS_API void EnumerateContent(TArray<FContentEntry>& Out, bool bIncludeHidden = false);

	/** Union of known UT team-colour vector param names — shotgunned onto every model's MIDs.
	 *  SetVectorParameterValue no-ops names a material lacks, so this colours any UT-framework
	 *  model and harmlessly skips the rest. */
	NETCODEPLUS_API const TArray<FName>& TeamColourParamNames();

	/** True if a material's name matches the skip list (face/eyes/hair) — don't recolour it. */
	NETCODEPLUS_API bool IsRecolorSkippedMaterial(const FString& MaterialName);

	/** True if a material's name matches the [ForceModels] BakedMaterials denylist — its model can't be
	 *  recoloured (inert params) and should fall back to its baked red/blue skin. Empty list -> false. */
	NETCODEPLUS_API bool IsBakedMaterial(const FString& MaterialName);

	/** Diagnostic (backs the `forcemodels_dumpmats` console command): log every AUTCharacter's body
	 *  materials + their vector/scalar parameter names, so we can see what a custom model actually
	 *  exposes. Client-side, on-demand, Shipping-safe (Warning verbosity). */
	NETCODEPLUS_API void DumpAllCharacterMaterials(class UWorld* World);
}
