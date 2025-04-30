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
#include "resourceUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2
const int PROCESS_TABLE_MAX_SIZE = 18;

struct PCB {
	bool occupied; // either true or false
	pid_t pid; // process id of this child
	int startSeconds; // time when it was forked
	int startNano; // time when it was forked
	int messagesSent; // number of messages sent to the process
	bool blocked; // if the process is blocked by an unfulfilled request
};
struct PCB processTable[PROCESS_TABLE_MAX_SIZE];
struct MessageBuffer {
	long messageType;
	int value;
};
int sharedMemoryId;
int* sharedClock;
int messageQueueId;
FILE* logFile = NULL;

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

	/* Interval converted to our clock's units */
	int intervalSeconds = forkIntervalMs / 1000;
	int intervalNano = (forkIntervalMs % 1000) * 1000000;

	/* Create resources structure */
	array<Descriptor, RESOURCE_TYPES_AMOUNT> resourcesArray = createResources();
	Descriptor* resources = resourcesArray.data();

	/* Set up failsafe that kills the program and its children after 60 seconds */
	signal(SIGALRM, handleFailsafeSignal);
	alarm(5);
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
				addToClock(pcbTimerSeconds, pcbTimerNano, 10, ONE_BILLION / 2);
				printfConsoleAndFile("Process table:\n\tR0\tR1\tR2\tR3\tR4\n");
				for (int i = 0; i < maxSimultaneousProcesses; i++) {
					const char* isOccupied = processTable[i].occupied ? "true" : "false";
					const char* tabIfNanosecondsIsZero = processTable[i].startNano == 0 ? "\t" : "";
					printfConsoleAndFile("P%d\t%d\t%d\t%d\t%d\t%d\n", i, resources[0].allocated[i], resources[1].allocated[i], resources[2].allocated[i], resources[3].allocated[i], resources[4].allocated[i]);
				}
			}
			
			/* First looks at blocked processes */
			for (int i = 0; i < MAX_PROCESSES_AMOUNT; i++) {
				if (!processTable[i].occupied || processTable[i].blocked) {
					continue;
				}
				
				for (int j = 0; j < RESOURCE_TYPES_AMOUNT; j++) {
					if (resources[j].requested[i] > 0 && resources[j].availableInstances > 0) {
						printfConsoleAndFile("OSS: blocked worker %d (PID %d) requested resource %d and is granted\n", i, processTable[i].pid, j);
						long processPidToMessage = processTable[i].pid;
						MessageBuffer messageToSend;
						messageToSend.messageType = processPidToMessage;
						messageToSend.value = 1;
						if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
							perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
							printf("errno: %d\n", errno);
							cleanUpSharedMemory();
							closeLogFileIfOpen();
							msgctl(messageQueueId, IPC_RMID, NULL);
							exit(1);
						}
						resources[j].requested[i]--;
						allocateToProcess(resources[j], i);
					}
				}
			}

			/* Finds process to message, then sends message */
			for (int i = 0; i < MAX_PROCESSES_AMOUNT; i++) {
				if (!processTable[i].occupied || processTable[i].blocked) {
					continue;
				}
				MessageBuffer messageReceived;
				if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer) - sizeof(long), processTable[i].pid, IPC_NOWAIT) == -1) {
					/* No message received - skip remaining code in loop and go to next iteration */
					if (errno == ENOMSG) {
						continue;
					}
					else {
						perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
						cleanUpSharedMemory();
						closeLogFileIfOpen();
						msgctl(messageQueueId, IPC_RMID, NULL);
						exit(1);
					}
				}
				/* Terminate */
				if (messageReceived.value == -1) {
					printfConsoleAndFile("OSS: worker %d (PID %d) will terminate\n", i, processTable[i].pid);
					/* Discard messages */
					MessageBuffer temp;
					while (msgrcv(messageQueueId, &temp, sizeof(MessageBuffer) - sizeof(long), processTable[i].pid, IPC_NOWAIT) != -1) {
					}
					freeProcess(resources, i);
					removePidFromProcessTable(processTable[i].pid);
					instancesRunning--;
				}
				/* Request */
				else if (messageReceived.value < RESOURCE_TYPES_AMOUNT) {
					bool successful = allocateToProcess(resources[messageReceived.value], i);
					if (successful) {
						printfConsoleAndFile("OSS: worker %d (PID %d) requested resource %d and is granted\n", i, processTable[i].pid, messageReceived.value);
						long processPidToMessage = processTable[i].pid;
						MessageBuffer messageToSend;
						messageToSend.messageType = processPidToMessage;
						messageToSend.value = 1;
						if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
							perror("OSS: Fatal error, msgsnd to child failed, terminating...\n");
							printf("errno: %d\n", errno);
							cleanUpSharedMemory();
							closeLogFileIfOpen();
							msgctl(messageQueueId, IPC_RMID, NULL);
							exit(1);
						}
					}
					/* Unable to request resource */
					else {
						printfConsoleAndFile("OSS: worker %d (PID %d) requested resource %d but not available, blocking...\n", i, processTable[i].pid, messageReceived.value);
						resources[messageReceived.value].requested[i]++;
						processTable[i].blocked = true;
					}
				}
				/* Free - freeing should always be successful when received by oss */
				else {
					int resourceId = messageReceived.value - RESOURCE_TYPES_AMOUNT;
					printfConsoleAndFile("OSS: worker %d (PID %d) is freeing one of its resources %d\n", i, processTable[i].pid, resourceId);
					freeFromProcess(resources[resourceId],i);
				}
			}
			/* Add 10 ms */
			addToClock(sharedClock[0], sharedClock[1], 0, ONE_BILLION / 100);
		}
	}

	printfConsoleAndFile("OSS: All child processes have been executed and are finished.\nTotal child processes launched: %d\nTotal messages sent: %d\n", processesAmount, totalMessagesSent);
	cleanUpSharedMemory();
	closeLogFileIfOpen();
	return EXIT_SUCCESS;
}
