#ifndef MANAGER_OPS_H
#define MANAGER_OPS_H

#include <string.h> // For strcspn, strncpy, strcmp, strlen
#include <stdio.h>  // For perror, printf, sprintf
#include <stdlib.h> // For atoi
#include <unistd.h> // For read, write, lseek, close, getpid
#include <fcntl.h>  // For file constants (O_RDWR etc) and fcntl
#include <errno.h>  // For checking errno with sem_trywait
#include <sys/types.h> // For off_t, pid_t

// Include structs (assuming bank_records.h is included *before* this in bank_server.c)
// #include "bank_records.h" // Not needed here if included before in server.c

// --- Function Prototypes ---
int authenticateManager(int clientSocket, int managerID, char *password_input);
void setAccountActiveStatus(int clientSocket);
void reviewClientFeedback(int clientSocket);
void assignLoanToEmployee(int clientSocket);
int updateManagerPassword(int clientSocket, int managerID);
void handleManagerSession(int clientSocket); // Main handler prototype
// Note: updateEmployeePassword is defined in employee_ops.h but declared/called here

// --- Function Definitions ---

// Authenticate a manager (Role 0)
int authenticateManager(int clientSocket, int managerID, char *password_input)
{
    struct Employee manager;
    int dbFile = open(EMPLOYEE_DB, O_RDONLY); // Open RDONLY first
    if (dbFile == -1) {
         if (errno == ENOENT) { // File doesn't exist? Create empty one
             dbFile = open(EMPLOYEE_DB, O_WRONLY | O_CREAT, 0644);
             if (dbFile != -1) close(dbFile);
             printf("Created empty employee database file: %s\n", EMPLOYEE_DB);
         } else {
            perror("AuthMgr: Error opening employee DB for read");
            return 0; // Cannot authenticate
         }
    } else {
        close(dbFile); // Close read handle if it existed
    }

    // Session lock
    sessionSemaphore = createSessionLock(managerID);
     if (sessionSemaphore == SEM_FAILED) {
         perror("AuthMgr: Failed to create/open session semaphore");
         return 0;
    }
    setupSignalHandlers();

    if (sem_trywait(sessionSemaphore) == -1) {
        if (errno == EAGAIN) {
            printf("Manager %d is already logged in!\n", managerID);
            bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "This ID is already logged in elsewhere.^");
            write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        } else {
            perror("AuthMgr: sem_trywait failed");
        }
        sem_close(sessionSemaphore);
        return 0;
    }

    // Now open for reading credentials
     dbFile = open(EMPLOYEE_DB, O_RDONLY);
     if (dbFile == -1) {
          perror("AuthMgr: Error opening employee DB for login check");
          sem_post(sessionSemaphore); sem_close(sessionSemaphore); sem_unlink(sessionSemName);
          return 0;
     }

    // Check credentials
    int loggedIn = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &manager, sizeof(manager)) == sizeof(manager))
    {
        // Compare sanitized input against stored password
        if (manager.employeeID == managerID && strcmp(manager.password, password_input) == 0 && manager.roleType == 0) { // 0 = Manager
           loggedIn = 1;
           break;
        }
    }
    close(dbFile);

     if (!loggedIn) {
        // Auth failed, release lock
        sem_post(sessionSemaphore);
        sem_close(sessionSemaphore);
        sem_unlink(sessionSemName);
        return 0; // Failure
    }
    printf("Manager %d logged in.\n", managerID);
    return 1; // Success - Semaphore still held!
}

// Activate or Deactivate a customer account
void setAccountActiveStatus(int clientSocket)
{
    int dbFile = open(ACCOUNT_DB, O_RDWR);
    if(dbFile == -1) {
        perror("SetStatus: Error opening account DB");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    struct AccountHolder account;
    int accountID, choice;

    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Account Number to modify: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(dbFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    accountID = atoi(inBuffer);

    // Find offset
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &account, sizeof(account)) > 0)
    {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account);
        if(account.accountID == accountID) {
            offset = currentPos;
            break;
        }
    }

    if (offset == -1) {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid account number^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(dbFile);
        return;
    }

    // Lock record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("SetStatus: Lock failed"); close(dbFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }


    // Re-read after lock
    lseek(dbFile, offset, SEEK_SET);
     if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("SetStatus: Re-read failed"); goto setstatus_unlock_fail;
     }

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Account %d (%s) is currently %s.\n[1] Deactivate\n[2] Activate\nChoice: ",
            accountID, account.holderName, account.isActive ? "Active" : "Inactive");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
         printf("Client disconnected during status choice.\n"); goto setstatus_unlock_fail;
     }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    choice = atoi(inBuffer);

    int statusChanged = 0;
    if(choice == 1 && account.isActive != 0) { // Deactivate
        account.isActive = 0;
        statusChanged = 1;
        printf("Manager deactivated account %d\n", accountID);
    } else if (choice == 2 && account.isActive != 1) { // Activate
        account.isActive = 1;
        statusChanged = 1;
        printf("Manager activated account %d\n", accountID);
    }

    if (statusChanged) {
        // Write changes
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account));
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Status Changed Successfully^");
    } else {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid choice or status already set.^");
    }


    lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    return; // Success or no change

setstatus_unlock_fail: // Cleanup on error/disconnect
    lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return; // Failure implicitly
}

// Read and display customer feedback
void reviewClientFeedback(int clientSocket)
{
    struct ClientFeedback feedback;
    int feedbackFile = open(FEEDBACK_DB, O_RDONLY | O_CREAT, 0644); // Ensure file exists
    if(feedbackFile == -1) {
        perror("ReviewFeedback: Error opening file");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error retrieving feedback.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; // Shared lock for reading
    fcntl(feedbackFile, F_SETLKW, &lock);

    bzero(outBuffer, sizeof(outBuffer));
    int feedbackCount = 0;
    while(read(feedbackFile, &feedback, sizeof(feedback)) == sizeof(feedback))
    {
        // Prevent buffer overflow when concatenating
        if (strlen(outBuffer) + strlen(feedback.message) + 2 < sizeof(outBuffer)) {
            strcat(outBuffer, feedback.message);
            strcat(outBuffer, "\n");
            feedbackCount++;
        } else {
            strcat(outBuffer, "... (more feedback truncated)\n");
            break; // Stop if buffer is full
        }
    }

    lock.l_type = F_UNLCK; fcntl(feedbackFile, F_SETLK, &lock);
    close(feedbackFile);

    if(feedbackCount == 0) {
        strcpy(outBuffer, "No feedback found.\n");
    }

    strcat(outBuffer, "^"); // Signal end
    printf("Manager reading %d feedback entries.\n", feedbackCount);
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
}

// Assign a requested loan (status 0) to an employee (status becomes 1)
void assignLoanToEmployee(int clientSocket)
{
    struct LoanRecord loan;
    int loanFile = open(LOAN_DB, O_RDWR); // Need RDWR to read and write
    if(loanFile == -1) {
         perror("AssignLoan: Error opening loan DB");
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    // --- Show unassigned loans first ---
    struct flock readLock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; // Shared lock for reading list
    fcntl(loanFile, F_SETLKW, &readLock);

    int unassignedFound = 0;
    lseek(loanFile, 0, SEEK_SET); // Rewind
    while(read(loanFile, &loan, sizeof(loan)) == sizeof(loan))
    {
        if(loan.assignedEmployeeID == -1 && loan.loanStatus == 0) // Unassigned and Requested
        {
            bzero(outBuffer, sizeof(outBuffer));
            sprintf(outBuffer, "-> Unassigned Loan ID: %d | Account: %d | Amount: %d^",
                    loan.loanRecordID, loan.accountID, loan.amount);
            write(clientSocket, outBuffer, strlen(outBuffer));
            read(clientSocket, inBuffer, 3); // ack for each
            unassignedFound = 1;
        }
    }

    // Release read lock
    readLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &readLock);

    if(!unassignedFound) {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "No unassigned loans found.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(loanFile);
        return;
    }
    // --- End showing loans ---


    // --- Get assignment details ---
    int loanID, employeeID;
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Loan ID to assign: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(loanFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    loanID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Employee ID to assign to: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(loanFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    employeeID = atoi(inBuffer);
    // --- End getting details ---


    // --- Find and update the specific loan ---
    int offset = -1;
    off_t currentPos = 0;
    lseek(loanFile, 0, SEEK_SET); // Rewind again
    while (read(loanFile, &loan, sizeof(loan)) == sizeof(loan))
    {
        currentPos = lseek(loanFile, 0, SEEK_CUR) - sizeof(loan);
        if(loan.loanRecordID == loanID) {
            offset = currentPos;
            break;
        }
    }

    if(offset == -1) {
        bzero(outBuffer, sizeof(outBuffer)); sprintf(outBuffer, "Loan ID %d not found.^", loanID);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(loanFile);
        return;
    }

    // Lock the specific loan record
    struct flock writeLock = {F_WRLCK, SEEK_SET, offset, sizeof(struct LoanRecord), getpid()};
    if(fcntl(loanFile, F_SETLKW, &writeLock) == -1) {
         perror("AssignLoan: Lock failed"); close(loanFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }


    // Re-read data after lock
    lseek(loanFile, offset, SEEK_SET);
    if (read(loanFile, &loan, sizeof(loan)) != sizeof(loan)) {
        perror("AssignLoan: Re-read failed"); goto assignloan_unlock_fail;
    }


    // Check if it's still unassigned and requested
    if(loan.assignedEmployeeID != -1 || loan.loanStatus != 0) {
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan %d was already assigned or processed.^", loanID);
    } else {
        // TODO: Optionally check if employeeID exists in employee_records.dat?
        loan.assignedEmployeeID = employeeID;
        loan.loanStatus = 1; // 1 = Pending (assigned)

        lseek(loanFile, offset, SEEK_SET);
        write(loanFile, &loan, sizeof(loan));

        printf("Manager assigned loan %d to employee %d\n", loanID, employeeID);
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan %d assigned to employee %d.^", loanID, employeeID);
    }

    // Unlock and close
    writeLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &writeLock);
    close(loanFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    return; // Success or already assigned message sent

assignloan_unlock_fail: // Cleanup
    writeLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &writeLock);
    close(loanFile);
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error during assignment.^");
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    return; // Failure implicitly
}

// Wrapper for changing Manager's own password
int updateManagerPassword(int clientSocket, int managerID)
{
    // This calls the function defined in employee_ops.h
    return updateEmployeePassword(clientSocket, managerID);
}

// Main handler for the Manager menu and actions
void handleManagerSession(int clientSocket)
{
    int authManagerID = -1; // Initialize
    char password[51]; // Buffer for password input, size 51

label_manager_login:
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "\nEnter Manager ID: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    authManagerID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    // Sanitize password input for login
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(password, inBuffer, sizeof(password) - 1); password[sizeof(password)-1] = '\0';


    // Pass sanitized password
    if(authenticateManager(clientSocket, authManagerID, password))
    {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "\nLogin Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack

        while(1)
        {
            bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, MANAGER_PROMPT);
            write(clientSocket, outBuffer, strlen(outBuffer));

            bzero(inBuffer, sizeof(inBuffer));
             if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                 printf("Manager %d disconnected during session.\n", authManagerID);
                 terminateClientSession(clientSocket, authManagerID);
                 return;
             }
             inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize choice
            int choice = atoi(inBuffer);
            printf("Manager %d choice: %d\n", authManagerID, choice);

            switch(choice)
            {
                case 1: // Activate/Deactivate
                    setAccountActiveStatus(clientSocket); // Handles own acks
                    break;
                case 2: // Assign Loan
                    assignLoanToEmployee(clientSocket); // Handles own acks
                    break;
                case 3: // Review Feedback
                    reviewClientFeedback(clientSocket); // Handles own acks
                    break;
                case 4: // Change Password
                    if(updateManagerPassword(clientSocket, authManagerID)) { // Calls employee func
                        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password changed. Please log in again.^");
                    } else {
                         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password change failed.^");
                    }
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                    endUserSession(clientSocket, authManagerID); // Shared func from customer_ops.h
                    authManagerID = -1;
                    goto label_manager_login; // Force re-login
                case 5: // Logout
                    printf("Manager %d Logged Out!\n", authManagerID);
                    endUserSession(clientSocket, authManagerID); // Shared func
                    authManagerID = -1;
                    return; // Back to main server loop
                case 6: // Exit
                    printf("Manager %d Exited!\n", authManagerID);
                    terminateClientSession(clientSocket, authManagerID); // Server func
                     authManagerID = -1;
                    return; // Exit child process
                default:
                    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid Choice^");
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
            }
        }
    }
    else // Login Failed
    {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "\nInvalid ID or Password^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        authManagerID = -1;
        goto label_manager_login;
    }
}


#endif // MANAGER_OPS_H
