#ifndef KV739CLIENT_H
#define KV739CLIENT_H

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_set>
#include <map>

class ClientLib {
  std::unordered_set<std::string> chubby_cells;
  std::map<std::string, std::unique_ptr<chubby::Chubby::Stub>> chubby_map;
  std::string current_leader = "UNKNOWN";
  uint64_t client_lease_timeout = 0;
  uint64_t grace_period = std::chrono::seconds(45).count();
  int connection_try_limit = 5;
  std::mutex leader_update_mutex;
  std::mutex client_timeout_mutex;
  int latest_epoch_number = 0;
  int client_id;

  std::string chubby_cell_handling_request_finder();
  void set_client_id(int client_id) { this->client_id = client_id; };

public:
  int chubby_init(char *config_file);

  int chubby_shutdown();

  int chubby_open();

  int chubby_close();

  int chubby_lock(const std::string &path, const std::string &locking_mode);

  int chubby_unlock(const std::string &path);

  int send_keep_alive();
};

#endif