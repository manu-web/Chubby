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

ClientLib client_lib;

std::string generate_random_string(size_t length, std::mt19937 &gen) {
  std::uniform_int_distribution<> distrib(0, characters.size() - 1);
  std::string random_string;
  random_string.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    random_string += characters[distrib(gen)];
  }
  return random_string;
}

void send_keep_alive() {
  while (true) {
    int response = client_lib.send_keep_alive();
    if (response != 0) {
      std::cerr << "Keep Alive failed" << std::endl;
    }
  }
}

void send_requests(perf_metrics *metrics, int num_requests) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> path_distrib(1, 1024);

  for (int i = 0; i < num_requests; i++) {
    std::string lock_path = "tmp/" + std::to_string(path_distrib(gen));
    high_resolution_clock::time_point start_time = high_resolution_clock::now();
    int response = client_lib.chubby_lock(lock_path, "SHARED");
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
        std::thread(send_requests, &metrics, num_requests));
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
  if (argc == 2) {
    num_requests = std::atoi(argv[1]);
  } else if (argc == 3) {
    num_requests = std::atoi(argv[1]);
    config_file = argv[2];
  }

  if (client_lib.chubby_init(config_file.data()) != 0) {
    std::cerr << "Failed to initialize client." << std::endl;
    return -1;
  }
  client_lib.set_client_id(123);

  std::thread keep_alive(send_keep_alive);
  keep_alive.detach();

  run_performance_test(num_requests, 100);

  assert(client_lib.chubby_shutdown() == 0);
  return 0;
}
