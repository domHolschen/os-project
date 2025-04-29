#ifndef RESOURCEUTILS_H
#define RESOURCEUTILS_H

#include <array>

const int MAX_PROCESSES_AMOUNT = 18;
const int RESOURCE_TYPES_AMOUNT = 5;
const int RESOURCE_INSTANCES_AMOUNT = 10;

struct Descriptor {
	int totalInstances;
	int availableInstances;
	int allocated[MAX_PROCESSES_AMOUNT];
	int requested[MAX_PROCESSES_AMOUNT];
};

std::array<Descriptor, RESOURCE_TYPES_AMOUNT> createResources();
bool allocateToProcess(Descriptor resource, int processId);
bool freeFromProcess(Descriptor resource, int processId);
void freeProcess(Descriptor* resources, int processId);

#endif