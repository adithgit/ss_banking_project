#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> 
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h> 

void serverCommunicationLoop(int serverSocket);
void getMaskedInput(char *buffer, int bufSize);

int main(int argc, char *argv[]) 
{
    int serverSocket;
    int connectStatus;
    struct sockaddr_in serverAddress;
    const char *serverIP = "127.0.0.1"; 
    int serverPort = 8080; 

    // Optional: Allow specifying server IP and Port via command line
    // if (argc > 1) {
    //     serverIP = argv[1];
    // }
    // if (argc > 2) {
    //     serverPort = atoi(argv[2]);
    //     if (serverPort <= 0 || serverPort > 65535) {
    //          fprintf(stderr, "Invalid port number: %s\n", argv[2]);
    //          exit(EXIT_FAILURE);
    //     }
    // }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1)
    {
        perror("Client socket creation failed");
        exit(EXIT_FAILURE);
    }
    printf("Client socket created\n");

    serverAddress.sin_addr.s_addr = inet_addr(serverIP); // Use specific IP
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(serverPort);

    printf("Attempting to connect to %s:%d...\n", serverIP, serverPort);
    connectStatus = connect(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if(connectStatus == -1)
    {
        perror("Client connection to server failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }
    printf("Connected to server\n");

    serverCommunicationLoop(serverSocket);

    close(serverSocket);
    printf("Connection closed.\n");
    return 0; 
}

// interacting with sever loop
void serverCommunicationLoop(int serverSocket)
{
    char inBuffer[4096], outBuffer[4096], displayBuffer[4096];
    int readBytes, writeBytes;
    ssize_t inputLen; 

    do
    {
        bzero(inBuffer, sizeof(inBuffer));
        readBytes = read(serverSocket, inBuffer, sizeof(inBuffer) - 1); // Leave space for null term

        if(readBytes <= 0)
        {
             if (readBytes == 0) {
                 printf("\nServer closed the connection.\n");
             } else {
                perror("\nRead from server failed");
             }
            break;
        }
        inBuffer[readBytes] = '\0'; 
        int isPasswordPrompt = (strstr(inBuffer, "password:") != NULL || strstr(inBuffer, "Password:") != NULL);

        // Server signals client to exit immediately
        if(strcmp(inBuffer, "Client logging out...\n") == 0)
        {
            printf("%s", inBuffer);
            break; // Exit loop
        }

        // Check if server sent a message ending with '^' (our signal for needing an ACK)
        char *ackSignal = strchr(inBuffer, '^');
        if (ackSignal != NULL) {
            *ackSignal = '\0'; // Replace '^' with null terminator to print message cleanly
            if (strlen(inBuffer) > 0) {
                 printf("%s\n", inBuffer); // Print message before the '^'
            }
             // send ack
             strcpy(outBuffer, "ACK");

        } else if (isPasswordPrompt) {
            // Print the password prompt without extra newline
            printf("%s", inBuffer);
            fflush(stdout); // to ennsure prompt is displayed before input
            getMaskedInput(outBuffer, sizeof(outBuffer)); 

        } else {
            // Standard prompt from server, needs user input
             printf("%s", inBuffer);
             fflush(stdout);
             // Read user input line 
             if (fgets(outBuffer, sizeof(outBuffer), stdin) == NULL) {
                 // Handle EOF or read error
                 printf("\nInput error or EOF detected. Exiting.\n");
                 break;
             }
              // Remove trailing newline from fgets
             outBuffer[strcspn(outBuffer, "\r\n")] = 0;
        }

        // send the response (ACK, password, or regular input)
        writeBytes = write(serverSocket, outBuffer, strlen(outBuffer));
        if(writeBytes < 0)
        {
            perror("Client write to server failed");
            break;
        }
        // write was not completed fully
        if (writeBytes < strlen(outBuffer)) {
            fprintf(stderr, "Warning: Partial write to server occurred.\n");
        }


    } while(readBytes > 0); // Continue loop as long as server is sending data
}

// for passwords
void getMaskedInput(char *buffer, int bufSize) {
    struct termios oldTerm, newTerm;
    int nread;
    // Get current terminal settings
    if (tcgetattr(STDIN_FILENO, &oldTerm) != 0) {
        perror("tcgetattr failed");
        fgets(buffer, bufSize, stdin); // Simple fallback
        buffer[strcspn(buffer, "\r\n")] = 0; // Remove newline
        return;
    }

    newTerm = oldTerm;
    newTerm.c_lflag &= ~(ECHO | ICANON);
    
    // set new settings
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newTerm) != 0) {
         perror("tcsetattr failed");
         // Fallback
         fgets(buffer, bufSize, stdin);
         buffer[strcspn(buffer, "\r\n")] = 0;
         return;
    }

    // read i/p
    if (fgets(buffer, bufSize, stdin) == NULL) {
        *buffer = '\0'; 
        perror("fgets error during masked input");
    } else {
         buffer[strcspn(buffer, "\r\n")] = 0;
    }

    // back to original settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
    printf("\n");
}