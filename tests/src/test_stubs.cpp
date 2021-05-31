#include <csignal>
#include <unistd.h>
#include "types.h"

void os_DebugBreak()
{
#ifdef __linux__
	raise(SIGTRAP);
#elif defined(_WIN32)
	__debugbreak();
#endif
}

void* libPvr_GetRenderTarget()
{
	return nullptr;
}

void os_SetupInput()
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

void os_LaunchFromURL(const std::string& url)
{
}

std::string os_FetchStringFromURL(const std::string& url)
{
    std::string empty;
    return empty;
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
void DestroyMainWindow()
{
}
#endif
