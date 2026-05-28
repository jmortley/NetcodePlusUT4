// NCEloUploader — POST per-match ELO data to ut4stats.com (Django backend).
//
// Companion to FNCStatsUploader (which stays a stub for full match-stats uploads —
// real match stats go through the StatSQL plugin). This uploader is narrowly
// scoped to the global-ELO push: each rating system (Duel, ElimPlus, ShaftArena)
// builds a JSON payload via its BuildResultPayload helper, then calls
// PostMatchResult here right after its match-end Flush.
//
// Config: reuses StatSQL's [UTPUGS_STATS] Mod.ini section for the auth token
// (one bot account per server is shared across StatSQL stats uploads + this
// ELO push since they land on the same Django site). Plus URL option overrides:
//   ?NCEloUrl=https://example.com    override base URL
//   ?NCEloKey=abc123                 override auth token
//   ?NCEloEnabled=false              disable upload entirely
//
// HTTP behavior mirrors AMutStatSQL::SendPost — 3 attempts, exponential backoff
// (1s, 2s, 4s), Authorization: Token <key> header. Fire-and-forget; callers
// don't get a result back.
//
// Server-only path. Free-function class (no UObject lifecycle).
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

class NETCODEPLUS_API FNCEloUploader
{
public:
	/** POST a pre-built JSON envelope to /elo_entry/. Builds an HTTP request,
	 *  attaches auth, retries on transient failure, logs the final outcome. */
	static void PostMatchResult(UWorld* World, const FString& JsonBody);

	/** Resolve the server-name string to put in the elo_entry payload.
	 *  Returns "<ModIni.ServerName> <GS->ServerName>" when Mod.ini's
	 *  [UTPUGS_STATS] ServerName is set — the Mod.ini value matches what
	 *  StatSQL records for the same match, so Django correlation gets a
	 *  reliable substring-match bonus; the trailing GS->ServerName preserves
	 *  the per-PUG instance identifier for display.
	 *  Falls back to plain GS->ServerName when Mod.ini ServerName is empty
	 *  (back-compat: servers that haven't configured it keep current behavior).
	 *  Truncates to 64 chars (Django CharField(max_length=64)) preserving the
	 *  Mod.ini prefix verbatim (the part that matters for correlation). */
	static FString ResolveServerName(UWorld* World);
};
