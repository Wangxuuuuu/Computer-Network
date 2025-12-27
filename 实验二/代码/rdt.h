#ifndef RDT_H
#define RDT_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>                   // 时间库
#include <algorithm>
#include <cstring>                  // memset()

#pragma comment(lib, "ws2_32.lib")  // 告知编译器链接 ws2_32.lib 库(Windows Socket 库)

// 常量定义
const int MSS = 1024;               // 最大分段大小,每个数据包的数据部分最多 1024 字节
const int HEADER_SIZE = 20;         // 头部大小 (固定)
const int MAX_SEQ = 0xFFFFFFFF;     // 最大序列号,标识数据包顺序
const int TIMEOUT_MS = 1000;         // 超时时间 (毫秒),这里指的是重传超时

// 标志位
const uint16_t FLAG_SYN = 0x01;     // 同步标志,用于连接建立
const uint16_t FLAG_ACK = 0x02;     // 确认标志,用于确认收到数据
const uint16_t FLAG_FIN = 0x04;     // 结束标志,用于断开连接

// 数据包结构
#pragma pack(push, 1)           // 将结构体的对齐方式设置为1字节对齐,避免编译器为了对齐而在结构体成员之间插入填充字节,确保数据包结构在内存中的布局与网络传输格式一致
struct PacketHeader {
    uint32_t seq;       // 序列号
    uint32_t ack;       // 确认号
    uint16_t flags;     // 标志位
    uint16_t checksum;  // 校验和
    uint16_t length;    // 数据长度
    uint16_t window;    // 窗口大小 (用于流量控制)
    uint32_t padding;   // 填充，确保头部总长 20 字节(对齐)
};

struct Packet {
    PacketHeader header;
    char data[MSS];
};
#pragma pack(pop)               // 恢复默认对齐方式

// 校验和计算函数
inline uint16_t calculate_checksum(Packet* pkt) {
    uint16_t old_checksum = pkt->header.checksum;
    pkt->header.checksum = 0;
    
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)pkt;
    int size = sizeof(PacketHeader) + pkt->header.length;       // 只计算头部和有效数据部分

    while (size > 1) {
        sum += *ptr++;          // 每次加2字节(uint16_t)
        size -= 2;              // 减少已处理的字节数
    }
    if (size > 0) {
        sum += *(uint8_t*)ptr;  // 处理剩余的1个单字节
    }

    // sum>>16 是为了处理溢出的高16位，将其加回低16位,这样做可以确保最终的校验和仍然是一个16位的值,并且符合网络协议中对校验和的定义.
    // 直至没有进位为止
    while (sum >> 16) {         
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    pkt->header.checksum = old_checksum; // 恢复原来的校验和

    // 取反得到最终的校验和
    return (uint16_t)(~sum);        
}

// 打印数据包信息 (调试用)
inline void print_packet_info(const char* tag, const Packet& pkt) {
    std::cout << "[" << tag << "] "
              << "Seq: " << pkt.header.seq << " "
              << "Ack: " << pkt.header.ack << " "
              << "Len: " << pkt.header.length << " "
              << "Flags: ";
    if (pkt.header.flags & FLAG_SYN) std::cout << "SYN ";
    if (pkt.header.flags & FLAG_ACK) std::cout << "ACK ";
    if (pkt.header.flags & FLAG_FIN) std::cout << "FIN ";
    std::cout << std::endl;
}

#endif // RDT_H
