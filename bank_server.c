#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>
#include<fcntl.h>
#include<unistd.h>
// #include<crypt.h> // Removed
#include<semaphore.h>
#include<netinet/ip.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/stat.h>
#include<sys/sem.h>
#include<sys/wait.h>
#include<errno.h>
#include<signal.h>

// Compile Command:
// gcc bank_server.c -o bank_server -pthread

// Database file paths
#define STAFF_DB "staff_records.dat"
#define ACCOUNT_DB "account_records.dat"
#define LOAN_DB "loan_records.dat"
#define LOAN_COUNTER_DB "loan_id_counter.dat"
#define HISTORY_DB "transaction_logs.dat"
#define FEEDBACK_DB "feedback_logs.dat"
#define ADMIN_PASS_DB "admin_pass.dat" // Added for admin password

// Menu prompts
#define MAIN_PROMPT "\n===== Login As =====\n1. Customer\n2. Employee\n3. Manager\n4. Admin\n5. Exit\nEnter your choice: "
#define ADMIN_PROMPT "\n===== Admin =====\n1. Add New Bank Employee\n2. Modify Customer/Employee Details\n3. Manage User Roles\n4. Change Password\n5. Logout\nEnter your choice: "
#define CUSTOMER_PROMPT "\n===== Customer =====\n1. Deposit\n2. Withdraw\n3. View Balance\n4. Apply for a loan\n5. Money Transfer\n6. Change Password\n7. View Transaction\n8. Add Feedback\n9. Logout\n10. Exit\nEnter your choice: "
#define STAFF_PROMPT "\n===== Employee =====\n1. Add New Customer\n2. Modify Customer Details\n3. Approve/Reject Loans\n4. View Assigned Loan Applications\n5. View Customer Transactions\n6. Change Password\n7. Logout\n8. Exit\nEnter your choice: "
#define MANAGER_PROMPT "\n===== Manager =====\n1. Activate/Deactivate Customer Accounts\n2. Assign Loan Application Processes to Employees\n3. Review Customer Feedback\n4. Change Password\n5. Logout\n6. Exit\nEnter your choice: "

// Global buffers and file descriptors (used across included files)
int writeBytes, readBytes;
char inBuffer[4096], outBuffer[4096];

// Function prototypes for main server functions and role handlers
void handleStaffSession(int clientSocket);
void handleManagerSession(int clientSocket);
void handleAdminSession(int clientSocket);
void handleCustomerSession(int clientSocket);
void clientConnectionLoop(int clientSocketFD);
void terminateClientSession(int clientSocket, int sessionID);

// Session management (semaphore) prototypes and globals
void sessionCleanupHandler(int signum);
void setupSignalHandlers();
sem_t *createSessionLock(int sessionID);

sem_t *sessionSemaphore; // Global pointer for the current session's semaphore
char sessionSemName[50]; // Global buffer for semaphore name

// Include all role-specific operation files
#include "bank_records.h"    // Struct definitions
#include "customer_ops.h" // Customer functions + endUserSession
#include "admin_ops.h"      // Admin functions
#include "staff_ops.h"      // Staff functions
#include "manager_ops.h"    // Manager functions

int main()
{
    int serverSocketFD, clientSocketFD;
    int bindStatus, listenStatus;
    socklen_t clientAddrSize; // Use socklen_t for size

    struct sockaddr_in serverAddress, clientAddress;

    serverSocketFD = socket(AF_INET, SOCK_STREAM, 0);
    if(serverSocketFD == -1)
    {
        perror("Server socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("Server Socket Created\n");

    // Allow address reuse quickly after server restart
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

    listenStatus = listen(serverSocketFD, 10); // Increased backlog
    if (listenStatus == -1)
    {
        perror("Server listen failed");
        close(serverSocketFD);
        exit(EXIT_FAILURE);
    }
    printf("Listening for connections on port 8080!\n");

    // Setup signal handlers for graceful shutdown/cleanup
    setupSignalHandlers();

    while(1)
    {
        clientAddrSize = sizeof(clientAddress);
        clientSocketFD = accept(serverSocketFD, (struct sockaddr *) &clientAddress, &clientAddrSize);

        if (clientSocketFD == -1) {
            // Check if accept was interrupted by a signal (like SIGINT)
            if (errno == EINTR) {
                printf("\nAccept interrupted, possibly shutting down.\n");
                break; // Exit loop if interrupted
            }
            perror("Server accept failed");
            continue; // Continue listening for other connections
        }

        pid_t childPid = fork();

        if(childPid < 0) { // Fork failed
             perror("Fork failed");
             close(clientSocketFD); // Close the accepted socket
             continue; // Continue parent loop
        }
        else if(childPid == 0) // Child process
        {
            close(serverSocketFD); // Child doesn't need the listener socket
            printf("Client connected. FD: %d, Process ID: %d\n", clientSocketFD, getpid());
            clientConnectionLoop(clientSocketFD);
            printf("Client FD %d disconnected. Child process %d exiting.\n", clientSocketFD, getpid());
            close(clientSocketFD); // Close client socket
            exit(EXIT_SUCCESS); // Child exits normally
        }
        else // Parent process
        {
            close(clientSocketFD); // Parent doesn't need the client socket
            // Optionally wait for child processes to prevent zombies if needed,
            // but for a simple server, letting init adopt them is okay.
            // waitpid(-1, NULL, WNOHANG); // Example of non-blocking wait
        }
    }

    printf("Server shutting down.\n");
    close(serverSocketFD); // Close listener socket
    return 0;
}

// Handles the main menu and directs to role-specific handlers
void clientConnectionLoop(int clientSocketFD)
{
    int userChoice;

    while(1)
    {
        bzero(outBuffer, sizeof(outBuffer)); // Clear buffer before use
        strcpy(outBuffer, MAIN_PROMPT);
        writeBytes = write(clientSocketFD, outBuffer, strlen(outBuffer)); // Send exact length
        if(writeBytes <= 0) {
            perror("Write main menu failed or client disconnected");
            break;
        }

        bzero(inBuffer, sizeof(inBuffer));
        readBytes = read(clientSocketFD, inBuffer, sizeof(inBuffer) - 1); // Read one less for null term
        if(readBytes <= 0) {
             perror("Read main choice failed or client disconnected");
            break;
        }
        inBuffer[readBytes] = '\0'; // Null-terminate received data
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Remove potential newline

        userChoice = atoi(inBuffer);
        printf("Client FD %d choice: %d\n", clientSocketFD, userChoice);

        switch (userChoice)
        {
            case 1:
                handleCustomerSession(clientSocketFD);
                break;
            case 2:
                handleStaffSession(clientSocketFD);
                break;
            case 3:
                handleManagerSession(clientSocketFD);
                break;
            case 4:
                handleAdminSession(clientSocketFD);
                break;
            case 5:
                terminateClientSession(clientSocketFD, 0); // Special ID 0 for non-logged-in exit
                return; // Exit connection loop
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

// Cleans up semaphore and informs client before closing connection
void terminateClientSession(int clientSocket, int sessionID)
{
    // Only unlink semaphore if a valid session was established (ID > 0)
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
    strcpy(outBuffer, "Client logging out...\n"); // Inform client (non-critical if write fails)
    write(clientSocket, outBuffer, strlen(outBuffer));
    // No need to wait for read, just close from server side in main/loop exit
}

// =================== Session Handling (Semaphores) =================

// Signal handler for cleaning up semaphore on unexpected termination
void sessionCleanupHandler(int signum) {
    printf("\nInterrupt signal (%d) received by process %d.\n", signum, getpid());
    // Check if this process was holding a session lock
    if (sessionSemaphore != NULL && strlen(sessionSemName) > 0) {
        printf("Attempting to cleanup semaphore %s...\n", sessionSemName);
        sem_post(sessionSemaphore);  // Try to release the lock
        sem_close(sessionSemaphore); // Close our handle
        sem_unlink(sessionSemName); // Attempt to remove it (might fail if others use it)
        printf("Semaphore %s potentially cleaned up.\n", sessionSemName);
        sessionSemaphore = NULL; // Reset global pointer
        bzero(sessionSemName, sizeof(sessionSemName)); // Clear name buffer
    } else {
        printf("No active session semaphore to clean up for this process.\n");
    }
    // Re-raise signal for default behavior (like core dump for SIGSEGV)
    signal(signum, SIG_DFL);
    raise(signum);
    // _exit(signum); // Alternative: Force exit immediately
}

// Creates or opens a POSIX named semaphore
sem_t *createSessionLock(int sessionID) {
    snprintf(sessionSemName, 50, "/bms_sem_%d", sessionID);
    // Create or open the semaphore, initialized to 1
    // Permissions 0666 allow other users if needed, 0600 is stricter
    sem_t *sem = sem_open(sessionSemName, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("sem_open failed in createSessionLock");
        bzero(sessionSemName, sizeof(sessionSemName)); // Clear name on failure
    }
    return sem;
}

// Sets up signal handlers for graceful cleanup
void setupSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sessionCleanupHandler;
    // Block other signals while handler runs? Maybe not necessary here.
    // sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART

    sigaction(SIGINT, &sa, NULL);  // Ctrl+C
    sigaction(SIGTERM, &sa, NULL); // Termination signal
    sigaction(SIGQUIT, &sa, NULL); // Ctrl+\
    sigaction(SIGHUP, &sa, NULL);  // Hangup
    // SIGSEGV might be harder to catch reliably, but attempt it
    sigaction(SIGSEGV, &sa, NULL); // Segmentation fault
}
