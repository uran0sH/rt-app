/* 
This file is part of rt-app - https://launchpad.net/rt-app
Copyright (C) 2010  Giacomo Bagnoli <g.bagnoli@asidev.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/ 

#include "rt-app.h"
#include "rt-app_utils.h"

static int errno;
static int continue_running;
static pthread_t *threads;
static int nthreads;

static inline
unsigned int max_run(int min, int max)
{
        return min + (((double) rand()) / RAND_MAX) * (max - min);
}

void run(int ind, struct timespec *min, struct timespec *max)
{
	//int m = max_run(timespec_to_msec(min), timespec_to_msec(max));
	//struct timespec t_start, t_step, t_exec = msec_to_timespec(m);
	struct timespec t_start, t_step, t_exec = *max;

	/* get the start time */
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t_start);
	/* compute finish time for CPUTIME_ID clock */
	t_exec = timespec_add(&t_start, &t_exec);

	while (1) {
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t_step);
		if (!timespec_lower(&t_step, &t_exec))
			break;
	}
}

static void
shutdown(int sig)
{
	int i;
	// notify threads, join them, then exit
	continue_running = 0;
	for (i = 0; i < nthreads; i++)
	{
		pthread_join(threads[i], NULL);
	}
	exit(EXIT_SUCCESS);
}

void *thread_body(void *arg)
{
	thread_data_t *data = (thread_data_t*) arg;
	struct sched_param param;
	struct timespec t, t_next;
	unsigned long t_start_usec;
	unsigned long my_duration_usec;
	int nperiods;
	timing_point_t *timings;
	timing_point_t tmp_timing;
	timing_point_t *curr_timing;
#ifdef AQUOSA
	qres_time_t prev_abs_used_budget = 0;
	qres_time_t abs_used_budget;
#endif
#ifdef DLSCHED
	pid_t tid;
#endif
	int ret, i = 0;
	int j;
	/* set scheduling policy and print pretty info on stdout */
	switch (data->sched_policy)
	{
		case rr:
			log_info("[%d] Using SCHED_RR policy:", data->ind);
			goto posixrtcommon;
		case fifo:
			log_info("[%d] Using SCHED_FIFO policy:", data->ind);
posixrtcommon:			
			param.sched_priority = data->sched_prio;
			ret = pthread_setschedparam(pthread_self(), 
						    data->sched_policy, 
						    &param);
			if (ret != 0) {
				errno = ret; 
				perror("pthread_setschedparam"); 
				exit(EXIT_FAILURE);
			}

			log_info("[%d] starting thread with period: %lu, exec: %lu,"
			       "deadline: %lu, priority: %d",
			       	data->ind,
				timespec_to_usec(&data->period), 
				timespec_to_usec(&data->min_et),
				timespec_to_usec(&data->deadline),
				data->sched_prio
			);
			break;

		case other:
			log_info("[%d] Using SCHED_OTHER policy:", data->ind);
			log_info("[%d] starting thread with period: %lu, exec: %lu,"
			       "deadline: %lu",
			       	data->ind,
				timespec_to_usec(&data->period), 
				timespec_to_usec(&data->min_et),
				timespec_to_usec(&data->deadline)
			);
			data->lock_pages = 0; /* forced off for SCHED_OTHER */
			break;
#ifdef AQUOSA			
		case aquosa:
			data->params.Q_min = round((timespec_to_usec(&data->min_et) * (( 100.0 + data->sched_prio ) / 100)) / (data->fragment * 1.0)); 
			data->params.Q = round((timespec_to_usec(&data->max_et) * (( 100.0 + data->sched_prio ) / 100)) / (data->fragment * 1.0));
			data->params.P = round(timespec_to_usec(&data->period) / (data->fragment * 1.0));
			data->params.flags = 0;
			log_info("[%d] Creating QRES Server with Q=%ld, P=%ld",
				data->ind,data->params.Q, data->params.P);
			
			qos_chk_ok_exit(qres_init());
			qos_chk_ok_exit(qres_create_server(&data->params, 
							   &data->sid));
			log_info("[%d] AQuoSA server ID: %d", data->ind, data->sid);
			log_info("[%d] attaching thread (deadline: %lu) to server %d",
				data->ind,
				timespec_to_usec(&data->deadline),
				data->sid
			);
			qos_chk_ok_exit(qres_attach_thread(data->sid, 0, 0));

			break;
#endif
#ifdef DLSCHED
		case deadline:
			tid = gettid();
			log_info("[%d] using %s policy for tid %d", data->ind, 
				 data->sched_policy_descr, tid);
			data->dl_params.sched_priority = data->sched_prio;
			data->dl_params.sched_runtime = data->max_et;
			data->dl_params.sched_deadline = data->period;
			data->dl_params.sched_period = data->period;
			data->dl_params.sched_flags = SCHED_BWRECL_RT;

			ret = sched_setscheduler_ex(tid, SCHED_DEADLINE, 
						    sizeof(struct sched_param_ex),
						    &data->dl_params);
			if (ret != 0) {
				log_critical("[%d] sched_setscheduler_ex"
					     "returned %d", data->ind, ret);
				exit(EXIT_FAILURE);
			}
				
			log_info("[%d] starting thread with period: %lu, exec: %lu,"
			       "deadline: %lu, priority: %d",
			       	data->ind,
				timespec_to_usec(&data->period), 
				timespec_to_usec(&data->min_et),
				timespec_to_usec(&data->deadline),
				data->sched_prio
			);
			break;
#endif
			
		default:
			log_error("Unknown scheduling policy %d",
				data->sched_policy);
			exit(EXIT_FAILURE);
	}
	
	if (data->lock_pages == 1)
	{
		log_info("[%d] Locking pages in memory", data->ind);
		ret = mlockall(MCL_CURRENT | MCL_FUTURE);
		if (ret < 0) {
			errno = ret;
			perror("mlockall");
			exit(EXIT_FAILURE);
		}
	}
	/* set thread affinity */
	if (data->cpuset != NULL)
	{
		log_info("[%d] setting cpu affinity to CPU(s) %s", data->ind, 
			 data->cpuset_str);
		ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
						data->cpuset);
		if (ret < 0) {
			errno = ret;
			perror("pthread_setaffinity_np");
			exit(EXIT_FAILURE);
		}
	}

	if (data->wait_before_start > 0) {
		log_info("[%d] Waiting %ld usecs... ", data->ind, 
			 data->wait_before_start);
		clock_gettime(CLOCK_MONOTONIC, &t);
		t_next = msec_to_timespec(data->wait_before_start);
		t_next = timespec_add(&t, &t_next);
		clock_nanosleep(CLOCK_MONOTONIC, 
				TIMER_ABSTIME, 
				&t_next,
				NULL);
		log_info("[%d] Starting...", data->ind);
	}
	/* if we know the duration we can calculate how many periods we will
	 * do at most, and the log to memory, instead of logging to file.
	 */
	timings = NULL;
	if (data->duration > 0) {
		my_duration_usec = (data->duration * 10e6) - 
				   (data->wait_before_start * 1000);
		nperiods = (int) ceil( my_duration_usec / 
				      (double) timespec_to_usec(&data->period));
		timings = malloc ( nperiods * sizeof(timing_point_t));
	}

	clock_gettime(CLOCK_MONOTONIC, &t);
	t_next = t;
	data->deadline = timespec_add(&t, &data->deadline);

	fprintf(data->log_handler, "#idx\tperiod\tmin_et\tmax_et\trel_st\tstart"
				   "\t\tend\t\tdeadline\tdur.\tslack"
				   "\tBudget\tUsed Budget\n");
	while (continue_running) {
		struct timespec t_start, t_end, t_diff, t_slack;

		clock_gettime(CLOCK_MONOTONIC, &t_start);
		run(data->ind, &data->min_et, &data->max_et);
		clock_gettime(CLOCK_MONOTONIC, &t_end);
		
		t_diff = timespec_sub(&t_end, &t_start);
		t_slack = timespec_sub(&data->deadline, &t_end);

		t_start_usec = timespec_to_usec(&t_start); 
		if (timings)
			curr_timing = &timings[i];
		else
			curr_timing = &tmp_timing;
		curr_timing->ind = data->ind;
		curr_timing->period = timespec_to_usec(&data->period);
		curr_timing->min_et = timespec_to_usec(&data->min_et);
		curr_timing->max_et = timespec_to_usec(&data->max_et);
		curr_timing->rel_start_time = 
			t_start_usec - timespec_to_usec(&data->main_app_start);
		curr_timing->abs_start_time = t_start_usec;
		curr_timing->end_time = timespec_to_usec(&t_end);
		curr_timing->deadline = timespec_to_usec(&data->deadline);
		curr_timing->duration = timespec_to_usec(&t_diff);
		curr_timing->slack =  timespec_to_lusec(&t_slack);
#ifdef AQUOSA
		if (data->sched_policy == aquosa) {
			curr_timing->budget = data->params.Q;
			qres_get_exec_time(data->sid, 
					   &abs_used_budget, 
					   NULL);
			curr_timing->used_budget = 
				abs_used_budget - prev_abs_used_budget;
			prev_abs_used_budget = abs_used_budget;

		} else {
			curr_timing->budget = 0;
			curr_timing->used_budget = 0;
		}
#endif
		if (!timings)
			log_timing(data->log_handler, curr_timing);

		t_next = timespec_add(&t_next, &data->period);
		data->deadline = timespec_add(&data->deadline, &data->period);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_next, NULL);
		i++;
	}

	if (timings)
		for (j=0; j < i; j++)
			log_timing(data->log_handler, &timings[j]);
	
	log_info("[%d] Exiting.", data->ind);
	fclose(data->log_handler);
#ifdef AQUOSA
	if (data->sched_policy == aquosa) {
		qres_destroy_server(data->sid);
		qres_cleanup();
	}
#endif
	pthread_exit(NULL);
}

/* parse a thread token in the form  $period:$exec:$deadline:$policy:$prio and
 * fills the thread_data structure
 */

int main(int argc, char* argv[])
{
	struct timespec t_curr, t_next, t_start;
	FILE *gnuplot_script = NULL;
	int i;
	thread_data_t *tdata;
	char tmp[PATH_LENGTH];

	rtapp_options_t opts;
	parse_command_line(argc, argv, &opts);

	nthreads = opts.nthreads;
	threads = malloc(nthreads * sizeof(pthread_t));
	
	/* install a signal handler for proper shutdown */
	signal(SIGQUIT, shutdown);
	signal(SIGTERM, shutdown);
	signal(SIGHUP, shutdown);
	signal(SIGINT, shutdown);

	continue_running = 1;

	/* Take the beginning time for everything */
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	/* start threads */
	for (i = 0; i < nthreads; i++)
	{
		tdata = &opts.threads_data[i];
		if (opts.spacing > 0 ) {
			/* start the thread, then it will sleep accordingly
			 * to its position. We don't sleep here anymore as 
			 * this would mean that 
			 * duration = spacing * nthreads + duration */
			tdata->wait_before_start = opts.spacing * (i+1);
		} else {
			tdata->wait_before_start = 0;
		}
		tdata->duration = opts.duration;
		tdata->ind = i;
		tdata->main_app_start = t_start;
		tdata->lock_pages = opts.lock_pages;
#ifdef AQUOSA
		tdata->fragment = opts.fragment;
#endif
		if (opts.logdir) {
			snprintf(tmp, PATH_LENGTH, "%s/%s-t%d.log",
				 opts.logdir,
				 opts.logbasename,
				 i);
			tdata->log_handler = fopen(tmp, "w");
			if (!tdata->log_handler){
				log_error("Cannot open logfile %s", tmp);
				exit(EXIT_FAILURE);
			}
		} else {
			tdata->log_handler = stdout;
		}
		
		if (pthread_create(&threads[i],
				  NULL, 
				  thread_body, 
				  (void*) tdata))
			goto exit_err;
	}

	/* print gnuplot files */ 
	if (opts.logdir && opts.gnuplot)
	{
		snprintf(tmp, PATH_LENGTH, "%s/%s-duration.plot", 
			 opts.logdir, opts.logbasename);
		gnuplot_script = fopen(tmp, "w+");
		snprintf(tmp, PATH_LENGTH, "%s/%s-duration.eps",
			opts.logdir, opts.logbasename);
		fprintf(gnuplot_script,
			"set terminal postscript enhanced color\n"
			"set output '%s'\n"
			"set grid\n"
			"set key outside right\n"
			"set title \"Measured exec time per period\"\n"
			"set xlabel \"Cycle start time [usec]\"\n"
			"set ylabel \"Exec Time [usec]\"\n"
			"plot ", tmp);

		for (i=0; i<nthreads; i++)
		{
			snprintf(tmp, PATH_LENGTH, "%s/%s-duration.plot", 
				 opts.logdir, opts.logbasename);

			fprintf(gnuplot_script, 
				"\"%s-t%d.log\" u ($5/1000):9 w l"
				" title \"thread%d (%s)\"", 
				opts.logbasename, i, i, 
				opts.threads_data[i].sched_policy_descr);

			if ( i == nthreads-1)
				fprintf(gnuplot_script, "\n");
			else
				fprintf(gnuplot_script, ", ");
		}
		fprintf(gnuplot_script, "set terminal wxt\nreplot\n");
		fclose(gnuplot_script);

		snprintf(tmp, PATH_LENGTH, "%s/%s-slack.plot", 
		 	 opts.logdir, opts.logbasename);
		gnuplot_script = fopen(tmp, "w+");
		snprintf(tmp, PATH_LENGTH, "%s/%s-slack.eps", 
		 	 opts.logdir, opts.logbasename);

		fprintf(gnuplot_script,
			"set terminal postscript enhanced color\n"
			"set output '%s'\n"
			"set grid\n"
			"set key outside right\n"
			"set title \"Slack (negative = tardiness)\"\n"
			"set xlabel \"Cycle start time [msec]\"\n"
			"set ylabel \"Slack/Tardiness [usec]\"\n"
			"plot ", tmp);

		for (i=0; i < nthreads; i++)
		{
			fprintf(gnuplot_script, 
				"\"%s-t%d.log\" u ($5/1000):10 w l"
				" title \"thread%d (%s)\"", 
				opts.logbasename, i, i,
				opts.threads_data[i].sched_policy_descr);

			if ( i == nthreads-1) 
				fprintf(gnuplot_script, ", 0 notitle\n");
			else
				fprintf(gnuplot_script, ", ");

		}
		fprintf(gnuplot_script, "set terminal wxt\nreplot\n");
		fclose(gnuplot_script);
	}
	
	if (opts.duration > 0)
	{
		sleep(opts.duration);
		shutdown(SIGTERM);
	}
	
	for (i = 0; i < nthreads; i++) 	{
		pthread_join(threads[i], NULL);
	}
	exit(EXIT_SUCCESS);


exit_err:
	exit(EXIT_FAILURE);
}



