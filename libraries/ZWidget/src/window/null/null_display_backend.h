#pragma once

#include "window/window.h"
#include <atomic>
#include <map>
#include <chrono>

class NullDisplayBackend : public DisplayBackend
{
public:
	std::unique_ptr<DisplayWindow> Create(DisplayWindowHost* windowHost, bool popupWindow, DisplayWindow* owner, RenderAPI renderAPI) override;
	void ProcessEvents() override;
	void RunLoop() override;
	void ExitLoop() override;

	void* StartTimer(int timeoutMilliseconds, std::function<void()> onTimer) override;
	void StopTimer(void* timerID) override;

	Size GetScreenSize() override;

private:
	std::atomic<bool> exitRunLoop { false };
	int nextTimerID = 1;

	struct TimerEntry
	{
		int intervalMs;
		std::function<void()> callback;
		std::chrono::steady_clock::time_point nextFire;
	};
	std::map<int, TimerEntry> timers;
};
