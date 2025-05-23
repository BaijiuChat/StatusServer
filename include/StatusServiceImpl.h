#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <iostream>
// gRPC 核心库
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
// Boost.Asio 异步I/O和信号处理
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>

#include "message.grpc.pb.h"
#include "ConfigMgr.h"
#include "const.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::LoginReq;
using message::LoginRsp;
using message::StatusService;

struct ChatServer {
    ChatServer() :host(""), port(""), name(""), con_count(0) {}
    ChatServer(const ChatServer& cs) :host(cs.host), port(cs.port), name(cs.name), con_count(cs.con_count) {}
    ChatServer& operator=(const ChatServer& cs) {
        if (&cs == this) {
            return *this;
        }

        host = cs.host;
        name = cs.name;
        port = cs.port;
        con_count = cs.con_count;
        return *this;
    }
    std::string host;
    std::string port;
    std::string name;
    int con_count;
};

class StatusServiceImpl final : public StatusService::Service
{
public:
    StatusServiceImpl();
    ~StatusServiceImpl(); // 添加析构函数声明

    Status GetChatServer(ServerContext* context, const GetChatServerReq* request,
        GetChatServerRsp* reply) override;
    Status Login(ServerContext* context, const LoginReq* request,
        LoginRsp* reply);

private:
    void insertToken(int uid, const std::string& token);
    ChatServer getChatServer();
    void updateServerConnectionCount(const std::string& serverName, int delta);

    std::unordered_map<std::string, ChatServer> _servers;
    std::mutex _server_mutex;
    std::unordered_map<int, std::string> _tokens;
    std::mutex _token_mutex;
};