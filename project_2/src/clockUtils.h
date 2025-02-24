#ifndef CLOCKUTILS_H
#define CLOCKUTILS_H

class ClockUtils {
public:
    static void addToClock(int& seconds, int& nano, int secondsToAdd, int nanoToAdd);
    static bool hasTimePassed(int currentSeconds, int currentNano, int endSeconds, int endNano);
};

#endif