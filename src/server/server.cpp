#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
#include <chrono>
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

using paxos::Paxos;
using paxos::PaxosProposeRequest;
using paxos::PaxosProposeResponse;
using paxos::PaxosAcceptRequest;
using paxos::PaxosAcceptResponse;

class PaxosImpl final : public Paxos::Service {
private:
  int group_size;
  int max_proposal_number_seen_so_far; 
  int accepted_proposal;
  std::string accepted_value;
  PaxosImpl(int group_size){
      this->group_size = group_size; 
      this->accepted_proposal = -1;
      this->accepted_value = nullptr;
  }

  // Status Write(ServerContext *context, const WriteRequest *request, WriteResponse *response) override {
  
  // }

  // Status Read(ServerContext *context, const ReadRequest *request, ReadResponse *response) override {
  
  // }

  Status Propose(ServerContext *context, const PaxosProposeRequest *request, PaxosProposeResponse *response) override {
    int proposal_number = request->proposal_number();
  
    if(proposal_number > max_proposal_number_seen_so_far){
        max_proposal_number_seen_so_far = proposal_number;   //Need to persist this to disk
        response->set_last_accepted_proposal(accepted_proposal);
        response->set_last_accepted_value(accepted_value);
        response->set_is_proposal_accepted(true);
    }else{
        response->set_is_proposal_accepted(false);
    }

    return Status::OK;
  }

  Status Accept(ServerContext *context, const PaxosAcceptRequest *request, PaxosAcceptResponse *response) override {
    int proposal_number = request->proposal_number();
    int log_index = request->log_index();
  
    if(proposal_number >= max_proposal_number_seen_so_far){
        max_proposal_number_seen_so_far = proposal_number; //Need to persist it to disk 
        accepted_proposal = proposal_number; //Need to persist it to disk 
        accepted_value = request->value(); //Need to persist it to disk 
        response->set_is_accepted(true);
    }else{
        response->set_is_accepted(false);
    }

    return Status::OK;
  }
};

int main(int argc, char **argv) {
  return 0;
}
