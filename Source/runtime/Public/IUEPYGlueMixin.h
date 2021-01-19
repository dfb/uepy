#pragma once

#pragma warning(push)
#pragma warning (disable : 4686 4191 340)
#pragma push_macro("check")
#undef check
#include <pybind11/pybind11.h>
#pragma pop_macro("check")
#pragma warning(pop)

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