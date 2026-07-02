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
	bool             bOutline      = false;   // team-coloured LOS outline instead of the body/armour super-tint (keeps the forced model)
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

	/** "Outline" flag: give players a client-local, team-coloured, LOS-GATED outline (via
	 *  AUTCharacter::SetOutlineLocal) instead of the body/armour super-tint — a cleaner team read that
	 *  keeps the forced model. The engine stencil has NO visible-only mode (no-128 = through-wall X-ray,
	 *  +128 adds the visible rim), so call this EVERY FRAME: it line-traces each candidate from the
	 *  viewer's camera and outlines only the visible ones (occluded -> no outline at all). Pass
	 *  bSlowTick ~4x/sec to re-assert against replication clobbers + refresh the MPC_NCPOutline colours
	 *  (graceful no-op until that collection/material exists — stock outline stays red/blue until then).
	 *  Because M_OutlinePP is a RestrictedAsset (not editable in place), the recolour ships as NEW pak
	 *  assets: Mod.ini [ForceModels] OutlineMaterial/OutlineMPC point at them and the slow tick RE-POINTS
	 *  the camera manager's OutlineMat + DefaultPPSettings blendable (replace, never add — a second
	 *  blendable would double-draw the stock rim). Yields entirely to spectator X-Ray (bTacComView).
	 *  Restores pawns that drop out (toggle off / left / dead). Runs AFTER SuppressFlagCarrierOutlines.
	 *  Client-side; no-op on a dedicated server. */
	NETCODEPLUS_API void OutlinePlayers(class UWorld* World, bool bSlowTick);

	/** True when the "Outline" render-mode is in effect for this world: flag set AND client/standalone
	 *  AND every side the outline would touch is configured to READ as the stock rim palette (red team 0
	 *  / blue team 1 — the material isn't being recoloured for now, so mismatched colours fall back to
	 *  the normal super-tint; the Red/Blue style always qualifies since its hue is forced to 0/240).
	 *  Listen servers are excluded — SetOutlineLocal writes the REPLICATED bOutlineWhenUnoccluded, so a
	 *  host's LOS gating would clobber every client's outline state. Keys BOTH the outline pass and the
	 *  TeamArenaCharacter tint-gating (so a gated-off config keeps the normal super-tint). */
	NETCODEPLUS_API bool OutlineModeActive(class UWorld* World);

	/** Last OutlineModeActive verdict, refreshed once per frame by OutlinePlayers. Use from per-pawn
	 *  per-frame call sites (Tick overlay retint) — the full check is too heavy for that cadence.
	 *  At most one frame stale; false before the first OutlinePlayers tick of a world. */
	NETCODEPLUS_API bool OutlineModeActiveCached();

	/** The local viewer's effective team (0/1) for friend/enemy bucketing under the Team-vs-Enemy and
	 *  Enemy-Only styles. Returns the local PC's real team when playing; for a teamless spectator it
	 *  defaults to red (team 0) so Team = red and Enemy = blue. Use TheirTeam == GetViewerTeam(World)
	 *  as the "friendly" test instead of OnSameTeam(), which returns false for a spectator. */
	NETCODEPLUS_API int32 GetViewerTeam(class UWorld* World);

	/** Resolve which side's settings apply to a pawn under the active Style. Returned BY VALUE: the
	 *  Red/Blue style forces the hue (red/blue) and may borrow the Team/Enemy model, so it can't
	 *  always hand back a reference straight into the config. */
	NETCODEPLUS_API FNCPlusModelSettings GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly);

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

	/** A selectable installed character (for the picker).
	 *  bHidden      = curated out of the picker (baked drop-list / [ForceModels] HiddenModels / bHideInUI);
	 *                 surfaced only on the audit path (forcemodels_list).
	 *  ClassPath    = the AUTCharacterContent class actually applied when this entry is chosen.
	 *  VariantPaths = every class path this entry stands for (always >=1). For a coalesced numbered family
	 *                 (the single "SkaarjMale" entry covering SkaarjMale01..03) it lists all members and
	 *                 ClassPath is the representative; for a normal entry it is just { ClassPath }. Lets the
	 *                 menu show a saved variant path (e.g. SkaarjMale03) as this entry's current selection. */
	struct FContentEntry
	{
		FString         DisplayName;
		FString         ClassPath;
		TArray<FString> VariantPaths;
		bool            bHidden = false;
	};
	/** Enumerate installed AUTCharacterContent for the model picker (on-disk only -> self-limiting to
	 *  renderable). Picker path (bIncludeHidden=false):
	 *   - LOCKED to a baked allowlist of stems (custom: NecrisFemaleCoat/Genghis/LiandriRobot; stock
	 *     families: NecrisFemale/NecrisMale/NecrisMale_Damian/NecrisMale_Necroth/SkaarjMale/TC_Male) — only
	 *     these reach the menu, so new/unknown installed content can't auto-appear. [ForceModels] AllowModels=
	 *     (comma-separated stems) appends more without a rebuild; and
	 *   - COALESCES numbered variant families — NecrisMale01..05 collapse to one "NecrisMale" entry applying
	 *     the lowest-numbered member.
	 *  Audit path (bIncludeHidden=true, used by forcemodels_list): EVERY installed character, uncoalesced,
	 *  with the denylist / bHideInUI-curated ones marked bHidden=true. */
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

	/** Debug (cvar `ncp.DebugHeads`): draw the capsule-relative headshot sphere (GREEN = what the server
	 *  validates) and the mesh head bone (RED cross = the visible head) for every other pawn, client-side,
	 *  so the head hitbox can be eyeballed ONLINE — ut.DebugHeadshots is compiled out of Shipping and draws
	 *  server-side. Call from a HUD's DrawHUD with its Canvas + PlayerOwner. TEMP calibration aid; strip /
	 *  cheat-gate before final ship. */
	NETCODEPLUS_API void DrawHeadDebug(class UCanvas* Canvas, class APlayerController* PC);
}
