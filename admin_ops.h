#ifndef ADMIN_OPS_H
#define ADMIN_OPS_H
#include <string.h> 
#include <stdio.h>  
#include <stdlib.h> 
#include <unistd.h> 
#include <fcntl.h>  
#include <sys/types.h> 


#define ADMIN_USER "admin"
#define ADMIN_PASS_DB "admin_pass.dat"
#define DEFAULT_ADMIN_PASS "root123" // if file admin_pass file is missing

// ---  Prototypes ---
void changeAdminPassword(int clientSocket);
int createNewEmployee(int clientSocket);
void modifyUser(int clientSocket, int modifyType);
void updateEmployeeRole(int clientSocket);
void handleAdminSession(int clientSocket); 

// --- Function Definitions ---

int createNewEmployee(int clientSocket)
{
    struct Employee employee, tempEmployee; 
    
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Employee ID: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during employee ID entry.\n"); return 0;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    employee.employeeID = atoi(inBuffer);

    // -duplicate employee check 
    int dbFile = open(EMPLOYEE_DB, O_RDWR | O_CREAT, 0644);
    if(dbFile == -1) {
        perror("CreateEmployee: Error opening Employee DB");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return 0; 
    }

    // xclusive lock file 
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    if (fcntl(dbFile, F_SETLKW, &lock) == -1) {
        perror("CreateEmployee: Failed to lock Employee DB");
        close(dbFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return 0; // Indicate failure
    }

    int duplicateFound = 0;
    lseek(dbFile, 0, SEEK_SET); // Go to start
    while(read(dbFile, &tempEmployee, sizeof(tempEmployee)) == sizeof(tempEmployee)) { 
        if (tempEmployee.employeeID == employee.employeeID) {
            duplicateFound = 1;
            break;
        }
    }

    if (duplicateFound) {
        //release lock
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Employee ID already exists. Please try again.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return 0;
    }
    // duplicate check end

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter FirstName: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during first name entry.\n"); goto createemployee_unlock_fail;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(employee.firstName, inBuffer, sizeof(employee.firstName) - 1);
     employee.firstName[sizeof(employee.firstName) - 1] = '\0';

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter LastName: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during last name entry.\n"); goto createemployee_unlock_fail;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(employee.lastName, inBuffer, sizeof(employee.lastName) - 1);
     employee.lastName[sizeof(employee.lastName) - 1] = '\0';

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during password entry.\n"); goto createemployee_unlock_fail;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(employee.password, inBuffer, sizeof(employee.password) - 1);
    employee.password[sizeof(employee.password) - 1] = '\0';
    employee.roleType = 1; // default 1 -> employee

    lseek(dbFile, 0, SEEK_END);
    write(dbFile, &employee, sizeof(employee));
    // release lock
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Admin added employee ID: %d\n", employee.employeeID);
    return 1; 

createemployee_unlock_fail: // unlock file if client disconnects midway
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return 0;
}

void modifyUser(int clientSocket, int modifyType)
{
    if(modifyType == 1) // customer
    {
        int dbFile = open(ACCOUNT_DB, O_RDWR);
        if(dbFile == -1) {
             perror("Modify customer: error opening DB");
              bzero(outBuffer, sizeof(outBuffer));
             strcpy(outBuffer, "DB error.^");
             write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
             return;
         }

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Enter Account Number: ");
        write(clientSocket, outBuffer, strlen(outBuffer));

        int accountID;
        bzero(inBuffer, sizeof(inBuffer));
        if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(dbFile); return; }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
        accountID = atoi(inBuffer);

        struct AccountHolder account;
        int offset = -1;
        off_t currentPos = 0;
        lseek(dbFile, 0, SEEK_SET);
        while (read(dbFile, &account, sizeof(account)) > 0)
        {
            currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account);
            if(account.accountID == accountID) {
                offset = currentPos;// where account info is in file
                break;
            }
        }

        if(offset == -1) {
            bzero(outBuffer, sizeof(outBuffer));
            strcpy(outBuffer, "Account not found.^");
            write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
            close(dbFile);
             return;
        }

        struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct AccountHolder), getpid()};
        if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
             perror("cust modifu: Lock failed"); close(dbFile);
             bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
             write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
             return;
        }

        char newName[50];
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Enter New Name: ");
        write(clientSocket, outBuffer, strlen(outBuffer));
        bzero(inBuffer, sizeof(inBuffer));
        if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
             printf("Client disconnected during name entry.\n"); goto modifycust_unlock_fail;
        }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
        strncpy(newName, inBuffer, sizeof(newName) - 1);
        newName[sizeof(newName)-1] = '\0';

        // Re-read before write
        lseek(dbFile, offset, SEEK_SET);
         if (read(dbFile, &account, sizeof(account)) != sizeof(account)) {
            perror("Modify Cust: Re-read failed"); goto modifycust_unlock_fail;
         }

        strncpy(account.holderName, newName, sizeof(account.holderName) -1);
        account.holderName[sizeof(account.holderName) -1] = '\0';


        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &account, sizeof(account));

        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        printf("Admin/employee modified name for account %d\n", accountID);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Customer name updated.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return; 

    modifycust_unlock_fail: // cleanup on error
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return; 
    }
    else if(modifyType == 2) // emp
    {
        struct Employee employee;
        int dbFile = open(EMPLOYEE_DB, O_RDWR);
        if(dbFile == -1) {
            perror("Modify employee: Error opening DB");
            bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
            write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
             return;
        }

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Enter Employee ID: ");
        write(clientSocket, outBuffer, strlen(outBuffer));

        int employeeID;
        bzero(inBuffer, sizeof(inBuffer));
         if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(dbFile); return; }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
        employeeID = atoi(inBuffer);

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
             bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "employee ID not found.^");
             write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
             close(dbFile);
             return;
         }

        struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct Employee), getpid()};
        if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
            perror("Modify employee: locking failed"); close(dbFile);
             bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
            write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
            return;
        }

        char newFirstName[50];
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Enter New First Name: ");
        write(clientSocket, outBuffer, strlen(outBuffer));
        bzero(inBuffer, sizeof(inBuffer));
         if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
             printf("Client disconnected during employee name entry.\n"); goto modifyemployee_unlock_fail;
         }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
        strncpy(newFirstName, inBuffer, sizeof(newFirstName) - 1);
        newFirstName[sizeof(newFirstName)-1] = '\0';

        lseek(dbFile, offset, SEEK_SET);
         if (read(dbFile, &employee, sizeof(employee)) != sizeof(employee)) {
             perror("Modify employee: Re-read failed"); goto modifyemployee_unlock_fail;
         }

        strncpy(employee.firstName, newFirstName, sizeof(employee.firstName) - 1);
         employee.firstName[sizeof(employee.firstName) - 1] = '\0';


        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &employee, sizeof(employee));

        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        printf("Admin modified name for employee %d\n", employeeID);
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "employee name updated.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return; 

    modifyemployee_unlock_fail: // cleanup error
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return;
    } else {
        // invalid option
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid modification type.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    }
}

void updateEmployeeRole(int clientSocket)
{
    int dbFile = open(EMPLOYEE_DB, O_RDWR); 
    if(dbFile == -1) {
        perror("UpdateRole: Error opening employee DB");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter employee ID to change role: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    int employeeID;
    bzero(inBuffer, sizeof(inBuffer));
     if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(dbFile); return; }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    employeeID = atoi(inBuffer);

    struct Employee employee;
    int offset = -1;
    off_t currentPos = 0;
    lseek(dbFile, 0, SEEK_SET);
    while(read(dbFile, &employee, sizeof(employee)) > 0)
    {
        currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(employee);
        if(employee.employeeID == employeeID) {
            offset = currentPos;
            break;
        }
    }

    if(offset == -1) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Invalid employee ID^");
        write(clientSocket, outBuffer, strlen(outBuffer)); 
        read(clientSocket, inBuffer, 3);
        close(dbFile);
        return;
    }

    // locking
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct Employee), getpid()};
    if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
         perror("UpdateRole: Lock failed"); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }

    //reread 
    lseek(dbFile, offset, SEEK_SET);
    if (read(dbFile, &employee, sizeof(employee)) != sizeof(employee)) {
        perror("UpdateRole: Re-read failed"); goto updaterole_unlock_fail;
    }

    int choice;
    bzero(outBuffer, sizeof(outBuffer));
    sprintf(outBuffer, "employee %d is currently %s.\n[0] Make Manager\n[1] Make Employee\nChoice: ",
            employeeID, (employee.roleType == 0) ? "Manager" : "Employee");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Cleint disconnected during role choice.\n"); goto updaterole_unlock_fail;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; 
    choice = atoi(inBuffer);

    int roleChanged = 0;
    if(choice == 0 && employee.roleType != 0) {
        employee.roleType = 0; // manager
        roleChanged = 1;
    } else if (choice == 1 && employee.roleType != 1) {
        employee.roleType = 1; // emp
        roleChanged = 1;
    }

    if (roleChanged) { //success
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &employee, sizeof(employee));
        printf("Admin changed role for employee %d to %s\n", employeeID, (employee.roleType == 0 ? "Manager" : "Employee"));
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Role updated.^");
    } else { //failed
         bzero(outBuffer, sizeof(outBuffer));
         strcpy(outBuffer, "Invalid choice or role already set.^");
    }


    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    return; 

updaterole_unlock_fail: // cleanup on error
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return; 
}

void changeAdminPassword(int clientSocket)
{
    char newPassword[50];
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter new admin password: ");
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during admin password entry.\n"); return;
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(newPassword, inBuffer, sizeof(newPassword) - 1);
    newPassword[sizeof(newPassword) - 1] = '\0';

    int passFile = open(ADMIN_PASS_DB, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (passFile == -1) {
        perror("Admin change pass: File write error");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error changing password (file write).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    if (fcntl(passFile, F_SETLKW, &lock) == -1) {
        perror("Admin change pass: Lock error");
        close(passFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error changing password (lock fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    size_t len_to_write = strlen(newPassword);
    if (len_to_write > sizeof(newPassword) - 1) len_to_write = sizeof(newPassword) -1; // bug fix - don't write anything more 
    write(passFile, newPassword, len_to_write + 1); // +1 for '\0'

    lock.l_type = F_UNLCK;
    fcntl(passFile, F_SETLK, &lock);
    close(passFile);

    printf("Admin password changed.\n");
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Admin password changed successfully.^");
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
}

// admin menu handler
void handleAdminSession(int clientSocket)
{
    char password_input[51];
label_admin_login:
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter admin password: "); 
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during admin login.\n"); return;
    }
    // sanitize
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(password_input, inBuffer, sizeof(password_input) - 1);
    password_input[sizeof(password_input)-1] = '\0';

    char storedPassword[51];
    int passFile = open(ADMIN_PASS_DB, O_RDWR | O_CREAT, 0644);
    int loggedIn = 0; 
    int fileNeedsInit = 0;

    if (passFile == -1) {
        perror("Admin login: Password file open error");
    } else {
        struct flock passLock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
        if (fcntl(passFile, F_SETLKW, &passLock) == -1) {
            perror("Admin login: Read lock failed");
            close(passFile);
            // todo handle error
        } else {
            bzero(storedPassword, sizeof(storedPassword));
            int bytesRead = read(passFile, storedPassword, sizeof(storedPassword) -1);

            if (bytesRead <= 0) {
                fileNeedsInit = 1;
            } else {
                storedPassword[bytesRead] = '\0'; 
            }

            // unlock lock
            passLock.l_type = F_UNLCK;
            fcntl(passFile, F_SETLK, &passLock);

            // if file needs initialization re-open and write default password
            if (fileNeedsInit) {
                 close(passFile); // close we opened earlier to reopen with write acecss
                 passFile = open(ADMIN_PASS_DB, O_WRONLY | O_TRUNC | O_CREAT, 0644); 
                 if (passFile != -1) {
                     struct flock writeLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
                     if (fcntl(passFile, F_SETLKW, &writeLock) != -1) {
                         strcpy(storedPassword, DEFAULT_ADMIN_PASS);
                         write(passFile, storedPassword, strlen(storedPassword) + 1);
                         writeLock.l_type = F_UNLCK;
                         fcntl(passFile, F_SETLK, &writeLock);
                         printf("Admin password file initialized.\n");
                     } else {
                          perror("Admin login: Init write lock failed");
                          strcpy(storedPassword,""); 
                     }
                      close(passFile);
                 } else {
                     perror("Admin login: Init write open failed");
                     strcpy(storedPassword,""); // Prevent login
                 }
            }
             // check password 
            if(strcmp(ADMIN_USER, "admin") == 0 && strcmp(storedPassword, password_input) == 0) {
                loggedIn = 1;
            }
        }
         if (!fileNeedsInit && passFile != -1) close(passFile);
    } 


    if(loggedIn) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nAdmin Login Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    }
    else{
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nInvalid credential^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
        goto label_admin_login;
    }

    //logged in -> admin menu loop
    while(1)
    {
        int modifyType, choice;

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, ADMIN_PROMPT);
        write(clientSocket, outBuffer, strlen(outBuffer));

        bzero(inBuffer, sizeof(inBuffer));
        if (read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
            printf("Admin client disconnected.\n"); return; // if client disconnects
        }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0;
        choice = atoi(inBuffer);
        printf("Admin choice: %d\n", choice);

        switch (choice) {
            case 1: // add emp
                if(createNewEmployee(clientSocket)) {
                    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Employee added successfully^");
                } else {
                    strcpy(outBuffer, "^"); // empty ack
                }
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                break;
            case 2: // modify emp
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "[1] Modify Customer\n[2] Modify Employee\nChoice: ");
                write(clientSocket, outBuffer, strlen(outBuffer));

                bzero(inBuffer, sizeof(inBuffer));
                 if (read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                     printf("Admin client disconnected.\n"); return;
                 }
                 inBuffer[strcspn(inBuffer, "\r\n")] = 0;
                modifyType = atoi(inBuffer);

                modifyUser(clientSocket, modifyType); 
                break;
            case 3:
                updateEmployeeRole(clientSocket); 
                break;
            case 4: 
                changeAdminPassword(clientSocket);
                break;
            case 5: 
                printf("Admin logged out.\n");
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "Logging out...^");
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
                return; // return to server loop
            default:
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "Invalid choice!^");
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); 
        }
    }
}

#endif 
