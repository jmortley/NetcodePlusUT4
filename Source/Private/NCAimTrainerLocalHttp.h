#pragma once

#include "CoreMinimal.h"

class IHttpRequest;

/** Isolated verified-TLS transport for the two fixed local-score endpoints.
 *  Returns null on platforms without an implemented secure transport. */
TSharedPtr<IHttpRequest> CreateNCAimTrainerLocalRequest();

/** Cancel all operations and drain native/game-thread callbacks during module shutdown.
 *  The Windows callback module is pinned until process exit as a final safeguard. */
bool ShutdownNCAimTrainerLocalHttp();
