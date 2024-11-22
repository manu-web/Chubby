#include "Chubby.grpc.pb.h"
#include "rocksdb_wrapper.h"
#include "Chubby.pb.h"
#include "paxos_server.h"
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

using chubby::Chubby;
using chubby::AcquireLockRequest;
using chubby::AcquireLockResponse;
using chubby::TryAcquireLockRequest;
using chubby::TryAcquireLockResponse;
using chubby::LockingMode;
using chubby::KeepAliveRequest;
using chubby::KeepAliveResponse;

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

  RocksDBWrapper chubby_db;
  KeyLock key_lock;
  std::set<std::string> client_session_map; //Tracks if a client currently has session with the Chubby master, need not persist, recovery through client 
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> client_lease_map;
  std::mutex lease_map_mutex;
  std::condition_variable lease_cv;
  const std::chrono::seconds lease_timeout = std::chrono::seconds(12);

  public:
  
  ChubbyImpl(std::string db_path,std::size_t cache_size) : 
    chubby_db(db_path,cache_size){
  
  }

  Status KeepAlive(ServerContext* context, const KeepAliveRequest* request, KeepAliveResponse* response) override {
    std::string client_id = request->client_id();

    {
      std::unique_lock<std::mutex> lock(lease_map_mutex);

      // Check if the client session exists
      if (client_session_map.find(client_id) == client_session_map.end()) {
          response->set_success(false);
          response->set_error_message("No client session available");
          return Status::OK;
      }

      // Block until lease timeout is about to expire
      lease_cv.wait_until(lock, client_lease_map[client_id] - std::chrono::milliseconds(100), [this, &client_id]() {
          return std::chrono::steady_clock::now() >= client_lease_map[client_id];
      });

      auto new_timeout = std::chrono::steady_clock::now() + lease_timeout;
      client_lease_map[client_id] = new_timeout;
    }

    response->set_success(true);
    response->set_lease_timeout(lease_timeout.count());
    response->set_error_message("");

    return Status::OK;
  }

  Status AcquireLock(ServerContext *context, const AcquireLockRequest *request, AcquireLockResponse *response) override {

    key_lock.lock(request->path()); //So that other key paths are not locked 

    std::string request_locking_mode = LockingMode_Name(request->locking_mode());

    if(client_session_map.find(request->client_id()) == client_session_map.end()){ //If machine comes back after getting killed for some time, should this be persisted?

      response->set_success(false);
      response->set_error_message("NO_CLIENT_SESSION");

    }else{
      std::string value;
      bool key_found = chubby_db.Get(request->path(),value);

      if(key_found){
        if(request_locking_mode == "SHARED"){
          if(value == "FREE"){
            //Need to add client_id also to the value?
            chubby_db.Put(request->path(),request_locking_mode,value);
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          }else if(value == "SHARED"){
            //If client id needs to be added then we need to put to chubby_db
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          }else if(value == "EXCLUSIVE"){
            response->set_success(false);
            response->set_error_message("EXCLUSIVE_LOCK_HELD");
          }
        }else if(request_locking_mode == "EXCLUSIVE"){
          if(value == "FREE"){
            chubby_db.Put(request->path(),request_locking_mode,value);
            response->set_success(true);
            response->set_error_message("NO_ERROR");
          }else if(value == "SHARED"){
            response->set_success(false);
            response->set_error_message("SHARED_LOCK_HELD");
          }else if(value == "EXCLUSIVE"){
            response->set_success(false);
            response->set_error_message("EXCLUSIVE_LOCK_HELD");
          }
        }
      }else{
        //Need to add client_id also to the value?
        std::string old_value;
        chubby_db.Put(request->path(),request_locking_mode,old_value);
        response->set_success(true);
        response->set_error_message("NO_ERROR");
      }
    }

    key_lock.unlock(request->path());

    return Status::OK;
      
  }

  Status TryAcquireLock(ServerContext *context, const TryAcquireLockRequest *request, TryAcquireLockResponse *response) override {

    return Status::OK;
  }

};

void RunServer(std::string &server_address, int total_servers,
               int virtual_servers_for_ch) {
  int host_port = std::stoi(server_address.substr(server_address.find(":") + 1,
                                             server_address.size()));
  

  size_t cache_size = 20 * 1024 * 1024; // 20MB cache
  ChubbyImpl chubby_service("/chubby",cache_size);
  PaxosImpl paxos_service(total_servers, "/rocksdb", cache_size, server_address);

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
  server->Wait();
}

int main(int argc, char **argv) {
  std::string server_address("127.0.0.1:50051");
  int total_servers = 10;
  if (argc > 1) {
    server_address = argv[1];
    total_servers = std::atoi(argv[2]);
  }

  RunServer(server_address, total_servers, 1);
  return 0;
}