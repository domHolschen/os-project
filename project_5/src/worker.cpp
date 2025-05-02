#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include "clockUtils.h"
#include "resourceUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2

struct MessageBuffer {
	long messageType;
	int value;
};

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

/* Returns an int based on what the process will do */
int makeDecision(int resourceAllocatedAmount) {
	int random = rand() % 200;
	/* Terminate */
	if (random == 0) {
		return 2;
	}
	/* Free a resource if it is maxed out */
	if (resourceAllocatedAmount >= 3) {
		return 1;
	}
	/* Free a resource randomly, if able */
	if (random <= 40 && resourceAllocatedAmount > 0) {
		return 1;
	}
	/* Request a resource */
	if (resourceAllocatedAmount < 3) {
		return 0;
	}
	
	/* Not intended to reach here. this is a failsafe and won't do anything */
	return -1;
}

/* Prepared printf statement that displays clock & process details */
void printProcessDetails(int simClockS, int simClockN, int termTimeS, int termTimeN) {
	printf("WORKER: PID:%d PPID:%d Clock(s):%d Clock(ns):%d Terminate(s):%d Terminate(ns):%d\n", getpid(), getppid(), simClockS, simClockN, termTimeS, termTimeN);
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	/* Set random seed */
	srand(getpid());

	/* Interval for worker to decide what it will do */
	int nextDecisionIntervalNano = 0;
	if (argc > 1 && isValidArgument(argv[1])) {
		nextDecisionIntervalNano = atoi(argv[1]) * 1000;
	}
	else {
		nextDecisionIntervalNano = ONE_BILLION / 1000;
		printf("WORKER: Invalid or missing interval argument, defaulting to 1 ms\n");
	}

	/* Set up resources */
	int* allocatedResources = new int[RESOURCE_TYPES_AMOUNT];
	for (int i = 0; i < RESOURCE_TYPES_AMOUNT; i++) {
		allocatedResources[i] = 0;
	}

	/* Set up message queue */
	int messageQueueId;
	key_t key;
	system("touch keyfile.txt");

	if ((key = ftok("keyfile.txt", 1)) == -1) {
		perror("WORKER: Fatal error generating key using keyfile.txt, terminating...\n");
		exit(1);
	}

	if ((messageQueueId = msgget(key, 0644 | IPC_CREAT)) == -1) {
		perror("WORKER: Fatal error getting message queue ID, terminating...\n");
		exit(1);
	}

	/* Set up shared memory & attach */
	int sharedMemoryId = shmget(SHMKEY, BUFFER_SIZE, 0777 | IPC_CREAT);
	if (sharedMemoryId == -1) {
		fprintf(stderr, "WORKER: Error defining shared memory, terminating...\n");
		exit(1);
	}
	int* sharedClock = (int*)(shmat(sharedMemoryId, 0, 0));
	if (sharedClock == (void*)-1) {
		perror("WORKER: Failed to attach shared memory");
		exit(1);
	}

	int nextDecisionSeconds = 0;
	int nextDecisionNano = nextDecisionIntervalNano;
	bool willTerminate = false;
	bool blocked = false;
	int requestedResource = -1;

	/* Adds simulated clock's time to the passed in duration to get the time to make a decision */
	addToClock(nextDecisionSeconds, nextDecisionNano, sharedClock[0], sharedClock[1]);

	printf("WORKER: Just starting...\n");

	while (!willTerminate) {
		if (blocked) {
			MessageBuffer blockedMessageReceived;
			if (msgrcv(messageQueueId, &blockedMessageReceived, sizeof(MessageBuffer) - sizeof(long), getpid(), IPC_NOWAIT) == -1) {
				if (errno == ENOMSG) {
					continue;
				}
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
			allocatedResources[requestedResource]++;
			blocked = false;
			continue;
		}
		int decisionCode = -1;
		bool canRequestResource;
		int resourceIndex;
		if (hasTimePassed(sharedClock[0], sharedClock[1], nextDecisionSeconds, nextDecisionNano)) {
			resourceIndex = rand() % 5;
			decisionCode = makeDecision(allocatedResources[resourceIndex]);
			nextDecisionSeconds = sharedClock[0];
			nextDecisionNano = sharedClock[1];
			addToClock(nextDecisionSeconds, nextDecisionNano, 0, nextDecisionIntervalNano);
		}
		else {
			continue;
		}

		MessageBuffer messageToSend;
		messageToSend.messageType = getpid();
		/* Request a resource*/
		printf("R0: %d R1: %d R2: %d R3: %d R4: %d\n", allocatedResources[0], allocatedResources[1], allocatedResources[2], allocatedResources[3], allocatedResources[4]);

		if (decisionCode == 0) {
			/* Send message back to parent to request resource */
			messageToSend.value = resourceIndex;
			printf("WORKER: Sending message with value %d\n", messageToSend.value);
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}

			requestedResource = resourceIndex;
			allocatedResources[resourceIndex]++;

			/* Wait to receive a message from oss that the resource was granted */
			MessageBuffer messageReceived;
			if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer) - sizeof(long), getpid() + 1000000, 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
			blocked = false;
			requestedResource = -1;

		}
		/* Free a resource*/
		if (decisionCode == 1) {
			/* Send message back to parent to free resource */
			messageToSend.value = resourceIndex + RESOURCE_TYPES_AMOUNT;
			printf("WORKER: Sending message with value %d\n", messageToSend.value);

			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
			allocatedResources[resourceIndex]--;
		}
		/* Termination */
		if (decisionCode == 2) {
			/* Send message back to parent to terminate */
			messageToSend.value = -1;
			printf("WORKER: Sending message with value %d\n", messageToSend.value);

			willTerminate = true;
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
		}
	}
	delete[] allocatedResources;

	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
