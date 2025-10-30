#ifndef EMPLOYEE_OPS_H
#define EMPLOYEE_OPS_H

#include <string.h> // For strcspn, strncpy, strcmp, strlen
#include <stdio.h>  // For perror, printf, sprintf
#include <stdlib.h> // For atoi, atof
#include <unistd.h> // For read, write, lseek, close, getpid
#include <fcntl.h>  // For file constants (O_RDWR etc) and fcntl
#include <errno.h>  // For checking errno with sem_trywait
#include <time.h>   // For logging timestamp
#include <sys/types.h> // For off_t, pid_t

// Include structs (assuming bank_records.h is included *before* this in bank_server.c)
// #include "bank_records.h" // Not needed here if included before in server.c

// --- Function Prototypes ---
int authenticateEmployee(int clientSocket, int employeeID, char *password_input);
void createNewCustomerAccount(int clientSocket);
void processLoanApplication(int clientSocket, int employeeID);
void viewAssignedLoans(int clientSocket, int employeeID);
int updateEmployeePassword(int clientSocket, int employeeID);
void handleEmployeeSession(int clientSocket); // Main handler prototype
// Note: modifyUser and viewTransactionLogs are declared/defined elsewhere (admin_ops.h, customer_ops.h)

// --- Function Definitions ---

// Authenticate a employee member (Role 1)
int authenticateEmployee(int clientSocket, int employeeID, char *password_input)
{
    struct Employee employee;
    int dbFile = open(EMPLOYEE_DB, O_RDONLY); // Open RDONLY first
    if (dbFile == -1) {
         if (errno == ENOENT) { // File doesn't exist? Create empty one
             dbFile = open(EMPLOYEE_DB, O_WRONLY | O_CREAT, 0644);
             if (dbFile != -1) close(dbFile);
             printf("Created empty employee database file: %s\n", EMPLOYEE_DB);
         } else {
            perror("AuthEmployee: Error opening employee DB for read");
            return 0; // Cannot authenticate
         }
    } else {
        close(dbFile); // Close read handle if it existed
    }

    // Session lock
    sessionSemaphore = createSessionLock(employeeID);
    if (sessionSemaphore == SEM_FAILED) {
         perror("AuthEmployee: Failed to create/open session semaphore");
         return 0;
    }
    setupSignalHandlers();

    if (sem_trywait(sessionSemaphore) == -1) {
        if (errno == EAGAIN) {
            printf("Employee %d is already logged in!\n", employeeID);
            bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "This ID is already logged in elsewhere.^");
            write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        } else {
            perror("AuthEmployee: sem_trywait failed");
        }
        sem_close(sessionSemaphore); // Close our handle
        return 0;
    }

    // Now open for reading credentials
     dbFile = open(EMPLOYEE_DB, O_RDONLY);
     if (dbFile == -1) {
          perror("AuthEmployee: Error opening employee DB for login check");
          sem_post(sessionSemaphore); sem_close(sessionSemaphore); sem_unlink(sessionSemName);
          return 0;
     }

    // Check credentials
    int loggedIn = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &employee, sizeof(employee)) == sizeof(employee)) // Check read result
    {
        // Compare sanitized input against stored password
        if (employee.employeeID == employeeID && strcmp(employee.password, password_input) == 0 && employee.roleType == 1) { // 1 = Employee
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
    printf("Employee %d logged in.\n", employeeID);
    return 1; // Success - Semaphore still held!
}

// Add a new customer account
void createNewCustomerAccount(int clientSocket) {
    struct AccountHolder account, tempAccount;
    struct TransactionLog log;

    // --- Get all data from client first ---
    // Get Name
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Name: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { printf("Client disconnected.\n"); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    strncpy(account.holderName, inBuffer, sizeof(account.holderName) - 1);
    account.holderName[sizeof(account.holderName)-1] = '\0';


    // Get Password
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { printf("Client disconnected.\n"); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    strncpy(account.password, inBuffer, sizeof(account.password) - 1);
    account.password[sizeof(account.password)-1] = '\0';


    // Get Account Number
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Account Number: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { printf("Client disconnected.\n"); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    account.accountID = atoi(inBuffer);

    // --- Check for duplicate Account Number ---
    int dbFile = open(ACCOUNT_DB, O_RDWR | O_CREAT, 0644);
    if(dbFile == -1) {
        perror("CreateCust: Error opening account DB");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Acquire an EXCLUSIVE lock on the ENTIRE file
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("CreateCust: Failed to lock account DB");
        close(dbFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    int duplicateFound = 0;
    lseek(dbFile, 0, SEEK_SET); // Go to start
    while(read(dbFile, &tempAccount, sizeof(tempAccount)) == sizeof(tempAccount)) {
        if (tempAccount.accountID == account.accountID) {
            duplicateFound = 1;
            break;
        }
    }

    if (duplicateFound) {
        lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Account number already exists.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return; // Stop the function
    }
    // --- End of duplicate check ---

    // Get Opening Balance
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Opening Balance: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected.\n"); goto createcust_unlock_fail;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    account.currentBalance = atof(inBuffer);
    if (account.currentBalance < 0) account.currentBalance = 0; // Ensure non-negative balance


    // --- Now, open log file and write data ---
    time_t now = time(NULL);
	struct tm* localTime = localtime(&now);

    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(logFile == -1) {
        perror("CreateCust: Error opening log file");
        // Log locally, proceed with account creation but inform client?
        printf("CRITICAL: Account %d created but initial log failed!\n", account.accountID);
        // Continue to write account, but then send error message.
    } else {
        // Lock log file for appending
        struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file
        fcntl(logFile, F_SETLKW, &logLock);

        bzero(log.logEntry, sizeof(log.logEntry));
        sprintf(log.logEntry, "%.2f Opening Balance at %02d:%02d:%02d %d-%d-%d\n",
                account.currentBalance, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
                (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);
        log.logEntry[sizeof(log.logEntry)-1]='\0'; // Ensure null term
        log.accountID = account.accountID;
        write(logFile, &log, sizeof(log));

        logLock.l_type = F_UNLCK; fcntl(logFile, F_SETLK, &logLock);
        close(logFile);
    }

    // Save account (we still hold the lock on dbFile)
    account.isActive = 1; // Active by default
    lseek(dbFile, 0, SEEK_END); // Seek to end
    write(dbFile, &account, sizeof(account));

    // Unlock account file
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Employee added customer %d\n", account.accountID);

    // Send appropriate confirmation message
    if (logFile == -1) {
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Customer added BUT LOG FAILED!^");
    } else {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Customer added successfully!^");
    }
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    return;

createcust_unlock_fail: // Cleanup label
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return;
}


// Process a loan application assigned to this employee member
void processLoanApplication(int clientSocket, int employeeID)
{
    char logBuffer[1024];
    struct LoanRecord loan;
    struct AccountHolder account;
    struct TransactionLog log;

    time_t now = time(NULL);
	struct tm* localTime = localtime(&now);

    int loanID;
    int loanFile = open(LOAN_DB, O_RDWR);
    int accountFile = open(ACCOUNT_DB, O_RDWR); // Open account file here

    if(loanFile == -1 || accountFile == -1) {
        perror("ProcessLoan: Error opening DB files");
        if(loanFile != -1) close(loanFile); if(accountFile != -1) close(accountFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Get Loan ID
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Loan ID to process: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { goto loanproc_close_files; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    loanID = atoi(inBuffer);

    // Find loan record offset
    int loanOffset = -1;
    off_t currentPosLoan = 0;
    lseek(loanFile, 0, SEEK_SET);
    while (read(loanFile, &loan, sizeof(loan)) == sizeof(loan))
    {
        currentPosLoan = lseek(loanFile, 0, SEEK_CUR) - sizeof(loan);
        if(loan.loanRecordID == loanID) {
            loanOffset = currentPosLoan;
            break;
        }
    }

    if(loanOffset == -1) {
        bzero(outBuffer, sizeof(outBuffer)); sprintf(outBuffer, "Loan ID %d not found.^", loanID);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto loanproc_close_files;
    }

    // Check if assigned to this employee and is pending
    if(loan.assignedEmployeeID != employeeID || loan.loanStatus != 1) { // 1 = Pending
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan ID %d is not assigned to you or is not pending.^", loanID);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto loanproc_close_files;
    }

    // Lock the loan record
    struct flock loanLock = {F_WRLCK, SEEK_SET, loanOffset, sizeof(struct LoanRecord), getpid()};
    if(fcntl(loanFile, F_SETLKW, &loanLock) == -1) {
        perror("ProcessLoan: Loan lock failed"); goto loanproc_close_files;
    }

    // Find account record offset
    int accountOffset = -1;
    off_t currentPosAcc = 0;
    lseek(accountFile, 0, SEEK_SET);
    while (read(accountFile, &account, sizeof(account)) == sizeof(account))
    {
        currentPosAcc = lseek(accountFile, 0, SEEK_CUR) - sizeof(account);
        if(account.accountID == loan.accountID) {
            accountOffset = currentPosAcc;
            break;
        }
    }
    if(accountOffset == -1) {
        // This is a data inconsistency error! Loan exists but account doesn't.
        printf("CRITICAL ERROR: Loan %d exists but account %d not found!\n", loanID, loan.accountID);
        bzero(outBuffer, sizeof(outBuffer)); sprintf(outBuffer, "Error: Account %d for loan %d not found!^", loan.accountID, loanID);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto loanproc_unlock_loan;
    }

    // Lock the account record
    struct flock accLock = {F_WRLCK, SEEK_SET, accountOffset, sizeof(struct AccountHolder), getpid()};
    if(fcntl(accountFile, F_SETLKW, &accLock) == -1) {
         perror("ProcessLoan: Account lock failed"); goto loanproc_unlock_loan;
    }


    // Re-read account data after lock
    lseek(accountFile, accountOffset, SEEK_SET);
     if (read(accountFile, &account, sizeof(account)) != sizeof(account)) {
         perror("ProcessLoan: Account re-read failed"); goto loanproc_unlock_both;
     }

    // Re-read loan data after lock (in case status changed between find and lock)
    lseek(loanFile, loanOffset, SEEK_SET);
     if (read(loanFile, &loan, sizeof(loan)) != sizeof(loan)) {
         perror("ProcessLoan: Loan re-read failed"); goto loanproc_unlock_both;
     }
     // Re-verify assignment and status *after* locks
     if(loan.assignedEmployeeID != employeeID || loan.loanStatus != 1) {
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan ID %d status changed before processing.^", loanID);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto loanproc_unlock_both;
    }


    // Get decision
    int choice;
    bzero(outBuffer, sizeof(outBuffer));
    // Provide some context (optional but helpful)
    sprintf(outBuffer, "Processing Loan ID: %d\nAccount: %d (%s)\nCurrent Balance: %.2f\nLoan Amount: %d\n[1] Approve Loan\n[2] Reject Loan\nChoice: ",
             loanID, account.accountID, account.holderName, account.currentBalance, loan.amount);
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
         printf("Client disconnected during loan decision.\n"); goto loanproc_unlock_both;
     }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    choice = atoi(inBuffer);

    int logFile = -1; // Initialize log file descriptor

    if(choice == 1) // Approve
    {
        if(account.isActive == 0)
        {
            loan.loanStatus = 3; // 3 = Rejected
            printf("Loan %d rejected for inactive account %d\n", loanID, account.accountID);
            bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Account is inactive. Loan rejected.^");
            // No balance change, no log needed. Just update loan status.
        }
        else
        {
            account.currentBalance += loan.amount;
            loan.loanStatus = 2; // 2 = Approved

            // --- Log Transaction ---
            logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (logFile == -1) {
                perror("ProcessLoan (Approve): Error opening log file");
                printf("CRITICAL: Loan %d approved for %d but logging failed!\n", loanID, account.accountID);
                // Update account & loan anyway, send warning message
                 bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Loan Approved BUT LOGGING FAILED!^");
            } else {
                struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
                fcntl(logFile, F_SETLKW, &logLock);

                bzero(logBuffer, sizeof(logBuffer));
                sprintf(logBuffer, "%d credited via loan %d at %02d:%02d:%02d %d-%d-%d\n",
                        loan.amount, loanID, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
                        (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);
                bzero(log.logEntry, sizeof(log.logEntry));
                strncpy(log.logEntry, logBuffer, sizeof(log.logEntry)-1); log.logEntry[sizeof(log.logEntry)-1]='\0';
                log.accountID = account.accountID;
                write(logFile, &log, sizeof(log));

                logLock.l_type = F_UNLCK; fcntl(logFile, F_SETLK, &logLock);
                close(logFile);
                 bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Loan Approved.^");
            }
             // --- End Log ---

            // Update account file
            lseek(accountFile, accountOffset, SEEK_SET);
            write(accountFile, &account, sizeof(account));

            printf("Loan %d approved for account %d\n", loanID, account.accountID);
        }
    }
    else if (choice == 2) // Reject
    {
        loan.loanStatus = 3; // 3 = Rejected
        printf("Loan %d rejected for account %d\n", loanID, account.accountID);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Loan Rejected.^");
        // No balance change, no log needed. Just update loan status.
    }
    else // Invalid choice
    {
         printf("Invalid choice (%d) for loan %d.\n", choice, loanID);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid choice. No action taken.^");
         // No status change, unlock and return
         goto loanproc_unlock_both_ack; // Jump to send ack and unlock
    }


    // Write updated loan status
    lseek(loanFile, loanOffset, SEEK_SET);
    write(loanFile, &loan, sizeof(loan));

loanproc_unlock_both_ack:
    write(clientSocket, outBuffer, strlen(outBuffer)); // Send result message
    read(clientSocket, inBuffer, 3); // ack

loanproc_unlock_both:
    accLock.l_type = F_UNLCK; fcntl(accountFile, F_SETLK, &accLock);
loanproc_unlock_loan:
    loanLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &loanLock);
loanproc_close_files:
    close(accountFile);
    close(loanFile);
}

// View loans currently assigned to this employee member and pending
void viewAssignedLoans(int clientSocket, int employeeID)
{
    struct LoanRecord loan;
    int loanFile = open(LOAN_DB, O_RDONLY);
    if(loanFile == -1) {
        perror("ViewLoans: Error opening file");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error retrieving assigned loans.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; // Shared read lock
    fcntl(loanFile, F_SETLKW, &lock);

    int found = 0;
    while(read(loanFile, &loan, sizeof(loan)) == sizeof(loan))
    {
        if(loan.assignedEmployeeID == employeeID && loan.loanStatus == 1) // 1 = Pending
        {
            bzero(outBuffer, sizeof(outBuffer));
            sprintf(outBuffer, "Loan ID: %d | Account: %d | Amount: %d^",
                    loan.loanRecordID, loan.accountID, loan.amount);
            write(clientSocket, outBuffer, strlen(outBuffer));
            read(clientSocket, inBuffer, 3); // ack for each record sent
            found = 1;
        }
    }

    lock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &lock);
    close(loanFile);

    if(!found) {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "No pending assigned loans found.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    } else {
        // Send final empty ack after the last record or if none were found previously
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    }
}

// Change the password for a employee member or manager
int updateEmployeePassword(int clientSocket, int employeeID) // Used by both employee and Manager
{
    char newPassword[50];
    struct Employee employee;
    int dbFile = open(EMPLOYEE_DB, O_RDWR);
     if(dbFile == -1) {
        perror("ChangePass: Error opening DB");
        return 0; // Failure
     }

    // Find offset
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while (read(dbFile, &employee, sizeof(employee)) > 0)
    {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(employee);
        if(employee.employeeID == employeeID) {
            offset = currentPos;
            break;
        }
    }
    if(offset == -1) {
        printf("Changepass: employee/Manager ID %d not found\n", employeeID);
        close(dbFile); return 0; // Failure
     }

    // Lock record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct Employee), getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
         perror("ChangePass: Failed to lock record");
         close(dbFile); return 0; // Failure
    }


    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter new password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
         printf("Client disconnected during password change entry.\n");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         return 0; // Failure
     }
    // Sanitize password input
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(newPassword, inBuffer, sizeof(newPassword) - 1);
    newPassword[sizeof(newPassword) - 1] = '\0';


    // Re-read data before writing
    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &employee, sizeof(employee)) != sizeof(employee)) {
        perror("ChangePass: Re-read failed");
        lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
        return 0; // Failure
    }

    // Update password in struct
    strncpy(employee.password, newPassword, sizeof(employee.password) - 1);
    employee.password[sizeof(employee.password) - 1] = '\0';


    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &employee, sizeof(employee));

    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("employee/Manager %d changed password\n", employeeID);
    return 1; // Success
}

// Main handler for the employee menu and actions
void handleEmployeeSession(int clientSocket)
{
    int authEmployeeID = -1, accountChoice; // Initialize
    char password[51]; // Buffer for password input, size 51

label_employee_login:
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "\nEnter Employee ID: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    authEmployeeID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    // Sanitize password input for login
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(password, inBuffer, sizeof(password) - 1);
    password[sizeof(password)-1] = '\0';


    // Pass sanitized password
    if(authenticateEmployee(clientSocket, authEmployeeID, password))
    {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nLogin Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack

        while(1)
        {
            bzero(outBuffer, sizeof(outBuffer));
            strcpy(outBuffer, EMPLOYEE_PROMPT);
            write(clientSocket, outBuffer, strlen(outBuffer));

            bzero(inBuffer, sizeof(inBuffer));
            if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                 printf("employee %d disconnected during session.\n", authEmployeeID);
                terminateClientSession(clientSocket, authEmployeeID);
                return;
            }
            inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize choice
            int choice = atoi(inBuffer);
            printf("employee %d choice: %d\n", authEmployeeID, choice);

            switch(choice)
            {
                case 1: // Add New Customer
                    createNewCustomerAccount(clientSocket); // Handles own acks
                    break;
                case 2: // Modify Customer Details
                    modifyUser(clientSocket, 1); // Handles own acks
                    break;
                case 3: // Approve/Reject Loans
                    processLoanApplication(clientSocket, authEmployeeID); // Handles own acks
                    break;
                case 4: // View Assigned Loan Applications
                    viewAssignedLoans(clientSocket, authEmployeeID); // Handles own acks
                    break;
                case 5: // View Customer Transactions
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Enter Account Number: ");
                    write(clientSocket, outBuffer, strlen(outBuffer));

                    bzero(inBuffer, sizeof(inBuffer));
                    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) goto disconnect_cleanup_employee;
                     inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
                    accountChoice = atoi(inBuffer);

                    viewTransactionLogs(clientSocket, accountChoice); // Handles own acks
                    break;
                case 6: // Change Password
                    if(updateEmployeePassword(clientSocket, authEmployeeID)) {
                        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password changed. Please log in again.^");
                    } else {
                        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password change failed.^");
                    }
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                    endUserSession(clientSocket, authEmployeeID);
                    authEmployeeID = -1;
                    goto label_employee_login; // Force re-login
                case 7: // Logout
                    printf("Employee ID: %d Logged Out!\n", authEmployeeID);
                    endUserSession(clientSocket, authEmployeeID);
                    authEmployeeID = -1;
                    return; // Back to main server loop
                case 8: // Exit
                    printf("Employee ID: %d Exited!\n", authEmployeeID);
                    terminateClientSession(clientSocket, authEmployeeID);
                    authEmployeeID = -1;
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
        authEmployeeID = -1;
        goto label_employee_login;
    }

disconnect_cleanup_employee: // Label for handling disconnects
    printf("Employee %d disconnected unexpectedly.\n", authEmployeeID);
    if (authEmployeeID > 0) {
        terminateClientSession(clientSocket, authEmployeeID);
    }
    return; // Exit child process
}


#endif 
