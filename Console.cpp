//========= Copyright KiwiEngine, All rights reserved ============//
//
// Purpose: 
//
//================================================================//

#include "Console.h"

#include <Windows.h>
#include <iostream>

bool Console::AllocateConsole()
{
    if (!AllocConsole()) return false;

    // Redirect stdin/stdout/strerr
    if (freopen_s((FILE**)stdin, "CONIN$", "r", stdin) != 0)    return false;
    if (freopen_s((FILE**)stdout, "CONOUT$", "w", stdout) != 0) return false;
    if (freopen_s((FILE**)stderr, "CONOUT$", "w", stderr) != 0) return false;

    // Make C++ standard streams work
    std::ios::sync_with_stdio(true);

	return true;
}

void Console::FreeConsole()
{
    // Restore original stdin/stdout/stderr
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);

    // Free the console window
    ::FreeConsole();
}

void Console::Print(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

void Console::PrintLine(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");
}

#ifdef UNICODE
void Console::SetTitle(const wchar_t* title)
#else
void Console::SetTitle(const char* title)
#endif
{
	SetConsoleTitle(title);
}