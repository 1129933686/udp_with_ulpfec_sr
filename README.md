# udp_with_ulpfec_sr
### 文件说明

1.include中文件为使用udp+fec程序时需要引用的头文件

2.shared中文件为使用MinGW编译器编译该库生成的文件，在windows下生成.dll和.a文件，在linux下生成.so和.a文件

3.cpp文件为使用udp+fec程序时需要引用的源文件，其中主程序为udp_with_ulpfec.cpp

4.将发送函数sendto_fec和recvfrom_fec制作为动态链接库



### 编译命令：

#### 1.Windows平台：

使用Qt的MinGW编译器工具，输入命令行命令为：
g++ -mavx2 -O3 -DWEBRTC_WIN -I. -I./include -shared -o udp_with_ulpfec.dll udp_with_ulpfec.cpp checks.cpp fec_private_tables_random.cpp fec_private_tables_bursty.cpp forward_error_correction.cpp simple_client_server.cpp -Wl,--out-implib,udp_with_ulpfec.a -lws2_32 -static-libstdc++

#### 2.Linux平台：

首先设置环境变量：
export PATH=/your/Qt/version/gcc_64/bin:$PATH
然后在终端依次输入如下命令：
①g++ -mavx2 -O3 -DWEBRTC_POSIX -I. -I./include -fPIC -shared -o udp_with_ulpfec.so udp_with_ulpfec.cpp checks.cpp fec_private_tables_random.cpp fec_private_tables_bursty.cpp forward_error_correction.cpp simple_client_server.cpp
②g++ -mavx2 -O3 -DWEBRTC_POSIX -I. -I./include -fPIC -c udp_with_ulpfec.cpp checks.cpp fec_private_tables_random.cpp fec_private_tables_bursty.cpp forward_error_correction.cpp simple_client_server.cpp
③ar rcs udp_with_ulpfec.a udp_with_ulpfec.o checks.o fec_private_tables_random.o fec_private_tables_bursty.o forward_error_correction.o simple_client_server.o
④rm *.o



### 版本更新

#### 邢启航09_08更新说明：

发送程序和接收程序出现了错误，具体的：
1.发送程序生成冗余包后下一轮次没有将冗余包内容重置
2.接收程序的接收函数出现了问题
已将上述问题改正并生成Windows平台下动态链接库文件

#### 邢启航11_05更新说明：

发送程序使用随机数进行随机丢包时出现问题，已修改随机丢包数的定义

#### 邢启航11_07更新说明：

包头添加了数据长度字段，现在支持发送端以不同长度进行发送（支持FEC）

#### 邢启航11_16更新说明：

1.发送端每次创建实例会发送重置信号，接收端接收到重置信号后清除恢复包列表、冗余包列表、takeout_sequence组号和组内序号更新为0；
2.接收端查重使用哈希表，插入时避免全局排序；
3.现在一旦接收到序号连续的目标包就会立刻送入buffer_packets中待后续取用，没有延迟

还需完成：
linux平台下动态链接库文件生成
