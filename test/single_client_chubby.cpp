#include "client_interface.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>

int main(int argc, char *argv[]) {
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

  // Do some Acquire and Release lock sequence here
  client_lib.set_client_id(123);
  client_lib.send_keep_alive();

  int status = client_lib.chubby_lock("/usr/aditya/xyz", "SHARED");
  if (status != 0) {
    std::cout << "Failure 1!" << std::endl;
  }

  status = client_lib.chubby_lock("/usr/aditya/abc", "EXCLUSIVE");
  if (status != 0) {
    std::cout << "Failure 2!" << std::endl;
  }

  status = client_lib.chubby_lock("/usr/aditya/xyz", "EXCLUSIVE");
  if (status == 0) {
    std::cout << "Failure 3!" << std::endl;
  }

  status = client_lib.chubby_unlock("/usr/aditya/xyz");
  if (status != 0) {
    std::cout << "Failure 4!" << std::endl;
  }

  status = client_lib.chubby_lock("/usr/aditya/xyz", "EXCLUSIVE");
  if (status != 0) {
    std::cout << "Failure 5!" << std::endl;
  }

  client_lib.chubby_shutdown();
  return 0;
}