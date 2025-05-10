#include "StatusServiceImpl.h"
#include <boost/uuid/uuid.hpp>  
#include <boost/uuid/uuid_generators.hpp>  
#include <boost/uuid/uuid_io.hpp>
#include <iostream>
#include <chrono>
#include <iomanip>

// 获取当前格式化时间字符串，用于日志输出
std::string getCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

    // 安全转换本地时间（跨平台）
#ifdef _WIN32
    localtime_s(&tm, &time_t_now);  // Windows 使用 localtime_s
#else
    localtime_r(&time_t_now, &tm);  // Linux/macOS 使用 localtime_r
#endif

    std::stringstream ss;
    ss << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S]");
    return ss.str();
}

std::string generate_unique_string() {
    // 创建UUID对象  
    boost::uuids::uuid uuid = boost::uuids::random_generator()();

    // 将UUID转换为字符串  
    std::string unique_string = to_string(uuid);

    std::cout << getCurrentTimeStr() << " 生成唯一令牌: " << unique_string << std::endl;
    return unique_string;
}

Status StatusServiceImpl::GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply)
{
    std::cout << getCurrentTimeStr() << " 收到获取聊天服务器请求，用户ID: " << request->uid() << std::endl;

    const auto& server = getChatServer();
    reply->set_host(server.host);
    reply->set_port(server.port);
    reply->set_error(ErrorCodes::SUCCESS);
    reply->set_token(generate_unique_string());

    insertToken(request->uid(), reply->token());

    std::cout << getCurrentTimeStr() << " 分配聊天服务器: " << server.name
        << " (地址: " << server.host << ":" << server.port
        << ")，当前连接数: " << server.con_count << std::endl;

    // 更新服务器连接数
    updateServerConnectionCount(server.name, 1);

    return Status::OK;
}

StatusServiceImpl::StatusServiceImpl()
{
    std::cout << getCurrentTimeStr() << " 初始化状态服务实现..." << std::endl;

    auto& cfg = ConfigMgr::Inst();

    // 初始化第一个聊天服务器
    ChatServer server;
    server.port = cfg["ChatServer1"]["Port"];
    server.host = cfg["ChatServer1"]["Host"];
    server.name = cfg["ChatServer1"]["Name"];
    server.con_count = 0;
    _servers[server.name] = server;
    std::cout << getCurrentTimeStr() << " 添加聊天服务器: " << server.name
        << " (地址: " << server.host << ":" << server.port << ")" << std::endl;

    // 初始化第二个聊天服务器
    server.port = cfg["ChatServer2"]["Port"];
    server.host = cfg["ChatServer2"]["Host"];
    server.name = cfg["ChatServer2"]["Name"];
    server.con_count = 0;
    _servers[server.name] = server;
    std::cout << getCurrentTimeStr() << " 添加聊天服务器: " << server.name
        << " (地址: " << server.host << ":" << server.port << ")" << std::endl;

    std::cout << getCurrentTimeStr() << " 状态服务初始化完成，共注册 " << _servers.size() << " 个聊天服务器" << std::endl;
}

StatusServiceImpl::~StatusServiceImpl() {
    std::cout << getCurrentTimeStr() << " 状态服务析构中..." << std::endl;

    std::lock_guard<std::mutex> token_guard(_token_mutex);
    std::lock_guard<std::mutex> server_guard(_server_mutex);

    std::cout << getCurrentTimeStr() << " 清理 " << _tokens.size() << " 个令牌记录" << std::endl;
    _tokens.clear();

    std::cout << getCurrentTimeStr() << " 清理 " << _servers.size() << " 个服务器记录" << std::endl;
    _servers.clear();

    std::cout << getCurrentTimeStr() << " 状态服务析构完成" << std::endl;
}

ChatServer StatusServiceImpl::getChatServer()
{
    std::lock_guard<std::mutex> guard(_server_mutex);

    // 获取负载最低的服务器
    auto iter = _servers.begin();
    if (iter == _servers.end()) {
        std::cerr << getCurrentTimeStr() << " 错误：没有可用的聊天服务器！" << std::endl;
        // 返回一个空服务器对象
        return ChatServer();
    }

    auto minServer = iter->second;
    ++iter;

    for (; iter != _servers.end(); ++iter) {
        if (iter->second.con_count < minServer.con_count) {
            minServer = iter->second;
        }
    }

    return minServer;
}

void StatusServiceImpl::updateServerConnectionCount(const std::string& serverName, int delta) {
    std::lock_guard<std::mutex> guard(_server_mutex);

    auto iter = _servers.find(serverName);
    if (iter != _servers.end()) {
        iter->second.con_count += delta;
        std::cout << getCurrentTimeStr() << " 更新服务器 " << serverName
            << " 连接数: " << (iter->second.con_count - delta)
            << " -> " << iter->second.con_count << std::endl;
    }
    else {
        std::cerr << getCurrentTimeStr() << " 错误：尝试更新不存在的服务器: " << serverName << std::endl;
    }
}

Status StatusServiceImpl::Login(ServerContext* context, const LoginReq* request, LoginRsp* reply)
{
    auto uid = request->uid();
    auto token = request->token();

    std::cout << getCurrentTimeStr() << " 收到登录请求，用户ID: " << uid << std::endl;

    std::lock_guard<std::mutex> guard(_token_mutex);
    auto iter = _tokens.find(uid);
    if (iter == _tokens.end()) {
        std::cout << getCurrentTimeStr() << " 登录失败：无效的用户ID " << uid << std::endl;
        reply->set_error(ErrorCodes::UidInvalid);
        return Status::OK;
    }

    if (iter->second != token) {
        std::cout << getCurrentTimeStr() << " 登录失败：无效的Token，用户ID: " << uid << std::endl;
        reply->set_error(ErrorCodes::TokenInvalid);
        return Status::OK;
    }

    std::cout << getCurrentTimeStr() << " 登录成功：用户ID " << uid << std::endl;
    reply->set_error(ErrorCodes::SUCCESS);
    reply->set_uid(uid);
    reply->set_token(token);
    return Status::OK;
}

void StatusServiceImpl::insertToken(int uid, const std::string& token)
{
    std::lock_guard<std::mutex> guard(_token_mutex);

    auto iter = _tokens.find(uid);
    if (iter != _tokens.end()) {
        std::cout << getCurrentTimeStr() << " 更新用户 " << uid << " 的令牌" << std::endl;
    }
    else {
        std::cout << getCurrentTimeStr() << " 为用户 " << uid << " 创建新令牌" << std::endl;
    }

    _tokens[uid] = token;
}