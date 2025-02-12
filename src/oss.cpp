#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/wait.h>
#include <string>
using namespace std;

void printHelp() {
	printf("Usage: oss [-h] [-n proc] [-s simul] [-t iter]");
	printf("-h : Print options for command");
	printf("-n : Total number of processes to run (default: 1)");
	printf("-s : Maximum number of simultaneously running processes (disabled by default)");
	printf("-t : Amount of iterations each child process will run through before terminating (default: 1)");
}

int main(int argc, char** argv) {
	const char optstr[] = "hn:s:t:";
	char opt;
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
				processesAmount = atoi(optarg);
				break;
			case 's':
				maxSimultaneousProcesses = atoi(optarg);
				simultaneousLimitEnabled = true;
				break;
			case 't':
				iterationsAmount = atoi(optarg);
				break;
		}
	}
	pid_t childPid = fork();
	if (childPid == 0) {
		printf("I am OSS but a copy of parent! My parent's PID is %d, and my PID is % d\n", getppid(), getpid());
		string arg0 = "./user";
		string arg1 = "Hello";
		string arg2 = "there";
		string arg3 = "exec";
		string arg4 = "is";
		string arg5 = "neat";
		execlp(arg0.c_str(), arg0.c_str(), arg1.c_str(), arg2.c_str(), arg3.c_str(), arg4.c_str(
		), arg5.c_str(), (char*)0);
		fprintf(stderr, "Exec failed, terminating\n");
		exit(1);
	}
	else {
		printf("I'm a parent! My pid is %d, and my child's pid is %d \n", getpid(), childPid);
		wait(0);
	}
	printf("Parent is now ending.\n");
	return EXIT_SUCCESS;
}
