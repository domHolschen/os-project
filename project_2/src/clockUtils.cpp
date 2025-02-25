#include "clockUtils.h"
#include <cstdio>

/* Takes in seconds + nano by reference and increments them based 3rd and 4th parameters*/
void addToClock(int& seconds, int& nano, int secondsToAdd, int nanoToAdd) {
	const int ONE_BILLION = 1000000000;

	seconds += secondsToAdd;
	nano += nanoToAdd;

	if (nano >= ONE_BILLION) {
		seconds += 1;
		nano = nano % ONE_BILLION;
	}
	else if (nano < 0) {
		seconds -= 1;
		nano = nano % ONE_BILLION;
	}
}

/* Compares times to see if the current time has passed the specified end time */
bool hasTimePassed(int currentSeconds, int currentNano, int endSeconds, int endNano) {
	if (currentSeconds == endSeconds) {
		return currentNano >= endNano;
	}
	return currentSeconds > endSeconds;
}