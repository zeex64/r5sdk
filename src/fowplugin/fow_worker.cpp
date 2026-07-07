#include "core/stdafx.h"
#include "fow_worker.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
std::mutex g_fowWorkerMutex;
std::condition_variable g_fowWorkerCv;
std::thread g_fowWorkerThread;
bool g_fowWorkerStarted;
bool g_fowWorkerStop;
bool g_fowWorkerPending;
bool g_fowWorkerReady;
unsigned long long g_fowWorkerSubmitted;
unsigned long long g_fowWorkerCompleted;
unsigned long long g_fowWorkerConsumed;
FOWWorkerBuildCallback g_fowWorkerBuildCallback;
std::vector<unsigned char> g_fowWorkerPendingSnapshot;
std::vector<unsigned char> g_fowWorkerResultSnapshot;

void WorkerMain()
{
	for (;;)
	{
		std::vector<unsigned char> snapshot;
		FOWWorkerBuildCallback buildCallback = nullptr;
		{
			std::unique_lock<std::mutex> lock(g_fowWorkerMutex);
			g_fowWorkerCv.wait(lock, [] { return g_fowWorkerStop || g_fowWorkerPending; });
			if (g_fowWorkerStop)
				return;

			snapshot = g_fowWorkerPendingSnapshot;
			buildCallback = g_fowWorkerBuildCallback;
			g_fowWorkerPending = false;
		}

		if (buildCallback && !snapshot.empty())
			buildCallback(snapshot.data());

		std::lock_guard<std::mutex> lock(g_fowWorkerMutex);
		g_fowWorkerResultSnapshot = std::move(snapshot);
		g_fowWorkerReady = true;
		++g_fowWorkerCompleted;
	}
}

void WorkerEnsureStarted()
{
	if (g_fowWorkerStarted)
		return;

	std::lock_guard<std::mutex> lock(g_fowWorkerMutex);
	if (g_fowWorkerStarted)
		return;

	g_fowWorkerStop = false;
	g_fowWorkerPending = false;
	g_fowWorkerReady = false;
	g_fowWorkerSubmitted = 0;
	g_fowWorkerCompleted = 0;
	g_fowWorkerConsumed = 0;
	g_fowWorkerBuildCallback = nullptr;
	g_fowWorkerThread = std::thread(WorkerMain);
	g_fowWorkerStarted = true;
}

struct FOWWorkerShutdown
{
	~FOWWorkerShutdown()
	{
		WorkerStop();
	}
};

FOWWorkerShutdown g_fowWorkerShutdown;
}

void WorkerSubmit(const void* snapshotData, size_t snapshotSize, FOWWorkerBuildCallback buildCallback)
{
	if (!snapshotData || !snapshotSize || !buildCallback)
		return;

	WorkerEnsureStarted();

	std::lock_guard<std::mutex> lock(g_fowWorkerMutex);
	g_fowWorkerPendingSnapshot.resize(snapshotSize);
	memcpy(g_fowWorkerPendingSnapshot.data(), snapshotData, snapshotSize);
	g_fowWorkerBuildCallback = buildCallback;
	g_fowWorkerPending = true;
	++g_fowWorkerSubmitted;
	g_fowWorkerCv.notify_one();
}

bool WorkerTryConsumeResult(void* outSnapshotData, size_t snapshotSize)
{
	if (!outSnapshotData || !snapshotSize)
		return false;

	std::lock_guard<std::mutex> lock(g_fowWorkerMutex);
	if (!g_fowWorkerReady || g_fowWorkerResultSnapshot.size() != snapshotSize)
		return false;

	memcpy(outSnapshotData, g_fowWorkerResultSnapshot.data(), snapshotSize);
	g_fowWorkerReady = false;
	++g_fowWorkerConsumed;
	return true;
}

FOWWorkerStats WorkerGetStats()
{
	std::lock_guard<std::mutex> lock(g_fowWorkerMutex);
	return {
		g_fowWorkerStarted,
		g_fowWorkerPending,
		g_fowWorkerReady,
		g_fowWorkerSubmitted,
		g_fowWorkerCompleted,
		g_fowWorkerConsumed
	};
}

void WorkerStop()
{
	std::unique_lock<std::mutex> lock(g_fowWorkerMutex);
	if (!g_fowWorkerStarted)
		return;

	g_fowWorkerStop = true;
	lock.unlock();
	g_fowWorkerCv.notify_one();

	if (g_fowWorkerThread.joinable())
		g_fowWorkerThread.join();

	lock.lock();
	g_fowWorkerStarted = false;
	g_fowWorkerPending = false;
	g_fowWorkerReady = false;
	g_fowWorkerBuildCallback = nullptr;
	g_fowWorkerPendingSnapshot.clear();
	g_fowWorkerResultSnapshot.clear();
}