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
public:
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
  RocksDBWrapper paxos_db;
  std::map<std::string, std::unique_ptr<Paxos::Stub>> paxos_stubs_map;

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

  Status Heartbeat(ServerContext *context, const HeartbeatRequest *request,
                   HeartbeatResponse *response) override;

  void SendHeartbeats();

  void DetectLeaderFailure();

  void Election(int my_view, int offset);

  int getPortNumber(const std::string &address);
};