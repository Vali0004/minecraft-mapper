// SPDX-License-Identifier: MPL-2.0
#include "metrics_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace std;

MetricsServer::MetricsServer(const string &bind_addr, int port) {
  signal(SIGPIPE, SIG_IGN);
  sfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sfd < 0) { perror("socket"); exit(1); }
  int one = 1, zero = 0;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  string a = bind_addr;
  if (a.empty()) a = "::";
  if (inet_pton(AF_INET6, a.c_str(), &addr.sin6_addr) != 1) {
    // allow plain ipv4 by mapping it
    a = "::ffff:" + a;
    if (inet_pton(AF_INET6, a.c_str(), &addr.sin6_addr) != 1) {
      fprintf(stderr, "invalid bind address %s\n", bind_addr.c_str());
      exit(1);
    }
  }
  if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
  if (listen(sfd, 16) < 0) { perror("listen"); exit(1); }
  printf("serving http on [%s]:%d\n", bind_addr.empty() ? "::" : bind_addr.c_str(), port);
}

MetricsServer::~MetricsServer() {
  close(sfd);
}

void MetricsServer::addCollector(Collector c) {
  collectors.push_back(std::move(c));
}

void MetricsServer::addHandler(Handler h) {
  handlers.push_back(std::move(h));
}

static void writeAll(int fd, const string &s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = write(fd, s.data() + off, s.size() - off);
    if (n <= 0) return;
    off += n;
  }
}

void MetricsServer::handleClient(int fd) {
  timeval tv{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == string::npos && req.size() < 16384) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return;
    req.append(buf, n);
  }

  size_t sp1 = req.find(' ');
  size_t sp2 = req.find(' ', sp1 + 1);
  if (sp1 == string::npos || sp2 == string::npos) return;
  string method = req.substr(0, sp1);
  string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = target.find('?');
  if (q != string::npos) target.resize(q);

  string status, ctype, body, extra;
  Response r;
  bool handled = false;
  // slow clients only hold their own thread, the handlers themselves share state
  unique_lock<mutex> l(lock);
  if (method == "GET" || method == "HEAD") {
    for (auto &h : handlers) {
      if ((handled = h(target, r))) break;
    }
  }
  if (handled) {
    status = r.status; ctype = r.ctype; body = std::move(r.body); extra = r.headers;
  } else if (method != "GET" && method != "HEAD") {
    status = "405 Method Not Allowed"; ctype = "text/plain"; body = "method not allowed\n";
  } else if (target == "/metrics") {
    status = "200 OK"; ctype = "text/plain; version=0.0.4; charset=utf-8";
    for (auto &c : collectors) c(body);
  } else if (target == "/") {
    status = "200 OK"; ctype = "text/html";
    body = "<html><body><a href=\"/metrics\">metrics</a></body></html>\n";
  } else {
    status = "404 Not Found"; ctype = "text/plain"; body = "not found\n";
  }

  l.unlock();

  string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
    "\r\nContent-Length: " + to_string(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n";
  if (method != "HEAD") resp += body;
  writeAll(fd, resp);
}

void MetricsServer::serve() {
  while (true) {
    int fd = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) { perror("accept"); continue; }
    thread([this, fd] {
      handleClient(fd);
      close(fd);
    }).detach();
  }
}
