#include "rdt.h"
#include <fstream>
#include <map>

using namespace std;

SOCKET sock;                            // Socket,用于UDP通信
sockaddr_in clientAddr;                 // 客户端地址(IP + 端口),用于发送 ACK
int addrLen = sizeof(clientAddr);       // 地址长度,用于 recvfrom 和 sendto 函数
bool connected = false;                 // 连接状态,确保握手完成后才处理数据包
int rcvWindowSize = 20;                 // 接收窗口大小 (默认20),可通过命令行参数修改

// 接收缓冲区 (用于乱序重排)
map<uint32_t, Packet> recvBuffer;       // 序列号 到 数据包Packet 的映射
uint32_t expectedSeq = 1;               // 期望收到的下一个序列号,数据包从 1 开始,握手包序列号为 0
ofstream outFile;

// 发送 ACK 确认包
// 在 SR 选择性重传模式下，每次收到数据包(无论是否期望的)，都要立即发送 ACK，确认该包已被收到
// ackNum: 要确认的序列号(收到包的序列号), targetAddr: 发送 ACK 的目标地址
void send_ack(uint32_t ackNum, sockaddr_in& targetAddr) {
    Packet ackPkt;
    memset(&ackPkt, 0, sizeof(ackPkt));
    ackPkt.header.flags = FLAG_ACK;
    ackPkt.header.ack = ackNum; // ACK 字段设置为收到的包的序列号，配合发送端的 SR 逻辑
    ackPkt.header.length = 0;
    ackPkt.header.checksum = calculate_checksum(&ackPkt);

    sendto(sock, (char*)&ackPkt, sizeof(PacketHeader), 0, (sockaddr*)&targetAddr, sizeof(targetAddr));
    // cout << "[ACK] Sent ACK for " << ackNum << endl;
}

int main(int argc, char* argv[]) {
    // 参数检查,必须至少是3个:(程序名 + port + output_file)
    if (argc < 3) {
        cout << "Usage: " << argv[0] << " <port> <output_file> [window_size]" << endl;
        return 1;
    }

    int port = atoi(argv[1]);               // 将端口号字符串转换为整数
    string outFileName = argv[2];           // 输出文件名
    
    if (argc >= 4) {
        rcvWindowSize = atoi(argv[3]);
        cout << "Receive Window Size set to: " << rcvWindowSize << endl;
    }

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);   // 初始化 Winsock,版本 2.2

    sock = socket(AF_INET, SOCK_DGRAM, 0);  // 创建 UDP 套接字,IPv4和UDP数据报模式
    
    sockaddr_in serverAddr;                 // 服务器地址
    serverAddr.sin_family = AF_INET;        // 地址族:IPv4
    serverAddr.sin_port = htons(port);      // 主机字节序转换为网络字节序(大端)
    serverAddr.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY表示绑定到所有本地接口的IP地址,这样服务器可以接受发送到任何本地IP地址的数据包.

    if (bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "Bind failed." << endl;
        return 1;
    }

    cout << "Server listening on port " << port << endl;
    outFile.open(outFileName, ios::binary);                 // 以二进制模式打开输出文件,确保数据按原始字节写入
    if (!outFile.is_open()) {
        cout << "Failed to open output file: " << outFileName << endl;
        return 1;
    }

    // 主循环:接收和处理数据包
    while (true) {
        Packet recvPkt;
        sockaddr_in fromAddr;               // 发送方地址
        int fromLen = sizeof(fromAddr);
        
        int len = recvfrom(sock, (char*)&recvPkt, sizeof(recvPkt), 0, (sockaddr*)&fromAddr, &fromLen);
        if (len > 0) {
            // 校验和检查
            if (calculate_checksum(&recvPkt) != recvPkt.header.checksum) {
                cout << "[Checksum Error] Drop packet." << endl;
                continue;
            }

            // 握手逻辑
            // SYN 处理:收到 SYN，发送 SYN+ACK(TCP 三次握手的第二步)
            if (recvPkt.header.flags & FLAG_SYN) {
                cout << "[Handshake] SYN received." << endl;
                
                Packet synAckPkt;
                memset(&synAckPkt, 0, sizeof(synAckPkt));
                synAckPkt.header.flags = FLAG_SYN | FLAG_ACK;
                synAckPkt.header.seq = 0;
                synAckPkt.header.ack = recvPkt.header.seq + 1;
                synAckPkt.header.checksum = calculate_checksum(&synAckPkt);
                
                sendto(sock, (char*)&synAckPkt, sizeof(PacketHeader), 0, (sockaddr*)&fromAddr, fromLen);
                cout << "[Handshake] SYN+ACK sent." << endl;
                continue;
            }

            // 握手最后一步 ACK
            // ACK 处理：收到纯 ACK(非 SYN)，建立连接(第三步)
            if ((recvPkt.header.flags & FLAG_ACK) && !connected && recvPkt.header.length == 0 && !(recvPkt.header.flags & FLAG_SYN)) {
                 cout << "[Handshake] Connection Established." << endl;
                 connected = true;
                 clientAddr = fromAddr;     // 保存客户端地址，用于发送 ACK
                 continue;
            }

            // 挥手逻辑
            // FIN 处理：收到 FIN,发送 ACK 确认,并关闭连接
            if (recvPkt.header.flags & FLAG_FIN) {
                cout << "[Teardown] FIN received." << endl;
                Packet ackPkt;
                memset(&ackPkt, 0, sizeof(ackPkt));
                ackPkt.header.flags = FLAG_ACK;                 // 发送 ACK 确认
                ackPkt.header.ack = recvPkt.header.seq + 1;
                ackPkt.header.checksum = calculate_checksum(&ackPkt);
                sendto(sock, (char*)&ackPkt, sizeof(PacketHeader), 0, (sockaddr*)&fromAddr, fromLen);
                cout << "[Teardown] ACK sent. Closing." << endl;
                break;
            }

            // 数据处理
            if (connected && recvPkt.header.length > 0) {
                uint32_t seq = recvPkt.header.seq;

                // 流量控制：如果序列号超出接收窗口，直接丢弃，不发送 ACK
                if (seq >= expectedSeq + rcvWindowSize) {
                    // cout << "[Flow Control] Drop packet " << seq << " outside window." << endl;
                    continue;
                }
                
                // 发送 ACK (SR 模式：收到什么确认什么)
                send_ack(seq, fromAddr);

                if (seq == expectedSeq) {
                    // 收到期望的包，写入文件
                    outFile.write(recvPkt.data, recvPkt.header.length);
                    expectedSeq++;

                    // 检查缓冲区是否有后续包
                    // .count() 用于检查 map 中是否存在指定键,返回 1 表示存在,0 表示不存在
                    while (recvBuffer.count(expectedSeq)) {
                        Packet& bufferedPkt = recvBuffer[expectedSeq];
                        outFile.write(bufferedPkt.data, bufferedPkt.header.length);
                        recvBuffer.erase(expectedSeq);
                        expectedSeq++;
                    }
                } else if (seq > expectedSeq) {
                    // 乱序包，缓存
                    if (recvBuffer.find(seq) == recvBuffer.end()) {
                        recvBuffer[seq] = recvPkt;
                        // cout << "[Buffer] Buffered packet " << seq << endl;
                    }
                }
                // 如果 seq < expectedSeq，说明是重复包，已经 ACK 过了，上面已经重发了 ACK
            }
        }
    }

    outFile.close();        
    closesocket(sock);      
    WSACleanup();
    return 0;
}
