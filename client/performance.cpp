#include "client_interface.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

using std::chrono::duration;
using std::chrono::high_resolution_clock;

struct perf_metrics {
  std::atomic<int> total_requests{0};
  std::atomic<int> successful_requests{0};
  std::atomic<long long> total_latency_ns{0};
};
static const std::string characters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

std::string generate_random_string(size_t length, std::mt19937 &gen) {
  std::uniform_int_distribution<> distrib(0, characters.size() - 1);
  std::string random_string;
  random_string.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    random_string += characters[distrib(gen)];
  }
  return random_string;
}

std::vector<ClientLib *> clients;

void send_keep_alive(ClientLib *client) {
  while (true) {
    int response = client->send_keep_alive();
    if (response != 0) {
      std::cerr << "Keep Alive failed" << std::endl;
    }
  }
}

void send_requests(perf_metrics *metrics, int num_requests,
                   ClientLib *client_lib) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> path_distrib(1, 1024);

  for (int i = 0; i < num_requests; i++) {
    std::string lock_path = "tmp/" + std::to_string(path_distrib(gen));
    high_resolution_clock::time_point start_time = high_resolution_clock::now();
    int response = client_lib->chubby_lock(lock_path, "SHARED");
    high_resolution_clock::time_point end_time = high_resolution_clock::now();

    duration<long long, std::nano> duration_nano =
        std::chrono::duration_cast<duration<long long, std::nano>>(end_time -
                                                                   start_time);
    long long latency_ns = duration_nano.count();

    metrics->total_latency_ns += latency_ns;
    if (response == 0) {
      metrics->successful_requests++;
    }
    metrics->total_requests++;
  }
}

void run_performance_test(int num_requests, int num_clients) {
  perf_metrics metrics;

  std::vector<std::thread> client_threads;

  for (int i = 0; i < num_clients; i++) {
    client_threads.push_back(
        std::thread(send_requests, &metrics, num_requests, clients[i]));
  }

  for (auto &t : client_threads) {
    t.join();
  }

  // send_requests(&metrics, num_requests);

  double throughput = metrics.total_requests / (metrics.total_latency_ns / 1e9);
  double average_latency_ms =
      (metrics.total_latency_ns / 1e6) / metrics.total_requests;

  std::cout << "Total requests: " << metrics.total_requests << std::endl;
  std::cout << "Successful requests: " << metrics.successful_requests
            << std::endl;
  std::cout << "Failed requests: "
            << metrics.total_requests - metrics.successful_requests
            << std::endl;
  std::cout << "Throughput: " << throughput << " rps" << std::endl;
  std::cout << "Average latency: " << average_latency_ms << " ms" << std::endl;
}

int main(int argc, char **argv) {
  std::string config_file("3.config");
  int num_requests = 100000;
  int num_clients = 8;
  if (argc >= 2) {
    num_requests = std::atoi(argv[1]);
  }
  if (argc >= 3) {
    num_clients = std::atoi(argv[2]);
  }
  if (argc >= 4) {
    config_file = argv[3];
  }

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> client_ids(1, 1024);

  for (int i = 0; i < num_clients; i++) {
    ClientLib *c = new ClientLib();
    c->chubby_init(config_file.data());
    c->set_client_id(client_ids(gen));
    clients.push_back(c);
    std::thread keep_alive(send_keep_alive, c);
    keep_alive.detach();
  }

  sleep(12);
  run_performance_test(num_requests, num_clients);

  return 0;
}
