#include <iostream>
#include <string> 
#include <memory> 
#include <thread>
#include <atomic>
#include <chrono>
#include "StatusServiceImpl.h"

// 添加全局变量用于控制服务退出
std::atomic<bool> g_running{ true };

void RunServer() {
    std::cout << "正在初始化状态服务器..." << std::endl;
    auto& cfg = ConfigMgr::Inst();

    std::string server_address(cfg["StatusServer"]["Host"] + ":" + cfg["StatusServer"]["Port"]);
    std::cout << "配置读取完成，服务器地址: " << server_address << std::endl;

    StatusServiceImpl service;
    std::cout << "服务实例已创建" << std::endl;

    grpc::ServerBuilder builder;
    // 监听端口和添加服务
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::cout << "服务注册完成" << std::endl;

    // 构建并启动gRPC服务器
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "服务器启动失败！" << std::endl;
        return;
    }
    std::cout << "状态服务器启动成功，正在监听：" << server_address << std::endl;

    // 创建Boost.Asio的io_context
    boost::asio::io_context io_context;
    // 创建signal_set用于捕获SIGINT和SIGTERM
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);

    // 设置异步等待信号
    signals.async_wait([&server](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            std::cout << "收到信号 " << signal_number << "，正在优雅关闭服务器..." << std::endl;
            g_running = false;
            server->Shutdown(); // 优雅地关闭服务器
        }
        });

    // 在单独的线程中运行io_context
    std::thread io_thread([&io_context]() {
        std::cout << "信号处理线程已启动" << std::endl;
        io_context.run();
        std::cout << "信号处理线程已退出" << std::endl;
        });

    // 添加一个健康检查线程
    std::thread health_thread([]() {
        std::cout << "健康检查线程已启动" << std::endl;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (g_running) {
                std::cout << "服务器运行正常，当前时间: "
                    << std::chrono::system_clock::now().time_since_epoch().count()
                    << std::endl;
            }
        }
        std::cout << "健康检查线程已退出" << std::endl;
        });

    std::cout << "等待服务器处理请求..." << std::endl;
    // 等待服务器关闭
    server->Wait();

    std::cout << "服务器已停止接收新请求" << std::endl;
    g_running = false;
    io_context.stop(); // 停止io_context

    // 等待线程结束
    if (io_thread.joinable()) {
        io_thread.join();
    }
    if (health_thread.joinable()) {
        health_thread.join();
    }

    std::cout << "所有线程已安全退出，服务器完全关闭" << std::endl;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8); // 输出编码
    SetConsoleCP(CP_UTF8); // 输入编码
    std::cout << "状态服务器启动中..." << std::endl;

    try {
        RunServer();
    }
    catch (std::exception const& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    catch (...) {
        std::cerr << "发生未知异常" << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "状态服务器正常退出" << std::endl;
    return 0;
}