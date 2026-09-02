#pragma once

// Installs the optional captured-gameplay mouse coalescer. The implementation
// is Windows-client-only; these functions are harmless no-ops elsewhere.
void RegisterNCHighPollingMouseInput();
void UnregisterNCHighPollingMouseInput();
