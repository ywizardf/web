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
#include <ctype.h>

//将16进制字符串的每个字符转化为数字
int hexit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

void strdecode(char *to, char *from)//把一个十六进制数转化为汉字字符
{
    for ( ; *from != '\0'; ++ to, ++ from ) 
    {
        //isxdigit:如果 c 是一个十六进制数字，则该函数返回非零的整数值(1)，否则返回 0。
        if ( from[0] == '%' && isxdigit( from[1] ) && isxdigit( from[2] ) ) { 

            *to = hexit( from[1] )*16 + hexit( from[2] );
            from += 2;                      
        } 
        else
            *to = *from;
    }
    *to = '\0';
}

//通过文件名字获得文件类型
char *get_mime_type(char *name)
{
    char* dot;

    dot = strrchr(name, '.');//自右向左查找'.'字符，如果不存在返回NULL
    //返回值指向这个位置，如果找不到字符 c 就返回 NULL。
    /*
    *charset=iso-8859-1
    *charset=gb2312
    *charset=utf-8
    *charset=euc-kr
    *charset=big5
    *以下是依据传递进来的文件名，使用后缀判断是何种文件类型
    *将对应的文件类型按照http定义的关键字发送回去
    */
  
    if (dot == (char*)0)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp( dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";//如果都没有就是普通文本类型
}

//发送状态行、首部行和空行
void send_head( int cfd, int code, char* info, char* filetype, int length )
{
    char buf[1024] = "";//状态行
    int len = 0;

    //发送状态行
    len = sprintf( buf, "HTTP/1.1 %d %s\r\n", code, info ); //版本 状态 状态信息
    send( cfd, buf, len, 0);
    
    //发送消息报头（首部行）
    len = sprintf( buf, "Content-Type:%s\r\n", filetype ); //发送消息头文件类型，这是必发的
	send( cfd, buf, len, 0 );
    if( length > 0 )
	{
		//发送消息头长度，这是可选的
		len = sprintf( buf, "Content-Length:%d\r\n", length );
		send( cfd, buf, len, 0 );
	}
    //空行
	send( cfd, "\r\n", 2, 0 );
    printf("发送状态行、首部行和空行\n");
}


//发送数据
void send_file( int sockfd, char *path )
{
    //创建内核事件表
    struct epoll_event events[ 5 ];
    int epollfd = epoll_create( 5 );
    addfd1( epollfd, sockfd );

    int fd = open( path, O_RDONLY );//打开要发送的文件
    //setnonblocking( fd );
    if( fd < 0 )
    {
        perror( "oepn" );
        return ;
    }
    
    char buf[1024] = "";
    int len = 0 ;

    while( 1 ) //可能文件数据很大，一次不够,故来个while
    {
        memset( buf, 0, sizeof buf );
        len = read( fd, buf, sizeof( buf )-1 );  //经过IO读到内核再到应用层，一次读完，阻塞。

        if( len < 0 )
        {
            perror( "read" );
            break;
        }
        else if( len == 0 )
        {
            break;  //读完文件
        }
        else if( len>0 )
        {
            int n = -1;
            while(1)
            {
                int number = epoll_wait( epollfd, events, 5, -1 );//阻塞
                if(number)
                {
                    n = send( sockfd, buf, len, 0 );
                    printf( "len=%d\n", n );//发送了字节数为n的数据
                    break;
                }
            }
        }
    }
    close( fd );
}