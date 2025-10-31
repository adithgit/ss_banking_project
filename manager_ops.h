#ifndef MANAGER_OPS_H
#define MANAGER_OPS_H

#include <string.h>
#include <stdio.h>  
#include <stdlib.h> 
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h> 

int authenticateManager(int clientSocket, int managerID, char *password_input);
void setAccountActiveStatus(int clientSocket);
void reviewClientFeedback(int clientSocket);
void assignLoanToEmployee(int clientSocket);
int updateManagerPassword(int clientSocket, int managerID);
void handleManagerSession(int clientSocket);

int authenticateManager(int clientSocket, int managerID, char *password_input)
{
    struct Employee manager;
    int dbFile = open(EMPLOYEE_DB, O_RDONLY);
    if (dbFile == -1) {
         if (errno == ENOENT) { // create fiel is not there
             dbFile = open(EMPLOYEE_DB, O_WRONLY | O_CREAT, 0644);
             if (dbFile != -1) close(dbFile);
             printf("Created empty employee database file: %s\n", EMPLOYEE_DB);
         } else {
            perror("AuthMgr: Error opening employee DB for read");
            return 0; //auth failed
         }
    } else {
        close(dbFile); 
    }

    
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

    //read and check credentials
     dbFile = open(EMPLOYEE_DB, O_RDONLY);
     if (dbFile == -1) {
          perror("AuthMgr: Error opening employee DB for login check");
          sem_post(sessionSemaphore); sem_close(sessionSemaphore); sem_unlink(sessionSemName);
          return 0;
     }
     
    int loggedIn = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &manager, sizeof(manager)) == sizeof(manager))
    {
        if (manager.employeeID == managerID && strcmp(manager.password, password_input) == 0 && manager.roleType == 0) { // 0 = Manager
           loggedIn = 1;
           break;
        }
    }
    close(dbFile);

     if (!loggedIn) {
        // relesae lock auth failed
        sem_post(sessionSemaphore);
        sem_close(sessionSemaphore);
        sem_unlink(sessionSemName);
        return 0; 
    }
    printf("Manager %d logged in.\n", managerID);
    return 1;
}

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
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    accountID = atoi(inBuffer);
    
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
    
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
    if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("SetStatus: Lock failed"); close(dbFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

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
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
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
        //write updates
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account));
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Status Changed Successfully^");
    } else {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid choice or status already set.^");
    }


    lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    return;

setstatus_unlock_fail: //cleanup on disconnect
    lock.l_type = F_UNLCK; fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return; 
}

void reviewClientFeedback(int clientSocket)
{
    struct ClientFeedback feedback;
    int feedbackFile = open(FEEDBACK_DB, O_RDONLY | O_CREAT, 0644);
    if(feedbackFile == -1) {
        perror("ReviewFeedback: Error opening file");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error retrieving feedback.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    struct flock lock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; 
    fcntl(feedbackFile, F_SETLKW, &lock);

    bzero(outBuffer, sizeof(outBuffer));
    int feedbackCount = 0;
    while(read(feedbackFile, &feedback, sizeof(feedback)) == sizeof(feedback))
    {
        
        if (strlen(outBuffer) + strlen(feedback.message) + 2 < sizeof(outBuffer)) {
            strcat(outBuffer, feedback.message);
            strcat(outBuffer, "\n");
            feedbackCount++;
        } else {
            strcat(outBuffer, "... (more feedback truncated)\n");
            break; 
        }
    }

    lock.l_type = F_UNLCK; fcntl(feedbackFile, F_SETLK, &lock);
    close(feedbackFile);

    if(feedbackCount == 0) {
        strcpy(outBuffer, "No feedback found.\n");
    }

    strcat(outBuffer, "^"); 
    printf("Manager reading %d feedback entries.\n", feedbackCount);
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
}

void assignLoanToEmployee(int clientSocket)
{
    struct LoanRecord loan;
    int loanFile = open(LOAN_DB, O_RDWR);
    if(loanFile == -1) {
         perror("AssignLoan: Error opening loan DB");
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    // -unassigned loans
    struct flock readLock = {F_RDLCK, SEEK_SET, 0, 0, getpid()}; 
    fcntl(loanFile, F_SETLKW, &readLock);

    int unassignedFound = 0;
    lseek(loanFile, 0, SEEK_SET);
    while(read(loanFile, &loan, sizeof(loan)) == sizeof(loan))
    {
        if(loan.assignedEmployeeID == -1 && loan.loanStatus == 0)
        {
            bzero(outBuffer, sizeof(outBuffer));
            sprintf(outBuffer, "-> Unassigned Loan ID: %d | Account: %d | Amount: %d^",
                    loan.loanRecordID, loan.accountID, loan.amount);
            write(clientSocket, outBuffer, strlen(outBuffer));
            read(clientSocket, inBuffer, 3); // ack for each
            unassignedFound = 1;
        }
    }

    readLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &readLock);

    if(!unassignedFound) {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "No unassigned loans found.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(loanFile);
        return;
    }

    
    int loanID, employeeID;
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Loan ID to assign: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(loanFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    loanID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter Employee ID to assign to: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(loanFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    employeeID = atoi(inBuffer);

    // update the loan
    int offset = -1;
    off_t currentPos = 0;
    lseek(loanFile, 0, SEEK_SET);
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
    
    struct flock writeLock = {F_WRLCK, SEEK_SET, offset, sizeof(struct LoanRecord), getpid()};
    if(fcntl(loanFile, F_SETLKW, &writeLock) == -1) {
         perror("AssignLoan: Lock failed"); close(loanFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    lseek(loanFile, offset, SEEK_SET);
    if (read(loanFile, &loan, sizeof(loan)) != sizeof(loan)) {
        perror("AssignLoan: Re-read failed"); goto assignloan_unlock_fail;
    }

    if(loan.assignedEmployeeID != -1 || loan.loanStatus != 0) {
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan %d was already assigned or processed.^", loanID);
    } else {
        loan.assignedEmployeeID = employeeID;
        loan.loanStatus = 1; // 1 = Pending (assigned)

        lseek(loanFile, offset, SEEK_SET);
        write(loanFile, &loan, sizeof(loan));

        printf("Manager assigned loan %d to employee %d\n", loanID, employeeID);
        bzero(outBuffer, sizeof(outBuffer));
        sprintf(outBuffer, "Loan %d assigned to employee %d.^", loanID, employeeID);
    }

    writeLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &writeLock);
    close(loanFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    return; 

assignloan_unlock_fail: // cleanup
    writeLock.l_type = F_UNLCK; fcntl(loanFile, F_SETLK, &writeLock);
    close(loanFile);
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error during assignment.^");
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    return;
}

int updateManagerPassword(int clientSocket, int managerID)
{
    return updateEmployeePassword(clientSocket, managerID);
}

void handleManagerSession(int clientSocket)
{
    int authManagerID = -1; 
    char password[51]; 

label_manager_login:
    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "\nEnter Manager ID: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    authManagerID = atoi(inBuffer);

    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Enter password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) return;
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(password, inBuffer, sizeof(password) - 1); password[sizeof(password)-1] = '\0';

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
             inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
            int choice = atoi(inBuffer);
            printf("Manager %d choice: %d\n", authManagerID, choice);

            switch(choice)
            {
                case 1: 
                    setAccountActiveStatus(clientSocket);
                    break;
                case 2:
                    assignLoanToEmployee(clientSocket);
                    break;
                case 3:
                    reviewClientFeedback(clientSocket); 
                    break;
                case 4: 
                    if(updateManagerPassword(clientSocket, authManagerID)) {
                        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password changed. Please log in again.^");
                    } else {
                         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer,"Password change failed.^");
                    }
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                    endUserSession(clientSocket, authManagerID);
                    authManagerID = -1;
                    goto label_manager_login; 
                case 5: 
                    printf("Manager %d Logged Out!\n", authManagerID);
                    endUserSession(clientSocket, authManagerID); 
                    authManagerID = -1;
                    return; 
                case 6: 
                    printf("Manager %d Exited!\n", authManagerID);
                    terminateClientSession(clientSocket, authManagerID); 
                     authManagerID = -1;
                    return; 
                default:
                    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid Choice^");
                    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
            }
        }
    }
    else 
    {
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "\nInvalid ID or Password^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        authManagerID = -1;
        goto label_manager_login;
    }
}


#endif
