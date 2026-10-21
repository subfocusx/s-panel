#pragma once
#include <windows.h>

void LogLine(const wchar_t* fmt, ...);
void LogLastError(const wchar_t* what);
