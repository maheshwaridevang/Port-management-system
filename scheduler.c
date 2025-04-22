#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>

#define MAX_DOCKS 30
#define MAX_CARGO_COUNT 200
#define MAX_AUTH_STRING_LEN 100
#define MAX_NEW_REQUESTS 100
#define MAX_SHIPS 1100
#define MAX_SOLVERS 8
#define MAX_CRANES 25

typedef struct ShipRequest
{
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory
{
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct MessageStruct
{
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union
    {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct SolverRequest
{
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse
{
    long mtype;
    int guessIsCorrect;
} SolverResponse;

typedef struct Dock
{
    int id;
    int category;
    int *craneCapacities;
    bool occupied;
    int shipId;
    int direction;
    int dockingTimestep;
    int lastCargoMovedTimestep;
    int remainingCargo;
    int *remainingCargoWeights;
} Dock;

typedef struct Ship
{
    int id;
    int direction;
    int category;
    int emergency;
    int waitingTime;
    int arrivalTimestep;
    int numCargo;
    int *cargoWeights;
    bool docked;
    int dockId;
    bool serviced;
    int remainingCargo;
    int deadline;
} Ship;

typedef struct
{
    int dockIndex;
    int solverId;
    bool *guessedCorrectly;
    char *correctAuthString;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} SolverThreadData;

// Global variables
int currentTimestep = 0;
int mainMsgQueueId, shmId;
int solverMsgQueueIds[MAX_SOLVERS];
int numSolvers, numDocks;
MainSharedMemory *shmPtr;
Dock *docks;
Ship *ships;
int shipCount = 0;
int shipCapacity = 100;
bool craneUsedTimestep[MAX_DOCKS][MAX_CRANES];
char ***precomputedStrings;
int *authStringCounts;
int powie[11] = {1, 6, 36, 216, 1296, 7776, 46656, 279936, 1679616, 10077696, 60466176};

// to set up shared memory
void SharedMemory(int key)
{
    shmId = shmget(key, sizeof(MainSharedMemory), 0666);
    if (shmId == -1)
    {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }

    shmPtr = (MainSharedMemory *)shmat(shmId, NULL, 0);
    if (shmPtr == (MainSharedMemory *)-1)
    {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }
}

// set up message queues
void MessageQueues(int mainQueueKey, int solverQueueKeys[], int numSolvers)
{
    mainMsgQueueId = msgget(mainQueueKey, 0666);
    if (mainMsgQueueId == -1)
    {
        perror("msgget failed for main queue");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < numSolvers; i++)
    {
        solverMsgQueueIds[i] = msgget(solverQueueKeys[i], 0666);
        if (solverMsgQueueIds[i] == -1)
        {
            perror("msgget failed for solver queue");
            exit(EXIT_FAILURE);
        }
    }
}

// This part computes all possible authentication strings using these rules:
// The first and last characters must be from {'5','6','7','8','9'}, and any middle characters (if any) must be from {'5','6','7','8','9','.'}. 
// It uses a recursive approach to generate these strings, stores them in a precomputed 3D array (precomputedStrings), and tracks the count for each length in `authStringCounts`.
void generateAuthStringRecursive(char *current, int pos, int length, char *FirstAndLast, char *middleChars, char **output, int *index)
{
    if (pos == length - 1)
    {   
        for (int i = 0; i < 5; i++)
        {
            current[pos] = FirstAndLast[i];
            strcpy(output[*index], current);
            (*index)++;
        }
    }
    else
    {
        for (int i = 0; i < 6; i++)
        {
            current[pos] = middleChars[i];
            generateAuthStringRecursive(current, pos + 1, length, FirstAndLast,
                                        middleChars, output, index);
        }
    }
}

// here we are generating auth string of specific lengths
void generateAuthStringsOfLength(int length, char *FirstAndLast, char *middleChars, char **output)
{
    int index = 0;
    if (length == 2)
    {
        for (int i = 0; i < 5; i++)
        {
            for (int j = 0; j < 5; j++)
            {
                output[index][0] = FirstAndLast[i];
                output[index][1] = FirstAndLast[j];
                output[index][2] = '\0';
                index++;
            }
        }
    }
    else
    {
        char *currentString = (char *)malloc((length + 1) * sizeof(char));
        currentString[length] = '\0';
        for (int i = 0; i < 5; i++)
        {
            currentString[0] = FirstAndLast[i];
            generateAuthStringRecursive(currentString, 1, length, FirstAndLast, middleChars, output, &index);
        }

        free(currentString);
    }
}

// here we are precomputing all the auth strings
void precomputeAuthStrings()
{
    authStringCounts = (int *)malloc(11 * sizeof(int));
    precomputedStrings = (char ***)malloc(11 * sizeof(char **));

    char FirstAndLast[] = "56789";
    char middleChars[] = "56789.";

    for (int length = 1; length <= 10; length++)
    {
        int numOfCombination;
        if (length == 1)
        {
            numOfCombination = 5;
        }
        else
        {
            numOfCombination = 5 * powie[length - 2] * 5;
        }

        authStringCounts[length] = numOfCombination;

        precomputedStrings[length] = (char **)malloc(numOfCombination * sizeof(char *));

        for (int i = 0; i < numOfCombination; i++)
        {
            precomputedStrings[length][i] = (char *)malloc((length + 1) * sizeof(char));
        }

        int index = 0;

        if (length == 1)
        {
            for (int i = 0; i < 5; i++)
            {
                precomputedStrings[length][index][0] = FirstAndLast[i];
                precomputedStrings[length][index][1] = '\0';
                index++;
            }
        }
        else
        {
            generateAuthStringsOfLength(length, FirstAndLast, middleChars, precomputedStrings[length]);
        }
    }
}

// here we are initializing docks from input
void initializeDocks(FILE *inputFile)
{
    fscanf(inputFile, "%d", &numDocks);

    docks = (Dock *)malloc(numDocks * sizeof(Dock));
    if (docks == NULL)
    {
        perror("Failed to allocate memory for docks");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < numDocks; i++)
    {
        docks[i].id = i;
        docks[i].occupied = false;

        fscanf(inputFile, "%d", &docks[i].category);

        docks[i].craneCapacities = (int *)malloc(docks[i].category * sizeof(int));
        if (docks[i].craneCapacities == NULL)
        {
            perror("Failed to allocate memory for crane capacities");
            exit(EXIT_FAILURE);
        }

        for (int j = 0; j < docks[i].category; j++)
        {
            fscanf(inputFile, "%d", &docks[i].craneCapacities[j]);
        }

        docks[i].remainingCargoWeights = NULL;
    }
}

// here we are initializing ships
void initializeShips()
{
    ships = (Ship *)malloc(MAX_SHIPS * sizeof(Ship));
    if (ships == NULL)
    {
        perror("Failed to allocate memory for ships");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < MAX_SHIPS; i++)
    {
        ships[i].cargoWeights = (int *)malloc(MAX_CARGO_COUNT * sizeof(int));
        if (ships[i].cargoWeights == NULL)
        {
            perror("Failed to allocate memory for cargo weights");
            exit(EXIT_FAILURE);
        }
        ships[i].serviced = true;
    }

    shipCapacity = MAX_SHIPS;
}

// here we are adding new ships
void addShip(ShipRequest shipRequest)
{
    int index = -1;

    for (int i = 0; i < shipCount; i++)
    {
        if (ships[i].id == shipRequest.shipId && ships[i].direction == shipRequest.direction && !ships[i].serviced)
        {
            index = i;
            break;
        }
    }

    if (index == -1)
    {
        index = shipCount++;
    }

    ships[index].id = shipRequest.shipId;
    ships[index].direction = shipRequest.direction;
    ships[index].category = shipRequest.category;
    ships[index].emergency = shipRequest.emergency;
    ships[index].waitingTime = shipRequest.waitingTime;
    ships[index].arrivalTimestep = shipRequest.timestep;
    ships[index].numCargo = shipRequest.numCargo;
    ships[index].docked = false;
    ships[index].serviced = false;
    ships[index].remainingCargo = shipRequest.numCargo;

    if (ships[index].direction == 1 && ships[index].emergency == 0)
    {
        ships[index].deadline = ships[index].arrivalTimestep + ships[index].waitingTime;
    }
    else
    {
        ships[index].deadline = INT_MAX;
    }

    for (int i = 0; i < shipRequest.numCargo; i++)
    {
        ships[index].cargoWeights[i] = shipRequest.cargo[i];
    }
}

// here we are comparing ships by their priority
int compareShipPriority(const void *a, const void *b)
{
    const Ship *shipA = (const Ship *)a;
    const Ship *shipB = (const Ship *)b;

    if (shipA->serviced || shipA->docked)
        return 1;
    if (shipB->serviced || shipB->docked)
        return -1;

    if (shipA->emergency > shipB->emergency)
        return -1;
    if (shipA->emergency < shipB->emergency)
        return 1;

    if (shipA->direction == 1 && shipB->direction == 1)
    {
        return shipA->deadline - shipB->deadline;
    }

    if (shipA->direction == 1 && shipB->direction == -1)
        return -1;
    if (shipA->direction == -1 && shipB->direction == 1)
        return 1;

    return shipA->arrivalTimestep - shipB->arrivalTimestep;
}

// here we are searching for the best dock for the given shipindex
int GetBestDock(int shipIndex)
{
    int bestDock = -1;
    int bestCategory = INT_MAX;
    int shipCategory = ships[shipIndex].category;

    for (int i = 0; i < numDocks; i++)
    {
        if (!docks[i].occupied && docks[i].category >= shipCategory)
        {
            if (docks[i].category < bestCategory)
            {
                bestCategory = docks[i].category;
                bestDock = i;
            }
        }
    }

    return bestDock;
}

// here we are docking the ship
void dockShip(int shipIndex, int dockIndex)
{
    MessageStruct msg;

    msg.mtype = 2;
    msg.shipId = ships[shipIndex].id;
    msg.direction = ships[shipIndex].direction;
    msg.dockId = docks[dockIndex].id;

    if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1)
    {
        perror("msgsnd failed for docking");
        exit(EXIT_FAILURE);
    }

    ships[shipIndex].docked = true;
    ships[shipIndex].dockId = docks[dockIndex].id;
    docks[dockIndex].occupied = true;
    docks[dockIndex].shipId = ships[shipIndex].id;
    docks[dockIndex].direction = ships[shipIndex].direction;
    docks[dockIndex].dockingTimestep = currentTimestep;
    docks[dockIndex].remainingCargo = ships[shipIndex].numCargo;

    docks[dockIndex].remainingCargoWeights = (int *)malloc(ships[shipIndex].numCargo * sizeof(int));
    if (docks[dockIndex].remainingCargoWeights == NULL)
    {
        perror("Failed to allocate memory for remaining cargo weights");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < ships[shipIndex].numCargo; i++)
    {
        docks[dockIndex].remainingCargoWeights[i] = ships[shipIndex].cargoWeights[i];
    }
}

// here we are processing emergency ships
bool processEmergencyShips()
{
    bool anyEmergencyShipDocked = false;

    for (int i = 0; i < shipCount; i++)
    {
        if (!ships[i].docked && !ships[i].serviced && ships[i].direction == 1 && ships[i].emergency == 1)
        {
            int dockIndex = GetBestDock(i);
            if (dockIndex != -1)
            {
                dockShip(i, dockIndex);
                anyEmergencyShipDocked = true;
            }
        }
    }

    return anyEmergencyShipDocked;
}

// here we are loading/unloading cargo to/from ships
bool moveCargo(int dockIndex)
{
    if (!docks[dockIndex].occupied || docks[dockIndex].remainingCargo == 0)
    {
        return false;
    }

    if (docks[dockIndex].dockingTimestep == currentTimestep)
    {
        return false;
    }

    int shipIndex = -1;
    for (int i = 0; i < shipCount; i++)
    {
        if (ships[i].id == docks[dockIndex].shipId && ships[i].direction == docks[dockIndex].direction)
        {
            shipIndex = i;
            break;
        }
    }

    if (shipIndex == -1)
        return false;

    for (int cargoId = 0; cargoId < ships[shipIndex].numCargo; cargoId++)
    {
        if (docks[dockIndex].remainingCargoWeights[cargoId] <= 0)
            continue;

        int cargoWeight = docks[dockIndex].remainingCargoWeights[cargoId];
        int bestCrane = -1;
        int minExtraCapacity = INT_MAX;

        for (int craneId = 0; craneId < docks[dockIndex].category; craneId++)
        {
            if (craneUsedTimestep[dockIndex][craneId])
                continue;

            int capacity = docks[dockIndex].craneCapacities[craneId];
            if (capacity >= cargoWeight)
            {
                int extra = capacity - cargoWeight;
                if (extra < minExtraCapacity)
                {
                    minExtraCapacity = extra;
                    bestCrane = craneId;
                }
            }
        }

        if (bestCrane != -1)
        {
            MessageStruct msg;
            msg.mtype = 4;
            msg.shipId = docks[dockIndex].shipId;
            msg.direction = docks[dockIndex].direction;
            msg.dockId = docks[dockIndex].id;
            msg.cargoId = cargoId;
            msg.craneId = bestCrane;
            msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0);

            craneUsedTimestep[dockIndex][bestCrane] = true;
            docks[dockIndex].remainingCargoWeights[cargoId] = 0;
            docks[dockIndex].remainingCargo--;
            docks[dockIndex].lastCargoMovedTimestep = currentTimestep;
            ships[shipIndex].remainingCargo--;
            return true;
        }
    }

    return false;
}

/* now for solver we have a 3d character array of precomputed strings whose 1st parameter is string length, 2nd parameter is no of combinations and 3rd is string length +1 where we are storing all the combinations character wise 
so by using multithreding we will divide that character array in no. of combinations/no. of solvers, give each section to one solver and then we will search for the auth string */
// here we are starting the thread for solver process
void *startSolverThread(void *arg)
{
    SolverThreadData *data = (SolverThreadData *)arg;
    int dockIndex = data->dockIndex;
    int solverId = data->solverId;
    bool *guessedCorrectly = data->guessedCorrectly;
    char *correctAuthString = data->correctAuthString;
    pthread_mutex_t *mutex = data->mutex;
    pthread_cond_t *cond = data->cond;

    int stringLength = docks[dockIndex].lastCargoMovedTimestep - docks[dockIndex].dockingTimestep;
    if (stringLength <= 0 || stringLength > 10)
    {
        pthread_exit(NULL);
    }

    SolverRequest req;
    req.mtype = 1;
    req.dockId = docks[dockIndex].id;

    if (msgsnd(solverMsgQueueIds[solverId], &req, sizeof(SolverRequest) - sizeof(long), 0) == -1)
    {
        perror("msgsnd failed for solver notification");
        pthread_exit(NULL);
    }

    int totalCombinations = authStringCounts[stringLength];
    int combPerThread = (totalCombinations + numSolvers - 1) / numSolvers;
    int startIndex = solverId * combPerThread;
    int endIndex = (solverId + 1) * combPerThread;

    if (endIndex > totalCombinations)
        endIndex = totalCombinations;

    for (int i = startIndex; i < endIndex; i++)
    {
        pthread_mutex_lock(mutex);
        if (*guessedCorrectly)
        {
            pthread_mutex_unlock(mutex);
            break;
        }
        pthread_mutex_unlock(mutex);

        char *authString = precomputedStrings[stringLength][i];

        req.mtype = 2;
        strcpy(req.authStringGuess, authString);

        if (msgsnd(solverMsgQueueIds[solverId], &req, sizeof(SolverRequest) - sizeof(long), 0) == -1)
        {
            perror("msgsnd failed for solver guess");
            pthread_exit(NULL);
        }

        SolverResponse resp;
        if (msgrcv(solverMsgQueueIds[solverId], &resp, sizeof(SolverResponse) - sizeof(long), 3, 0) == -1)
        {
            perror("msgrcv failed for solver response");
            pthread_exit(NULL);
        }

        if (resp.guessIsCorrect == 1)
        {
            pthread_mutex_lock(mutex);
            *guessedCorrectly = true;
            strcpy(correctAuthString, authString);
            pthread_cond_broadcast(cond);
            pthread_mutex_unlock(mutex);
            break;
        }
        else if (resp.guessIsCorrect == -1)
        {
            pthread_mutex_lock(mutex);
            *guessedCorrectly = true;
            pthread_cond_broadcast(cond);
            pthread_mutex_unlock(mutex);
            pthread_exit(NULL);
        }
    }

    pthread_exit(NULL);
}

// here we are undocking the ship
bool undockShip(int dockIndex)
{
    if (!docks[dockIndex].occupied || docks[dockIndex].remainingCargo > 0)
    {
        return false;
    }

    if (docks[dockIndex].lastCargoMovedTimestep == currentTimestep)
    {
        return false;
    }

    int stringLength = docks[dockIndex].lastCargoMovedTimestep - docks[dockIndex].dockingTimestep;
    if (stringLength <= 0)
        return false;

    char correctAuthString[MAX_AUTH_STRING_LEN];
    bool guessedCorrectly = false;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

    pthread_t threads[numSolvers];
    SolverThreadData threadData[numSolvers];

    for (int i = 0; i < numSolvers; i++)
    {
        threadData[i].dockIndex = dockIndex;
        threadData[i].solverId = i;
        threadData[i].guessedCorrectly = &guessedCorrectly;
        threadData[i].correctAuthString = correctAuthString;
        threadData[i].mutex = &mutex;
        threadData[i].cond = &cond;

        if (pthread_create(&threads[i], NULL, startSolverThread, &threadData[i]) != 0)
        {
            perror("Failed to create thread");
            return false;
        }
    }

    pthread_mutex_lock(&mutex);
    if (!guessedCorrectly)
    {
        pthread_cond_wait(&cond, &mutex);
    }
    pthread_mutex_unlock(&mutex);

    for (int i = 0; i < numSolvers; i++)
    {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);

    if (!guessedCorrectly)
    {
        return false;
    }

    strcpy(shmPtr->authStrings[docks[dockIndex].id], correctAuthString);

    MessageStruct msg;
    msg.mtype = 3;
    msg.shipId = docks[dockIndex].shipId;
    msg.direction = docks[dockIndex].direction;
    msg.dockId = docks[dockIndex].id;

    if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1)
    {
        perror("msgsnd failed for undocking");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < shipCount; i++)
    {
        if (ships[i].id == docks[dockIndex].shipId && ships[i].direction == docks[dockIndex].direction)
        {
            ships[i].docked = false;
            ships[i].serviced = true;
            break;
        }
    }

    docks[dockIndex].occupied = false;
    free(docks[dockIndex].remainingCargoWeights);
    docks[dockIndex].remainingCargoWeights = NULL;

    return true;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int testCaseNum = atoi(argv[1]);

    char inputFilePath[100];
    sprintf(inputFilePath, "testcase%d/input.txt", testCaseNum);
    FILE *inputFile = fopen(inputFilePath, "r");
    if (inputFile == NULL)
    {
        perror("Failed to open input file");
        exit(EXIT_FAILURE);
    }

    int shmKey, mainQueueKey;
    fscanf(inputFile, "%d", &shmKey);
    fscanf(inputFile, "%d", &mainQueueKey);

    fscanf(inputFile, "%d", &numSolvers);

    int solverQueueKeys[MAX_SOLVERS];
    for (int i = 0; i < numSolvers; i++)
    {
        fscanf(inputFile, "%d", &solverQueueKeys[i]);
    }

    initializeDocks(inputFile);
    fclose(inputFile);

    SharedMemory(shmKey);
    MessageQueues(mainQueueKey, solverQueueKeys, numSolvers);
    initializeShips();
    precomputeAuthStrings();
    srand(time(NULL));

    bool finished = false;
    while (!finished)
    {
        MessageStruct msg;
        if (msgrcv(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 1, 0) == -1)
        {
            perror("msgrcv failed for new ship requests");
            exit(EXIT_FAILURE);
        }

        currentTimestep = msg.timestep;
        memset(craneUsedTimestep, 0, sizeof(craneUsedTimestep));

        if (msg.isFinished)
        {
            finished = true;
            break;
        }

        int numNewRequests = msg.numShipRequests;
        for (int i = 0; i < numNewRequests; i++)
        {
            addShip(shmPtr->newShipRequests[i]);
        }

        bool emergencyHandled = processEmergencyShips();

        if (numNewRequests > 0 || emergencyHandled)
        {
            qsort(ships, shipCount, sizeof(Ship), compareShipPriority);
        }

        for (int i = 0; i < shipCount; i++)
        {
            if (!ships[i].docked && !ships[i].serviced)
            {
                if (ships[i].direction == 1 && ships[i].emergency == 0 &&
                    currentTimestep > ships[i].deadline)
                {
                    continue;
                }

                int dockIndex = GetBestDock(i);
                if (dockIndex != -1)
                {
                    dockShip(i, dockIndex);
                }
            }
        }

        for (int dockIndex = 0; dockIndex < numDocks; dockIndex++)
        {
            while (moveCargo(dockIndex))
            {
                // Keep moving cargo from this dock until no more can be moved
            }
        }

        for (int dockIndex = 0; dockIndex < numDocks; dockIndex++)
        {   
            if(undockShip(dockIndex)==true)
            {
                continue;
            }
            else
            {
                continue;
            }
        }

        MessageStruct nextMsg;
        nextMsg.mtype = 5;
        msgsnd(mainMsgQueueId, &nextMsg, sizeof(MessageStruct) - sizeof(long), 0);
    }

    for (int length = 1; length <= 10; length++)
    {
        for (int i = 0; i < authStringCounts[length]; i++)
        {
            free(precomputedStrings[length][i]);
        }
        free(precomputedStrings[length]);
    }
    free(precomputedStrings);
    free(authStringCounts);

    for (int i = 0; i < numDocks; i++)
    {
        free(docks[i].craneCapacities);
        if (docks[i].remainingCargoWeights != NULL)
        {
            free(docks[i].remainingCargoWeights);
        }
    }
    free(docks);

    for (int i = 0; i < shipCount; i++)
    {
        if (ships[i].cargoWeights != NULL)
        {
            free(ships[i].cargoWeights);
        }
    }
    free(ships);

    if (shmdt(shmPtr) == -1)
    {
        perror("shmdt failed");
    }
    return 0;
}