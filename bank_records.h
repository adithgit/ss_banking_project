#ifndef BANK_RECORDS_H
#define BANK_RECORDS_H

struct TransactionLog
{
    int accountID;
    char logEntry[1024];
};

struct ClientFeedback
{
    char message[1024];
};

struct BankStaff {
    int staffID;
    char firstName[20];
    char lastName[20];
    char password[50]; // Plaintext password
    int roleType; // 0 -> Manager, 1 -> Staff
};

struct LoanRecord {
    int assignedStaffID;
    int accountID;
    int loanRecordID;
    int amount;
    int loanStatus; // 0 -> requested, 1 -> pending, 2 -> approved, 3 -> rejected
};

struct AccountHolder {
    int accountID;
    float currentBalance;
    char holderName[20];
    char password[50]; // Plaintext password
    int isActive; // 0 -> deactivate, 1 -> activate
};

struct IDGenerator {
    int nextID;
};

#endif
