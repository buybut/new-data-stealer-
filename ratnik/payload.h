#pragma once
#include "common.h"

// Finds fsadf.py, makes sure Python is available (installing it within
// PYTHON_ACQUIRE_TIMEOUT_MS if necessary), then starts the animation.
// If Python cannot be obtained in that budget the whole step is skipped.
void LaunchPayload();
