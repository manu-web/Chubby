#include "Paxos.grpc.pb.h"
#include "Paxos.pb.h"
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

enum ConsensusStatus { DECIDED, NOT_DECIDED, PENDING, FORGOTTEN };

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

  void ToProtobuf(paxos::PaxosSlot *protoPaxosSlot) const {
    protoPaxosSlot->set_n(n);
    protoPaxosSlot->set_n_p(n_p);
    protoPaxosSlot->set_n_a(n_a);
    protoPaxosSlot->set_v_a(v_a);
    protoPaxosSlot->set_highest_n(highest_N);
    protoPaxosSlot->set_value(value);
    protoPaxosSlot->set_status(static_cast<int>(status));
  }
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

  bool leader_dead;
  int group_size;
  int max_proposal_number_seen_so_far;
  int highest_log_idx;
  int max_accept_retries = 3;
  int first_port;
  int last_port;
  int self_port;
  int missed_heartbeats;
  int highest_accepted_seq;
  int num_servers;
  int is_dead;
  std::string server_address;
  std::string master_address;
  std::map<int, PaxosSlot> log;
  ThreadPool accept_thread_pool; // Should be formed conditionally if the
                                 // current server is a master
  RocksDBWrapper rocks_db_wrapper;
  std::map<std::string, std::unique_ptr<Paxos::Stub>> paxos_stubs_map;

public:
  // void SendHeartbeats();
  // void DetectLeaderFailure();

  // bool send_propose(std::string server_address, int log_index,
  //                   std::string value);
  //   bool InvokeAcceptRequests(std::string server_address, int log_index,
  //                           std::string value);

  PaxosImpl(int group_size, std::string db_path, size_t cache_size,
            std::string server_address);

  void InitializeServerStubs();

  PaxosSlot *initSlot(int view);

  PaxosSlot *fillSlot(paxos::PaxosSlot rpc_slot);

  PaxosSlot *addSlots(int seq);

  Status Elect(ServerContext *context, const ElectRequest *request,
               ElectResponse *response) override;

  Status Prepare(ServerContext *context, const PrepareRequest *request,
                 PrepareResponse *response) override;

  Status Accept(ServerContext *context, const AcceptRequest *request,
                AcceptResponse *response) override;

  Status Learn(ServerContext *context, const DecideRequest *request,
               DecideResponse *response) override;

  Status ForwardLeader(ServerContext *context,
                       const ForwardLeaderRequest *request,
                       ForwardLeaderResponse *response) override;

  Status Start(ServerContext *context, const StartRequest *request,
               Empty *response) override;

  void Start(int seq, std::string v);

  void StartOnForward(int seq, std::string v);

  void StartOnNewSlot(int seq, std::string v, PaxosSlot *slot, int my_view);

  // bool send_propose(std::string server_address, int log_index,
  //                   std::string value) {
  //   if (view % group_size == self_index) { // if its a master
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

  // bool InvokeAcceptRequests(std::string server_address, int log_index,
  //                           std::string value) {

  //   ClientContext context;
  //   PaxosAcceptRequest paxos_accept_request;
  //   PaxosAcceptResponse paxos_accept_response;

  //   int retry_count = 0;
  //   paxos_accept_request.set_proposal_number(
  //       max_proposal_number_seen_so_far +
  //       1); // TODO : Read from the db, maybe this machine just came up
  //       after
  //           // failing
  //   paxos_accept_request.set_log_index(log_index);
  //   paxos_accept_request.set_value(value);

  //   while (retry_count < max_accept_retries) {
  //     Status status = paxos_stubs_map[server_address]->Accept(
  //         &context, paxos_accept_request, &paxos_accept_response);
  //     if (status.ok()) {
  //       if (paxos_accept_response.is_accepted()) {
  //         return true;
  //       } else {
  //         break;
  //       }
  //     }
  //     retry_count++;
  //   }

  //   return false;
  // }

  Status Heartbeat(ServerContext *context, const HeartbeatRequest *request,
                   HeartbeatResponse *response) override;

  void SendHeartbeats();

  void DetectLeaderFailure();

  // bool InvokeAcceptRequests(std::string server_address, int log_index,
  //                           std::string value) {

  //   ClientContext context;
  //   AcceptRequest paxos_accept_request;
  //   AcceptResponse paxos_accept_response;

  //   int retry_count = 0;
  //   paxos_accept_request.set_proposal_number(
  //       max_proposal_number_seen_so_far +
  //       1); // TODO : Read from the db, maybe this machine just came up
  //       after
  //           // failing
  //   paxos_accept_request.set_log_index(log_index);
  //   paxos_accept_request.set_value(value);

  //   while (retry_count < max_accept_retries) {
  //     Status status = paxos_stubs_map[server_address]->Accept(
  //         &context, paxos_accept_request, &paxos_accept_response);
  //     if (status.ok()) {
  //       if (paxos_accept_response.is_accepted()) {
  //         return true;
  //       } else {
  //         break;
  //       }
  //     }
  //     retry_count++;
  //   }

  //   return false;
  // }
  void Election(int my_view, int offset);

  int getPortNumber(const std::string &address);

};

// void RunServer(std::string &server_address) {
//   int port = std::stoi(server_address.substr(server_address.find(":") + 1,
//                                              server_address.size()));
//   PaxosImpl service(3, "db_" + std::to_string(port), 20 * 1024 * 1024,
//                     server_address);

//   ServerBuilder builder;
//   builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
//   builder.RegisterService(&service);
//   std::unique_ptr<Server> server(builder.BuildAndStart());

//   if (!server) {
//     std::cerr << "Failed to start server on " << server_address << std::endl;
//     exit(1);
//   }

//   std::cout << "Server started at " << server_address << std::endl;
//   server->Wait();
// }

// int main(int argc, char **argv) {
//   std::string server_address("127.0.0.1:50051");
//   if (argc > 1) {
//     server_address = argv[1];
//   }

//   RunServer(server_address);
//   return 0;
// }

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
