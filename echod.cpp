#include <ace/OS_main.h>
#include <ace/Auto_Ptr.h>
#include <ace/Acceptor.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/SOCK_Stream.h>
#include <ace/Svc_Handler.h>
#include <ace/Select_Reactor.h>

#ifdef _WIN32
#   include <ace/WFMO_Reactor.h>
#else // !_WIN32
#   include <ace/Dev_Poll_Reactor.h>
#endif // _WIN32

#define DEFAULT_REACTOR_TYPE "select"

#define IPV6_ONLY_OPT ACE_TEXT("ipv6only")
#define IPV6_ONLY_OPT_LEN (sizeof(IPV6_ONLY_OPT) / sizeof(ACE_TCHAR) - 1)

typedef ACE_Reactor* (*make_reactor_t) ();

template <typename T>
ACE_Reactor* make_reactor()
{
    ACE_Auto_Ptr<T> impl;

    T* t;
    ACE_NEW_RETURN(t, T, 0);
    impl.reset(t);

    ACE_Reactor* reactor;
    ACE_NEW_RETURN(reactor, ACE_Reactor(impl.get(), 1), 0);

    impl.release();
    return reactor;
}

make_reactor_t get_reactor_factory(const ACE_TCHAR* type)
{
    if (!ACE_OS::strcmp(type, "select")) {
        return make_reactor<ACE_Select_Reactor>;
#ifdef ACE_WIN32
    } else if (!ACE_OS::strcmp(type, "wfmo")) {
        return make_reactor<ACE_WFMO_Reactor>;
#endif // ACE_WIN32
    } else if (!ACE_OS::strcmp(type, "dev_poll")) {
        return make_reactor<ACE_Dev_Poll_Reactor>;
    }

    return 0;
}

static ACE_THR_FUNC_RETURN event_loop(ACE_Reactor* reactor)
{
    reactor->owner(ACE_OS::thr_self());
    while (!reactor->reactor_event_loop_done()) {
        reactor->run_reactor_event_loop();
    }

    return 0;
}

static ACE_Reactor* make_reactor_event_loop(make_reactor_t maker, ACE_thread_t& tid)
{
    ACE_Reactor* const reactor = maker();
    if (!reactor)
        return 0;

    if (ACE_Thread_Manager::instance()->spawn(
                (ACE_THR_FUNC) event_loop,
                reactor,
                THR_NEW_LWP | THR_JOINABLE | THR_INHERIT_SCHED,
                &tid) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          "Failed to spawn event_loop, errno=%d, %m\n",
                          ACE_OS::last_error()),
                          0);
    }

    return reactor;
}

class Event_Loop_Manager
{
public:
    Event_Loop_Manager(): reactors_(0), tids_(0), threads_(0), current_(0)
    {
    }

    ~Event_Loop_Manager()
    {
        close();
    }

    bool open(const char* reactor_type, unsigned threads)
    {
        close();

        ACE_Reactor** r;
        ACE_NEW_RETURN(r, ACE_Reactor*[threads], false);
        reactors_.reset(r);
        memset(r, 0, sizeof(ACE_Reactor*) * threads);

        ACE_thread_t* t;
        ACE_NEW_RETURN(t, ACE_thread_t[threads], false);
        tids_.reset(t);
        memset(t, 0, sizeof(ACE_thread_t*) * threads);

        threads_ = threads;

        make_reactor_t maker = get_reactor_factory(reactor_type);
        if (!maker)
            ACE_ERROR_RETURN((LM_ERROR,
                              ACE_TEXT("Invalid reactor type %s\n"),
                              reactor_type),
                              false);

        ACE_DEBUG((LM_DEBUG,
                   "Event loop: reactor type %s, threads %d\n",
                   reactor_type, threads_));

        for (unsigned i = 0; i < threads_; ++i) {
            reactors_[i] = make_reactor_event_loop(maker, tids_[i]);
        }

        current_ = 0;
        return true;
    }

    void close()
    {
        for (unsigned i = 0; i < threads_; ++i) {
            ACE_Thread_Manager::instance()->join(tids_[i]);
            delete reactors_[i];
            reactors_[i] = 0;
        }

        current_ = threads_ = 0;
        reactors_.reset();
        tids_.reset();
    }

    ACE_Reactor* reactor()
    {
        ACE_Reactor* r = reactors_[current_];
        current_ = (current_ + 1) % threads_;
        return r;
    }
protected:
    ACE_Auto_Basic_Array_Ptr<ACE_Reactor*> reactors_;
    ACE_Auto_Basic_Array_Ptr<ACE_thread_t> tids_;
    unsigned threads_;
    unsigned current_;
};

class Echo_Handler: public ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH>
{
public:
    typedef ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH> super_type;

    virtual int open(void*)
    {
        if (const int rc = super_type::open ())
            return rc;

        ACE_TCHAR buf[BUFSIZ];
        ACE_INET_Addr addr;
        if (peer().get_remote_addr(addr) < 0) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%p\n"),
                       ACE_TEXT("Unable to get remote address")));
        } else if (!addr.addr_to_string(buf, sizeof(buf))) {
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%t) Connected by %s\n"), buf));
        }

        return 0;
    }

    virtual int handle_input(ACE_HANDLE)
    {
        ACE_DEBUG((LM_DEBUG, "(%t) handle_input\n"));
        char buf[BUFSIZ];
        const ssize_t n = peer().recv(buf, sizeof(buf));
        if (n < 0)
            ACE_ERROR_RETURN((LM_ERROR,
                              ACE_TEXT("%p\n"), ACE_TEXT("recv")),
                              -1);

        if (peer().send_n(buf, n) != n)
            ACE_ERROR_RETURN((LM_ERROR,
                              ACE_TEXT("%p\n"), ACE_TEXT("send")),
                              -1);

        return -1;
    }
};

class Echo_Acceptor: public ACE_Acceptor<Echo_Handler, ACE_SOCK_ACCEPTOR>
{
public:
    int open(const addr_type& local_addr,
             ACE_Reactor* reactor,
             int flags,
             int use_select,
             int reuse_addr,
             int ipv6_only,
             const char* reactor_type = 0)
    {
        flags_ = flags;
        use_select_ = use_select;
        reuse_addr_ = reuse_addr;
        peer_acceptor_addr_ = local_addr;

        if (!reactor) {
            errno = EINVAL;
            return -1;
        }

        if (peer_acceptor_.open(local_addr,
                                reuse_addr,
                                PF_UNSPEC,
                                ACE_DEFAULT_BACKLOG,
                                0,
                                ipv6_only) == -1)
            return -1;

        (void) peer_acceptor_.enable(ACE_NONBLOCK);
        const int rc = reactor->register_handler(this, ACE_Event_Handler::ACCEPT_MASK);
        if (rc != -1) {
            this->reactor(reactor);
        } else {
            peer_acceptor_.close();
        }

        if (!loops_.open(reactor_type, ACE_OS::num_processors()))
            return -1;

        ACE_DEBUG((LM_DEBUG, "(%t) Acceptor\n"));
        return rc;
    }

    virtual int make_svc_handler(handler_type*& sh)
    {
        if (!sh)
            ACE_NEW_RETURN(sh, handler_type, -1);

        sh->reactor(loops_.reactor());
        return 0;
    }
protected:
    Event_Loop_Manager loops_;
};

int ACE_TMAIN(int argc, ACE_TCHAR* argv[])
{
    if (argc < 2) {
        ACE_OS::fprintf(stderr,
                        ACE_TEXT("%s <address:port> [<address:port> ...]\n"),
                        *argv);
        return 1;
    }

    const char* reactor_type = ACE_OS::getenv("REACTOR_TYPE");
    if (!reactor_type)
        reactor_type = DEFAULT_REACTOR_TYPE;

    ACE_LOG_MSG->sync(ACE_TEXT("echod"));
    ACE_LOG_MSG->set_flags(ACE_Log_Msg::VERBOSE);
    const int n = argc - 1;
    ACE_Auto_Array_Ptr<Echo_Acceptor> acceptors;
    {
        Echo_Acceptor* p;
        ACE_NEW_RETURN(p, Echo_Acceptor[n], 1);
        acceptors.reset(p);
    }

    for (int i = 0; i < n; ++i) {
        bool ipv6_only = false;
        ACE_Auto_Array_Ptr<ACE_TCHAR> addr(ACE::strnew(argv[i + 1]));
        if (ACE_TCHAR* p = ACE_OS::strchr(addr.get(), '/')) {
            *p++ = 0;
            ipv6_only = !ACE_OS::strncmp(p, IPV6_ONLY_OPT, IPV6_ONLY_OPT_LEN);
        }

        if (acceptors[i].open(ACE_INET_Addr(addr.get()),
                              ACE_Reactor::instance(),
                              0,
                              1,
                              1,
                              ipv6_only,
                              reactor_type) < 0) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("Unable to lisent '%s', %m\n"),
                       addr.get()));
        } else {
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("Listening %s\n"), addr.get()));
        }
    }

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}
// vim: set ts=4 sw=4 sts=4 et:
