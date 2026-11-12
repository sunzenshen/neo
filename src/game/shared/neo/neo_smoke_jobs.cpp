// NEO volumetric smoke - fill jobs on a worker pool. See neo_smoke_jobs.h.
//
// The SDK traces the world from pool threads too (threaded particle
// simulation on the client, the query cache and nav analysis on the server),
// though it waits for those jobs within the frame; it does not document
// traces running while the main thread is inside the engine.

#include "cbase.h"
#include "neo_smoke_jobs.h"
#include "neo_smoke_fill.h"
#include "vstdlib/jobthread.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static IThreadPool *s_pSmokeFillPool = NULL;
static bool s_bSmokeFillPoolTried = false;

static int SmokeFillWorkers()
{
	// With SMT the sibling threads absorb the main, render and sound threads.
	// Without it, half the cores stay free for them.
	const CPUInformation *pCpu = GetCPUInformation();
	const int cores = pCpu->m_nPhysicalProcessors;
	const bool smt = pCpu->m_nLogicalProcessors > pCpu->m_nPhysicalProcessors;
	const int workers = smt ? cores - 1 : cores / 2;
	return clamp( workers, 1, NEO_SMOKE_FILL_MAX_WORKERS );
}

IThreadPool *NEOSmokeFillPool()
{
	if ( s_bSmokeFillPoolTried )
	{
		return s_pSmokeFillPool;
	}

	s_bSmokeFillPoolTried = true;
	IThreadPool *pPool = CreateThreadPool();
	ThreadPoolStartParams_t params;
	params.nThreads = SmokeFillWorkers();
	params.nThreadsMax = params.nThreads;
	params.fDistribute = TRS_FALSE;
	params.iThreadPriority = TP_PRIORITY_LOW;
	if ( !pPool || !pPool->Start( params ) || pPool->NumThreads() < 1 )
	{
		Warning( "neo_smoke: no fill workers, filling on the main thread\n" );
		if ( pPool )
		{
			DestroyThreadPool( pPool );
		}

		return NULL;
	}

	DevMsg( "neo_smoke: %d fill workers\n", pPool->NumThreads() );
	s_pSmokeFillPool = pPool;
	return s_pSmokeFillPool;
}

// The pool stays marked as tried, so a later fill runs on the main thread
// instead of on a dead pool.
void NEOSmokeFillPoolShutdown()
{
	if ( !s_pSmokeFillPool )
	{
		return;
	}

	s_pSmokeFillPool->Stop();
	DestroyThreadPool( s_pSmokeFillPool );
	s_pSmokeFillPool = NULL;
}

void NEOSmokeFillRun( CNEOSmokeFill &fill, const CInterlockedInt &cancel )
{
	while ( !fill.IsDone() && cancel == 0 )
	{
		fill.Step( NEO_SMOKE_FILL_CHUNK_CELLS, NEO_SMOKE_FILL_CHUNK_QUERIES );
	}
}

void NEOSmokeFillJoin( CJob *&pJob )
{
	if ( !pJob )
	{
		return;
	}

	// CJob::WaitForFinish waits through the global pool, not the one that
	// runs the job.
	if ( s_pSmokeFillPool )
	{
		s_pSmokeFillPool->YieldWait( pJob );
	}
	else
	{
		pJob->WaitForFinish();
	}

	pJob->Release();
	pJob = NULL;
}
