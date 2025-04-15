#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<random>
#include "clockUtils.h"
using namespace std;

#define SHMKEY 9021011
#define BUFFER_SIZE sizeof(int) * 2

struct MessageBuffer {
	long messageType;
	int value;
};

/* Prepared printf statement that displays clock & process details */
void printProcessDetails(int simClockS, int simClockN, int termTimeS, int termTimeN) {
	printf("WORKER: PID:%d PPID:%d Clock(s):%d Clock(ns):%d Terminate(s):%d Terminate(ns):%d\n", getpid(), getppid(), simClockS, simClockN, termTimeS, termTimeN);
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	int terminateSeconds = 0;
	int terminateNano = 0;

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

	/* Adds simulated clock's time to the passed in duration to get the time to terminate */
	addToClock(terminateSeconds, terminateNano, sharedClock[0], sharedClock[1]);

	printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
	printf("WORKER: Just starting...\n");
	int previousSecond = sharedClock[0];
	int iterationsElapsed = 1;
	bool willTerminate = false;

	while (!willTerminate) {
		/* Wait for message from oss (parent) */
		MessageBuffer messageReceived;
		if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer), getpid(), 0) == -1) {
			perror("WORKER: Fatal error, msgrcv from parent failed, terminating...\n");
			exit(1);
		}
		printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
		printf("WORKER: %d iterations elapsed since starting\n", iterationsElapsed++);
		/* Send message back to parent whether it will terminate or not */
		MessageBuffer messageToSend;
		messageToSend.messageType = getppid();
		messageToSend.value = willTerminate ? 0 : 1;

		if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
			perror("OSS: Fatal error, msgsnd to parent failed, terminating...\n");
			exit(1);
		}
	}
	printProcessDetails(sharedClock[0], sharedClock[1], terminateSeconds, terminateNano);
	printf("WORKER: Time limit reached, terminating...\n");

	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
