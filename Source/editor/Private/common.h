#pragma once

DECLARE_LOG_CATEGORY_EXTERN(UEPYED, Log, All);
#define LOG(format, ...) UE_LOG(UEPYED, Log, TEXT("[%s:%d] %s"), TEXT(__FUNCTION__), __LINE__, *FString::Printf(TEXT(format), ##__VA_ARGS__ ))
#define LWARN(format, ...) UE_LOG(UEPYED, Warning, TEXT("[%s:%d] %s"), TEXT(__FUNCTION__), __LINE__, *FString::Printf(TEXT(format), ##__VA_ARGS__ ))
#define LERROR(format, ...) UE_LOG(UEPYED, Error, TEXT("[%s:%d] %s"), TEXT(__FUNCTION__), __LINE__, *FString::Printf(TEXT(format), ##__VA_ARGS__ ))

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


