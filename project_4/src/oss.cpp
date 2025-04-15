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
#include<stdarg.h>
#include "clockUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2
const int PROCESS_TABLE_MAX_SIZE = 20;
const int ONE_BILLION = 1000000000;
const int MAX_TIME_BETWEEN_FORK_NANO = ONE_BILLION - 1;
const int MAX_TIME_BETWEEN_FORK_SECS = 1;

struct PCB {
	bool occupied; // either true or false
	pid_t pid; // process id of this child
	int startSeconds; // time when it was forked
	int startNano; // time when it was forked
	int messagesSent; // number of messages sent to the process
	int serviceTimeSeconds; // total seconds it has been "scheduled"
	int serviceTimeNano; // total nanoseconds it has been "scheduled"
	int eventWaitSec; // when does its event happen?
	int eventWaitNano; // when does its event happen?
	bool blocked; // is this process waiting on event?
};
struct PCB processTable[PROCESS_TABLE_MAX_SIZE];
struct MessageBuffer {
	long messageType;
	int value;
};
int sharedMemoryId;
int* sharedClock;
FILE* logFile = NULL;
struct Queue {
	pid_t[] pids;
	int count;
	int quantumNano;
	int quantumSeconds;
};
struct Queue queues[3];

void printHelp() {
	printf("Usage: oss [-h]\n");
	printf("-h : Print options for the oss tool and exits\n");
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

/* Acts as a printf statement that writes to both the console and a file if defined */
void printfConsoleAndFile(const char* baseString, ...) {
	va_list args;

	va_start(args, baseString);
	vprintf(baseString, args);
	va_end(args);

	if (logFile == NULL) {
		return;
	}

	va_start(args, baseString);
	vfprintf(logFile, baseString, args);
	va_end(args);
	fflush(logFile);
}

/* Detaches pointer and clears shared memory at the key */
void cleanUpSharedMemory() {
	shmdt(sharedClock);
	shmctl(sharedMemoryId, IPC_RMID, NULL);
}

void closeLogFileIfOpen() {
	if (logFile != NULL) {
		fclose(logFile);
	}
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
	closeLogFileIfOpen();

	exit(1);
}

int main(int argc, char** argv) {
	const char optstr[] = "h";
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
			}
	}

	/* Set up message queue */
	int messageQueueId;
	key_t key;
	system("touch keyfile.txt");

	if ((key = ftok("keyfile.txt", 1)) == -1) {
		perror("OSS: Fatal error generating key using keyfile.txt, terminating...\n");
		closeLogFileIfOpen();
		exit(1);
	}

	if ((messageQueueId = msgget(key, 0644 | IPC_CREAT)) == -1) {
		perror("OSS: Fatal error getting message queue ID, terminating...\n");
		closeLogFileIfOpen();
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
		perror("OSS: Error defining shared memory");
		closeLogFileIfOpen();
		msgctl(messageQueueId, IPC_RMID, NULL);
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
			execlp(arg0.c_str(), arg0.c_str(), (char*)0);
			perror("OSS: Launching worker failed, terminating\n");
			exit(1);
			/* Parent process - waits for children to terminate */
		} else {
			/* Adds child process to table if one was forked this iteration */
			if (shouldAddToProcessTable) {
				int index = findUnoccupiedProcessTableIndex();
				if (index == -1) {
					printfConsoleAndFile("OSS: No unoccupied entry in process table found. Continuing child execution\n");
				}
				PCB newPcb = { true, childPid, sharedClock[0], sharedClock[1] };
				processTable[index] = newPcb;
			}

			/* Printing PCB table */
			if (hasTimePassed(sharedClock[0], sharedClock[1], pcbTimerSeconds, pcbTimerNano)) {
				addToClock(pcbTimerSeconds, pcbTimerNano, 0, ONE_BILLION / 2);
				printfConsoleAndFile("Process table:\nEntry\tOccupied?\tPID\t\tStart(s)\tStart(ns)\tMessages Sent\n");
				for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
					int entry = i;
					const char* isOccupied = processTable[i].occupied ? "true" : "false";
					const char* tabIfNanosecondsIsZero = processTable[i].startNano == 0 ? "\t" : "";
					printfConsoleAndFile("%d\t%s\t\t%d\t\t%d\t\t%d\t%s%d\n", entry, isOccupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano, tabIfNanosecondsIsZero, processTable[i].messagesSent);
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

				printfConsoleAndFile("OSS: Sending message to worker %d (PID: %ld) at time %d:%d\n", processIndexToMessage, processPidToMessage, sharedClock[0], sharedClock[1]);
				if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
					perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
					cleanUpSharedMemory();
					closeLogFileIfOpen();
					msgctl(messageQueueId, IPC_RMID, NULL);
					exit(1);
				}
				processTable[processIndexToMessage].messagesSent++;
				totalMessagesSent++;

				/* Receive message from child and handle it */
				printfConsoleAndFile("OSS: Receiving message from worker %d (PID: %ld) at time %d:%d\n", processIndexToMessage, processPidToMessage, sharedClock[0], sharedClock[1]);
				MessageBuffer messageReceived;
				if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer), getpid(), 0) == -1) {
					perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
					cleanUpSharedMemory();
					closeLogFileIfOpen();
					msgctl(messageQueueId, IPC_RMID, NULL);
					exit(1);
				}

				if (messageReceived.value == 0) {
					printfConsoleAndFile("OSS: Worker %d (PID: %ld) is planning to terminate\n", processIndexToMessage, processPidToMessage);
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

	printfConsoleAndFile("OSS: All child processes have been executed and are finished.\nTotal child processes launched: %d\nTotal messages sent: %d\n", processesAmount, totalMessagesSent);
	cleanUpSharedMemory();
	closeLogFileIfOpen();
	msgctl(messageQueueId, IPC_RMID, NULL);
	return EXIT_SUCCESS;
}
