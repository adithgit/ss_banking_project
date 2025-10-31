#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>
#include<fcntl.h>
#include<unistd.h>
#include<semaphore.h>
#include<netinet/ip.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/stat.h>
#include<sys/sem.h>
#include<sys/wait.h>
#include<errno.h>
#include<signal.h>

// Database file paths
#define EMPLOYEE_DB "employee_records.dat"
#define ACCOUNT_DB "account_records.dat"
#define LOAN_DB "loan_records.dat"
#define LOAN_COUNTER_DB "loan_id_counter.dat"
#define HISTORY_DB "transaction_logs.dat"
#define FEEDBACK_DB "feedback_logs.dat"
#define ADMIN_PASS_DB "admin_pass.dat"

// promptss
#define MAIN_PROMPT "\n===== Login As =====\n1. Customer\n2. Employee\n3. Manager\n4. Admin\n5. Exit\nEnter your choice: "
#define ADMIN_PROMPT "\n===== Admin =====\n1. Add New Bank Employee\n2. Modify Customer/Employee Details\n3. Manage User Roles\n4. Change Password\n5. Logout\nEnter your choice: "
#define CUSTOMER_PROMPT "\n===== Customer =====\n1. Deposit\n2. Withdraw\n3. View Balance\n4. Apply for a loan\n5. Money Transfer\n6. Change Password\n7. View Transaction\n8. Add Feedback\n9. Logout\n10. Exit\nEnter your choice: "
#define EMPLOYEE_PROMPT "\n===== Employee =====\n1. Add New Customer\n2. Modify Customer Details\n3. Approve/Reject Loans\n4. View Assigned Loan Applications\n5. View Customer Transactions\n6. Change Password\n7. Logout\n8. Exit\nEnter your choice: "
#define MANAGER_PROMPT "\n===== Manager =====\n1. Activate/Deactivate Customer Accounts\n2. Assign Loan Application Processes to Employees\n3. Review Customer Feedback\n4. Change Password\n5. Logout\n6. Exit\nEnter your choice: "

// Global buffers and file descriptors
int writeBytes, readBytes;
char inBuffer[4096], outBuffer[4096];

void handleEmployeeSession(int clientSocket);
void handleManagerSession(int clientSocket);
void handleAdminSession(int clientSocket);
void handleCustomerSession(int clientSocket);
void clientConnectionLoop(int clientSocketFD);
void terminateClientSession(int clientSocket, int sessionID);

// Session management (semaphore) prototypes and globals
void sessionCleanupHandler(int signum);
void setupSignalHandlers();
sem_t *createSessionLock(int sessionID);

sem_t *sessionSemaphore; 
char sessionSemName[50]; 

#include "bank_records.h" 
#include "customer_ops.h" 
#include "admin_ops.h"
#include "employee_ops.h"
#include "manager_ops.h"

int main()
{
    int serverSocketFD, clientSocketFD;
    int bindStatus, listenStatus;
    socklen_t clientAddrSize; 

    struct sockaddr_in serverAddress, clientAddress;

    serverSocketFD = socket(AF_INET, SOCK_STREAM, 0);
    if(serverSocketFD == -1)
    {
        perror("Server socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("Server Socket Created\n");

    // for address reuse after restart
    int reuse = 1;
    if (setsockopt(serverSocketFD, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);

    bindStatus = bind(serverSocketFD, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if (bindStatus == -1)
    {
        perror("Server socket binding failed");
        close(serverSocketFD); // Close socket on error
        exit(EXIT_FAILURE);
    }
    printf("Binding to socket successful!\n");

    listenStatus = listen(serverSocketFD, 10); // 10 is the backlog
    if (listenStatus == -1)
    {
        perror("Server listen failed");
        close(serverSocketFD);
        exit(EXIT_FAILURE);
    }
    printf("Listening for connections on port 8080!\n");

    // cleanup function
    setupSignalHandlers();

    while(1)
    {
        clientAddrSize = sizeof(clientAddress);
        clientSocketFD = accept(serverSocketFD, (struct sockaddr *) &clientAddress, &clientAddrSize);

        if (clientSocketFD == -1) {
            // check if accept was interrupted by some signal
            if (errno == EINTR) {
                printf("\nAccept interrupted, possibly shutting down.\n");
                break; 
            }
            perror("Server accept failed");
            continue; // continue listening
        }

        pid_t childPid = fork();

        if(childPid < 0) { 
             perror("Fork failed");
             close(clientSocketFD); // close socket
             continue; 
        }
        else if(childPid == 0)
        {
            close(serverSocketFD);// listener not needed for child
            printf("Client connected. FD: %d, Process ID: %d\n", clientSocketFD, getpid());
            clientConnectionLoop(clientSocketFD);
            printf("Client FD %d disconnected. Child process %d exiting.\n", clientSocketFD, getpid());
            close(clientSocketFD);
            exit(EXIT_SUCCESS);
        }
        else //parent
        {
            close(clientSocketFD); 
        }
    }

    printf("Server shutting down.\n");
    close(serverSocketFD); // Close listener socket
    return 0;
}

// got o main menu and handle that
void clientConnectionLoop(int clientSocketFD)
{
    int userChoice;

    while(1)
    {
        bzero(outBuffer, sizeof(outBuffer)); // clear buffer
        strcpy(outBuffer, MAIN_PROMPT);
        writeBytes = write(clientSocketFD, outBuffer, strlen(outBuffer));
        if(writeBytes <= 0) {
            perror("Write main menu failed or client disconnected");
            break;
        }

        bzero(inBuffer, sizeof(inBuffer));
        readBytes = read(clientSocketFD, inBuffer, sizeof(inBuffer) - 1);
        if(readBytes <= 0) {
             perror("Read main choice failed or client disconnected");
            break;
        }
        inBuffer[readBytes] = '\0'; // didn't read null so insert null
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; 

        userChoice = atoi(inBuffer);
        printf("Client FD %d choice: %d\n", clientSocketFD, userChoice);

        switch (userChoice)
        {
            case 1:
                handleCustomerSession(clientSocketFD);
                break;
            case 2:
                handleEmployeeSession(clientSocketFD);
                break;
            case 3:
                handleManagerSession(clientSocketFD);
                break;
            case 4:
                handleAdminSession(clientSocketFD);
                break;
            case 5:
                terminateClientSession(clientSocketFD, 0); //special ID 0 for non-logged-in exit
                return; 
            default:
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "Invalid Choice for Login menu^");
                write(clientSocketFD, outBuffer, strlen(outBuffer));
                // Wait for ack
                bzero(inBuffer, sizeof(inBuffer));
                read(clientSocketFD, inBuffer, 3);
        }
    }
}

// clean up semaphore and informs client before closing connection
void terminateClientSession(int clientSocket, int sessionID)
{
    // ->only unlink semaphore if a valid session was established (ID > 0)
    if (sessionID > 0) {
        snprintf(sessionSemName, 50, "/bms_sem_%d", sessionID);
        sem_t *sema = sem_open(sessionSemName, 0);
        if (sema != SEM_FAILED) {
            sem_post(sema); // Release the lock
            sem_close(sema);
            sem_unlink(sessionSemName); // Remove the semaphore
            printf("Cleaned up semaphore for ID %d\n", sessionID);
        } else {
             printf("Could not open semaphore %s for cleanup (may already be unlinked).\n", sessionSemName);
        }

    }

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Client logging out...\n"); 
    write(clientSocket, outBuffer, strlen(outBuffer));
}

// Session handling using sems

// Signal handler for cleaning up semaphore on unexpected termination
void sessionCleanupHandler(int signum) {
    printf("\nInterrupt signal (%d) received by process %d.\n", signum, getpid());
    // Check if this process was holding a session lock
    if (sessionSemaphore != NULL && strlen(sessionSemName) > 0) {
        printf("Attempting to cleanup semaphore %s...\n", sessionSemName);
        sem_post(sessionSemaphore);  
        sem_close(sessionSemaphore);
        sem_unlink(sessionSemName);
        printf("Semaphore %s potentially cleaned up.\n", sessionSemName);
        sessionSemaphore = NULL; 
        bzero(sessionSemName, sizeof(sessionSemName)); 
    } else {
        printf("No active session semaphore to clean up for this process.\n");
    }
    // Re-raise signal for default behavior (like core dump for SIGSEGV)
    signal(signum, SIG_DFL);
    raise(signum);
}

// create semaphore
sem_t *createSessionLock(int sessionID) {
    snprintf(sessionSemName, 50, "/bms_sem_%d", sessionID);
    // create or open the semaphore, initialized to 1
    sem_t *sem = sem_open(sessionSemName, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("sem_open failed in createSessionLock");
        bzero(sessionSemName, sizeof(sessionSemName)); 
    }
    return sem;
}

// cleanup function
void setupSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sessionCleanupHandler;
    sa.sa_flags = 0; 

    sigaction(SIGINT, &sa, NULL);  
    sigaction(SIGTERM, &sa, NULL); 
    sigaction(SIGQUIT, &sa, NULL); 
    sigaction(SIGSEGV, &sa, NULL); 
}
