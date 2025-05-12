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

/*
	Verbose parameter. Set to true for more detailed logs.
*/
const bool VERBOSE_LOGS_ENABLED = true;
int timesWrittenToLogs = 0;

const int PROCESS_TABLE_MAX_SIZE = 18;
const int PAGES_PER_PROCESS = 32;
struct PCB {
	bool occupied; // either true or false
	pid_t pid; // process id of this child
	int startSeconds; // time when it was forked
	int startNano; // time when it was forked
	bool blocked = false;
	int blockedAtSeconds = 0; // time when it was blockeds
	int blockedAtNano = 0;
	MessageBuffer requestDetails; // save message's details for when it gets blocked and it creates a new frame later
};
struct PCB processTable[PROCESS_TABLE_MAX_SIZE];

const int FRAME_TABLE_SIZE = 256;
struct Frame {
	bool occupied; // either true or false
	int processIndex; // entry in process table
	int pageNumber;
	bool hasDirtyBit;
	int lastUsedSecond;
	int lastUsedNano;
};
struct Frame frameTable[FRAME_TABLE_SIZE];

const int PARENT_TO_CHILD_MSG_TYPE_OFFSET = 1000000;
struct MessageBuffer {
	long messageType;
	int value;
	int readWrite;
};
int sharedMemoryId;
int* sharedClock;
int messageQueueId;
FILE* logFile = NULL;

const int EVENT_WAIT_TIME_NANO = ONE_BILLION / 1000 * 14;

void printHelp() {
	printf("Usage: oss [-h] [-n proc] [-s simul] [-i interval] [-f logfile]\n");
	printf("-h : Print options for the oss tool and exits\n");
	printf("-n : Total number of processes to run (default: 1)\n");
	printf("-s : Maximum number of simultaneously running processes. Maximum of %d (default: %d)\n", PROCESS_TABLE_MAX_SIZE, PROCESS_TABLE_MAX_SIZE);
	printf("-i : Interval (in ms) to launch child processes (default: 0)\n");
	printf("-f : Specify a filename to save a copy of oss's logs to (disabled by default)\n");
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
void removeIndexFromProcessTable(int index) {
	if (processTable[index].pid == pid && processTable[index].occupied) {
		kill(processTable[index].pid, SIGTERM);
		processTable[index] = { false, 0, 0, 0, false, false };
		return;
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

/* Sets an entry in the frame table to the default struct */
void clearFrame(int index) {
	frameTable[index] = { false, -1, -1, false, 0, 0 };
}

/* Acts as a printf statement that writes to both the console and a file if defined */
void vprintfConsoleAndFile(const char* baseString, va_list args) {
	const int MAX_LOGS_AMOUNT = 100000;
	if (timesWrittenToLogs >= MAX_LOGS_AMOUNT) {
		return;
	}
	timesWrittenToLogs++;

	vprintf(baseString, args);

	if (logFile != NULL) {
		va_list argsCopy;
		va_copy(argsCopy, args);
		vfprintf(logFile, baseString, argsCopy);
		va_end(argsCopy);
		fflush(logFile);
	}
}

/* Wrapper for vprintfConsoleAndFile() that prints regardless of verbose flag */
void printfConsoleAndFile(const char* baseString, ...) {
	va_list args;
	va_start(args, baseString);
	vprintfConsoleAndFile(baseString, args);
	va_end(args);
}

/* Wrapper for vprintfConsoleAndFile() that returns early if verbose is not enabled */
void printfConsoleAndFileVerbose(const char* baseString, ...) {
	if (!VERBOSE_LOGS_ENABLED) {
		return;
	}

	va_list args;
	va_start(args, baseString);
	vprintfConsoleAndFile(baseString, args);
	va_end(args);
}

/* Detaches pointer and clears shared memory at the key */
void cleanUpSharedMemory() {
	/* Clear out all messages in queue */
	MessageBuffer temp;
	while (msgrcv(messageQueueId, &temp, sizeof(MessageBuffer) - sizeof(long), 0, IPC_NOWAIT) != -1) {
	}
	msgctl(messageQueueId, IPC_RMID, NULL);

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

/* Helper function for finding an unoccupied slot in the frame table array. Returns -1 if all are occupied */
int findUnoccupiedFrameTableIndex() {
	for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
		if (!frameTable[i].occupied) {
			return i;
		}
	}
	return -1;
}

int main(int argc, char** argv) {
	const char optstr[] = "hn:s:i:f:";
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
			case 'i':
				forkIntervalMs = processOptarg(optarg);
				forkIntervalEnabled = true;
				break;
			case 'f':
				string systemCall = "touch " + string(optarg);
				system(systemCall.c_str());
				logFile = fopen(optarg, "w");
				if (!logFile) {
					perror("OSS: Fatal error opening log file, terminating...\n");
					exit(1);
				}
		}
	}

	/* Set random seed */
	srand(getpid());

	/* Set up message queue */
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

	/* Set up page table, init everything to -1 */
	int pageTable[PROCESS_TABLE_MAX_SIZE][PAGES_PER_PROCESS];
	for (int i = 0; i < PROCESS_TABLE_MAX_SIZE i++) {
		for (int j = 0; j < PAGES_PER_PROCESS; j++) {
			pageTable[i][j] = -1;
		}
	}

	for (int i = 0; i < FRAME_TABLE_SIZE; i++) {
		clearFrame(i);
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

	/* Interval converted to our clock's units */
	int intervalSeconds = forkIntervalMs / 1000;
	int intervalNano = (forkIntervalMs % 1000) * 1000000;

	/* Set up failsafe that kills the program and its children after 60 seconds */
	signal(SIGALRM, handleFailsafeSignal);
	alarm(5);
	signal(SIGINT, handleFailsafeSignal);

	int instancesRunning = 0;
	int totalInstancesToLaunch = processesAmount;
	int currentProcessIndex = 0;

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
			string arg1 = to_string(rand() % 1000);
			execlp(arg0.c_str(), arg0.c_str(), arg1.c_str(), (char*)0);
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
			}

			/* Checking all blocked processes to add a page */
			for (int i = 0; i < maxSimultaneousProcesses; i++) {
				PCB currentProcess = processTable[i];
				if (currentProcess.occupied && currentProcess.blocked) {
					int unblockSeconds = sharedClock[0];
					int unblockNano = sharedClock[1];

					addToClock(unblockSeconds, unblockNano, 0, EVENT_WAIT_TIME_NANO);

					if (hasTimePassed(sharedClock[0], sharedClock[1], unblockSeconds, unblockNano)) {
						currentProcess.blocked = false;
						int pageRequested = currentProcess.requestDetails.value / 1024;
						bool isRead = currentProcess.requestDetails.readWrite == 0;

						int frameIndex = findUnoccupiedFrameTableIndex();
						frameTable[frameIndex] = { true, i, pageRequested, !isRead, sharedClock[0], sharedClock[1] };
					}
				}
			}

			int processIndex = findNextProcessInTable(currentProcessIndex);
			PCB currentProcess = processTable[processIndex];
			if (currentProcess.occupied) {
				MessageBuffer messageToSend;
				messageToSend.messageType = currentProcess.pid + PARENT_TO_CHILD_MSG_TYPE_OFFSET;
				messageToSend.value = 1;

				if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
					perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
					printf("errno: %d\n", errno);
					handleFailsafeSignal(1);
					exit(1);
				}

				MessageBuffer messageReceived;
				int messageType = currentProcess.pid;
				messageReceived.messageType = messageType;
				if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer) - sizeof(long), messageType, 0) == -1) {
					perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
					handleFailsafeSignal(1);
					exit(1);
				}
				int memoryAddressRequested = messageReceived.value;
				int pageRequested = memoryAddressRequested / 1024;
				bool isRead = messageReceived.readWrite == 0;
				bool isTerminate = messageReceived.readWrite == 2;
				bool pagefault = pageTable[i][pageRequested] == -1;

				/* Received message to terminate*/
				if (isTerminate) {
					removeIndexFromProcessTable(processIndex);
					for (int i = 0; i < PAGES_PER_PROCESS; i++) {
						int frameIndex = pageTable[processIndex][i];
						if (frameIndex > -1) {
							clearFrame(frameIndex);
						}
					}
				} else {
					/* Page requested not already in page table */
					if (pagefault) {
						currentProcess.blocked = true;
						currentProcess.blockedAtSeconds = sharedClock[0];
						currentProcess.blockedAtNano = sharedClock[1];
						currentProcess.requestDetails = messageReceived;

					/* Page requested is in table */
					} else {
						int frameIndex = pageTable[i][pageRequested];
						Frame currentFrame = frameTable[frameIndex];
						currentFrame.hasDirtyBit = currentFrame.hasDirtyBit || !isRead;
						currentFrame.lastUsedSecond = sharedClock[0];
						currentFrame.lastUsedNano = sharedClock[1];

						/* Send message to allow child to proceed */
						MessageBuffer messageToSend;
						messageToSend.messageType = currentProcess.pid + PARENT_TO_CHILD_MSG_TYPE_OFFSET;
						messageToSend.value = 1;

						if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
							perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
							printf("errno: %d\n", errno);
							handleFailsafeSignal(1);
							exit(1);
						}
					}
				}
			}


			/* Add 10 ms */
			addToClock(sharedClock[0], sharedClock[1], 0, ONE_BILLION / 100);
		}
	}

	cleanUpSharedMemory();
	closeLogFileIfOpen();
	return EXIT_SUCCESS;
}
