#ifndef __FD_MANAGER_H__
#define __FD_MANAGER_H__
#include "sylar/singleton.h"
#include "sylar/thread.h"
#include <memory>
#include <vector>
/**
 * @brief 文件描述符管理类
 * 用于判断一个句柄是否和socket相关，如果是，就去用hook，否则就直接调用系统调用
 *
 */
namespace sylar
{
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
public:
    typedef std::shared_ptr<FdCtx> ptr;
    FdCtx(int fd);
    ~FdCtx();
    bool init();

    bool isInit() const
    {
        return m_isInit;
    }
    bool isSocket() const
    {
        return m_isSocket;
    }

    bool isClose() const
    {
        return m_isClosed;
    }

    void close();

    void setUserNonblock(bool v)
    {
        m_userNonblock = v;
    }
    bool getUserNonblock() const
    {
        return m_userNonblock;
    }

    void setSysNonblock(bool v)
    {
        m_sysNonblock = v;
    }
    bool getSysNonblock() const
    {
        return m_sysNonblock;
    }

    void setTimeout(int type, uint64_t v);
    uint64_t getTimeout(int type);

private:
    int m_fd;
    bool m_isInit : 1;
    bool m_isSocket : 1;
    bool m_sysNonblock : 1;
    bool m_userNonblock : 1;
    bool m_isClosed : 1;
    uint64_t m_recvTimeout;
    uint64_t m_sendTimeout;
};

class FdManager
{
public:
    typedef RWMutex RWMutexType;
    FdManager();
    FdCtx::ptr get(int fd, bool auto_create = false);
    void del(int fd);

private:
    RWMutexType m_mutex;
    std::vector<FdCtx::ptr> m_datas;
};

typedef Singleton<FdManager> FdMgr;

} // namespace sylar

#endif
