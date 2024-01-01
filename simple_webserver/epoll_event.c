#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <assert.h>
#include <signal.h>
#include "package.h"
#include "http.h"

//sockfd事件到逻辑单元处理
void read_client_request( int epollfd, struct epoll_event *event )
{
    char buf[1024];//读取http请求行
    char tmp[1024];//读取http报文剩下行
    int ret = Readline( event->data.fd, buf, 1024 );
    if( ret <= 0 )
    {
        printf( "the oppsite client has an error or closed\n" );
        epoll_ctl( epollfd, EPOLL_CTL_DEL, event->data.fd,  event );
        close( event->data.fd );
        return;
    }
    //printf( "http请求行:[%s]\n", buf );

    while( ( ret = Readline( event->data.fd, tmp, 1024 ) ) > 0 );

    //printf("一则http报文读取完毕！！！！\n");
    char method[256]="";//方法
    char content[256]="";//请求的内容
    char protocol[256]="";//协议
    
    sscanf( buf, "%[^ ] %[^ ] %[^ \r\n]", method, content, protocol );
    //%[^ ]遇到空格停止赋值 %[^ \r\n]遇到空格、回车、换行不要
    printf( "[%s] [%s] [%s]\n", method, content, protocol );

    //如果是操作方法是：get
    if( strcasecmp(method,"GET" ) == 0 )
    {
        char *strfile = content+1;//略过'/'
        
        strdecode(strfile,strfile);

        printf("第一个：%c",strfile[0]);

        if(*strfile == 0)
        {   //若没有请求就直接换到 求主目录文件
            strfile = "./";
        }

        struct stat root;

        if(stat(strfile,&root)< 0)
        {   //判断存不存在该文件，若不存在发送error.html
            printf("file not found \n");

            send_head( event->data.fd, 404, "NOT FOUND", get_mime_type("*.html"), 0 ); //发送HTTP响应
            send_file( event->data.fd, "error.html" ); //发送html文件
        }
        else
        {   //进入若文件存在流
            //1.请求的是一个普通文件
            if( S_ISREG( root.st_mode ) )
            {
                printf("file\n");
                send_head( event->data.fd, 200, "OK", get_mime_type(strfile), root.st_size);
                send_file( event->data.fd, strfile );
            }

            //2.请求的若是一个目录
            else if( S_ISDIR( root.st_mode ) )
            {
                printf("dir\n");
                //发送状态行、首部行和空行
                send_head( event->data.fd, 200, "OK", get_mime_type("*.html"), 0 );//
                //发送dirhead
				send_file( event->data.fd, "dir_header.html" );

                //浏览目录所有文件，并将文件依次通过<li></li> 发送，加个超链接<a></a>
                struct dirent ** mylist = NULL;

                char buf[1024] = "";
                int len = 0;
                int n = scandir( strfile, &mylist, NULL, alphasort );
                //dirp：目标路径名
                //namelist：mylist地址
                //filter：过滤的函数入口
                //compar：排序函数指针， alphasort：文件名排序，versionsort：版本排序
                int i = 0 ;

                //扫描目录所有文件
                for( i = 0; i < n ; i ++ )
                { 
                    if( mylist[i]->d_type == DT_DIR )//若是DIR
                    {
                        len = sprintf( buf, "<li><a href=%s/ >%s</a></li>",  mylist[i]->d_name, mylist[i]->d_name ); 
                        //注意我们超链接目录时，一定要到要后面加个‘/’,
                        //否则比如目录pub，没加后就会/pub,再请求文件如ASD则成了/pubASD而应该是/pub/ASD
                    }
                    else//普通文件
                    { 
                        len = sprintf( buf, "<li><a href=%s >%s</a></li>", mylist[i]->d_name, mylist[i]->d_name ); 
                        //HTTP点击超链接后会在原名后加上%s,如/pub/里点击asd /pub/asd
                    }
                    

                    send( event->data.fd , buf, len, 0 );//发送给cfd
                    free(mylist[i]);
                }
                free(mylist);
                //最后发送dir tail
                send_file( event->data.fd, "dir_tail.html" ); 
            }
        }
        printf("回应已发送，断开一个连接。。。。\n");
        close( event->data.fd );
        epoll_ctl( epollfd, EPOLL_CTL_DEL, event->data.fd, event );
    }        
}

int main(int argc,char *argv[])
{
    //切换工作目录.../web_http
    //获取当前工作路径
    char pwd_path[256]="";
    char *path=getenv("PWD");
    strcpy(pwd_path,path);
    strcat(pwd_path,"/web_http");
    chdir(pwd_path);

    signal(SIGPIPE,SIG_IGN);
    //避免因发送内容过大死去
    //SIGPIPE：当服务器close一个连接时，若client端接着发数据。根据TCP协议的规定，会收到一个RST响应，
    //client再往这个服务器发送数据时，系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不要再写了。
    //某些服务器满了的时候会短暂关闭这个socket
    //SIG_IGN:信号处理函数(忽略该信号)

    if(argc != 3)
    {
        printf("usage: %s ip_address prot_number\n",basename(argv[0]));
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi( argv[2] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof(address) );
    assert( ret != -1 );

    ret = listen ( listenfd, 1024 );
    assert( ret != -1 );

    //创建内核事件表
    struct epoll_event events[ 1024 ];
    int epollfd = epoll_create( 5 );

    addfd( epollfd, listenfd );

    while( 1 )
    {
        int i = 0;
        char cip[20] = "";
        int number = epoll_wait( epollfd, events, 1024, -1 );//阻塞
        if( ( number < 0 ) && ( errno != EINTR) )
        {
            perror( "epoll_wait" );
            break;
        }

        for( i = 0; i < number; i ++ )
        {
            int sockfd = events[i].data.fd;
            // 新到达的客户
            if( ( sockfd == listenfd ) && ( events[i].events & EPOLLIN ) )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if( connfd < 0 )
                {
                    perror( "accept" );
                    break;
                }
                addfd( epollfd, connfd );
                printf( "new client ip=%s port=%d\n",
					inet_ntop( AF_INET, &client_address.sin_addr.s_addr, cip, 16 ),
					ntohs( client_address.sin_port ) );
            }
            else if( events[i].events & EPOLLIN )
            {
                read_client_request( epollfd, &events[i] ); //cfd事件到逻辑单元处理
            }
            
        }
    }
    return 0;
}