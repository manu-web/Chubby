#include "Paxos.grpc.pb.h"
#include "Paxos.pb.h"
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string>
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using paxos::AcceptRequest;
using paxos::AcceptResponse;
using paxos::DecideRequest;
using paxos::DecideResponse;
using paxos::ElectRequest;
using paxos::ElectResponse;
using paxos::Empty;
using paxos::ForwardLeaderRequest;
using paxos::ForwardLeaderResponse;
using paxos::HeartbeatRequest;
using paxos::HeartbeatResponse;
using paxos::Paxos;
using paxos::PrepareRequest;
using paxos::PrepareResponse;
using paxos::StartRequest;

int main() {
  std::string server_address("127.0.0.1:50051");
  auto channel =
      grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<paxos::Paxos::Stub> paxos_stub =
      paxos::Paxos::NewStub(channel);
  grpc_connectivity_state state = channel->GetState(true);
  if (state == GRPC_CHANNEL_SHUTDOWN ||
      state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    std::cerr << "Failed to establish gRPC channel connection\n";
  }

  ClientContext context;
  StartRequest request;
  Empty response;
  request.set_seq(0);
  request.set_value("Hi");
  Status status = paxos_stub->Start(&context, request, &response);

  ClientContext context2;
  request.set_seq(1);
  request.set_value("Hello");
  status = paxos_stub->Start(&context2, request, &response);
  return 0;
}