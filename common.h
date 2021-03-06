#ifndef __COMMON_H__
#define __COMMON_H__

#include <sys/epoll.h>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include "utils/error_functions.hpp"

/* 内核epoll表的初始大小 */
#define EPOLL_SIZE 5000

#define MAXLINE 8192

// 当系统中只要一个用户的时候给出提示消息
#define CAUTION "\033[31mSYSTEM MESSAGE:\033[0mOnly you in the chat room now!"


// 新用户登录后的欢迎信息
#define SERVER_WELCOME "\033[31mSYSTEM MESSAGE:\033[0m[%s] joined to the chat room,welcome!"

//　当某一用户离开时的通知信息
#define LEAVE_INFO "\033[31mSYSTEM MESSAGE:\033[0m[%s] leaved the chat room!"

// 其他用户收到消息的前缀
#define SERVER_MESSAGE "[%s] say >> %s"


#define INITCOLOR(color)  std::string("\033[1;") + std::string(color) + std::string("m")
#define RED_COLOR "31"
#define GREEN_COLOR "32"
#define YELLOW_COLOR "33"
#define BLUE_COLOR "34"
#define ZERO_COLOR "0"


static inline void addfd(int epFd, int fd, bool enable_et) {
    struct epoll_event ev;
    ev.data.fd = fd;

    /* LT is default */
    ev.events = EPOLLIN;

    /* if enable_et set to  true, then ET is used here*/
    if (enable_et)
        ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epFd, EPOLL_CTL_ADD, fd, &ev) == -1)
        errExit("error epoll_ctl");
}


static inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
        errExit("error fcntl F_GETFL");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        errExit("error fcntl F_SETFL");
}


static inline void trim(const char *strIn, char *strOut) {
    size_t i, j;

    i = 0;

    j = strlen(strIn) - 1;

    while (strIn[i] == ' ')
        ++i;

    while (strIn[j] == ' ')
        --j;

    strncpy(strOut, strIn + i, j - i + 1);

    strOut[j - i + 1] = '\0';
}

#endif