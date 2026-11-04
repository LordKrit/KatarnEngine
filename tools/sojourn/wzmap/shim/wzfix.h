/* WzMap MinGW build: MSVC's windows.h min/max macros, done as templates; cmdlib helpers picomodel calls */
#pragma once
#ifdef __cplusplus
#include <type_traits>
/* by value (decltype of ?: on two same-type lvalues is a reference - it returned a dangling reference) */
template<class A, class B> static inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }
template<class A, class B> static inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
extern void StripFilename(char *path);
#endif
