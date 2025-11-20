#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <thread>
#include <mutex>
#include <string>
#include <map>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm> // for std::remove_if

#pragma comment(lib, "ws2_32.lib")

// 全局变量
std::vector<SOCKET> clients; // 存储所有客户端的socket
std::map<SOCKET, std::string> client_names; // 存储socket对应的用户名
std::mutex clients_mutex; // 用于保护对clients和client_names的访问

// 获取当前时间的字符串
std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    // 使用 localtime_s 保证线程安全
    tm buf;
    localtime_s(&buf, &in_time_t);
    ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// 稳定发送所有数据的函数
bool sendAll(SOCKET sock, const std::string& message) {
    const char* data = message.c_str();
    int total = message.size();
    int sent = 0;
    int bytesleft = total;
    int n;

    while (sent < total) {
        n = send(sock, data + sent, bytesleft, 0);
        if (n == SOCKET_ERROR) {
            std::cerr << "[" << getCurrentTimestamp() << "] 发送数据失败: " << WSAGetLastError() << std::endl;
            return false;
        }
        sent += n;
        bytesleft -= n;
    }
    return true;
}

// 向所有客户端广播消息
void broadcastMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    // 协议规定每条消息以 \n 结尾
    std::string full_message = message + "\n";
    for (SOCKET client_socket : clients) {
        if (!sendAll(client_socket, full_message)) {
            std::cerr << "[" << getCurrentTimestamp() << "] 向客户端 " << client_socket << " 广播消息失败。" << std::endl;
        }
    }
}

// 从缓冲区中提取一行 (以 \n 结尾，不含 \n)
std::string extractLine(std::string& buffer) {
    size_t pos = buffer.find('\n');
    if (pos != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1); // 移除已处理的行，包括\n
        return line;
    }
    return ""; // 没有找到完整的一行
}

// 处理单个客户端的函数
void handleClient(SOCKET client_socket) {
    std::string recv_buffer; // 为此客户端维护接收缓冲区
    char temp_buffer[4096];
    int bytes_received;
    std::string username;
    bool username_received = false;

    std::cout << "[" << getCurrentTimestamp() << "] 新客户端连接 (Socket: " << client_socket << ")" << std::endl;

    while (true) {
        bytes_received = recv(client_socket, temp_buffer, sizeof(temp_buffer) - 1, 0);
        if (bytes_received > 0) {
            temp_buffer[bytes_received] = '\0';
            recv_buffer.append(temp_buffer, bytes_received);

            // 处理缓冲区中可能存在的完整消息行
            std::string line;
            while (!(line = extractLine(recv_buffer)).empty()) {
                if (!username_received) {
                    // 第一条消息是用户名
                    username = line;
                    username_received = true;

                    // 存储用户名
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        client_names[client_socket] = username;
                    }
                    std::string join_msg = "[" + getCurrentTimestamp() + "] 用户 \"" + username + "\" 加入了聊天室。";
                    std::cout << join_msg << std::endl;
                    broadcastMessage(join_msg);
                    break; // 处理完用户名就跳出内层循环，继续接收聊天内容

                } else {
                    // 后续消息是聊天内容
                    std::string formatted_msg = "[" + getCurrentTimestamp() + "] " + username + ": " + line;
                    std::cout << "[" << getCurrentTimestamp() << "] 收到来自 " << username << " 的消息: " << line << std::endl;
                    broadcastMessage(formatted_msg);
                }
            }
        } else if (bytes_received == 0) {
            // 客户端正常关闭连接
            std::cout << "[" << getCurrentTimestamp() << "] 客户端 (Socket: " << client_socket << ", 用户: " << username << ") 断开连接。" << std::endl;
            break;
        } else {
            // recv 出错
            std::cerr << "[" << getCurrentTimestamp() << "] 接收客户端 (Socket: " << client_socket << ") 数据失败: " << WSAGetLastError() << std::endl;
            break;
        }
    }

    // 客户端断开连接后的清理工作
    if (username_received && !username.empty()) {
        std::string leave_msg = "[" + getCurrentTimestamp() + "] 用户 \"" + username + "\" 离开了聊天室。";
        std::cout << leave_msg << std::endl;
        broadcastMessage(leave_msg);
    }

    // 从全局列表中移除客户端
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [client_socket](SOCKET s) { return s == client_socket; }),
                      clients.end());
        client_names.erase(client_socket);
    }

    closesocket(client_socket);
    std::cout << "[" << getCurrentTimestamp() << "] 客户端 (Socket: " << client_socket << ") 资源已释放。" << std::endl;
}

int main() {
    // 设置控制台输出为UTF-8编码，以便正确显示中文
    SetConsoleOutputCP(CP_UTF8);

    // 1. 初始化Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "[" << getCurrentTimestamp() << "] WSAStartup 失败: " << result << std::endl;
        return 1;
    }
    std::cout << "[" << getCurrentTimestamp() << "] Winsock 初始化成功。" << std::endl;

    // 2. 创建监听socket
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "[" << getCurrentTimestamp() << "] 创建监听socket失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }
    std::cout << "[" << getCurrentTimestamp() << "] 监听socket创建成功。" << std::endl;

    // 3. 绑定IP和端口
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY为特殊常量，值为0.0.0.0,表示监听所有网络接口
    server_addr.sin_port = htons(1221); // 监听1221端口

    if (bind(listen_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[" << getCurrentTimestamp() << "] 绑定地址失败: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[" << getCurrentTimestamp() << "] 地址绑定成功 (端口 1221)。" << std::endl;

    // 4. 开始监听
    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[" << getCurrentTimestamp() << "] 开始监听失败: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[" << getCurrentTimestamp() << "] 服务器已在端口 1221 启动并等待客户端连接..." << std::endl;

    // 5. 循环接受客户端连接
    while (true) {
        SOCKET client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
            std::cerr << "[" << getCurrentTimestamp() << "] 接受客户端连接失败: " << WSAGetLastError() << std::endl;
            continue; // 继续等待下一个连接
        }

        // 将新客户端socket加入全局列表
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(client_socket);
        }

        // 为每个客户端创建一个新线程
        std::thread t(handleClient, client_socket);
        t.detach(); // 分离线程，让它在后台独立运行
    }

    // 6. 清理 (注意：此代码段在无限循环中不会被执行)
    closesocket(listen_socket);
    WSACleanup();
    std::cout << "[" << getCurrentTimestamp() << "] 服务器关闭。" << std::endl;
    return 0;
}