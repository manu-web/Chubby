#include "paxos_server.h"
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

PaxosImpl::PaxosImpl(
    int group_size, std::string db_path, size_t cache_size,
    std::string server_address) // This is the rocksDB cache size
    : accept_thread_pool(8), paxos_db(db_path, cache_size) {
  this->group_size = group_size;
  this->highest_log_idx = 0; // Do this only when spawned first time.
                             // Otherwise read it from database
  this->first_port = 50051;
  this->last_port = 50051 + group_size - 1;
  this->server_address = server_address;
  this->self_port = getPortNumber(server_address);
  this->me = self_port % 50051;
  this->num_servers = group_size;
  for (int i = 0; i < group_size; i++) {
    done.push_back(-1);
  }
  InitializeServerStubs();
}

int PaxosImpl::getPortNumber(const std::string &address) {
  size_t colon_pos = address.find(':');
  if (colon_pos == std::string::npos) {
    throw std::invalid_argument("Invalid address format.");
  }

  return std::stoi(address.substr(colon_pos + 1));
}

void PaxosImpl::InitializeServerStubs() {
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

PaxosSlot *PaxosImpl::initSlot(int view) {
  // PaxosSlot *slot = (PaxosSlot *)malloc(sizeof(PaxosSlot));
  PaxosSlot *slot = new PaxosSlot();
  slot->status = PENDING;
  slot->value = "";
  slot->n_a = view;
  slot->v_a = "";
  slot->n_p = -1;
  slot->n = view;
  slot->highest_N = view;
  return slot;
}

PaxosSlot *PaxosImpl::fillSlot(paxos::PaxosSlot rpc_slot) {
  PaxosSlot *slot = new PaxosSlot();
  slot->n = rpc_slot.n();
  slot->n_p = rpc_slot.n_p();
  slot->n_a = rpc_slot.n_a();
  slot->v_a = rpc_slot.v_a();
  slot->highest_N = rpc_slot.highest_n();
  slot->value = rpc_slot.value();
  slot->status = static_cast<ConsensusStatus>(rpc_slot.status());

  return slot;
}

PaxosSlot *PaxosImpl::addSlots(int seq) {
  if (slots.find(seq) == slots.end()) {
    std::cout << "Started init" << std::endl;
    slots[seq] = initSlot(view);
    std::cout << "Completed init" << std::endl;
  }
  slots[seq]->n_a = view;
  return slots[seq];
}

Status PaxosImpl::Elect(ServerContext *context, const ElectRequest *request,
                        ElectResponse *response) {
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

Status PaxosImpl::Prepare(ServerContext *context, const PrepareRequest *request,
                          PrepareResponse *response) {
  std::cout << "Received prepare request with seq, prop = " << request->seq()
            << ", " << request->proposal_number() << std::endl;
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

Status PaxosImpl::Accept(ServerContext *context, const AcceptRequest *request,
                         AcceptResponse *response) {

  std::unique_lock<std::mutex> accept_lock(mu);
  if (request->seq() < lowest_slot) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "This slot has been garbage collected");
  }
  std::cout << "Received Accept request with seq, value = " << request->seq()
            << ", " << request->value() << std::endl;
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

Status PaxosImpl::Learn(ServerContext *context, const DecideRequest *request,
                        DecideResponse *response) {
  std::cout << "Received Learn request with seq, value = " << request->seq()
            << ", " << request->value() << std::endl;
  mu.lock();
  if (request->seq() < lowest_slot) {
    mu.unlock();
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "This slot has been garbage collected");
  }

  response->set_latest_done(done[me]);
  PaxosSlot *slot = addSlots(request->seq());

  if (request->seq() > highest_slot) {
    highest_slot = request->seq();
  }

  mu.unlock();
  std::unique_lock<std::mutex> slot_lock(slot->mu_);

  if (slot->status != DECIDED) {
    slot->status = DECIDED;
    slot->value = request->value();
    std::string key, value, old_value;
    getKeyValue(request->value(), key, value);
    paxos_db.Put(key, value, old_value);
  }

  return Status::OK;
}

Status PaxosImpl::ForwardLeader(ServerContext *context,
                                const ForwardLeaderRequest *request,
                                ForwardLeaderResponse *response) {

  Start(request->seq(), request->value());
  response->set_status("OK");

  return Status::OK;
}

void PaxosImpl::Forget(int peer, int peerDone) {
  std::unique_lock<std::mutex> forget_lock(mu);
  if (peerDone > done[peer]) {
    done[peer] = peerDone;
  }
  int mini = done[0];
  for (int i = 0; i < done.size(); i++) {
    mini = std::min(mini, done[i]);
  }
  int offset = mini - lowest_slot + 1;
  if (offset > 0) {
    lowest_slot += offset;
  }
  // Erase outdated slots
  for (auto it = slots.begin(); it != slots.end();) {
    if (it->first < lowest_slot) {
      it = slots.erase(it);
    } else {
      ++it;
    }
  }
  forget_lock.unlock();
}

Status PaxosImpl::Start(ServerContext *context, const StartRequest *request,
                        Empty *response) {
  std::cout << "received start" << std::endl;
  Start(request->seq(), request->value());
  return Status::OK;
}

void PaxosImpl::Start(int seq, std::string v) {
  std::unique_lock<std::mutex> start_on_forward_lock(mu);
  std::cout << "received start with seq, v = " << seq << ", " << v << std::endl;
  if (seq < lowest_slot) {
    std::cout << "Seq less than lowest_slot" << std::endl;
    return;
  }

  PaxosSlot *slot = addSlots(seq);
  std::cout << "Completed add slots" << std::endl;
  if (slot->status != DECIDED && !is_dead) {
    if (view % num_servers == me) {
      if (!leader_dead) {
        start_on_forward_lock.unlock();
        std::thread(&PaxosImpl::StartOnNewSlot, this, seq, v, slot, view)
            .join();
        // StartOnNewSlot(seq, v, slot, view);
      }
    } else {
      start_on_forward_lock.unlock();
      std::thread(&PaxosImpl::StartOnForward, this, seq, v).join();
      // StartOnForward(seq, v);
    }
  }
}

void PaxosImpl::StartOnForward(int seq, std::string v) {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 1000));
    mu.unlock();
  }
}

void PaxosImpl::StartOnNewSlot(int seq, std::string v, PaxosSlot *slot,
                               int my_view) {
  std::cout << "Started StartOnNewSlot" << std::endl;
  std::unique_lock<std::mutex> slot_lock(slot->mu);
  // std::cout << "Received lock1" << std::endl;
  if (slot->status == DECIDED || is_dead) {
    return;
  }
  // prepare phase
  std::cout << "Started Prepare Phase" << std::endl;
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
  // std::cout << "Received lock2" << std::endl;
  majority_count = 0;
  reject_count = 0;
  std::cout << seq << ", " << highest_accepted_seq << std::endl;
  if (seq <= highest_accepted_seq) {
    // std::cout << "Inside if" << std::endl;
    start_on_new_slot_lock.unlock();
    while (slot->n <= slot->highest_N) {
      slot->n += num_servers;
    }
    std::cout << "trying to send prepare" << std::endl;
    std::vector<std::thread> threads;
    for (const auto &pair : paxos_stubs_map) {
      // std::cout << "Inside loop for server " << pair.first << std::endl;
      PrepareRequest prepare_request;
      prepare_request.set_seq(seq);
      prepare_request.set_proposal_number(slot->n);
      prepare_request.set_sender_id(me);
      prepare_request.set_latest_done(done[me]);
      // threads.emplace_back(
      //     [&](PrepareRequest prepare_request) {
      ClientContext context;
      PrepareResponse prepare_response;
      std::cout << "trying send of prepare for pair.second " << pair.first
                << std::endl;
      Status status =
          pair.second->Prepare(&context, prepare_request, &prepare_response);
      std::cout << "completed send of prepare for pair.second " << pair.first
                << std::endl;
      if (status.ok()) {
        int port = getPortNumber(pair.first);
        int i = port - first_port;
        // Forget(i, prepare_response.latest_done());
        if (prepare_response.status() == "OK") {
          std::cout << "Increment majority" << std::endl;
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
            break;
          }
          if (na_count_map[highest_na] > paxos_stubs_map.size() / 2) {
            is_decided_prep = true;
            decided_V = highest_va;
            break;
          }
        }
      }
      if (reject_count > paxos_stubs_map.size() / 2 ||
          majority_count > paxos_stubs_map.size() / 2 ||
          na_count_map[highest_na] > paxos_stubs_map.size() / 2) {
        break;
      }
      // },
      // prepare_request);
    }
    // for (auto &t : threads) {
    //   t.join();
    // }
    std::cout << "Completed prepare phase threads" << std::endl;
    std::cout << decided_V << ", " << highest_va << std::endl;
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

  if (highest_va == "") {
    highest_va = v;
  }

  // accept phase
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
          int port = getPortNumber(pair.first);
          int i = port - first_port;
          // Forget(i, accept_response.latest_done());
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

      Status status =
          pair.second->Learn(&context, decide_request, &decide_response);

      if (status.ok()) {
        int port = getPortNumber(pair.first);
        int i = port - first_port;
        // Forget(i, decide_response.latest_done());
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }
}

Status PaxosImpl::Heartbeat(ServerContext *context,
                            const HeartbeatRequest *request,
                            HeartbeatResponse *response) {
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

void PaxosImpl::SendHeartbeats() {
  mu.lock();
  if (view % group_size != me) {
    mu.unlock();
    return;
  }

  HeartbeatRequest request;
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
      ClientContext context;
      HeartbeatResponse response;
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

void PaxosImpl::DetectLeaderFailure() {
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

    // Server should make sure it informs all other server about a recent leader 
    // election and update a field persisted in DB. For all requests coming to a server,
    // the server will check for a recent leader election using this field. If it determines
    // so, it will inform the client to create its new session with the new leader and then
    // continue with further requests.
    //
    // OR
    //
    // After a new leader election, the new leader should set a field here so that it can
    // keep on rejection acquire and release requests for a while. This will allow the client
    // to send a new keep alive request to create new session in the meanwhile.
    leader_dead = true;
  }
}

void PaxosImpl::Election(int my_view, int offset) {
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

void PaxosImpl::getKeyValue(const std::string &key_value, std::string &key,
                            std::string &value) {
  size_t pos = key_value.find('#');
  if (pos == std::string::npos) {
    throw std::invalid_argument("Invalid key_value format.");
  }
  key = key_value.substr(0, pos);
  value = key_value.substr(pos + 1);
}