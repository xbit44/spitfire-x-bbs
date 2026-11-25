#include "linux_caller_listener.hpp"
#include "alpha_ssh_stream.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/network.hpp"
#include <array>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

namespace sfx::apps::runtime {
namespace {
using platform::linux_host::FileDescriptor;
struct Socket final {
    explicit Socket(FileDescriptor fd):value(std::move(fd)) {}
    ~Socket(){close();}
    void interrupt() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if(value) (void)shutdown(value.get(),SHUT_RDWR);
    }
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if(value) {(void)shutdown(value.get(),SHUT_RDWR);value=FileDescriptor{};}
    }
    int get() const noexcept {return value.get();} // Worker only; interrupt never changes it.
    FileDescriptor value;
    std::mutex mutex;
};
struct Endpoint {
    FileDescriptor socket{};
    std::uint16_t port{};
};
class EndpointStartError final:public std::runtime_error {
public:
    EndpointStartError(std::string_view address,std::uint16_t port,bool ssh,int error)
        :std::runtime_error(std::string(ssh?"ssh":"telnet")+
            (error==EADDRINUSE?"_port_in_use:":error==EACCES?"_port_permission_denied:":"_endpoint_failed:")+
            std::string(address)+":"+std::to_string(port)) {}
};
Endpoint bind_endpoint(std::string_view text,std::uint16_t port,bool ssh) {
    const std::string address(text);
    sockaddr_in native{};native.sin_family=AF_INET;native.sin_port=htons(port);
    if(inet_pton(AF_INET,address.c_str(),&native.sin_addr)!=1)
        throw std::runtime_error("listener_address_invalid");
    FileDescriptor fd(socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0));
    if(!fd) throw std::runtime_error("listener_socket_failed");
    const int reuse=1;
    if(setsockopt(fd.get(),SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))!=0 ||
        (ssh && !AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(fd.get()))))
        throw EndpointStartError(address,port,ssh,0);
    if(bind(fd.get(),reinterpret_cast<const sockaddr*>(&native),sizeof(native))!=0){
        const int error=errno;
        throw EndpointStartError(address,port,ssh,error);
    }
    if(listen(fd.get(),SOMAXCONN)!=0)throw EndpointStartError(address,port,ssh,0);
    socklen_t length=sizeof(native);
    if(getsockname(fd.get(),reinterpret_cast<sockaddr*>(&native),&length)!=0)
        throw std::runtime_error("listener_address_unavailable");
    return {std::move(fd),ntohs(native.sin_port)};
}
}
class LinuxCallerListener::Impl final {
public:
    void observe(application::OperationalKind kind,std::string source,bool ssh,
        application::NodeAssignmentIdentity identity={},std::uint16_t local_port=0)noexcept{
        if(!ports.observe)return;
        try{application::OperationalContext context;context.source_ip=std::move(source);context.transport=ssh?application::OperationalTransport::ssh:application::OperationalTransport::telnet;
            context.node=identity.node;context.generation=identity.generation;context.attempt=identity.generation;context.local_port=local_port;
            ports.observe({kind,{},std::move(context)});}catch(...){}
    }
    struct Worker {
        Worker(FileDescriptor fd,bool protocol,std::shared_ptr<application::CallerAssignment> state)
            :socket(std::move(fd)),ssh(protocol),assignment(std::move(state)) {}
        Socket socket;
        bool ssh;
        std::shared_ptr<application::CallerAssignment> assignment;
        std::thread thread;
        std::atomic<bool> done{false};
        bool clean{},retired{};
    };
#include "linux_local_logon.inc"
    Impl(application::Identifier128 epoch,application::OperationalConfiguration config,
        LinuxCallerListenerPorts callbacks,application::NodeEnablement enabled,
        std::array<std::uint64_t,application::kAlphaNodePoolCapacity> generations)
        :configuration(std::move(config)),ports(std::move(callbacks)),pool(epoch,configuration.logical_nodes,
            configuration.maximum_connections_per_source,std::numeric_limits<std::uint64_t>::max(),generations,
            configuration.source_connection_exceptions) {
        if(configuration.logical_nodes<1 || configuration.logical_nodes>enabled.size())
            throw std::runtime_error("listener_node_count_invalid");
        for(std::uint16_t n=1;n<=configuration.logical_nodes;++n) (void)pool.set_enabled(n,enabled[n-1]);
    }
    ~Impl(){stop();}
    bool start() {
        std::lock_guard<std::mutex> operation(control);
        std::lock_guard<std::mutex> lock(mutex);
        if(started || retired || !ports.admit || !ports.serve) return false;
        try {
            Endpoint telnet,ssh;
            if(configuration.telnet.enabled) telnet=bind_endpoint(configuration.telnet.bind_address,configuration.telnet.port,false);
            if(configuration.ssh.enabled) ssh=bind_endpoint(configuration.ssh.bind_address,configuration.ssh.port,true);
            if(!telnet.socket && !ssh.socket) {diagnostic="listeners_disabled";return false;}
            endpoints[0]=std::make_shared<Endpoint>(std::move(telnet));endpoints[1]=std::make_shared<Endpoint>(std::move(ssh));
            stopping=false;started=true;diagnostic="listening";
            acceptor=std::thread([this]{accept_loop();});
            for(unsigned i=0;i<endpoints.size();++i)if(endpoints[i]&&endpoints[i]->socket)observe(application::OperationalKind::listener_ready,{},i==1,{},endpoints[i]->port);
            return true;
        } catch(const EndpointStartError& error) {
            endpoints={};started=false;diagnostic=error.what();return false;
        } catch(const std::exception&) {
            endpoints={};started=false;diagnostic="listener_start_failed";return false;
        }
    }
    void stop() noexcept {
        std::lock_guard<std::mutex> operation(control);
        {
            std::unique_lock<std::mutex> lock(mutex);
            if(started)for(unsigned i=0;i<endpoints.size();++i)if(endpoints[i]&&endpoints[i]->socket)observe(application::OperationalKind::listener_stopping,{},i==1,{},endpoints[i]->port);
            stopping=true;retired=true;pool.stop();
            for(std::size_t i=0;i<local_reservations.size();++i)if(local_reservations[i])(void)release_local(i);
            for(auto& worker:workers) if(worker) {worker->assignment->cancel();worker->socket.interrupt();}
            changed.wait(lock,[&]{return local_admissions==0;});
        }
        // Poll uses a bounded wakeup. Listener descriptors remain owned until
        // accept has exited, so descriptor reuse cannot redirect the loop.
        if(acceptor.joinable()) acceptor.join();
        for(auto& worker:workers) if(worker && worker->thread.joinable()) worker->thread.join();
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& worker:workers) if(worker && !worker->retired) retire_worker(worker);
        if(started)for(unsigned i=0;i<endpoints.size();++i)if(endpoints[i]&&endpoints[i]->socket)observe(application::OperationalKind::listener_stopped,{},i==1,{},endpoints[i]->port);
        endpoints={};started=false;
        diagnostic=cleanup_failed?"caller_cleanup_failed":"stopped";
    }
    void finish_worker(const std::shared_ptr<Worker>& worker) noexcept {
        bool clean=false;
        try {clean=ports.serve(worker->socket.get(),worker->ssh,worker->assignment);}
        catch(...) {worker->assignment->diagnose(application::CallerDiagnostic::session_failure);}
        worker->socket.close();
        worker->assignment->transport_closed();
        worker->clean=clean && !worker->assignment->cleanup_health->failed.load();
        if(!clean) {
            std::lock_guard<std::mutex> lock(mutex);
            cleanup_failed=true;diagnostic="caller_cleanup_failed";
        }
        worker->done=true;
    }
    void retire_worker(const std::shared_ptr<Worker>& worker) {
        const auto identity=worker->assignment->identity();const auto source=worker->assignment->snapshot().source_ip;
        const auto released=worker->assignment->retire(worker->clean);
        if(!released) {
            cleanup_failed=true;diagnostic="caller_cleanup_failed";
        }
        observe(released?application::OperationalKind::assignment_released:application::OperationalKind::assignment_quarantined,source,worker->ssh,identity);
        worker->retired=true;
        changed.notify_all();
    }
    void reap() {
        // Only the acceptor joins completed workers during operation. Stop
        // joins the acceptor before touching these thread objects.
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& worker:workers) if(worker && worker->done.load() && !worker->retired) {
            if(worker->thread.joinable()) worker->thread.join();
            retire_worker(worker);
            if(worker->clean && !worker->assignment->cleanup_health->failed.load()) worker.reset();
        }
    }
    void accept_one(unsigned index,const std::shared_ptr<Endpoint>& endpoint) {
        sockaddr_in peer{};socklen_t length=sizeof(peer);
        int fd=-1;
        {std::lock_guard<std::mutex> lock(mutex);
            if(stopping||endpoints[index]!=endpoint)return;
            fd=accept4(endpoint->socket.get(),reinterpret_cast<sockaddr*>(&peer),&length,SOCK_CLOEXEC);
        }
        if(fd<0) return;
        FileDescriptor accepted(fd);
        char address[INET_ADDRSTRLEN]{};
        if(peer.sin_family!=AF_INET || !inet_ntop(AF_INET,&peer.sin_addr,address,sizeof(address))) return;
        // No authentication, node assignment or SSH handshake precedes this.
        const auto admitted=ports.admit(address,index==1,stopping);
        if(!admitted.allowed) return;
        std::lock_guard<std::mutex> lock(mutex);
        if(stopping || draining || event_held || cleanup_failed || endpoints[index]!=endpoint) {observe(application::OperationalKind::runtime_refused,address,index==1);return;}
        auto allocation=pool.allocate(address,admitted.source_limit_exempt);
        if(!allocation.lease) {observe(allocation.code==application::NodeAllocationCode::source_limit?application::OperationalKind::source_refused:application::OperationalKind::capacity_refused,address,index==1);return;}
        auto assignment=std::make_shared<application::CallerAssignment>(std::move(*allocation.lease),
            pool,ports.clock,index==1,address);
        const auto slot=assignment->identity().node-1;
        if(workers[slot]) {
            // A previous finished worker still owns the slot's OS resources.
            (void)assignment->retire(true);return;
        }
        // Move the accepted CLOEXEC descriptor into its sole close owner.
        auto worker=std::make_shared<Worker>(std::move(accepted),index==1,assignment);
        workers[slot]=worker;
        try {worker->thread=std::thread([this,worker]{finish_worker(worker);});}
        catch(...) {
            worker->socket.close();(void)assignment->retire(true);workers[slot].reset();
            throw;
        }
    }
    void accept_loop() noexcept {
        try {
            while(!stopping.load()) {
                reap();
                accept_local();
                std::array<std::shared_ptr<Endpoint>,2> current;
                {std::lock_guard<std::mutex> lock(mutex);current=endpoints;}
                std::array<pollfd,2> waiting{{{current[0]?current[0]->socket.get():-1,POLLIN,0},
                    {current[1]?current[1]->socket.get():-1,POLLIN,0}}};
                const int count=poll(waiting.data(),waiting.size(),100);
                if(count<0) {if(errno==EINTR)continue;throw std::runtime_error("listener_poll_failed");}
                for(unsigned i=0;i<waiting.size() && !stopping;++i) {
                    if(waiting[i].revents&(POLLERR|POLLHUP|POLLNVAL)) throw std::runtime_error("listener_poll_failed");
                    if(waiting[i].revents&POLLIN) accept_one(i,current[i]);
                }
            }
        } catch(...) {
            std::lock_guard<std::mutex> lock(mutex);
            stopping=true;diagnostic="listener_accept_failed";
            for(auto& worker:workers) if(worker) {worker->assignment->cancel();worker->socket.interrupt();}
        }
    }
    bool disconnect(application::NodeAssignmentIdentity identity) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!identity.valid() || identity.node>workers.size()) return false;
        const auto& reservation=local_reservations[identity.node-1];
        if(reservation&&reservation->lease.identity()==identity)return release_local(identity.node-1);
        const auto& worker=workers[identity.node-1];
        if(!worker || !(worker->assignment->identity()==identity) || worker->done) return false;
        worker->assignment->cancel();worker->socket.interrupt();return true;
    }
    bool terminate_door(application::NodeAssignmentIdentity identity,std::uint64_t launch) {
        std::shared_ptr<application::CallerAssignment> assignment;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!identity.valid()||identity.node>workers.size()||!launch)return false;
            const auto& worker=workers[identity.node-1];
            if(!worker||worker->done||!(worker->assignment->identity()==identity))return false;
            assignment=worker->assignment;
            if(!assignment->request_door_termination(launch))return false;
        }
        // Retain this assignment, never look up a successor while waiting.
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do{
            if(assignment->snapshot().door_launch_sequence!=launch)return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }while(std::chrono::steady_clock::now()<until);
        return false;
    }
    bool wait_for_retirement(application::NodeAssignmentIdentity identity,std::uint32_t timeout_millis) {
        if(!identity.valid() || timeout_millis>30000) return false;
        std::unique_lock<std::mutex> lock(mutex);
        if(identity.node>configuration.logical_nodes)return false;
        const auto slot=pool.snapshot().slots[identity.node-1];
        if(!(slot.identity.runtime_epoch==identity.runtime_epoch) || slot.identity.generation<identity.generation) return false;
        const auto completed=[&] {
            const auto& worker=workers[identity.node-1];
            const auto& reservation=local_reservations[identity.node-1];
            return cleanup_failed || ((!reservation||!(reservation->lease.identity()==identity))&&
                (!worker || !(worker->assignment->identity()==identity) || worker->retired));
        };
        return changed.wait_for(lock,std::chrono::milliseconds(timeout_millis),completed) && !cleanup_failed;
    }
    bool set_enabled(std::uint16_t node,bool enabled,std::optional<application::NodeAssignmentIdentity> observed={},const std::function<void()>& publish={}) {
        std::lock_guard<std::mutex> lock(mutex);
        if(retired||!node||node>configuration.logical_nodes)return false;
        const auto slot=pool.snapshot().slots[node-1];
        if(observed&&(!(observed->runtime_epoch==slot.identity.runtime_epoch)||observed->node!=node||observed->generation!=slot.identity.generation))return false;
        // Keep the assignment gate throughout publication and cancellation.
        // An admitted successor cannot slip between reviewed identity and Save.
        if(publish)publish();
        if(!pool.set_enabled(node,enabled))return false;
        if(!enabled&&node>0&&node<=local_reservations.size()&&local_reservations[node-1])(void)release_local(node-1);
        if(!enabled && node>0 && node<=workers.size() && workers[node-1]) {
            workers[node-1]->assignment->cancel();workers[node-1]->socket.interrupt();
        }
        return true;
    }
    LinuxCallerListenerStatus status() const {
        std::lock_guard<std::mutex> lock(mutex);
        LinuxCallerListenerStatus result;
        result.listening=started && !stopping;result.draining=draining;result.event_held=event_held;
        result.telnet_port=endpoints[0]?endpoints[0]->port:0;result.ssh_port=endpoints[1]?endpoints[1]->port:0;
        result.diagnostic=diagnostic;result.pool=pool.snapshot();
        for(const auto& worker:workers) if(worker) result.callers.push_back(worker->assignment->snapshot());
        for(const auto& reservation:local_reservations)if(reservation){
            application::AlphaNodeStatus node;const auto id=reservation->lease.identity();node.number=id.node;
            node.assignment_sequence=id.generation;node.screen_owner=id;node.available=node.enabled=true;
            node.state=core::NodeState::connecting;node.connection="Telnet";node.source_ip="127.0.0.1";
            node.activity="Waiting for local client";node.details="One-use local node reservation";result.callers.push_back(std::move(node));}
        return result;
    }
    bool reconfigure_endpoints(const application::OperationalConfiguration& candidate,const std::function<bool()>& publish) {
        std::lock_guard<std::mutex> operation(control);
        try {
            static_assert(std::is_nothrow_move_assignable_v<application::OperationalConfiguration>);
            auto prepared=candidate;
            std::array<std::shared_ptr<Endpoint>,2> next;
            {std::lock_guard<std::mutex> lock(mutex);
                if(!started||stopping||retired||cleanup_failed||!publish||
                    candidate.logical_nodes!=configuration.logical_nodes||
                    candidate.maximum_connections_per_source!=configuration.maximum_connections_per_source||
                    candidate.source_connection_exceptions!=configuration.source_connection_exceptions)return false;
                next=endpoints;
            }
            const bool telnet_changed=candidate.telnet.enabled!=configuration.telnet.enabled||candidate.telnet.bind_address!=configuration.telnet.bind_address||candidate.telnet.port!=configuration.telnet.port;
            const bool ssh_changed=candidate.ssh.enabled!=configuration.ssh.enabled||candidate.ssh.bind_address!=configuration.ssh.bind_address||candidate.ssh.port!=configuration.ssh.port;
            if(telnet_changed)next[0]=std::make_shared<Endpoint>(candidate.telnet.enabled?bind_endpoint(candidate.telnet.bind_address,candidate.telnet.port,false):Endpoint{});
            if(ssh_changed)next[1]=std::make_shared<Endpoint>(candidate.ssh.enabled?bind_endpoint(candidate.ssh.bind_address,candidate.ssh.port,true):Endpoint{});
            std::lock_guard<std::mutex> lock(mutex);
            if(pool.snapshot().active&&((telnet_changed&&ssh_changed)||candidate.hostname_lookup!=configuration.hostname_lookup))return false;
            if(stopping||cleanup_failed||!publish())return false;
            endpoints=std::move(next);configuration=std::move(prepared);return true;
        }catch(const std::exception&){return false;}
    }
    application::OperationalConfiguration configuration;
    LinuxCallerListenerPorts ports;
    application::FourNodePool pool;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::mutex control;
    std::array<std::shared_ptr<Endpoint>,2> endpoints;
    std::array<std::shared_ptr<Worker>,application::kAlphaNodePoolCapacity> workers;
    std::array<std::unique_ptr<LocalReservation>,application::kAlphaNodePoolCapacity> local_reservations;
    std::thread acceptor;
    std::atomic<bool> stopping{false};
    bool started{},retired{},draining{},event_held{},cleanup_failed{};
    unsigned local_admissions{};
    std::string diagnostic{"not_started"};
};
LinuxCallerListener::LinuxCallerListener(application::Identifier128 epoch,
    application::OperationalConfiguration config,LinuxCallerListenerPorts ports,application::NodeEnablement enabled,
    std::array<std::uint64_t,application::kAlphaNodePoolCapacity> generations)
    :impl_(std::make_unique<Impl>(epoch,std::move(config),std::move(ports),enabled,generations)) {}
LinuxCallerListener::~LinuxCallerListener()=default;
bool LinuxCallerListener::start(){return impl_->start();}
void LinuxCallerListener::stop() noexcept {impl_->stop();}
bool LinuxCallerListener::disconnect(application::NodeAssignmentIdentity identity){return impl_->disconnect(identity);}
bool LinuxCallerListener::terminate_door(application::NodeAssignmentIdentity identity,std::uint64_t launch){return impl_->terminate_door(identity,launch);}
application::AlphaLocalSessionStartResult LinuxCallerListener::reserve_local_node(std::uint16_t node){return impl_->reserve_local_node(node);}
bool LinuxCallerListener::cancel_local_node(application::NodeAssignmentIdentity identity){return impl_->cancel_local_node(identity);}
LinuxBrowserNodeStart LinuxCallerListener::open_browser_node(std::uint16_t node){return impl_->open_browser_node(node);}
bool LinuxCallerListener::wait_for_retirement(application::NodeAssignmentIdentity identity,std::uint32_t timeout_millis){return impl_->wait_for_retirement(identity,timeout_millis);}
bool LinuxCallerListener::set_enabled(std::uint16_t node,bool enabled){return impl_->set_enabled(node,enabled);}
bool LinuxCallerListener::set_enabled(std::uint16_t node,bool enabled,std::optional<application::NodeAssignmentIdentity> observed,const std::function<void()>& publish){return impl_->set_enabled(node,enabled,observed,publish);}
bool LinuxCallerListener::reconfigure_endpoints(const application::OperationalConfiguration& candidate,const std::function<bool()>& publish){return impl_->reconfigure_endpoints(candidate,publish);}
bool LinuxCallerListener::set_draining(bool value){
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->retired)return false;
    impl_->draining=value;
    if(value)for(std::size_t i=0;i<impl_->local_reservations.size();++i)if(impl_->local_reservations[i])(void)impl_->release_local(i);
    return true;
}
void LinuxCallerListener::set_external_event_hold(bool held) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (held && !impl_->event_held) {
        for (std::size_t i = 0; i < impl_->local_reservations.size(); ++i)
            if (impl_->local_reservations[i]) (void)impl_->release_local(i);
    }
    impl_->event_held = held;
}
LinuxCallerListenerStatus LinuxCallerListener::status() const{return impl_->status();}
} // namespace sfx::apps::runtime
