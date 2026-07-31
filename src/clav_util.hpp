#ifndef WIN32_UTIL_HPP_019f8131_b40b_780e_8820_3bb985fd237c
#define WIN32_UTIL_HPP_019f8131_b40b_780e_8820_3bb985fd237c
#include <string>
#include <string_view>
#include <windows.h>
std::wstring to_wide(std::string_view const s);
std::string myerr(DWORD const err);
std::wstring mywerr(DWORD const err);
void check_hresult(HRESULT hr, char const* what);
#endif /* WIN32_UTIL_HPP_019f8131_b40b_780e_8820_3bb985fd237c */
