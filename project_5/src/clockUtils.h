#ifndef CLOCKUTILS_H
#define CLOCKUTILS_H

const int ONE_BILLION = 1000000000;

void addToClock(int& seconds, int& nano, int secondsToAdd, int nanoToAdd);
bool hasTimePassed(int currentSeconds, int currentNano, int endSeconds, int endNano);

#endif