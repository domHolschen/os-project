#include "resourceUtils.h"
#include <cstdio>
#include <array>
using namespace std;

array<Descriptor, RESOURCE_TYPES_AMOUNT> createResources() {
    array<Descriptor, RESOURCE_TYPES_AMOUNT> resources;
    for (int i = 0; i < RESOURCE_TYPES_AMOUNT; i++) {
        resources[i].totalInstances = MAX_PROCESSES_AMOUNT;
        resources[i].availableInstances = MAX_PROCESSES_AMOUNT;
        for (int j = 0; j < MAX_PROCESSES_AMOUNT; j++) {
            resources[i].allocated[j] = 0;
            resources[i].requested[j] = 0;
        }
    }
    return resources;
}

/* Allocates a resource to a particular process, if able. Returns whether successful */
bool allocateToProcess(Descriptor resource, int processId) {
    if (processId < 0 || processId >= MAX_PROCESSES_AMOUNT) return false;
    if (resource.availableInstances < 1) return false;

    resource.allocated[processId]++;
    resource.availableInstances--;
    resource.requested[processId]--;
    return true;
}

/* Frees one particular resource from a particular process, if able. Returns whether successful */
bool freeFromProcess(Descriptor resource, int processId) {
    if (processId < 0 || processId >= MAX_PROCESSES_AMOUNT) return false;
    if (resource.allocated[processId] < 1) return false;

    resource.allocated[processId]--;
    resource.availableInstances++;
    return true;
}
 /* Removes everything associated with a process. Used for when the process is terminated */
void freeProcess(Descriptor* resources, int processId) {
    if (processId < 0 || processId >= MAX_PROCESSES_AMOUNT) return;
    for (int i; i < RESOURCE_TYPES_AMOUNT; i++) {
        int resourcesFreed = resources[i].allocated[processId];
        resources[i].availableInstances += resourcesFreed;
        resources[i].requested[processId] = 0;
    }
}