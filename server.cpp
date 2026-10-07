// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
     
        count = 0;


    }
    void push(const T& val)
    {
        if (count < MAX_STACK_DEPTH){

            Node* t = new Node;
        t->data = val;
        t->next = top;
        top = t;
        count++;
        }
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if (count > 0) {
            Node* t=top;
            
            top = top->next;
            T st = t->data;
            delete t;
            count--;
            return st;

        }
        return T();
        // pop the top value on the stack
    }
    T& peek()
    {
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node * temp = top;
        int index = 0;
        while (temp!= nullptr && index < maxLen) {
            out[index++] = temp->data;
            temp = temp->next;
        }
        return index;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
    }
    TimelineNode* begin()
    {
    }
    int32_t getStepCount()
    {
    }
};

// Core structs
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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    while (getline(in,out)) {
        if (out != "") {
            return true;
        }
    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string& line)
{
    string word;
    stringstream ss(line);
    ss >> word;

    return word;
    // returns first word from the input string
}
string secondWord(const string& line)
{
    string word;
    
    stringstream ss(line);

    ss >> word;
    ss >> word;

    return word;
    // returns the second word
}
bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath);
    string out;
    int32_t count = 0;

    while (readSourceLine(in,out)) {
        if (count > 1 || count < 0) {
            return false;
        }
        if (firstWord(out) == "func") {
            count++;
        }
        if (out == "func_end") {
            count--;
        }
      
    }
    return count == 0;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

 //PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t starting_pos = ftell(f);

    int32_t string_size = text.size();

    fwrite(&offsetField, sizeof(int64_t), 1, f);

    fwrite(&string_size, sizeof(int32_t), 1, f);

    for (int i = 0; i < string_size; i++) {

        fwrite(&text[i], sizeof(char), 1, f);
    }

    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    return starting_pos;
}

int64_t readResolveRecord(FILE* f, string& outText)
{

    int64_t offset;

    int32_t string_size;

    fread(&offset, sizeof(int64_t), 1, f);
    fread(&string_size, sizeof(int32_t), 1, f);
    outText.resize(string_size);

    for (int i = 0; i < string_size; i++) {

        fread(&outText[i], sizeof(char), 1, f);
    }
    return offset;

    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}


int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream rdr(sourcePath);
    FILE* write = fopen(resolveBinPath, "wb");

    if (!rdr || !write) {
       
        return -1;
    }

    int64_t main_offset = -1;

    int64_t current_offset = 0;
    string line;
    while (readSourceLine(rdr,line)) {
        int32_t string_size = line.size();
        writeResolveRecord(write, current_offset, line);

        if (firstWord(line) == "func") {

            if (secondWord(line) == "main") {
                main_offset = current_offset;
            }
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = current_offset; 
                funcCount++;
        }
        if (firstWord(line) == "call") {
            patches[patchCount].byteOffsetOfOffsetField = current_offset;
            patches[patchCount].targetFuncName = secondWord(line);;
            patchCount++;
        }

        current_offset = current_offset + 8 + 4 + string_size;

    }

    if (main_offset == -1) {
        cout << "ERROR 404 : Main Not Found!" << endl;
        fclose(write);
        return -1;
    }

    

    for (int i = 0; i < patchCount; i++) {
        bool check = false;
        int64_t temp_offset = -1;
        for (int j = 0; j < funcCount; j++) {

            if (funcArray[j].funcName == patches[i].targetFuncName) {
                check = true;
                temp_offset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (!check) {

            cout << "Undefined Error!" << endl;
            fclose(write);
            return -1;
        }

        fseek(write, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&temp_offset, sizeof(int64_t), 1, write);

    }

    fclose(write);
    return main_offset;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};

struct Token
{
    TokenType type;
    string text;
};

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    string word;
    stringstream ss(line);
    int32_t size = 0;
    while (ss >> word) {
        if (size >= maxTokens) {
            break;
        }

        tokens[size].text = word;

        if (size == 0) {
            tokens[size].type = KEYWORD;
        }
        if (size == 1) {

            tokens[size].type = IDENTIFIER;
        }
        else {
            tokens[size].type = PARAM;
        }
        size++;
    }
    return size;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}


Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* ns = new Snapshot;

    ns->stackDepth = callStack.snapshot_into(ns->callStack, MAX_STACK_DEPTH);

    return ns;

    // build the snapshot based on the callStack given
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{

    FILE* f = fopen(resolveBinPath, "rb");
    Stack<Frame> callstack;
    int64_t offset = mainOffset;
    Frame main;

    main.argc = 0;
    main.func_name = "main";
    main.localCount =0;
    main.returnLine = -1;

    callstack.push(main);

    while (true) {

        fseek(f, offset, SEEK_SET);

        string line;

        readResolveRecord(f, line);

        Token Tokens[MAX_TOKENS];

        int64_t size = tokenizeLine(line, Tokens, MAX_TOKENS);

        if (size == 0) {

            break;
        }
        string keyword = Tokens[0].text;
        if (keyword == "set") {

            Frame topframe = callstack.pop();

            string variable_name = Tokens[1].text;
            int64_t variable_value= stoi(Tokens[2].text);

            bool ispresent = false;

            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name) {

                    topframe.locals[i].value = variable_value;
                    ispresent = true;
                    break;
                }

            }
            if (ispresent == false) {

                if (topframe.localCount < MAX_VARS_PER_FRAME) {
                    topframe.locals[topframe.localCount].name = variable_name;
                    topframe.locals[topframe.localCount].value = variable_value;
                    topframe.localCount++;
                }
              
            }
            callstack.push(topframe);
        }

        if (keyword == "add") {
            Frame topframe = callstack.pop();

            string variable_name1 = Tokens[1].text;
            string variable_name2 = Tokens[2].text;
            int64_t val2 = 0;
            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name2) {

                    val2 = topframe.locals[i].value;
                    break;
                }

            }

            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name1) {

                    topframe.locals[i].value = topframe.locals[i].value + val2;
                    break;
                }

            }

            callstack.push(topframe);
        }

        if (keyword == "sub") {
            Frame topframe = callstack.pop();

            string variable_name1 = Tokens[1].text;
            string variable_name2 = Tokens[2].text;
            int64_t val2 = 0;
            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name2) {

                    val2 = topframe.locals[i].value;
                    break;
                }

            }

            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name1) {

                    topframe.locals[i].value = topframe.locals[i].value - val2;
                    break;
                }

            }

            callstack.push(topframe);
        }

        if (keyword == "div") {
            Frame topframe = callstack.pop();

            string variable_name1 = Tokens[1].text;
            string variable_name2 = Tokens[2].text;
            int64_t val2 = 0;
            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name2) {

                    val2 = topframe.locals[i].value;
                    break;
                }

            }

            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name1) {

                    topframe.locals[i].value = topframe.locals[i].value / val2;
                    break;
                }

            }

            callstack.push(topframe);
        }

        if (keyword == "mul") {
            Frame topframe = callstack.pop();

            string variable_name1 = Tokens[1].text;
            string variable_name2 = Tokens[2].text;
            int64_t val2 = 0;
            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name2) {

                    val2 = topframe.locals[i].value;
                    break;
                }

            }

            for (int i = 0; i < topframe.localCount; i++) {

                if (topframe.locals[i].name == variable_name1) {

                    topframe.locals[i].value = topframe.locals[i].value * val2;
                    break;
                }

            }

            callstack.push(topframe);
        }


    }

    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        cout << "error";
        // send an error response instead of a .tdbg file
        return 1;
    }
    cout << "Done";
   
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    cout << "done2" << endl;
    /*
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");
    */
    return 0;
}