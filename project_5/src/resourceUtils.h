#ifndef RESOURCEUTILS_H
#define RESOURCEUTILS_H

#define MAX_PROCESSES_AMOUNT 18
#define RESOURCE_TYPES_AMOUNT 5
#define RESOURCE_INSTANCES_AMOUNT 10

struct Descriptor {
	int totalInstances;
	int availableInstances;
	int allocated[MAX_PROCESSES_AMOUNT];
	int requested[MAX_PROCESSES_AMOUNT];
};

array<Descriptor, RESOURCE_TYPES_AMOUNT> createResources();
bool allocateToProcess(Descriptor resource, int processId);
bool freeFromProcess(Descriptor resource, int processId);
void freeProcess(Descriptor resources[], int processId);


#endif