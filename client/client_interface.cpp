#include "client_interface.h"

int ClientLib::chubby_init(char *config_file) {

  std::ifstream file(config_file);
  if (!file.is_open()) {
    std::cerr << "Unable to open config file: " << config_file << std::endl;
    return -1;
  }
  std::string server_address;
  int num_servers_successful = 0;
  while (std::getline(file, server_address)) {
    if (!server_address.empty()) {
      chubby_cells.insert(server_address);
      chubby_map[server_address] = nullptr;
    }
  }
  file.close();

  if (chubby_cells.empty()) {
    std::cerr << "No valid servers found in config file\n";
    return -1;
  }

  for (const auto &address : chubby_cells) {
    auto channel =
        grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    chubby_map[address] = chubby::Chubby::NewStub(channel);
    if (!chubby_map[address]) {
      std::cerr << "Failed to create gRPC stub\n";
    }
    grpc_connectivity_state state = channel->GetState(true);
    if (state == GRPC_CHANNEL_SHUTDOWN ||
        state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
      std::cerr << "Failed to establish gRPC channel connection\n";
    } else {
      num_servers_successful++;
    }
  }
  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  client_lease_timeout = current_time * 2;
  if (num_servers_successful == 0)
    return -1;
  else
    return 0;
}

int ClientLib::chubby_shutdown(void) {
  for (const auto &address : chubby_cells) {
    chubby_map[address].reset();
  }
  chubby_cells.clear();
  return 0;
}

std::string ClientLib::chubby_cell_handling_request_finder() {
  return "127.0.0.1:50051";
  std::string chubby_cell_handling_request;
  Empty ping_request;
  Empty ping_response;
  int num_tries = 0;

  if (current_leader != "UNKNOWN") {

    ClientContext context;
    Status status = chubby_map[current_leader]->Ping(&context, ping_request,
                                                     &ping_response);
    if (status.ok())
      return current_leader;
  }

  std::vector<std::string> server_ports(connection_try_limit);
  auto gen = std::mt19937{std::random_device{}()};
  std::ranges::sample(chubby_cells.begin(), chubby_cells.end(),
                      server_ports.begin(), connection_try_limit, gen);
  std::ranges::shuffle(server_ports, gen);

  for (std::string &server_address : server_ports) {

    if (!chubby_map[server_address]) {
      std::cerr << "Client not initialized in kv739_get, call chubby_init\n";
      return "";
    }

    ClientContext context;
    Status status = chubby_map[server_address]->Ping(&context, ping_request,
                                                     &ping_response);

    num_tries++;

    if (!status.ok()) {
      std::cerr << "Failed for server " << server_address << std::endl;
      if (num_tries ==
          connection_try_limit) { // Set connection try limit to 3 or something
        std::cerr << "Server Get Connection retry limit reached, Aborting "
                     "client request"
                  << std::endl;
        return "";
      }
    } else {
      chubby_cell_handling_request = server_address;
      break;
    }
  }

  return chubby_cell_handling_request;
}

int ClientLib::chubby_lock(const std::string &path,
                           const std::string &locking_mode) {

  std::string chubby_cell_handling_request;
  AcquireLockRequest acquire_request;
  AcquireLockResponse acquire_response;
  ClientContext context;

  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (current_time < client_lease_timeout) {

    chubby_cell_handling_request = chubby_cell_handling_request_finder();

    if (chubby_cell_handling_request == "")
      return -3;

    ClientContext context;
    // context.set_deadline(std::chrono::time_point<std::chrono::system_clock>(
    //     std::chrono::milliseconds(client_lease_timeout)));
    acquire_request.set_client_id(
        std::to_string(this->client_id)); // Have to take a lock here
    acquire_request.set_path(path);
    acquire_request.set_locking_mode(locking_mode);

    Status status = chubby_map[chubby_cell_handling_request]->AcquireLock(
        &context, acquire_request, &acquire_response);

    if (status.ok()) {
      std::cout << "CHUBBY LOCK : Request went through, master is alive"
                << std::endl;
      leader_update_mutex.lock();
      current_leader = acquire_response.current_leader();
      leader_update_mutex.unlock();

      if (acquire_response.success()) {
        std::cout << "CHUBBY UNLOCK : Lock acquired by client with id = "
                  << client_id << std::endl;
      } else {
        std::cout
            << "CHUBBY UNLOCK : Lock could not be acquired by client with id = "
            << client_id << " due to " << acquire_response.error_message()
            << std::endl;
        return -1;
      }
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      // Now entered the jeopardy phase
      std::cout << "CHUBBY LOCK : Entered Grace Period" << std::endl;

      uint64_t current_time =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();

      if (current_time < client_lease_timeout + grace_period) {
        chubby_cell_handling_request = chubby_cell_handling_request_finder();

        if (chubby_cell_handling_request == "")
          return -3;

        ClientContext context;
        context.set_deadline(std::chrono::time_point<std::chrono::system_clock>(
            std::chrono::milliseconds(grace_period)));
        acquire_request.set_client_id(std::to_string(this->client_id));
        acquire_request.set_path(path);
        acquire_request.set_locking_mode(locking_mode);

        Status status = chubby_map[chubby_cell_handling_request]->AcquireLock(
            &context, acquire_request, &acquire_response);

        if (status.ok()) {
          std::cout << "CHUBBY LOCK Request went through, master is alive"
                    << std::endl;
          leader_update_mutex.lock();
          current_leader = acquire_response.current_leader();
          leader_update_mutex.unlock();

          if (acquire_response.success()) {
            std::cout << "CHUBBY LOCK : Lock acquired by client with id = "
                      << client_id << std::endl;
          } else {
            std::cout << "CHUBBY LOCK : Lock could not be acquired by client "
                         "with id = "
                      << client_id << "due to "
                      << acquire_response.error_message() << std::endl;
            return -1;
          }
        } else {
          std::cerr
              << "CHUBBY LOCK : Could not release lock even till grace period"
              << std::endl;
        }
      }
    }
  } else {
    std::cerr
        << "CHUBBY LOCK : Can't acquire lock, client does not have a lease"
        << std::endl;
    return -2;
  }

  return 0;
}

int ClientLib::chubby_unlock(const std::string &path) {

  std::string chubby_cell_handling_request;
  ReleaseLockRequest release_request;
  ReleaseLockResponse release_response;
  ClientContext context;

  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (current_time < client_lease_timeout) {

    chubby_cell_handling_request = chubby_cell_handling_request_finder();

    if (chubby_cell_handling_request == "")
      return -3;

    ClientContext context;
    context.set_deadline(std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::milliseconds(client_lease_timeout)));
    release_request.set_client_id(std::to_string(this->client_id));
    release_request.set_path(path);

    Status status = chubby_map[chubby_cell_handling_request]->ReleaseLock(
        &context, release_request, &release_response);

    if (status.ok()) {

      std::cout << "CHUBBY UNLOCK : Request went through, master is alive"
                << std::endl;
      leader_update_mutex.lock();
      current_leader = release_response.current_leader();
      leader_update_mutex.unlock();

      if (release_response.success()) {
        std::cout << "CHUBBY UNLOCK : Lock released by client with id = "
                  << client_id << std::endl;
      } else {
        std::cout
            << "CHUBBY UNLOCK : Lock could not be released by client with id = "
            << client_id << "due to " << release_response.error_message()
            << std::endl;
        return -1;
      }

    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {

      std::cout << "CHUBBY UNLOCK : Entered Grace Period" << std::endl;

      uint64_t current_time =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();

      if (current_time < client_lease_timeout + grace_period) {
        chubby_cell_handling_request = chubby_cell_handling_request_finder();

        if (chubby_cell_handling_request == "")
          return -3;

        ClientContext context;
        // context.set_deadline(std::chrono::time_point<std::chrono::system_clock>(
        //     std::chrono::milliseconds(grace_period)));
        release_request.set_client_id(std::to_string(this->client_id));
        release_request.set_path(path);

        Status status = chubby_map[chubby_cell_handling_request]->ReleaseLock(
            &context, release_request, &release_response);

        if (status.ok()) {
          std::cout << "CHUBBY UNLOCK : Request went through, master is alive"
                    << std::endl;
          leader_update_mutex.lock();
          current_leader = release_response.current_leader();
          leader_update_mutex.unlock();
          if (release_response.success()) {
            std::cout << "CHUBBY UNLOCK : Lock released by client with id = "
                      << client_id << std::endl;
          } else {
            std::cout << "CHUBBY UNLOCK : Lock could not be released by client "
                         "with id = "
                      << client_id << "due to "
                      << release_response.error_message() << std::endl;
            return -1;
          }
        } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
          std::cerr
              << "CHUBBY UNLOCK : Could not release lock even till grace period"
              << std::endl;
          return -1;
        }
      } else {
        std::cerr
            << "CHUBBY UNLOCK : Could not release lock even till grace period"
            << std::endl;
        return -1;
      }
    }
  } else {
    std::cerr
        << "CHUBBY UNLOCK : Can't release lock, client does not have a lease"
        << std::endl;
    return -2;
  }

  return 0;
}

int ClientLib::send_keep_alive() {

  std::string chubby_cell_handling_request;

  ClientContext context;
  KeepAliveRequest keep_alive_request;
  KeepAliveResponse keep_alive_response;

  chubby_cell_handling_request = chubby_cell_handling_request_finder();
  keep_alive_request.set_client_id(std::to_string(this->client_id));

  Status status = chubby_map[chubby_cell_handling_request]->KeepAlive(
      &context, keep_alive_request, &keep_alive_response);

  if (status.ok() && keep_alive_response.success()) {
    client_lease_timeout = keep_alive_response.lease_timeout();
    return 0;
  } else {
    return -1;
  }
}

int main(int argc, char **argv) { return 0; }