#include <ace/Acceptor.h>
#include <ace/OS_main.h>
#include <ace/Signal.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/SOCK_Stream.h>
#include <ace/Svc_Handler.h>
#include <ace/Reactor.h>
#include <ace/Auto_Ptr.h>

#define IPV6_ONLY_OPT ACE_TEXT("ipv6only")
#define IPV6_ONLY_OPT_LEN (sizeof(IPV6_ONLY_OPT) / sizeof(ACE_TCHAR) - 1)

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
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("Connected by %s\n"), buf));
        }

        return 0;
    }

    virtual int handle_input(ACE_HANDLE)
    {
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
             int ipv6_only)
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

        return rc;
    }
};

int ACE_TMAIN(int argc, ACE_TCHAR* argv[])
{
    if (argc < 2) {
        ACE_OS::fprintf(stderr,
                        ACE_TEXT("%s <address:port> [<address:port> ...]\n"),
                        *argv);
        return 1;
    }

    ACE_LOG_MSG->sync (ACE_TEXT("echod"));
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
                              ipv6_only) < 0) {
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
