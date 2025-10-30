#include <errno.h> // For checking errno with sem_trywait
#include <stdio.h> // For perror
#include <string.h> // For strcspn, strncpy, strcmp, strlen
#include <stdlib.h> // For atof, atoi
#include <time.h>   // For logging timestamp
#include <unistd.h> // For read, write, lseek, close
#include <fcntl.h>  // For file constants (O_RDWR etc) and fcntl
#include <sys/types.h> // For off_t, pid_t

// --- Function Prototypes ---
void endUserSession(int clientSocket, int sessionID);
int authenticateCustomer(int clientSocket, int accountID, char *password_input);
void processDeposit(int clientSocket, int accountID);
void checkBalance(int clientSocket, int accountID);
void processWithdrawal(int clientSocket, int accountID);
void requestLoan(int clientSocket, int accountID);
void executeTransfer(int clientSocket, int sourceAccountID, int destAccountID, float transferAmount);
void viewTransactionLogs(int clientSocket, int accountID);
void submitFeedback(int clientSocket);
int updateCustomerPassword(int clientSocket, int accountID);
// void handleCustomerSession(int clientSocket); // Defined below

// --- Function Definitions ---

// ======================= Shared Session Function =======================
void endUserSession(int clientSocket, int sessionID){
    snprintf(sessionSemName, 50, "/bms_sem_%d", sessionID);

    sem_t *sema = sem_open(sessionSemName, 0);
    if (sema != SEM_FAILED) {
        sem_post(sema);
        sem_close(sema);
        sem_unlink(sessionSemName);
        printf("Session lock for ID %d released.\n", sessionID);
    } else {
        printf("Semaphore for ID %d might already be unlinked.\n", sessionID);
    }

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "^"); // Signal end of operation
    write(clientSocket, outBuffer, strlen(outBuffer)); // Send exact length

    bzero(inBuffer, sizeof(inBuffer));
    read(clientSocket, inBuffer, 3); // Read "ACK" or handle timeout/error
}


// Renamed loginCustomer
int authenticateCustomer(int clientSocket, int accountID, char *password_input) {
    struct AccountHolder account;
    int dbFile = open(ACCOUNT_DB, O_RDONLY); // Open RDONLY first for check

    if (dbFile == -1) {
        // If file doesn't exist, create it (edge case for first run)
        if (errno == ENOENT) {
            dbFile = open(ACCOUNT_DB, O_WRONLY | O_CREAT, 0644);
            if (dbFile != -1) close(dbFile); // Just create and close
            printf("Created empty account database file: %s\n", ACCOUNT_DB);
            // Now attempt login again (will fail, but semaphore logic runs)
        } else {
            perror("Error opening account DB for read");
            return 0;
        }
    } else {
        close(dbFile); // Close read-only handle if it existed
    }


    // Initialize and try to lock the semaphore for this session
    sessionSemaphore = createSessionLock(accountID);
     if (sessionSemaphore == SEM_FAILED) {
         perror("Failed to create/open session semaphore");
         return 0;
    }
    setupSignalHandlers(); // Setup signal handlers for cleanup in this process

    if (sem_trywait(sessionSemaphore) == -1) {
        if (errno == EAGAIN) {
            printf("Account %d is already logged in!\n", accountID);
            bzero(outBuffer, sizeof(outBuffer));
            strcpy(outBuffer, "This account is already logged in elsewhere.^");
            write(clientSocket, outBuffer, strlen(outBuffer));
            read(clientSocket, inBuffer, 3); // Ack
        } else {
            perror("sem_trywait failed");
        }
        sem_close(sessionSemaphore);
        // Do NOT unlink here
        return 0;
    }

    // Now open for reading credentials
    dbFile = open(ACCOUNT_DB, O_RDONLY);
    if (dbFile == -1) {
         perror("Error opening account DB for login check");
         sem_post(sessionSemaphore); // Release lock before returning error
         sem_close(sessionSemaphore);
         sem_unlink(sessionSemName);
         return 0;
    }


    // Check credentials
    int loggedIn = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &account, sizeof(account)) > 0)
    {
        // Compare input password (without newline) against stored password
        if (account.accountID == accountID && strcmp(account.password, password_input) == 0 && account.isActive == 1) {
            printf("Customer %d logged in.\n", accountID);
            loggedIn = 1;
            break; // Found the user
        }
    }
    close(dbFile); // Close file regardless of login success

    if (!loggedIn) {
        // Login failed, release the lock
        sem_post(sessionSemaphore);
        sem_close(sessionSemaphore);
        sem_unlink(sessionSemName); // Unlink since we held it briefly
        return 0; // Failure
    }

    return 1; // Success - IMPORTANT: Semaphore is still held by this process!
}

// Renamed depositMoney
void processDeposit(int clientSocket, int accountID){
    char logBuffer[1024];
    struct AccountHolder account;
    struct TransactionLog log;
    float depositAmount;

    time_t now = time(NULL);
	struct tm* localTime = localtime(&now);

    int dbFile = open(ACCOUNT_DB, O_RDWR);
    if (dbFile == -1) {
        perror("Deposit: Error opening account DB");
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    // Find the account record offset *before* locking
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while (read(dbFile, &account, sizeof(account)) > 0) {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account); // Get start pos of record read
        if (account.accountID == accountID) {
            offset = currentPos;
            break;
        }
    }

    if(offset == -1) {
        printf("Deposit: Error - Account %d not found.\n", accountID);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Account not found.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        close(dbFile);
        return;
    }

    // Lock the specific account record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("Deposit: Failed to lock account record");
        close(dbFile);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing deposit (lock fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    // Get amount from client
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter the amount to deposit: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer) - 1) <= 0) {
        printf("Client disconnected during deposit amount entry.\n");
        lock.l_type = F_UNLCK; // Unlock before exiting
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize

    depositAmount = atof(inBuffer);
    if(depositAmount <= 0) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Invalid deposit amount.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // Wait for ack

        lock.l_type = F_UNLCK; // Unlock
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }

    // Re-read data after lock acquisition
    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("Deposit: Failed to re-read record after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error reading account data.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }


    // Perform transaction
    account.currentBalance += depositAmount;

    // --- Log Transaction ---
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644); // Use WRONLY for append
    if (logFile == -1) {
        perror("Deposit: Error opening log file");
        // Log locally, inform client about partial failure?
        printf("CRITICAL: Deposit to %d occurred but logging failed!\n", accountID);
        // Still update account file, but warn client.
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account)); // Write updated account

        lock.l_type = F_UNLCK; // Unlock account
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Deposit successful BUT LOGGING FAILED! New Balance: %.2f^", account.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }
    // Lock log file for append (whole file lock for simplicity here)
    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file for append safety
    fcntl(logFile, F_SETLKW, &logLock);

    bzero(logBuffer, sizeof(logBuffer));
    sprintf(logBuffer, "%.2f deposited at %02d:%02d:%02d %d-%d-%d\n",
            depositAmount, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
            (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);

    bzero(log.logEntry, sizeof(log.logEntry));
    strncpy(log.logEntry, logBuffer, sizeof(log.logEntry) - 1);
    log.logEntry[sizeof(log.logEntry)-1] = '\0';
    log.accountID = account.accountID;
    write(logFile, &log, sizeof(log));

    // Unlock log file
    logLock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &logLock);
    close(logFile);
    // --- End Log Transaction ---


    // Update account file
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    // Unlock the account record
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Account %d deposited %.2f. New balance: %.2f\n", accountID, depositAmount, account.currentBalance);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Deposit successful! New Balance: %.2f^", account.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // Wait for ack
}

// Renamed customerBal
void checkBalance(int clientSocket, int accountID){
    struct AccountHolder account;
    int dbFile = open(ACCOUNT_DB, O_RDONLY);
    if (dbFile == -1) {
        perror("Balance Check: Error opening account DB");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error retrieving balance.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }
    float balance = -1.0; // Indicate error if not found

    // Use a read lock for safety
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file for read
    fcntl(dbFile, F_SETLKW, &lock);

    while(read(dbFile, &account, sizeof(account)) > 0) {
        if (account.accountID == accountID) {
            balance = account.currentBalance;
            break;
        }
    }

    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    if (balance >= 0) {
        printf("Balance check for %d: %.2f\n", accountID, balance);
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "The current balance is: %.2f^", balance);
    } else {
        printf("Balance check failed for %d: Account not found\n", accountID);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Account not found.^");
    }
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // Wait for ack
}

// Renamed withdrawMoney
void processWithdrawal(int clientSocket, int accountID){
    char logBuffer[1024];
    struct AccountHolder account;
    struct TransactionLog log;
    float withdrawAmount;

    time_t now = time(NULL);
	struct tm* localTime = localtime(&now);

    int dbFile = open(ACCOUNT_DB, O_RDWR);
     if (dbFile == -1) {
        perror("Withdraw: Error opening account DB");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    // Find record and get offset
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &account, sizeof(account)) > 0) {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account);
        if(account.accountID == accountID) {
            offset = currentPos;
            break;
        }
    }
    if(offset == -1) {
        printf("Withdraw: Error - Account %d not found.\n", accountID);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Account not found.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        close(dbFile);
        return;
    }

    // Lock the record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("Withdraw: Failed to lock record");
        close(dbFile);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing withdrawal (lock fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    // Get amount
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter the amount to withdraw: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during withdrawal amount entry.\n");
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize

    withdrawAmount = atof(inBuffer);

    // Re-read data after lock
    lseek(dbFile, offset, SEEK_SET);
     if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("Withdraw: Failed to re-read record after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error reading account data.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }


    // Check funds
    if (withdrawAmount <= 0 || account.currentBalance < withdrawAmount ){
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Insufficient funds or invalid amount! Balance: %.2f^", account.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack

        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }

    // Process transaction
    account.currentBalance -= withdrawAmount;

     // --- Log Transaction ---
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (logFile == -1) {
        perror("Withdraw: Error opening log file");
        printf("CRITICAL: Withdraw from %d occurred but logging failed!\n", accountID);
        // Update account file anyway
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account));
        lock.l_type = F_UNLCK; // Unlock account
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        // Inform client
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Withdrawal successful BUT LOGGING FAILED! Balance: %.2f^", account.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole log file
    fcntl(logFile, F_SETLKW, &logLock);

    bzero(logBuffer, sizeof(logBuffer));
    sprintf(logBuffer, "%.2f withdrawn at %02d:%02d:%02d %d-%d-%d\n",
            withdrawAmount, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
            (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);

    bzero(log.logEntry, sizeof(log.logEntry));
    strncpy(log.logEntry, logBuffer, sizeof(log.logEntry) - 1);
     log.logEntry[sizeof(log.logEntry)-1] = '\0';
    log.accountID = account.accountID;
    write(logFile, &log, sizeof(log));

    logLock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &logLock);
    close(logFile);
    // --- End Log Transaction ---


    // Update account file
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    // Unlock account record
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Account %d withdrew %.2f. New balance: %.2f\n", accountID, withdrawAmount, account.currentBalance);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Withdrawal successful! New Balance: %.2f^", account.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // ack
}

// Renamed applyLoan
void requestLoan(int clientSocket, int accountID){
    struct IDGenerator idGen;
    struct LoanRecord loan;

    // Get and update the next loan ID
    int counterFile = open(LOAN_COUNTER_DB, O_RDWR | O_CREAT, 0644);
    if(counterFile == -1) {
        perror("Loan Request: Failed to open counter DB");
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing loan request (counter fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
     }

    // Lock the entire counter file
    struct flock idLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    if (fcntl(counterFile, F_SETLKW, &idLock) == -1) {
        perror("Loan Request: Failed to lock counter DB");
        close(counterFile);
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing loan request (counter lock fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    int newLoanID;
    if(read(counterFile, &idGen, sizeof(idGen)) > 0) {
        newLoanID = idGen.nextID;
    } else {
        newLoanID = 1;
        idGen.nextID = 1;
    }
    idGen.nextID = newLoanID + 1; // Prepare for next time
    lseek(counterFile, 0, SEEK_SET);
    write(counterFile, &idGen, sizeof(idGen));

    // Unlock counter file
    idLock.l_type = F_UNLCK;
    fcntl(counterFile, F_SETLK, &idLock);
    close(counterFile);

    // Get loan amount from client
    int loanAmount;
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Loan Amount: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0){
        printf("Client disconnected during loan amount entry.\n"); return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    loanAmount = atoi(inBuffer);

    if(loanAmount <= 0) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Invalid loan amount.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    // Open loan DB for append
    int loanFile = open(LOAN_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(loanFile == -1) {
        perror("Loan Request: Failed to open loan DB");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing loan request (db fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

     // Lock for append safety
    struct flock loanDBLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file
    fcntl(loanFile, F_SETLKW, &loanDBLock);

    loan.assignedEmployeeID = -1;
    loan.accountID = accountID;
    loan.amount = loanAmount;
    loan.loanStatus = 0; // Requested
    loan.loanRecordID = newLoanID;

    write(loanFile, &loan, sizeof(loan));

    loanDBLock.l_type = F_UNLCK;
    fcntl(loanFile, F_SETLK, &loanDBLock);
    close(loanFile);

    printf("Loan %d for amount %d from account %d requested.\n", newLoanID, loanAmount, accountID);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Loan %d for amount %d has been requested.^", newLoanID, loanAmount);
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // ack
}

// Renamed transferFunds
void executeTransfer(int clientSocket, int sourceAccountID, int destAccountID, float transferAmount) {
    char logBuffer[1024];
    struct AccountHolder sourceAccount, destAccount, tempAccount;
    struct TransactionLog log;

    if(sourceAccountID == destAccountID) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Cannot transfer to the same account.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    if(transferAmount <= 0) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Invalid transfer amount.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    time_t now = time(NULL);
	struct tm* localTime = localtime(&now);

    int dbFile = open(ACCOUNT_DB, O_RDWR);
    if(dbFile == -1) {
        perror("Transfer: Error opening account DB");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Database error during transfer.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Find offsets for both accounts robustly
    int srcOffset = -1, dstOffset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(1) {
        currentPos = lseek(dbFile, 0, SEEK_CUR); // Pos before read
        int bytesRead = read(dbFile, &tempAccount, sizeof(tempAccount));
        if (bytesRead <= 0) break; // EOF or read error

        if(tempAccount.accountID == sourceAccountID) {
            srcOffset = currentPos;
        }
        else if(tempAccount.accountID == destAccountID) {
            dstOffset = currentPos;
        }
        if(srcOffset != -1 && dstOffset != -1) break; // Found both
    }


    if(dstOffset == -1) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Destination account does not exist.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(dbFile);
        return;
    }
    if(srcOffset == -1) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Source account not found. Critical error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(dbFile);
        return;
    }

    // Lock records. Lock lower offset first to prevent deadlock.
    struct flock lock1 = {F_WRLCK, SEEK_SET, 0, sizeof(struct AccountHolder), getpid()};
    struct flock lock2 = {F_WRLCK, SEEK_SET, 0, sizeof(struct AccountHolder), getpid()};

    // Assign locks based on which offset is smaller
    if (srcOffset < dstOffset) {
        lock1.l_start = srcOffset;
        lock2.l_start = dstOffset;
    } else {
        lock1.l_start = dstOffset;
        lock2.l_start = srcOffset;
    }

    // Always lock in the same order (lowest offset first)
    if (fcntl(dbFile, F_SETLKW, &lock1) == -1) {
        perror("Transfer: Fcntl lock1 failed"); close(dbFile); return;
    }
    if (fcntl(dbFile, F_SETLKW, &lock2) == -1) {
        perror("Transfer: Fcntl lock2 failed");
        lock1.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock1); close(dbFile); return;
    }

    // Read data *after* acquiring locks
    lseek(dbFile, srcOffset, SEEK_SET);
     if (read(dbFile, &sourceAccount, sizeof(sourceAccount)) != sizeof(sourceAccount)) {
         perror("Transfer: Failed read source after lock"); goto unlock_close;
     }
    lseek(dbFile, dstOffset, SEEK_SET);
     if (read(dbFile, &destAccount, sizeof(destAccount)) != sizeof(destAccount)) {
         perror("Transfer: Failed read dest after lock"); goto unlock_close;
     }

    // Check funds
    if (sourceAccount.currentBalance < transferAmount) {
        printf("Transfer: Insufficient funds (%.2f < %.2f).\n", sourceAccount.currentBalance, transferAmount);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Insufficient funds.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto unlock_close;
    }

    // Perform transfer
    sourceAccount.currentBalance -= transferAmount;
    destAccount.currentBalance += transferAmount;

    // --- Log Transactions ---
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(logFile == -1) {
        perror("Transfer: Error opening log file");
        printf("CRITICAL: Transfer between %d and %d occurred but logging failed!\n", sourceAccountID, destAccountID);
        // Write account changes anyway but warn client
        lseek(dbFile, srcOffset, SEEK_SET); write(dbFile, &sourceAccount, sizeof(sourceAccount));
        lseek(dbFile, dstOffset, SEEK_SET); write(dbFile, &destAccount, sizeof(destAccount));
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Transfer successful BUT LOGGING FAILED! New Balance: %.2f^", sourceAccount.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto unlock_close; // Jump to cleanup
    }

    // Lock the log file
    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file
    fcntl(logFile, F_SETLKW, &logLock);

    // Log for source
    bzero(logBuffer, sizeof(logBuffer));
    sprintf(logBuffer,"%.2f transferred to acc %d at %02d:%02d:%02d %d-%d-%d\n",
            transferAmount, destAccountID, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
            (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);
    bzero(log.logEntry, sizeof(log.logEntry));
    strncpy(log.logEntry, logBuffer, sizeof(log.logEntry)-1); log.logEntry[sizeof(log.logEntry)-1]='\0';
    log.accountID = sourceAccountID;
    write(logFile, &log, sizeof(log));

    // Log for destination
    bzero(logBuffer, sizeof(logBuffer));
    sprintf(logBuffer,"%.2f credited from acc %d at %02d:%02d:%02d %d-%d-%d\n",
            transferAmount, sourceAccountID, localTime->tm_hour, localTime->tm_min, localTime->tm_sec,
            (localTime->tm_year)+1900, (localTime->tm_mon)+1, localTime->tm_mday);
    bzero(log.logEntry, sizeof(log.logEntry));
    strncpy(log.logEntry, logBuffer, sizeof(log.logEntry)-1); log.logEntry[sizeof(log.logEntry)-1]='\0';
    log.accountID = destAccountID;
    write(logFile, &log, sizeof(log));

    // Unlock log file
    logLock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &logLock);
    close(logFile);
    // --- End Log Transactions ---


    // Write back to account file
    lseek(dbFile, srcOffset, SEEK_SET);
    write(dbFile, &sourceAccount, sizeof(sourceAccount));
    lseek(dbFile, dstOffset, SEEK_SET);
    write(dbFile, &destAccount, sizeof(destAccount));


    printf("Transfer %.2f from %d to %d successful.\n", transferAmount, sourceAccountID, destAccountID);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Transfer successful! New Balance: %.2f^", sourceAccount.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);

unlock_close: // Label for unlocking and closing dbFile
    lock1.l_type = F_UNLCK;
    lock2.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock1);
    fcntl(dbFile, F_SETLK, &lock2);
    close(dbFile);
}

// Renamed transactionHistory
void viewTransactionLogs(int clientSocket, int accountID){
    struct TransactionLog log;
    int maxLogs = 10; // Show last 10
    int logCount = 0;
    off_t fileSize, readPos;


    int logFile = open(HISTORY_DB, O_RDONLY | O_CREAT, 0644); // Ensure file exists
    if(logFile == -1) {
        perror("View Logs: Error opening log file");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error retrieving transaction history.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Lock file for reading (shared lock)
    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(logFile, F_SETLKW, &lock);

    // --- Logic to read last N logs ---
    fileSize = lseek(logFile, 0, SEEK_END);
    logCount = fileSize / sizeof(struct TransactionLog);

    bzero(outBuffer, sizeof(outBuffer)); // Clear buffer
    int foundCount = 0;

    // Calculate starting position for reading backwards
    readPos = (logCount > maxLogs) ? fileSize - (maxLogs * sizeof(struct TransactionLog)) : 0;

    // If starting from 0, handle case where file isn't perfectly divisible
    if (readPos == 0 && fileSize > 0 && fileSize % sizeof(struct TransactionLog) != 0) {
       // Adjust readPos slightly if needed, though reading from 0 is usually fine
    }

    lseek(logFile, readPos, SEEK_SET);

    while(foundCount < maxLogs && read(logFile, &log, sizeof(log)) == sizeof(log))
    {
        if(log.accountID == accountID)
        {
            // Simple check to prevent buffer overflow
            if (strlen(outBuffer) + strlen(log.logEntry) + 1 < sizeof(outBuffer)) {
                 strcat(outBuffer, log.logEntry);
                 foundCount++;
            } else {
                 // Buffer full, maybe add indication?
                 strcat(outBuffer, "... (more entries truncated)\n");
                 break;
            }
        }
    }
    // --- End reading logic ---


    lock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &lock);
    close(logFile);

    if(foundCount == 0) {
        strcpy(outBuffer, "No transactions found.\n");
    }

    strcat(outBuffer, "^"); // Signal end
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // ack
}

// Renamed addFeedback
void submitFeedback(int clientSocket){
    struct ClientFeedback feedback;
    int feedbackFile = open(FEEDBACK_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(feedbackFile == -1) {
        perror("Feedback: Error opening file");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error submitting feedback.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
     }

    // Lock for append
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; // Lock whole file
    fcntl(feedbackFile, F_SETLKW, &lock);

    int choice;
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Feedback:\n1. Good\n2. Average\n3. Poor\nChoice: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <=0){
        printf("Client disconnected during feedback.\n"); goto feedback_unlock_close;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    choice = atoi(inBuffer);

    bzero(feedback.message, sizeof(feedback.message));
    if(choice == 1) strncpy(feedback.message, "Good", sizeof(feedback.message)-1);
    else if(choice == 2) strncpy(feedback.message, "Average", sizeof(feedback.message)-1);
    else if(choice == 3) strncpy(feedback.message, "Poor", sizeof(feedback.message)-1);
    else strncpy(feedback.message, "Unknown Choice", sizeof(feedback.message)-1);
    feedback.message[sizeof(feedback.message)-1] = '\0'; // Ensure null term


    write(feedbackFile, &feedback, sizeof(feedback));

feedback_unlock_close:
    lock.l_type = F_UNLCK;
    fcntl(feedbackFile, F_SETLK, &lock);
    close(feedbackFile);

    // Send confirmation regardless of potential disconnect during input
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Thank you for your feedback!^");
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // ack
}

// Renamed changePassword
int updateCustomerPassword(int clientSocket, int accountID){
    char newPassword[50];
    struct AccountHolder account;

    int dbFile = open(ACCOUNT_DB, O_RDWR);
    if(dbFile == -1) {
        perror("ChangePass: Error opening DB");
        return 0;
     }

    // Find offset
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &account, sizeof(account)) > 0) {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account);
        if(account.accountID == accountID) {
            offset = currentPos;
            break;
        }
    }
    if(offset == -1) {
        printf("ChangePass: Account %d not found\n", accountID);
        close(dbFile); return 0;
    }

    // Lock record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
         perror("ChangePass: Failed to lock record");
         close(dbFile); return 0;
    }

    // Get new password
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter new password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during password change entry.\n");
        lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
        return 0; // Indicate failure
    }
    // Sanitize password input - Remove trailing newline
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    // Ensure null termination and prevent overflow
    strncpy(newPassword, inBuffer, sizeof(newPassword) - 1);
    newPassword[sizeof(newPassword) - 1] = '\0';

    // Re-read data before writing
    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("ChangePass: Failed re-read after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         return 0;
    }


    // Update password in the struct (using the sanitized version)
    strncpy(account.password, newPassword, sizeof(account.password) - 1);
    account.password[sizeof(account.password) - 1] = '\0';

    // Write back
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    // Unlock
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Customer %d changed password\n", accountID);
    return 1; // Indicate success
}


// Renamed customerMenu
void handleCustomerSession(int clientSocket){
    struct AccountHolder account;
    int authAccountID = -1, destAccountID; // Initialize authAccountID
    int choice;
    char password[50]; // Buffer for password input

label_customer_login:
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "\nEnter account number: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected before login.\n"); return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize potential newline
    authAccountID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer,  "Enter password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
         printf("Client disconnected before login password.\n"); return;
     }
    // Sanitize password input for login
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Remove trailing newline
    strncpy(password, inBuffer, sizeof(password) - 1); // Copy safely
    password[sizeof(password)-1] = '\0'; // Ensure null termination


    // Pass the sanitized password to the authentication function
    if (authenticateCustomer(clientSocket, authAccountID, password))
    {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nLogin Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack

        while(1)
        {
            bzero(outBuffer, sizeof(outBuffer));
            strcpy(outBuffer, CUSTOMER_PROMPT);
            write(clientSocket, outBuffer, strlen(outBuffer));
            bzero(inBuffer, sizeof(inBuffer));
            if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                printf("Client %d disconnected during session.\n", authAccountID);
                terminateClientSession(clientSocket, authAccountID); // Release semaphore
                return;
            }
            inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize choice input

            choice = atoi(inBuffer);
            printf("Customer %d choice: %d\n", authAccountID, choice);

            switch(choice)
            {
                case 1: // Deposit
                    processDeposit(clientSocket, authAccountID);
                    break;
                case 2: // Withdraw
                    processWithdrawal(clientSocket, authAccountID);
                    break;
                case 3: // View Balance
                    checkBalance(clientSocket, authAccountID);;
                    break;
                case 4: // Apply Loan
                    requestLoan(clientSocket, authAccountID);
                    break;
                case 5: // Transfer
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Enter destination account number: ");
                    write(clientSocket, outBuffer, strlen(outBuffer));
                    bzero(inBuffer, sizeof(inBuffer));
                    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) goto disconnect_cleanup;
                    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
                    destAccountID = atoi(inBuffer);

                    float amount;
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Enter amount: ");
                    write(clientSocket, outBuffer, strlen(outBuffer));
                    bzero(inBuffer, sizeof(inBuffer));
                     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) goto disconnect_cleanup;
                     inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
                    amount = atof(inBuffer);

                    executeTransfer(clientSocket, authAccountID, destAccountID, amount);
                    break;
                case 6: // Change Password
                    if(updateCustomerPassword(clientSocket, authAccountID)) {
                        bzero(outBuffer, sizeof(outBuffer));
                        strcpy(outBuffer, "Password changed. Please log in again.^");
                        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
                    } else {
                         bzero(outBuffer, sizeof(outBuffer));
                        strcpy(outBuffer, "Password change failed.^");
                        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
                    }
                    endUserSession(clientSocket, authAccountID);
                    authAccountID = -1; // Reset auth ID after logout
                    goto label_customer_login; // Force re-login
                case 7: // View Transaction
                    viewTransactionLogs(clientSocket, authAccountID);
                    break;
                case 8: // Add Feedback
                    submitFeedback(clientSocket);
                    break;
                case 9: // Logout
                    printf("%d logged out.\n", authAccountID);
                    endUserSession(clientSocket, authAccountID);
                     authAccountID = -1; // Reset auth ID
                    return; // Back to main menu
                case 10: // Exit
                    printf("Customer: %d Exited!\n", authAccountID);
                    terminateClientSession(clientSocket, authAccountID);
                     authAccountID = -1; // Reset auth ID
                    return; // Will close socket in parent
                default:
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Invalid Choice^");
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
            }
        }
    }
    else // Login Failed
    {
        // Login failed (auth function handles sem cleanup if needed)
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nInvalid ID, Password, or Inactive Account^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         authAccountID = -1; // Reset auth ID
        goto label_customer_login;
    }

disconnect_cleanup: // Label for handling disconnects within the switch case
    printf("Client %d disconnected unexpectedly.\n", authAccountID);
    if (authAccountID > 0) { // Only terminate if logged in
         terminateClientSession(clientSocket, authAccountID);
    }
    return;
}
