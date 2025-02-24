#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<string>
using namespace std;

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

/* Prepared printf statement that appends a custom string to the end */
void printProcessDetails(int i, char* suffix) {
	printf("USER PID:%d PPID:%d Iteration:%d %s\n", getpid(), getppid(), i, suffix);
}

/* Main method, defines the terminal command */
int main(int argc, char** argv) {
	int iterations;
	if (isValidArgument(argv[1])) {
		iterations = atoi(argv[1]);
	} else {
		iterations = 1;
		printf("USER: Invalid or missing argument, defaulting to 1\n");
	}

	for (int i = 1; i <= iterations; i++) {
		printProcessDetails(i, "before sleeping");
		sleep(1);
		printProcessDetails(i, "after sleeping");
	}

	return EXIT_SUCCESS;
}
