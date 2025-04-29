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
	/* Free a resource, only if able, but forced to free if it is maxed out */
	if ((random <= 40 && resourceAllocatedAmount > 0) || resourceAllocatedAmount >= RESOURCE_INSTANCES_AMOUNT) {
		return 1;
	}
	/* Request a resource */
	return 0;
}

/* Prepared printf statement that displays clock & process details */
void printProcessDetails(int simClockS, int simClockN, int termTimeS, int termTimeN) {
	printf("WORKER: PID:%d PPID:%d Clock(s):%d Clock(ns):%d Terminate(s):%d Terminate(ns):%d\n", getpid(), getppid(), simClockS, simClockN, termTimeS, termTimeN);
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	/* Interval for worker to decide what it will do */
	int nextDecisionIntervalNano = 0;
	if (isValidArgument(argv[1])) {
		nextDecisionIntervalNano = atoi(argv[1]) * 1000;
	}
	else {
		nextDecisionIntervalNano = ONE_BILLION / 1000;
		printf("WORKER: Invalid or missing interval argument, defaulting to 1 ms\n");
	}

	/* Set up resources */
	int allocatedResources[RESOURCE_TYPES_AMOUNT];
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

	int nextDecisionSeconds = 0;
	int nextDecisionNano = nextDecisionIntervalNano;
	bool willTerminate = false;

	/* Adds simulated clock's time to the passed in duration to get the time to make a decision */
	addToClock(nextDecisionSeconds, nextDecisionNano, sharedClock[0], sharedClock[1]);

	printf("WORKER: Just starting...\n");

	while (!willTerminate) {
		int decisionCode = -1;
		bool canRequestResource;
		int resourceIndex;
		if (hasTimePassed(sharedClock[0], sharedClock[1], nextDecisionSeconds, nextDecisionNano)) {
			resourceIndex = rand() % 5;
			decisionCode = makeDecision(allocatedResources[resourceIndex]);
			addToClock(nextDecisionSeconds, nextDecisionNano, 0, nextDecisionIntervalNano);
		}

		/* Request a resource*/
		if (decisionCode == 0) {
			/* Send message back to parent to request resource */
			MessageBuffer messageToSend;
			messageToSend.messageType = getppid();
			messageToSend.value = resourceIndex;
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}

			/* Wait to receive a message from oss that the resource was granted */
			MessageBuffer messageReceived;
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
		}
		/* Free a resource*/
		if (decisionCode == 1) {
			/* Send message back to parent to free resource */
			MessageBuffer messageToSend;
			messageToSend.messageType = getppid();
			messageToSend.value = resourceIndex + RESOURCE_TYPES_AMOUNT;
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
		}
		/* Termination */
		if (decisionCode == 2) {
			/* Send message back to parent to terminate */
			MessageBuffer messageToSend;
			messageToSend.messageType = getppid();
			messageToSend.value = -1;
			willTerminate = true;
			if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
				perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
				exit(1);
			}
		}
	}

	printf("WORKER: Time limit reached, terminating...\n");

	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
