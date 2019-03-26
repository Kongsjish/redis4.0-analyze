/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif


//创建一个事件管理器，需要初始化文件事件，触发事件，定时器事件等，stop值默认为0，
//最大文件描述符值为-1，并将所有的文件描述符的监控事件类型设置为NULL。

aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */

	//mask == AE_NONE的事件没有设置。因此，让我们用它初始化向量
     
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
//返回当前设置的大小

//返回管理器能管理的事件的个数
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */

/*
调整事件循环的最大集大小。

如果请求的集大小小于当前集大小，但是已经使用了一个文件描述符>=请求的集大小减去1，
则返回AE_ERR，并且根本不执行操作。

否则返回AE_OK，操作成功。*/

//重置管理器能管理的事件个数
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    /*如果新大小等于现有的大小则直接返回成*/
    if (setsize == eventLoop->setsize) return AE_OK;
	 /*如果重置的大小小于当前管理器中最大的文件描述符大小，则不能进行重置，否则会丢失已经注册的事件。*/
    if (eventLoop->maxfd >= setsize) return AE_ERR;
	/*调用已经封装好的重置大小函数*/
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

	/*重置记录内存块大小*/
    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    //确保如果我们创建了新槽，它们是用AE_NONE掩码初始化的。
	
     /*将新增部分的事件标记置为无效*/
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

//删除EventLoop。释放对应的事件所占的空间
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

//增加一个文件事件，参数为事件控制器，文件描述符，事件掩码，处理函数，
//函数参数，对一个描述符添加多个事件的时候要挨个添加
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
	/*检查新事件的描述符大小是否超过了设置的大小，如果超了，已经申请的内存空间中没有位置，返回错误*/
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    /*设置读写事件的掩码和事件处理函数*/
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/*删除文件事件中指定文件描述符的指定事件*/
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    /*检查文件描述符是否超限*/
    if (fd >= eventLoop->setsize) return;
	/*检查该位置是否启用了*/
    aeFileEvent *fe = &eventLoop->events[fd];
	
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */

	//如果在删除AE_WRITABLE时设置了AE_BARRIER，我们希望总是删除AE_BARRIER。
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    /*先把事件从处理模型中去掉*/
    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask); /*去掉读写掩码*/

	/*检查是否需要更新管理器中的最大文件描述符的值*/
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
	   //更新最大文件描述符
        int j;

        /*从最大位置反向找启用的位置，更新最大文件描述符*/
        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

/*获取某个文件描述符的注册事件*/
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

//获取当前精确时间
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

//设定处理事件的时间 
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

//创建时间事件,返回时间事件的id

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    // 更新时间计数器
    long long id = eventLoop->timeEventNextId++;
	// 创建时间事件结构
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;//事件执行状态：出错
    te->id = id;    // 设置 ID
	//设定处理事件的时间 
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);

	te->timeProc = proc;                  // 设置事件处理器
    te->finalizerProc = finalizerProc;    //设置事件释放函数
    te->clientData = clientData;          // 设置私有数据
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;   // 将新事件放入表头
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

//删除事件
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */

/*
搜索第一个要触发的定时器。此操作有助于了解在不延迟任何事件的情况下，
可以将选择放入休眠的时间。如果没有计时器，则返回NULL。

注意这是O(N)因为时间事件是无序的。

可能的优化(目前Redis还不需要，但是……):

1)按顺序插入事件，使最近的只是头部。

更好，但仍然插入或删除定时器是O(N)。

2)使用skiplist将此操作设置为O(1)，并将插入设置为O(log(N))
*/


//获取距离定时结束最近的事件
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */

//处理已达到的事件
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
	//获取当前时间
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */

	/*
    如果将系统时钟移到未来，然后重新设置为正确的值，
    则时间事件可能会以随机方式延迟。这通常意味着计划的操作不会很快执行。
    在这里，我们尝试检测系统时钟倾斜，并强制所有时间事件在
    发生这种情况时都要尽快处理:其思想是，较早地处理事件要比
    无限期地延迟事件危险得多，实践表明确实如此。
	*/
	
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
	//更新最后一次处理时间事件的时间
    eventLoop->lastTime = now;

    // 遍历链表
    // 执行那些已经到达的事件
    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
		//删除计划删除的事件
		
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
         /*
         确保我们不在这个迭代中处理由时间事件创建的时间事件。
         注意，这个检查目前是无用的:我们总是在头部添加新的计时器，
         但是如果我们更改实现细节，这个检查可能会再次有用:
         我们将它保存在这里，以备将来使用。		*/

        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
 /*
处理每个挂起的时间事件，然后处理每个挂起的文件事件(可能由刚刚处理的时间事件回调函数注册)。
*没有特殊标志，函数休眠，直到一些文件事件触发，或当下一次事件发生时(如果有的话)。
*如果标志为0，则函数不执行任何操作并返回。
*如果标志设置了AE_ALL_EVENTS，则处理所有类型的事件。
*如果标记设置了AE_FILE_EVENTS，则处理文件事件。
*如果标记设置了AE_TIME_EVENTS，则处理时间事件。
*如果标志设置AE_DONT_WAIT，则函数将尽快返回，直到全部
*如果标志设置了AE_CALL_AFTER_SLEEP，则调用aftersleep回调函数。
不需要等待就可以处理的事件被处理。
*函数返回处理的事件数
*/

//事件处理
int aeFileEventaeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP *///无事可做吗?尽快返回
    //如果不是文件时间和定时事件返回0
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */

	/*注意，我们希望调用select()，即使没有文件事件要处理，
     只要我们想处理时间事件，就可以休眠，直到下一次事件准备触发*/

	//如果发生的是定时事件
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        //获取距离定时结束最近的事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
		//如果最近事件不为空
        if (shortest) {
            long now_sec, now_ms;

            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */

			//等待下一次事件触发需要多少毫秒?
            long long ms =
                (shortest->when_sec - now_sec)*1000 +
                shortest->when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */

		/*如果我们必须检查事件，但由于AE_DONT_WAIT而需要尽快返回，
		则需要将超时设置为零*/

			//若为非阻塞事件
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */

         //调用多路复用API，只会在超时或某些事件触发时返回。
        numevents = aeApiPoll(eventLoop, tvp);

        /* After sleep callback. *///睡眠后的回调函数。

		//如果poll任务未完成
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        for (j = 0; j < numevents; j++) {
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int fired = 0; /* Number of events fired for current fd. */
			              //当前fd触发的事件数

            /* Normally we execute the readable event first, and the writable
             * event laster. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsynching a file to disk,
             * before replying to a client. */

		/*通常，我们首先执行可读事件，然后执行可写事件laster。

        这很有用，因为有时我们可以在处理查询之后立即提供查询的答复。

        但是，如果掩码中设置了AE_BARRIER，则应用程序要求我们执行相反的操作:
        永远不要在可读事件之后触发可写事件。在这种情况下，我们反转调用。

        例如，当我们希望在be包皮leep()钩子中执行一些操作时，
        比如在响应客户机之前将文件同步到磁盘，这是非常有用的。*/

	     int invert = fe->mask & AE_BARRIER;

	    /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */

		    /*注意“fe->mask & mask & ...”代码:可能已经已处理事件删除了触发的元素，
		    但我们仍然没有处理该元素，因此我们检查该事件是否仍然有效。
            如果调用序列没有反转，则触发可读事件。
		    */

		   //处理读事件
            if (!invert && fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
            }

            /* Fire the writable event. */
			//处理写事件
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */

			//如果必须回调，请在可写事件之后触发可读事件。
            if (invert && fe->mask & mask & AE_READABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */

//等待几毫秒，直到给定的文件描述符变为可写/可读/异常

int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    //事件的结束标志
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
		//执行poll之前回调函数
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
		//处理所有类型的事件并执行poll之后的回调函数
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|AE_CALL_AFTER_SLEEP);
    }
}

//获取API名字
char *aeGetApiName(void) {
    return aeApiName();
}

//设置beforeSleep处理函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

//设置afterSleep处理函数
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}
