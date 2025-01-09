#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

// Some general defines
#define MAX_INPUT_LENGTH      100
#define MAX_OUTPUT_LENGTH     200
#define SERVER_PORT           60000
#define LEVENSHTEIN_LIST_LIMIT   5

// Holds a list of words (the dictionary)
typedef struct {
    char **entries;
    size_t entryCount;
} Dictionary;

// For searching similar words in a thread
typedef struct {
    char   inputStr[MAX_INPUT_LENGTH + 1];
    Dictionary *lex;
    char   closestWords[LEVENSHTEIN_LIST_LIMIT][MAX_INPUT_LENGTH + 1];
    int    editDist[LEVENSHTEIN_LIST_LIMIT];
} SearchTaskData;

// Function declarations
int clientSock = 0;
static int  isFile(const char *filename);
static int  loadFromFile(const char *filename, Dictionary *lex);
static void convertToLowercase(char *str);
static int  isValidInput(const char *input);
static int  calculateLevenshteinDistance(const char *s1, const char *s2);
static void *searchSimilarWordsThread(void *arg);
static void addNewEntry(Dictionary *lex, const char *newEntry);
static int  appendNewWordToFile(const char *filename, const char *newWord);
static void notifyErrorAndDisconnect(int clientSock, const char *msg);
static void displayClosestWords(int clientSock, const char *word,
                                char closestWords[LEVENSHTEIN_LIST_LIMIT][MAX_INPUT_LENGTH + 1],
                                int editDist[LEVENSHTEIN_LIST_LIMIT]);
static int  setupServerSocket(void);
static void runServerLoop(int serverSock, Dictionary *mainLex, const char *DictionaryFile);
static void handleClientConnection(int clientSock, Dictionary *mainLex, const char *DictionaryFile);

// main function
int main(int argc, char *argv[]) {
    const char *File = "basic_english_2000.txt";

    // Check dictionary file
    if (!isFile(File)) {
        fprintf(stderr, "ERROR: Could not find the file: %s\n", File);
        return 1;
    }

    // Load the words into memory
    Dictionary mainLex = { NULL, 0 };
    if (!loadFromFile(File, &mainLex)) {
        fprintf(stderr, "ERROR: Failed to load the file: %s\n", File);
        return 1;
    }

    // Prepare server
    int serverSock = setupServerSocket();
    if (serverSock < 0) {
        return 1;
    }

    // Start listening for clients
    runServerLoop(serverSock, &mainLex, File);

    // Cleanup
    close(serverSock);
    for (size_t i = 0; i < mainLex.entryCount; i++) {
        free(mainLex.entries[i]);
    }
    free(mainLex.entries);

    return 0;
}

// Checks if file exists
static int isFile(const char *filename) {
    struct stat s;
    return (stat(filename, &s) == 0);
}

// Loads words from file into memory
static int loadFromFile(const char *filename, Dictionary *lex) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return 0;
    }

    size_t lineCount = 0;
    char tempBuf[256];

    // Count lines
    while (fgets(tempBuf, sizeof(tempBuf), fp)) {
        lineCount++;
    }
    rewind(fp);

    // Allocate memory for lines
    lex->entryCount = lineCount;
    lex->entries = (char **)malloc(sizeof(char *) * lineCount);
    size_t idx = 0;

    // Store each line
    while (fgets(tempBuf, sizeof(tempBuf), fp)) {
        tempBuf[strcspn(tempBuf, "\r\n")] = 0;
        convertToLowercase(tempBuf);

        lex->entries[idx] = (char *)malloc(strlen(tempBuf) + 1);
        strcpy(lex->entries[idx], tempBuf);
        idx++;
    }
    fclose(fp);

    return 1;
}

// Makes a string lowercase
static void convertToLowercase(char *str) {
    while (*str) {
        *str = tolower((unsigned char)*str);
        str++;
    }
}

// Checks if input has only letters or spaces
static int isValidInput(const char *input) {
    for (int i = 0; input[i] != '\0'; i++) {
        if (!isalpha((unsigned char)input[i]) && input[i] != ' ') {
            return 0;
        }
    }
    return 1;
}

// Calculate edit distance between two strings
static int calculateLevenshteinDistance(const char *s1, const char *s2) {
    int len1 = strlen(s1);
    int len2 = strlen(s2);

    int *column = (int *)malloc(sizeof(int) * (len1 + 1));
    if (!column) {
        return (len1 > len2) ? len1 : len2;
    }

    for (int i = 0; i <= len1; i++) {
        column[i] = i;
    }

    for (int x = 1; x <= len2; x++) {
        column[0] = x;
        int lastDiagonal = x - 1;
        for (int y = 1; y <= len1; y++) {
            int oldDiagonal = column[y];
            int costDeletion     = column[y] + 1;
            int costInsertion    = column[y - 1] + 1;
            int costSubstitution = lastDiagonal + ((s1[y - 1] == s2[x - 1]) ? 0 : 1);

            int minCost = costDeletion;
            if (costInsertion < minCost) minCost = costInsertion;
            if (costSubstitution < minCost) minCost = costSubstitution;

            column[y] = minCost;
            lastDiagonal = oldDiagonal;
        }
    }

    int result = column[len1];
    free(column);
    return result;
}

// Thread function to find words with closest distance
static void *searchSimilarWordsThread(void *arg) {
    SearchTaskData *data = (SearchTaskData *)arg;
    const char *inputWord = data->inputStr;
    Dictionary *lx = data->lex;

    for (size_t i = 0; i < lx->entryCount; i++) {
        int dist = calculateLevenshteinDistance(inputWord, lx->entries[i]);

        // Put the new word if it's closer
        for (int k = 0; k < LEVENSHTEIN_LIST_LIMIT; k++) {
            if (dist < data->editDist[k]) {
                // shift the list down
                for (int m = LEVENSHTEIN_LIST_LIMIT - 1; m > k; m--) {
                    data->editDist[m] = data->editDist[m - 1];
                    strcpy(data->closestWords[m], data->closestWords[m - 1]);
                }
                data->editDist[k] = dist;
                strcpy(data->closestWords[k], lx->entries[i]);
                break;
            }
        }
    }

    pthread_exit(NULL);
    return NULL;
}

// Add new word to in-memory dictionary
static void addNewEntry(Dictionary *lex, const char *newEntry) {
    lex->entryCount++;
    lex->entries = (char **)realloc(lex->entries, lex->entryCount * sizeof(char *));
    lex->entries[lex->entryCount - 1] = (char *)malloc(strlen(newEntry) + 1);
    strcpy(lex->entries[lex->entryCount - 1], newEntry);
}

// Also append new word to file
static int appendNewWordToFile(const char *filename, const char *newWord) {
    FILE *fp = fopen(filename, "a");
    if (!fp) {
        return 0;
    }
    fprintf(fp, "%s\n", newWord);
    fclose(fp);
    return 1;
}

static void notifyErrorAndDisconnect(int clientSock, const char *msg) {
    dprintf(clientSock, "%s", msg);
    dprintf(clientSock, "Check the other word...\r\n");
    shutdown(clientSock, SHUT_RDWR); 

    if(clientSock==0){
         close(clientSock); // Socket closed
    }
    
}

// Show closest words (optional)
static void displayClosestWords(int clientSock, const char *word,
                                char closestWords[LEVENSHTEIN_LIST_LIMIT][MAX_INPUT_LENGTH + 1],
                                int editDist[LEVENSHTEIN_LIST_LIMIT])
{
    dprintf(clientSock, "WORD: %s\r\n", word);
    dprintf(clientSock, "MATCHES:\r\n");
    for (int i = 0; i < LEVENSHTEIN_LIST_LIMIT; i++) {
        dprintf(clientSock, "  %s (%d)\r\n", closestWords[i], editDist[i]);
    }
}

// Setup server socket
static int setupServerSocket(void) {
    //AF_INET (IPv4)
    //SOCK_STREAM (TCP Protocol)
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    //Eğer socket başarısız olursa serverSock egatif olur. Bunun kontrolü sağlanıyor.
    if (serverSock < 0) {
        perror("ERROR: Cannot create socket");
        return -1;
    }

    int enable = 1;
    //Customize the settings with setSockopt
    //We enable the socket to be reopened after it is closed with SO_REUSEADDR
    if (setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt error");
    }

    //Server Address Configuration
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET; //(IPv4)
    serverAddr.sin_port        = htons(SERVER_PORT); //(Port numarası belirlenir)
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Herhangi bir ağdan bağlanabilmemizi sağlar.

    //We connect the scket to a specific port number
    if (bind(serverSock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("ERROR: bind failed");
        close(serverSock);
        return -1;
    }
    //It orıvides listening to the connection from the server
    if (listen(serverSock, 1) < 0) {
        perror("ERROR: listen failed");
        close(serverSock);
        return -1;
    }

    //It says that it has succesfully connected to the server
    printf("Server is up on port %d.", SERVER_PORT);
    printf("Waiting for connections...\n");
    return serverSock;
}

// Runs in a loop, accepts connections
//ServerSock starts listening to the server
//mainLex includes input
// Runs in a loop, accepts connections
static void runServerLoop(int serverSock, Dictionary *mainLex, const char *File) {
    // KEEP THE SERVER ACTIVE
    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        // Accept connection request
        int clientSock = accept(serverSock, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (clientSock < 0) {
            perror("ERROR: accept failed");
            close(serverSock);
            exit(1);
        }
        printf("New client connected.\n"); // New line added here
        handleClientConnection(clientSock, mainLex,File);
    }
}

// Deal with a client after we accept
static void handleClientConnection(int clientSock, Dictionary *mainLex, const char *File) {
    dprintf(clientSock, "Hello, this is Text Analysis Server!\r\n");
    dprintf(clientSock, "Please enter your input string:\r\n");

    char inputBuffer[MAX_INPUT_LENGTH + 10];
    memset(inputBuffer, 0, sizeof(inputBuffer));

    ssize_t bytesRead = recv(clientSock, inputBuffer, sizeof(inputBuffer) - 1, 0);
    if (bytesRead < 0) {
        perror("ERROR: recv failed");
        close(clientSock);
        return;
    }

    // Replace \r or \n with \0
    for (int i = 0; i < (int)bytesRead; i++) {
        if (inputBuffer[i] == '\r' || inputBuffer[i] == '\n') {
            inputBuffer[i] = '\0';
            break;
        }
    }

    // Check size
    if (strlen(inputBuffer) > MAX_INPUT_LENGTH) {
        char errorMsg[MAX_OUTPUT_LENGTH];
        snprintf(errorMsg, sizeof(errorMsg),
                 "ERROR: Input is longer than %d!\r\n",
                 MAX_INPUT_LENGTH);
        notifyErrorAndDisconnect(clientSock, errorMsg);
        return;
    }

    // Check valid chars
    if (!isValidInput(inputBuffer)) {
        notifyErrorAndDisconnect(clientSock, "ERROR: including dots, commas, question marks, etc!\r\n");
        return;
    }

    // To lowercase
    convertToLowercase(inputBuffer);

    char originalInput[MAX_INPUT_LENGTH + 1];
    strncpy(originalInput, inputBuffer, MAX_INPUT_LENGTH);
    originalInput[MAX_INPUT_LENGTH] = '\0';

    // Break input into tokens
    char *tokens[100];
    memset(tokens, 0, sizeof(tokens));
    int tokenCount = 0;
    char *savePtr = NULL;

    char *splitWord = strtok_r(inputBuffer, " ", &savePtr);
    while (splitWord != NULL) {
        tokens[tokenCount++] = splitWord;
        splitWord = strtok_r(NULL, " ", &savePtr);
    }

    // Create threads to find close words
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * tokenCount);
    SearchTaskData *taskData = (SearchTaskData *)malloc(sizeof(SearchTaskData) * tokenCount);

    for (int i = 0; i < tokenCount; i++) {
        memset(&taskData[i], 0, sizeof(SearchTaskData));
        strncpy(taskData[i].inputStr, tokens[i], MAX_INPUT_LENGTH);
        taskData[i].lex = mainLex;

        for (int j = 0; j < LEVENSHTEIN_LIST_LIMIT; j++) {
            taskData[i].editDist[j] = 999999;
            taskData[i].closestWords[j][0] = '\0';
        }
        pthread_create(&threads[i], NULL, searchSimilarWordsThread, &taskData[i]);
    }

    for (int i = 0; i < tokenCount; i++) {
        pthread_join(threads[i], NULL);
    }

    // Show results
    char finalOutput[MAX_OUTPUT_LENGTH];
    memset(finalOutput, 0, sizeof(finalOutput));
    char tempString[MAX_INPUT_LENGTH + 10]; // to avoid truncation, we add 10 more chars


for (int i = 0; i < tokenCount; i++) {
    int foundExactMatch = 0;
    for (int j = 0; j < LEVENSHTEIN_LIST_LIMIT; j++) {
        if (taskData[i].editDist[j] == 0 &&
            strcmp(taskData[i].closestWords[j], taskData[i].inputStr) == 0) {
            foundExactMatch = 1;
            break;
        }
    }

    
    dprintf(clientSock, "WORD %02d: %s\r\n", i + 1, taskData[i].inputStr);  
    dprintf(clientSock, "MATCHES: ");
    for (int j = 0; j < LEVENSHTEIN_LIST_LIMIT; j++) {
        dprintf(clientSock, "%s (%d)", taskData[i].closestWords[j], taskData[i].editDist[j]);
        if (j < LEVENSHTEIN_LIST_LIMIT - 1) {
            dprintf(clientSock, ", ");
        }
    }
    dprintf(clientSock, "\r\n");


    
    if (foundExactMatch) {
        snprintf(tempString, sizeof(tempString), "%s ", taskData[i].inputStr);
    } else {
        dprintf(clientSock, "WORD '%s' is not present in dictionary.\r\n", taskData[i].inputStr);
        dprintf(clientSock, "Do you want to add this word to dictionary or exit? (y/N/q): "); 

        char response[10];
        memset(response, 0, sizeof(response));

        ssize_t resp = recv(clientSock, response, sizeof(response) - 1, 0);
        if (resp > 0) {
            for (int jj = 0; jj < (int)resp; jj++) {
                if (response[jj] == '\r' || response[jj] == '\n') {
                    response[jj] = '\0';
                    break;
                }
            }
        }
        convertToLowercase(response);

        if (response[0] == 'y' || response[0] == 'Y') {
            notifyErrorAndDisconnect(1,"Word added to dictionary.\r\n");
            addNewEntry(mainLex, taskData[i].inputStr);
            appendNewWordToFile(File, taskData[i].inputStr);
            snprintf(tempString, sizeof(tempString), "%s ", taskData[i].inputStr);
        } else if (response[0] == 'q' || response[0] == 'Q') { 
            notifyErrorAndDisconnect(clientSock, "Connection closed by user.\r\n");
            free(threads);
            free(taskData);
            close(clientSock);  
            return; 
        } else {
            notifyErrorAndDisconnect(1,"Word not added to dictionary.\r\n");
            snprintf(tempString, sizeof(tempString), "%s ", taskData[i].closestWords[0]); 
        }
    }
    strncat(finalOutput, tempString, sizeof(finalOutput) - strlen(finalOutput) - 1);
    }

    dprintf(clientSock, "\r\nINPUT : %s\r\n", originalInput);
    dprintf(clientSock, "OUTPUT: %s\r\n", finalOutput);

    dprintf(clientSock, "Thanks for using Text Analysis Server! Goodbye!\r\n");

    free(threads);
    free(taskData);
    close(clientSock); 
}