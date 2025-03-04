#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/wait.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<signal.h>
#include "clockUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2
const int PROCESS_TABLE_MAX_SIZE = 20;
const int ONE_BILLION = 1000000000;

struct PCB {
	bool occupied; // either true or false
	pid_t pid; // process id of this child
	int startSeconds; // time when it was forked
	int startNano; // time when it was forked
};
struct PCB processTable[PROCESS_TABLE_MAX_SIZE];
int sharedMemoryId;
int* sharedClock;

void printHelp() {
	printf("Usage: oss [-h] [-n proc] [-s simul] [-t iter]\n");
	printf("-h : Print options for the oss tool and exits\n");
	printf("-n : Total number of processes to run (default: 1)\n");
	printf("-s : Maximum number of simultaneously running processes. Maximum of %d (default: %d)\n", PROCESS_TABLE_MAX_SIZE, PROCESS_TABLE_MAX_SIZE);
	printf("-t : Maximum number of seconds that workers will run (default: 1)\n");
	printf("-i : Interval (in ms) to launch child processes (default: 0)\n");
}

/* Takes in optarg and returns int. Defaults to 1 if out of bounds */
int processOptarg(const char* optarg) {
	int argAsInt = atoi(optarg);
	return argAsInt >= 1 ? argAsInt : 1;
}

/* Helper function for finding an unoccupied slot in the process table array. Returns -1 if all are occupied */
int findUnoccupiedProcessTableIndex() {
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		if (!processTable[i].occupied) {
			return i;
		}
	}
	return -1;
}

/* Helper function for finding an entry in the process table with a specific PID and remove it */
void removePidFromProcessTable(pid_t pid) {
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		if (processTable[i].pid == pid && processTable[i].occupied) {
			kill(processTable[i].pid, SIGTERM);
			processTable[i].occupied = false;
			return;
		}
	}
}


/* Detaches pointer and clears shared memory at the key */
void cleanUpSharedMemory() {
	shmdt(sharedClock);
	shmctl(SHMKEY, IPC_RMID, NULL);
}

/* Kills child processes and clears shared memory */
void handleFailsafeSignal(int signal) {
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		if (processTable[i].occupied) {
			kill(processTable[i].pid, SIGTERM);
		}
	}
	cleanUpSharedMemory();

	exit(1);
}

int main(int argc, char** argv) {
	const char optstr[] = "hn:s:t:i:";
	char opt;

	/* Default parameters */
	int processesAmount = 1;
	int maxSimultaneousProcesses = PROCESS_TABLE_MAX_SIZE;
	int maxSeconds = 1;
	int forkIntervalMs = 0;
	bool forkIntervalEnabled = false;

	/* Process options */
	while ((opt = getopt(argc, argv, optstr)) != -1) {
		switch (opt) {
			case 'h':
				printHelp();
				return EXIT_SUCCESS;
			case 'n':
				processesAmount = processOptarg(optarg);
				break;
			case 's':
				maxSimultaneousProcesses = processOptarg(optarg);
				if (maxSimultaneousProcesses > PROCESS_TABLE_MAX_SIZE) {
					maxSimultaneousProcesses = PROCESS_TABLE_MAX_SIZE;
				}
				break;
			case 't':
				maxSeconds = processOptarg(optarg);
				break;
			case 'i':
				forkIntervalMs = processOptarg(optarg);
				forkIntervalEnabled = true;
				break;
		}
	}

	/* Set up process table */
	PCB emptyPcb = { false, 0, 0, 0 };
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		processTable[i] = emptyPcb;
	}

	/* Set up shared memory & attach */
	sharedMemoryId = shmget(SHMKEY, BUFFER_SIZE, 0777 | IPC_CREAT);
	if (sharedMemoryId == -1) {
		fprintf(stderr, "OSS: Error defining shared memory");
		exit(1);
	}
	sharedClock = (int*)(shmat(sharedMemoryId, 0, 0));

	/* Simulated clock: seconds */
	sharedClock[0] = 0;
	/* Simulated clock: nanoseconds */
	sharedClock[1] = 0;

	/* Keeps track of the half-second intervals where OSS will print the PCB table */
	int pcbTimerSeconds = 0;
	int pcbTimerNano = ONE_BILLION / 2;

	/* Keeps track of the interval it can create new instances */
	int readyToForkSeconds = 0;
	int readyToForkNano = 0;

	/* Interval converted to our clock's units */
	int intervalSeconds = forkIntervalMs / 1000;
	int intervalNano = (forkIntervalMs % 1000) * 1000000;

	/* Set up failsafe that kills the program and its children after 60 seconds */
	signal(SIGALRM, handleFailsafeSignal);
	alarm(60);

	int instancesRunning = 0;
	int totalInstancesToLaunch = processesAmount;
	while (totalInstancesToLaunch > 0 || instancesRunning > 0) {
		/* Printing PCB table*/
		if (hasTimePassed(sharedClock[0], sharedClock[1], pcbTimerSeconds, pcbTimerNano)) {
			addToClock(pcbTimerSeconds, pcbTimerNano, 0, ONE_BILLION / 2);
			printf("Entry\tOccupied?\tPID\tStart(s)\tStart(ns)\n");
			for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
				int entry = i;
				const char* isOccupied = processTable[i].occupied ? "true" : "false";
				printf("%d\t%s\t\t%d\t%d\t\t%d\n", entry, isOccupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
			}
		}
		
		pid_t childPid;
		bool shouldAddToProcessTable = false;
		bool shouldFork = totalInstancesToLaunch > 0 && instancesRunning < maxSimultaneousProcesses;
		bool readyToFork = !forkIntervalEnabled || hasTimePassed(sharedClock[0], sharedClock[1], readyToForkSeconds, readyToForkNano);
		if (shouldFork && readyToFork) {
			/* Creating child */
			totalInstancesToLaunch--;
			instancesRunning++;
			shouldAddToProcessTable = true;

			if (forkIntervalEnabled) {
				readyToForkSeconds = sharedClock[0];
				readyToForkNano = sharedClock[1];
				addToClock(readyToForkSeconds, readyToForkNano, intervalSeconds, intervalNano);
			}

			childPid = fork();
		}

		/* Child process - launches worker */
		if (childPid == 0) {
			string arg0 = "./worker";
			string arg1 = to_string(rand() % maxSeconds);
			string arg2 = to_string(rand() % ONE_BILLION);
			execlp(arg0.c_str(), arg0.c_str(), arg1.c_str(), arg2.c_str(), (char*)0);
			fprintf(stderr, "OSS: Launching worker failed, terminating\n");
			exit(1);
			/* Parent process - waits for children to terminate */
		} else {
			if (shouldAddToProcessTable) {
				int index = findUnoccupiedProcessTableIndex();
				if (index == -1) {
					fprintf(stderr, "OSS: No unoccupied entry in process table found. Continuing child execution\n");
				}
				PCB newPcb = { true, childPid, sharedClock[0], sharedClock[1] };
				processTable[index] = newPcb;
			}

			int status;
			int pid = waitpid(-1, &status, WNOHANG);

			if (pid != 0) {
				removePidFromProcessTable(pid);
				instancesRunning--;
			}

			const int NANO_SECONDS_TO_ADD_EACH_LOOP = ONE_BILLION / 800000;
			addToClock(sharedClock[0], sharedClock[1], 0, NANO_SECONDS_TO_ADD_EACH_LOOP);
		}
	}

	cleanUpSharedMemory();
	return EXIT_SUCCESS;
}
