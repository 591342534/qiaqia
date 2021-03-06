/** MIT License

    Copyright (c) 2018 阿超 andrewpqc@mails.ccnu.edu.cn

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
            copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#ifndef _CHATROOM_SERVER_HPP_
#define _CHATROOM_SERVER_HPP_

/*comment this macro to disable some
 * debug output in the program*/
#define __DEVELOPMENT__ 0

#include <map>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <list>
#include <ctime> //time
#include <cstring>
#include <unistd.h> // close
#include <pthread.h> //pthread_create
#include <sys/mman.h> // mmap
#include <fcntl.h>  // fcntl
#include <sys/epoll.h> //epoll_create epoll_ctl epoll_wait
#include <sys/socket.h> // socket getaddrinfo/getnameinfo
#include <netdb.h> //gai_strerror NI_MAXHOST NI_MAXSERV

#include "logger.hpp"
#include "common.h" // addfd set_nonblocking trim
#include "error_functions.hpp"

using namespace vogro;

/* 监听缓冲队列大小 */
#define LISTENQ 50


/* 当系统中只要一个用户的时候给出提示消息 */
#define CAUTION "\033[31mSYSTEM MESSAGE:\033[0mOnly you in the chat room now!"

/* 新用户登录后的欢迎信息 */
#define SERVER_WELCOME "\033[31mSYSTEM MESSAGE:\033[0m[%s] joined to the chat room,welcome!"

/* 当某一用户离开时的通知信息 */
#define LEAVE_INFO "\033[31mSYSTEM MESSAGE:\033[0m[%s] leaved the chat room!"

/* 其他用户收到消息的前缀 */
#define SERVER_MESSAGE "[%s] say >> %s"

/* 表示客户端地址的字符串的最大长度 */
#define ADDRSTRLENGTH  (NI_MAXHOST + NI_MAXSERV +10)


namespace server_ns {

    typedef struct {
        std::string clientIP;
        std::string clientPort;
        std::string clientNickname;
        std::string joinAt;
        int connFd=0;
        bool isNicknameSet=false;
    } ClientInfo; // struct ClientInfo



    static int listenFd;
    static int epollFd;
    static std::map<int, ClientInfo> clients;

    static std::mutex io_event_mux;
    static std::mutex clients_map_mux;
    static Logger<FilePolicy> &logger=Logger<FilePolicy>::getLoggerInstance("qiaqia.log");


    class Server {
    private:
        std::string serverPort;


        int workerNum;

        static int broadcast(int sender_fd, char *msg) {
            for (auto it: clients) {
                if (it.first != sender_fd) {
                    if (send(it.first, msg, MAXLINE, 0) < 0
                        && (errno != EAGAIN || errno != EWOULDBLOCK)) {
                        return -1;
                    }
                }
            }
            return 0;
        }

        static int show_userinfo_to_client(int connfd) {
            char message[MAXLINE];
            sprintf(message, "\033[32mHere are %lu users online now!", clients.size());
            sprintf(message, "%s\nHOST        PORT    JOIN_TIME                  USERNAME", message);
            for (auto client:clients) {
                sprintf(message, "%s\n%s   %s   %s   %s", message,
                        client.second.clientIP.c_str(),
                        client.second.clientPort.c_str(),
                        client.second.joinAt.c_str(),
                        client.second.clientNickname.c_str());
            }
            sprintf(message, "%s\033[0m", message);
            if (send(connfd, message, strlen(message), 0) < 0 && (errno != EAGAIN || errno != EWOULDBLOCK)) {
                return -1;
            }
            return 0;
        }

        int get_listen_fd(const std::string &port) {
            struct addrinfo hints, *listp, *p;
            int listenfd, rc, optval = 1;

            /* Get a list of potential server addresses */
            memset(&hints, 0, sizeof(struct addrinfo));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
            hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
            hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
            if ((rc = getaddrinfo(nullptr, port.c_str(), &hints, &listp)) != 0) {
                fprintf(stderr, "getaddrinfo failed (port %s): %s\n",
                        port.c_str(), gai_strerror(rc));
                return -2;
            }

            /* Walk the list for one that we can bind to */
            for (p = listp; p; p = p->ai_next) {
                /* Create a socket descriptor */
                if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
                    continue; /* Socket failed, try the next */

                /* Eliminates "Address already in use" error from bind */
                setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                           (const void *) &optval, sizeof(int));

                /* Bind the descriptor to the address */
                if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
                    break; /* Success */
                if (close(listenfd) < 0) { /* Bind failed, try the next */
                    fprintf(stderr, "listenfd close failed: %s\n", strerror(errno));
                    return -1;
                }
            }

            /* Clean up */
            freeaddrinfo(listp);
            if (!p) /* No address worked */
                return -1;

            /* Make it a listening socket ready to accept connection requests */
            if (listen(listenfd, LISTENQ) < 0) {
                close(listenfd);
                return -1;
            }

            return listenfd;
        }


        static void store_client_infomation(int connFd, char *hostname, char *port) {
            ClientInfo client;

            client.clientIP = static_cast<std::string>(hostname);
            client.clientPort = static_cast<std::string>(port);
            client.connFd = connFd;
            client.isNicknameSet = false;

            time_t now = time(nullptr);
            char *now_time_str = ctime(&now);
            now_time_str[strlen(now_time_str) - 1] = '\0';
            client.joinAt = std::string(now_time_str);

            clients_map_mux.lock();
            clients.insert(std::pair<int, ClientInfo>(connFd, client));
            clients_map_mux.unlock();
        }

        static void *start_worker(void *arg) {
#if __DEVELOPMENT__

            char workerId = *((char *) arg);
            printf("worker %c started\n", workerId);
#endif
            static struct epoll_event events[EPOLL_SIZE];
            socklen_t client_addr_len;
            struct sockaddr_storage client_addr;

            char hostname[NI_MAXHOST], port[NI_MAXSERV], addr_str[ADDRSTRLENGTH];
            while (true) {
#if __DEVELOPMENT__
                printf("Worker %c try to lock\n", workerId);
#endif
                io_event_mux.lock();
#if __DEVLOPMENT__
                printf("worker %c get lock\n", workerId);
#endif
                int ready_count = epoll_wait(epollFd, events, EPOLL_SIZE, -1);
                io_event_mux.unlock();
                if (ready_count < 0) {
                    perror("epoll_wait failure");
                    break;
                }
#if __DEVELOPMENT__
                /* log the events count which are ready */
                std::cout << "ready_count = " << ready_count << std::endl;
#endif
                for (int i = 0; i < ready_count; ++i) {

                    int readyFd = events[i].data.fd;

                    /*error case*/
                    if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) ||
                        (!(events[i].events & EPOLLIN))) {
                        fprintf(stderr, "epoll error\n");
                        close(events[i].data.fd);
                        continue;
                    }

                    /*new connection*/
                    if (readyFd == listenFd) {
                        client_addr_len = sizeof(struct sockaddr_storage);
                        int conn_fd = accept(listenFd, (struct sockaddr *) &client_addr, &client_addr_len);
                        if (conn_fd == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // we processed all of the connections
                                break;
                            } else {
                                errExit("error accept");
                            }
                        }

                        if (getnameinfo((struct sockaddr *) &client_addr, client_addr_len,
                                        hostname, NI_MAXHOST, port, NI_MAXSERV, 0) == 0)
                            snprintf(addr_str, ADDRSTRLENGTH, "(%s:%s)", hostname, port);
                        else snprintf(addr_str, ADDRSTRLENGTH, "(?UNKNOWN?)");


                        store_client_infomation(conn_fd, hostname, port);

                        /*add the connfd to kernel event table*/
                        addfd(epollFd, conn_fd, true);

                        /*set the conn_fd to nonblock*/
                        set_nonblocking(conn_fd);
#if __DEVELOPMENT__
                        printf("Connection from %s\n", addr_str);
#endif
                    } else { //message is comming
                        if (get_msg_and_forward_to_clients(readyFd) < 0) {
                            perror("errorp");
//                            close(readyFd);
//                            exit(-1);
                        }
                    }
                }

            }
            return nullptr; // unreached statement, to remove the compile warning
        }

        static int get_msg_and_forward_to_clients(int connfd) {

#if __DEVELOPMENT__
            std::cout << "read from client(clientID = " << connfd << ")" << std::endl;
#endif
            char buf[MAXLINE];
            bzero(buf, MAXLINE);
            std::string byte_stream;
            for (;;) {
                ssize_t len = recv(connfd, buf, MAXLINE, 0);

                //error case
                if (len == -1) {
                    if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        //error recv
                        return -1;
                    }
                }

                    //client side closed
                else if (len == 0) {
                    std::cout<<"client close"<<std::endl;
                    if (close(connfd) < 0) errExit("error close fd");


                    const char * the_leave_user_name =  clients[connfd].clientNickname.c_str();
                    //remove client
                    clients_map_mux.lock();
                    clients.erase(connfd);
                    clients_map_mux.unlock();
#if __DEVELOPMENT__
                    //print log to the server side stdout
                std::cout << "ClientID = " << connfd
                          << " closed.\nnow there are "
                          << clients.size()
                          << " client in the chat room"
                          << std::endl;
#endif

                    // broadcast leave information
                    char message[MAXLINE];
                    bzero(message, MAXLINE);
                    sprintf(message, LEAVE_INFO, the_leave_user_name);
                    if (broadcast(connfd, message) == -1) return -1;
                    else return 0;
                } else {
                    // add buf to
                    byte_stream += std::string(buf);

                    bzero(buf, MAXLINE);
                }

            }

            if(byte_stream.empty()){
                return 0;
            }
            // set the current user's nickname,this will be
            // run for the first msg for every client
            if (!clients[connfd].isNicknameSet) {
                clients_map_mux.lock();
                clients[connfd].clientNickname = byte_stream;
                clients[connfd].isNicknameSet = true;
                clients_map_mux.unlock();

                //broadcast the welcome message to all other users
                char message[MAXLINE];
                bzero(message, MAXLINE);
                sprintf(message, SERVER_WELCOME, clients[connfd].clientNickname.c_str());
                if (broadcast(connfd, message) == -1) return -1;
                else return 0;
            }


            // if there only one user in the chat room,
            // send caution message
            if (clients.size() == 1) {
                if (send(connfd, CAUTION, strlen(CAUTION), 0) < 0 && (errno != EAGAIN || errno != EWOULDBLOCK)) {
                    return -1;
                }
            }


            if (strncasecmp(byte_stream.c_str(), "$ show users", strlen("$ show users")) == 0) {
                if (show_userinfo_to_client(connfd) == -1) return -1;
                else return 0;
            } else if (byte_stream[0] == '>') {
                std::size_t pos1 = byte_stream.find_first_of(' ');
                std::size_t pos2 = byte_stream.find_last_of(' ');

                //提取出name,msg
                std::string name = byte_stream.substr(pos1 + 1, pos2 - 1);
                std::string msg = byte_stream.substr(pos2 + 1);

                //去掉可能的空格
                char name_c[name.size()], msg_c[msg.size()];
                trim(name.c_str(), name_c);
                trim(msg.c_str(), msg_c);

                //转发给要求的人
                for (auto client: clients) {
                    if (client.second.clientNickname == (static_cast<std::string>(name_c))) {
                        if (send(client.first, msg_c, strlen(msg_c), 0) < 0 &&
                            (errno != EAGAIN || errno != EWOULDBLOCK)) {
                            return -1;
                        }
                    }
                }
                return 0;
            }

            char message[MAXLINE];
            bzero(message, MAXLINE);
            // format the msg to be send to clients
            sprintf(message, SERVER_MESSAGE, clients[connfd].clientNickname.c_str(), byte_stream.c_str());

//            printf("消息：%s\n", byte_stream.c_str());
            logger.LOG_INFO("消息:",byte_stream);
            // broadcast
            if (broadcast(connfd, message) == -1) return -1;
            else return 0;
        }


    public:
        Server(const std::string &port, int workerNum) {
            this->serverPort = port;
            epollFd = 0;
            listenFd = 0;
            this->workerNum = workerNum;
        }


        ~Server() {
            if (close(listenFd) == -1) errExit("close listenFd");
            if (close(epollFd) == -1) errExit("close epollFd");
        }

        void start_server() {
            /* 获取监听描述符 */
            int listen_fd = this->get_listen_fd(this->serverPort);
            if (listen_fd < 0) errExit("get_listen_fd");

            listenFd = listen_fd;

            if ((epollFd = epoll_create(EPOLL_SIZE)) < 0)
                errExit("epoll_create");

            /* 将监听描述符添加到epoll内核结构中 */
            addfd(epollFd, listenFd, true);

            set_nonblocking(listenFd);

//            int fork_result;
//            for (int i = 1; i <= this->workerNum; ++i) {
//                fork_result = fork();
//                if (fork_result == -1) errExit("fork");
//                if (fork_result > 0) continue;
//                if (fork_result == 0) {
//                    this->start_worker(i);
//                    break;
//                }
//
//            }

            pthread_t tids[this->workerNum];
            char A = 'A';
            for (int i = 0; i < this->workerNum; ++i) {
                int s;
                if ((s = pthread_create(&tids[i], nullptr, start_worker, (void *) &A)) != 0)
                    errExitEN(s, "pthread_create");
                A++;

            }

            printf("server listening on localhost : %s\n", this->serverPort.c_str());

            for (int i = 0; i < this->workerNum; ++i) {
                int s = pthread_join(tids[i], nullptr);
                if (s != 0) errExitEN(s, "pthread_join");
            }


        }

    }; // class Server
} // namespace server_ns
#endif