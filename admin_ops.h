#ifndef ADMIN_OPS_H
#define ADMIN_OPS_H

#include <string.h> // For strcspn, strncpy, strcmp, strlen
#include <stdio.h>  // For perror, printf
#include <stdlib.h> // For atoi
#include <unistd.h> // For read, write, lseek, close, getpid
#include <fcntl.h>  // For file constants (O_RDWR etc) and fcntl
#include <sys/types.h> // For off_t, pid_t

// Include structs (assuming bank_records.h is included *before* this in bank_server.c)
// #include "bank_records.h" // Not needed here if included before in server.c

// Define admin user and password file (relative to server executable)
#define ADMIN_USER "admin"
#define ADMIN_PASS_DB "admin_pass.dat"
#define DEFAULT_ADMIN_PASS "root123" // Default password if file is missing/empty

// --- Function Prototypes ---
void changeAdminPassword(int clientSocket);
int createNewEmployee(int clientSocket);
void modifyUser(int clientSocket, int modifyType);
void updateEmployeeRole(int clientSocket);
void handleAdminSession(int clientSocket); // Main handler prototype

// --- Function Definitions ---

// Function to add a new Employee member (Employee or Manager by default role)
int createNewEmployee(int clientSocket)
{
    struct Employee employee, tempEmployee; // Define structs needed

    // Get Employee ID
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter Employee ID: ");
    write(clientSocket, outBuffer, strlen(outBuffer));
    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during employee ID entry.\n"); return 0; // Indicate failure
    }
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    employee.employeeID = atoi(inBuffer);

    // --- Check for duplicate Employee ID ---
    int dbFile = open(EMPLOYEE_DB, O_RDWR | O_CREAT, 0644);
    if(dbFile == -1) {
        perror("CreateEmployee: Error opening Employee DB");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database error.^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return 0; // Indicate failure
    }

    // Acquire an EXCLUSIVE lock on the ENTIRE file
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
    while(read(dbFile, &tempEmployee, sizeof(tempEmployee)) == sizeof(tempEmployee)) { // Check read result
        if (tempEmployee.employeeID == employee.employeeID) {
            duplicateFound = 1;
            break;
        }
    }

    if (duplicateFound) {
        // Release the lock
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Employee ID already exists. Please try again.^");
        write(clientSocket, outBuffer, strlen(outBuffer));
        read(clientSocket, inBuffer, 3); // ack
        return 0; // Return failure
    }
    // --- End of duplicate check ---

    // Get FirstName
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


    // Get LastName
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

    // Get Password
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


    employee.roleType = 1; // Default to 1 (employee)

    // We still hold the lock, so we can safely append
    lseek(dbFile, 0, SEEK_END);
    write(dbFile, &employee, sizeof(employee));

    // Release the lock
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    printf("Admin added employee ID: %d\n", employee.employeeID);
    return 1; // Return success

createemployee_unlock_fail: // Label for cleanup on disconnect
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return 0; // Indicate failure
}

// Function to modify Customer name or employee first name
void modifyUser(int clientSocket, int modifyType)
{
    if(modifyType == 1) // Modify Customer
    {
        int dbFile = open(ACCOUNT_DB, O_RDWR);
        if(dbFile == -1) {
             perror("Modify Cust: Error opening DB");
              bzero(outBuffer, sizeof(outBuffer));
             strcpy(outBuffer, "Database error.^");
             write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
             return;
         }

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Enter Account Number: ");
        write(clientSocket, outBuffer, strlen(outBuffer));

        int accountID;
        bzero(inBuffer, sizeof(inBuffer));
        if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) { close(dbFile); return; }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
        accountID = atoi(inBuffer);

        struct AccountHolder account;
        int offset = -1;
        off_t currentPos = 0;
        lseek(dbFile, 0, SEEK_SET);
        while (read(dbFile, &account, sizeof(account)) > 0)
        {
            currentPos = lseek(dbFile, 0, SEEK_CUR) - sizeof(account);
            if(account.accountID == accountID) {
                offset = currentPos;
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
             perror("Modify Cust: Lock failed"); close(dbFile);
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
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
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
        return; // Success

    modifycust_unlock_fail: // Cleanup on error/disconnect
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return; // Indicate failure implicitly
    }
    else if(modifyType == 2) // Modify Employee
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
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
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
            perror("Modify employee: Lock failed"); close(dbFile);
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

        // Re-read before write
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
        return; // Success

    modifyemployee_unlock_fail: // Cleanup on error/disconnect
        lock.l_type = F_UNLCK;
        fcntl(dbFile, F_SETLK, &lock);
        close(dbFile);
        return; // Failure implicitly
    } else {
        // Invalid modifyType requested (shouldn't happen with current menus)
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Invalid modification type.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    }
}

// Function to change a employee member's role (Manager <-> Employee)
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
    inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
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
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        close(dbFile);
        return;
    }

    // Lock record
    struct flock lock = {F_WRLCK, SEEK_SET, offset, sizeof(struct Employee), getpid()};
    if(fcntl(dbFile, F_SETLKW, &lock) == -1) {
         perror("UpdateRole: Lock failed"); close(dbFile);
         bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Database lock error.^");
         write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
         return;
    }


    // Re-read data after lock
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
        printf("Client disconnected during role choice.\n"); goto updaterole_unlock_fail;
    }
     inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize
    choice = atoi(inBuffer);

    int roleChanged = 0;
    if(choice == 0 && employee.roleType != 0) {
        employee.roleType = 0; // Manager
        roleChanged = 1;
    } else if (choice == 1 && employee.roleType != 1) {
        employee.roleType = 1; // employee
        roleChanged = 1;
    }

    if (roleChanged) {
        lseek(dbFile, offset, SEEK_SET);
        write(dbFile, &employee, sizeof(employee));
        printf("Admin changed role for employee %d to %s\n", employeeID, (employee.roleType == 0 ? "Manager" : "Employee"));
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "Role updated.^");
    } else {
         bzero(outBuffer, sizeof(outBuffer));
         strcpy(outBuffer, "Invalid choice or role already set.^");
    }


    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);

    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
    return; // Success or invalid choice handled

updaterole_unlock_fail: // Cleanup on error/disconnect
    lock.l_type = F_UNLCK;
    fcntl(dbFile, F_SETLK, &lock);
    close(dbFile);
    return; // Failure implicitly
}

// Function to change the admin password stored in ADMIN_PASS_DB
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
    // Sanitize password input
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(newPassword, inBuffer, sizeof(newPassword) - 1);
    newPassword[sizeof(newPassword) - 1] = '\0';


    // Open file for writing, truncating existing content
    int passFile = open(ADMIN_PASS_DB, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (passFile == -1) {
        perror("Admin change pass: File write error");
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error changing password (file write).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Lock the entire file
    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
    if (fcntl(passFile, F_SETLKW, &lock) == -1) {
        perror("Admin change pass: Lock error");
        close(passFile);
        bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Error changing password (lock fail).^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
        return;
    }

    // Write the new password AND a null terminator
    // Ensure we don't write more than the buffer size allows (though unlikely here)
    size_t len_to_write = strlen(newPassword);
    if (len_to_write > sizeof(newPassword) - 1) len_to_write = sizeof(newPassword) -1; // Should not happen
    write(passFile, newPassword, len_to_write + 1); // +1 for '\0'


    // Unlock and close
    lock.l_type = F_UNLCK;
    fcntl(passFile, F_SETLK, &lock);
    close(passFile);

    printf("Admin password changed.\n");
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Admin password changed successfully.^");
    write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3);
}

// Main handler for the Admin menu and actions
void handleAdminSession(int clientSocket)
{
    char password_input[51]; // Renamed input variable, size 51
label_admin_login:
    bzero(outBuffer, sizeof(outBuffer));
    strcpy(outBuffer, "Enter admin password: "); // Use strcpy, not strcat
    write(clientSocket, outBuffer, strlen(outBuffer));

    bzero(inBuffer, sizeof(inBuffer));
    if(read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
        printf("Client disconnected during admin login.\n"); return;
    }
    // Sanitize password input for login
    inBuffer[strcspn(inBuffer, "\r\n")] = 0;
    strncpy(password_input, inBuffer, sizeof(password_input) - 1);
    password_input[sizeof(password_input)-1] = '\0';


    // Read stored password from file
    char storedPassword[51]; // Size 51 to read null terminator
    int passFile = open(ADMIN_PASS_DB, O_RDWR | O_CREAT, 0644); // RDWR needed for potential init
    int loggedIn = 0; // Flag for login status
    int fileNeedsInit = 0;

    if (passFile == -1) {
        perror("Admin login: Password file open error");
        // Cannot login without password file
    } else {
        // Lock file for reading first
        struct flock passLock = {F_RDLCK, SEEK_SET, 0, 0, getpid()};
        if (fcntl(passFile, F_SETLKW, &passLock) == -1) {
            perror("Admin login: Read lock failed");
            close(passFile);
            // Handle error - maybe deny login?
        } else {
            bzero(storedPassword, sizeof(storedPassword));
            // Read including potential null terminator
            int bytesRead = read(passFile, storedPassword, sizeof(storedPassword) -1);

            // Check if file was empty or read failed
            if (bytesRead <= 0) {
                fileNeedsInit = 1;
            } else {
                 // Ensure null termination even if file didn't have one (unlikely now)
                storedPassword[bytesRead] = '\0'; // Null term right after read data
            }

            // Unlock read lock
            passLock.l_type = F_UNLCK;
            fcntl(passFile, F_SETLK, &passLock);

            // If file needs initialization, re-open and write default password
            if (fileNeedsInit) {
                 close(passFile); // Close RDWR handle
                 passFile = open(ADMIN_PASS_DB, O_WRONLY | O_TRUNC | O_CREAT, 0644); // Open O_WRONLY
                 if (passFile != -1) {
                     struct flock writeLock = {F_WRLCK, SEEK_SET, 0, 0, getpid()};
                     if (fcntl(passFile, F_SETLKW, &writeLock) != -1) {
                         strcpy(storedPassword, DEFAULT_ADMIN_PASS);
                         // Write null terminator
                         write(passFile, storedPassword, strlen(storedPassword) + 1);
                         writeLock.l_type = F_UNLCK;
                         fcntl(passFile, F_SETLK, &writeLock);
                         printf("Admin password file initialized.\n");
                     } else {
                          perror("Admin login: Init write lock failed");
                          strcpy(storedPassword,""); // Prevent login if init fails
                     }
                      close(passFile);
                 } else {
                     perror("Admin login: Init write open failed");
                     strcpy(storedPassword,""); // Prevent login
                 }
            }
             // Compare entered password (sanitized) with stored password
            if(strcmp(ADMIN_USER, "admin") == 0 && strcmp(storedPassword, password_input) == 0) {
                loggedIn = 1;
            }
        } // End of successful read lock
        // Close file handle if it wasn't closed during init
         if (!fileNeedsInit && passFile != -1) close(passFile);
    } // End of file open check


    if(loggedIn) {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nAdmin Login Successfully^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
    }
    else // Login Failed
    {
        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, "\nInvalid credential^");
        write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
        goto label_admin_login;
    }

    // --- Admin Menu Loop ---
    while(1)
    {
        int modifyType, choice;

        bzero(outBuffer, sizeof(outBuffer));
        strcpy(outBuffer, ADMIN_PROMPT);
        write(clientSocket, outBuffer, strlen(outBuffer));

        bzero(inBuffer, sizeof(inBuffer));
        if (read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
            printf("Admin client disconnected.\n"); return; // Exit if client disconnects
        }
        inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize choice
        choice = atoi(inBuffer);
        printf("Admin choice: %d\n", choice);

        switch (choice) {
            case 1: // Add New Employee
                if(createNewEmployee(clientSocket)) {
                    bzero(outBuffer, sizeof(outBuffer)); strcpy(outBuffer, "Employee added successfully^");
                } else {
                    // Error message already sent by createNewEmployee if needed
                    strcpy(outBuffer, "^"); // Send empty ack if needed
                }
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                break;
            case 2: // Modify
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "[1] Modify Customer\n[2] Modify Employee\nChoice: ");
                write(clientSocket, outBuffer, strlen(outBuffer));

                bzero(inBuffer, sizeof(inBuffer));
                 if (read(clientSocket, inBuffer, sizeof(inBuffer)-1) <= 0) {
                     printf("Admin client disconnected.\n"); return;
                 }
                 inBuffer[strcspn(inBuffer, "\r\n")] = 0; // Sanitize choice
                modifyType = atoi(inBuffer);

                modifyUser(clientSocket, modifyType); // modifyUser handles its own acks
                break;
            case 3: // Manage Roles
                updateEmployeeRole(clientSocket); // handles its own acks
                break;
            case 4: // Change Password
                changeAdminPassword(clientSocket); // handles its own acks
                // No need to force re-login for admin in this simple setup
                break;
            case 5: // Logout
                printf("Admin logged out.\n");
                // Send final ack before returning
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "Logging out...^");
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
                return; // Return to main server loop
            default:
                bzero(outBuffer, sizeof(outBuffer));
                strcpy(outBuffer, "Invalid choice!^");
                write(clientSocket, outBuffer, strlen(outBuffer)); read(clientSocket, inBuffer, 3); // ack
        }
    }
}

#endif // ADMIN_OPS_H
