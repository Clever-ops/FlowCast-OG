#include <csignal>
#include <unistd.h>
#include "types.h"

#ifndef __ANDROID__
void os_DebugBreak()
{
#ifdef __linux__
	raise(SIGTRAP);
#elif defined(_WIN32)
	__debugbreak();
#endif
}

#ifdef _WIN32
HWND getNativeHwnd()
{
	return (HWND)NULL;
}
#endif

void os_SetupInput()
{
}
void os_TermInput()
{
}

void UpdateInputState()
{
}

void os_DoEvents()
{
}

void os_CreateWindow()
{
}
#endif

void os_LaunchFromURL(const std::string& url)
{
}

std::string os_FetchStringFromURL(const std::string& url)
{
    std::string empty;
    return empty;
}

int os_UploadFilesToURL(const std::string& url, const std::vector<UploadField>& fields)
{
	//Not implemented
	return 501;
}

std::string os_GetMachineID()
{
    std::string empty;
    return empty;
}

std::string os_GetConnectionMedium()
{
    std::string empty;
    return empty;
}


#ifdef _WIN32
#include <windows.h>

static LARGE_INTEGER qpf;
static double  qpfd;
//Helper functions
double os_GetSeconds()
{
	static bool initme = (QueryPerformanceFrequency(&qpf), qpfd=1/(double)qpf.QuadPart);
	LARGE_INTEGER time_now;

	QueryPerformanceCounter(&time_now);
	static LARGE_INTEGER time_now_base = time_now;
	return (time_now.QuadPart - time_now_base.QuadPart)*qpfd;
}
#endif
