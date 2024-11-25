#include "client_interface.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <iomanip>
#include <unordered_map>

int main(int argc, char* argv[]){
    ClientLib client_lib;

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }

    std::string config_file = argv[1];

    if (client_lib.chubby_init(config_file.data()) != 0) {
        std::cerr << "Failed to initialize client." << std::endl;
        return -1;
    }

    //Do some Acquire and Release lock sequence here

    client_lib.chubby_shutdown();
    return 0;
}