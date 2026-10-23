#pragma once

#include <functional>
#include <string>
#include <vector>

// minimal prometheus-style HTTP exporter, any exporter can register collectors
// each collector appends text exposition format lines to `out` on every scrape of /metrics
class MetricsServer {
public:
  using Collector = std::function<void(std::string &out)>;

  MetricsServer(const std::string &bind_addr, int port);
  ~MetricsServer();
  void addCollector(Collector c);
  // blocks forever, serving one request at a time
  void serve();
private:
  void handleClient(int fd);
  int sfd;
  std::vector<Collector> collectors;
};
