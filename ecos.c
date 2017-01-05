#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>

#include <likwid.h>


const int SAMPLE_FREQ_NS = 200000000;
const int SAMPLE_ERR_LIMIT = 5;

const int LIKWID_MODE = ACCESSMODE_DIRECT;
char LIKWID_EVENTS[] = "ECOS";
const int LIKWID_METRIC_ID = 4;

const char* LIKWID_LOG_NAME = "ecos.likwid.log";
FILE* LIKWID_LOG;


struct cpu_data {
	int id;          // core id
	int sid;         // socket id
	FILE* fp;        // handle for system file
	double sample;   // last sample value
	int freq;        // frequency of the cpu
};


static CpuTopology_t topo;
static PowerInfo_t power;
static int gid;
static int min_freq, base_freq, max_freq;

static struct cpu_data* cpus;
static double *cpus_stats;

struct timespec start_t;
struct timespec stop_t;


int init();
void finalize();
int update_samples();
bool should_throttle(const double core_val, const double socket_avg);
void set_freq(const bool throttle, struct cpu_data *const cpu, const struct timespec *const ts);

void signal_handler(int signo);
void likwid_info();
void likwid_log(const struct timespec *const ts);
void time_restart();
void time_print(const char*const desc);


int main(int argc, char* argv[])
{
	int err, err_count = 0;
	struct timespec ts;

	printf("Initialization...\n");
	err = init();
	if (err)
		return EXIT_FAILURE;

	likwid_info();

// 	for (int i = 0; i < topo->numHWThreads; i++)
// 		printf("i: %d, threadId: %d, coreId: %d, packageId: %d, acpiId: %d, inCpuSet: %d\n",
// 			   i, topo->threadPool[i].threadId, topo->threadPool[i].coreId, topo->threadPool[i].packageId,
// 			   topo->threadPool[i].apicId, topo->threadPool[i].inCpuSet
// 			  );
//
// 	finalize();
// 	return 0;

	printf("Running...\n");
	while (1)
	{
		err = update_samples();
		clock_gettime(CLOCK_REALTIME, &ts);

		if (!err)
		{
			// get average sample per socket
			memset(cpus_stats, 0, topo->numSockets * sizeof (double));

			for (int i = 0; i < topo->numHWThreads; i++)
				cpus_stats[cpus[i].sid] += cpus[i].sample;

			for (int i = 0; i < topo->numSockets; i++)
				cpus_stats[i] /= topo->numCoresPerSocket;

			for (int i = 0; i < topo->numHWThreads; i++)
				set_freq(should_throttle(cpus[i].sample, cpus_stats[cpus[i].sid]), &cpus[i], &ts);

// 			for (int i = 0; i < topo->numHWThreads; i += 2)
// 				printf("%10.0f, ", perfmon_getLastResult(gid, 0, i));
// 			printf("\n");
//
// 			for (int i = 0; i < topo->numHWThreads; i += 2)
// 				printf("%10.0f, ", perfmon_getLastResult(gid, 2, i));
// 			printf("\n");
//
// 			for (int i = 0; i < topo->numHWThreads; i += 2)
// 				printf("%10f, ", perfmon_getLastResult(gid, 0, i)/perfmon_getLastResult(gid, 2, i));
// 			printf("\n");
// 			printf("\n");
//
// 			int events = perfmon_getNumberOfEvents(gid);
// 			for (int e = 0; e < events; e++)
// 				printf("%10.0f, ", perfmon_getLastResult(gid, e, 0));
// 			printf("\n");

			err_count = 0;
		}
		else if (++err_count >= SAMPLE_ERR_LIMIT) {
			fprintf(stderr, "Failed to update sample data for %d iterations", SAMPLE_ERR_LIMIT);
			break;
		}

		likwid_log(&ts);

		// fflush(stdout);
		nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = SAMPLE_FREQ_NS }, 0);
	}

	printf("Ending...\n");
	finalize();

	return EXIT_SUCCESS;
}


int init()
{
	int err;

	if (signal(SIGINT, signal_handler) == SIG_ERR)
		fprintf(stderr, "Can't catch SIGINT\n");
	if (signal(SIGTERM, signal_handler) == SIG_ERR)
		fprintf(stderr, "Can't catch SIGTERM\n");

	// Load the topology module.
	err = topology_init();
	if (err < 0)
	{
		fprintf(stderr, "Failed to initialize LIKWID's topology module\n");
		return EXIT_FAILURE;
	}

	// Get information about the topology of the CPUs.
	topo = get_cpuTopology();

	// Must be called before perfmon_init(). For direct access (0) you have to
	// be root.
	HPMmode(LIKWID_MODE);

	int threads_mapping[topo->numHWThreads];
	for (int i = 0; i < topo->numHWThreads; i++)
		threads_mapping[i] = topo->threadPool[i].apicId;

	// Initialize the perfmon module.
	err = perfmon_init(topo->numHWThreads, threads_mapping);
	if (err < 0)
	{
		fprintf(stderr, "Failed to initialize LIKWID's performance monitoring module\n");
		goto err;
	}

	// Get information about CPUs frequencies.
	power = get_powerInfo();
	min_freq = power->minFrequency * 1000;
	base_freq = power->baseFrequency * 1000;
	max_freq = (power->turbo.steps ? power->turbo.steps[0] : power->baseFrequency) * 1000;

	// Add eventset string to the perfmon module.
	gid = perfmon_addEventSet(LIKWID_EVENTS);
	if (gid < 0)
	{
		fprintf(stderr, "Failed to add event to LIKWID's performance monitoring module\n");
		goto err;
	}

	// Setup the eventset identified by group ID (gid).
	err = perfmon_setupCounters(gid);
	if (err < 0)
	{
		fprintf(stderr, "Failed to setup group in LIKWID's performance monitoring module\n");
		goto err;
	}

	cpus = calloc(topo->numHWThreads, sizeof(struct cpu_data));
	if (!cpus)
	{
		fprintf(stderr, "Failed to allocate memory for cpus array\n");
		goto err;
	}

	cpus_stats = calloc(topo->numSockets, sizeof(double));
	if (!cpus_stats)
	{
		fprintf(stderr, "Failed to allocate memory for cpus stats array\n");
		goto err;
	}

	char cpu_file_name[64];
	for (int i = 0; i < topo->numHWThreads; i++)
	{
		cpus[i].id = i;
		cpus[i].sid = topo->threadPool[i].packageId;
		cpus[i].freq = max_freq;

		// open system file for each cpu
		sprintf(cpu_file_name, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
		cpus[i].fp = fopen(cpu_file_name, "r+");

		if (cpus[i].fp == NULL)
		{
			perror("Failed to open system file");
			goto err;
		}

		// disable buffering
		setbuf(cpus[i].fp, NULL);

		// read current value
		fscanf(cpus[i].fp, "%d", &cpus[i].freq);
	}

	LIKWID_LOG = fopen(LIKWID_LOG_NAME, "w");

	// Start all counters in the previously set up event set.
	err = perfmon_startCounters();
	if (err < 0)
	{
		printf("Failed to start counters for group %d for thread %d\n", gid, (-1*err)-1);
		goto err;
	}

	return EXIT_SUCCESS;

err:
	finalize();

	return EXIT_FAILURE;
}


void finalize()
{
	perfmon_finalize();
	topology_finalize();

	for (int i = 0; i < topo->numHWThreads; i++)
		if (cpus[i].fp)
		{
			fprintf(cpus[i].fp, "%d", max_freq);
			fclose(cpus[i].fp);
		}

	fclose(LIKWID_LOG);

	free(cpus);
}


int update_samples()
{
	int err = perfmon_readGroupCounters(gid);
	if (err < 0)
	{
		fprintf(stderr, "Failed to read counters for thread %d\n", (-1*err)-1);
		return EXIT_FAILURE;
	}

	// CYCLE_ACTIVITY_STALLS_LDM_PENDING / CPU_CLK_UNHALTED_CORE
// 	for (int i = 0; i < topo->numHWThreads; i++)
// 		cpus[i].sample = perfmon_getLastResult(gid, 0, i) / perfmon_getLastResult(gid, 2, i);
	for (int i = 0; i < topo->numHWThreads; i++)
		cpus[i].sample = perfmon_getLastMetric(gid, LIKWID_METRIC_ID, i);

	return EXIT_SUCCESS;
}


bool should_throttle(const double core_val, const double socket_avg)
{
	return socket_avg > 0.5 && core_val > 0.8;
}


void set_freq(const bool throttle, struct cpu_data *const cpu, const struct timespec *const ts)
{
	int new_freq;

	if (throttle)
	{
		if (cpu->freq <= min_freq)
			return;
		else if (cpu->freq == max_freq && cpu->freq != base_freq)
			new_freq = base_freq;
		else
			new_freq = cpu->freq - 1e5;
	}
	else
	{
		if (cpu->freq >= max_freq)
			return;
		else if (cpu->freq == base_freq)
			new_freq = max_freq;
		else
			new_freq = cpu->freq + 1e5;
	}

	printf("set_freq %ld.%09ld %2d %d %d %d %.4f %.4f\n",
		   ts->tv_sec, ts->tv_nsec, cpu->id, throttle, cpu->freq, new_freq, cpus_stats[cpu->sid], cpu->sample);

	int res = fprintf(cpu->fp, "%d", new_freq);

	if (res > 0)
		cpu->freq = new_freq;
	else
		perror("Failed to set new frequency");
}


void signal_handler(int signo)
{
	fprintf(stderr, "\nReceived signal %d, finishing gracefully...\n", signo);

	finalize();

	exit(EXIT_SUCCESS);
}


void likwid_info()
{
	printf("Processor info:\n");

	printf("\tCores: %d (%d * %d * %d)\n", topo->numHWThreads, topo->numSockets, topo->numCoresPerSocket, topo->numThreadsPerCore);
	printf("\tBase clock: %f\n", power->baseFrequency);
	printf("\tMinimal clock: %f\n", power->minFrequency);

	printf("\tTurbo: ");
	for (int s = 0; s < power->turbo.numSteps; s++)
		printf("%f ", power->turbo.steps[s]);
	printf("\n");

	printf("\tmin_freq: %d, max_freq: %d\n", min_freq, max_freq);

	printf("Events info:\n");

	int groups = perfmon_getNumberOfGroups();
	printf("groups: %d\n", groups);
	for (int g = 0; g < groups; g++)
	{
		printf("group name: %s\n", perfmon_getGroupName(g));
		printf("group info: %s\n", perfmon_getGroupInfoShort(g));

		int events = perfmon_getNumberOfEvents(g);
		for (int e = 0; e < events; e++)
			printf("\tevent: %d, name: %s, counter: %s\n", e, perfmon_getEventName(g, e), perfmon_getCounterName(g, e));

		int metrics = perfmon_getNumberOfMetrics(g);
		for (int m = 0; m < metrics; m++)
			printf("\tmetric: %d, name: %s\n", m, perfmon_getMetricName(g, m));
	}
}


void likwid_log(const struct timespec *const ts)
{
	const int events = perfmon_getNumberOfEvents(gid);
	const int metrics = perfmon_getNumberOfMetrics(gid);

	fprintf(LIKWID_LOG, "TIMESTAMP,%ld.%09ld\n", ts->tv_sec, ts->tv_nsec);

	for (int e = 0; e < events; e++)
	{
		fprintf(LIKWID_LOG, "%s", perfmon_getEventName(gid, e));

		for (int i = 0; i < topo->numHWThreads; i++)
			fprintf(LIKWID_LOG, ",%f", perfmon_getLastResult(gid, e, i));

		fprintf(LIKWID_LOG, "\n");
	}
	for (int m = 0; m < metrics; m++)
	{
		fprintf(LIKWID_LOG, "%s", perfmon_getMetricName(gid, m));

		for (int i = 0; i < topo->numHWThreads; i++)
			fprintf(LIKWID_LOG, ",%f", perfmon_getLastMetric(gid, m, i));

		fprintf(LIKWID_LOG, "\n");
	}
	fprintf(LIKWID_LOG, "---\n");
	fflush(LIKWID_LOG);
}


void time_restart()
{
	clock_gettime (CLOCK_MONOTONIC, &start_t);
}


void time_print(const char *const desc)
{
	clock_gettime (CLOCK_MONOTONIC, &stop_t);

	unsigned long long int s = 1000000000 * start_t.tv_sec + start_t.tv_nsec;
	unsigned long long int e = 1000000000 * stop_t.tv_sec + stop_t.tv_nsec;

	printf("TIMER (%s): %f\n", desc, (double)(e - s) / 1000000000);

	start_t.tv_sec = stop_t.tv_sec;
	start_t.tv_nsec = stop_t.tv_nsec;
}
