#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
#include "rocksdb_wrapper.h"
#include "thread_pool.h"
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

using paxos::AcceptRequest;
using paxos::AcceptResponse;
using paxos::DecideRequest;
using paxos::DecideResponse;
using paxos::Empty;
using paxos::HeartbeatRequest;
using paxos::HeartbeatResponse;
using paxos::Paxos;
using paxos::PrepareRequest;
using paxos::PrepareResponse;

enum ConsensusStatus { DECIDED, NOT_DECIDED, PENDING };

struct PaxosSlot {
  int64_t n;         // Proposal number
  int64_t n_p;       // Prepared proposal number
  int64_t n_a;       // Accepted proposal number
  std::string v_a;   // Accepted value
  int64_t highest_N; // Highest proposal number seen so far
  std::mutex mu;
  std::mutex mu_;
  std::string value; // Proposed value
  ConsensusStatus status;
};

class PaxosImpl final : public Paxos::Service {
private:
  ////////////////////////////////////////////
  // Prepare stuff
  ////////////////////////////////////////////

  int highest_slot; // for prepare requests
  int lowest_slot;  // for prepare requests
  std::vector<int> done;
  std::map<int, PaxosSlot *> slots;
  int view;
  std::mutex mu;
  int me;
  ////////////////////////////////////////////

  int group_size;
  int max_proposal_number_seen_so_far;
  int highest_log_idx;
  int max_accept_retries = 3;
  int first_port;
  int last_port;
  int self_port;
  int self_index;
  int missed_heartbeats;
  int view_number;
  std::string server_address;
  std::string master_address;
  std::mutex log_mutex;
  std::mutex leader_mutex;
  std::map<int, PaxosSlot> log;
  ThreadPool accept_thread_pool; // Should be formed conditionally if the
                                 // current server is a master
  RocksDBWrapper rocks_db_wrapper;
  std::map<std::string, std::unique_ptr<Paxos::Stub>> paxos_stubs_map;

  // int getPortNumber(const std::string &address);
  // void SendHeartbeats();
  // void DetectLeaderFailure();

  // bool send_propose(std::string server_address, int log_index,
  //                   std::string value);
  //   bool InvokeAcceptRequests(std::string server_address, int log_index,
  //                           std::string value);

  PaxosImpl(int group_size, std::string db_path, size_t cache_size,
            std::string server_address) // This is the rocksDB cache size
      : accept_thread_pool(8), rocks_db_wrapper(db_path, cache_size) {
    this->group_size = group_size;
    this->highest_log_idx = 0; // Do this only when spawned first time.
                               // Otherwise read it from database
    this->first_port = 50051;
    this->last_port = 50051 + group_size - 1;
    this->server_address = server_address;
    this->self_port = getPortNumber(server_address);
    this->self_index = self_port % group_size;
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

  PaxosSlot *initSlot(int view) {
    PaxosSlot *slot;
    slot->status = PENDING;
    slot->value = nullptr;
    slot->n_a = view;
    slot->v_a = nullptr;
    slot->n_p = -1;
    slot->n = view;
    slot->highest_N = view;
    return slot;
  }

  PaxosSlot *addSlots(int seq) {
    if (slots.find(seq) != slots.end()) {
      slots[seq] = initSlot(view);
    }
    slots[seq]->n_a = view;
    return slots[seq];
  }

  Status Prepare(ServerContext *context, const PrepareRequest *request,
                 PrepareResponse *response) override {
    std::unique_lock<std::mutex> prepare_lock(mu);
    if (request->seq() < lowest_slot) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "This slot has been garbage collected");
    }
    response->set_latest_done(done[me]);
    PaxosSlot *slot = addSlots(request->seq());
    prepare_lock.unlock();
    std::unique_lock<std::mutex> slot_lock(slot->mu_);
    if (request->proposal_number() > slot->n_p) {
      response->set_status("OK");
      slot->n_p = request->proposal_number();
      response->set_n_a(slot->n_a);
      response->set_v_a(slot->v_a);
      response->set_highest_n(request->proposal_number());
    } else {
      response->set_status("Reject");
      response->set_n_a(slot->n_a);
      response->set_v_a(slot->v_a);
      response->set_highest_n(slot->n_p);
    }
    if (slot->status == DECIDED) {
      response->set_value(slot->value);
    } else {
      response->set_value(nullptr);
    }
    return Status::OK;
  }

  // bool send_propose(std::string server_address, int log_index,
  //                   std::string value) {
  //   if (view_number % group_size == self_index) { // if its a master
  //     for (int port = first_port; port <= last_port; port++) {
  //       std::string server_address =
  //           std::string("127.0.0.1:") + std::to_string(port);
  //       if (paxos_stubs_map.contains(server_address)) {
  //         futures.emplace_back(
  //             std::async(std::launch::async,
  //             &PaxosImpl::InvokeAcceptRequests,
  //                        this, server_address, log_index, value));
  //       }
  //     }
  //   }
  // }

  bool InvokeAcceptRequests(std::string server_address, int log_index,
                            std::string value) {

    ClientContext context;
    PaxosAcceptRequest paxos_accept_request;
    PaxosAcceptResponse paxos_accept_response;

    int retry_count = 0;
    paxos_accept_request.set_proposal_number(
        max_proposal_number_seen_so_far +
        1); // TODO : Read from the db, maybe this machine just came up after
            // failing
    paxos_accept_request.set_log_index(log_index);
    paxos_accept_request.set_value(value);

    while (retry_count < max_accept_retries) {
      Status status = paxos_stubs_map[server_address]->Accept(
          &context, paxos_accept_request, &paxos_accept_response);
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

  Status Accept(ServerContext *context, const PaxosAcceptRequest *request,
                PaxosAcceptResponse *response) override {
    int proposal_number = request->proposal_number();
    int log_index = request->log_index();

    if (proposal_number >= log[log_index].n_a) {
      // max_proposal_number_seen_so_far = max(proposal_number; //Need to
      // persist it to disk
      log[log_index].n_a = proposal_number;  // Need to persist it to disk
      log[log_index].v_a = request->value(); // Need to persist it to disk
      response->set_is_accepted(true);
    } else {
      response->set_is_accepted(false);
    }

    return Status::OK;
  }

  Status Heartbeat(ServerContext *context, const HeartbeatRequest *request,
                   HeartbeatResponse *response) override {
    std::lock_guard<std::mutex> lock(leader_mutex);
    if (self_index == request->server_index()) {
      missed_heartbeats = 0;
      return Status::OK;
    }

    if (view_number = request->view()) {
      return grpc::Status(grpc::StatusCode::ABORTED,
                          "your view is lower than mine");
    }

    if (impl.View < args.View) {
      view_number = request->view();
      leader_dead = false;
      missed_heartbeats = 0;
    }

    return Status::OK;
  }

  int getPortNumber(const std::string &address) {
    size_t colon_pos = address.find(':');
    if (colon_pos == std::string::npos) {
      throw std::invalid_argument("Invalid address format.");
    }

    return std::stoi(address.substr(colon_pos + 1));
  }

  void SendHeartbeats() {
    if (view_number % group_size == self_index) {
      for (const auto &pair : paxos_stubs_map) {
        std::cout << pair.first << std::endl;
        HeartbeatRequest message;
        ClientContext context;
        Empty response;
        // message.set_server_id(server_address);
        pair.second->Heartbeat(&context, message, &response);
      }
    }
  }

  void DetectLeaderFailure() {
    std::lock_guard<std::mutex> lock(leader_mutex);
    missed_heartbeats++;

    if (missed_heartbeats > 3) {
      int mod = view_number % group_size;

      if (mod < self_index) {
        std::thread(&PaxosImpl::Election, this, view_number, self_index - mod)
            .detach();
      } else if (mod > self_index) {
        std::thread(&PaxosImpl::Election, this, view_number,
                    self_index + group_size - mod)
            .detach();
      }

      missed_heartbeats = 0;
      leader_dead = true;
    }
  }

  bool InvokeAcceptRequests(std::string server_address, int log_index,
                            std::string value) {

    ClientContext context;
    AcceptRequest paxos_accept_request;
    AcceptResponse paxos_accept_response;

    int retry_count = 0;
    paxos_accept_request.set_proposal_number(
        max_proposal_number_seen_so_far +
        1); // TODO : Read from the db, maybe this machine just came up after
            // failing
    paxos_accept_request.set_log_index(log_index);
    paxos_accept_request.set_value(value);

    while (retry_count < max_accept_retries) {
      Status status = paxos_stubs_map[server_address]->Accept(
          &context, paxos_accept_request, &paxos_accept_response);
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

  void Election(int view, int offset);
};

void RunServer() {}

int main(int argc, char **argv) { return 0; }

// Status Write(ServerContext *context, const WriteRequest *request,
//              WriteResponse *response) override {

//   int log_index;
//   {
//     std::lock_guard<std::mutex> lock(log_mutex);
//     log_index = this->highest_log_idx++;
//   }

//   if (log.find(log_index) == log.end()) {
//     initSlot(log_index);
//   }

//   bool decided = false;
//   std::string server_address = this->server_address;
//   std::string value = request->key();

//   accept_thread_pool.enqueue([this, server_address, log_index, value,
//                               &decided] {
//     std::vector<std::future<bool>> futures;
//     int no_of_successful_accept_requests = 0;

//     for (int p = first_port; p <= last_port; p++) {
//       std::string server_address =
//           std::string("127.0.0.1:") + std::to_string(p);
//       if (paxos_stubs_map.contains(server_address)) {
//         futures.emplace_back(
//             std::async(std::launch::async,
//             &PaxosImpl::InvokeAcceptRequests,
//                        this, server_address, log_index, value));
//       }
//     }

//     for (auto &future : futures) {
//       if (future.get())
//         ++no_of_successful_accept_requests;
//     }

//     if (no_of_successful_accept_requests >= (group_size + 1) / 2)
//       decided = true;
//   });

//   if (decided)
//     log[log_index].status = ConsensusStatus::DECIDED;

//   // Have some commit logic here before responding to the client

//   return Status::OK;
// }

// Status Read(ServerContext *context, const ReadRequest *request,
// ReadResponse *response) override {

// }
