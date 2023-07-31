/*
编译：gcc minihttp.c -lpthread -o minihttp.exe
运行：./minihttp.exe
*/

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#define SERVER_PORT 80

static int debug = 1;

// 从socket中读取请求行，一次读一行
int get_line(int sock, char *buf, int size);

// 读取客户端发送的http 请求
void* do_http_request(void* pclient_sock);

// 响应http请求
void do_http_response(int client_sock, const char *path);

// 响应404
void not_found(int client_sock);

// 501 方法未实现
void unimplemented(int client_sock);

// 400 客户端发送的请求格式有问题等
void bad_request(int client_sock);

// 发送文件头
int headers(int client_sock, FILE *resource);

// 发送http body
void cat(int client_sock, FILE *resource);

// 500 响应 服务器内部出错
void inner_error(int client_sock);


int main(void) {
    /*
    创建socket
    int socket(int domain, int type, int protocol);

    int domain:
        AF_UNIX（本机通信）
        AF_INET（TCP/IP – IPv4）
        AF_INET6（TCP/IP – IPv6）

    int type:
        SOCK_STREAM（TCP流）
        SOCK_DGRAM（UDP数据报）
        SOCK_RAW（原始套接字）

    int protocol:
        最后一个 “protocol”一般设置为“0”，也就是当确定套接字使用的协议簇和类型时，
        这个参数的值就为0，但是有时候创建原始套接字时，并不知道要使用的协议簇和类型，
        也就是domain参数未知情况下，这时protocol这个参数就起作用了，它可以确定协议的种类
    return:
        socket是一个函数，那么它也有返回值，当套接字创建成功时，
        返回套接字，失败返回“-1”，错误代码则写入“errno”中
    */
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // sockaddr_in把14个字节拆分成sin_port, sin_addr和sin_zero分别表示端口、ip地址
    struct sockaddr_in server_addr;

    // bzero() 能够将内存块（字符串）的前n个字节清零，然后写上地址和端口号，
    bzero(&server_addr, sizeof(server_addr));  

    // 选择协议族IPV4
    server_addr.sin_family = AF_INET;
    
    // 监听本地所有IP地址
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /*
    htons():
        在Linux和Windows网络编程时需要用到htons和htonl函数，用来将主机字节顺序转换为网络字节顺序
        由于Intel机器是小尾端，存储数字16时实际顺序为1000，存储4096时实际顺序为0010。
        因此在发送网络包时为了报文中数据为0010，需要经过htons进行字节转换。
        如果用IBM等大尾端机器，则没有这种字节顺序转换，但为了程序的可移植性，也最好用这个函数。

        另外注意，数字所占位数小于或等于一个字节（8 bits）时，不要用htons转换。
        这是因为对于主机来说，大小尾端的最小单位为字节(byte)。

    SERVER_PORT:
        转换过来就是0.0.0.0，泛指本机的意思，也就是表示本机的所有IP，
        因为有些机子不止一块网卡，多网卡的情况下，这个就表示所有网卡ip地址的意思。
        比如一台电脑有3块网卡，分别连接三个网络，那么这台电脑就有3个ip地址了
        所以出现INADDR_ANY，你只需绑定INADDR_ANY，管理一个套接字就行，
        不管数据是从哪个网卡过来的，只要是绑定的端口号过来的数据，都可以接收到
    */
    server_addr.sin_port = htons(SERVER_PORT);

    /*
    int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
        sockfd 表示socket函数创建的通信文件描述符
        addr 表示struct sockaddr的地址，用于设定要绑定的ip和端口
        addrlen 表示所指定的结构体变量的大小
    */
    bind(sock, (struct sockaddr *)&server_addr,  sizeof(server_addr));

    /*
    int listen(int s, int backlog);
        参数backlog 指定同时能处理的最大连接要求, 
        如果连接数目达此上限则client 端将收到ECONNREFUSED 的错误

        Listen()并未开始接收连线, 只是设置socket 为listen 模式, 
        真正接收client 端连线的是accept(). 通常listen()会在socket(), bind()之后调用, 
        接着才调用accept()

    TCP编程模型中的socket和bind接口是tcp服务器在为接收客户端的链接做准备，
    保证tcp的面向字节流，面向连接的可靠通信服务正常进行。
    接下来的listen端口则为我们进行三次握手与客户端进行链接的接口。

    TCP服务器为什么调用listen?
        因为连接请求只能由客户端发起，此时服务端的listen函数是将服务端的主动描述符转为
        被动描述符，否则无法用于监听客户端的连接。
        TCP服务器监听客户端链接时，使用的是socket返回的“套接字文件描述符”来实现的，
        但是这个文件描述符默认是主动文件描述符（主动向对方发送数据），
        所以需要使用listen函数将其转换为被动描述符（只能被动得等待别人主动发送数据，再回应），
        否则无法用于被动监听客户端。
    */
    listen(sock, 128);

    printf("等待客户端的连接\n");

    int done = 1;

    while(done) {
        struct sockaddr_in client;
        int client_sock, len, i;
        char client_ip[64];
        char buf[256];
        pthread_t id;
        int* pclient_sock = NULL;

        socklen_t client_addr_len = sizeof(client);

        /*
        accept()系统调用主要用在基于连接的套接字类型，比如SOCK_STREAM和SOCK_SEQPACKET。
        它提取出所监听套接字的等待连接队列中第一个连接请求，创建一个新的套接字，
        并返回指向该套接字的文件描述符。新建立的套接字不在监听状态，
        原来所监听的套接字也不受该系统调用的影响。
        */
        client_sock = accept(sock, (struct sockaddr *)&client, &client_addr_len);

        /*
        打印客户端IP地址和端口号

        inet_ntop:
            原型const char * inet_ntop(int family, const void *addrptr, char *strptr, size_t len); 
            功能: 将数值格式转化为点分十进制的ip地址格式
            返回值
        　　　　1）、成功则为指向结构的指针
        　　　　2）、出错则为NULL

        inet_pton:
            功能: 将点分十进制的ip地址转化为用于网络传输的数值格式
            返回值
                成功则为1
                输入不是有效的表达式则为0
                出错则为-1
        */
        printf("client ip: %s\t port : %d\n",
                inet_ntop(AF_INET, &client.sin_addr.s_addr, client_ip, sizeof(client_ip)),
                ntohs(client.sin_port));

        /*
        pthread_create函数
        创建一个新线程，并行的执行任务。
        #include <pthread.h>
        int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);

        返回值：成功：0； 失败：错误号。
        参数：	
            pthread_t：当前Linux中可理解为：typedef unsigned long int pthread_t;
            参数1：传出参数，保存系统为我们分配好的线程ID
            参数2：通常传NULL，表示使用线程默认属性。若想使用具体属性也可以修改该参数。
            参数3：函数指针，指向线程主函数(线程体)，该函数运行结束，则线程结束。 
                void * ：void 指针可以指向任意类型的数据，就是说可以用任意类型的指针对 void 指针对 void 指针赋值
            参数4：线程主函数执行期间所使用的参数。

        在一个线程中调用pthread_create()创建新的线程后，当前线程从pthread_create()返回继续往下执行，
        而新的线程所执行的代码由我们传给pthread_create的函数指针start_routine决定。start_routine函数接收一个参数，
        是通过pthread_create的arg参数传递给它的，该参数的类型为void *，这个指针按什么类型解释由调用者自己定义。
        start_routine的返回值类型也是void *，这个指针的含义同样由调用者自己定义。start_routine返回时，这个线程就退出了，
        其它线程可以调用pthread_join得到start_routine的返回值。

        pthread_create成功返回后，新创建的线程的id被填写到thread参数所指向的内存单元。
        attr参数表示线程属性，不深入讨论线程属性，所有代码例子都传NULL给attr参数，表示线程属性取缺省值。
        */
        pclient_sock = malloc(sizeof(int));
        *pclient_sock = client_sock;
        pthread_create(&id, NULL, do_http_request, (void *)pclient_sock);  
    }

    close(sock);

    return 0;
}


/**
 * 读取客户端发送的http 请求
 * 例子：
        GET /demo.html HTTP/1.1
        Host: 47.100.162.191
        Connection: keep-alive
        User-Agent: Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/63.0.3239.26 Safari/537.36 Core/1.63.6788.400 QQBrowser/10.3.2767.400
        Upgrade-Insecure-Requests: 1
        Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng;q=0.8
        Accept-Encoding: gzip, deflate
        Accept-Language: zh-CN,zh;q=0.9
        Cookie:cna=BT0+EoVi1FACAXH3Nv5I7h6k;isg=BIOD99I03BNYvZDfE2FJUOsMB0ftUBZcBFi4E7VgYOJZdKOWPcvRinAl6kSfVG8y
*/
void* do_http_request(void* pclient_sock) {
    // 行的长度
    int len = 0;
    // buf长度，先把数据从socket读到buf中
  	char buf[256];
    // 请求方法
	char method[64];
    // url
	char url[256];
    // 请求的资源在本机的地址
	char path[256];
	// 保存文件元数据的结构体
	struct stat st;

    int client_sock = *(int *)pclient_sock;

    /*
    从socket中读取请求行，一次读一行
    关于http请求头结构，可以看：https://zhuanlan.zhihu.com/p/450128753
    */
    len = get_line(client_sock, buf, sizeof(buf));

    // 读到了请求行
    if (len > 0) {
        int i = 0, j = 0;

        // 读取请求方法
        // 当前不是空白符，并且method还放得下
        // 空白符指空格、水平制表、垂直制表、换页、回车和换行符
        while (!isspace(buf[j]) && (i < sizeof(method) - 1)) {
            method[i] = buf[j];
            i++;
            j++;
        }

        method[i] = '\0';

        if (debug) {
            printf("request method: %s\n", method);
        }

        // 只处理get请求，strncasecmp不区分大小写比较字符串
        if (strncasecmp(method, "GET", i) == 0) {
            if (debug) {
                printf("method = GET\n");
            }

            // 获取url
            // 空白符指空格、水平制表、垂直制表、换页、回车和换行符
            while (isspace(buf[j++]));
            i = 0;
            while (!isspace(buf[j]) && (i < sizeof(url) - 1)) {
			    url[i] = buf[j];
			    i++;
			    j++;
		    }
            url[i] = '\0';

		    if (debug) {
                printf("url: %s\n", url);
            }

            // 继续读取http 头部
            // 如果读到len <= 0则说明头结束了，两个\r\n
		    do {
          		len = get_line(client_sock, buf, sizeof(buf));
			    if (debug) {
                    printf("read: %s\n", buf);
                }
		    } while (len > 0);

            // 定位服务器本地的html文件
            // 处理url中的?
            {
                // strchr() 用于查找字符串中的一个字符，并返回该字符在字符串中第一次出现的位置
				// 返回的指针指向字符串中的字符
                char *pos = strchr(url, '?');

                // 通过指针把 ? 改成字符串结束符，就自动截断?了
				if (pos) {
					*pos = '\0';
					printf("real url: %s\n", url);
				}
			}
            
            // path拿到请求的地址在服务器的映射
            sprintf(path, "./html_docs/%s", url);
			if (debug) {
                printf("path: %s\n", path);
            }

            /* 
            执行http响应：
                判断文件是否存在，如果存在就响应200 OK，同时发送相应的html文件。如果不存在，就响应 404 NOT FOUND.
                inode - "索引节点",储存文件的元信息，比如文件的创建者、文件的创建日期、文件的大小等等。
                每个inode都有一个号码，操作系统用inode号码来识别不同的文件。ls -i  查看inode 号
            
            stat函数 int stat(const char *path, struct stat *buf);
                作用：返回文件的状态信息
                path: 文件的路径
                buf: 传入的保存文件状态的指针，用于保存文件的状态
                返回值：成功返回0，失败返回-1，设置errno
            */
            if (stat(path, &st) == -1) {
                fprintf(stderr, "stat %s failed. reason: %s\n", path, strerror(errno));
			    not_found(client_sock);
            } 
            // 文件存在
            else {
                // 如果是一个目录，就追加/index.html
			    if (S_ISDIR(st.st_mode)) {
                    // strcat追加
					strcat(path, "/index.html");
				}
				// 执行http响应
				do_http_response(client_sock, path);
				
			}
        }
        // 非get请求, 读取http 头部，并响应客户端 501  Method Not Implemented
        else {
            // stderr是标准错误输出设备，与标准输出设备stdout默认向屏幕输出
			fprintf(stderr, "warning! other request [%s]\n", method);
			do {
			    len = get_line(client_sock, buf, sizeof(buf));
			    if (debug) {
                    printf("read: %s\n", buf);
                }
          	} while (len > 0);

			// 请求未实现
			unimplemented(client_sock);
		}
    }
    // 请求格式有问题，出错处理
    else {
	    bad_request(client_sock);   
	}

    // 关闭sorket
	close(client_sock);

    // 释放动态分配的内存
    if (pclient_sock) {
        free(pclient_sock);
    }

    return NULL;
}


/**
 * 响应http请求
 * 例如：
        HTTP/1.0 200 OK
        Server: C Http Server
        Content-Type: text/html
        Connection: Close
        Content-Length: 526 这个是下文html文件的长度，要动态处理

        <html lang="zh-CN">
        <head>
        <meta content="text/html; charset=utf-8" http-equiv="Content-Type">
        <title>This is a test</title>
        </head>
        <body>
        <div align=center height="500px" >
        <br/><br/><br/>
        <h2>大家好，欢迎来到奇牛学院VIP 试听课！</h2><br/><br/>
        <form action="commit" method="post">
        尊姓大名: <input type="text" name="name" />
        <br/>芳龄几何: <input type="password" name="age" />
        <br/><br/><br/><input type="submit" value="提交" />
        <input type="reset" value="重置" />
        </form>
        </div>
        </body>
        </html>  
*/
void do_http_response(int client_sock, const char *path) {
    int ret = 0;
	FILE *resource = NULL;
	
    // 打开文件
	resource = fopen(path, "r");
	if (resource == NULL) {
		not_found(client_sock);
		return ;
	}
	
	// 发送http 头部
	ret = headers(client_sock, resource);
	
	// 发送http body
	if (!ret) {
	    cat(client_sock, resource);
	}
	
	fclose(resource);
}


/**
 * 返回关于响应文件信息的http 头部
 * 输入： 
 *      client_sock - 客户端socket 句柄
 *      resource    - 文件的句柄 
 * 返回值： 成功返回0 ，失败返回-1
*/
int headers(int client_sock, FILE *resource) {
    struct stat st;
	int fileid = 0;
	char tmp[64];

    // 存头部的buf
	char buf[1024]={0};
	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	strcat(buf, "Server: C Http Server\r\n");
	strcat(buf, "Content-Type: text/html\r\n");
	strcat(buf, "Connection: Close\r\n");

    // fileno：把文件流指针转换成文件描述符
	fileid = fileno(resource);
	
	if (fstat(fileid, &st) == -1) {
		inner_error(client_sock);
		return -1;
	}

    // snprintf 用于格式化输出字符串，并将结果写入到指定的缓冲区，
    // 与 sprintf() 不同的是，snprintf() 会限制输出的字符数，避免缓冲区溢出
    snprintf(tmp, 64, "Content-Length: %ld\r\n\r\n", st.st_size);

    strcat(buf, tmp);
	
	if (debug) {
        fprintf(stdout, "header: %s\n", buf);
    }
	
    // send就是往socket上面送消息
	if (send(client_sock, buf, strlen(buf), 0) < 0) {
        // Linux中系统调用的错误都存储于 errno中，errno由操作系统维护，
        // 存储就近发生的错误，即下一次的错误码会覆盖掉上一次的错误
		fprintf(stderr, "send failed. data: %s, reason: %s\n", buf, strerror(errno));
		return -1;
	}
	
	return 0;
}


/**
 * 说明：实现将html文件的内容按行
        读取并送给客户端
*/
void cat(int client_sock, FILE *resource){
	char buf[1024];
    fgets(buf, sizeof(buf), resource);
	
    // feof() 判断文件是否结束
	while (!feof(resource)) {
		int len = write(client_sock, buf, strlen(buf));
		
		if (len < 0) {
			fprintf(stderr, "send body error. reason: %s\n", strerror(errno));
			break;
		}
		
		if (debug) {
            fprintf(stdout, "%s", buf);
        }

		fgets(buf, sizeof(buf), resource);
	}
}


/**
 * 从socket中读取请求行，一次读一行
 * 返回值： -1 表示读取出错， 
 * 等于0表示读到一个空行， 
 * 大于0 表示成功读取一行
*/
int get_line(int sock, char *buf, int size) {
    // 计数器，已经读了多少个字符
    int count = 0;
    // 当前读到的字符
	char ch = '\0';
    // 目前已读的长度
	int len = 0;

    // 一个一个字符读
    while ((count < size - 1) && ch != '\n') {
        len = read(sock, &ch, 1);

        if (len == 1) {
            if (ch == '\r') {
                // 如果读到回车，会自动跳到本行的行首，所以应该忽略回车
                continue;
            } else if (ch == '\n'){
				break;
			}

            // 这里处理一般的字符(非回车换行)
            buf[count] = ch;
            count++;
        } 
        // 读取出错
        else if (len == -1) {
            // perror(s) 用来将上一个函数发生错误的原因输出到标准设备(stderr)
            perror("read failed");
            // 让之前读的数据无效
            count = -1;
            break;
        } 
        // len == 0，客户端关闭sock 连接
        else {
            fprintf(stderr, "client close.\n");
            // 让之前读的数据无效
			count = -1;
			break;
        }
    }

    // 给字符串加个'\0'结尾
    if (count >= 0) {
        buf[count] = '\0';
    }

    return count;
}

/**
 * 文件不存在
*/
void not_found(int client_sock){
	const char * reply = "HTTP/1.0 404 NOT FOUND\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>NOT FOUND</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>文件不存在！\r\n\
    <P>The server could not fulfill your request because the resource specified is unavailable or nonexistent.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client_sock, reply, strlen(reply));

	if (debug) {
        fprintf(stdout, reply);
    }
	
	if (len <=0) {
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}


/**
 * 500 响应 服务器内部出错
*/
void inner_error(int client_sock){
	const char * reply = "HTTP/1.0 500 Internal Sever Error\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>Inner Error</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>服务器内部出错.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client_sock, reply, strlen(reply));
	if (debug) {
        fprintf(stdout, reply);
    }
	
	if (len <= 0) {
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}


/**
 * 400 客户端发送的请求格式有问题等
*/
void bad_request(int client_sock) {
    const char * reply = "HTTP/1.0 400 BAD REQUEST\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML>\r\n\
<HEAD>\r\n\
<TITLE>BAD REQUEST</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>Your browser sent a bad request！\r\n\
</BODY>\r\n\
</HTML>";

    int len = write(client_sock, reply, strlen(reply));
    if (debug) {
        fprintf(stdout, reply);
    }
    if (len<=0) {
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }	
}


/**
 * 501 方法未实现
*/
void unimplemented(int client_sock) {
	const char * reply = "HTTP/1.0 501 Method Not Implemented\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML>\r\n\
<HEAD>\r\n\
<TITLE>Method Not Implemented</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>HTTP request method not supported.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client_sock, reply, strlen(reply));
	if (debug) {
        fprintf(stdout, reply);
    }
	
	if (len <=0) {
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}
