// Implements the 'uepy' module exposed to Python
#pragma once

#include "uepy.h"

namespace uepy {

// called once during launch, at engine post init
void FinishPythonInit();

#if WITH_EDITOR
// called anytime we're in the editor and about to start PIE
void OnPreBeginPIE(bool b);
#endif

}; // end of namespace uepy

