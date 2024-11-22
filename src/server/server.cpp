#include "Chubby.grpc.pb.h"
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

class ChubbyImpl final : public Chubby::Service {
  public:
  ChubbyImpl() {}
};

void RunServer(std::string &server_address, int total_servers,
               int virtual_servers_for_ch) {
  int host_port = std::stoi(server_address.substr(server_address.find(":") + 1,
                                             server_address.size()));
  
  ChubbyImpl chubby_service;

  size_t cache_size = 20 * 1024 * 1024; // 20MB cache
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