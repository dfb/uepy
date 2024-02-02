#pragma once

// use this to include pybind in a header file w/o weird errors
#define PY_MAJOR_VERSION 3
#define PY_MINOR_VERSION 11
#pragma warning(push)
#pragma warning (disable : 4686 4191 340)
#pragma push_macro("check")
#undef check
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <pybind11/operators.h>
#pragma pop_macro("check")
#pragma warning(pop)

namespace py = pybind11;

