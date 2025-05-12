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
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2

const int PARENT_TO_CHILD_MSG_TYPE_OFFSET = 1000000;

struct MessageBuffer {
	long messageType;
	int value;
	int readWrite;
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

/* Prepared printf statement that displays clock & process details */
void printProcessDetails(int simClockS, int simClockN, int termTimeS, int termTimeN) {
	printf("WORKER: PID:%d PPID:%d Clock(s):%d Clock(ns):%d Terminate(s):%d Terminate(ns):%d\n", getpid(), getppid(), simClockS, simClockN, termTimeS, termTimeN);
}

/* Function to wait for a message from parent */
void waitForMessage() {
	MessageBuffer messageReceived;
	int messageType = currentProcess.pid + PARENT_TO_CHILD_MSG_TYPE_OFFSET;
	messageReceived.messageType = messageType;
	if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer) - sizeof(long), messageType, 0) == -1) {
		perror("OSS: Fatal error, msgrcv from child failed, terminating...\n");
		handleFailsafeSignal(1);
		exit(1);
	}
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
	int allocatedResources[RESOURCE_TYPES_AMOUNT] = { 0 };

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

	bool willTerminate = false;
	/* Performs 900-1100 refs before terminating */
	int refsBeforeTermination = rand() % 900 + 200;

	/* Adds simulated clock's time to the passed in duration to get the time to make a decision */
	addToClock(nextDecisionSeconds, nextDecisionNano, sharedClock[0], sharedClock[1]);

	printf("WORKER: Just starting...\n");

	while (!willTerminate) {
		/* Wait for message from parent before proceeding */
		waitForMessage();

		/* 80% read, 20% write */
		bool shouldRead = rand() % 5 > 0;

		int pageNumber = rand() % 32;
		int byteOffset = rand() % 1024;

		MessageBuffer messageToSend;
		messageToSend.messageType = getpid();
		messageToSend.value = pageNumber * 1024 + byteOffset;
		/* Message to terminate */
		if (refsBeforeTermination <= 0) {
			messageToSend.readWrite = 2;
			willTerminate = true;
			break;
		}
		else {
			messageToSend.readWrite = shouldRead ? 0 : 1;
		}

		if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
			perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
			exit(1);
		}

		refsBeforeTermination--;

		/* Wait for message from parent before doing anything */
		waitForMessage();
	}
	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
