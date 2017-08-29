#if (!UWP && !WIN32)
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#else
#define USE_WIN32_CODE
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <vector>
#include "getaddrinfo_with_timeout.h"

#include "comm/thread/mutex.h"
#include "comm/thread/lock.h"
#include "comm/thread/thread.h"
#include "comm/thread/condition.h"
#include "comm/xlogger/xlogger.h"
#include "comm/time_utils.h"


enum {
    kGetADDRNotBegin,
    kGetADDRDoing,
    kGetADDRTimeout,
    kGetADDRSuc,
    kGetADDRFail,
};

enum {
    kRetCodeInternalStateError =  -888,
    kRetCodeParamNotMatch,
    kRetCodeDnsItemNotFound,
    kRetCodeGetADDRTimeout
};
struct DnsItem {
    thread_tid      threadid;
    //parameter
    const char *node;
    const char *service;
    const struct addrinfo *hints;
    struct addrinfo **res;
    //~parameter
    
    int error_code;
    int status;
    
    DnsItem(): node(NULL), service(NULL), hints(NULL), res(NULL), error_code(0), status(kGetADDRNotBegin) {}
    
    bool EqualParameter(const DnsItem& _item) {
        return _item.node == node
            && _item.service == service
            && _item.hints == hints
            && _item.res == res;
    }
    std::string ToString() const {
        XMessage xmsg;
        xmsg(TSF"node:%_, service:%_, hints:%_, res:%_, tid:%_, error_code:%_, status:%_",
             NULL==node?"NULL":node
            ,NULL==service?"NULL":service
            ,NULL==hints?(void*)0:hints
            ,NULL==res?(void*)0:res
            ,threadid
            ,error_code
            ,status);
        return xmsg.String();
    }
};

static std::vector<DnsItem> sg_dnsitem_vec;
static Condition sg_condition;
static Mutex sg_mutex;


static void __WorkerFunc() {
    xverbose_function();
    
    
    //parameter
    const char *worker_node = NULL;
    const char *worker_service= NULL;
    const struct addrinfo *worker_hints= NULL;
    struct addrinfo **worker_res= NULL;
    
    ScopedLock lock(sg_mutex);
    std::vector<DnsItem>::iterator iter = sg_dnsitem_vec.begin();
    
    for (; iter != sg_dnsitem_vec.end(); ++iter) {
        if (iter->threadid == ThreadUtil::currentthreadid()) {
            worker_node = iter->node;
            worker_service =  iter->service;
            worker_hints = iter->hints;
            worker_res = iter->res;
            iter->status = kGetADDRDoing;
            break;
        }
    }
    
    lock.unlock();
    
    int error = getaddrinfo(worker_node, worker_service, worker_hints, worker_res);
    
    lock.lock();
    
    iter = sg_dnsitem_vec.begin();
    for (; iter != sg_dnsitem_vec.end(); ++iter) {
        if (iter->threadid == ThreadUtil::currentthreadid()) {
            break;
        }
    }
    
    if (error != 0) {
        if (iter != sg_dnsitem_vec.end()) {
            iter->error_code = error;
            iter->status = kGetADDRFail;
        }
        sg_condition.notifyAll();
    } else {
        if (iter != sg_dnsitem_vec.end()) {
            if (iter->status==kGetADDRDoing) {
                iter->status = kGetADDRSuc;
            } else {
                xinfo2(TSF"getaddrinfo end but timeout. dns_item:%_", iter->ToString());
            }
        }
        
        sg_condition.notifyAll();
    }
}




int getaddrinfo_with_timeout(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res, bool& _is_timeout, unsigned long long _timeout_msec) {
    xverbose_function();
    //Check param
    
    ScopedLock lock(sg_mutex);
    
    
    Thread thread(&__WorkerFunc, node);
    int start_ret = thread.start();
    
    if (start_ret != 0) {
        xerror2(TSF"start the thread fail, host:%_", node);
        return false;
    }
    
    DnsItem dns_item;
    dns_item.threadid = thread.tid();
    dns_item.node = node;
    dns_item.service = service;
    dns_item.hints = hints;
    dns_item.res = res;
    dns_item.status = kGetADDRNotBegin;
    sg_dnsitem_vec.push_back(dns_item);
    
    
    uint64_t time_end = gettickcount() + (uint64_t)_timeout_msec;
    
    while (true) {
        uint64_t time_cur = gettickcount();
        uint64_t time_wait = time_end > time_cur ? time_end - time_cur : 0;
        
        int wait_ret = sg_condition.wait(lock, (long)time_wait);
        
        std::vector<DnsItem>::iterator it = sg_dnsitem_vec.begin();
        
        for (; it != sg_dnsitem_vec.end(); ++it) {
            if (dns_item.threadid == it->threadid)
                break;
        }
        
        xassert2(it != sg_dnsitem_vec.end());
        
        if (it != sg_dnsitem_vec.end()){
            
            if (ETIMEDOUT == wait_ret) {
                it->status = kGetADDRTimeout;
            }
            
            if (kGetADDRDoing == it->status) {
                continue;
            }
            
            if (kGetADDRSuc == it->status) {
                if (it->EqualParameter(dns_item)) {
                    sg_dnsitem_vec.erase(it);
                    return 0;
                } else {
                    std::vector<DnsItem>::iterator iter = sg_dnsitem_vec.begin();
                    int i = 0;
                    for (; iter != sg_dnsitem_vec.end(); ++iter) {
                        xerror2(TSF"sg_dnsitem_vec[%_]:%_", i++, iter->ToString());
                    }
                    xassert2(false, TSF"dns_item:%_", dns_item.ToString());
                    return kRetCodeParamNotMatch;
                }
            }
            
            if (kGetADDRTimeout == it->status ) {
                xinfo2(TSF "dns get ip status:kGetADDRTimeout item:%_", it->ToString());
                sg_dnsitem_vec.erase(it);
                _is_timeout = true;
                return kRetCodeGetADDRTimeout;
            } else if (kGetADDRFail == it->status) {
                xinfo2(TSF "dns get ip status:kGetADDRFail item:%_", it->ToString());
                int ret_code = it->error_code;
                sg_dnsitem_vec.erase(it);
                return ret_code;
            }
            
            xassert2(false, TSF"%_", it->status);
            
            
            sg_dnsitem_vec.erase(it);
        }
        return kRetCodeDnsItemNotFound;
    }
    
    return kRetCodeInternalStateError;
}
