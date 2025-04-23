#include "StatusServiceImpl.h"
#include <boost/uuid/uuid.hpp>  
#include <boost/uuid/uuid_generators.hpp>  
#include <boost/uuid/uuid_io.hpp>  

std::string generate_unique_string() {  
   // 创建UUID对象  
   boost::uuids::random_generator generator;  
   boost::uuids::uuid uuid = generator();  

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
    server.name = cfg["ChatServer1"][]

    server.port = cfg["ChatServer2"]["Port"];
    server.host = cfg["ChatServer2"]["Host"];
    _servers.push_back(server);
}