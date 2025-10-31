#include <errno.h> 
#include <stdio.h> 
#include <string.h> 
#include <stdlib.h>
#include <time.h> 
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h> 

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
    strcpy(outBuffer, "^"); 
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    read(clientSocket, inBuffer, 3); 
}


// login customer
int authenticateCustomer(int clientSocket, int accountID, char *password_input) {
    struct AccountHolder account;
    int dbFile = open(ACCOUNT_DB, O_RDONLY);

    if (dbFile == -1) {
        if (errno == ENOENT) {
            dbFile = open(ACCOUNT_DB, O_WRONLY | O_CREAT, 0644);
            if (dbFile != -1) close(dbFile); // crate if first time and close
            printf("Created empty account database file: %s\n", ACCOUNT_DB);
        } else {
            perror("Error opening account DB for read");
            return 0;
        }
    } else {
        close(dbFile);
    }
    // sem init
    sessionSemaphore = createSessionLock(accountID);
     if (sessionSemaphore == SEM_FAILED) {
         perror("Failed to create/open session semaphore");
         return 0;
    }
    setupSignalHandlers(); 

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
        return 0;
    }

    dbFile = open(ACCOUNT_DB, O_RDONLY);
    if (dbFile == -1) {
         perror("Error opening account DB for login check");
         sem_post(sessionSemaphore); // Release lock before returning error
         sem_close(sessionSemaphore);
         sem_unlink(sessionSemName);
         return 0;
    }

    int loggedIn = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &account, sizeof(account)) > 0)
    {
        if (account.accountID == accountID && strcmp(account.password, password_input) == 0 && account.isActive == 1) {
            printf("Customer %d logged in.\n", accountID);
            loggedIn = 1;
            break;
        }
    }
    close(dbFile);
    if (!loggedIn) {
        // login failed
        sem_post(sessionSemaphore);
        sem_close(sessionSemaphore);
        sem_unlink(sessionSemName); 
        return 0; 
    }

    return 1; 
}

// deposit
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

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter the amount to deposit: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer) - 1) <= 0) {
        printf("Client disconnected during deposit amount entry.\n");
        lock.l_type = F_UNLCK; 
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;

    depositAmount = atof(inBuffer);
    if(depositAmount <= 0) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Invalid deposit amount.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3);

        lock.l_type = F_UNLCK; 
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    }

    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("Deposit: Failed to re-read record after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error reading account data.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    account.currentBalance += depositAmount;

    // logging
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644); 
    if (logFile == -1) {
        perror("Deposit: Error opening log file");
        printf("CRITICAL: Deposit to %d occurred but logging failed!\n", accountID);
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account)); 

        lock.l_type = F_UNLCK; 
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Deposit successful BUT LOGGING FAILED! New Balance: %.2f^", account.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); 
        return;
    }
    
    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; //lock whole file for append safety
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

    logLock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &logLock);
    close(logFile);

    // udpate account file
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Account %d deposited %.2f. New balance: %.2f\n", accountID, depositAmount, account.currentBalance);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Deposit successful! New Balance: %.2f^", account.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); 
}

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
    float balance = -1.0; 

    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; 
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
    read(clientSocket, inBuffer, 3); 
}

//  withdraw money
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
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;

    withdrawAmount = atof(inBuffer);

    lseek(dbFile, offset, SEEK_SET);
     if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("Withdraw: Failed to re-read record after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error reading account data.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    // fund check
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

    // process
    account.currentBalance -= withdrawAmount;

     // -logging
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (logFile == -1) {
        perror("Withdraw: Error opening log file");
        printf("CRITICAL: Withdraw from %d occurred but logging failed!\n", accountID);
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account));
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Withdrawal successful BUT LOGGING FAILED! Balance: %.2f^", account.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return;
    }

    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; 
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

    // account update
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Account %d withdrew %.2f. New balance: %.2f\n", accountID, withdrawAmount, account.currentBalance);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Withdrawal successful! New Balance: %.2f^", account.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); // ack
}

//  apply loan
void requestLoan(int clientSocket, int accountID){
    struct IDGenerator idGen;
    struct LoanRecord loan;

    int counterFile = open(LOAN_COUNTER_DB, O_RDWR | O_CREAT, 0644);
    if(counterFile == -1) {
        perror("Loan Request: Failed to open counter DB");
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing loan request (counter fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
     }

    // lock counter file
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
    idGen.nextID = newLoanID + 1; 
    lseek(counterFile, 0, SEEK_SET);
    write(counterFile, &idGen, sizeof(idGen));

    idLock.l_type = F_UNLCK;
    fcntl(counterFile, F_SETLK, &idLock);
    close(counterFile);

    //loan amount from client
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

    int loanFile = open(LOAN_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(loanFile == -1) {
        perror("Loan Request: Failed to open loan DB");
         bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Error processing loan request (db fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }
    
    struct flock loanDBLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; 
    fcntl(loanFile, F_SETLKW, &loanDBLock);

    loan.assignedEmployeeID = -1;
    loan.accountID = accountID;
    loan.amount = loanAmount;
    loan.loanStatus = 0; // requestd
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

// send money
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
        currentPos = lseek(dbFile, 0, SEEK_CUR);
        int bytesRead = read(dbFile, &tempAccount, sizeof(tempAccount));
        if (bytesRead <= 0) break; 

        if(tempAccount.accountID == sourceAccountID) {
            srcOffset = currentPos;
        }
        else if(tempAccount.accountID == destAccountID) {
            dstOffset = currentPos;
        }
        if(srcOffset != -1 && dstOffset != -1) break;
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
    
    struct flock lock1 = {F_WRLCK, SEEK_SET, 0, sizeof(struct AccountHolder), getpid()};
    struct flock lock2 = {F_WRLCK, SEEK_SET, 0, sizeof(struct AccountHolder), getpid()};

    // lock to smaller offest
    if (srcOffset < dstOffset) {
        lock1.l_start = srcOffset;
        lock2.l_start = dstOffset;
    } else {
        lock1.l_start = dstOffset;
        lock2.l_start = srcOffset;
    }

    if (fcntl(dbFile, F_SETLKW, &lock1) == -1) {
        perror("Transfer: Fcntl lock1 failed"); close(dbFile); return;
    }
    if (fcntl(dbFile, F_SETLKW, &lock2) == -1) {
        perror("Transfer: Fcntl lock2 failed");
        lock1.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock1); close(dbFile); return;
    }
    
    //read 
    lseek(dbFile, srcOffset, SEEK_SET);
     if (read(dbFile, &sourceAccount, sizeof(sourceAccount)) != sizeof(sourceAccount)) {
         perror("Transfer: Failed read source after lock"); goto unlock_close;
     }
    lseek(dbFile, dstOffset, SEEK_SET);
     if (read(dbFile, &destAccount, sizeof(destAccount)) != sizeof(destAccount)) {
         perror("Transfer: Failed read dest after lock"); goto unlock_close;
     }

    // check funds
    if (sourceAccount.currentBalance < transferAmount) {
        printf("Transfer: Insufficient funds (%.2f < %.2f).\n", sourceAccount.currentBalance, transferAmount);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Insufficient funds.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto unlock_close;
    }

    // transfer
    sourceAccount.currentBalance -= transferAmount;
    destAccount.currentBalance += transferAmount;

    // logging
    int logFile = open(HISTORY_DB, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(logFile == -1) {
        perror("Transfer: Error opening log file");
        printf("CRITICAL: Transfer between %d and %d occurred but logging failed!\n", sourceAccountID, destAccountID);
        lseek(dbFile, srcOffset, SEEK_SET); write(dbFile, &sourceAccount, sizeof(sourceAccount));
        lseek(dbFile, dstOffset, SEEK_SET); write(dbFile, &destAccount, sizeof(destAccount));
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Transfer successful BUT LOGGING FAILED! New Balance: %.2f^", sourceAccount.currentBalance);
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        goto unlock_close; 
    }

    // Lock the log file
    struct flock logLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
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

    logLock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &logLock);
    close(logFile);

    // up;date account
    lseek(dbFile, srcOffset, SEEK_SET);
    write(dbFile, &sourceAccount, sizeof(sourceAccount));
    lseek(dbFile, dstOffset, SEEK_SET);
    write(dbFile, &destAccount, sizeof(destAccount));

    printf("Transfer %.2f from %d to %d successful.\n", transferAmount, sourceAccountID, destAccountID);

    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "Transfer successful! New Balance: %.2f^", sourceAccount.currentBalance);
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);

unlock_close: // cleanup dbfile / can be also used for closing 
    lock1.l_type = F_UNLCK;
    lock2.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock1);
    fcntl(dbFile, F_SETLK, &lock2);
    close(dbFile);
}

// transactions log - will show last 10 
void viewTransactionLogs(int clientSocket, int accountID){
    struct TransactionLog log;
    int maxLogs = 10;
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

    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
    fcntl(logFile, F_SETLKW, &lock);

    fileSize = lseek(logFile, 0, SEEK_END);
    logCount = fileSize / sizeof(struct TransactionLog);

    bzero(outBuffer, sizeof(outBuffer)); 
    int foundCount = 0;

    //starting position for reading backwards
    readPos = (logCount > maxLogs) ? fileSize - (maxLogs * sizeof(struct TransactionLog)) : 0;
    lseek(logFile, readPos, SEEK_SET);

    while(foundCount < maxLogs && read(logFile, &log, sizeof(log)) == sizeof(log))
    {
        if(log.accountID == accountID)
        {
            //check to prevent buffer overflow
            if (strlen(outBuffer) + strlen(log.logEntry) + 1 < sizeof(outBuffer)) {
                 strcat(outBuffer, log.logEntry);
                 foundCount++;
            } else {
                 // buffer full
                 strcat(outBuffer, "... (more entries truncated)\n");
                 break;
            }
        }
    }

    lock.l_type = F_UNLCK;
    fcntl(logFile, F_SETLK, &lock);
    close(logFile);

    if(foundCount == 0) {
        strcpy(outBuffer, "No transactions found.\n");
    }

    strcat(outBuffer, "^");
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); 
}


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

    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()}; 
    fcntl(feedbackFile, F_SETLKW, &lock);

    int choice;
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Feedback:\n1. Good\n2. Average\n3. Poor\nChoice: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <=0){
        printf("Client disconnected during feedback.\n"); goto feedback_unlock_close;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    choice = atoi(inBuffer);

    bzero(feedback.message, sizeof(feedback.message));
    if(choice == 1) strncpy(feedback.message, "Good", sizeof(feedback.message)-1);
    else if(choice == 2) strncpy(feedback.message, "Average", sizeof(feedback.message)-1);
    else if(choice == 3) strncpy(feedback.message, "Poor", sizeof(feedback.message)-1);
    else strncpy(feedback.message, "Unknown Choice", sizeof(feedback.message)-1);
    feedback.message[sizeof(feedback.message)-1] = '\0'; 

    write(feedbackFile, &feedback, sizeof(feedback));

feedback_unlock_close:
    lock.l_type = F_UNLCK;
    fcntl(feedbackFile, F_SETLK, &lock);
    close(feedbackFile);

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Thank you for your feedback!^");
    write(clientSocket, outBuffer, strlen(outBuffer));
    read(clientSocket, inBuffer, 3); 
}

// change password
int updateCustomerPassword(int clientSocket, int accountID){
    char newPassword[50];
    struct AccountHolder account;

    int dbFile = open(ACCOUNT_DB, O_RDWR);
    if(dbFile == -1) {
        perror("ChangePass: Error opening DB");
        return 0;
     }

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

    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
         perror("ChangePass: Failed to lock record");
         close(dbFile); return 0;
    }

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter new password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during password change entry.\n");
        lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
        return 0;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(newPassword, inBuffer, sizeof(newPassword) - 1);
    newPassword[sizeof(newPassword) - 1] = '\0';

    // Re-read data before writing
    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
         perror("ChangePass: Failed re-read after lock");
         lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock); close(dbFile);
         return 0;
    }

    // set pass
    strncpy(account.password, newPassword, sizeof(account.password) - 1);
    account.password[sizeof(account.password) - 1] = '\0';
    lseek(dbFile, offset, SEEK_SET);
    write(dbFile, &account, sizeof(account));

    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Customer %d changed password\n", accountID);
    return 1; 
}


// customer session
void handleCustomerSession(int clientSocket){
    struct AccountHolder account;
    int authAccountID = -1, destAccountID; 
    int choice;
    char password[50]; 

label_customer_login:
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "\nEnter account number: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected before login.\n"); return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    authAccountID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer,  "Enter password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
         printf("Client disconnected before login password.\n"); return;
     }
     
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    strncpy(password, inBuffer, sizeof(password) - 1); 
    password[sizeof(password)-1] = '\0';

    if (authenticateCustomer(clientSocket, authAccountID, password))
    {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nLogin Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); 

        while(1)
        {
            bzero(outBuffer, sizeof(outBuffer));
            strcpy(outBuffer, CUSTOMER_PROMPT);
            write(clientSocket, outBuffer, strlen(outBuffer));
            bzero(inBuffer, sizeof(inBuffer));
            if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                printf("Client %d disconnected during session.\n", authAccountID);
                terminateClientSession(clientSocket, authAccountID); 
                return;
            }
            inBuffer[strcspn(inBuffer, "\r\n")] = 0;
            choice = atoi(inBuffer);
            printf("Customer %d choice: %d\n", authAccountID, choice);

            switch(choice)
            {
                case 1: 
                    processDeposit(clientSocket, authAccountID);
                    break;
                case 2:
                    processWithdrawal(clientSocket, authAccountID);
                    break;
                case 3: 
                    checkBalance(clientSocket, authAccountID);;
                    break;
                case 4: 
                    requestLoan(clientSocket, authAccountID);
                    break;
                case 5: 
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Enter destination account number: ");
                    write(clientSocket, outBuffer, strlen(outBuffer));
                    bzero(inBuffer, sizeof(inBuffer));
                    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) goto disconnect_cleanup;
                    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
                    destAccountID = atoi(inBuffer);

                    float amount;
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Enter amount: ");
                    write(clientSocket, outBuffer, strlen(outBuffer));
                    bzero(inBuffer, sizeof(inBuffer));
                     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) goto disconnect_cleanup;
                     inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
                    amount = atof(inBuffer);

                    executeTransfer(clientSocket, authAccountID, destAccountID, amount);
                    break;
                case 6: 
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
                    authAccountID = -1; 
                    goto label_customer_login; //relogin
                case 7: 
                    viewTransactionLogs(clientSocket, authAccountID);
                    break;
                case 8: 
                    submitFeedback(clientSocket);
                    break;
                case 9: 
                    printf("%d logged out.\n", authAccountID);
                    endUserSession(clientSocket, authAccountID);
                     authAccountID = -1; 
                    return; 
                case 10: // exit
                    printf("Customer: %d Exited!\n", authAccountID);
                    terminateClientSession(clientSocket, authAccountID);
                     authAccountID = -1;
                    return; 
                default:
                    bzero(outBuffer, sizeof(outBuffer));
                    strcpy(outBuffer, "Invalid Choice^");
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
            }
        }
    }
    else 
    {
        // login failed
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nInvalid ID, Password, or Inactive Account^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         authAccountID = -1; // reset auth 
        goto label_customer_login;
    }

disconnect_cleanup: // disconnect cleanup
    printf("Client %d disconnected unexpectedly.\n", authAccountID);
    if (authAccountID > 0) { // terminate if logged in
         terminateClientSession(clientSocket, authAccountID);
    }
    return;
}
