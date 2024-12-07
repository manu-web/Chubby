#include "client_interface.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>

std::atomic<bool> keep_alive_running{true};

// Function to send keep-alive messages
void keep_alive_thread(ClientLib &client_lib) {
  while (true) {
    int response = client_lib.send_keep_alive();
  }
}

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
  client_lib.chubby_open();
  
  std::thread keep_alive(keep_alive_thread, std::ref(client_lib));
  keep_alive.detach();

  sleep(10);

  int status = client_lib.chubby_lock("/usr/aditya/xyz", "SHARED");
  if (status != 0) {
    std::cout << "Failure 1!" << std::endl;
  }

  status = client_lib.chubby_lock("/usr/aditya/abc", "EXCLUSIVE");
  if (status != 0) {
    std::cout << "Failure 2!" << std::endl;
  }

  sleep(10);

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

  std::cout << "Check 1" << std::endl;
  // keep_alive_running.store(false);
  std::cout << "Check 2" << std::endl;
  // keep_alive.join();
  std::cout << "Check 3" << std::endl;

  client_lib.chubby_shutdown();
  return 0;
}