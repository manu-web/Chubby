#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
#include "paxos_server.h"
#include "rocksdb_wrapper.h"
#include <chrono>
#include <functional>
#include <future>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using grpc::Channel;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using chubby::AcquireLockRequest;
using chubby::AcquireLockResponse;
using chubby::Chubby;
using chubby::KeepAliveRequest;
using chubby::KeepAliveResponse;
using chubby::ReleaseLockRequest;
using chubby::ReleaseLockResponse;
using chubby::TryAcquireLockRequest;
using chubby::TryAcquireLockResponse;

class KeyLock {
  std::unordered_map<std::string, std::shared_ptr<std::mutex>> lock_map;
  std::mutex map_mutex;

public:
  void lock(std::string key) {
    std::shared_ptr<std::mutex> key_mutex;
    {
      std::lock_guard<std::mutex> lock(map_mutex);
      if (lock_map.find(key) == lock_map.end()) {
        lock_map[key] = std::make_shared<std::mutex>();
      }
      key_mutex = lock_map[key];
    }
    key_mutex->lock();
  }

  void unlock(std::string key) {
    std::shared_ptr<std::mutex> key_mutex;
    {
      std::lock_guard<std::mutex> lock(map_mutex);
      key_mutex = lock_map[key];
    }
    key_mutex->unlock();
  }
};

class ChubbyImpl final : public Chubby::Service {
private:
  KeyLock key_lock;
  PaxosImpl *paxos_service;
  std::set<std::string>
      client_session_map; // Tracks if a client currently has session with the
                          // Chubby master, need not persist, recovery through
                          // client
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      client_lease_map;
  std::mutex lease_map_mutex;
  std::condition_variable lease_cv;
  const std::chrono::seconds lease_timeout = std::chrono::seconds(12);
  std::map<std::string, std::unique_ptr<Chubby::Stub>> chubby_stubs_map;
  int first_port;
  int last_port;
  std::atomic<int> slot_number;

public:
  ChubbyImpl(PaxosImpl *paxos) {
    this->paxos_service = paxos;

    InitializeServerStubs();
  }

  void Put(const std::string &path, std::string &locking_mode) {

    std::string key_value_to_be_sent_to_paxos;

    key_value_to_be_sent_to_paxos = path + "#" + locking_mode;

    paxos_service->Start(slot_number, key_value_to_be_sent_to_paxos);
  }

  void InitializeServerStubs() {
    for (int port = paxos_service->first_port; port <= paxos_service->last_port;
         port++) {
      std::string address("127.0.0.1:" + std::to_string(port));
      auto channel =
          grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      chubby_stubs_map[address] = chubby::Chubby::NewStub(channel);
      if (!chubby_stubs_map[address]) {
        std::cerr << "Failed to create gRPC stub\n";
      }
      grpc_connectivity_state state = channel->GetState(true);
      if (state == GRPC_CHANNEL_SHUTDOWN ||
          state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
        std::cerr << "Failed to establish gRPC channel connection\n";
      }
    }
  }

  Status Ping(ServerContext *context, const chubby::Empty *request,
              chubby::Empty *response) override {
    return Status::OK;
  }

  Status Ping(ServerContext *context, const chubby::Empty *request,
              chubby::Empty *response) override {
    return Status::OK;
  }

  Status KeepAlive(ServerContext *context, const KeepAliveRequest *request,
                   KeepAliveResponse *response) override {
    std::string client_id = request->client_id();

    if (paxos_service->leader_dead) {
      std::string leader_address(
          "127.0.0.1:" +
          std::to_string(paxos_service->first_port +
                         paxos_service->view % paxos_service->num_servers));
      ClientContext c_context;
      chubby_stubs_map[leader_address]->KeepAlive(&c_context, *request,
                                                  response);
    }

    {
      std::unique_lock<std::mutex> lock(lease_map_mutex);
      std::cout << "Received KeepAlive for client id " << client_id
                << std::endl;
      // Check if the client session exists
      // if (client_session_map.find(client_id) == client_session_map.end()) {
      //   response->set_success(false);
      //   response->set_error_message("No client session available");
      //   return Status::OK;
      // }

      // Block until lease timeout is about to expire
      lease_cv.wait_until(
          lock, client_lease_map[client_id] - std::chrono::milliseconds(100),
          [this, &client_id]() {
            return std::chrono::steady_clock::now() >=
                   client_lease_map[client_id];
          });

      auto new_timeout = std::chrono::steady_clock::now() + lease_timeout;
      client_lease_map[client_id] = new_timeout;
    }

    uint64_t new_timeout = std::chrono::duration_cast<std::chrono::seconds>(
                               client_lease_map[client_id].time_since_epoch())
                               .count();
    response->set_success(true);
    response->set_lease_timeout(new_timeout);
    response->set_error_message("");

    return Status::OK;
  }

  Status AcquireLock(ServerContext *context, const AcquireLockRequest *request,
                     AcquireLockResponse *response) override {
    if (paxos_service->leader_dead) {
      std::string leader_address(
          "127.0.0.1:" +
          std::to_string(paxos_service->first_port +
                         paxos_service->view % paxos_service->num_servers));
      ClientContext c_context;
      chubby_stubs_map[leader_address]->AcquireLock(&c_context, *request,
                                                    response);
    }

    key_lock.lock(request->path()); // So that other key paths are not locked
    slot_number++;

    std::string request_locking_mode = request->locking_mode();

    if (client_lease_map.find(request->client_id()) ==
        client_lease_map.end()) { // If machine comes back after getting killed
                                  // for some time, should this be persisted?

      response->set_success(false);
      response->set_error_message("NO_CLIENT_SESSION");

    } else {
      std::string value;
      bool key_found = paxos_service->paxos_db.Get(request->path(), value);

      if (key_found) {
        std::cout << "Key_Found AcquireLock request_mode, value_found "
                  << request_locking_mode << ", " << value << std::endl;
        if (request_locking_mode == "SHARED") {
          if (value == "FREE") {
            // Need to add client_id also to the value?
            Put(request->path(), request_locking_mode);
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          } else if (value == "SHARED") {
            // If client id needs to be added then we need to put to chubby_db
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          } else if (value == "EXCLUSIVE") {
            response->set_success(false);
            response->set_error_message("EXCLUSIVE_LOCK_HELD");
          }
        } else if (request_locking_mode == "EXCLUSIVE") {
          if (value == "FREE") {
            Put(request->path(), request_locking_mode);
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          } else if (value == "SHARED") {
            response->set_success(false);
            response->set_error_message("SHARED_LOCK_HELD");
          } else if (value == "EXCLUSIVE") {
            response->set_success(false);
            response->set_error_message("EXCLUSIVE_LOCK_HELD");
          }
        }
      } else {
        // Need to add client_id also to the value?
        //  std::string old_value;
        Put(request->path(), request_locking_mode);
        response->set_success(true);
        response->set_error_message("NO_ERROR");
      }
    }

    key_lock.unlock(request->path());

    return Status::OK;
  }

  Status TryAcquireLock(ServerContext *context,
                        const TryAcquireLockRequest *request,
                        TryAcquireLockResponse *response) override {
    if (paxos_service->leader_dead) {
      std::string leader_address(
          "127.0.0.1:" +
          std::to_string(paxos_service->first_port +
                         paxos_service->view % paxos_service->num_servers));
      ClientContext c_context;
      chubby_stubs_map[leader_address]->TryAcquireLock(&c_context, *request,
                                                       response);
    }

    return Status::OK;
  }

  Status ReleaseLock(ServerContext *context, const ReleaseLockRequest *request,
                     ReleaseLockResponse *response) override {
    if (paxos_service->leader_dead) {
      std::string leader_address(
          "127.0.0.1:" +
          std::to_string(paxos_service->first_port +
                         paxos_service->view % paxos_service->num_servers));
      ClientContext c_context;
      chubby_stubs_map[leader_address]->ReleaseLock(&c_context, *request,
                                                    response);
    }

    key_lock.lock(request->path());

    std::string value;
    bool key_found = paxos_service->paxos_db.Get(request->path(), value);

    if (key_found) {
      if (value == "FREE") {
        response->set_success(false);
        response->set_error_message("TRYING_TO_RELEASE_UNACQUIRED_LOCK");
      } else if (value == "SHARED" || value == "EXCLUSIVE") {
        response->set_success(true);
        response->set_error_message("NO_ERROR");

        std::string new_value = "FREE";
        Put(request->path(), new_value);
      }
    } else {
      response->set_success(false);
      response->set_error_message("TRYING_TO_RELEASE_UNACQUIRED_LOCK");
    }

    key_lock.unlock(request->path());

    return Status::OK;
  }
};

void RunServer(std::string &server_address) {
  int host_port = std::stoi(server_address.substr(server_address.find(":") + 1,
                                                  server_address.size()));

  PaxosImpl paxos_service(3, "db_" + std::to_string(host_port),
                          20 * 1024 * 1024, server_address);

  ChubbyImpl chubby_service(&paxos_service);
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&chubby_service);
  builder.RegisterService(&paxos_service);
  std::unique_ptr<Server> server(builder.BuildAndStart());

  if (!server) {
    std::cerr << "Failed to start server on " << server_address << std::endl;
    exit(1);
  }

  std::cout << "Server started at " << server_address << std::endl;

  std::thread([&paxos_service]() {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      paxos_service.SendHeartbeats();
    }
  }).detach();

  std::thread([&paxos_service]() {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    while (true) {
      paxos_service.DetectLeaderFailure();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();

  server->Wait();
}

int main(int argc, char **argv) {
  std::string server_address("127.0.0.1:50051");
  if (argc > 1) {
    server_address = argv[1];
  }

  RunServer(server_address);
  return 0;
}