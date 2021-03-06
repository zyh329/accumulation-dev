#include <assert.h>
#include <set>
#include <vector>
#include <map>
#include <thread>
#include <string.h>
#include <iostream>

#include "msgqueue.h"

using namespace std;

#include "SocketLibFunction.h"
#include "fdset.h"
#include "systemlib.h"

#include "connector.h"

ThreadConnector::ThreadConnector(std::function<void(sock, int64_t)> callback)
{
    mCallback = callback;
    mThread = nullptr;
    mIsRun = false;
    mFDSet = nullptr;
}

ThreadConnector::~ThreadConnector()
{
    destroy();
}

void ThreadConnector::startThread()
{
    if (mThread == nullptr)
    {
        mIsRun = true;
        mThread = new std::thread(ThreadConnector::s_thread, this);
    }
}

void ThreadConnector::destroy()
{
    if (mThread != nullptr)
    {
        mIsRun = false;
        if (mThread->joinable())
        {
            mThread->join();
        }
        delete mThread;
        mThread = nullptr;
    }

    mConnectRequests.clear();
    mConnectingInfos.clear();
    mConnectingFds.clear();
}

bool ThreadConnector::isConnectSuccess(struct fdset_s* fdset, sock clientfd)
{
    bool connect_ret = false;

    if (ox_fdset_check(fdset, clientfd, WriteCheck))
    {
        int error;
        int len = sizeof(error);
        if (getsockopt(clientfd, SOL_SOCKET, SO_ERROR, (char*)&error, (socklen_t*)&len) != -1)
        {
            connect_ret = error == 0;
        }
    }

    return connect_ret;
}

void ThreadConnector::checkConnectStatus(struct fdset_s* fdset, int timeout)
{
    if (ox_fdset_poll(fdset, timeout) <= 0)
    {
        return;
    }

    set<sock>       complete_fds;   /*  完成队列    */
    set<sock>       failed_fds;     /*  失败队列    */

#if defined PLATFORM_WINDOWS
    fd_set* error_result = ox_fdset_getresult(fdset, ErrorCheck);
    fd_set* write_result = ox_fdset_getresult(fdset, WriteCheck);

    for (size_t i = 0; i < error_result->fd_count; ++i)
    {
        sock clientfd = error_result->fd_array[i];
        complete_fds.insert(clientfd);
        failed_fds.insert(clientfd);
    }

    for (size_t i = 0; i < write_result->fd_count; ++i)
    {
        sock clientfd = write_result->fd_array[i];
        complete_fds.insert(clientfd);
        if (!isConnectSuccess(fdset, clientfd))
        {
            failed_fds.insert(clientfd);
        }
    }
#else
    for (std::set<sock>::iterator it = mConnectingFds.begin(); it != mConnectingFds.end(); ++it)
    {
        sock clientfd = *it;
        if(ox_fdset_check(fdset, clientfd, ErrorCheck))
        {
            complete_fds.insert(clientfd);
            failed_fds.insert(clientfd);
        }
        else if (ox_fdset_check(fdset, clientfd, WriteCheck))
        {
            complete_fds.push_back(clientfd);
            if (!isConnectSuccess(fdset, clientfd))
            {
                failed_fds.insert(clientfd);
            }
        }
    }
#endif
    for (auto fd : complete_fds)
    {
        ox_fdset_del(fdset, fd, WriteCheck | ErrorCheck);

        map<sock, ConnectingInfo>::iterator it = mConnectingInfos.find(fd);
        if (it != mConnectingInfos.end())
        {
            if (failed_fds.find(fd) != failed_fds.end())
            {
                ox_socket_close(fd);
                mCallback(-1, it->second.uid);
            }
            else
            {
                mCallback(fd, it->second.uid);
            }

            mConnectingInfos.erase(it);
        }

        mConnectingFds.erase(fd);
    }
}

void ThreadConnector::run()
{
    mFDSet = ox_fdset_new();

    while (mIsRun)
    {
        mThreadEventloop.loop(10);

        checkConnectStatus(mFDSet, 0);

        pollConnectRequest();

        checkTimeout();
    }
    ox_fdset_delete(mFDSet);
    mFDSet = nullptr;
}

void ThreadConnector::pollConnectRequest()
{
    mConnectRequests.SyncRead(0);

    while (mConnectingFds.size() < FD_SETSIZE)
    {
        AsyncConnectAddr addr;
        if (mConnectRequests.PopBack(&addr))
        {
            bool addToFDSet = false;
            bool connectSuccess = false;

            struct sockaddr_in server_addr;
            sock clientfd = SOCKET_ERROR;

            ox_socket_init();

            clientfd = ox_socket_create(AF_INET, SOCK_STREAM, 0);
            ox_socket_nonblock(clientfd);

            if (clientfd != SOCKET_ERROR)
            {
                server_addr.sin_family = AF_INET;
                server_addr.sin_addr.s_addr = inet_addr(addr.getIP().c_str());
                server_addr.sin_port = htons(addr.getPort());

                int n = connect(clientfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr));
                if (n < 0)
                {
                    int check_error = 0;
#if defined PLATFORM_WINDOWS
                    check_error = WSAEWOULDBLOCK;
#else
                    check_error = EINPROGRESS;
#endif
                    if (check_error != sErrno)
                    {
                        ox_socket_close(clientfd);
                        clientfd = SOCKET_ERROR;
                    }
                    else
                    {
                        ConnectingInfo ci;
                        ci.startConnectTime = ox_getnowtime();
                        ci.uid = addr.getUID();
                        ci.timeout = addr.getTimeout();

                        mConnectingInfos[clientfd] = ci;
                        mConnectingFds.insert(clientfd);

                        ox_fdset_add(mFDSet, clientfd, WriteCheck | ErrorCheck);
                        addToFDSet = true;
                    }
                }
                else if (n == 0)
                {
                    connectSuccess = true;
                }
            }

            if (connectSuccess)
            {
                mCallback(clientfd, addr.getUID());
            }
            else
            {
                if (!addToFDSet)
                {
                    mCallback(SOCKET_ERROR, addr.getUID());
                }
            }
        }
        else
        {
            break;
        }
    }
}

void ThreadConnector::checkTimeout()
{
    int64_t now_time = ox_getnowtime();

    for (map<sock, ConnectingInfo>::iterator it = mConnectingInfos.begin(); it != mConnectingInfos.end();)
    {
        if ((now_time - it->second.startConnectTime) >= it->second.timeout)
        {
            sock fd = it->first;
            int64_t uid = it->second.uid;

            ox_fdset_del(mFDSet, fd, WriteCheck | ErrorCheck);

            mConnectingFds.erase(fd);
            mConnectingInfos.erase(it++);

            ox_socket_close(fd);

            mCallback(SOCKET_ERROR, uid);
        }
        else
        {
            ++it;
        }
    }
}


void ThreadConnector::asyncConnect(const char* ip, int port, int ms, int64_t uid)
{
    mConnectRequests.Push(AsyncConnectAddr(ip, port, ms, uid));
    mConnectRequests.ForceSyncWrite();
    mThreadEventloop.wakeup();
}

void ThreadConnector::s_thread(void* arg)
{
    ThreadConnector* tc = (ThreadConnector*)arg;
    tc->run();
}