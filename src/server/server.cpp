#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
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
private:
  ChubbyImpl() {}
};

void RunServer() {}

int main(int argc, char **argv) { return 0; }