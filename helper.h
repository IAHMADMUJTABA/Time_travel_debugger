#pragma once



#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>

using namespace std;


const uint8_t status_success = 0;

const uint8_t status_error = 1;

const int checksum_size = 4;

const int buffer_size = 1024 * 64;

const int max_variables_per_frame = 16;

const int max_stack_size = 64;





// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

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


struct variable {


    int32_t variable_value;

    string variable_name;

};

struct frame {

    string function_name;

    int32_t argument_count;

    int32_t local_variable_count;

    int32_t local_variables[max_variables_per_frame];

    variable argument_list[max_variables_per_frame];

    int32_t return_line_number;


};

struct snapshot {

    int32_t current_stack_depth;

    frame call_stack_frames[max_stack_size];

};

struct TTDB_header {

    char identifier[4];

    int32_t format_version;

    int32_t total_step_count;

    int64_t dense_index_file_offset;


};

struct client_info {
    string client_ip_address;

    int client_port_number;

    time_t queue_arrival_time;

    int client_socket_descriptor;
};

inline uint32_t compute_crc(const void* input_buffer, size_t total_bytes, uint32_t previous_check_sum = 0) {

    const uint8_t* data_bytes = (const uint8_t*)input_buffer;

    uint32_t remainder = ~previous_check_sum;

    for (size_t byte_iterator = 0; byte_iterator < total_bytes; byte_iterator++) {


        remainder = remainder ^ data_bytes[byte_iterator];

        for (int bit = 0; bit < 8; bit++) {

            if (remainder & 1) {
               
                remainder = (remainder >> 1) ^ 0xEDB88320;
            }
            else {
                
                remainder = remainder >> 1;
            }
        }

    }
    return ~remainder;
}

inline bool send_all(int socket, const char* data, size_t total_size) {



    size_t sent_so_far = 0;


    while (sent_so_far < total_size) {

        size_t remaining_bytes = total_size - sent_so_far;

        const char* current_pos = data + sent_so_far;

        size_t sent_now = send(socket, current_pos, remaining_bytes, MSG_NOSIGNAL);

        if(sent_now <= 0) {
            return false;
        }

        sent_so_far = sent_so_far + sent_now;

    }

    return true;

}

inline bool receive_all(int socket, char* buffer, size_t total_size) {

    size_t received_so_far = 0;

    while (received_so_far < total_size) {

        size_t remaining_bytes = total_size - received_so_far;

        char* currentPosition = buffer + received_so_far;

        size_t received_now = recv(socket, currentPosition, remaining_bytes, 0);

        if (received_now <= 0) {
            return false;
        }
        received_so_far = received_so_far + received_now;
    }

    return true;
}



class TrailerChecker {
private:
    vector<char> waiting_room;   
    uint32_t running_checksum;

public:
    TrailerChecker() {
        running_checksum = 0;
    }

    void feed(const char* new_data, size_t length, vector<char>& file) {

        for (size_t i = 0; i < length; i++) {

            waiting_room.push_back(new_data[i]);

            if (waiting_room.size() > 4) {

                char confirm_data = waiting_room.front();

                waiting_room.erase(waiting_room.begin());

                running_checksum = compute_crc(&confirm_data, 1, running_checksum);

                file.push_back(confirm_data);
            }
        }
    }

    bool verify() {

        if (waiting_room.size() != 4) {
            return false;
        }

        uint32_t receivedChecksum = 0;
        memcpy(&receivedChecksum, waiting_room.data(), 4);

        return (running_checksum == receivedChecksum);
    }
};