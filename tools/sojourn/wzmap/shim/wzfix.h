/* WzMap MinGW build: MSVC's windows.h min/max macros, done as templates; cmdlib helpers picomodel calls */
#pragma once
#ifdef __cplusplus
template<class A, class B> static inline auto max(A a, B b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template<class A, class B> static inline auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
extern void StripFilename(char *path);
#endif
