/**
 * WorkScheduler.cpp
 * Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Support/WorkScheduler.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

WorkSignal mainThreadWork;

WorkSignal::WorkSignal() {
#ifdef _WIN32
	wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	// Condition-variable timeouts on Windows can round a 16.7 ms frame up to
	// 31 ms. This timer blocks precisely without a spin tail or a process-wide
	// timer-resolution change. Older systems retain the portable fallback.
	timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                               TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
}

WorkSignal::~WorkSignal() {
#ifdef _WIN32
	if (timer)
		CloseHandle(timer);
	if (wakeEvent)
		CloseHandle(wakeEvent);
#endif
}

void WorkSignal::notify() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		++generation;
#ifdef _WIN32
		if (wakeEvent)
			SetEvent(wakeEvent);
#endif
	}
	condition.notify_one();
}

bool WorkSignal::waitUntil(uint64_t observed, uint64_t deadline) {
	std::unique_lock<std::mutex> lock(mutex);
	if (generation != observed)
		return true;
#ifdef _WIN32
	const uint64_t now = WorkSchedule::now();
	if (now >= deadline)
		return false;
	if (timer && wakeEvent) {
		ResetEvent(wakeEvent);
		LARGE_INTEGER due;
		due.QuadPart = -static_cast<LONGLONG>((deadline - now + 99) / 100);
		if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
			const HANDLE handles[]{wakeEvent, timer};
			lock.unlock();
			const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
			lock.lock();
			if (result != WAIT_FAILED)
				return generation != observed;
		}
	}
#endif
	const auto end = std::chrono::steady_clock::time_point(
	    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(deadline)));
	return condition.wait_until(lock, end, [&] { return generation != observed; });
}
