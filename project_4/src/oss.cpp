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
const int QUEUE_LEVELS = 3;
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
	int eventUnblockSeconds; // when does its event happen?
	int eventUnblockNano; // when does its event happen?
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

struct ProcessQueue {
	int* pcbTableEntry;
	int front;
	int back;
	int quantumSeconds;
	int quantumNano;
	int currentSize = 0;
	int maxSize;
};

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

/* Put an int (the index of the PCB table) inside of a provided queue struct */
void queuePush(int pcbIndex, ProcessQueue& queue) {
	int location = (queue.back + 1) % PROCESS_TABLE_MAX_SIZE;
	queue.pcbTableEntry[location] = pcbIndex;
	queue.back = location;
	queue.currentSize++;
}

/* Remove head of queue and return the value of it */
int queuePop(ProcessQueue& queue) {
	int value = queue.pcbTableEntry[queue.front];
	if (value == -1) {
		perror("ERROR popped -1\n");
	}
	queue.pcbTableEntry[queue.front] = -1;
	queue.front = (queue.front + 1) % PROCESS_TABLE_MAX_SIZE;
	queue.currentSize--;
	return value;
}

/* Get a float between 0 and max */
float randFloat(float max) {
	return static_cast<float>(rand()) / RAND_MAX * max;
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

	/* Process options */
	while ((opt = getopt(argc, argv, optstr)) != -1) {
		switch (opt) {
			case 'h':
				printHelp();
				return EXIT_SUCCESS;
			}
	}

	srand(getpid());
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

	/* Set up scheduler queues */
	ProcessQueue queues[QUEUE_LEVELS];
	int queueQuantumSeconds = 0;
	int queueQuantumNano = ONE_BILLION / 100;
	for (int i = 0; i < QUEUE_LEVELS; i++) {
		/* Initialize all indexes in queues with -1 to indicate unoccupied */
		queues[i] = { new int[PROCESS_TABLE_MAX_SIZE], 0, -1, queueQuantumSeconds, queueQuantumNano, 0, PROCESS_TABLE_MAX_SIZE };
		for (int j = 0; j < PROCESS_TABLE_MAX_SIZE; j++) {
			queues[i].pcbTableEntry[j] = -1;
		}
		/* doubles queue quantum duration */
		addToClock(queueQuantumSeconds, queueQuantumNano, queueQuantumSeconds, queueQuantumNano);
	}
	/* Since blocked queue is not FIFO we just use a normal array */
	int blockedQueue[20];
	for (int j = 0; j < PROCESS_TABLE_MAX_SIZE; j++) {
		blockedQueue[j] = -1;
	}

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

	/* Maximum time interval between forking children */
	int maxTimeBetweenForkSeconds = 0;
	int maxTimeBetweenForkNano = ONE_BILLION / 5;

	/* Timers for statistics */

	/* Set up failsafe that kills the program and its children after 3 (real life) seconds */
	signal(SIGALRM, handleFailsafeSignal);
	alarm(3);
	signal(SIGINT, handleFailsafeSignal);

	int instancesRunning = 0;
	int instancesBlocked = 0;
	int totalInstancesToLaunch = 100;

	int instancesTerminated = 0;

	while (totalInstancesToLaunch > 0 || instancesRunning > 0) {
		pid_t childPid = -1;
		bool shouldAddToProcessTable = false;

		/* Determining if child should be created */
		bool shouldFork = totalInstancesToLaunch > 0 && instancesRunning < PROCESS_TABLE_MAX_SIZE;
		bool readyToFork = hasTimePassed(sharedClock[0], sharedClock[1], readyToForkSeconds, readyToForkNano);
		if (shouldFork && readyToFork) {
			
			/* Creating child */
			totalInstancesToLaunch--;
			instancesRunning++;
			shouldAddToProcessTable = true;

			readyToForkSeconds = sharedClock[0];
			readyToForkNano = sharedClock[1];

			/* Uniformly deciding interval between next fork */
			float maxTimeBetweenForkSecondsFloat = maxTimeBetweenForkSeconds + (float) maxTimeBetweenForkNano / ONE_BILLION;
			float timeBetweenForkSecondsFloat = randFloat(maxTimeBetweenForkSecondsFloat);
			/* truncates decimal */
			int timeBetweenForkSeconds = timeBetweenForkSecondsFloat;
			int timeBetweenForkNano = (timeBetweenForkSecondsFloat - timeBetweenForkSeconds) * ONE_BILLION;
			addToClock(readyToForkSeconds, readyToForkNano, timeBetweenForkSeconds, timeBetweenForkNano);

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
				queuePush(index, queues[0]);
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

				/* Printing queues*/
				for (int i = 0; i < QUEUE_LEVELS; i++) {
					printfConsoleAndFile("q%d:", i);
					for (int j = 0; j < queues[i].currentSize; j++) {
						int idx = (queues[i].front + j) % PROCESS_TABLE_MAX_SIZE;
						printfConsoleAndFile(" %d", queues[i].pcbTableEntry[idx]);
						if (idx == queues[i].back) {
							break;
						}
					}
					printfConsoleAndFile("\n");
				}
				printfConsoleAndFile("blocked: ");
				for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
					if (blockedQueue[i] != -1) {
						printfConsoleAndFile("%d ", blockedQueue[i]);
					}
				}
				printfConsoleAndFile("\n");
			}

			/* Scans blocked queue and puts a worker in ready state if it can */
			for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
				int processIndex = blockedQueue[i];
				if (processIndex != -1) {
					PCB blockedProcess = processTable[processIndex];
					if (blockedProcess.blocked && hasTimePassed(sharedClock[0], sharedClock[1], blockedProcess.eventUnblockSeconds, blockedProcess.eventUnblockNano)) {
						blockedProcess.blocked = false;
						blockedQueue[i] = -1;
						instancesBlocked--;
						queuePush(processIndex, queues[0]);
						printfConsoleAndFile("OSS: Worker %d unblocked\n", processIndex);

						/* Add 1ms to the clock */
						addToClock(sharedClock[0], sharedClock[1], 0, ONE_BILLION / 1000);
					}
				}
			}

			/*If nothing to do*/
			if (instancesRunning - instancesBlocked == 0) {
				printfConsoleAndFile("OSS: Nothing to do, incrementing clock by 100 ms\n");
				addToClock(sharedClock[0], sharedClock[1], 0, ONE_BILLION / 10);
				continue;
			}

			/* Finds next process to execute, then sends message */
			int processIndexToExecute = -1;
			int currentQueueLevel = 0;
			/* Starts from highest priority to find a queue that is not empty, then pops it */
			for (int i = 0; i < QUEUE_LEVELS; i++) {
				if (queues[i].currentSize > 0) {
					processIndexToExecute = queuePop(queues[i]);
					currentQueueLevel = i;
					break;
				}
			}

			if (processIndexToExecute != -1) {
				/* Set up and send message to child */
				long processPidToMessage = processTable[processIndexToExecute].pid;
				MessageBuffer messageToSend;
				messageToSend.messageType = processPidToMessage;
				int quantum = queues[currentQueueLevel].quantumNano;
				messageToSend.value = quantum;

				if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
					perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
					cleanUpSharedMemory();
					closeLogFileIfOpen();
					msgctl(messageQueueId, IPC_RMID, NULL);
					exit(1);
				}

				/* Receive message from child and handle it */
				MessageBuffer messageReceived;
				if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer), getpid(), 0) == -1) {
					perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
					cleanUpSharedMemory();
					closeLogFileIfOpen();
					msgctl(messageQueueId, IPC_RMID, NULL);
					exit(1);
				}
				int childExecutionTime = messageReceived.value;
				/* Terminated */
				if (childExecutionTime < 0) {
					printfConsoleAndFile("OSS: Worker %d (PID: %ld) is planning to terminate\n", processIndexToExecute, processPidToMessage);
					wait(0);
					removePidFromProcessTable(processPidToMessage);
					instancesRunning--;
					addToClock(sharedClock[0], sharedClock[1], 0, -childExecutionTime);
				}
				else {
					/* Blocked by I/O event */
					if (childExecutionTime < quantum) {
						printfConsoleAndFile("OSS: Worker %d (PID: %ld) did not use full quantum and was blocked\n", processIndexToExecute, processPidToMessage);
						instancesBlocked++;
						processTable[processIndexToExecute].blocked = true;
						processTable[processIndexToExecute].eventUnblockSeconds = rand() % 6;
						processTable[processIndexToExecute].eventUnblockNano = rand() % 1001;
						for (int i = 0; i < PROCESS_TABLE_MAX_SIZE; i++) {
							if (blockedQueue[i] == -1) {
								blockedQueue[i] = processIndexToExecute;
								break;
							}
						}
					}
					/* Used full quantum */
					else {
						printfConsoleAndFile("OSS: Worker %d (PID: %ld) executed for full quantum\n", processIndexToExecute, processPidToMessage);
						/* Add to next/lowest queue */
						int maxQueueLevel = QUEUE_LEVELS - 1;
						int nextQueueLevel = currentQueueLevel == maxQueueLevel ? currentQueueLevel : currentQueueLevel + 1;
						queuePush(processIndexToExecute, queues[nextQueueLevel]);
					}
					addToClock(sharedClock[0], sharedClock[1], 0, messageReceived.value);
				}
			}
		}
	}

	printf("OSS: Finished\n");
	cleanUpSharedMemory();
	closeLogFileIfOpen();
	msgctl(messageQueueId, IPC_RMID, NULL);
	return EXIT_SUCCESS;
}
