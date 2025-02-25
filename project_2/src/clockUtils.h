#ifndef CLOCKUTILS_H
#define CLOCKUTILS_H

void addToClock(int& seconds, int& nano, int secondsToAdd, int nanoToAdd);
bool hasTimePassed(int currentSeconds, int currentNano, int endSeconds, int endNano);

#endif