#ifndef FOW_WORKER_H
#define FOW_WORKER_H

#include <cstddef>

typedef void(*FOWWorkerBuildCallback)(void* snapshotData);

struct FOWWorkerStats
{
	bool started;
	bool pending;
	bool ready;
	unsigned long long submitted;
	unsigned long long completed;
	unsigned long long consumed;
};

void WorkerSubmit(const void* snapshotData, size_t snapshotSize, FOWWorkerBuildCallback buildCallback);
bool WorkerTryConsumeResult(void* outSnapshotData, size_t snapshotSize);
FOWWorkerStats WorkerGetStats();
void WorkerStop();

#endif // FOW_WORKER_H