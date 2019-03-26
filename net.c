/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <limits.h>
#include <stdlib.h>

#include "net.h"
#include "sds.h"

/* Defined in hiredis.c */
//设置错误信息
void __redisSetError(redisContext *c, int type, const char *str);

//关闭redisContext的上下文环境文件
static void redisContextCloseFd(redisContext *c) {
    if (c && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

//设置错误信息
static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
	   //snprintf拷贝字符串
    __redis_strerror_r(errno, (char *)(buf + len), sizeof(buf) - len);
    __redisSetError(c,type,buf);
}

//设置socket
static int redisSetReuseAddr(redisContext *c) {
    int on = 1;
	//setsockopt()设置socket信息
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
		//设置失败，调用错误信息
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
		//关闭文件描述符
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

//创建socket
static int redisCreateSocket(redisContext *c, int type) {
    int s;

	//TCP方式
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
		//socket()创建套接字
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    c->fd = s;
	//创建socket成功设置相关信息
    if (type == AF_INET) {
        if (redisSetReuseAddr(c) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}


//改变redisContext的上下文环境文件状态，若为阻塞变为非阻塞，为非阻塞变为阻塞
static int redisSetBlocking(redisContext *c, int blocking) {
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
		//F_GETFL 取得文件描述符状态旗标，此旗标为open（）的参数flags。
		//获取文件的flags，即open函数的第二个参数:阻塞或非阻塞
		
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
	   //非阻塞为阻塞
    else
        flags |= O_NONBLOCK;
	  //阻塞为非阻塞

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
	//设置文件的flags
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}


//心跳包机制（保活）
int redisKeepAlive(redisContext *c, int interval) {
    int val = 1;
    int fd = c->fd;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval;

#ifdef _OSX
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    val = interval;
    //设置对一个连接进行有效性探测之前运行的最大非活跃时间间隔
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
	//设置两个探测的时间间隔
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = 3;
	//设置关闭一个非活跃连接之前进行探测的最大次数
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#endif
#endif

    return REDIS_OK;
}


//禁用了Nagle算法，允许小包的发送
static int redisSetTcpNoDelay(redisContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
		//禁用了Nagle算法，允许小包的发送
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        redisContextCloseFd(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)
//long_max 指long类型的最大值，对应的是long_min


//获取超时时间
static int redisContextTimeoutMsec(redisContext *c, long *result)
{
    const struct timeval *timeout = c->timeout;
    long msec = -1;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            *result = msec;
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

       //时间超出范围时，为INT_MAX
        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return REDIS_OK;
}

//查看是否有等待的事件
static int redisContextWaitReady(redisContext *c, long msec) {
    struct pollfd   wfd[1];

    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;
    //POLLOUT 	数据可写（普通数据&优先数据）
    if (errno == EINPROGRESS) {
        int res;

        //失败时，poll()返回-1
        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            redisContextCloseFd(c);
            return REDIS_ERR;
		//如果在超时前没有任何事件发生，poll()返回0
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            redisContextCloseFd(c);
            return REDIS_ERR;
        }

		//若有事件发生，查看网络连接是否发生错误
        if (redisCheckSocketError(c) != REDIS_OK)
            return REDIS_ERR;

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    redisContextCloseFd(c);
    return REDIS_ERR;
}


//获取套接字的错误信息
int redisCheckSocketError(redisContext *c) {
    int err = 0;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
	 //获取与某个套接字关联的选项。
	 //当一个socket发生错误的时候，将使用一个名为so_error的变量记录对应的错误代码
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        return REDIS_ERR;
    }

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

//设置socket接收、发送数据超时时间
int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)) == -1) {
	  //设置socket接收数据超时时间
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)) == -1) {
		//发送数据超时时间
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}



static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    int s, rv, n;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & REDIS_BLOCK);
    int reuseaddr = (c->flags & REDIS_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = REDIS_CONN_TCP;
    c->tcp.port = port;

    /*我们需要拥有已传递的参数，以使它们在重新连接时可重用。
    我们也会仔细检查我们是否释放了我们已经拥有的数据，就像重新连接方法一样。
    这有点难看，但至少它可以工作，而且不会泄漏内存。*/
    
    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but atleast it works and doesn't leak memory.
     **/
     
    if (c->tcp.host != addr) {
        if (c->tcp.host)
            free(c->tcp.host);

        c->tcp.host = strdup(addr);
    }

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
		//获取超时时间失败
        __redisSetError(c, REDIS_ERR_IO, "Invalid timeout specified");
        goto error;
    }

    if (source_addr == NULL) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = strdup(source_addr);
		//strdup 将串拷贝到新建的位置处
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Try with IPv6 if no IPv4 address was found. We do it in this order since
     * in a Redis client you can't afford to test if you have IPv6 connectivity
     * as this would add latency to every connect. Otherwise a more sensible
     * route could be: Use IPv6 if both addresses are available and there is IPv6
     * connectivity. */

	//检查地址是否正确（IPv4 ，IPv6）
    if ((rv = getaddrinfo(c->tcp.host,_port,&hints,&servinfo)) != 0) {
		//gethostbyname()函数完成主机名到地址解析，这个函数仅仅支持IPv4
         hints.ai_family = AF_INET6;
         if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
            __redisSetError(c,REDIS_ERR_OTHER,gai_strerror(rv));
            return REDIS_ERR;
        }
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
	//由于一个域名可以对应多个IP地址，addrinfo也就支持了这个场景。
addrretry:
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
			//socket()创建套接字
            continue;

        c->fd = s;
		
		//设置非阻塞
        if (redisSetBlocking(c,0) != REDIS_OK)
            goto error;
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
		   //使用getaddrinfo可以避免我们自确定的IPv4与IPv6
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }

            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*) &n,
                               sizeof(n)) < 0) {
                 /*SO_REUSEADDR这个套接字选项通知内核，如果端口忙，但TCP状态位于 TIME_WAIT ，可以重用端口。
                 如果端口忙，而TCP状态位于其他状态，重用端口时依旧得到一个错误信息，指明"地址已经使用中"*/
                    goto error;
                }
            }

            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
					//绑定
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
			/*由getaddrinfo返回的存储空间，
			包括addrinfo结构、ai_addr结构和ai_canonname字符串，
			都是用malloc动态获取的。这些空间可调用 freeaddrinfo释放。*/
			
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                redisContextCloseFd(c);
                continue;
            } else if (errno == EINPROGRESS && !blocking) {
                /* This is ok. */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                if (++reuses >= REDIS_CONNECT_RETRIES) {
                    goto error;
                } else {
                    redisContextCloseFd(c);
                    goto addrretry;
                }
            } else {
                if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                    goto error;
            }
        }
        if (blocking && redisSetBlocking(c,1) != REDIS_OK)
            goto error;
        if (redisSetTcpNoDelay(c) != REDIS_OK)
            goto error;

        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

error:
    rv = REDIS_ERR;
end:
    freeaddrinfo(servinfo);
    return rv;  // Need to return REDIS_OK if alright
}

/*
Unix domain socket,也叫IPC socket(inter-precess communication socket,
也就是进程间通信套接字),用于同一台主机上的不同进程间交换数据，
是Posix系统的标准组件。

IP socket要利用主机的传输层(tcp)，
可以用于同一台主机上不同进程间的通信，
也可以用于网络上不同主机间的通信。
*/



//任意客户端连接服务器
int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _redisContextConnectTcp(c, addr, port, timeout, NULL);
}

//指定客户端连接服务器
int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

//连接本机（Unix域套接字用于同一台机器上运行的进程之间的通信）
int redisContextConnectUnix(redisContext *c, const char *path, const struct timeval *timeout) {
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un sa;
    long timeout_msec = -1;

//创建AF_LOCAL本的socket
    if (redisCreateSocket(c,AF_LOCAL) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,0) != REDIS_OK)
        return REDIS_ERR;

    c->connection_type = REDIS_CONN_UNIX;
    if (c->unix_sock.path != path)
        c->unix_sock.path = strdup(path);

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    //获取超时时间
    if (redisContextTimeoutMsec(c,&timeout_msec) != REDIS_OK)
        return REDIS_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
          //查看是否有等待的事件
            if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
	//连接后将socket复位为阻塞
    if (blocking && redisSetBlocking(c,1) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
