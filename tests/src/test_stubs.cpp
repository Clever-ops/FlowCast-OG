#include <csignal>
#include <unistd.h>
#include "types.h"

// FIXME
void* x11_glc;

void os_DebugBreak()
{
	raise(SIGTRAP);
}

void* libPvr_GetRenderTarget()
{
	return nullptr;
}

void* libPvr_GetRenderSurface()
{
	return nullptr;
}

void os_SetupInput()
{
}

void UpdateInputState(u32 port)
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

int get_mic_data(u8* buffer)
{
	return 0;
}
