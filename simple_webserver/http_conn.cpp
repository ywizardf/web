#include"http_conn.h"
/*定义HTTP响应的一些状态信息*/
const char*ok_200_title="OK";
const char*error_400_title="Bad Request";
const char*error_400_form="Your request has bad syntax or is inherently impossible to satisfy.\n";
const char*error_403_title="Forbidden";
const char*error_403_form="You do not have permission to get file from this server.\n";
const char*error_404_title="Not Found";
const char*error_404_form="The requested file was not found on this server.\n";
const char*error_500_title="Internal Error";
const char*error_500_form="There was an unusual problem serving the requested file.\n";

/*网站的根目录*/
const char*doc_root="";
int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    if(one_shot)
    {
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

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

int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;
void http_conn::close_conn(bool real_close)
{
    if(real_close&&(m_sockfd!=-1))
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;/*关闭一个连接时，将客户总量减1*/
    }
}

void http_conn::init(int sockfd,const sockaddr_in&addr)
{
    m_sockfd=sockfd;
    m_address=addr;
    /*如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉*/
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

void http_conn::init()
{
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_linger=false;
    m_method=GET;
    m_url=0;
    m_version=0;    
    m_content_length=0;
    m_host=0;
    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;
    m_write_idx=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
    memset(m_buf,'\0',sizeof(m_buf));
    is_file=false;
    is_dir=false;
    len_a=0;
}

/*从状态机，其分析请参考8.6节，这里不再赘述*/
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx)
    {
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r')
        {
            if((m_checked_idx+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1]=='\n')
            {
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp=='\n')
        {
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r'))
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

/*循环读取客户数据，直到无数据可读或者对方关闭连接*/
bool http_conn::mread()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read=0;
    while(true)
    {
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read==0)
        {
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}

/*解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号*/
http_conn::HTTP_CODE http_conn::parse_request_line(char*text)
{
    m_url=strpbrk(text," \t");
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++='\0';
    char*method=text;
    if(strcasecmp(method,"GET")==0)
    {
        m_method=GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    m_url+=strspn(m_url," \t");
    m_version=strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version," \t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        m_url=strchr(m_url,'/');
    }
    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}

/*解析HTTP请求的一个头部信息*/
http_conn::HTTP_CODE http_conn::parse_headers(char*text)
{
    /*遇到空行，表示头部字段解析完毕*/
    if(text[0]=='\0')
    {
        /*如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态*/
        if(m_content_length!=0)
        {
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        /*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    }
    /*处理Connection头部字段*/
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger=true;
        }
    }
    /*处理Content-Length头部字段*/
    else if(strncasecmp(text,"Content-Length:",15)==0)
    {
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
    }
    /*处理Host头部字段*/
    else if(strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else
    {
        printf("oop!unknow header%s\n",text);
    }
    return NO_REQUEST;
}

/*我们没有真正解析HTTP请求的消息体，只是判断它是否被完整地读入了*/
http_conn::HTTP_CODE http_conn::parse_content(char*text)
{
    if(m_read_idx>=(m_content_length+m_checked_idx))
    {
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*主状态机*/
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char*text=0;
    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))||((line_status=parse_line())==LINE_OK))
    {
        text=get_line();
        m_start_line=m_checked_idx;
        printf("got 1 http line:%s\n",text);
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret=parse_headers(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存
在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address
处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    // strcpy(m_real_file,doc_root);
    // int len=strlen(doc_root);
    // strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);

    if( !(m_url[0]=='/' && m_url[1]=='\0') )
        strncpy(m_real_file,m_url+1,FILENAME_LEN-1);
    else
    {
        m_real_file[0]='.';
        m_real_file[1]='/';
        m_real_file[2]='\0';
    }

    strdecode(m_real_file,m_real_file);

    if(stat(m_real_file,&m_file_stat)<0)
    {
        memset(m_real_file,'\0',sizeof(m_real_file));
        char path[]={"error.html"};
        strncpy(m_real_file,path,FILENAME_LEN-1);
        stat(m_real_file,&m_file_stat);

        m_fd = open( "error.html", O_RDONLY );//发送error.html
        //setnonblocking( fd );
        if( m_fd < 0 )
        {
            perror( "oepn" );
            return BAD_REQUEST;
        }
        is_file=true;
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode&S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode))//访问的是目录
    {
        is_dir=true;
        return DIR_REQUEST;
    }
    //int fd=open(m_real_file,O_RDONLY);
    //m_file_address=(char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    //close(fd);
    m_fd = open( m_real_file, O_RDONLY );//打开要发送的文件
    //setnonblocking( m_fd );
    if( m_fd < 0 )
    {
        perror( "oepn" );
        return BAD_REQUEST;
    }
    is_file=true;
    return FILE_REQUEST;
}

/*对内存映射区执行munmap操作*/
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

//大文件发送
void http_conn::send_file( char *path )
{
    //创建内核事件表
    struct epoll_event events[ 5 ];
    int w_epollfd = epoll_create( 5 );

    epoll_event event;
    event.data.fd = m_sockfd;
    event.events = EPOLLOUT ;
    epoll_ctl( w_epollfd, EPOLL_CTL_ADD, m_sockfd, &event );
    setnonblocking( m_sockfd );

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
                int number = epoll_wait( w_epollfd, events, 5, -1 );//阻塞
                if(number)
                {
                    n = send( m_sockfd, buf, len, 0 );
                    printf( "len=%d\n", n );//发送了字节数为n的数据
                    break;
                }
            }
        }
    }
    close( fd );
    close( w_epollfd );
}

/*写HTTP响应*/
bool http_conn::write()
{
    // int temp=0;
    // int bytes_have_send=0;
    // int bytes_to_send=m_write_idx;
    // if(bytes_to_send==0)    
    // {
    //     modfd(m_epollfd,m_sockfd,EPOLLIN);
    //     init();
    //     return true;
    // }
    // while(1)
    // {
    //     temp=writev(m_sockfd,m_iv,m_iv_count);
    //     if(temp<=-1)
    //     {
    //         /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无
    //         法立即接收到同一客户的下一个请求，但这可以保证连接的完整性*/
    //         if(errno==EAGAIN)
    //         {
    //             modfd(m_epollfd,m_sockfd,EPOLLOUT);
    //             return true;
    //         }
    //         //unmap();
    //         return false;
    //     }
    //     bytes_to_send-=temp;
    //     bytes_have_send+=temp;
    //     if(bytes_to_send<=bytes_have_send)
    //     {
    //         /*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
    //         //unmap();
    //         if(m_linger)
    //         {
    //             init();
    //             modfd(m_epollfd,m_sockfd,EPOLLIN);
    //             return true;
    //         }
    //         else
    //         {
    //             modfd(m_epollfd,m_sockfd,EPOLLIN);
    //             return false;
    //         }
    //     }
    // }
    
    //先发送状态行和首部行
    send( m_sockfd, m_write_buf, m_write_idx, 0 );
    printf("\n发送头部：%s\n",m_write_buf);
    m_write_idx=0;

    /*发送实体主体*/
    if( is_file )//文件
    {
        printf("\n发送文件\n");
        send_file( m_real_file );
    }
    
    if( is_dir )//目录的html的文件
    {
        printf("\n发送目录\n");
        char dir_header[]={"dir_header.html"};
        send_file( dir_header );//html头部

        //浏览目录所有文件，并将文件依次通过<li></li> 发送，加个超链接<a></a>
        struct dirent ** mylist = NULL;

        char buf[1024] = "";
        int len = 0;
        int n = scandir( m_real_file, &mylist, NULL, alphasort );
        //dirp：目标路径名
        //namelist：mylist地址
        //filter：过滤的函数入口
        //compar：排序函数指针， alphasort：文件名排序，versionsort：版本排序

        //扫描目录所有文件
        for( int i = 0; i < n ; i ++ )
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
                    
            send( m_sockfd , buf, len, 0 );//发送给cfd
            free(mylist[i]);
        }
        free(mylist);
        char dir_tail[]={"dir_tail.html"};
        send_file( dir_tail );//html尾部
    }

    if(m_linger)
    {
        init();
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return true;
    }
    else
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return false;
    }

    return true;
}

/*往写缓冲中写入待发送的数据*/
bool http_conn::add_response(const char*format,...)
{
    if(m_write_idx>=WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx))
    {
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status,const char*title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

int http_conn::d_len()//可以使用Transfer-Encoding: chunked改进
{
    int length=0;
    struct stat sta1,sta2;
    stat("dir_header.html",&sta1);
    stat("dir_tail.html",&sta2);
    length+=sta1.st_size;
    length+=sta2.st_size;
    //浏览目录所有文件，并将文件依次通过<li></li> 发送，加个超链接<a></a>
    struct dirent ** mylist = NULL;

    char buf[1024] = "";
    int len = 0;
    int n = scandir( m_real_file, &mylist, NULL, alphasort );
    //dirp：目标路径名
    //namelist：mylist地址
    //filter：过滤的函数入口
    //compar：排序函数指针， alphasort：文件名排序，versionsort：版本排序

    //扫描目录所有文件
    for( int i = 0; i < n ; i ++ )
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
        length+=len;
        free(mylist[i]);
    }
    free(mylist);
    return length; 
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char*content)
{
    return add_response("%s",content);
}

/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))    
            {
                return false;
            }   
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404,error_404_title);
            add_headers(m_file_stat.st_size);
            // if(!add_content(error_404_form))
            // {
            //     return false;
            // }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
        case DIR_REQUEST:
        {
            add_status_line(200,ok_200_title);
            add_headers( d_len() );
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            if(m_file_stat.st_size!=0)
            {
                add_headers(m_file_stat.st_size);
                // m_iv[0].iov_base=m_write_buf;
                // m_iv[0].iov_len=m_write_idx;
                // m_iv[1].iov_base=m_file_address;
                // m_iv[1].iov_len=m_file_stat.st_size;
                // m_iv_count=2;
                return true;
            }
            else
            {
                const char*ok_string="<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                {
                    return false;
                }
            }
            break;
        }
        default:
        {
            return false;
        }
    }
    // m_iv[0].iov_base=m_write_buf;
    // m_iv[0].iov_len=m_write_idx;
    // m_iv_count=1;
    return true;
}

/*由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST)    
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    bool write_ret=process_write(read_ret); 
    if(!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}