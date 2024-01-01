#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>

//将文件设置为非阻塞
int setnonblocking( int fd )
{
	int old_option = fcntl( fd, F_GETFL );
	int new_option = old_option | O_NONBLOCK;
	fcntl( fd, F_SETFL, new_option );
	return old_option;
}

//向内核事件表中添加事件
void addfd( int epollfd, int fd )
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN ;
	epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
	setnonblocking( fd );
}

//向内核事件表中添加事件
void addfd1( int epollfd, int fd )
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLOUT ;
	epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
	setnonblocking( fd );
}

//展示当前目录
void showcwd()
{
	char buf[512];
	getcwd( buf, 512 );
	printf( "Current pwd is %s \n", buf );
}

//读取一个子节
static ssize_t my_read( int fd, char *ptr )
{
	static int read_cnt;//还未读取得字节数
	static char *read_ptr;//指向读取缓冲区
	static char read_buf[100];//读取缓冲区

	//先读一个缓存区，然后一个字节一个字节读，直到为0读取下一个缓冲区
	if ( read_cnt <= 0 ) 
	{
	again:
		if ( ( read_cnt = read( fd, read_buf, sizeof( read_buf ) ) ) < 0 ) 
		{
			if ( errno == EINTR )
				goto again;
			return -1;
		} 
		else if ( read_cnt == 0 )//对方已关闭连接
			return 0;
		read_ptr = read_buf;
	}
	read_cnt--;
	*ptr = *read_ptr++;//*ptr=*read_ptr;ptr++;
	return 1;
}

ssize_t Readline( int fd, void *vptr, size_t maxlen )//读取一行
{
	ssize_t n, rc;//读取的一行的字节数，
	char c, *ptr; 

	ptr = (char *)vptr;//将数据读入vptr
	
	for ( n = 1; n < maxlen; n ++ ) 
	{
		if ( ( rc = my_read(fd, &c) ) == 1 ) 
		{
			*ptr++ = c;//*ptr=c;ptr++; 
			if ( c  == '\n' )
				break;
		} 
		else if ( rc == 0 ) 
		{
			*ptr = 0;
			return n - 1;
		}
		 else
			return -1;
	}
	*ptr  = 0;
	return n;//读取一行的字节数（包括换行符）
}
