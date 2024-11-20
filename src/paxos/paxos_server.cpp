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

  int getPortNumber(const std::string &address);
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
    this->me = self_port % group_size;
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

  PaxosSlot *fillSlot(paxos::PaxosSlot rpc_slot) {
    PaxosSlot *slot;
    slot->n = rpc_slot.n();
    slot->n_p = rpc_slot.n_p();
    slot->n_a = rpc_slot.n_a();
    slot->v_a = rpc_slot.v_a();
    slot->highest_N = rpc_slot.highest_n();
    slot->value = rpc_slot.value();
    slot->status = static_cast<ConsensusStatus>(rpc_slot.status());

    return slot;
  }

  PaxosSlot *addSlots(int seq) {
    if (slots.find(seq) != slots.end()) {
      slots[seq] = initSlot(view);
    }
    slots[seq]->n_a = view;
    return slots[seq];
  }

  Status Elect(ServerContext *context, const ElectRequest *request,
                 ElectResponse *response) override {
    mu.lock();
    if (request->view() > view) {
      response->set_status("OK");
      view = request->view();
      response->set_view(view);
      leader_dead = false;
      missed_heartbeats = 0;
      response->set_highest_seq(highest_accepted_seq);
    } else if (request->view() == view) {
      response->set_status("OK");
      response->set_view(view);
      response->set_highest_seq(highest_accepted_seq);
    } else {
      response->set_status("Reject");
      response->set_view(view);
    }
    mu.unlock();
    return Status::OK;
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
    slot_lock.unlock();
    return Status::OK;
  }

  Status Accept(ServerContext *context, const AcceptRequest *request,
                AcceptResponse *response) override {

    std::unique_lock<std::mutex> accept_lock(mu);
    if (request->seq() < lowest_slot) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "This slot has been garbage collected");
    }
    response->set_latest_done(done[me]);
    PaxosSlot *slot = addSlots(request->seq());
    if (request->view() > view) {
      view = request->view();
      leader_dead = false;
      missed_heartbeats = 0;
    }
    accept_lock.unlock();
    std::unique_lock<std::mutex> slot_lock(slot->mu_);
    response->set_view(slot->n_a);
    response->set_value(slot->v_a);

    if (request->view() >= slot->n_a) {
      slot->n_a = request->view();
      slot->v_a = request->value();
      response->set_status("OK");
      accept_lock.lock();
      if (request->seq() > highest_accepted_seq) {
        highest_accepted_seq = request->seq();
      }
      accept_lock.unlock();
    } else {
      response->set_status("REJECT");
    }

    if (slot->status == DECIDED) {
      response->set_value(slot->value);
    } else {
      response->set_value(nullptr);
    }

    return Status::OK;
  }

  Status Learn(ServerContext *context, const DecideRequest *request,
                DecideResponse *response) override {

      mu.lock();
      if(request->seq() < lowest_slot){
        mu.unlock();
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "This slot has been garbage collected");
      }

      response->set_latest_done(done[me]);
      PaxosSlot *slot = addSlots(request->seq());

      if(request->seq() > highest_slot){
        highest_slot = request->seq();
      }

      mu.unlock();
      std::unique_lock<std::mutex> slot_lock(slot->mu_);

      if(slot->status != DECIDED){
        slot->status = DECIDED;
        slot->value = request->value();
      }

      return Status::OK;
  }

  Status ForwardLeader(ServerContext *context,
                       const ForwardLeaderRequest *request,
                       ForwardLeaderResponse *response) override {

    Start(request->seq(), request->value());
    response->set_status("OK");

    return Status::OK;
  }

  void Start(int seq, std::string v) {
    std::unique_lock<std::mutex> start_on_forward_lock(mu);

    if (seq < lowest_slot) {
      return;
    }

    PaxosSlot *slot = addSlots(seq);
    if (slot->status != DECIDED && !is_dead) {
      if (view % num_servers == me) {
        if (!leader_dead) {
          StartOnNewSlot(seq, v, slot, view);
        }
      } else {
        StartOnForward(seq, v);
      }
    }
  }

  void StartOnForward(int seq, std::string v) {
    std::unique_lock<std::mutex> start_on_forward_lock(mu);
    PaxosSlot *slot = addSlots(seq);

    if (slot->status != DECIDED) {
      if (view % (num_servers) == me) {
        if (!leader_dead) {
          StartOnNewSlot(seq, v, slot, view);
          return;
        }
      }
    }

    ClientContext context;
    ForwardLeaderRequest forward_request;
    ForwardLeaderResponse forward_response;

    std::string leader_address("127.0.0.1:" +
                               std::to_string(first_port + view % num_servers));

    forward_request.set_seq(seq);
    forward_request.set_value(v);
    paxos_stubs_map[leader_address]->ForwardLeader(&context, forward_request,
                                                   &forward_response);

    if (forward_response.status() != "OK") {
      mu.lock();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::rand() % 1000));
      mu.unlock();
    }
  }

  void StartOnNewSlot(int seq, std::string v, PaxosSlot *slot, int my_view) {

    std::unique_lock<std::mutex> slot_lock(slot->mu_);
    if (slot->status == DECIDED || is_dead) {
      return;
    }
    // prepare phase

    int highest_na;
    std::string highest_va = v;
    std::atomic<bool> is_decided_prep = false;
    std::string decided_V;
    std::atomic<int> majority_count;
    std::atomic<int> reject_count;
    std::atomic<int> highest_view = -1;
    std::map<int, int> na_count_map;
    highest_na = -1;

    std::unique_lock<std::mutex> start_on_new_slot_lock(mu);
    majority_count = 0;
    reject_count = 0;
    if (seq <= highest_accepted_seq) {
      start_on_new_slot_lock.unlock();
      while (slot->n <= slot->highest_N) {
        slot->n += num_servers;
      }

      std::vector<std::thread> threads;
      for (const auto &pair : paxos_stubs_map) {
        threads.emplace_back([&]() {
          ClientContext context;
          PrepareRequest prepare_request;
          PrepareResponse prepare_response;
          prepare_request.set_seq(seq);
          prepare_request.set_proposal_number(slot->n);
          prepare_request.set_sender_id(me);
          prepare_request.set_latest_done(done[me]);
          Status status = pair.second->Prepare(&context, prepare_request,
                                               &prepare_response);

          if (status.ok()) {
            // TODO : Call Forget RPC here
            if (prepare_response.status() == "OK") {
              majority_count++;
              if (prepare_response.n_a() > highest_na) {
                highest_na = prepare_response.n_a();
                highest_va = prepare_response.v_a();
                na_count_map[highest_na] += 1;
              } else {
                reject_count += 1;
                if (slot->highest_N < prepare_response.highest_n()) {
                  slot->highest_N = prepare_response.highest_n();
                }
              }
              if (prepare_response.status() == "OK" &&
                  prepare_response.value() != "") {
                is_decided_prep = true;
                decided_V = prepare_response.value();
                return;
              }
              if (na_count_map[highest_na] > paxos_stubs_map.size() / 2) {
                is_decided_prep = true;
                decided_V = highest_va;
                return;
              }
            }
          }
          if (reject_count > paxos_stubs_map.size() / 2 ||
              majority_count > paxos_stubs_map.size() / 2 ||
              na_count_map[highest_na] > paxos_stubs_map.size() / 2) {
            return;
          }
        });
      }
      for (auto &t : threads) {
        t.join();
      }

      if (highest_na > my_view) {
        start_on_new_slot_lock.lock();
        if (highest_na > view) {
          view = highest_na;
          leader_dead = false;
          missed_heartbeats = 0;
          start_on_new_slot_lock.unlock();
          StartOnForward(seq, v);
        }
        start_on_new_slot_lock.unlock();
        StartOnForward(seq, v);
        return;
      }

      if (majority_count <= num_servers / 2 && !is_decided_prep) {
        std::unique_lock<std::mutex> slot_lock(slot->mu_);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::rand() % 1000));
        slot_lock.unlock();
      }
    } else {
      start_on_new_slot_lock.unlock();
    }

    if (highest_na == -1) {
      highest_va = v;
    }

    std::atomic<bool> is_decided_acc = false;
    if (!is_decided_prep) {
      majority_count = 0;
      reject_count = 0;

      std::vector<std::thread> threads;
      for (const auto &pair : paxos_stubs_map) {
        threads.emplace_back([&]() {
          ClientContext context;
          AcceptRequest accept_request;
          AcceptResponse accept_response;
          accept_request.set_seq(seq);
          accept_request.set_view(my_view);
          accept_request.set_value(highest_va);
          accept_request.set_sender_id(me);
          accept_request.set_latest_done(done[me]);
          Status status = pair.second->Accept(
              &context, accept_request,
              &accept_response); // Have to add some timeout to the grpc
                                 // request otherwise might be blocking

          if (status.ok()) {
            // TODO : Call Forget RPC here
            if (accept_response.status() == "OK") {
              majority_count++;
            } else {
              reject_count++;
              if (accept_response.view() > highest_view) {
                highest_view = accept_response.view();
              }
            }
            if (accept_response.latest_done() >= seq ||
                accept_response.value() != "") {
              is_decided_acc = true;
              mu.lock();
              decided_V = accept_response.value();
              mu.unlock();
            }
          }

          // if(reject_count > num_servers/2 || majority_count >
          // num_servers/2) {
          //   break;
        });
      }

      for (auto &thread : threads) {
        thread.join();
      }

      if (is_decided_acc)
        return;

      if (highest_na > my_view) {
        start_on_new_slot_lock.lock();
        if (highest_na > view) {
          view = highest_na;
          leader_dead = false;
          missed_heartbeats = 0;
          start_on_new_slot_lock.unlock();
          StartOnForward(seq, v);
        }
        start_on_new_slot_lock.unlock();
        StartOnForward(seq, v);
        return;
      }

      if (majority_count <= num_servers / 2 && !is_decided_prep) {
        std::unique_lock<std::mutex> slot_lock(slot->mu_);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::rand() % 1000));
        slot_lock.unlock();
      }
    }

    if (is_decided_prep || is_decided_acc) {
      highest_va = decided_V;
    }

    // TODO : Call learn here
    std::vector<std::thread> threads;
    for (const auto &pair : paxos_stubs_map) {
        threads.emplace_back([&]() {
          ClientContext context;
          mu.lock();
          DecideRequest decide_request;
          DecideResponse decide_response;
          decide_request.set_seq(seq);
          decide_request.set_value(highest_va);
          decide_request.set_sender_id(me);
          decide_request.set_latest_done(done[me]);
          mu.unlock();

          Status status = pair.second->Learn(
              &context, decide_request,
              &decide_response);

          if(status.ok()){
            //TODO : Call forget method here
          }
        });
    }

    for (auto &thread : threads) {
      thread.join();
    }
  }

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
  //       1); // TODO : Read from the db, maybe this machine just came up after
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
                   HeartbeatResponse *response) override {
    std::lock_guard<std::mutex> lock(mu);
    if (me == request->id()) {
      missed_heartbeats = 0;
      return Status::OK;
    }

    if (view > request->view()) {
      return grpc::Status(grpc::StatusCode::ABORTED,
                          "your view is lower than mine");
    }

    if (view < request->view()) {
      view = request->view();
      leader_dead = false;
      missed_heartbeats = 0;
    }

    std::vector<int> done_vec(request->done().begin(), request->done().end());
    done.assign(done_vec.begin(), done_vec.end());
    missed_heartbeats = 0;

    std::map<int, PaxosSlot *> request_slots;

    for (const auto &pair : request->slots()) {
      const paxos::PaxosSlot &slot = pair.second;
      request_slots[pair.first] = fillSlot(slot);
    }

    std::map<int, PaxosSlot *> reply_slots;

    for (const auto &pair : slots) {
      if (pair.second->status == ConsensusStatus::DECIDED) {
        if (request_slots.find(pair.first) == request_slots.end()) {
          auto decided_value = pair.second->value;
          auto newSlot = initSlot(view);
          newSlot->value = decided_value;
          newSlot->status = ConsensusStatus::DECIDED;
          reply_slots[pair.first] = newSlot;
        }
      }
    }

    for (const auto &pair : request_slots) {
      auto new_slot = addSlots(pair.first);
      std::lock_guard<std::mutex> lock(new_slot->mu_);
      if (new_slot->status != ConsensusStatus::DECIDED) {
        new_slot->status = ConsensusStatus::DECIDED;
        new_slot->value = pair.second->value;
      }
    }

    for (const auto &pair : reply_slots) {
      paxos::PaxosSlot protobufSlot;
      pair.second->ToProtobuf(&protobufSlot);
      (*response->mutable_slots())[pair.first] = protobufSlot;
    }

    return Status::OK;
  }

  void SendHeartbeats() {
    mu.lock();
    if (view % group_size != me) {
      mu.unlock();
      return;
    }

    HeartbeatRequest request;
    ClientContext context;
    HeartbeatResponse response;
    std::map<int, PaxosSlot *> request_slots;

    for (const auto &pair : slots) {
      if (pair.second->status == ConsensusStatus::DECIDED) {
        auto decided_value = pair.second->value;
        auto newSlot = initSlot(view);
        newSlot->value = decided_value;
        request_slots[pair.first] = newSlot;
      }
    }

    for (const auto &pair : request_slots) {
      paxos::PaxosSlot protobufSlot;
      pair.second->ToProtobuf(&protobufSlot);
      (*request.mutable_slots())[pair.first] = protobufSlot;
    }
    int curr_view = view;
    mu.unlock();

    if (curr_view % group_size == me) {
      for (const auto &pair : paxos_stubs_map) {
        mu.lock();
        request.set_id(me);
        request.set_view(curr_view);
        for (const int val : done) {
          request.add_done(val);
        }
        mu.unlock();
        Status status = pair.second->Heartbeat(&context, request, &response);
        if (status.ok() && pair.first != server_address) {
          mu.lock();
          std::map<int, PaxosSlot *> response_slots;

          for (const auto &pair : response.slots()) {
            const paxos::PaxosSlot &slot = pair.second;
            response_slots[pair.first] = fillSlot(slot);
          }

          for (const auto &pair : response_slots) {
            auto slot = addSlots(pair.first);
            slot->mu_.lock();
            if (slot->status != ConsensusStatus::DECIDED) {
              slot->status = ConsensusStatus::DECIDED;
              slot->value = pair.second->value;
            }
            slot->mu_.unlock();
          }
          mu.unlock();
        }
      }
    }
  }

  void DetectLeaderFailure() {
    std::lock_guard<std::mutex> lock(mu);
    missed_heartbeats++;

    if (missed_heartbeats > 3) {
      int mod = view % group_size;

      if (mod < me) {
        std::thread(&PaxosImpl::Election, this, view, me - mod).detach();
      } else if (mod > me) {
        std::thread(&PaxosImpl::Election, this, view, me + group_size - mod)
            .detach();
      }

      missed_heartbeats = 0;
      leader_dead = true;
    }
  }

  // bool InvokeAcceptRequests(std::string server_address, int log_index,
  //                           std::string value) {

  //   ClientContext context;
  //   AcceptRequest paxos_accept_request;
  //   AcceptResponse paxos_accept_response;

  //   int retry_count = 0;
  //   paxos_accept_request.set_proposal_number(
  //       max_proposal_number_seen_so_far +
  //       1); // TODO : Read from the db, maybe this machine just came up after
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
  void Election(int my_view, int offset) {
    while (true) {
      int majority_count = 0;
      int reject_count = 0;
      int64_t highest_view = -1;
      int max_highest_accepted_seq = -1;

      for (const auto &pair : paxos_stubs_map) {
        ClientContext context;
        ElectRequest request;
        ElectResponse response;
        request.set_view(my_view + offset);

        if (pair.first != server_address) {
          Status status = pair.second->Elect(&context, request, &response);

          if (status.ok()) {
            if (response.status() == "Reject") {
              reject_count++;
              if (response.view() > highest_view) {
                highest_view = response.view();
              }
            } else if (response.status() == "OK") {
              majority_count++;
              if (response.highest_seq() > max_highest_accepted_seq) {
                max_highest_accepted_seq = response.highest_seq();
              }
            }
          }
        }
      }

      mu.lock();
      if (reject_count > 0) {
        if (highest_view > view && my_view + offset < highest_view) {
          view = highest_view;
          leader_dead = false;
          missed_heartbeats = 0;
        }
        mu.unlock();
        return;
      }

      if (majority_count + 1 > paxos_stubs_map.size() / 2) {
        if (highest_view <= my_view + offset && view < my_view + offset) {
          leader_dead = false;
          view = my_view + offset;
          missed_heartbeats = 0;
          if (max_highest_accepted_seq > highest_accepted_seq) {
            highest_accepted_seq = max_highest_accepted_seq;
          }
          mu.unlock();
          return; // Election succeeded
        } else {
          if (highest_view > view) {
            view = highest_view;
            leader_dead = false;
            missed_heartbeats = 0;
          }
          mu.unlock();
          return; // Election failed
        }
      }

      mu.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 100));
    }
  }
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
