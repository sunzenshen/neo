// NEO volumetric smoke - fill jobs on a worker pool. The client and the
// server each run their fills on a pool of their own, so neither the frame
// nor the tick pays for a fill.
//
// A fill job traces the world only. The map must stay loaded while a job
// runs: the owner joins the job before the fill goes, and all of them
// before the level shuts down.

#ifndef NEO_SMOKE_JOBS_H
#define NEO_SMOKE_JOBS_H
#ifdef _WIN32
#pragma once
#endif

#include "tier0/threadtools.h"

class CJob;
class IThreadPool;
class CNeoSmokeFill;

// A job works in chunks this size, so a cancel waits for one chunk at most.
static constexpr int NEO_SMOKE_FILL_CHUNK_CELLS = 64;
static constexpr int NEO_SMOKE_FILL_CHUNK_QUERIES = 128;

// The pool of this DLL, started on the first call with up to `maxWorkers`
// threads. NULL when it cannot start: the caller then fills on its own
// thread.
IThreadPool *NEOSmokeFillPool( int maxWorkers );
void NEOSmokeFillPoolShutdown();

// Steps the fill until it is done or cancelled. Runs on a worker.
void NEOSmokeFillRun( CNeoSmokeFill &fill, const CInterlockedInt &cancel );

// Waits for the job through the fill pool, releases it and clears the
// pointer. The caller sets its cancel flag first.
void NEOSmokeFillJoin( CJob *&pJob );

#endif // NEO_SMOKE_JOBS_H
