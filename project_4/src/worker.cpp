#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<string>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<cstdlib>
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

/* Randomly generate true/false based on percentage */
bool percentChance(int percentage) {
	return rand() % 100 < percentage;
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	/* Set up random seed based on pid */
	srand(getpid());

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
	
	while (true) {
		/* Wait for message from oss (parent) */
		MessageBuffer messageReceived;
		if (msgrcv(messageQueueId, &messageReceived, sizeof(MessageBuffer), getpid(), 0) == -1) {
			perror("WORKER: Fatal error, msgrcv from parent failed, terminating...\n");
			exit(1);
		}
		int timeScheduled = messageReceived.value;

		/* Rolls whether the process will terminate or get blocked */
		bool willTerminate = percentChance(10);
		bool willGetBlockedByIO = percentChance(30);

		/* If planning to terminate or get blocked, randomly truncate quantum */
		if (willTerminate || willGetBlockedByIO) {
			int p = rand() % 99 + 1;
			timeScheduled /= 100;
			timeScheduled *= p;
		}

		/* Send message back to parent */
		MessageBuffer messageToSend;
		messageToSend.messageType = getppid();
		/* Sends time executed to parent as negative if it ends up terminating */
		messageToSend.value = willTerminate ? -timeScheduled : timeScheduled;

		if (msgsnd(messageQueueId, &messageToSend, sizeof(MessageBuffer) - sizeof(long), 0) == -1) {
			perror("WORKER: Fatal error, msgsnd to parent failed, terminating...\n");
			exit(1);
		}

		if (willTerminate) {
			break;
		}
	}

	/* Clean up shared memory */
	shmdt(sharedClock);

	return EXIT_SUCCESS;
}
