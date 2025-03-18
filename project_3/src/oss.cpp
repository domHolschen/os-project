#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/wait.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<signal.h>
#include<cstdlib>
#include<sys/msg.h>
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
	int messagesSent; // number of messages sent to the process
};
struct PCB processTable[PROCESS_TABLE_MAX_SIZE];
struct MessageBuffer {
	long messageType;
	int value;
};
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
			processTable[i] = { false, 0, 0, 0 };
			return;
		}
	}
}

/* Finds the next running process in the process table. If it loops past the end of the array, it resets to 0. Returns -1 if all are unoccupied */
int findNextProcessInTable(int currentIndex) {
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		int indexToCheck = (i + currentIndex + 1) % PROCESS_TABLE_MAX_SIZE;
		if (processTable[indexToCheck].occupied) {
			return indexToCheck;
		}
	}
	return -1;
}

/* Detaches pointer and clears shared memory at the key */
void cleanUpSharedMemory() {
	shmdt(sharedClock);
	shmctl(sharedMemoryId, IPC_RMID, NULL);
}

/* Kills child processes and clears shared memory */
void handleFailsafeSignal(int signal) {
	printf("OSS: Failsafe signal captured, terminating...\n");

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

	/* Set up message queue */
	int messageQueueId;
	key_t key;
	system("touch keyfile.txt");

	if ((key = ftok("keyfile.txt", 1)) == -1) {
		perror("OSS: Fatal error generating key using keyfile.txt, terminating...\n");
		exit(1);
	}

	if ((messageQueueId = msgget(key, 0644 | IPC_CREAT)) == -1) {
		perror("OSS: Fatal error getting message queue ID, terminating...\n");
		exit(1);
	}

	/* Set up process table */
	PCB emptyPcb = { false, 0, 0, 0 };
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
		processTable[i] = emptyPcb;
	}
	int processIndexToMessage = 0;
	int totalMessagesSent = 0;

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
	signal(SIGINT, handleFailsafeSignal);

	int instancesRunning = 0;
	int totalInstancesToLaunch = processesAmount;
	while (totalInstancesToLaunch > 0 || instancesRunning > 0) {
		pid_t childPid = -1;
		bool shouldAddToProcessTable = false;

		/* Determining if child should be created */
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
			/* Adds child process to table if one was forked this iteration */
			if (shouldAddToProcessTable) {
				int index = findUnoccupiedProcessTableIndex();
				if (index == -1) {
					fprintf(stderr, "OSS: No unoccupied entry in process table found. Continuing child execution\n");
				}
				PCB newPcb = { true, childPid, sharedClock[0], sharedClock[1] };
				processTable[index] = newPcb;
			}

			/* Printing PCB table */
			if (hasTimePassed(sharedClock[0], sharedClock[1], pcbTimerSeconds, pcbTimerNano)) {
				addToClock(pcbTimerSeconds, pcbTimerNano, 0, ONE_BILLION / 2);
				printf("Process table:\nEntry\tOccupied?\tPID\t\tStart(s)\tStart(ns)\tMessages Sent\n");
				for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
					int entry = i;
					const char* isOccupied = processTable[i].occupied ? "true" : "false";
					const char* tabIfNanosecondsIsZero = processTable[i].startNano == 0 ? "\t" : "";
					printf("%d\t%s\t\t%d\t\t%d\t\t%d\t%s%d\n", entry, isOccupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano, tabIfNanosecondsIsZero, processTable[i].messagesSent);
				}
			}

			/* Finds process to message, then sends message */
			processIndexToMessage = findNextProcessInTable(processIndexToMessage);

			if (processIndexToMessage != -1) {
				/* Set up and send message to child */
				long processPidToMessage = processTable[processIndexToMessage].pid;
				MessageBuffer messageToSend;
				messageToSend.messageType = processPidToMessage;
				messageToSend.value = 1;

				printf("OSS: Sending message to worker %d (PID: %ld) at time %d:%d\n", processIndexToMessage, processPidToMessage, sharedClock[0], sharedClock[1]);
				if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
					perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
					cleanUpSharedMemory();
					exit(1);
				}
				processTable[processIndexToMessage].messagesSent++;
				totalMessagesSent++;

				/* Receive message from child and handle it */
				printf("OSS: Receiving message from worker %d (PID: %ld) at time %d:%d\n", processIndexToMessage, processPidToMessage, sharedClock[0], sharedClock[1]);
				MessageBuffer messageReceived;
				if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer), getpid(), 0) == -1) {
					perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
					cleanUpSharedMemory();
					exit(1);
				}

				if (messageReceived.value == 0) {
					printf("OSS: Worker %d (PID: %ld) is planning to terminate\n", processIndexToMessage, processPidToMessage);
					wait(0);
					removePidFromProcessTable(processPidToMessage);
					instancesRunning--;
				}
			}

			/* Quarter of a second divided by the amount of currently running children. Ternary prevents dividing by 0 errors */
			int NANO_SECONDS_TO_ADD_EACH_LOOP = instancesRunning > 0 ? ONE_BILLION / 4 / instancesRunning : ONE_BILLION / 4;
			addToClock(sharedClock[0], sharedClock[1], 0, NANO_SECONDS_TO_ADD_EACH_LOOP);
		}
	}

	printf("OSS: All child processes have been executed and are finished.\nTotal child processes launched: %d\nTotal messages sent: %d\n", processesAmount, totalMessagesSent);
	cleanUpSharedMemory();
	return EXIT_SUCCESS;
}
