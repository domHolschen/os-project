#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/wait.h>
#include<string>
#include "clockUtils.h"
using namespace std;

#define SHMKEY 9021090210
#define BUFFER_SIZE sizeof(int) * 2

const int PROCESS_TABLE_MAX_SIZE = 20;
struct PCB {
	int occupied; // either true or false
	pid_t pid; // process id of this child
	int startSeconds; // time when it was forked
	int startNano; // time when it was forked
};

const int ONE_BILLION = 1000000000;

void printHelp() {
	printf("Usage: oss [-h] [-n proc] [-s simul] [-t iter]\n");
	printf("-h : Print options for the oss tool and exits\n");
	printf("-n : Total number of processes to run (default: 1)\n");
	printf("-s : Maximum number of simultaneously running processes (disabled by default)\n");
	printf("-t : Maximum number of seconds that workers will run (default: 1)\n");
}

/* Takes in optarg and returns int. Defaults to 1 if out of bounds */
int processOptarg(const char* optarg) {
	int argAsInt = atoi(optarg);
	return argAsInt >= 1 ? argAsInt : 1;
}

/* Helper function for finding an unoccupied slot in the process table array. Returns -1 if all are occupied */
int findUnoccupiedProcessTableIndex(PCB* processTable) {
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		if (!processTable[i].occupied) {
			return i;
		}
	}
	return -1;
}

int main(int argc, char** argv) {
	const char optstr[] = "hn:s:t:";
	char opt;

	/* Default parameters */
	int processesAmount = 1;
	int maxSimultaneousProcesses = 1;
	bool simultaneousLimitEnabled = false;
	int maxSeconds = 1;

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
				break;
			case 't':
				maxSeconds = processOptarg(optarg);
				break;
		}
	}

	/* Set up process table */
	struct PCB processTable[PROCESS_TABLE_MAX_SIZE];
	PCB emptyPcb = { false, 0, 0, 0 };
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		processTable[i] = emptyPcb;
	}

	int simClockSeconds = 0;
	int simClockNano = 0;

	if (maxSimultaneousProcesses < processesAmount) {
		simultaneousLimitEnabled = true;
	}

	int instancesToWaitFor = 0;
	int totalInstancesToLaunch = processesAmount;

	/* Keeps track of the half-second intervals where OSS will print the PCB table*/
	int pcbTimerSeconds = 0;
	int pcbTimerNano = ONE_BILLION / 2;
	while (totalInstancesToLaunch > 0) {
		/* Printing PCB table*/
		if (ClockUtils::hasTimePassed(simClockSeconds, simClockNano, pcbTimerSeconds, pcbTimerNano)) {
			ClockUtils::addToClock(pcbTimerSeconds, pcbTimerNano, 0, ONE_BILLION / 2);
			printf("Entry\tOccupied?\tPID\tStart(s)\tStart(ns)");
			for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
				int entry = i;
				string isOccupied = processTable[i].occupied ? "true" : "false";
				printf("%d\t%s\t%d\t%d\t%d", entry, isOccupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
			}
		}
		instancesToWaitFor++;
		totalInstancesToLaunch--;
		pid_t childPid = fork();

		/* Child process - launches user */
		if (childPid == 0) {
			string arg0 = "./worker";
			string arg1 = to_string(rand() % maxSeconds);
			string arg2 = to_string(rand() % ONE_BILLION);
			execlp(arg0.c_str(), arg0.c_str(), arg1.c_str(), arg2.c_str(), (char*)0);
			fprintf(stderr, "Launching user failed, terminating\n");
			exit(1);
			/* Parent process - waits for children to terminate */
		} else {
			bool shouldWait = totalInstancesToLaunch == 0 || (simultaneousLimitEnabled && instancesToWaitFor >= maxSimultaneousProcesses);
			if (shouldWait) {
				for (int i = 0; i < instancesToWaitFor; i++) {
					wait(0);
				}
				instancesToWaitFor = 0;
			}
		}
		/* 100 million nanoseconds */
		const int NANO_SECONDS_TO_ADD_EACH_LOOP = 100000000;
		ClockUtils::addToClock(simClockSeconds, simClockNano, 0, NANO_SECONDS_TO_ADD_EACH_LOOP);
	}

	return EXIT_SUCCESS;
}
