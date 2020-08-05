// Implements the 'uepy' module exposed to Python
#pragma once

#include "uepy.h"

namespace uepy {

// called once during launch, at engine post init
void FinishPythonInit();

}; // end of namespace uepy

