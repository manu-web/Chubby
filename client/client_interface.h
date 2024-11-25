#ifndef KV739CLIENT_H
#define KV739CLIENT_H

#include "Chubby.grpc.pb.h"
#include "Chubby.pb.h"
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <chrono>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using chubby::Chubby;
using chubby::AcquireLockRequest;
using chubby::AcquireLockResponse;
using chubby::ReleaseLockRequest;
using chubby::ReleaseLockResponse;
using chubby::TryAcquireLockRequest;
using chubby::TryAcquireLockResponse;
using chubby::KeepAliveRequest;
using chubby::KeepAliveResponse;
using chubby::Empty;

class ClientLib{
    std::unordered_set<std::string> chubby_cells;
    std::map<std::string, std::unique_ptr<chubby::Chubby::Stub>> chubby_map;
    std::string current_leader = "UNKNOWN";
    uint32_t client_lease_timeout = 0;
    uint32_t grace_period = std::chrono::seconds(45).count();
    int connection_try_limit = 5;
    std::mutex leader_update_mutex;
    int client_id;

    std::string chubby_cell_handling_request_finder();

    public:

    void set_client_id(int client_id){
        this->client_id = client_id;
    };

    int chubby_init(char* config_file);

    int chubby_shutdown();

    int chubby_lock(std::string &path, std::string &locking_mode);

    int chubby_unlock(std::string &path);

    int send_keep_alive();
};

#endif