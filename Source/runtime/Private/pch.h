// TODO: cfg plugin to actually use this as the PCH

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#define PY_MAJOR_VERSION 3
#define PY_MINOR_VERSION 11
#include "GameFramework/Actor.h"
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

namespace py = pybind11;

