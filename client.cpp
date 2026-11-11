// ======================= TIME-TRVEL DEBUGGR - CLIENT TEMPLATE =======================

// The client never talks to the server diretly. It only talks to the load balancer.
// The load balancer waks the servr up if it is down, so the client does not care.
// The first "start" my just take a bit longer.
//
// The network is usd ONLY by "start". After th .tdbg file is downloaed,
// next / prv / jmp read that file on the local disk. They never contct the load balancer.
//
// How the pieces ft together:
//   main loop -> radCommand -> runCommand
//   start -> cmdSart()  -> requestDebugFile() (Part A) -> openTdbg( (Part B) -> shows step 1
//   next, prv, jmp -> movBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <sstream>
#include <cstdint>
#include<fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"

#include <netinet/in.h>  
#include <arpa/inet.h>

using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.

  
    return receive_all(sock, buf, n);

}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    return send_all(sock, buf, n);
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.

    int network_socket = socket(AF_INET , SOCK_STREAM, 0);

    if (network_socket < 0) {
        return -1;
    }


    sockaddr_in load_balancer_address{};

    load_balancer_address.sin_family = AF_INET;

    load_balancer_address.sin_port = htons(port);

    inet_pton(AF_INET, host, &load_balancer_address.sin_addr);

    if (connect(network_socket, (struct sockaddr*)&load_balancer_address, sizeof(load_balancer_address)) < 0) {

        close(network_socket);
        return -1;
    }
    return network_socket;

}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    ifstream input_file(sourcePath, ios::binary | ios::ate);

    if (!input_file.is_open()) 
    {
        return false;
    }

    streamsize total_file_size = input_file.tellg();

    input_file.seekg(0, ios::beg);

    vector<char> file_content(total_file_size);

    input_file.read(file_content.data(), total_file_size);

    input_file.close();

    if (!sendAll(sock, file_content.data(), file_content.size())) {

        return false;
    }
    shutdown(sock, SHUT_WR);

    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    char network_buffer[IO_BUFFER_SIZE];

    ssize_t bytes_received;

    vector<char> data;           

    TrailerChecker checksum_verifier;

  
    while (true) {

        bytes_received = recv(sock, network_buffer, sizeof(network_buffer), 0);

        if (bytes_received <= 0) {
            break;
        }

        checksum_verifier.feed(network_buffer, bytes_received, data);
    }

    if (!checksum_verifier.verify()) {

        remove(outTdbgPath);

        outError = "checksum mismatch, response corrupted";
        return false;
    }

    if (data.empty()) {
        outError = "empty response";
        return false;
    }
    uint8_t status_flag = data[0];

    if (status_flag == 1) {
        outError = string(data.begin() + 1, data.end());
        return false;
    }
    else {
        ofstream output_file(outTdbgPath, ios::binary);
        if (!output_file.is_open()) {
            outError = "could not create .tdbg file";
            return false;
        }

        // Status flag (index 0) ko chhor kar, baqi ka saara data file mein write kar do
        output_file.write(data.data() + 1, data.size() - 1);
        output_file.close();

        return true;
    }
}

// =============================================================================================
// PART B: READING TE .tdbg FILE
// File layout (witten by Phase 1):
//   header | napsot 0 | snapshot 1 | ... | index (one 8 byte offet per snapshot)
// The index lets us jup straight to an stp without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{

    FILE* my_file = fopen(path, "rb");

    if (my_file == nullptr) {
        return false;
    }

    TTDBHeader header_info;

    fread(&header_info, sizeof(TTDBHeader), 1, my_file);

    if (header_info.magic[0] != 'T' || header_info.magic[1] != 'T' || header_info.magic[2] != 'D' || header_info.magic[3] != 'B') {
        fclose(my_file);
        return false;
    }

    r.header = header_info;

    fseek(my_file, header_info.indexOffset, SEEK_SET);

    r.index.resize(header_info.stepCount);

    fread(r.index.data(), sizeof(int64_t), header_info.stepCount, my_file);

    r.file_index = (ssize_t)my_file;

    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.

    if (r.file_index != 0) {

        FILE* my_file = (FILE*)r.file_index;

        fclose(my_file);

        r.file_index = 0; 
    }

    r.index.clear();
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{

    if (step < 0 || step >= r.header.stepCount) {
        return nullptr;
    }

    int64_t target_byte_address = r.index[step];


    FILE* my_file = (FILE*)r.file_index;

    fseek(my_file, target_byte_address, SEEK_SET);

    Snapshot* requested_snapshot = new Snapshot();

    fread(requested_snapshot, sizeof(Snapshot), 1, my_file);

    return requested_snapshot;

}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = false; 
    s.currentStep = 0;     
    s.reader.file_index = 0;
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.rader). Then st active = false.
    // Called when the user types quit and whn te input ends.

    if (s.active_flag) {

        closeTdbg(s.reader);

        s.active_flag = false;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.

    Snapshot* current_snapshot = readSnapshotAt(s.reader, s.currentStep);

    if (current_snapshot == nullptr) {

        cout << "damaged tdbg file" << endl;
        return false;
    }


    int32_t total_steps = s.reader.header.stepCount;

    printSnapshot(s.currentStep, total_steps, *current_snapshot);

    delete current_snapshot;

    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    int32_t target_step = s.currentStep + delta;

    int32_t max_step_index = s.reader.header.stepCount - 1;

    if (target_step < 0) {
        target_step = 0;
    }

    if (target_step > max_step_index) {
        target_step = max_step_index;
    }

    s.currentStep = target_step;

    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fals, print the errr text and return false. Keep the old sesion as it was.
    // 3. Open the new file with openTdg into a NEW local TdbgReader.
    //    If tht fails, print "server set a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if .active), then put the new readr into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show stp 1 with showCurrent(s).



    int network_socket = connectToHost(LB_HOST.c_str(), LB_PORT);

    if (network_socket < 0) {

        cout << " could not connect to load balancer." << endl;

        return false;
    }

    if (!uploadSource(network_socket, sourcePath.c_str())) {

        cout << "failed to upload source code." << endl;

        close(network_socket);

        return false;
    }

    string error_message;

    if (!receiveResponse(network_socket, TDBG_FILE, error_message)) {

        cout << error_message << endl; 

        close(network_socket);

        return false; 
    }

    close(network_socket);

    TdbgReader new_reader;

    new_reader.file_index = 0;

    if (!openTdbg(TDBG_FILE, new_reader)) {

        cout << "server sent a damaged tdbg file" << endl;
        return false;
    }

    if (s.active_flag) {

        closeTdbg(s.reader);
    }

    s.reader = new_reader;

    s.currentStep = 0;

    s.active_flag = true;

    return showCurrent(s);
}
bool cmdNext(DebugSession& s) {
    
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession& s) { 
    return moveBy(s, -1); 
}
bool cmdJmpNext(DebugSession& s, int32_t n) {
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession& s, int32_t n) { 
    return moveBy(s, -n);
}
// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "\nstep " << step << " of " << totalSteps - 1 << endl;

    for (int i = s.stackDepth - 1; i >= 0; i--) {
        const Frame& f = s.callStack[i];

        cout << f.func_name << "(";
        for (int a = 0; a < f.argc; a++) {
            cout << f.argv[a].name << "=" << f.argv[a].value;
            if (a < f.argc - 1) cout << ", ";
        }
        cout << ")" << endl;

        if (f.localCount > 0) {

            cout << "locals: ";

            for (int l = 0; l < f.localCount; l++) {

                cout << f.locals[l].name << "=" << f.locals[l].value;

                if (l < f.localCount - 1) {
                    cout << " ";
                }
            }
            cout << endl;
        }
    }
}
void printHelp()
{
    // Print the commands
    cout << "command:" << endl;
    cout << "start <file>        : start debugging" << endl;
    cout << "next         :  go forward 1 step" << endl;
    cout << "prv            : go backward 1 step" << endl;
    cout << "jmp next <n> : jump forward n steps" << endl;
    cout << "jmp prv <n>  : jump backward n steps" << endl;
    cout << "help         : show commands" << endl;
    cout << "quit / exit  : close" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.


    stringstream ss(line);
    string word;
    vector<string> words;

    while (ss >> word) {
        words.push_back(word);
    }

    if (words.empty()) {
        return true;
    }

    int start = 0;

    if (words[0] == "ttdb") {

        start = 1;
    }

    if (start >= words.size()) {

        return true;
    }

    string cmd = words[start];

    if (cmd == "quit" || cmd == "exit") {

        quitRequested = true;

        return true;
    }
    else if (cmd == "help") {

        printHelp();

        return true;
    }
    else if (cmd == "start") {

        if (start + 1 < words.size()) {

            return cmdStart(s, words[start + 1]);

        }
        else {

            cout << " missing file name" << endl;

            return false;
        }
    }


    else if (cmd == "next" && start + 1 == words.size()) {

        return cmdNext(s);
    }
    else if (cmd == "prv" && start + 1 == words.size()) {

        return cmdPrv(s);
    }
    else if (cmd == "jmp") {

        if (start + 2 < words.size()) {

            string dir = words[start + 1];

            int n = stoi(words[start + 2]);

            if (dir == "next")
            {
                return cmdJmpNext(s, n);
            }

            if (dir == "prv") {
                return cmdJmpPrv(s, n);
            }
        }
    }

    cout << "unknown command" << endl;

    printHelp();

    return false;


}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.


    if (argc >= 3) {
        LB_HOST = argv[1];
        LB_PORT = stoi(argv[2]);
    }
    else {
      
        return 1; 
    }

    
    DebugSession session;

    initSession(session);

    
    cout << "debugger ready connected to " << LB_HOST << ":" << LB_PORT << endl;
    printHelp();

    bool quit_requested = false;

    string userInput;

    while (!quit_requested) {

        cout << PROMPT; 

        if (!getline(cin, userInput)) {
            break; 
        }

        runCommand(session, userInput, quit_requested);
    }

    closeSession(session);

    return 0;




}
