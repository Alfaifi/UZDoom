#include "null_display_backend.h"
#include <thread>
#include <vector>

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

		// Collect ready timer IDs first — callbacks may call StopTimer()
		// which erases from the map, invalidating iterators.
		std::vector<int> ready;
		for (auto& [id, entry] : timers)
		{
			if (now >= entry.nextFire)
				ready.push_back(id);
		}
		for (int id : ready)
		{
			auto it = timers.find(id);
			if (it != timers.end())
			{
				it->second.callback();
				// Re-lookup: callback may have stopped this or other timers.
				it = timers.find(id);
				if (it != timers.end())
					it->second.nextFire = now + std::chrono::milliseconds(it->second.intervalMs);
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
