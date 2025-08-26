# iApp框架完整技术文档

## 1. 框架概述

iApp是一个基于C++的进程模型基础框架，旨在为开发者提供快速创建服务进程的能力。通过继承iApp基类并实现几个核心回调函数，开发者可以轻松构建一个功能完整的独立进程应用。

### 1.1 核心特性

- **简单易用**：只需继承iApp类并实现5个核心回调函数
- **配置管理**：支持JSON格式配置文件，具备热加载功能
- **信号处理**：完善的信号处理机制，支持可靠信号与不可靠信号
- **命令行解析**：内置命令行参数解析，支持业务层自定义参数
- **终端接口**：支持标准输入输出和telnet远程操作
- **定时任务**：可配置的定时执行机制
- **线程安全**：多线程环境下的安全操作

### 1.2 架构设计

```
┌─────────────────────────────────────────────────────────┐
│                    用户应用层                              │
│              (继承iApp实现回调函数)                        │
├─────────────────────────────────────────────────────────┤
│                    iApp框架核心                           │
├─────────────────────────────────────────────────────────┤
│  ConfigManager  │ SignalHandler │ CommandLineParser │    │
│  (配置管理)      │ (信号处理)     │ (命令行解析)       │    │
├─────────────────┼───────────────┼──────────────────┤    │
│  TerminalInterface              │  其他组件          │    │
│  (终端接口)                      │                   │    │
└─────────────────────────────────────────────────────────┘
```

## 2. 核心组件详解

### 2.1 iApp核心类

iApp是框架的核心基类，提供了完整的进程生命周期管理。

#### 2.1.1 核心回调函数

用户必须实现以下5个纯虚函数：

```cpp
/**
 * @brief 初始化回调函数
 * 在应用程序启动时调用，用于初始化资源
 * @return true表示初始化成功，false表示失败
 */
virtual bool onInit() = 0;

/**
 * @brief 定时运行回调函数
 * 按配置的时间间隔定期调用（默认10毫秒）
 * @param delta_ms 距离上次调用的时间间隔（毫秒）
 */
virtual void onTick(uint32_t delta_ms) = 0;

/**
 * @brief 结束清理回调函数
 * 在应用程序退出前调用，用于清理资源
 */
virtual void onCleanup() = 0;

/**
 * @brief 重载回调函数
 * 收到重载信号时调用，用于重新加载配置等
 * @return true表示重载成功，false表示失败
 */
virtual bool onReload() = 0;

/**
 * @brief 消息处理主循环回调函数
 * 在主循环中处理各种消息和事件
 * @return true表示继续运行，false表示退出
 */
virtual bool onMessageLoop() = 0;
```

#### 2.1.2 框架提供的辅助函数

```cpp
// 注册命令行参数
void registerCommandLineArg(const std::string& name, 
                           const std::string& description,
                           const std::string& default_value,
                           std::function<void(const std::string&)> callback = nullptr);

// 获取命令行参数值
std::string getCommandLineArg(const std::string& name) const;

// 注册终端命令
void registerTerminalCommand(const std::string& command,
                           const std::string& description,
                           std::function<std::string(const std::vector<std::string>&)> callback);

// 设置定时器间隔
void setTickInterval(uint32_t interval_ms);

// 获取配置上下文
ConfigManager& getContext();
```

### 2.2 ConfigManager配置管理器

ConfigManager负责配置文件的读取、写入和热加载，支持JSON格式。

#### 2.2.1 主要功能

- **JSON格式支持**：使用标准JSON格式存储配置
- **热加载机制**：自动监控配置文件变化并重新加载
- **层级访问**：支持使用点号分隔的多级键访问
- **事件通知**：配置变更时的事件通知机制
- **线程安全**：多线程环境下的安全访问

#### 2.2.2 核心接口

```cpp
// 加载配置文件
bool loadConfig(const std::string& config_file, bool auto_create = true);

// 保存配置文件
bool saveConfig(const std::string& config_file = "");

// 启用热加载
void enableHotReload(bool enabled = true, int check_interval_ms = 1000);

// 获取配置值（支持多种类型）
template<typename T>
T getValue(const std::string& key, const T& default_value) const;

// 设置配置值
template<typename T>
void setValue(const std::string& key, const T& value, bool notify = true);

// 检查配置键是否存在
bool hasKey(const std::string& key) const;
```

#### 2.2.3 配置文件示例

```json
{
    "app": {
        "name": "ExampleApp",
        "version": "1.0.0",
        "tick_interval_ms": 1000
    },
    "service": {
        "name": "ExampleService",
        "port": 8080,
        "max_connections": 100,
        "timeout_ms": 5000
    },
    "logging": {
        "level": "info",
        "file": "app.log",
        "max_size_mb": 10,
        "max_files": 5
    },
    "terminal": {
        "enable_telnet": true,
        "telnet_port": 2323,
        "max_clients": 5,
        "timeout_sec": 300
    }
}
```

### 2.3 SignalHandler信号处理器

SignalHandler提供完善的信号处理机制，区分处理可靠信号与不可靠信号。

#### 2.3.1 支持的信号类型

- **SIGTERM**：终止信号（优雅关闭）
- **SIGINT**：中断信号（Ctrl+C）
- **SIGHUP**：挂起信号（重新加载配置）
- **SIGUSR1/SIGUSR2**：用户自定义信号

#### 2.3.2 核心接口

```cpp
// 初始化信号处理
void initialize();

// 注册信号处理函数
void registerHandler(int signal, std::function<void(int)> handler);

// 等待信号（阻塞式）
int waitForSignal();

// 检查是否有待处理的信号
bool hasPendingSignals() const;
```

### 2.4 CommandLineParser命令行解析器

CommandLineParser提供强大的命令行参数解析功能，支持业务层自定义参数。

#### 2.4.1 内置参数

- `--tick-interval`：设置tick间隔（毫秒）
- `--config`：指定配置文件路径
- `--terminal`：进入终端模式
- `--telnet-port`：设置telnet端口
- `--enable-telnet`：启用telnet接口
- `--help`：显示帮助信息

#### 2.4.2 核心接口

```cpp
// 解析命令行参数
bool parse(int argc, char* argv[]);

// 注册自定义参数
void registerArg(const std::string& name, 
                const std::string& description,
                const std::string& default_value);

// 获取参数值
std::string getValue(const std::string& name) const;

// 检查是否为终端模式
bool isTerminalMode() const;

// 显示帮助信息
void showHelp() const;
```

### 2.5 TerminalInterface终端接口

TerminalInterface提供交互式终端功能，支持标准输入输出和telnet远程操作。

#### 2.5.1 支持的操作模式

- **标准IO模式**：通过标准输入输出进行交互
- **Telnet模式**：通过网络telnet连接进行远程操作
- **混合模式**：同时支持本地和远程操作

#### 2.5.2 内置命令

- `help`：显示可用命令列表
- `config`：配置管理（显示、重载、保存、设置、获取）
- `status`：显示应用程序状态
- `stop`：停止应用程序
- `tick`：获取或设置tick间隔
- `quit/exit`：退出终端

#### 2.5.3 核心接口

```cpp
// 启动终端接口
bool start(bool enable_telnet = false, uint16_t telnet_port = 23);

// 停止终端接口
void stop();

// 注册自定义命令
void registerCommand(const std::string& command,
                    const std::string& description,
                    std::function<std::string(const std::vector<std::string>&)> handler);

// 执行命令
std::string executeCommand(const std::string& command_line);
```

## 3. 使用示例

### 3.1 基本使用示例

```cpp
#include "iApp.h"
#include <iostream>
#include <chrono>

class ExampleApp : public iApp {
public:
    ExampleApp() : iApp("ExampleApp"), m_counter(0) {
        // 注册自定义命令行参数
        registerCommandLineArg("custom-param", "自定义参数示例", "default-value", 
            [this](const std::string& value) {
                std::cout << "自定义参数值已更新: " << value << std::endl;
                m_custom_param = value;
            });
        
        // 注册终端命令
        registerTerminalCommand("counter", "显示或重置计数器", 
            [this](const std::vector<std::string>& args) -> std::string {
                if (!args.empty() && args[0] == "reset") {
                    m_counter = 0;
                    return "计数器已重置为0\n";
                } else {
                    return "当前计数器值: " + std::to_string(m_counter) + "\n";
                }
            });
    }

protected:
    bool onInit() override {
        std::cout << "ExampleApp初始化..." << std::endl;
        
        // 读取配置
        m_service_name = getContext().getValue<std::string>("service.name", "DefaultService");
        m_service_port = getContext().getValue<int>("service.port", 8080);
        
        // 设置tick间隔
        int tick_interval = getContext().getValue<int>("app.tick_interval_ms", 1000);
        setTickInterval(tick_interval);
        
        return true;
    }
    
    void onTick(uint32_t delta_ms) override {
        m_counter++;
        
        if (m_counter % 10 == 0) {
            std::cout << "计数器: " << m_counter 
                      << ", 间隔: " << delta_ms << "ms" << std::endl;
        }
    }
    
    void onCleanup() override {
        std::cout << "ExampleApp清理资源..." << std::endl;
        std::cout << "总计数: " << m_counter << std::endl;
    }
    
    bool onReload() override {
        std::cout << "ExampleApp重新加载配置..." << std::endl;
        
        // 重新读取配置
        m_service_name = getContext().getValue<std::string>("service.name", "DefaultService");
        m_service_port = getContext().getValue<int>("service.port", 8080);
        
        return true;
    }
    
    bool onMessageLoop() override {
        // 处理自定义消息和事件
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

private:
    int m_counter;
    std::string m_custom_param;
    std::string m_service_name;
    int m_service_port;
};

// 使用宏定义入口点
IAPP_MAIN(ExampleApp)
```

### 3.2 编译和运行

#### 3.2.1 Makefile示例

```makefile
# iApp框架 Makefile

CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -pthread
LDFLAGS = -ljsoncpp

# 目录定义
INCLUDE_DIR = include
SRC_DIR = src
EXAMPLE_DIR = example
BIN_DIR = bin
OBJ_DIR = obj

# 创建必要的目录
$(shell mkdir -p $(BIN_DIR) $(OBJ_DIR))

# 源文件
SRC_FILES = $(wildcard $(SRC_DIR)/*.cpp)
INCLUDE_FILES = $(wildcard $(INCLUDE_DIR)/*.h)

# 目标文件
OBJ_FILES = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRC_FILES))

# 默认目标
all: example

# 示例应用程序
example: $(OBJ_FILES) $(OBJ_DIR)/ExampleApp.o
	$(CXX) $(CXXFLAGS) -o $(BIN_DIR)/ExampleApp $^ $(LDFLAGS)

# 编译源文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(INCLUDE_FILES)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 编译示例应用程序
$(OBJ_DIR)/ExampleApp.o: $(EXAMPLE_DIR)/ExampleApp.cpp $(INCLUDE_FILES)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# 清理目标
clean:
	rm -rf $(OBJ_DIR)/* $(BIN_DIR)/*

# 运行示例应用程序
run: example
	$(BIN_DIR)/ExampleApp --config $(EXAMPLE_DIR)/config.json

# 终端模式运行示例应用程序
terminal: example
	$(BIN_DIR)/ExampleApp --terminal --config $(EXAMPLE_DIR)/config.json

.PHONY: all example clean run terminal
```

#### 3.2.2 编译命令

```bash
# 编译框架和示例应用
make

# 运行示例应用（正常模式）
make run

# 运行示例应用（终端模式）
make terminal

# 清理编译文件
make clean
```

### 3.3 运行模式

#### 3.3.1 正常模式

```bash
./ExampleApp --config config.json --tick-interval 500
```

#### 3.3.2 终端模式

```bash
./ExampleApp --terminal --config config.json --enable-telnet --telnet-port 2323
```

在终端模式下，可以使用以下命令：

```
> help                    # 显示可用命令
> status                  # 显示应用状态
> config                  # 显示当前配置
> config reload           # 重新加载配置
> config set app.name MyApp  # 设置配置项
> config get app.name     # 获取配置项
> counter                 # 显示计数器
> counter reset           # 重置计数器
> tick                    # 显示tick间隔
> tick 200                # 设置tick间隔为200ms
> stop                    # 停止应用
> quit                    # 退出终端
```

#### 3.3.3 Telnet远程操作

```bash
telnet localhost 2323
```

连接后可以使用与终端模式相同的命令进行远程操作。

## 4. 高级特性

### 4.1 配置热加载

框架支持配置文件的热加载功能，当配置文件发生变化时，会自动触发重新加载：

```cpp
// 启用配置热加载
getContext().enableHotReload(true, 1000);  // 每1000ms检查一次

// 注册配置变更事件监听器
getContext().registerEventListener([this](const ConfigManager::ConfigEvent& event) {
    if (event.type == ConfigManager::EventType::CONFIG_RELOADED) {
        std::cout << "配置文件已重新加载" << std::endl;
        // 触发应用重载
        onReload();
    }
});
```

### 4.2 信号处理

框架提供完善的信号处理机制：

```cpp
// 在initialize()中自动注册的信号处理
m_signal_handler->registerHandler(SIGTERM, [this](int signal) {
    std::cout << "收到SIGTERM信号，正在关闭..." << std::endl;
    m_running = false;
});

m_signal_handler->registerHandler(SIGHUP, [this](int signal) {
    std::cout << "收到SIGHUP信号，重新加载配置..." << std::endl;
    m_should_reload = true;
});
```

### 4.3 自定义终端命令

业务层可以注册自定义的终端命令：

```cpp
registerTerminalCommand("business-cmd", "业务相关命令", 
    [this](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
            return "请提供参数\n";
        }
        
        // 处理业务逻辑
        std::string result = "处理结果: ";
        for (const auto& arg : args) {
            result += arg + " ";
        }
        return result + "\n";
    });
```

### 4.4 命令行参数回调

支持命令行参数的回调处理：

```cpp
registerCommandLineArg("log-level", "日志级别", "info", 
    [this](const std::string& value) {
        // 当命令行参数被解析时自动调用
        setLogLevel(value);
        std::cout << "日志级别设置为: " << value << std::endl;
    });
```

## 5. 目录结构

```
iApp框架/
├── include/                    # 头文件目录
│   ├── iApp.h                 # 主框架头文件
│   ├── ConfigManager.h        # 配置管理器头文件
│   ├── SignalHandler.h        # 信号处理器头文件
│   ├── CommandLineParser.h    # 命令行解析器头文件
│   └── TerminalInterface.h    # 终端接口头文件
├── src/                       # 源文件目录
│   ├── iApp.cpp              # 主框架实现
│   ├── ConfigManager.cpp     # 配置管理器实现
│   ├── SignalHandler.cpp     # 信号处理器实现
│   ├── CommandLineParser.cpp # 命令行解析器实现
│   └── TerminalInterface.cpp # 终端接口实现
├── example/                   # 示例目录
│   ├── ExampleApp.cpp        # 示例应用程序
│   └── config.json           # 示例配置文件
├── bin/                       # 可执行文件目录
├── obj/                       # 目标文件目录
├── Makefile                   # 编译脚本
└── README.md                  # 使用说明
```

## 6. 依赖要求

### 6.1 编译环境

- **编译器**：支持C++14标准的编译器（GCC 5.0+, Clang 3.4+）
- **操作系统**：Linux/Unix系统
- **构建工具**：Make

### 6.2 第三方库

- **jsoncpp**：JSON解析库
  ```bash
  # Ubuntu/Debian
  sudo apt-get install libjsoncpp-dev
  
  # CentOS/RHEL
  sudo yum install jsoncpp-devel
  ```

### 6.3 系统要求

- **线程支持**：需要pthread库支持
- **信号处理**：需要POSIX信号支持
- **网络功能**：需要socket库支持（用于telnet功能）

## 7. 最佳实践

### 7.1 错误处理

```cpp
bool onInit() override {
    try {
        // 初始化代码
        if (!initializeService()) {
            std::cerr << "服务初始化失败" << std::endl;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "初始化异常: " << e.what() << std::endl;
        return false;
    }
}
```

### 7.2 资源管理

```cpp
void onCleanup() override {
    // 确保资源正确释放
    if (m_database_connection) {
        m_database_connection->close();
        m_database_connection.reset();
    }
    
    if (m_network_service) {
        m_network_service->stop();
        m_network_service.reset();
    }
}
```

### 7.3 配置管理

```cpp
void loadConfiguration() {
    // 使用默认值确保配置的健壮性
    m_server_port = getContext().getValue<int>("server.port", 8080);
    m_max_connections = getContext().getValue<int>("server.max_connections", 100);
    m_timeout_ms = getContext().getValue<int>("server.timeout_ms", 5000);
    
    // 验证配置的有效性
    if (m_server_port <= 0 || m_server_port > 65535) {
        throw std::runtime_error("无效的服务器端口: " + std::to_string(m_server_port));
    }
}
```

### 7.4 线程安全

```cpp
void onTick(uint32_t delta_ms) override {
    // 使用锁保护共享资源
    std::lock_guard<std::mutex> lock(m_data_mutex);
    
    // 处理定时任务
    processTimerTasks();
    
    // 更新统计信息
    updateStatistics(delta_ms);
}
```

## 8. 故障排除

### 8.1 常见问题

#### 8.1.1 编译错误

**问题**：找不到jsoncpp头文件
```
fatal error: json/json.h: No such file or directory
```

**解决方案**：
```bash
# 安装jsoncpp开发包
sudo apt-get install libjsoncpp-dev
```

#### 8.1.2 运行时错误

**问题**：配置文件加载失败
```
Failed to load config file: config.json
```

**解决方案**：
- 检查配置文件路径是否正确
- 检查配置文件格式是否为有效的JSON
- 检查文件权限是否可读

#### 8.1.3 信号处理问题

**问题**：信号处理不生效

**解决方案**：
- 确保信号处理器已正确初始化
- 检查信号是否被其他代码屏蔽
- 验证信号处理函数是否正确注册

### 8.2 调试技巧

#### 8.2.1 启用详细日志

```cpp
bool onInit() override {
    // 启用详细日志输出
    std::cout << "应用初始化开始..." << std::endl;
    
    // 输出配置信息
    std::cout << "配置文件: " << getContext().getConfigFilePath() << std::endl;
    std::cout << "Tick间隔: " << getTickInterval() << "ms" << std::endl;
    
    return true;
}
```

#### 8.2.2 使用终端命令调试

```bash
# 进入终端模式
./app --terminal

# 查看应用状态
> status

# 查看配置
> config

# 动态修改配置进行测试
> config set debug.enabled true
```

## 9. 扩展开发

### 9.1 添加新组件

框架采用模块化设计，可以轻松添加新的组件：

```cpp
// 1. 创建新组件头文件
class NewComponent {
public:
    NewComponent();
    ~NewComponent();
    
    bool initialize();
    void process();
    void cleanup();
    
private:
    // 组件私有数据
};

// 2. 在iApp中集成新组件
class iApp {
private:
    std::unique_ptr<NewComponent> m_new_component;
    
public:
    bool initialize() {
        // 初始化新组件
        m_new_component = std::make_unique<NewComponent>();
        if (!m_new_component->initialize()) {
            return false;
        }
        return true;
    }
};
```

### 9.2 自定义配置格式

虽然框架默认使用JSON格式，但可以扩展支持其他配置格式：

```cpp
class CustomConfigLoader {
public:
    bool loadConfig(const std::string& file_path);
    bool saveConfig(const std::string& file_path);
    
    template<typename T>
    T getValue(const std::string& key, const T& default_value) const;
    
    template<typename T>
    void setValue(const std::string& key, const T& value);
};
```

## 10. 版本历史

### v1.0.0 (当前版本)
- 实现核心iApp框架
- 支持JSON配置管理和热加载
- 完整的信号处理机制
- 命令行参数解析
- 终端接口（标准IO + Telnet）
- 示例应用程序和完整文档

### 未来版本规划
- 日志系统集成
- 性能监控和统计
- 插件系统支持
- 更多配置格式支持
- Web管理界面

## 11. 许可证

本框架采用MIT许可证，允许自由使用、修改和分发。

---

**联系信息**：
- 项目地址：/usr/local/app/workspace/plan_8ab862170af77923a68331b6915eec89/stage_5
- 文档版本：1.0.0
- 最后更新：2025年8月26日