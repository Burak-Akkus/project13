
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// XDCtools Header files
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>

/* TI-RTOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/net/http/httpcli.h>

#include "Board.h"

#include <sys/socket.h>
#include <arpa/inet.h>

#define HOSTNAME          "api.openweathermap.org"
#define REQUEST_URI       "/data/2.5/forecast/?id=315202&APPID=96c05b3c27bdc0565b8287d29e6f8ac7"
#define USER_AGENT        "HTTPCli (ARM; TI-RTOS)"
#define SOCKETTEST_IP     "192.168.1.109"
#define NTP_IP            "128.138.140.44"
#define TASKSTACKSIZE     4096
#define OUTGOING_PORT     5011
#define INCOMING_PORT     5030
#define NTP_PORT          37

extern Semaphore_Handle semaphore0;     // posted by httpTask and pended by clientTask
extern Semaphore_Handle semaphore1;
extern Semaphore_Handle semaphore2;
extern Event_Handle event0;
extern Swi_Handle swi0;
char   tempstr[20];                     // temperature string
char   humidstr[20];
char   tmp[] = "The temperature in Eskisehir is: ";
char   hmd[] = ", and the humidity is: ";
char   nl[]  = "\n";
char timeString[48];
int  convertedTime;
int ctr = 0;
/*
 *  ======== printError ========
 */

Void Timer_ISR(UArg arg1) // executed every second
{
    Swi_post(swi0);
}

Void SWI_ISR(UArg arg1)
{
    convertedTime  = timeString[0]*16777216 +  timeString[1]*65536 + timeString[2]*256 + timeString[3];
    convertedTime += 10800;
    convertedTime += ctr++;
}

void printError(char *errString, int code)
{
    System_printf("Error! code = %d, desc = %s\n", code, errString);
    BIOS_exit(code);
}

bool sendData2Server(char *serverIP, int serverPort, char *data, int size)
{
    int sockfd, connStat, numSend;
    bool retval=false;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        close(sockfd);
        return false;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);     // convert port # to network order
    inet_pton(AF_INET, serverIP, &(serverAddr.sin_addr));

    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("sendData2Server::Error while connecting to server\n");
    }
    else {
        numSend = send(sockfd, data, size, 0);       // send data to the server
        if(numSend < 0) {
            System_printf("sendData2Server::Error while sending data to server\n");
        }
        else {
            retval = true;      // we successfully sent the temperature string
        }
    }
    System_flush();
    close(sockfd);
    return retval;
}

Void clientSocketTask(UArg arg0, UArg arg1)
{
    char result[20];
    while(1) {
        // wait for the semaphore that httpTask() will signal
        // when temperature string is retrieved from api.openweathermap.org site
        //
        Semaphore_pend(semaphore0, BIOS_WAIT_FOREVER);

        GPIO_write(Board_LED0, 1); // turn on the LED

        // connect to SocketTest program on the system with given IP/port
        // send hello message whihc has a length of 5.
        //
        strcpy(result,ctime(&convertedTime));
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, result, strlen(result));
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, tmp, strlen(tmp));
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, tempstr, strlen(tempstr));
        System_printf("clientSocketTask:: Temperature is sent to the server\n");
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, hmd, strlen(hmd));
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, humidstr, strlen(humidstr));
        sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, nl, strlen(nl));
        System_printf("clientSocketTask:: Humidity is sent to the server\n");
        System_flush();

        GPIO_write(Board_LED0, 0);  // turn off the LED
    }
}

float getTemperature(void)
{
    return atof(tempstr);
}

float getHumidity(void)
{
    return atof(humidstr);
}

Void serverSocketTask(UArg arg0, UArg arg1)
{
    int serverfd, new_socket, valread, len;
    struct sockaddr_in serverAddr, clientAddr;
    float temp, humid;
    char buffer[30];
    char outstr[30], tmpstr[30];
    bool quit_protocol;

    serverfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverfd == -1) {
        System_printf("serverSocketTask::Socket not created.. quiting the task.\n");
        return;     // we just quit the tasks. nothing else to do.
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(INCOMING_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // Attaching socket to the port
    //
    if (bind(serverfd, (struct sockaddr *)&serverAddr,  sizeof(serverAddr))<0) {
         System_printf("serverSocketTask::bind failed..\n");

         // nothing else to do, since without bind nothing else works
         // we need to terminate the task
         return;
    }
    if (listen(serverfd, 3) < 0) {

        System_printf("serverSocketTask::listen() failed\n");
        // nothing else to do, since without bind nothing else works
        // we need to terminate the task
        return;
    }

    while(1) {

        len = sizeof(clientAddr);
        if ((new_socket = accept(serverfd, (struct sockaddr *)&clientAddr, &len))<0) {
            System_printf("serverSocketTask::accept() failed\n");
            continue;               // get back to the beginning of the while loop
        }

        System_printf("Accepted connection\n"); // IP address is in clientAddr.sin_addr
        System_flush();

        // task while loop
        //
        quit_protocol = false;
        do {

            // let's receive data string
            if((valread = recv(new_socket, buffer, 10, 0))<0) {

                // there is an error. Let's terminate the connection and get out of the loop
                //
                close(new_socket);
                break;
            }

            // let's truncate the received string
            //
            buffer[10]=0;
            if(valread<10) buffer[valread]=0;

            System_printf("message received: %s\n", buffer);

            if(!strcmp(buffer, "HELLO")) {
                strcpy(outstr,"GREETINGS 200\n");
                send(new_socket , outstr , strlen(outstr) , 0);
                System_printf("Server <-- GREETINGS 200\n");
            }
            else if(!strcmp(buffer, "GETHUMID")) {
                humid = getHumidity();
                sprintf(outstr, "OK %5.2f\n", humid);
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "GETTEMP")) {
                temp = getTemperature();
                sprintf(outstr, "OK %5.2f\n", temp);
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "QUIT")) {
                quit_protocol = true;     // it will allow us to get out of while loop
                strcpy(outstr, "BYE 200");
                send(new_socket , outstr , strlen(outstr) , 0);
            }

        }
        while(!quit_protocol);

        System_flush();
        close(new_socket);
    }

    close(serverfd);
    return;
}

void receiveTime(char *serverIP, int serverPort)
{
    int sockfd, connStat, numRecv;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        close(sockfd);
    }

    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);     // convert port # to network order
    inet_pton(AF_INET, serverIP, &(serverAddr.sin_addr));

    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("receiveTime::Error while connecting to server\n");
    }
    else {
        numRecv = recv(sockfd, timeString, sizeof(timeString), 0);       // send data to the server
        if(numRecv < 0) {
            System_printf("receiveTime::Error while sending data to server\n");
        }
    }
    System_flush();
    close(sockfd);

}

/*
 *  ======== httpTask ========
 *  Makes a HTTP GET request
 */
Void httpTask(UArg arg0, UArg arg1)
{
    bool moreFlag = false;
    char data[64], *s1, *s2, *s3, *s4;
    int ret, temp_received=0, humid_received=0, len;
    struct sockaddr_in addr;

    HTTPCli_Struct cli;
    HTTPCli_Field fields[3] = {
        { HTTPStd_FIELD_NAME_HOST, HOSTNAME },
        { HTTPStd_FIELD_NAME_USER_AGENT, USER_AGENT },
        { NULL, NULL }
    };

    while(1) {
        Event_pend(event0, Event_Id_00 + Event_Id_01, Event_Id_NONE, BIOS_WAIT_FOREVER);//Wait on both left and right hand movement events

        receiveTime(NTP_IP, NTP_PORT);

        System_printf("Sending a HTTP GET request to '%s'\n", HOSTNAME);
        System_flush();

        HTTPCli_construct(&cli);

        HTTPCli_setRequestFields(&cli, fields);

        ret = HTTPCli_initSockAddr((struct sockaddr *)&addr, HOSTNAME, 0);
        if (ret < 0) {
            HTTPCli_destruct(&cli);
            System_printf("httpTask: address resolution failed\n");
            continue;
        }

        ret = HTTPCli_connect(&cli, (struct sockaddr *)&addr, 0, NULL);
        if (ret < 0) {
            HTTPCli_destruct(&cli);
            System_printf("httpTask: connect failed\n");
            continue;
        }

        ret = HTTPCli_sendRequest(&cli, HTTPStd_GET, REQUEST_URI, false);
        if (ret < 0) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: send failed");
            continue;
        }

        ret = HTTPCli_getResponseStatus(&cli);
        if (ret != HTTPStd_OK) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: cannot get status");
            continue;
        }

        System_printf("HTTP Response Status Code: %d\n", ret);

        ret = HTTPCli_getResponseField(&cli, data, sizeof(data), &moreFlag);
        if (ret != HTTPCli_FIELD_ID_END) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: response field processing failed\n");
            continue;
        }

        len = 0;
        do {
            ret = HTTPCli_readResponseBody(&cli, data, sizeof(data), &moreFlag);
            if (ret < 0) {
                HTTPCli_disconnect(&cli);
                HTTPCli_destruct(&cli);
                System_printf("httpTask: response body processing failed\n");
                moreFlag = false;
            }
            else {
                // string is read correctly
                // find "temp:" string
                //
                s1=strstr(data, "temp");
                s3=strstr(data, "humidity");
                if(s1) {
                    if(temp_received) continue;     // temperature is retrieved before, continue
                        // is s1 is not null i.e. "temp" string is found
                        // search for comma
                        s2=strstr(s1, ",");
                        if(s2) {
                            *s2=0;                      // put end of string
                            strcpy(tempstr, s1+6);      // copy the string
                            temp_received = 1;
                        }
                }
                if(s3){                              // if s3 is not null, which means "humidity" string is found
                    if(humid_received) continue;     // humidity is retrieved before, continue
                        s4=strstr(s3, ",");          // search for comma since that is where the value ends
                        if(s4) {                     // if s4 is not null, which means "," is found
                            *s4=0;                   // Make the string in that address equal to 0 to put an end point
                            strcpy(humidstr, s3+10); // copy the string starting from 10 points away from s1 which corresponds to the value
                            humid_received = 1;
                        }
                }
            }

            len += ret;     // update the total string length received so far
        } while (moreFlag);

        System_printf("Received %d bytes of payload\n", len);
        System_printf("Temperature %s\n", tempstr);
        System_printf("Humidity %s\n", humidstr);
        System_flush();                                         // write logs to console

        HTTPCli_disconnect(&cli);                               // disconnect from openweathermap
        HTTPCli_destruct(&cli);
        Semaphore_post(semaphore0);                             // activate socketTask
        Semaphore_post(semaphore1);

        Task_sleep(5000);                                       // sleep 5 seconds
    }
}

Void LeftTask(UArg arg0, UArg arg1){
    /*This task should normally pend on a semaphore produced by the left reading value from paj7620 sensor
     * However in spite of all the efforts put in to reading the sensor value, there was no successful reading
     * Thus, this task assumes the reading "left" is coming from the sensor.
     * */

    while(1){
        Semaphore_pend(semaphore1, BIOS_WAIT_FOREVER); // semaphore1 has an initial value 1 to prevent the deadlock
        System_printf("Left hand movement is detected\n");
        System_flush();
        Task_sleep(3000); // this sleep is put just to make it more clear to read in console
        Event_post(event0, Event_Id_00); // notify httpTask that left hand movement arrived
        Semaphore_post(semaphore2);  // RightTask can continue to run as if paj7620 sensor has detected right hand movement
    }
}

Void RightTask(UArg arg0, UArg arg1){
    /*This task should normally pend on a semaphore produced by the "right" reading value from paj7620 sensor
     * However in spite of all the efforts put in to reading the sensor value, there was no successful reading
     * Thus, this task assumes the reading "right" is coming from the sensor.
     * */
    while(1){
        Semaphore_pend(semaphore2, BIOS_WAIT_FOREVER);
        System_printf("Right hand movement is detected\n");
        System_flush();
        Task_sleep(3000); // this sleep is put just to make it more clear to read in console
        Event_post(event0, Event_Id_01); // now that both left and right movements are detected,
    }                                    // pending event in httpTask will continue
}

bool createTasks(void)
{
    static Task_Handle taskHandle1, taskHandle2, taskHandle3;
    Task_Params taskParams;
    Error_Block eb;

    Error_init(&eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle1 = Task_create((Task_FuncPtr)httpTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle2 = Task_create((Task_FuncPtr)clientSocketTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr)serverSocketTask, &taskParams, &eb);

    if (taskHandle1 == NULL || taskHandle2 == NULL || taskHandle3 == NULL) {
        printError("netIPAddrHook: Failed to create HTTP, Socket and Server Tasks\n", -1);
        return false;
    }

    return true;
}

//  This function is called when IP Addr is added or deleted
//
void netIPAddrHook(unsigned int IPAddr, unsigned int IfIdx, unsigned int fAdd)
{
    // Create a HTTP task when the IP address is added
    if (fAdd) {
        createTasks();
    }
}

int main(void)
{
    /* Call board init functions */
    Board_initGeneral();
    Board_initGPIO();
    Board_initEMAC();

    /* Turn on user LED */
    GPIO_write(Board_LED0, Board_LED_ON);

    System_printf("Starting the HTTP GET example\nSystem provider is set to "
            "SysMin. Halt the target to view any SysMin contents in ROV.\n");
    /* SysMin will only print to the console when you call flush or exit */
    System_flush();


    /* Start BIOS */
    BIOS_start();

    return (0);
}
