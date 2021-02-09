#pragma once

#include "incpybind.h"
#include "IUEPYGlueMixin.generated.h"

namespace py = pybind11;

// any engine class we want to extend via Python should implement the IUEPYGlueMixin interface
UINTERFACE()
class UEPY_API UUEPYGlueMixin : public UInterface
{
    GENERATED_BODY()
};

class IUEPYGlueMixin
{
    GENERATED_BODY()

public:
    py::object pyInst;
};
