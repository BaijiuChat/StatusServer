#include "StatusServiceImpl.h"
#include <boost/uuid/uuid.hpp>  
#include <boost/uuid/uuid_generators.hpp>  
#include <boost/uuid/uuid_io.hpp>  

std::string generate_unique_string() {  
   // 创建UUID对象  
   boost::uuids::uuid uuid = boost::uuids::random_generator()();

   // 将UUID转换为字符串  
   std::string unique_string = to_string(uuid);  

   return unique_string;  
}

Status StatusServiceImpl::GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply)
{
    std::string prefix("白久服务器接收到了：");
    const auto& server = getChatServer();
    reply->set_host(server.host);
    reply->set_port(server.port);   
    reply->set_error(ErrorCodes::SUCCESS);
    reply->set_token(generate_unique_string());
    insertToken(request->uid(), reply->token());
    // gRPC底层在我们设置完后会自动发送回包
    return Status::OK;
}

StatusServiceImpl::StatusServiceImpl()
{
    auto& cfg = ConfigMgr::Inst();
    ChatServer server;
    server.port = cfg["ChatServer1"]["Port"];
    server.host = cfg["ChatServer1"]["Host"];
    server.name = cfg["ChatServer1"]["Name"];
    server.con_count = 0;
    _servers[server.name] = server;
    server.port = cfg["ChatServer2"]["Port"];
    server.host = cfg["ChatServer2"]["Host"];
    server.name = cfg["ChatServer2"]["Name"];
    server.con_count = 0;
    _servers[server.name] = server;
}

ChatServer StatusServiceImpl::getChatServer() 
{
    std::lock_guard<std::mutex> guard(_server_mutex);
    // 获取负载最低的服务器
    auto minServer = _servers.begin()->second;
    for (const auto& server : _servers) {
        if (server.second.con_count < minServer.con_count) {
            minServer = server.second;
        }
    }
    return minServer;
}

Status StatusServiceImpl::Login(ServerContext* context, const LoginReq* request, LoginRsp* reply)
{
    auto uid = request->uid();
    auto token = request->token();
    std::lock_guard<std::mutex> guard(_token_mutex);
    auto iter = _tokens.find(uid);
    if (iter == _tokens.end()) {
        reply->set_error(ErrorCodes::UidInvalid);
        return Status::OK;
    }
    if (iter->second != token) {
        reply->set_error(ErrorCodes::TokenInvalid);
        return Status::OK;
    }
    reply->set_error(ErrorCodes::SUCCESS);
    reply->set_uid(uid);
    reply->set_token(token);
    return Status::OK;
}

void StatusServiceImpl::insertToken(int uid, std::string token)
{
    std::lock_guard<std::mutex> guard(_token_mutex);
    _tokens[uid] = token;
}