// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (C) 2016 Gvozden Nešković. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/zio.h>
#include <umem.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <assert.h>
#include <stdio.h>
#include "raidz_test.h"

static int *rand_data;
raidz_test_opts_t rto_opts;

static char pid_s[16];

static void sig_handler(int signo)
{
	int old_errno = errno;
	struct sigaction action;
	/*
	 * Restore default action and re-raise signal so SIGSEGV and
	 * SIGABRT can trigger a core dump.
	 */
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void) sigaction(signo, &action, NULL);

	if (rto_opts.rto_gdb) {
		pid_t pid = fork();
		if (pid == 0) {
			execlp("gdb", "gdb", "-ex", "set pagination 0",
			    "-p", pid_s, NULL);
			_exit(-1);
		} else if (pid > 0)
			while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
				;
	}

	raise(signo);
	errno = old_errno;
}

static void print_opts(raidz_test_opts_t *opts, boolean_t force)
{
	const char *verbose;
	switch (opts->rto_v) {
		case D_ALL:
			verbose = "no";
			break;
		case D_INFO:
			verbose = "info";
			break;
		case D_DEBUG:
		default:
			verbose = "debug";
			break;
	}

	if (force || opts->rto_v >= D_INFO) {
		(void) fprintf(stdout, DBLSEP "Running with options:\n"
		    "  (-a) zio ashift                   : %zu\n"
		    "  (-o) zio offset                   : 1 << %zu\n"
		    "  (-e) expanded map                 : %s\n"
		    "  (-r) reflow offset                : %llx\n"
		    "  (-d) number of raidz data columns : %zu\n"
		    "  (-s) size of DATA                 : 1 << %zu\n"
		    "  (-S) sweep parameters             : %s \n"
		    "  (-v) verbose                      : %s \n\n",
		    opts->rto_ashift,				/* -a */
		    ilog2(opts->rto_offset),			/* -o */
		    opts->rto_expand ? "yes" : "no",		/* -e */
		    (u_longlong_t)opts->rto_expand_offset,	/* -r */
		    opts->rto_dcols,				/* -d */
		    ilog2(opts->rto_dsize),			/* -s */
		    opts->rto_sweep ? "yes" : "no",		/* -S */
		    verbose);					/* -v */
	}
}

static void usage(boolean_t requested)
{
	const raidz_test_opts_t *o = &rto_opts_defaults;

	FILE *fp = requested ? stdout : stderr;

	(void) fprintf(fp, "Usage:\n"
	    "\t[-a zio ashift (default: %zu)]\n"
	    "\t[-o zio offset, exponent radix 2 (default: %zu)]\n"
	    "\t[-d number of raidz data columns (default: %zu)]\n"
	    "\t[-s zio size, exponent radix 2 (default: %zu)]\n"
	    "\t[-S parameter sweep (default: %s)]\n"
	    "\t[-t timeout for parameter sweep test]\n"
	    "\t[-B benchmark all raidz implementations]\n"
	    "\t[-e use expanded raidz map (default: %s)]\n"
	    "\t[-r expanded raidz map reflow offset (default: %llx)]\n"
	    "\t[-v increase verbosity (default: %d)]\n"
	    "\t[-h (print help)]\n"
	    "\t[-T test the test, see if failure would be detected]\n"
	    "\t[-D debug (attach gdb on SIGSEGV)]\n"
	    "",
	    o->rto_ashift,				/* -a */
	    ilog2(o->rto_offset),			/* -o */
	    o->rto_dcols,				/* -d */
	    ilog2(o->rto_dsize),			/* -s */
	    rto_opts.rto_sweep ? "yes" : "no",		/* -S */
	    rto_opts.rto_expand ? "yes" : "no",		/* -e */
	    (u_longlong_t)o->rto_expand_offset,		/* -r */
	    o->rto_v);					/* -v */

	exit(requested ? 0 : 1);
}

static void process_options(int argc, char **argv)
{
	size_t value;
	int opt;
	raidz_test_opts_t *o = &rto_opts;

	memcpy(o, &rto_opts_defaults, sizeof (*o));

	while ((opt = getopt(argc, argv, "TDBSvha:er:o:d:s:t:")) != -1) {
		switch (opt) {
		case 'a':
			value = strtoull(optarg, NULL, 0);
			o->rto_ashift = MIN(13, MAX(9, value));
			break;
		case 'e':
			o->rto_expand = 1;
			break;
		case 'r':
			o->rto_expand_offset = strtoull(optarg, NULL, 0);
			break;
		case 'o':
			value = strtoull(optarg, NULL, 0);
			o->rto_offset = ((1ULL << MIN(12, value)) >> 9) << 9;
			break;
		case 'd':
			value = strtoull(optarg, NULL, 0);
			o->rto_dcols = MIN(255, MAX(1, value));
			break;
		case 's':
			value = strtoull(optarg, NULL, 0);
			o->rto_dsize = 1ULL <<  MIN(SPA_MAXBLOCKSHIFT,
			    MAX(SPA_MINBLOCKSHIFT, value));
			break;
		case 't':
			value = strtoull(optarg, NULL, 0);
			o->rto_sweep_timeout = value;
			break;
		case 'v':
			o->rto_v++;
			break;
		case 'S':
			o->rto_sweep = 1;
			break;
		case 'B':
			o->rto_benchmark = 1;
			break;
		case 'D':
			o->rto_gdb = 1;
			break;
		case 'T':
			o->rto_sanity = 1;
			break;
		case 'h':
			usage(B_TRUE);
			break;
		case '?':
		default:
			usage(B_FALSE);
			break;
		}
	}
}

#define	DATA_COL(rr, i) ((rr)->rr_col[rr->rr_firstdatacol + (i)].rc_abd)
#define	DATA_COL_SIZE(rr, i) ((rr)->rr_col[rr->rr_firstdatacol + (i)].rc_size)

#define	CODE_COL(rr, i) ((rr)->rr_col[(i)].rc_abd)
#define	CODE_COL_SIZE(rr, i) ((rr)->rr_col[(i)].rc_size)

static int
cmp_code(raidz_test_opts_t *opts, const raidz_map_t *rm, const int parity)
{
	int r, i, ret = 0;

	VERIFY(parity >= 1 && parity <= 3);

	for (r = 0; r < rm->rm_nrows; r++) {
		raidz_row_t * const rr = rm->rm_row[r];
		raidz_row_t * const rrg = opts->rm_golden->rm_row[r];
		for (i = 0; i < parity; i++) {
			if (CODE_COL_SIZE(rrg, i) == 0) {
				VERIFY0(CODE_COL_SIZE(rr, i));
				continue;
			}

			if (abd_cmp(CODE_COL(rr, i),
			    CODE_COL(rrg, i)) != 0) {
				ret++;
				LOG_OPT(D_DEBUG, opts,
				    "\nParity block [%d] different!\n", i);
			}
		}
	}
	return (ret);
}

static int
cmp_data(raidz_test_opts_t *opts, raidz_map_t *rm)
{
	int r, i, dcols, ret = 0;

	for (r = 0; r < rm->rm_nrows; r++) {
		raidz_row_t *rr = rm->rm_row[r];
		raidz_row_t *rrg = opts->rm_golden->rm_row[r];
		dcols = opts->rm_golden->rm_row[0]->rr_cols -
		    raidz_parity(opts->rm_golden);
		for (i = 0; i < dcols; i++) {
			if (DATA_COL_SIZE(rrg, i) == 0) {
				VERIFY0(DATA_COL_SIZE(rr, i));
				continue;
			}

			if (abd_cmp(DATA_COL(rrg, i),
			    DATA_COL(rr, i)) != 0) {
				ret++;

				LOG_OPT(D_DEBUG, opts,
				    "\nData block [%d] different!\n", i);
			}
		}
	}
	return (ret);
}

static int
init_rand(void *data, size_t size, void *private)
{
	(void) private;
	memcpy(data, rand_data, size);
	return (0);
}

static void
corrupt_colums(raidz_map_t *rm, const int *tgts, const int cnt)
{
	for (int r = 0; r < rm->rm_nrows; r++) {
		raidz_row_t *rr = rm->rm_row[r];
		for (int i = 0; i < cnt; i++) {
			raidz_col_t *col = &rr->rr_col[tgts[i]];
			abd_iterate_func(col->rc_abd, 0, col->rc_size,
			    init_rand, NULL);
		}
	}
}

void
init_zio_abd(zio_t *zio)
{
	abd_iterate_func(zio->io_abd, 0, zio->io_size, init_rand, NULL);
}

static void
fini_raidz_map(zio_t **zio, raidz_map_t **rm)
{
	vdev_raidz_map_free(*rm);
	raidz_free((*zio)->io_abd, (*zio)->io_size);
	umem_free(*zio, sizeof (zio_t));

	*zio = NULL;
	*rm = NULL;
}

static int
init_raidz_golden_map(raidz_test_opts_t *opts, const int parity)
{
	int err = 0;
	zio_t *zio_test;
	raidz_map_t *rm_test;
	const size_t total_ncols = opts->rto_dcols + parity;

	if (opts->rm_golden) {
		fini_raidz_map(&opts->zio_golden, &opts->rm_golden);
	}

	opts->zio_golden = umem_zalloc(sizeof (zio_t), UMEM_NOFAIL);
	zio_test = umem_zalloc(sizeof (zio_t), UMEM_NOFAIL);

	opts->zio_golden->io_offset = zio_test->io_offset = opts->rto_offset;
	opts->zio_golden->io_size = zio_test->io_size = opts->rto_dsize;

	opts->zio_golden->io_abd = raidz_alloc(opts->rto_dsize);
	zio_test->io_abd = raidz_alloc(opts->rto_dsize);

	init_zio_abd(opts->zio_golden);
	init_zio_abd(zio_test);

	VERIFY0(vdev_raidz_impl_set("original"));

	if (opts->rto_expand) {
		opts->rm_golden =
		    vdev_raidz_map_alloc_expanded(opts->zio_golden,
		    opts->rto_ashift, total_ncols+1, total_ncols,
		    parity, opts->rto_expand_offset, 0, B_FALSE);
		rm_test = vdev_raidz_map_alloc_expanded(zio_test,
		    opts->rto_ashift, total_ncols+1, total_ncols,
		    parity, opts->rto_expand_offset, 0, B_FALSE);
	} else {
		opts->rm_golden = vdev_raidz_map_alloc(opts->zio_golden,
		    opts->rto_ashift, total_ncols, parity);
		rm_test = vdev_raidz_map_alloc(zio_test,
		    opts->rto_ashift, total_ncols, parity);
	}

	VERIFY(opts->zio_golden);
	VERIFY(opts->rm_golden);

	vdev_raidz_generate_parity(opts->rm_golden);
	vdev_raidz_generate_parity(rm_test);

	/* sanity check */
	err |= cmp_data(opts, rm_test);
	err |= cmp_code(opts, rm_test, parity);

	if (err)
		ERR("initializing the golden copy ... [FAIL]!\n");

	/* tear down raidz_map of test zio */
	fini_raidz_map(&zio_test, &rm_test);

	return (err);
}

static raidz_map_t *
init_raidz_map(raidz_test_opts_t *opts, zio_t **zio, const int parity)
{
	raidz_map_t *rm = NULL;
	const size_t alloc_dsize = opts->rto_dsize;
	const size_t total_ncols = opts->rto_dcols + parity;
	const int ccols[] = { 0, 1, 2 };

	VERIFY(zio);
	VERIFY(parity <= 3 && parity >= 1);

	*zio = umem_zalloc(sizeof (zio_t), UMEM_NOFAIL);

	(*zio)->io_offset = 0;
	(*zio)->io_size = alloc_dsize;
	(*zio)->io_abd = raidz_alloc(alloc_dsize);
	init_zio_abd(*zio);

	if (opts->rto_expand) {
		rm = vdev_raidz_map_alloc_expanded(*zio,
		    opts->rto_ashift, total_ncols+1, total_ncols,
		    parity, opts->rto_expand_offset, 0, B_FALSE);
	} else {
		rm = vdev_raidz_map_alloc(*zio, opts->rto_ashift,
		    total_ncols, parity);
	}
	VERIFY(rm);

	/* Make sure code columns are destroyed */
	corrupt_colums(rm, ccols, parity);

	return (rm);
}

static int
run_gen_check(raidz_test_opts_t *opts)
{
	char **impl_name;
	int fn, err = 0;
	zio_t *zio_test;
	raidz_map_t *rm_test;

	err = init_raidz_golden_map(opts, PARITY_PQR);
	if (0 != err)
		return (err);

	LOG(D_INFO, DBLSEP);
	LOG(D_INFO, "Testing parity generation...\n");

	for (impl_name = (char **)raidz_impl_names+1; *impl_name != NULL;
	    impl_name++) {

		LOG(D_INFO, SEP);
		LOG(D_INFO, "\tTesting [%s] implementation...", *impl_name);

		if (0 != vdev_raidz_impl_set(*impl_name)) {
			LOG(D_INFO, "[SKIP]\n");
			continue;
		} else {
			LOG(D_INFO, "[SUPPORTED]\n");
		}

		for (fn = 0; fn < RAIDZ_GEN_NUM; fn++) {

			/* Check if should stop */
			if (rto_opts.rto_should_stop)
				return (err);

			/* create suitable raidz_map */
			rm_test = init_raidz_map(opts, &zio_test, fn+1);
			VERIFY(rm_test);

			LOG(D_INFO, "\t\tTesting method [%s] ...",
			    raidz_gen_name[fn]);

			if (!opts->rto_sanity)
				vdev_raidz_generate_parity(rm_test);

			if (cmp_code(opts, rm_test, fn+1) != 0) {
				LOG(D_INFO, "[FAIL]\n");
				err++;
			} else
				LOG(D_INFO, "[PASS]\n");

			fini_raidz_map(&zio_test, &rm_test);
		}
	}

	fini_raidz_map(&opts->zio_golden, &opts->rm_golden);

	return (err);
}

static int
run_rec_check_impl(raidz_test_opts_t *opts, raidz_map_t *rm, const int fn)
{
	int x0, x1, x2;
	int tgtidx[3];
	int err = 0;
	static const int rec_tgts[7][3] = {
		{1, 2, 3},	/* rec_p:   bad QR & D[0]	*/
		{0, 2, 3},	/* rec_q:   bad PR & D[0]	*/
		{0, 1, 3},	/* rec_r:   bad PQ & D[0]	*/
		{2, 3, 4},	/* rec_pq:  bad R  & D[0][1]	*/
		{1, 3, 4},	/* rec_pr:  bad Q  & D[0][1]	*/
		{0, 3, 4},	/* rec_qr:  bad P  & D[0][1]	*/
		{3, 4, 5}	/* rec_pqr: bad    & D[0][1][2] */
	};

	memcpy(tgtidx, rec_tgts[fn], sizeof (tgtidx));

	if (fn < RAIDZ_REC_PQ) {
		/* can reconstruct 1 failed data disk */
		for (x0 = 0; x0 < opts->rto_dcols; x0++) {
			if (x0 >= rm->rm_row[0]->rr_cols - raidz_parity(rm))
				continue;

			/* Check if should stop */
			if (rto_opts.rto_should_stop)
				return (err);

			LOG(D_DEBUG, "[%d] ", x0);

			tgtidx[2] = x0 + raidz_parity(rm);

			corrupt_colums(rm, tgtidx+2, 1);

			if (!opts->rto_sanity)
				vdev_raidz_reconstruct(rm, tgtidx, 3);

			if (cmp_data(opts, rm) != 0) {
				err++;
				LOG(D_DEBUG, "\nREC D[%d]... [FAIL]\n", x0);
			}
		}

	} else if (fn < RAIDZ_REC_PQR) {
		/* can reconstruct 2 failed data disk */
		for (x0 = 0; x0 < opts->rto_dcols; x0++) {
			if (x0 >= rm->rm_row[0]->rr_cols - raidz_parity(rm))
				continue;
			for (x1 = x0 + 1; x1 < opts->rto_dcols; x1++) {
				if (x1 >= rm->rm_row[0]->rr_cols -
				    raidz_parity(rm))
					continue;

				/* Check if should stop */
				if (rto_opts.rto_should_stop)
					return (err);

				LOG(D_DEBUG, "[%d %d] ", x0, x1);

				tgtidx[1] = x0 + raidz_parity(rm);
				tgtidx[2] = x1 + raidz_parity(rm);

				corrupt_colums(rm, tgtidx+1, 2);

				if (!opts->rto_sanity)
					vdev_raidz_reconstruct(rm, tgtidx, 3);

				if (cmp_data(opts, rm) != 0) {
					err++;
					LOG(D_DEBUG, "\nREC D[%d %d]... "
					    "[FAIL]\n", x0, x1);
				}
			}
		}
	} else {
		/* can reconstruct 3 failed data disk */
		for (x0 = 0; x0 < opts->rto_dcols; x0++) {
			if (x0 >= rm->rm_row[0]->rr_cols - raidz_parity(rm))
				continue;
			for (x1 = x0 + 1; x1 < opts->rto_dcols; x1++) {
				if (x1 >= rm->rm_row[0]->rr_cols -
				    raidz_parity(rm))
					continue;
				for (x2 = x1 + 1; x2 < opts->rto_dcols; x2++) {
					if (x2 >= rm->rm_row[0]->rr_cols -
					    raidz_parity(rm))
						continue;

					/* Check if should stop */
					if (rto_opts.rto_should_stop)
						return (err);

					LOG(D_DEBUG, "[%d %d %d]", x0, x1, x2);

					tgtidx[0] = x0 + raidz_parity(rm);
					tgtidx[1] = x1 + raidz_parity(rm);
					tgtidx[2] = x2 + raidz_parity(rm);

					corrupt_colums(rm, tgtidx, 3);

					if (!opts->rto_sanity)
						vdev_raidz_reconstruct(rm,
						    tgtidx, 3);

					if (cmp_data(opts, rm) != 0) {
						err++;
						LOG(D_DEBUG,
						    "\nREC D[%d %d %d]... "
						    "[FAIL]\n", x0, x1, x2);
					}
				}
			}
		}
	}
	return (err);
}

static int
run_rec_check(raidz_test_opts_t *opts)
{
	char **impl_name;
	unsigned fn, err = 0;
	zio_t *zio_test;
	raidz_map_t *rm_test;

	err = init_raidz_golden_map(opts, PARITY_PQR);
	if (0 != err)
		return (err);

	LOG(D_INFO, DBLSEP);
	LOG(D_INFO, "Testing data reconstruction...\n");

	for (impl_name = (char **)raidz_impl_names+1; *impl_name != NULL;
	    impl_name++) {

		LOG(D_INFO, SEP);
		LOG(D_INFO, "\tTesting [%s] implementation...", *impl_name);

		if (vdev_raidz_impl_set(*impl_name) != 0) {
			LOG(D_INFO, "[SKIP]\n");
			continue;
		} else
			LOG(D_INFO, "[SUPPORTED]\n");


		/* create suitable raidz_map */
		rm_test = init_raidz_map(opts, &zio_test, PARITY_PQR);
		/* generate parity */
		vdev_raidz_generate_parity(rm_test);

		for (fn = 0; fn < RAIDZ_REC_NUM; fn++) {

			LOG(D_INFO, "\t\tTesting method [%s] ...",
			    raidz_rec_name[fn]);

			if (run_rec_check_impl(opts, rm_test, fn) != 0) {
				LOG(D_INFO, "[FAIL]\n");
				err++;

			} else
				LOG(D_INFO, "[PASS]\n");

		}
		/* tear down test raidz_map */
		fini_raidz_map(&zio_test, &rm_test);
	}

	fini_raidz_map(&opts->zio_golden, &opts->rm_golden);

	return (err);
}

static int
run_test(raidz_test_opts_t *opts)
{
	int err = 0;

	if (opts == NULL)
		opts = &rto_opts;

	print_opts(opts, B_FALSE);

	err |= run_gen_check(opts);
	err |= run_rec_check(opts);

	return (err);
}

#define	SWEEP_RUNNING	0
#define	SWEEP_FINISHED	1
#define	SWEEP_ERROR	2
#define	SWEEP_TIMEOUT	3

static int sweep_state = 0;
static raidz_test_opts_t failed_opts;

static kmutex_t sem_mtx;
static kcondvar_t sem_cv;
static int max_free_slots;
static int free_slots;

static __attribute__((noreturn)) void
sweep_thread(void *arg)
{
	int err = 0;
	raidz_test_opts_t *opts = (raidz_test_opts_t *)arg;
	VERIFY(opts != NULL);

	err = run_test(opts);

	if (rto_opts.rto_sanity) {
		/* 25% chance that a sweep test fails */
		if (rand() < (RAND_MAX/4))
			err = 1;
	}

	if (0 != err) {
		mutex_enter(&sem_mtx);
		memcpy(&failed_opts, opts, sizeof (raidz_test_opts_t));
		sweep_state = SWEEP_ERROR;
		mutex_exit(&sem_mtx);
	}

	umem_free(opts, sizeof (raidz_test_opts_t));

	/* signal the next thread */
	mutex_enter(&sem_mtx);
	free_slots++;
	cv_signal(&sem_cv);
	mutex_exit(&sem_mtx);

	thread_exit();
}

static int
run_sweep(void)
{
	static const size_t dcols_v[] = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 15, 16 };
	static const size_t ashift_v[] = { 9, 12, 14 };
	static const size_t size_v[] = { 1 << 9, 21 * (1 << 9), 13 * (1 << 12),
		1 << 17, (1 << 20) - (1 << 12), SPA_MAXBLOCKSIZE };

	(void) setvbuf(stdout, NULL, _IONBF, 0);

	ulong_t total_comb = ARRAY_SIZE(size_v) * ARRAY_SIZE(ashift_v) *
	    ARRAY_SIZE(dcols_v);
	ulong_t tried_comb = 0;
	hrtime_t time_diff, start_time = gethrtime();
	raidz_test_opts_t *opts;
	int a, d, s;

	max_free_slots = free_slots = MAX(2, boot_ncpus);

	mutex_init(&sem_mtx, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&sem_cv, NULL, CV_DEFAULT, NULL);

	for (s = 0; s < ARRAY_SIZE(size_v); s++)
	for (a = 0; a < ARRAY_SIZE(ashift_v); a++)
	for (d = 0; d < ARRAY_SIZE(dcols_v); d++) {

		if (size_v[s] < (1 << ashift_v[a])) {
			total_comb--;
			continue;
		}

		if (++tried_comb % 20 == 0)
			LOG(D_ALL, "%lu/%lu... ", tried_comb, total_comb);

		/* wait for signal to start new thread */
		mutex_enter(&sem_mtx);
		while (cv_timedwait_sig(&sem_cv, &sem_mtx,
		    ddi_get_lbolt() + hz)) {

			/* check if should stop the test (timeout) */
			time_diff = (gethrtime() - start_time) / NANOSEC;
			if (rto_opts.rto_sweep_timeout > 0 &&
			    time_diff >= rto_opts.rto_sweep_timeout) {
				sweep_state = SWEEP_TIMEOUT;
				rto_opts.rto_should_stop = B_TRUE;
				mutex_exit(&sem_mtx);
				goto exit;
			}

			/* check if should stop the test (error) */
			if (sweep_state != SWEEP_RUNNING) {
				mutex_exit(&sem_mtx);
				goto exit;
			}

			/* exit loop if a slot is available */
			if (free_slots > 0) {
				break;
			}
		}

		free_slots--;
		mutex_exit(&sem_mtx);

		opts = umem_zalloc(sizeof (raidz_test_opts_t), UMEM_NOFAIL);
		opts->rto_ashift = ashift_v[a];
		opts->rto_dcols = dcols_v[d];
		opts->rto_offset = (1ULL << ashift_v[a]) * rand();
		opts->rto_dsize = size_v[s];
		opts->rto_expand = rto_opts.rto_expand;
		opts->rto_expand_offset = rto_opts.rto_expand_offset;
		opts->rto_v = 0; /* be quiet */

		VERIFY3P(thread_create(NULL, 0, sweep_thread, (void *) opts,
		    0, NULL, TS_RUN, defclsyspri), !=, NULL);
	}

exit:
	LOG(D_ALL, "\nWaiting for test threads to finish...\n");
	mutex_enter(&sem_mtx);
	VERIFY(free_slots <= max_free_slots);
	while (free_slots < max_free_slots) {
		(void) cv_wait(&sem_cv, &sem_mtx);
	}
	mutex_exit(&sem_mtx);

	if (sweep_state == SWEEP_ERROR) {
		ERR("Sweep test failed! Failed option: \n");
		print_opts(&failed_opts, B_TRUE);
	} else {
		if (sweep_state == SWEEP_TIMEOUT)
			LOG(D_ALL, "Test timeout (%lus). Stopping...\n",
			    (ulong_t)rto_opts.rto_sweep_timeout);

		LOG(D_ALL, "Sweep test succeeded on %lu raidz maps!\n",
		    (ulong_t)tried_comb);
	}

	mutex_destroy(&sem_mtx);

	return (sweep_state == SWEEP_ERROR ? SWEEP_ERROR : 0);
}


int
main(int argc, char **argv)
{
	size_t i;
	struct sigaction action;
	int err = 0;

	/* init gdb pid string early */
	(void) sprintf(pid_s, "%d", getpid());

	action.sa_handler = sig_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	if (sigaction(SIGSEGV, &action, NULL) < 0) {
		ERR("raidz_test: cannot catch SIGSEGV: %s.\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	(void) setvbuf(stdout, NULL, _IOLBF, 0);

	dprintf_setup(&argc, argv);

	process_options(argc, argv);

	kernel_init(SPA_MODE_READ);

	/* setup random data because rand() is not reentrant */
	rand_data = (int *)umem_alloc(SPA_MAXBLOCKSIZE, UMEM_NOFAIL);
	srand((unsigned)time(NULL) * getpid());
	for (i = 0; i < SPA_MAXBLOCKSIZE / sizeof (int); i++)
		rand_data[i] = rand();

	mprotect(rand_data, SPA_MAXBLOCKSIZE, PROT_READ);

	if (rto_opts.rto_benchmark) {
		run_raidz_benchmark();
	} else if (rto_opts.rto_sweep) {
		err = run_sweep();
	} else {
		err = run_test(NULL);
	}

	umem_free(rand_data, SPA_MAXBLOCKSIZE);
	kernel_fini();

	return (err);
}
