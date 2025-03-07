#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h> 
#include <netinet/in.h>
#include <arpa/inet.h>

// 逐行读取以 \r\n 结尾的数据
int read_line(struct bufferevent *bev, char *buf, int size)
{
    int i=0;
    while(i<size-1){
        
        int n=bufferevent_read(bev, &buf[i], 1);    //bufferevent_read 不设置errno
        if(n<=0){
            if(n==0){
                buf[i]='\0';
                return i;
            }
            else{
                return -1;
            }
        }
        else{
            if(i>0&&buf[i-1]=='\r'&&buf[i]=='\n'){
                buf[i+1]='\0';
                return i+1;
            }
        }
        i++;
    }
    // 跳出循环 说明缓冲区已满
    buf[i]='\0';
    return 0;
}

// 发送http响应头 状态码，状态码描述，文件类型，其他参数（调用时可传NULL）
void send_respond_head(int num, char *discription, char *type, int len, void *arg)
{
    struct bufferevent *bev=(struct bufferevent *)arg;
    char buf[1024]={0};
    sprintf(buf, "HTTP/1.1 %d %s\r\n", num, discription);
    sprintf(buf+strlen(buf),"Content-Type: %s\r\n", type);
    sprintf(buf+strlen(buf), "Content-Length: %d\r\n", len);
    sprintf(buf+strlen(buf), "\r\n");
    //write(STDOUT_FILENO, buf, strlen(buf));
    bufferevent_write(bev, buf, strlen(buf));
}

// 发送数据
void send_respond_body(const char *file, struct bufferevent *bev)
{
    char buf[1024];
    int fd=open(file, O_RDONLY);
    if (fd == -1) {
        perror("open error");
        return;
    }
    int n;
    // 循环读取数据，每次读取出来的数据都发送给浏览器
    while((n=read(fd, buf, sizeof(buf)))>0){
        //write(STDOUT_FILENO, buf, strlen(buf));
        bufferevent_write(bev, buf, n);
    }
    close(fd);
}

// 处理http请求 包括判断资源是否存在，判断是文件还是目录，返回响应
void http_requst(const char *file, struct bufferevent *bev)
{
    int n;
    // 判断文件是否存在。使用 stat() 函数
    struct stat sbuf;
    n= stat(file, &sbuf);
    if(n==-1){
        perror("stat error");
        // 文件不存在 返回错误页面
        bufferevent_write(bev, "404 not found\n", sizeof("404 not found\n"));
        bufferevent_free(bev);
        return;
    }
    // 判断是文件还是目录
    // 如果是文件 
    if(sbuf.st_mode&__S_IFREG){
        // 先发回http响应报文的状态行和消息报头
        send_respond_head(200, "OK", "text/html; charset=UTF-8", sbuf.st_size, (void *)bev);
        // 再发回数据
        send_respond_body(file, bev);
    }
}

// 读回调
void read_cb(struct bufferevent *bev, void *args)
{
    int n;
    // 1.获取协议的请求行
    char buf[1024]={0};
    n=read_line(bev, buf, sizeof(buf)-1);
    if(n==-1){
        printf("read_line error");
        bufferevent_free(bev);
        return;
    }
    else if(n==0){
        printf("缓冲区已满，未获取完整一行数据\n");
        bufferevent_free(bev);
        return;
    }
    // 2.从首行中拆分 GET、文件名、协议版本。得到用户请求的文件名
    char methed[16], path[256], protocol[16];
    sscanf((const char*)buf, "%[^ ] %[^ ] %[^ ]", methed, path, protocol);
    printf("methed=%s, path=%s, protocol=%s\n", methed, path, protocol);
    if(strncasecmp(methed, "GET", 3)==0){
        char *file=path+1;  // 去掉/
        // 3.处理http请求
        http_requst(file, bev);
    }
}

// 写回调
void write_cb(struct bufferevent *bev, void *args)
{
    printf("send success\n");
    int len = evbuffer_get_length(bufferevent_get_output(bev));
    if (len == 0) {
        bufferevent_free(bev);
    }
}

// 事件回调
void event_cb(struct bufferevent *bev, short events, void *args)
{
    if (events & BEV_EVENT_ERROR) {
        printf("Error from bufferevent\n");
    }
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);
    }
}

// 监听新连接的回调函数
void accept_cb(struct evconnlistener *listener, evutil_socket_t cfd, struct sockaddr *addr, int len, void *ptr)
{
    struct event_base *base=evconnlistener_get_base(listener);
    struct bufferevent *bev=bufferevent_socket_new(base, cfd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    bufferevent_setcb(bev, read_cb, write_cb, event_cb, NULL);
    
    return;
}

// 启动服务器
void run_server(int port)
{
    printf("Starting server on port %d\n", port);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_port=htons(port);
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    struct event_base *base=event_base_new();
    if (!base) {
        fprintf(stderr, "Could not create event_base\n");
        return;
    }
    struct evconnlistener *listener=evconnlistener_new_bind(base, accept_cb, NULL, LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1, 
                                                                 (const struct sockaddr *)&addr, sizeof(addr));
    if (!listener) {
        fprintf(stderr, "Could not create listener\n");
        event_base_free(base);
        return;
    }
    event_base_dispatch(base);
    evconnlistener_free(listener);
    event_base_free(base);

    return;
}

int main(int argc, char *argv[])
{
    // 检查命令行参数 端口号和资源文件目录
    if(argc<3){
        printf("./server port path\n");
        exit(1);
    }
    // 获取用户输入的端口
    int port=atoi(argv[1]);
    // 改变进程工作目录
    int ret=chdir(argv[2]);
    if(ret==-1){
        perror("chdir error");
        exit(1);
    }
    // 启动libevent监听
    run_server(port);

    return 0;
}