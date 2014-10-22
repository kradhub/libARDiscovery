#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include <libARSAL/ARSAL_Print.h>
#include <libARSAL/ARSAL_Socket.h>
#include <libARSAL/ARSAL_Sem.h>

#include <libARDiscovery/ARDISCOVERY_Connection.h>
#include "ARDISCOVERY_Connection.h"

#define ARDISCOVERY_CONNECTION_TAG "ARDISCOVERY_Connection"

#define ARDISCOVERY_CONNECTION_TIMEOUT_SEC 5

#define ARDISCOVERY_RECONNECTION_TIME_SEC  3

/*************************
 * Private header
 *************************/

/**
 * @brief Initializes a socket
 * @param[in] socket socket to intialize
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_CreateSocket (int *socket);

/**
 * @brief Initializes the TCP socket for sending
 * On controller, port is self known and connecting is done first.
 * On device, port is known once received from controller.
 * @param[in] deviceSocket socket used by the device
 * @param[in] port port to receive
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_DeviceInitSocket (int *deviceSocket, int port);

/**
 * @brief Connects the Controller TCP socket
 * On controller, port is self known and connecting is done first.
 * On device, port is known once received from controller.
 * @param[in] connectionData connection data
 * @param[in] port port to receive
 * @param[in] ip IP to connect to
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_ControllerInitSocket (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int port, const char *ip);

/**
 * @brief accepts the connection
 * @param[in] connectionData connection data
 * @param[in] deviceSocket socket used by the device
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_DeviceAccept (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int deviceSocket);

/**
 * @brief receives the connection data
 * @param[in] connectionData connection data
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_RxPending (ARDISCOVERY_Connection_ConnectionData_t *connectionData);

/**
 * @brief sends the connection data
 * @param[in] connectionData connection data
 * @return error during execution
 */
static eARDISCOVERY_ERROR ARDISCOVERY_Connection_TxPending (ARDISCOVERY_Connection_ConnectionData_t *connectionData);

/**
 * @brief unlocks
 * @param[in] connectionData connection data
 */
static void ARDISCOVERY_Connection_Unlock (ARDISCOVERY_Connection_ConnectionData_t *connectionData);

/*************************
 * Implementation
 *************************/

ARDISCOVERY_Connection_ConnectionData_t* ARDISCOVERY_Connection_New (ARDISCOVERY_Connection_SendJsonCallback_t sendJsonCallback, ARDISCOVERY_Connection_ReceiveJsonCallback_t receiveJsonCallback, void *customData, eARDISCOVERY_ERROR *error)
{
    /* - Create and initialize connection data - */
    
    ARDISCOVERY_Connection_ConnectionData_t *connectionData = NULL;
    eARDISCOVERY_ERROR localError = ARDISCOVERY_OK;
    
    /* Check parameter */
    if ((sendJsonCallback == NULL) || (receiveJsonCallback == NULL))
    {
        localError = ARDISCOVERY_ERROR_BAD_PARAMETER;
    }
    
    if (localError == ARDISCOVERY_OK)
    {
        connectionData = malloc(sizeof(ARDISCOVERY_Connection_ConnectionData_t));
        if (connectionData != NULL)
        {
            /* Initialize connectionData */
            connectionData->txData.buffer = NULL;
            connectionData->txData.size = 0;
            connectionData->rxData.buffer = NULL;
            connectionData->rxData.size = 0;
            connectionData->isAlive = 0;
            ARSAL_Sem_Init (&(connectionData->runningSem), 0, 1);
            connectionData->sendJsoncallback = sendJsonCallback;
            connectionData->receiveJsoncallback = receiveJsonCallback;
            connectionData->customData = customData;
            connectionData->socket = -1;
            memset(&(connectionData->address), 0, sizeof(connectionData->address));
            connectionData->abortPipe[0] = -1;
            connectionData->abortPipe[1] = -1;
        }
        else
        {
            localError = ARDISCOVERY_ERROR_ALLOC;
        }
    }
    
    /* Allocate reception buffer */
    if (localError == ARDISCOVERY_OK)
    {
        connectionData->rxData.buffer = (uint8_t *) malloc (sizeof(uint8_t) * ARDISCOVERY_CONNECTION_RX_BUFFER_SIZE);
        if (connectionData->rxData.buffer != NULL)
        {
            connectionData->rxData.size = 0;
        }
        else
        {
            localError = ARDISCOVERY_ERROR_ALLOC;
        }
    }
    
    /* Allocate transmission buffer */
    if (localError == ARDISCOVERY_OK)
    {
        connectionData->txData.buffer = (uint8_t *) malloc(sizeof(uint8_t) * ARDISCOVERY_CONNECTION_TX_BUFFER_SIZE);
        if (connectionData->txData.buffer != NULL)
        {
            connectionData->txData.size = 0;
        }
        else
        {
            localError = ARDISCOVERY_ERROR_ALLOC;
        }
    }
    
    /* initialize the abortPipe */
    if (localError == ARDISCOVERY_OK)
    {
        if (pipe(connectionData->abortPipe) != 0)
        {
            localError = ARDISCOVERY_ERROR_PIPE_INIT;
        }
    }
    
    /* Delete connection data if an error occurred */
    if (localError != ARDISCOVERY_OK)
    {
        ARDISCOVERY_Connection_Delete (&connectionData);
    }

    /* return error */
    if (error != NULL)
    {
        *error = localError;
    }

    return connectionData;
}

eARDISCOVERY_ERROR ARDISCOVERY_Connection_Delete (ARDISCOVERY_Connection_ConnectionData_t **connectionData)
{
    /*
     * Free connection data
     */
     
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    
    /* Check parameter */
    if (connectionData == NULL)
    {
        error = ARDISCOVERY_ERROR_BAD_PARAMETER;
    }
     
    if (error == ARDISCOVERY_OK)
    {
        if ((*connectionData) != NULL)
        {
            if ( ARSAL_Sem_Trywait(&((*connectionData)->runningSem)) == 0)
            {
                /* Destroy runningSem*/
                ARSAL_Sem_Destroy(&((*connectionData)->runningSem));
                
                if ((*connectionData)->txData.buffer)
                {
                    free((*connectionData)->txData.buffer);
                    (*connectionData)->txData.buffer = NULL;
                    (*connectionData)->txData.size = 0;
                }
                if ((*connectionData)->rxData.buffer)
                {
                    free((*connectionData)->rxData.buffer);
                    (*connectionData)->rxData.buffer = NULL;
                    (*connectionData)->rxData.size = 0;
                }
                
                /* close the abortPipe */
                if((*connectionData)->abortPipe[0] != -1)
                {
                    close ((*connectionData)->abortPipe[0]);
                    (*connectionData)->abortPipe[0] = -1;
                }
                
                if((*connectionData)->abortPipe[1] != -1)
                {
                    close ((*connectionData)->abortPipe[1]);
                    (*connectionData)->abortPipe[1] = -1;
                }
                
                free (*connectionData);
                (*connectionData) = NULL;
            }
            else
            {
                error = ARDISCOVERY_ERROR_BUSY;
            }
        }
    }
    
    return error;
}

eARDISCOVERY_ERROR ARDISCOVERY_Connection_DeviceListeningLoop (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int port)
{
    /*
     * Initialize connection
     */
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    eARDISCOVERY_ERROR loopError = ARDISCOVERY_OK;
    
    int deviceSocket = -1;
    
    /* Check parameter */
    if (connectionData == NULL)
    {
        error = ARDISCOVERY_ERROR_BAD_PARAMETER;
    }
    
    if (error == ARDISCOVERY_OK)
    {
        /* check if it is already running */
        if (ARSAL_Sem_Trywait(&(connectionData->runningSem)) != 0)
        {
            error = ARDISCOVERY_ERROR_BUSY;
        }
    }
    
    if (error == ARDISCOVERY_OK)
    {
        /* Initialize the server socket */
        error = ARDISCOVERY_Connection_DeviceInitSocket (&deviceSocket, port);
    }
    
    if (error == ARDISCOVERY_OK)
    {
        /*  Initialize is alive */
        connectionData->isAlive = 1;
        
        /* while is alive */
        while (connectionData->isAlive == 1)
        {
            /* Wait for any incoming connection from controller */
            loopError = ARDISCOVERY_Connection_DeviceAccept (connectionData, deviceSocket);
            
            if (loopError == ARDISCOVERY_OK)
            {
                ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "Device accepts a socket");
                
                /* receiption  */
                loopError = ARDISCOVERY_Connection_RxPending (connectionData);
            }
            
            if (loopError == ARDISCOVERY_OK)
            {
                /* sending */
                loopError = ARDISCOVERY_Connection_TxPending (connectionData);
            }
            
            /* close the client socket */
            if (connectionData->socket != -1)
            {
                ARSAL_Socket_Close (connectionData->socket);
                connectionData->socket = -1;
            }
            
            if (loopError != ARDISCOVERY_OK)
            {
                /* print the error occurred */
                ARSAL_PRINT(ARSAL_PRINT_ERROR, ARDISCOVERY_CONNECTION_TAG, "error: %s", ARDISCOVERY_Error_ToString (loopError));
            }
        }
        
        /* close deviceSocket */
        if(deviceSocket != -1)
        {
            ARSAL_Socket_Close (deviceSocket);
            deviceSocket = -1;
        }
        
        /* reset the runningSem */
        ARSAL_Sem_Post(&(connectionData->runningSem));
    }
    
    return error;
}

void ARDISCOVERY_Connection_Device_StopListening (ARDISCOVERY_Connection_ConnectionData_t *connectionData)
{
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    
    /* Check parameter */
    if (connectionData == NULL)
    {
        error = ARDISCOVERY_ERROR_BAD_PARAMETER;
    }
    
    if (error == ARDISCOVERY_OK)
    {
        /* Stop reception */
        connectionData->isAlive = 0;
        ARDISCOVERY_Connection_Unlock (connectionData);
        
        /* wait the end of the run*/
        ARSAL_Sem_Wait (&(connectionData->runningSem));
        /* reset the runningSem */
        ARSAL_Sem_Post(&(connectionData->runningSem));
    }
}

eARDISCOVERY_ERROR ARDISCOVERY_Connection_ControllerConnection (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int port, const char *ip)
{
    /*
     * Initialize connection
     */
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;

    /* Check parameter */
    if (connectionData == NULL)
    {
        error = ARDISCOVERY_ERROR_BAD_PARAMETER;
    }

    if (error == ARDISCOVERY_OK)
    {
        /* check if it is already running */
        if (ARSAL_Sem_Trywait(&(connectionData->runningSem)) != 0)
        {
            error = ARDISCOVERY_ERROR_BUSY;
        }
    }

    if (error == ARDISCOVERY_OK)
    {
        /* Start by contacting the device we're interested in */
        error = ARDISCOVERY_Connection_ControllerInitSocket (connectionData, port, ip);
        
        if (error == ARDISCOVERY_OK)
        {
            /* sending */
            error = ARDISCOVERY_Connection_TxPending (connectionData);
        }
        
        if (error == ARDISCOVERY_OK)
        {
            /* receiption  */
            error = ARDISCOVERY_Connection_RxPending (connectionData);
        }
        
        /* close the socket*/
        if (connectionData->socket != -1)
        {
            ARSAL_Socket_Close (connectionData->socket);
            connectionData->socket = -1;
        }
        
        /* reset the runningSem */
        ARSAL_Sem_Post(&(connectionData->runningSem));
    }
    return error;
}

/*************************
 * local Implementation
 *************************/

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_CreateSocket (int *socket)
{
    /*
     * Create TCP socket
     */
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    int optval = 1;
    
    *socket = ARSAL_Socket_Create(AF_INET, SOCK_STREAM, 0);
    if (*socket < 0)
    {
        error = ARDISCOVERY_ERROR_SOCKET_CREATION;
    }
    else
    {
        if (setsockopt (*socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) != 0)
        {
            error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
        }
    }
    
    return error;
}

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_DeviceInitSocket (int *deviceSocket, int port)
{
    /*
     * Bind TCP socket for receiving
     * On device, port is self known and listening is done first.
     */
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    struct sockaddr_in recvSin;
    int errorBind, errorListen = 0;

    /* Create TCP socket */
    if (error == ARDISCOVERY_OK)
    {
        error = ARDISCOVERY_Connection_CreateSocket (deviceSocket);
    }

    /* Init socket */
    if (error == ARDISCOVERY_OK)
    {
        /* Server side (device) : listen to anyone */
        recvSin.sin_addr.s_addr = htonl(INADDR_ANY);
        
        recvSin.sin_family = AF_INET;
        recvSin.sin_port = htons(port);

        errorBind = ARSAL_Socket_Bind (*deviceSocket, (struct sockaddr*) &recvSin, sizeof (recvSin));

        if (errorBind != 0)
        {
            ARSAL_PRINT(ARSAL_PRINT_ERROR, ARDISCOVERY_CONNECTION_TAG, "bind() failed: %s", strerror(errno));

            switch (errno)
            {
            case EACCES:
                error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
                break;

            default:
                error = ARDISCOVERY_ERROR;
                break;
            }
        }

        errorListen = ARSAL_Socket_Listen (*deviceSocket, 10);

        if (errorListen != 0)
        {
            ARSAL_PRINT(ARSAL_PRINT_ERROR, ARDISCOVERY_CONNECTION_TAG, "listen() failed: %s", strerror(errno));

            switch (errno)
            {
            case EINVAL:
                error = ARDISCOVERY_ERROR_SOCKET_ALREADY_CONNECTED;
                break;

            default:
                error = ARDISCOVERY_ERROR;
                break;
            }
        }
    }

    return error;
}

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_ControllerInitSocket (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int port, const char *ip)
{
    /*
     * On controller, port is known once received from device.
     */
    
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    int connectError;
    int flags = 0;
    fd_set readSet;
    fd_set writeSet;
    fd_set errorSet;
    int maxFd = 0;
    struct timeval tv = {ARDISCOVERY_CONNECTION_TIMEOUT_SEC, 0};
    int selectErr = 0;
    char dump[10];

    /* Create TCP socket */
    if (error == ARDISCOVERY_OK)
    {
        error = ARDISCOVERY_Connection_CreateSocket (&(connectionData->socket));
    }

    /* Initialize socket */
    if (error == ARDISCOVERY_OK)
    {
        /* Client side (controller) : listen to the device we chose */
        connectionData->address.sin_addr.s_addr = inet_addr (ip);   
        connectionData->address.sin_family = AF_INET;
        connectionData->address.sin_port = htons (port);
        
        /* set the socket non blocking */
        flags = fcntl(connectionData->socket, F_GETFL, 0);
        fcntl(connectionData->socket, F_SETFL, flags | O_NONBLOCK);
        
        ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "contoller try to connect ip:%s port:%d", ip, port);
        
        connectError = ARSAL_Socket_Connect (connectionData->socket, (struct sockaddr*) &(connectionData->address), sizeof (connectionData->address));
        
        if (connectError != 0)
        {
            switch (errno)
            {
                case EINPROGRESS:
                    /* in connection */
                    /* do nothing */
                    break;
                case EACCES:
                    error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
                    break;
                case ENETUNREACH:
                case EHOSTUNREACH:
                {
                    // in that particular case, we retry a connection because the host may be not resolved after a very recent connection
                    sleep(ARDISCOVERY_RECONNECTION_TIME_SEC);
                    connectError = ARSAL_Socket_Connect (connectionData->socket, (struct sockaddr*) &(connectionData->address), sizeof (connectionData->address));
                    if (connectError != 0)
                    {
                        switch (errno)
                        {
                            case EINPROGRESS:
                                /* in connection */
                                /* do nothing */
                                break;
                            case EACCES:
                                error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
                                break;
                                
                            default:
                                error = ARDISCOVERY_ERROR;
                                break;
                        }
                    }
                    break;
                }
                default:
                    error = ARDISCOVERY_ERROR;
                    break;
            }
            
            if (error != ARDISCOVERY_OK)
            {
                ARSAL_PRINT(ARSAL_PRINT_ERROR, ARDISCOVERY_CONNECTION_TAG, "connect() failed: %d %s", errno, strerror(errno));
            }
        }
        
        /* set the socket non blocking */
        flags = fcntl(connectionData->socket, F_GETFL, 0);
        fcntl(connectionData->socket, F_SETFL, flags & (~O_NONBLOCK));
        
        /* Initialize set */
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(connectionData->socket, &writeSet);
        FD_SET(connectionData->abortPipe[0], &readSet);
        FD_SET(connectionData->socket, &errorSet);
        
        /* Get the max fd +1 for select call */
        maxFd = (connectionData->socket > connectionData->abortPipe[0]) ? connectionData->socket +1 : connectionData->abortPipe[0] +1;
        
        /* Wait for either file to be reading for a read */
        selectErr = select (maxFd, &readSet, &writeSet, &errorSet, &tv);
        
        if (selectErr < 0)
        {
            /* Read error */
            error = ARDISCOVERY_ERROR_SELECT;
        }
        else if(selectErr == 0)
        {
            /* timeout error*/
            error = ARDISCOVERY_ERROR_TIMEOUT;
        }
        else
        {
            if (FD_ISSET(connectionData->socket, &errorSet))
            {
                error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
            }
            /* No else: no socket error*/
            
            if (FD_ISSET(connectionData->socket, &writeSet))
            {
                int valopt = 0;
                socklen_t lon;
                int getsockoptRes = 0;
                
                lon = sizeof(int);
                getsockoptRes = getsockopt(connectionData->socket, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);
                if (getsockoptRes < 0)
                {
                    error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
                }
                else
                {
                    if (valopt)
                    {
                        error = ARDISCOVERY_ERROR_SOCKET_PERMISSION_DENIED;
                    }
                    /* No else: socket successfully connected */
                }
            }
            /* No else: socket not ready */
            
            if (FD_ISSET(connectionData->abortPipe[0], &readSet))
            {
                /* If the abortPipe is ready for a read, dump bytes from it (so it won't be ready next time) */
                read (connectionData->abortPipe[0], &dump, 10);
                error = ARDISCOVERY_ERROR_ABORT;
            }
            /* No else: no timeout */
        }
    }
    
    return error;
}

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_DeviceAccept (ARDISCOVERY_Connection_ConnectionData_t *connectionData, int deviceSocket)
{
    /* - Accept connection - */
    
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    
    socklen_t clientLen = sizeof (connectionData->address);
    
    fd_set set;
    int maxFd = 0;
    int selectErr =0;
    char dump[10];
    
    /* Initialize set */
    FD_ZERO(&set);
    FD_SET(deviceSocket, &set);
    FD_SET(connectionData->abortPipe[0], &set);
    
    /* Get the max fd +1 for select call */
    maxFd = (deviceSocket > connectionData->abortPipe[0]) ? deviceSocket +1 : connectionData->abortPipe[0] +1;
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "Device waits to accept a socket");
            
    /* Wait for either file to be reading for a read */
    selectErr = select (maxFd, &set, NULL, NULL, NULL);
    
    if (selectErr < 0)
    {
        /* Read error */
        error = ARDISCOVERY_ERROR_SELECT;
    }
    else
    {
        if (FD_ISSET(deviceSocket, &set))
        {
            /* Wait for any incoming connection from controller */
            connectionData->socket = ARSAL_Socket_Accept (deviceSocket, (struct sockaddr*) &(connectionData->address), &clientLen);
            if (connectionData->socket < 0)
            {
                ARSAL_PRINT(ARSAL_PRINT_ERROR, ARDISCOVERY_CONNECTION_TAG, "accept() failed: %s", strerror(errno));
                error = ARDISCOVERY_ERROR_ACCEPT;
            }
        }
        
        if (FD_ISSET(connectionData->abortPipe[0], &set))
        {
            /* If the abortPipe is ready for a read, dump bytes from it (so it won't be ready next time) */
            read (connectionData->abortPipe[0], &dump, 10);
            error = ARDISCOVERY_ERROR_ABORT;
        }
    }
    
    return error;
}

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_RxPending (ARDISCOVERY_Connection_ConnectionData_t *connectionData)
{
    /* - read connection data - */
    
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    fd_set set;
    int maxFd = 0;
    struct timeval tv = { ARDISCOVERY_CONNECTION_TIMEOUT_SEC, 0 };
    int selectErr = 0;
    char dump[10];
    
    /* Initialize set */
    FD_ZERO(&set);
    FD_SET(connectionData->socket, &set);
    FD_SET(connectionData->abortPipe[0], &set);
    
    /* Get the max fd +1 for select call */
    maxFd = (connectionData->socket > connectionData->abortPipe[0]) ? connectionData->socket +1 : connectionData->abortPipe[0] +1;
    
    /* Wait for either file to be reading for a read */
    selectErr = select (maxFd, &set, NULL, NULL, &tv);
    
    if (selectErr < 0)
    {
        /* Read error */
        error = ARDISCOVERY_ERROR_SELECT;
    }
    else if(selectErr == 0)
    {
        /* timeout error*/
        error = ARDISCOVERY_ERROR_TIMEOUT;
    }
    else
    {
        if (FD_ISSET(connectionData->socket, &set))
        {
            /* Read content from incoming connection */
            int readSize = ARSAL_Socket_Recv (connectionData->socket, connectionData->rxData.buffer, ARDISCOVERY_CONNECTION_RX_BUFFER_SIZE, 0);
            
            ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "data read size: %d", readSize);
            
            if (readSize > 0)
            {
                /* set the rxdata size */
                connectionData->rxData.size = readSize;
                
                ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "data read: %s", connectionData->rxData.buffer);
                
                /* receive callback */
                error = connectionData->receiveJsoncallback (connectionData->rxData.buffer, connectionData->rxData.size, inet_ntoa(connectionData->address.sin_addr), connectionData->customData);
            }
            else
            {
                error = ARDISCOVERY_ERROR_READ;
            }
        }
        
        if (FD_ISSET(connectionData->abortPipe[0], &set))
        {
            /* If the abortPipe is ready for a read, dump bytes from it (so it won't be ready next time) */
            read (connectionData->abortPipe[0], &dump, 10);
            error = ARDISCOVERY_ERROR_ABORT;
        }
    }
    
    return error;
}

static eARDISCOVERY_ERROR ARDISCOVERY_Connection_TxPending (ARDISCOVERY_Connection_ConnectionData_t *connectionData)
{
    /* - send connection data - */
    
    eARDISCOVERY_ERROR error = ARDISCOVERY_OK;
    ssize_t sendSize = 0;
    fd_set readSet;
    fd_set writeSet;
    int maxFd = 0;
    struct timeval tv = { ARDISCOVERY_CONNECTION_TIMEOUT_SEC, 0 };
    int selectErr = 0;
    char dump[10];
    
    /* initilize set */
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_SET(connectionData->socket, &writeSet);
    FD_SET(connectionData->abortPipe[0], &readSet);
    
    /* Get the max fd +1 for select call */
    maxFd = (connectionData->socket > connectionData->abortPipe[0]) ? connectionData->socket +1 : connectionData->abortPipe[0] +1;
    
    /* sending callback */
    error = connectionData->sendJsoncallback (connectionData->txData.buffer, &(connectionData->txData.size), connectionData->customData);
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "data send size: %d", connectionData->txData.size);
    
    /* check the txData size */
    if ((error == ARDISCOVERY_OK) && (connectionData->txData.size > 0) && (connectionData->txData.size <= ARDISCOVERY_CONNECTION_TX_BUFFER_SIZE))
    {
        ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDISCOVERY_CONNECTION_TAG, "data send: %s", connectionData->txData.buffer);
        
        /* Wait for either file to be reading for a read */
        selectErr = select (maxFd, &readSet, &writeSet, NULL, &tv);
        
        if (selectErr < 0)
        {
            /* Read error */
            error = ARDISCOVERY_ERROR_SELECT;
        }
        else if(selectErr == 0)
        {
            /* timeout error*/
            error = ARDISCOVERY_ERROR_TIMEOUT;
        }
        else
        {
            if (FD_ISSET(connectionData->socket, &writeSet))
            {
                /* Send txData */
                sendSize = ARSAL_Socket_Send(connectionData->socket, connectionData->txData.buffer, connectionData->txData.size, 0);
                if (sendSize < 0)
                {
                    error = ARDISCOVERY_ERROR_SEND;
                }
            }
            
            if (FD_ISSET(connectionData->abortPipe[0], &readSet))
            {
                /* If the abortPipe is ready for a read, dump bytes from it (so it won't be ready next time) */
                read (connectionData->abortPipe[0], &dump, 10);
                error = ARDISCOVERY_ERROR_ABORT;
            }
        }
    }
    
    return error;
}

void ARDISCOVERY_Connection_Unlock (ARDISCOVERY_Connection_ConnectionData_t *connectionData)
{
    /* - signal - */
    
    char *buff = "x";
    
    if (connectionData->abortPipe[1] != -1)
    {
        write (connectionData->abortPipe[1], buff, 1);
    }
}

void ARDISCOVERY_Connection_getControllerIP (ARDISCOVERY_Connection_ConnectionData_t *connectionData, char* buffer)
{
    strncpy (buffer, inet_ntoa (connectionData->address.sin_addr), 16);
}
