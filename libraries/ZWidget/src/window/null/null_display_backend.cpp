#include "null_display_backend.h"
#include <thread>

std::unique_ptr<DisplayWindow> NullDisplayBackend::Create(DisplayWindowHost* windowHost, bool popupWindow, DisplayWindow* owner, RenderAPI renderAPI)
{
	return nullptr;
}

void NullDisplayBackend::ProcessEvents()
{
}

void NullDisplayBackend::RunLoop()
{
	exitRunLoop = false;
	while (!exitRunLoop)
	{
		auto now = std::chrono::steady_clock::now();
		for (auto& [id, entry] : timers)
		{
			if (now >= entry.nextFire)
			{
				entry.callback();
				entry.nextFire = now + std::chrono::milliseconds(entry.intervalMs);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

void NullDisplayBackend::ExitLoop()
{
	exitRunLoop = true;
}

void* NullDisplayBackend::StartTimer(int timeoutMilliseconds, std::function<void()> onTimer)
{
	int id = nextTimerID++;
	TimerEntry entry;
	entry.intervalMs = timeoutMilliseconds;
	entry.callback = std::move(onTimer);
	entry.nextFire = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	timers[id] = std::move(entry);
	return reinterpret_cast<void*>(static_cast<intptr_t>(id));
}

void NullDisplayBackend::StopTimer(void* timerID)
{
	int id = static_cast<int>(reinterpret_cast<intptr_t>(timerID));
	timers.erase(id);
}

Size NullDisplayBackend::GetScreenSize()
{
	return Size(640, 480);
}
