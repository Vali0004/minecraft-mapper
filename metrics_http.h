// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

// minimal prometheus-style HTTP exporter, any exporter can register collectors, and extra handlers
// each collector appends text exposition format lines to `out` on every scrape of /metrics
class MetricsServer {
public:
  using Collector = std::function<void(std::string &out)>;
  // returns false if it doesn't handle `target`, otherwise fills status ("200 OK"), content type and body
  // `headers` is appended verbatim to the response headers, each line ending in \r\n
  struct Response { std::string status, ctype, body, headers; };
  using Handler = std::function<bool(const std::string &target, Response &resp)>;

  MetricsServer(const std::string &bind_addr, int port);
  ~MetricsServer();
  void addCollector(Collector c);
  // handlers are tried in order before the built in routes
  void addHandler(Handler h);
  // blocks forever. every connection gets a thread, collectors and handlers still run one at a time
  void serve();
private:
  void handleClient(int fd);
  int sfd;
  std::vector<Collector> collectors;
  std::vector<Handler> handlers;
  std::mutex lock;
};
