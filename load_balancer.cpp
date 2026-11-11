// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"

using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
        Node(T val) { 
            data = val; 
            next = nullptr;
        }
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {

    }
    void enqueue(const T &val)
    {
        Node* temp = new Node(val);
        temp->next = nullptr;

        if (rear == nullptr) {
            front = rear = temp;
            count++;
        }

        else {


            rear->next = temp;
            rear = temp;
            count++;
        }
        

    }
    T dequeue()
    {
        if (isEmpty()) {
           return T();
        }
        Node* temp = front;

        T val = temp->data;

        front = front->next;

        if (front == nullptr) {
            rear = nullptr;
        }

        delete temp;
        count--;

        return val;

    }
    bool isEmpty()
    {
        return count == 0;
        
    }
    int size()
    {
        return count;
    }
};



// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<client_info> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    return send_all(sock, buf, n);
}
bool sendError(int sock, const string &message)
{
    vector<char> payload;

    payload.push_back(1);

    int s = message.size();

    for (int i = 0; i < s;i++) {
        payload.push_back(message[i]);
    }
    uint32_t check_sum = compute_crc(payload.data(), payload.size());

    send_all(sock, payload.data(), payload.size());

    send_all(sock, (const char*)&check_sum, sizeof(check_sum));

    return true;
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
}
int createListenSocket(int port)
{ 
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.

   
    int main_socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (main_socket < 0) {
        cout << "Error: Main door could not be created!" << endl;
        exit(1); 
    }

  
    int allow_reuse = 1;
    if (setsockopt(main_socket, SOL_SOCKET, SO_REUSEADDR, &allow_reuse, sizeof(allow_reuse)) < 0) {
        cout << "Error: Could not allow port reuse!" << endl;
        exit(1);
    }

    sockaddr_in address_settings{};

    address_settings.sin_family = AF_INET;

    address_settings.sin_addr.s_addr = INADDR_ANY;

    address_settings.sin_port = htons(port);

    
    if (bind(main_socket, (struct sockaddr*)&address_settings, sizeof(address_settings)) < 0) {
        cout << "Error: Could not bind to port " << port << endl;
        exit(1);
    }

    if (listen(main_socket, LISTEN_BACKLOG) < 0) {
        cout << "Error: Failed to start listening!" << endl;
        exit(1);
    }

    return main_socket;


}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 


    int client_socket = socket(AF_INET, SOCK_STREAM ,0);

    if (client_socket < 0) {
        return -1;
    }

    sockaddr_in server_address{};

    server_address.sin_family = AF_INET;

    server_address.sin_port = htons(port);

    inet_pton(AF_INET, host, &server_address.sin_addr);

    int connect_result = connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address));

    if (connect_result < 0) {
        close(client_socket);
        return -1;
    }

    return client_socket;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.

    string terminal_cmd = string(SERVER_PATH) + " " + to_string(port) + " &";

    int execution_result = system(terminal_cmd.c_str());

    if (execution_result == 0) {
        return true;
    }
    else {
        return false;
    }




}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.

    int server_socket = connectToServer(serverHost.c_str(), serverPort);

    if (server_socket != -1) {
        return server_socket;
    }


    if (startServer(serverPort) == false) {
        return -1;
    }

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {

        usleep(STARTUP_POLL_INTERVAL_MS * 1000);

        server_socket = connectToServer(serverHost.c_str(), serverPort);

        if (server_socket != -1) {
            return server_socket;
        }
    }

    return -1;

}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(client_info &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.



    int server_socket = connectToServerWithWakeup();

    if (server_socket == -1) {
        sendError(c.client_socket_descriptor, "backend server failed to start");
        close(c.client_socket_descriptor);
        return;
    }


    char buffer[IO_BUFFER_SIZE];
    size_t bytes_read;

    while ((bytes_read = recv(c.client_socket_descriptor, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(server_socket, buffer, bytes_read)) {
            break; 
        }
    }

    shutdown(server_socket, SHUT_WR);


    while ((bytes_read = recv(server_socket, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(c.client_socket_descriptor, buffer, bytes_read)) {
            break;
        }
    }
    close(server_socket);
    close(c.client_socket_descriptor);

}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).

    while (flag) {

        sockaddr_in incoming_client_address{};
        socklen_t address_size = sizeof(incoming_client_address);

        int connected_client_socket = accept(listenSock, (struct sockaddr*)&incoming_client_address, &address_size);

        if (connected_client_socket < 0) {
            continue;
        }

        queueMutex.lock();

        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {

            queueMutex.unlock();

            sendError(connected_client_socket, "server busy, try again later");
            close(connected_client_socket);
        }

        else {
            client_info new_client_ticket;

            new_client_ticket.client_socket_descriptor = connected_client_socket;
            new_client_ticket.client_ip_address = inet_ntoa(incoming_client_address.sin_addr);

            new_client_ticket.client_port_number = ntohs(incoming_client_address.sin_port);
            new_client_ticket.queue_arrival_time = time(nullptr); 

            
            pendingClients.enqueue(new_client_ticket);

            queueMutex.unlock();


        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.


    while (flag) {

        bool has_client_to_serve = false;

        client_info client_to_serve;



        queueMutex.lock();


        if (!pendingClients.isEmpty()) {

            client_to_serve = pendingClients.dequeue();
            has_client_to_serve = true;

        }

        queueMutex.unlock();

        if (has_client_to_serve) {


            time_t now = time(nullptr);

            int wait_time = now - client_to_serve.queue_arrival_time;

            if (wait_time > 30) {
                close(client_to_serve.client_socket_descriptor);
            }
            else {

                relayOneClient(client_to_serve);
            }


        }
       
        else {

            usleep(10000);
        }


    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}


int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().


    int main_socket = createListenSocket(lbPort);

    cout << " Load Balancer is running on port " << lbPort << endl;
    cout << " Press Ctrl+C to stop safely." << endl;

    thread receptionistThread(acceptLoop, main_socket);


    receptionistThread.detach();
    dispatcherLoop();

    close(main_socket);


    cout << "Load Balancer successfully closed." << endl;

    return 0;
}