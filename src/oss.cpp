#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/wait.h>
#include <string>
using namespace std;

void printHelp() {
	printf("Usage: oss [-h] [-n proc] [-s simul] [-t iter]\n");
	printf("-h : Print options for the oss tool and exits\n");
	printf("-n : Total number of processes to run (default: 1)\n");
	printf("-s : Maximum number of simultaneously running processes (disabled by default)\n");
	printf("-t : Amount of iterations each child process will run through before terminating (default: 1)\n");
}

/* Takes in optarg and returns int. Defaults to 1 if out of bounds */
int processOptarg(const char* optarg) {
	int argAsInt = atoi(optarg);
	return argAsInt >= 1 ? argAsInt : 1;
}

int main(int argc, char** argv) {
	const char optstr[] = "hn:s:t:";
	char opt;

	/* Default parameters */
	int processesAmount = 1;
	int maxSimultaneousProcesses = 1;
	bool simultaneousLimitEnabled = false;
	int iterationsAmount = 1;

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
				break;
			case 't':
				iterationsAmount = processOptarg(optarg);
				break;
		}
	}

	if (maxSimultaneousProcesses < processesAmount) {
		simultaneousLimitEnabled = true;
	}

	int instancesToWaitFor = 0;
	int totalInstancesToLaunch = processesAmount;
	while (totalInstancesToLaunch > 0) {
		instancesToWaitFor++;
		totalInstancesToLaunch--;
		pid_t childPid = fork();

		/* Child process - launches user */
		if (childPid == 0) {
			string arg0 = "./user";
			string arg1 = to_string(iterationsAmount);
			execlp(arg0.c_str(), arg0.c_str(), arg1.c_str(), (char*)0);
			fprintf(stderr, "Launching user failed, terminating\n");
			exit(1);
			/* Parent process - waits for children to terminate */
		} else {
			bool shouldWait = totalInstancesToLaunch == 0 || (simultaneousLimitEnabled && instancesToWaitFor >= maxSimultaneousProcesses);
			if (shouldWait) {
				for (int i = 0; i < instancesToWaitFor; i++) {
					wait(0);
				}
				instancesToWaitFor = 0;
			}
		}
	}

	return EXIT_SUCCESS;
}
