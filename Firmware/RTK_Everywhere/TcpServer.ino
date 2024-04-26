/*
tcpServer.ino

  The TCP (position, velocity and time) server sits on top of the network layer
  and sends position data to one or more computers or cell phones for display.

                Satellite     ...    Satellite
                     |         |          |
                     |         |          |
                     |         V          |
                     |        RTK         |
                     '------> Base <------'
                            Station
                               |
                               | NTRIP Server sends correction data
                               V
                         NTRIP Caster
                               |
                               | NTRIP Client receives correction data
                               |
                               V
            Bluetooth         RTK                 Network: TCP Client
           .---------------- Rover ----------------------------------.
           |                   |                                     |
           |                   | Network: TCP Server                 |
           | TCP data          | Position, velocity & time data      | TCP data
           |                   V                                     |
           |              Computer or                                |
           '------------> Cell Phone <-------------------------------'
                          for display

*/

#ifdef COMPILE_WIFI

//----------------------------------------
// Constants
//----------------------------------------

#define TCP_SERVER_MAX_CLIENTS 4
#define TCP_SERVER_CLIENT_DATA_TIMEOUT (15 * 1000)

// Define the TCP server states
enum tcpServerStates
{
    TCP_SERVER_STATE_OFF = 0,
    TCP_SERVER_STATE_NETWORK_STARTED,
    TCP_SERVER_STATE_RUNNING,
    // Insert new states here
    TCP_SERVER_STATE_MAX // Last entry in the state list
};

const char *const tcpServerStateName[] = {
    "TCP_SERVER_STATE_OFF",
    "TCP_SERVER_STATE_NETWORK_STARTED",
    "TCP_SERVER_STATE_RUNNING",
};

const int tcpServerStateNameEntries = sizeof(tcpServerStateName) / sizeof(tcpServerStateName[0]);

const RtkMode_t tcpServerMode = RTK_MODE_BASE_FIXED | RTK_MODE_BASE_SURVEY_IN | RTK_MODE_ROVER;

//----------------------------------------
// Locals
//----------------------------------------

// TCP server
static WiFiServer *tcpServer = nullptr;
static uint8_t tcpServerState;
static uint32_t tcpServerTimer;

// TCP server clients
static volatile uint8_t tcpServerClientConnected;
static volatile uint8_t tcpServerClientDataSent;
static volatile uint8_t tcpServerClientWriteError;
static NetworkClient *tcpServerClient[TCP_SERVER_MAX_CLIENTS];
static IPAddress tcpServerClientIpAddress[TCP_SERVER_MAX_CLIENTS];
static volatile RING_BUFFER_OFFSET tcpServerClientTails[TCP_SERVER_MAX_CLIENTS];

//----------------------------------------
// TCP Server handleGnssDataTask Support Routines
//----------------------------------------

// Send data to the TCP clients
int32_t tcpServerClientSendData(int index, uint8_t *data, uint16_t length)
{
    length = tcpServerClient[index]->write(data, length);
    if (length > 0)
    {
        // Update the data sent flag when data successfully sent
        if (length > 0)
            tcpServerClientDataSent |= 1 << index;
        if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_CLIENT_DATA)) && (!inMainMenu))
        {
            PERIODIC_CLEAR(PD_TCP_SERVER_CLIENT_DATA);
            systemPrintf("TCP server wrote %d bytes to %d.%d.%d.%d\r\n", length, tcpServerClientIpAddress[index][0],
                         tcpServerClientIpAddress[index][1], tcpServerClientIpAddress[index][2],
                         tcpServerClientIpAddress[index][3]);
        }
    }

    // Failed to write the data
    else
    {
        // Done with this client connection
        if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_CLIENT_DATA)) && (!inMainMenu))
        {
            PERIODIC_CLEAR(PD_TCP_SERVER_CLIENT_DATA);
            systemPrintf("TCP server breaking connection %d with client %d.%d.%d.%d\r\n", index,
                         tcpServerClientIpAddress[index][0], tcpServerClientIpAddress[index][1],
                         tcpServerClientIpAddress[index][2], tcpServerClientIpAddress[index][3]);
        }

        tcpServerClient[index]->stop();
        tcpServerClientConnected &= ~(1 << index);
        tcpServerClientWriteError |= 1 << index;
        length = 0;
    }
    return length;
}

// Send TCP data to the clients
int32_t tcpServerSendData(uint16_t dataHead)
{
    int32_t usedSpace = 0;

    int32_t bytesToSend;
    int index;
    uint16_t tail;

    // Update each of the clients
    for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
    {
        tail = tcpServerClientTails[index];

        // Determine if the client is connected
        if (!(tcpServerClientConnected & (1 << index)))
            tail = dataHead;
        else
        {
            // Determine the amount of TCP data in the buffer
            bytesToSend = dataHead - tail;
            if (bytesToSend < 0)
                bytesToSend += settings.gnssHandlerBufferSize;
            if (bytesToSend > 0)
            {
                // Reduce bytes to send if we have more to send then the end of the buffer
                // We'll wrap next loop
                if ((tail + bytesToSend) > settings.gnssHandlerBufferSize)
                    bytesToSend = settings.gnssHandlerBufferSize - tail;

                // Send the data to the TCP server clients
                bytesToSend = tcpServerClientSendData(index, &ringBuffer[tail], bytesToSend);

                // Assume all data was sent, wrap the buffer pointer
                tail += bytesToSend;
                if (tail >= settings.gnssHandlerBufferSize)
                    tail -= settings.gnssHandlerBufferSize;

                // Update space available for use in UART task
                bytesToSend = dataHead - tail;
                if (bytesToSend < 0)
                    bytesToSend += settings.gnssHandlerBufferSize;
                if (usedSpace < bytesToSend)
                    usedSpace = bytesToSend;
            }
        }
        tcpServerClientTails[index] = tail;
    }

    // Return the amount of space that TCP server client is using in the buffer
    return usedSpace;
}

// Remove previous messages from the ring buffer
void discardTcpServerBytes(RING_BUFFER_OFFSET previousTail, RING_BUFFER_OFFSET newTail)
{
    int index;
    uint16_t tail;

    // Update each of the clients
    for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
    {
        tail = tcpServerClientTails[index];
        if (previousTail < newTail)
        {
            // No buffer wrap occurred
            if ((tail >= previousTail) && (tail < newTail))
                tcpServerClientTails[index] = newTail;
        }
        else
        {
            // Buffer wrap occurred
            if ((tail >= previousTail) || (tail < newTail))
                tcpServerClientTails[index] = newTail;
        }
    }
}

//----------------------------------------
// TCP Server Routines
//----------------------------------------

// Update the state of the TCP server state machine
void tcpServerSetState(uint8_t newState)
{
    if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_STATE)) && (!inMainMenu))
    {
        if (tcpServerState == newState)
            systemPrint("*");
        else
            systemPrintf("%s --> ", tcpServerStateName[tcpServerState]);
    }
    tcpServerState = newState;
    if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_STATE)) && (!inMainMenu))
    {
        PERIODIC_CLEAR(PD_TCP_SERVER_STATE);
        if (newState >= TCP_SERVER_STATE_MAX)
        {
            systemPrintf("Unknown TCP Server state: %d\r\n", tcpServerState);
            reportFatalError("Unknown TCP Server state");
        }
        else
            systemPrintln(tcpServerStateName[tcpServerState]);
    }
}

// Start the TCP server
bool tcpServerStart()
{
    IPAddress localIp;

    if (settings.debugTcpServer && (!inMainMenu))
        systemPrintln("TCP server starting the server");

    // Start the TCP server
    tcpServer = new WiFiServer(settings.tcpServerPort);
    if (!tcpServer)
        return false;

    tcpServer->begin();
    online.tcpServer = true;
    localIp = wifiGetIpAddress();
    systemPrintf("TCP server online, IP address %d.%d.%d.%d:%d\r\n", localIp[0], localIp[1], localIp[2], localIp[3],
                 settings.tcpServerPort);
    return true;
}

// Stop the TCP server
void tcpServerStop()
{
    int index;

    // Notify the rest of the system that the TCP server is shutting down
    if (online.tcpServer)
    {
        // Notify the GNSS UART tasks of the TCP server shutdown
        online.tcpServer = false;
        delay(5);
    }

    // Determine if TCP server clients are active
    if (tcpServerClientConnected)
    {
        // Shutdown the TCP server client links
        for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
            tcpServerStopClient(index);
    }

    // Shutdown the TCP server
    if (tcpServer != nullptr)
    {
        // Stop the TCP server
        if (settings.debugTcpServer && (!inMainMenu))
            systemPrintln("TCP server stopping");
        tcpServer->stop();
        delete tcpServer;
        tcpServer = nullptr;
    }

    // Stop using the network
    if (tcpServerState != TCP_SERVER_STATE_OFF)
    {
        networkUserClose(NETWORK_USER_TCP_SERVER);

        // The TCP server is now off
        tcpServerSetState(TCP_SERVER_STATE_OFF);
        tcpServerTimer = millis();
    }
}

// Stop the TCP server client
void tcpServerStopClient(int index)
{
    bool connected;
    bool dataSent;

    // Done with this client connection
    if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_DATA)) && (!inMainMenu))
    {
        PERIODIC_CLEAR(PD_TCP_SERVER_DATA);

        // Determine the shutdown reason
        connected = tcpServerClient[index]->connected() && (!(tcpServerClientWriteError & (1 << index)));
        dataSent =
            ((millis() - tcpServerTimer) < TCP_SERVER_CLIENT_DATA_TIMEOUT) || (tcpServerClientDataSent & (1 << index));
        if (!dataSent)
            systemPrintf("TCP Server: No data sent over %d seconds\r\n", TCP_SERVER_CLIENT_DATA_TIMEOUT / 1000);
        if (!connected)
            systemPrintf("TCP Server: Link to client broken\r\n");
        systemPrintf("TCP server client %d disconnected from %d.%d.%d.%d\r\n", index,
                     tcpServerClientIpAddress[index][0], tcpServerClientIpAddress[index][1],
                     tcpServerClientIpAddress[index][2], tcpServerClientIpAddress[index][3]);
    }

    // Shutdown the TCP server client link
    tcpServerClient[index]->stop();
    tcpServerClientConnected &= ~(1 << index);
    tcpServerClientWriteError &= ~(1 << index);
}

// Update the TCP server state
void tcpServerUpdate()
{
    bool connected;
    bool dataSent;
    int index;
    IPAddress ipAddress;

    // Shutdown the TCP server when the mode or setting changes
    DMW_st(tcpServerSetState, tcpServerState);
    if (NEQ_RTK_MODE(tcpServerMode) || (!settings.enableTcpServer))
    {
        if (tcpServerState > TCP_SERVER_STATE_OFF)
            tcpServerStop();
    }

    /*
        TCP Server state machine

                .---------------->TCP_SERVER_STATE_OFF
                |                           |
                | tcpServerStop             | settings.enableTcpServer
                |                           |
                |                           V
                +<----------TCP_SERVER_STATE_NETWORK_STARTED
                ^                           |
                |                           | networkUserConnected
                |                           |
                |                           V
                +<--------------TCP_SERVER_STATE_RUNNING
                ^                           |
                |                           | network failure
                |                           |
                |                           V
                '-----------TCP_SERVER_STATE_WAIT_NO_CLIENTS
    */

    switch (tcpServerState)
    {
    default:
        break;

    // Wait until the TCP server is enabled
    case TCP_SERVER_STATE_OFF:
        // Determine if the TCP server should be running
        if (EQ_RTK_MODE(tcpServerMode) && settings.enableTcpServer && (!wifiIsConnected()))
        {
            if (networkUserOpen(NETWORK_USER_TCP_SERVER, NETWORK_TYPE_ACTIVE))
            {
                if (settings.debugTcpServer && (!inMainMenu))
                    systemPrintln("TCP server starting the network");
                tcpServerSetState(TCP_SERVER_STATE_NETWORK_STARTED);
            }
        }
        break;

    // Wait until the network is connected
    case TCP_SERVER_STATE_NETWORK_STARTED:
        // Determine if the network has failed
        if (networkIsShuttingDown(NETWORK_USER_TCP_SERVER))
            // Failed to connect to to the network, attempt to restart the network
            tcpServerStop();

        // Wait for the network to connect to the media
        else if (networkUserConnected(NETWORK_USER_TCP_SERVER))
        {
            // Delay before starting the TCP server
            if ((millis() - tcpServerTimer) >= (1 * 1000))
            {
                // The network type and host provide a valid configuration
                tcpServerTimer = millis();

                // Start the TCP server
                if (tcpServerStart())
                    tcpServerSetState(TCP_SERVER_STATE_RUNNING);
            }
        }
        break;

    // Handle client connections and link failures
    case TCP_SERVER_STATE_RUNNING:
        // Determine if the network has failed
        if ((!settings.enableTcpServer) || networkIsShuttingDown(NETWORK_USER_TCP_SERVER))
        {
            if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_DATA)) && (!inMainMenu))
            {
                PERIODIC_CLEAR(PD_TCP_SERVER_DATA);
                systemPrintln("TCP server initiating shutdown");
            }

            // Network connection failed, attempt to restart the network
            tcpServerStop();
            break;
        }

        // Walk the list of TCP server clients
        for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
        {
            // Determine if the client data structure is in use
            if (tcpServerClientConnected & (1 << index))
            {
                // Data structure in use
                // Check for a working TCP server client connection
                connected = tcpServerClient[index]->connected();
                dataSent = ((millis() - tcpServerTimer) < TCP_SERVER_CLIENT_DATA_TIMEOUT) ||
                           (tcpServerClientDataSent & (1 << index));
                if (connected && dataSent)
                {
                    // Display this client connection
                    if (PERIODIC_DISPLAY(PD_TCP_SERVER_DATA) && (!inMainMenu))
                    {
                        PERIODIC_CLEAR(PD_TCP_SERVER_DATA);
                        systemPrintf("TCP server client %d connected to %d.%d.%d.%d\r\n", index,
                                     tcpServerClientIpAddress[index][0], tcpServerClientIpAddress[index][1],
                                     tcpServerClientIpAddress[index][2], tcpServerClientIpAddress[index][3]);
                    }
                }

                // Shutdown the TCP server client link
                else
                    tcpServerStopClient(index);
            }
        }

        // Walk the list of TCP server clients
        for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
        {
            // Determine if the client data structure is in use
            if (!(tcpServerClientConnected & (1 << index)))
            {
                WiFiClient client;

                // Data structure not in use
                // Check for another TCP server client
                client = tcpServer->available();

                // Done if no TCP server client found
                if (!client)
                    break;

                // Start processing the new TCP server client connection
                tcpServerClient[index] = new NetworkWiFiClient(client);
                tcpServerClientIpAddress[index] = tcpServerClient[index]->remoteIP();
                tcpServerClientConnected |= 1 << index;
                tcpServerClientDataSent |= 1 << index;
                if ((settings.debugTcpServer || PERIODIC_DISPLAY(PD_TCP_SERVER_DATA)) && (!inMainMenu))
                {
                    PERIODIC_CLEAR(PD_TCP_SERVER_DATA);
                    systemPrintf("TCP server client %d connected to %d.%d.%d.%d\r\n", index,
                                 tcpServerClientIpAddress[index][0], tcpServerClientIpAddress[index][1],
                                 tcpServerClientIpAddress[index][2], tcpServerClientIpAddress[index][3]);
                }
            }
        }

        // Check for data moving across the connections
        if ((millis() - tcpServerTimer) >= TCP_SERVER_CLIENT_DATA_TIMEOUT)
        {
            // Restart the data verification
            tcpServerTimer = millis();
            tcpServerClientDataSent = 0;
        }
        break;
    }

    // Periodically display the TCP state
    if (PERIODIC_DISPLAY(PD_TCP_SERVER_STATE) && (!inMainMenu))
        tcpServerSetState(tcpServerState);
}

// Verify the TCP server tables
void tcpServerValidateTables()
{
    char line[128];

    // Verify TCP_SERVER_MAX_CLIENTS
    if ((sizeof(tcpServerClientConnected) * 8) < TCP_SERVER_MAX_CLIENTS)
    {
        snprintf(line, sizeof(line),
                 "Please set TCP_SERVER_MAX_CLIENTS <= %d or increase size of tcpServerClientConnected",
                 sizeof(tcpServerClientConnected) * 8);
        reportFatalError(line);
    }
    if (tcpServerStateNameEntries != TCP_SERVER_STATE_MAX)
        reportFatalError("Fix tcpServerStateNameEntries to match tcpServerStates");
}

// Zero the TCP server client tails
void tcpServerZeroTail()
{
    int index;

    for (index = 0; index < TCP_SERVER_MAX_CLIENTS; index++)
        tcpServerClientTails[index] = 0;
}

#endif // COMPILE_WIFI
