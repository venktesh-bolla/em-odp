/*
 *   Copyright (c) 2012, Nokia Siemens Networks
 *   Copyright (c) 2019, Nokia Solutions and Networks
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *
 * Event Machine performance test example
 *
 * Measures the average cycles consumed during an event send-sched-receive loop
 * for a certain number of EO pairs in the system. Test has a number of EO
 * pairs, that send ping-pong events. Depending on test dynamics (e.g. single
 * burst in atomic queue) only one EO of a pair might be active at a time.
 *
 * Uses three different queue priority levels that affect scheduling (might
 * starve low prio queues if using a strict prio scheduler).
 */

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include <event_machine.h>
#include <event_machine/platform/env/environment.h>

#include "cm_setup.h"
#include "cm_error_handler.h"

/*
 * Test configuration
 */

/** Number of test EOs and queues. Must be an even number. */
#define NUM_EO  128

/** Number of events per queue */
#define NUM_EVENT_PER_QUEUE  8  /* Increase the value to tune performance */

/** Number of ping-pong events per EO pair */
#define NUM_EVENT  (2 * NUM_EVENT_PER_QUEUE)

/** sizeof data[DATA_SIZE] in bytes in the event payload */
#define DATA_SIZE  250

/** Max number of cores */
#define MAX_NBR_OF_CORES  256

/** The number of events to be received before printing a result */
#define PRINT_EVENT_COUNT  0xff0000

/** Print results on all cores */
#define PRINT_ON_ALL_CORES  1 /* 0=False or 1=True */

/** EM Queue type used */
#define QUEUE_TYPE  EM_QUEUE_TYPE_ATOMIC

/*
 * Per event processing options
 */

/** Alloc and free per event */
#define ALLOC_FREE_PER_EVENT  0 /* 0=False or 1=True */

/** memcpy per event */
#define MEMCPY_PER_EVENT  0 /* 0=False or 1=True */

/** Check sequence numbers, works only with atomic queues */
#define CHECK_SEQ_PER_EVENT  0 /* 0=False or 1=True */

/* Result APPL_PRINT() format string */
#define RESULT_PRINTF_FMT \
"cycles/event:% -8.2f  Mevents/s/core: %-6.2f %5.0f MHz  core%02d %" PRIu64 "\n"

/**
 * Performance test statistics (per core)
 */
typedef union {
	uint8_t u8[ENV_CACHE_LINE_SIZE] ENV_CACHE_LINE_ALIGNED;
	struct {
		uint64_t events;
		uint64_t begin_cycles;
		uint64_t end_cycles;
		uint64_t print_count;
	};
} perf_stat_t;

COMPILE_TIME_ASSERT(sizeof(perf_stat_t) == ENV_CACHE_LINE_SIZE,
		    PERF_STAT_T_SIZE_ERROR);

/**
 * Performance test EO context
 */
typedef struct {
	/* Next destination queue */
	em_queue_t dest;
	/** EO's own queue*/
	em_queue_t queue;
	/** EO handle */
	em_eo_t eo;
	/** Next sequence number (used with CHECK_SEQ_PER_EVENT) */
	int next_seq;
} eo_context_t;

/**
 * EO context padded to cache line size
 */
typedef union {
	uint8_t u8[ENV_CACHE_LINE_SIZE] ENV_CACHE_LINE_ALIGNED;
	eo_context_t eo_ctx;
} eo_context_array_elem_t;

COMPILE_TIME_ASSERT(sizeof(eo_context_array_elem_t) == ENV_CACHE_LINE_SIZE,
		    PERF_EO_CONTEXT_SIZE_ERROR);

/**
 * Performance test event
 */
typedef struct {
	/* Sequence number */
	int seq;
	/* Test data */
	uint8_t data[DATA_SIZE];
} perf_event_t;

/**
 * Perf test shared memory
 */
typedef struct {
	/* Event pool used by this application */
	em_pool_t pool ENV_CACHE_LINE_ALIGNED;
	/* EO context array */
	eo_context_array_elem_t perf_eo_context[NUM_EO] ENV_CACHE_LINE_ALIGNED;
	/* Array of core specific data accessed by using core index. */
	perf_stat_t core_stat[MAX_NBR_OF_CORES] ENV_CACHE_LINE_ALIGNED;
} perf_shm_t;

/** EM-core local pointer to shared memory */
static ENV_LOCAL perf_shm_t *perf_shm;

/*
 * Local function prototypes
 */

static em_status_t
perf_start(void *eo_context, em_eo_t eo, const em_eo_conf_t *conf);

static em_status_t
perf_stop(void *eo_context, em_eo_t eo);

static void
perf_receive(void *eo_context, em_event_t event, em_event_type_t type,
	     em_queue_t queue, void *q_ctx);

static void
print_result(perf_stat_t *const perf_stat);

static int
get_queue_priority(const int index);

/**
 * Main function
 *
 * Call cm_setup() to perform test & EM setup common for all the
 * test applications.
 *
 * cm_setup() will call test_init() and test_start() and launch
 * the EM dispatch loop on every EM-core.
 */
int main(int argc, char *argv[])
{
	return cm_setup(argc, argv);
}

/**
 * Init of the Pairs performance test application.
 *
 * @attention Run on all cores.
 *
 * @see cm_setup() for setup and dispatch.
 */
void
test_init(void)
{
	int core = em_core_id();

	if (core == 0) {
		perf_shm = env_shared_reserve("PerfSharedMem",
					      sizeof(perf_shm_t));
		em_register_error_handler(test_error_handler);
	} else {
		perf_shm = env_shared_lookup("PerfSharedMem");
	}

	if (perf_shm == NULL)
		test_error(EM_ERROR_SET_FATAL(0xec0de), 0xdead,
			   "Perf init failed on EM-core:%u", em_core_id());
	else if (core == 0)
		memset(perf_shm, 0, sizeof(perf_shm_t));
}

/**
 * Startup of the Pairs performance test application.
 *
 * @attention Run only on EM core 0.
 *
 * @param appl_conf Application configuration
 *
 * @see cm_setup() for setup and dispatch.
 */
void
test_start(appl_conf_t *const appl_conf)
{
	/*
	 * Store the event pool to use, use the EM default pool if no other
	 * pool is provided through the appl_conf.
	 */
	if (appl_conf->num_pools >= 1)
		perf_shm->pool = appl_conf->pools[0];
	else
		perf_shm->pool = EM_POOL_DEFAULT;

	APPL_PRINT("\n"
		   "***********************************************************\n"
		   "EM APPLICATION: '%s' initializing:\n"
		   "  %s: %s() - EM-core:%i\n"
		   "  Application running on %d EM-cores (procs:%d, threads:%d)\n"
		   "  using event pool:%" PRI_POOL "\n"
		   "***********************************************************\n"
		   "\n",
		   appl_conf->name, NO_PATH(__FILE__), __func__, em_core_id(),
		   em_core_count(),
		   appl_conf->num_procs, appl_conf->num_threads,
		   perf_shm->pool);

	test_fatal_if(perf_shm->pool == EM_POOL_UNDEF,
		      "Undefined application event pool!");

	/*
	 * Create and start application pairs.
	 * Send initial test events to the queues.
	 */
	for (int i = 0; i < NUM_EO / 2; i++) {
		em_queue_t queue_a, queue_b;
		eo_context_t *eo_ctx_a, *eo_ctx_b;
		em_eo_t eo;
		em_status_t ret, start_ret = EM_OK;

		/* Create both queues for the pair */
		queue_a = em_queue_create("queue-A", QUEUE_TYPE,
					  get_queue_priority(i),
					  EM_QUEUE_GROUP_DEFAULT, NULL);
		queue_b = em_queue_create("queue-B", QUEUE_TYPE,
					  get_queue_priority(i),
					  EM_QUEUE_GROUP_DEFAULT, NULL);

		/* Create EO "A" */
		eo_ctx_a = &perf_shm->perf_eo_context[2 * i].eo_ctx;

		eo = em_eo_create("pairs-eo-a", perf_start, NULL, perf_stop,
				  NULL, perf_receive, eo_ctx_a);
		test_fatal_if(eo == EM_EO_UNDEF,
			      "EO(%d) creation failed!", 2 * i);
		eo_ctx_a->dest = queue_b;
		eo_ctx_a->queue = queue_a;
		eo_ctx_a->eo = eo;
		eo_ctx_a->next_seq = 0;

		ret = em_eo_add_queue_sync(eo, queue_a);
		test_fatal_if(ret != EM_OK,
			      "EO add queue:%" PRI_STAT "\n"
			      "EO:%" PRI_EO " Queue:%" PRI_QUEUE "",
			      ret, eo, queue_a);

		ret = em_eo_start_sync(eo, &start_ret, NULL);
		test_fatal_if(ret != EM_OK || start_ret != EM_OK,
			      "EO start:%" PRI_STAT " %" PRI_STAT "",
			      ret, start_ret);

		/* Create EO "B" */
		eo_ctx_b = &perf_shm->perf_eo_context[2 * i + 1].eo_ctx;

		eo = em_eo_create("pairs-eo-b", perf_start, NULL, perf_stop,
				  NULL, perf_receive, eo_ctx_b);

		eo_ctx_b->dest = queue_a;
		eo_ctx_b->queue = queue_b;
		eo_ctx_b->eo = eo;
		eo_ctx_b->next_seq = NUM_EVENT / 2;

		ret = em_eo_add_queue_sync(eo, queue_b);
		test_fatal_if(ret != EM_OK,
			      "EO add queue:%" PRI_STAT "\n"
			      "EO:%" PRI_EO " queue:%" PRI_QUEUE "",
			      ret, eo, queue_b);

		ret = em_eo_start_sync(eo, &start_ret, NULL);
		test_fatal_if(ret != EM_OK || start_ret != EM_OK,
			      "EO start:%" PRI_STAT " %" PRI_STAT "",
			      ret, start_ret);

		/* Alloc and send test events */
		for (int j = 0; j < NUM_EVENT; j++) {
			em_event_t event;
			perf_event_t *perf_ev;
			em_queue_t first_q;

			event = em_alloc(sizeof(perf_event_t),
					 EM_EVENT_TYPE_SW, perf_shm->pool);
			test_fatal_if(event == EM_EVENT_UNDEF,
				      "Event allocation failed (%i, %i)", i, j);

			perf_ev = em_event_pointer(event);

			if (j & 0x1) {
				/* Odd: j = 1, 3, 5, ... */
				perf_ev->seq = NUM_EVENT / 2 + j / 2;
				first_q = queue_b;
			} else {
				/* Even: j = 0, 2, 4, ... */
				perf_ev->seq = j / 2;
				first_q = queue_a;
			}

			ret = em_send(event, first_q);
			test_fatal_if(ret != EM_OK,
				      "Send:%" PRI_STAT " Q:%" PRI_QUEUE "",
				      ret, first_q);
		}
	}
	env_sync_mem();
}

void
test_stop(appl_conf_t *const appl_conf)
{
	const int core = em_core_id();
	eo_context_t *eo_ctx;
	em_eo_t eo;
	em_status_t ret;
	int i;

	(void)appl_conf;

	APPL_PRINT("%s() on EM-core %d\n", __func__, core);

	for (i = 0; i < NUM_EO; i++) {
		/* Stop & delete EO */
		eo_ctx = &perf_shm->perf_eo_context[i].eo_ctx;
		eo = eo_ctx->eo;

		ret = em_eo_stop_sync(eo);
		test_fatal_if(ret != EM_OK,
			      "EO:%" PRI_EO " stop:%" PRI_STAT "", eo, ret);

		ret = em_eo_delete(eo);
		test_fatal_if(ret != EM_OK,
			      "EO:%" PRI_EO " delete:%" PRI_STAT "", eo, ret);
	}
}

void
test_term(void)
{
	const int core = em_core_id();

	APPL_PRINT("%s() on EM-core %d\n", __func__, core);

	if (core == 0) {
		env_shared_free(perf_shm);
		em_unregister_error_handler();
	}
}

/**
 * @private
 *
 * EO start function.
 *
 */
static em_status_t
perf_start(void *eo_context, em_eo_t eo, const em_eo_conf_t *conf)
{
	(void)eo_context;
	(void)eo;
	(void)conf;

	return EM_OK;
}

/**
 * @private
 *
 * EO stop function.
 *
 */
static em_status_t
perf_stop(void *eo_context, em_eo_t eo)
{
	em_status_t ret;

	(void)eo_context;

	/* remove and delete all of the EO's queues */
	ret = em_eo_remove_queue_all_sync(eo, EM_TRUE);
	test_fatal_if(ret != EM_OK,
		      "EO remove queue all:%" PRI_STAT " EO:%" PRI_EO "",
		      ret, eo);
	return ret;
}

/**
 * @private
 *
 * EO receive function for EO A.
 *
 * Loops back events and calculates the event rate.
 */
static void
perf_receive(void *eo_context, em_event_t event, em_event_type_t type,
	     em_queue_t queue, void *q_ctx)
{
	const int core = em_core_id();
	uint64_t events = perf_shm->core_stat[core].events;
	perf_event_t *perf_ev;
	eo_context_t *const eo_ctx = eo_context;
	const em_queue_t dst_queue = eo_ctx->dest;

	em_status_t ret;
	int seq;

	(void)type;
	(void)q_ctx;

	if (unlikely(appl_shm->exit_flag)) {
		em_free(event);
		return;
	}

	if (CHECK_SEQ_PER_EVENT || MEMCPY_PER_EVENT)
		perf_ev = em_event_pointer(event);

	/* Update the cycle count and print results when necessary */
	if (unlikely(events == 0)) {
		perf_shm->core_stat[core].begin_cycles = env_get_cycle();
		events += 1;
	} else if (unlikely(events >= PRINT_EVENT_COUNT)) {
		perf_shm->core_stat[core].end_cycles = env_get_cycle();
		perf_shm->core_stat[core].print_count += 1;

		/* Print measurement result */
		if (PRINT_ON_ALL_CORES)
			print_result(&perf_shm->core_stat[core]);
		else if (core == 0)
			print_result(&perf_shm->core_stat[core]);
		/* Restart the measurement */
		perf_shm->core_stat[core].begin_cycles = env_get_cycle();
		events = 0;
	} else {
		events += 1;
	}

	if (CHECK_SEQ_PER_EVENT) {
		eo_context_t *const eo_ctx = eo_context;

		seq = perf_ev->seq;
		if (unlikely(seq != eo_ctx->next_seq))
			APPL_PRINT("Bad sequence number. EO(A) %" PRI_EO ",\t"
				   "Q:%" PRI_QUEUE " expected:%i eventseq:%i\n",
				   eo_ctx->eo, queue, eo_ctx->next_seq, seq);

		if (likely(eo_ctx->next_seq < (NUM_EVENT - 1)))
			eo_ctx->next_seq++;
		else
			eo_ctx->next_seq = 0;
	}

	if (MEMCPY_PER_EVENT) {
		uint8_t *const from = &perf_ev->data[0];
		uint8_t *const to = &perf_ev->data[DATA_SIZE / 2];

		memcpy(to, from, DATA_SIZE / 2);
	}

	if (ALLOC_FREE_PER_EVENT) {
		em_free(event);
		event = em_alloc(sizeof(perf_event_t), EM_EVENT_TYPE_SW,
				 perf_shm->pool);
		test_fatal_if(event == EM_EVENT_UNDEF, "Event alloc fails");
		perf_ev = em_event_pointer(event);
		if (CHECK_SEQ_PER_EVENT)
			perf_ev->seq = seq;
	}

	ret = em_send(event, dst_queue);
	if (unlikely(ret != EM_OK)) {
		em_free(event);
		test_fatal_if(!appl_shm->exit_flag,
			      "Send:%" PRI_STAT " Queue:%" PRI_QUEUE "",
			      ret, dst_queue);
	}

	perf_shm->core_stat[core].events = events;
}

/**
 * Get queue priority value based on the index number.
 *
 * @param Queue index
 *
 * @return Queue priority value
 *
 * @note Priority distribution: 40% LOW, 40% NORMAL, 20% HIGH
 */
static int
get_queue_priority(const int queue_index)
{
	int remainder = queue_index % 5;

	if (remainder <= 1)
		return EM_QUEUE_PRIO_LOW;
	else if (remainder <= 3)
		return EM_QUEUE_PRIO_NORMAL;
	else
		return EM_QUEUE_PRIO_HIGH;
}

/**
 * Prints test measurement result
 */
static void
print_result(perf_stat_t *const perf_stat)
{
	uint64_t diff;
	uint32_t hz;
	double mhz;
	double cycles_per_event, events_per_sec;
	uint64_t print_count;

	hz = env_core_hz();
	mhz = ((double)hz) / 1000000.0;

	if (perf_stat->end_cycles > perf_stat->begin_cycles)
		diff = perf_stat->end_cycles - perf_stat->begin_cycles;
	else
		diff = UINT64_MAX - perf_stat->begin_cycles +
			perf_stat->end_cycles + 1;

	print_count = perf_stat->print_count;
	cycles_per_event = ((double)diff) / ((double)perf_stat->events);
	events_per_sec = mhz / cycles_per_event; /* Million events/s */

	APPL_PRINT(RESULT_PRINTF_FMT, cycles_per_event, events_per_sec,
		   mhz, em_core_id(), print_count);
}
