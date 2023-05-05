/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui_driver.h"
#include "profiler/fc_profiler.h"
#include "network/ggpo.h"

#include <chrono>
#include <thread>
#include "sleep.h"
#include "../gdxsv/gdxsv_emu_hooks.h"

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

void UpdateInputState();

int64_t get_period() {
	const auto mode = config::FixedFrequency.get();
	// Native NTSC/VGA
	if (mode == 2 ||
		(mode == 1 && (config::Cable == 0 || config::Cable == 1)) ||
		(mode == 1 && config::Cable == 3 && (config::Broadcast == 0 || config::Broadcast == 4)))
		return 16683; // 1/59.94
	// Approximate VGA
	if (mode == 3) return 16666; // 1/60
	// PAL
	if (mode == 4 || (mode == 1 && config::Cable == 3))
		return 20000; // 1/50
	// Half Native NTSC/VGA
	if (mode == 5) return 33333; // 1/30
	return 16683;
}

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	UpdateInputState();

	if (gui_is_open() || gui_state == GuiState::VJoyEdit)
	{
		gui_display_ui();
		// TODO refactor android vjoy out of renderer
		if (gui_state == GuiState::VJoyEdit && renderer != nullptr)
			renderer->DrawOSD(true);
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const FlycastException& e) {
			emu.unloadGame();
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error("Renderer initialization failed.\nPlease select a different graphics API");
	}
}

void mainui_term()
{
	rend_term_renderer();
}

void mainui_loop()
{
	mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;
	int currentDupeFrames = config::DupeFrames;

	set_timer_resolution();
	std::chrono::time_point<std::chrono::steady_clock> start;
	auto fixedFrequencyWait = [&start]() {
		if (!config::FixedFrequency || gui_is_open() || settings.input.fastForwardMode)
			return;

		const auto period = get_period();
		const auto deltaUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		int64_t overSlept = 0;
		if (deltaUs < period)
			overSlept = sleep_and_busy_wait(period - deltaUs);
		start = std::chrono::steady_clock::now();
		if (1000 <= overSlept)
			WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", overSlept);
	};

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");

		if (mainui_rend_frame())
			fixedFrequencyWait();

		if (imguiDriver == nullptr)
			forceReinit = true;
		else
			imguiDriver->present();

		if (ggpo::active() && MainFrameCount % 60 == 0 && 0 < ggpo::timeSyncFrames) {
			ggpo::timeSyncFrames.fetch_sub(1);
			fixedFrequencyWait();
		}

		if (currentDupeFrames != config::DupeFrames) {
			forceReinit = true;
			currentDupeFrames = config::DupeFrames;
		}

		if (config::RendererType != currentRenderer || forceReinit)
		{
			mainui_term();
			int prevApi = isOpenGL(currentRenderer) ? 0 : isVulkan(currentRenderer) ? 1 : currentRenderer == RenderType::DirectX9 ? 2 : 3;
			int newApi = isOpenGL(config::RendererType) ? 0 : isVulkan(config::RendererType) ? 1 : config::RendererType == RenderType::DirectX9 ? 2 : 3;
			if (newApi != prevApi || forceReinit)
				switchRenderApi();
			mainui_init();
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		gdxsv_emu_mainui_loop();

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	reset_timer_resolution();
	mainui_term();
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
