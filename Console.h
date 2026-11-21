//========= Copyright KiwiEngine, All rights reserved ============//
//
// Purpose: 
//
//================================================================//

#pragma once

class Console
{
public:
	// use for Win apps with no console
	static bool AllocateConsole();
	static void FreeConsole();

	static void Print(const char* fmt, ...);
	static void PrintLine(const char* fmt, ...);

public:
#ifdef UNICODE
	static void SetTitle(const wchar_t* title);
#else
	static void SetTitle(const char* title);
#endif
};