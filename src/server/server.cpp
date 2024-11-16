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
#include "thread_pool.h"
#include "rocksdb_wrapper.h"

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
using paxos::WriteRequest;
using paxos::WriteResponse;

enum ConsensusStatus {DECIDED,NOT_DECIDED};

struct PaxosSlot{
    int n_p;
    int n_a;
    std::string v_a;
    ConsensusStatus status;
};

class PaxosImpl final : public Paxos::Service {
private:
  int group_size;
  int max_proposal_number_seen_so_far;
  int highest_log_idx;
  int max_accept_retries = 3;
  int first_port;
  int last_port;
  std::string server_address;
  std::mutex log_mutex;
  std::map<int,PaxosSlot> log;
  ThreadPool accept_thread_pool; //Should be formed conditionally if the current server is a master
  RocksDBWrapper rocks_db_wrapper;
  std::map<std::string, std::unique_ptr<Paxos::Stub>> paxos_stubs_map;

  PaxosImpl(int group_size,std::string db_path,size_t cache_size,int server_address) //This is the rocksDB cache size
      : accept_thread_pool(8),
        rocks_db_wrapper(db_path,cache_size)
  {
      this->group_size = group_size; 
      this->highest_log_idx = 0; //Do this only when spawned first time. Otherwise read it from database 
      this->first_port = 50051;
      this->last_port = 50051 + group_size - 1;
      this->server_address = server_address;
      InitializeServerStubs();
  }

  void InitializeServerStubs() {
    for (int port = first_port; port <= last_port; port++) {
      std::string address("127.0.0.1:" + std::to_string(port));
      auto channel =
          grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
      paxos_stubs_map[address] = paxos::Paxos::NewStub(channel);
      if (!paxos_stubs_map[address]) {
        std::cerr << "Failed to create gRPC stub\n";
      }
      grpc_connectivity_state state = channel->GetState(true);
      if (state == GRPC_CHANNEL_SHUTDOWN ||
          state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
        std::cerr << "Failed to establish gRPC channel connection\n";
      }
    }
  }

  void initSlot(int log_index){

    PaxosSlot slot;
    slot.n_p = -1;
    slot.n_a = -1;
    slot.v_a = nullptr;
    slot.status = ConsensusStatus::NOT_DECIDED;

    log[log_index] = slot;

  }

  Status Write(ServerContext *context, const WriteRequest *request, WriteResponse *response) override {
  
      int log_index;
      {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_index = this->highest_log_idx++;
      }

      if(log.find(log_index) == log.end()){
        initSlot(log_index);
      }

      bool decided = false;
      std::string server_address = this->server_address;
      std::string value = request->key();

      accept_thread_pool.enqueue([this,server_address,log_index,value,&decided]{

        std::vector<std::future<bool>> futures;
        int no_of_successful_accept_requests = 0;

        for (int p = first_port; p <= last_port; p++) {
          std::string server_address = std::string("127.0.0.1:") + std::to_string(p);
          if(paxos_stubs_map.contains(server_address)){
            futures.emplace_back(std::async(
                                 std::launch::async, &PaxosImpl::InvokeAcceptRequests, this,
                                 server_address,log_index,value));
          }
        }

        for (auto& future : futures) {
            if(future.get())
                ++no_of_successful_accept_requests;
        }

        if(no_of_successful_accept_requests >= (group_size + 1)/2)
            decided = true;
      });

      if(decided)
          log[log_index].status = ConsensusStatus::DECIDED;

      //Have some commit logic here before responding to the client 

      return Status::OK;
  }

  // Status Read(ServerContext *context, const ReadRequest *request, ReadResponse *response) override {
  
  // }

  Status Propose(ServerContext *context, const PaxosProposeRequest *request, PaxosProposeResponse *response) override {
    int proposal_number = request->proposal_number();
    int log_index = request->log_index();
  
    if(proposal_number > log[log_index].n_p){
        log[log_index].n_p = proposal_number;   //Need to persist this to disk
        response->set_last_accepted_proposal(log[log_index].n_a);
        response->set_last_accepted_value(log[log_index].v_a);
        response->set_is_proposal_accepted(true);
    }else{
        response->set_is_proposal_accepted(false);
    }

    return Status::OK;
  }

  bool InvokeAcceptRequests(std::string server_address, int log_index, std::string value){

    ClientContext context;
    PaxosAcceptRequest paxos_accept_request;
    PaxosAcceptResponse paxos_accept_response;

    int retry_count = 0;
    paxos_accept_request.set_proposal_number(max_proposal_number_seen_so_far + 1);  //TODO : Read from the db, maybe this machine just came up after failing
    paxos_accept_request.set_log_index(log_index); 
    paxos_accept_request.set_value(value);

    while (retry_count < max_accept_retries) {
      Status status = paxos_stubs_map[server_address]->Accept(&context, paxos_accept_request, &paxos_accept_response);
      if (status.ok()) {
        if (paxos_accept_response.is_accepted()) {
          return true;
        } else {
          break;
        }
      }
      retry_count++;
    }

    return false;
  }

  Status Accept(ServerContext *context, const PaxosAcceptRequest *request, PaxosAcceptResponse *response) override {
    int proposal_number = request->proposal_number();
    int log_index = request->log_index();
  
    if(proposal_number >= log[log_index].n_a){
        // max_proposal_number_seen_so_far = max(proposal_number; //Need to persist it to disk 
        log[log_index].n_a = proposal_number; //Need to persist it to disk 
        log[log_index].v_a = request->value(); //Need to persist it to disk 
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
