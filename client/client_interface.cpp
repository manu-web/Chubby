#include "client_interface.h"

#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using chubby::AcquireLockRequest;
using chubby::AcquireLockResponse;
using chubby::Chubby;
using chubby::Empty;
using chubby::KeepAliveRequest;
using chubby::KeepAliveResponse;
using chubby::ReleaseLockRequest;
using chubby::ReleaseLockResponse;
using chubby::TryAcquireLockRequest;
using chubby::TryAcquireLockResponse;


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

int ClientLib::chubby_open() {
  std::random_device entropy_source;
  std::mt19937_64 generator(entropy_source());
  std::uniform_int_distribution<int> client_id_distrib(0, INT_MAX);
  set_client_id(client_id_distrib(generator));

  uint64_t current_time =
    std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
  
  client_lease_timeout = current_time + lease_timeout;

  return send_keep_alive();
}

int ClientLib::chubby_close() { return 0; }

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

  if(!accept_requests){
    std::cout << "Cannot accept lock requests, in jeopardy phase" << std::endl;
    return -4;
  }

  std::string chubby_cell_handling_request;
  AcquireLockRequest acquire_request;
  AcquireLockResponse acquire_response;
  ClientContext context;

  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (current_time < client_lease_timeout) {

    chubby_cell_handling_request = current_leader;

    ClientContext context;
    acquire_request.set_client_id(
        std::to_string(this->client_id)); // Have to take a lock here
    acquire_request.set_path(path);
    acquire_request.set_locking_mode(locking_mode);

    std::cout<< "Current leader at client side : " << current_leader << std::endl;
    Status status = chubby_map[chubby_cell_handling_request]->AcquireLock(
        &context, acquire_request, &acquire_response);

    if (status.ok()) {
      std::cout << "CHUBBY LOCK : Request went through, master is alive"
                << std::endl;
      leader_update_mutex.lock();
      current_leader = acquire_response.current_leader();
      leader_update_mutex.unlock();

      if (acquire_response.success()) {
        std::cout << "CHUBBY LOCK : Lock acquired by client with id = "
                  << client_id << " with view number: " << acquire_response.current_leader()<< std::endl;
      } else {
        std::cout
            << "CHUBBY LOCK : Lock could not be acquired by client with id = "
            << client_id << " due to " << acquire_response.error_message()
            << acquire_response.current_leader()<< std::endl;
        return -1;
      }
    } else {
      // Now entered the jeopardy phase
      std::cout << "CHUBBY LOCK : Lock could not be acquired by client with id = " << client_id << " as the master is dead or unresponse" << std::endl;
      return -2;
    }
  } else {
    std::cout << "CHUBBY LOCK : Lock could not be acquired by client with id = " << client_id << " as the client lease has expired" << std::endl;
    return -3;
  }

  return 0;
}

int ClientLib::chubby_unlock(const std::string &path) {

  if(!accept_requests){
    std::cout << "Cannot accept unlock requests, in jeopardy phase" << std::endl;
    return -4;
  }

  std::string chubby_cell_handling_request;
  ReleaseLockRequest release_request;
  ReleaseLockResponse release_response;
  ClientContext context;

  uint64_t current_time =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  if (current_time < client_lease_timeout) {

    chubby_cell_handling_request = current_leader;

    ClientContext context;
    // context.set_deadline(std::chrono::time_point<std::chrono::system_clock>(
    //     std::chrono::milliseconds(client_lease_timeout)));
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
        std::cout << "CHUBBY UNLOCK : Lock could not be released by client with id = "
            << client_id << "due to either master being dead or unresponsive"
            << std::endl;
        return -2;
    }
  } else {
    std::cout << "CHUBBY UNLOCK : Lock could not be released by client with id = "
            << client_id << "as client lease has expired"
            << std::endl;
    return -3;
  }

  return 0;
}

int ClientLib::send_keep_alive() {

    std::string chubby_cell_handling_request;

    KeepAliveRequest keep_alive_request;
    KeepAliveResponse keep_alive_response;

    uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    
    while (current_time < client_lease_timeout) {
        keep_alive_request.set_client_id(std::to_string(this->client_id));
        keep_alive_request.set_epoch_number(this->latest_epoch_number);

        ClientContext context;
        // context.set_deadline(std::chrono::system_clock::time_point(std::chrono::seconds(client_lease_timeout)));

        Status status = chubby_map[current_leader]->KeepAlive(
            &context, keep_alive_request, &keep_alive_response);

        if (status.ok()) {
            if (keep_alive_response.success()) {
                if (keep_alive_response.epoch_number() > this->latest_epoch_number) {
                    this->latest_epoch_number = keep_alive_response.epoch_number();
                    continue;  
                }

                client_lease_timeout = keep_alive_response.lease_timeout();
                accept_requests = true;
                return 0;  
            }
        } else{
            std::cout << "KEEP ALIVE : Error, retrying" << std::endl;
            sleep(1);
        }

        current_time = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    }

    current_time = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();

    while (current_time < client_lease_timeout + grace_period) {

        accept_requests = false;

        std::vector<std::string> server_ports(connection_try_limit);
        auto gen = std::mt19937{std::random_device{}()};
        std::ranges::sample(chubby_cells.begin(), chubby_cells.end(),
                      server_ports.begin(), connection_try_limit, gen);
        std::ranges::shuffle(server_ports, gen);

        // chubby_cell_handling_request = chubby_cell_handling_request_finder();
        chubby_cell_handling_request = server_ports[0];

        if (chubby_cell_handling_request.empty()) {
            return -2;  
        }

        keep_alive_request.set_client_id(std::to_string(this->client_id));
        keep_alive_request.set_epoch_number(this->latest_epoch_number);

        std::cout<<"Sending keep alive";

        ClientContext context;

        Status status = chubby_map[chubby_cell_handling_request]->KeepAlive(
            &context, keep_alive_request, &keep_alive_response);

        if (status.ok() && keep_alive_response.success()) {
            if (keep_alive_response.epoch_number() > this->latest_epoch_number) {
                this->latest_epoch_number = keep_alive_response.epoch_number();
                continue;  
            }

            client_timeout_mutex.lock();
            client_lease_timeout = keep_alive_response.lease_timeout();
            client_timeout_mutex.unlock();

            leader_update_mutex.lock();
            current_leader = chubby_cell_handling_request;
            leader_update_mutex.unlock();

            accept_requests = true;

            return 0;  
        }else{
            std::cout << "KEEP ALIVE : Error, retrying" << std::endl;
            sleep(1);
        }

        current_time = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    }

    return -1;  
}

int main(int argc, char **argv) { return 0; }