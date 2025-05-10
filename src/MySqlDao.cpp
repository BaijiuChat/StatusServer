#include "MySqlDao.h"

SqlConnection::SqlConnection(sql::Connection* con, int64_t lasttime)
    :_con(con), _last_oper_time(lasttime)
{
}

MySqlPool::MySqlPool(const std::string& url, const std::string& user, const std::string& pass, const std::string& schema, int poolSize)
    :url_(url), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false)
{
    try {
        for (int i = 0; i < poolSize_; ++i) {
            // Connector C++ 使用单例模式来创建驱动实例，所以需要用指针
            sql::mysql::MySQL_Driver* driver = sql::mysql::get_driver_instance();
            sql::Connection* con = driver->connect(url_, user_, pass_);
            con->setSchema(schema_);
            auto CurrentTime = std::chrono::system_clock::now().time_since_epoch();
            long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(CurrentTime).count();
            // 记录连接和最后操作时间
            pool_.push(std::make_unique<SqlConnection>(con, timestamp));
        }
        _check_thread = std::thread([this]() {
            while (!b_stop_) {
                checkConnection();
                std::this_thread::sleep_for(std::chrono::seconds(60)); // 每60秒检查一次
            }
            });
        _check_thread.detach(); // 分离线程，交给系统管理
    }
    catch (sql::SQLException& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        // 清理已创建的连接
        Close();
        throw; // 重新抛出异常，让调用者知道初始化失败
    }
    catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        Close();
        throw;
    }
    catch (...) {
        std::cerr << "Unknown error" << std::endl;
        Close();
        throw;
    }
}

void MySqlPool::checkConnection()
{
    std::lock_guard<std::mutex> guard(mutex_);

    // 创建临时队列存储连接
    std::queue<std::unique_ptr<SqlConnection>> tempPool;
    auto currentTime = std::chrono::system_clock::now().time_since_epoch();
    long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();

    // 检查所有连接
    while (!pool_.empty()) {
        auto con = std::move(pool_.front());
        pool_.pop();

        // 如果60秒内有操作过，跳过检查
        if (con->_last_oper_time + 60 > timestamp) {
            tempPool.push(std::move(con));
            continue;
        }

        try {
            std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT 1"));
            pstmt->execute();
            con->_last_oper_time = timestamp; // 更新最后操作时间
            std::cout << "MySQL链接存活，操作时间为：" << timestamp << std::endl;
            tempPool.push(std::move(con));
        }
        catch (sql::SQLException& e) {
            std::cerr << "MySQL链接失效，重新创建连接，错误为：" << e.what() << std::endl;
            try {
                sql::mysql::MySQL_Driver* driver = sql::mysql::get_driver_instance();
                sql::Connection* newCon = driver->connect(url_, user_, pass_);
                newCon->setSchema(schema_);
                con.reset(new SqlConnection(newCon, timestamp)); // 创建新的连接对象
                tempPool.push(std::move(con));
            }
            catch (std::exception& ex) {
                std::cerr << "重新创建连接失败: " << ex.what() << std::endl;
                // 连接无法恢复，池大小减少
            }
        }
        catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            // 尝试保留连接，避免池耗尽
            tempPool.push(std::move(con));
        }
        catch (...) {
            std::cerr << "Unknown error" << std::endl;
            // 尝试保留连接，避免池耗尽
            tempPool.push(std::move(con));
        }
    }

    // 将临时池中的连接放回主池
    pool_.swap(tempPool);

    // 如果池大小减少，尝试补充
    while (pool_.size() < poolSize_) {
        try {
            sql::mysql::MySQL_Driver* driver = sql::mysql::get_driver_instance();
            sql::Connection* newCon = driver->connect(url_, user_, pass_);
            newCon->setSchema(schema_);
            auto CurrentTime = std::chrono::system_clock::now().time_since_epoch();
            long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(CurrentTime).count();
            pool_.push(std::make_unique<SqlConnection>(newCon, timestamp));
            std::cout << "补充新的MySQL连接到池中" << std::endl;
        }
        catch (std::exception& e) {
            std::cerr << "补充连接失败: " << e.what() << std::endl;
            break; // 不能创建新连接，停止尝试
        }
    }
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (b_stop_) {
        return nullptr; // 如果池已经停止，返回空指针
    }

    // 等待可用连接
    bool success = cond_.wait_for(lock, std::chrono::seconds(30), [this] {
        return b_stop_ || !pool_.empty();
        });

    if (!success || b_stop_ || pool_.empty()) {
        // 等待超时或池已停止
        return nullptr;
    }

    std::unique_ptr<SqlConnection> con = std::move(pool_.front());
    pool_.pop();

    // 更新最后操作时间
    auto currentTime = std::chrono::system_clock::now().time_since_epoch();
    long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
    con->_last_oper_time = timestamp;

    return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
    if (!con) {
        return; // 忽略空连接
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (b_stop_) {
        return; // 如果池子已经停止，不用返回连接
    }

    pool_.push(std::move(con));
    cond_.notify_one(); // 通知一个等待的线程
}

void MySqlPool::Close()
{
	{ // 作用域锁，确保线程安全
        std::unique_lock<std::mutex> lock(mutex_);
        if (b_stop_) return; // 防止重复关闭
        b_stop_ = true; // 设置停止标志
    }

    cond_.notify_all(); // 通知所有等待的线程

    // 清理所有连接
    std::unique_lock<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
}

MySqlPool::~MySqlPool()
{
    Close(); // 确保资源被正确释放
}

/// MySqlDao函数
/// //////////////////////////////////////////////////////////////
/// MySqlDao函数

MySqlDao::MySqlDao()
{
    auto& config = ConfigMgr::Inst();
    const auto& host = config["MySQL"]["Host"];
    const auto& port = config["MySQL"]["Port"];
    const auto& user = config["MySQL"]["User"];
    const auto& pwd = config["MySQL"]["Passwd"];
    const auto& schema = config["MySQL"]["Schema"];
    const auto& poolSize = std::stoi(config["MySQL"]["PoolSize"]);
	pool_.reset(new MySqlPool(host + ":" + port, user, pwd, schema, poolSize));
}

MySqlDao::~MySqlDao()
{
	pool_->Close();
}

int MySqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
    auto con = pool_->getConnection();
    if (con == nullptr) {
        pool_->returnConnection(std::move(con));
        return -2;
    } // 获取连接失败，返回特定错误码

    try
    {
        std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("CALL reg_user(?,?,?,@result)"));
        pstmt->setString(1, name);
        pstmt->setString(2, email);
        pstmt->setString(3, pwd);
        pstmt->execute();

        std::unique_ptr<sql::Statement> stmtResult(con->_con->createStatement());
        std::unique_ptr<sql::ResultSet> res(stmtResult->executeQuery("SELECT @result AS result"));

        int result = -3; // 默认错误码，表示未获取到结果
        if (res->next()) {
            result = res->getInt("result");
            std::cout << "用户注册结果: " << result << std::endl;
        }

        // 无论是否获取到结果，都返回连接并返回结果
        pool_->returnConnection(std::move(con));
        return result;
    }
    catch (sql::SQLException& e)
    {
        // 先记录错误
        std::cerr << "SQLException: " << e.what();
        std::cerr << " (MySQL error code: " << e.getErrorCode();
        std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
        // 确保连接被返回到池中
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -1; // SQL异常错误码
    }
    catch (std::exception& e)
    {
        std::cerr << "标准异常: " << e.what() << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -4; // 一般异常错误码
    }
    catch (...)
    {
        std::cerr << "未知异常" << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -5; // 未知异常错误码
    }
}

int MySqlDao::CheckEmail(const std::string& name, const std::string& email)
{
    auto con = pool_->getConnection();
    if (con == nullptr) {
        std::cerr << "无法获取数据库连接" << std::endl;
        return -2; // 数据库连接失败
    }

    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT email FROM user WHERE name = ?"));
        pstmt->setString(1, name);
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        // 如果没有结果，说明用户不存在
        if (!res->next()) {
            std::cout << "用户 " << name << " 不存在" << std::endl;
            pool_->returnConnection(std::move(con));
            return -6; // 用户不存在
        }

        // 检查邮箱是否匹配
        std::string db_email = res->getString("email");
        std::cout << "数据库邮箱: " << db_email << ", 请求邮箱: " << email << std::endl;
        if (email != db_email) {
            pool_->returnConnection(std::move(con));
            return -7; // 邮箱不匹配
        }

        // 匹配成功
        pool_->returnConnection(std::move(con));
        return 0; // 成功
    }
    catch (sql::SQLException& e)
    {
        // 记录错误
        std::cerr << "SQLException: " << e.what();
        std::cerr << " (MySQL error code: " << e.getErrorCode();
        std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;

        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -1; // SQL异常
    }
    catch (std::exception& e)
    {
        std::cerr << "标准异常: " << e.what() << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -4; // 一般异常
    }
    catch (...)
    {
        std::cerr << "未知异常" << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -5; // 未知异常
    }
}

int MySqlDao::UpdatePwd(const std::string& name, const std::string& newpwd)
{
    auto con = pool_->getConnection();
    if (con == nullptr) {
        std::cerr << "无法获取数据库连接" << std::endl;
        return -2; // 数据库连接失败
    }

    try {
        // 准备查询语句
        std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("UPDATE user SET pwd = ? WHERE name = ?"));
        // 绑定参数
        pstmt->setString(1, newpwd);
        pstmt->setString(2, name);

        // 执行更新
        int updateCount = pstmt->executeUpdate();
        std::cout << "Updated rows: " << updateCount << std::endl;

        // 检查是否有行被更新
        if (updateCount == 0) {
            std::cout << "没有找到用户: " << name << std::endl;
            pool_->returnConnection(std::move(con));
            return -6; // 用户不存在或者没有行被更新
        }

        pool_->returnConnection(std::move(con));
        return 0; // 成功
    }
    catch (sql::SQLException& e)
    {
        // 记录错误
        std::cerr << "SQLException: " << e.what();
        std::cerr << " (MySQL error code: " << e.getErrorCode();
        std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;

        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -1; // SQL异常
    }
    catch (std::exception& e)
    {
        std::cerr << "标准异常: " << e.what() << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -4; // 一般异常
    }
    catch (...)
    {
        std::cerr << "未知异常" << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -5; // 未知异常
    }
}

int MySqlDao::CheckLogin(const std::string& email, const std::string& pwd, UserInfo& userInfo)
{
    auto con = pool_->getConnection();
    if (con == nullptr) {
        std::cerr << "无法获取数据库连接" << std::endl;
        return -2; // 数据库连接失败
    }

    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT pwd FROM user WHERE email = ?"));
        pstmt->setString(1, email);
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        // 如果没有结果，说明用户不存在
        if (!res->next()) {
            std::cout << "用户 " << email << " 不存在" << std::endl;
            pool_->returnConnection(std::move(con));
            return -1; // 用户不存在
        }

        // 检查密码是否匹配
        std::string db_pwd = res->getString("pwd");
        std::cout << "数据库密码: " << db_pwd << ", 请求密码: " << pwd << std::endl;
        if (pwd != db_pwd) {
            pool_->returnConnection(std::move(con));
            return -3; // 密码不匹配
        }

        // 匹配成功
        userInfo.name = res->getString("name");;
        userInfo.email = email;
        userInfo.uid = res->getInt("uid");
        userInfo.pwd = db_pwd;
        pool_->returnConnection(std::move(con));
        return 0; // 成功
    }
    catch (sql::SQLException& e)
    {
        // 记录错误
        std::cerr << "SQLException: " << e.what();
        std::cerr << " (MySQL error code: " << e.getErrorCode();
        std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;

        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -6; // SQL异常
    }
    catch (std::exception& e)
    {
        std::cerr << "标准异常: " << e.what() << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -4; // 一般异常
    }
    catch (...)
    {
        std::cerr << "未知异常" << std::endl;
        try {
            if (con) {
                pool_->returnConnection(std::move(con));
            }
        }
        catch (...) {
            std::cerr << "返回连接到池中时发生异常" << std::endl;
        }
        return -5; // 未知异常
    }
}