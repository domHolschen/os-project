#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include "clockUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2

/* Function to verify whether the argument is not null, numeric, and at least 1 */
bool isValidArgument(const char* arg) {
	if (arg == NULL || *arg == '\0') {
		return false;
	}

	const char* argToStepThrough = arg;
	while (*argToStepThrough) {
		if (!isdigit(*argToStepThrough)) {
			return false;
		}
		argToStepThrough++;
	}

	int argAsInt = atoi(arg);
	return argAsInt > 0;
}

/* Prepared printf statement that displays clock & process details */
void printProcessDetails(int simClockS, int simClockN, int termTimeS, int termTimeN) {
	printf("WORKER: PID:%d PPID:%d Clock(s):%d Clock(ns):%d Terminate(s):%d Terminate(ns):%d\n", getpid(), getppid(), simClockS, simClockN, termTimeS, termTimeN);
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	int terminateSeconds = 0;
	int terminateNano = 0;
	if (isValidArgument(argv[1])) {
		terminateSeconds = atoi(argv[1]);
	} else {
		terminateSeconds = 1;
		printf("WORKER: Invalid or missing seconds argument, defaulting to 1\n");
	}
	if (isValidArgument(argv[2])) {
		terminateNano = atoi(argv[2]);
	}
	else {
		terminateNano = 1;
		printf("WORKER: Invalid or missing nanoseconds argument, defaulting to 1\n");
	}

	/* Set up shared memory & attach */
	int sharedMemoryId = shmget(SHMKEY, BUFFER_SIZE, 0777 | IPC_CREAT);
	if (sharedMemoryId == -1) {
		fprintf(stderr, "WORKER: Error defining shared memory");
		exit(1);
	}
	int* sharedClock = (int*)(shmat(sharedMemoryId, 0, 0));

	/* Adds simulated clock's time to the passed in duration to get the time to terminate */
	addToClock(terminateSeconds, terminateNano, sharedClock[0], sharedClock[1]);

	printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
	printf("WORKER: Just starting...\n");
	int previousSecond = sharedClock[0];
	int secondsElapsed = 0;
	/* Loop waits for the terminate time to pass */
	while (!hasTimePassed(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano)) {
		/* Checks for when shared clock's seconds update */
		if (previousSecond < sharedClock[0]) {
			secondsElapsed++;
			printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
			printf("WORKER: %d seconds elapsed since starting\n", secondsElapsed);
		}
		previousSecond = sharedClock[0];
	}
	printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
	printf("WORKER: Terminating...\n");

	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
