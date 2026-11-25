    struct LocalReservation {
        LocalReservation(application::NodeLease&& owned,FileDescriptor&& socket,std::uint64_t deadline)
            :lease(std::move(owned)),listener(std::move(socket)),expires(deadline){}
        application::NodeLease lease;
        FileDescriptor listener;
        std::uint64_t expires{};
    };
    bool release_local(std::size_t index) {
        auto reservation=std::move(local_reservations[index]);if(!reservation)return false;
        reservation->listener=FileDescriptor{};
        const bool clean=reservation->lease.release_after_cleanup();
        if(!clean){cleanup_failed=true;diagnostic="local_reservation_cleanup_failed";}
        changed.notify_all();return clean;
    }
    bool local_ready(std::uint16_t node) const {
        return node>0&&node<=configuration.logical_nodes&&started&&!stopping&&!retired&&!draining&&!event_held&&!cleanup_failed&&
            !workers[node-1]&&!local_reservations[node-1];
    }
    std::optional<LinuxConnectionAdmission> local_admission(std::uint16_t node){
        {std::lock_guard<std::mutex> lock(mutex);if(!local_ready(node))return {}; ++local_admissions;}
        LinuxConnectionAdmission result;
        try{result=ports.admit("127.0.0.1",false,stopping);}catch(...){result={};}
        // Shutdown sets cancellation before waiting for this count. Admission
        // never holds the control gate while DNS/policy work can block.
        {std::lock_guard<std::mutex> lock(mutex);--local_admissions;changed.notify_all();}
        return result;
    }
    std::shared_ptr<Worker> local_worker(FileDescriptor connection,application::NodeLease lease) {
        auto assignment=std::make_shared<application::CallerAssignment>(std::move(lease),pool,ports.clock,false,"127.0.0.1");
        const auto index=assignment->identity().node-1;
        auto worker=std::make_shared<Worker>(std::move(connection),false,assignment);workers[index]=worker;
        try{worker->thread=std::thread([this,worker]{finish_worker(worker);});}
        catch(...){worker->socket.close();(void)assignment->retire(true);workers[index].reset();throw;}
        return worker;
    }
    application::AlphaLocalSessionStartResult reserve_local_node(std::uint16_t node) {
        using Code=application::LocalControlResponseCode;
        const auto admission=local_admission(node);
        std::lock_guard<std::mutex> lock(mutex);
        if(!admission||!admission->allowed||!local_ready(node))return {{Code::invalid_request,"local_node_admission_refused"},{},0,{},0};
        auto allocated=pool.allocate_exact(node,"127.0.0.1",admission->source_limit_exempt);
        if(!allocated.lease)return {{Code::invalid_request,"node_or_local_source_unavailable"},{},0,{},0};
        try{
            auto endpoint=bind_endpoint("127.0.0.1",0,false);const auto port=endpoint.port;const auto identity=allocated.lease->identity();
            local_reservations[node-1]=std::make_unique<LocalReservation>(std::move(*allocated.lease),std::move(endpoint.socket),
                ports.clock.monotonic_millis()+application::kAlphaLocalSessionTimeoutMilliseconds);
            return {{Code::ok,"local_node_reserved"},"127.0.0.1",port,identity,application::kAlphaLocalSessionTimeoutMilliseconds};
        }catch(...){if(allocated.lease&&allocated.lease->identity().valid())(void)allocated.lease->release_after_cleanup();
            return {{Code::internal_failure,"local_node_reservation_failed"},{},0,{},0};}
    }
    LinuxBrowserNodeStart open_browser_node(std::uint16_t node) {
        using Code=application::LocalControlResponseCode;
        const auto admission=local_admission(node);
        std::lock_guard<std::mutex> lock(mutex);
        if(!admission||!admission->allowed||!local_ready(node))return {{Code::invalid_request,"local_node_admission_refused"},{},FileDescriptor{}};
        auto allocated=pool.allocate_exact(node,"127.0.0.1",admission->source_limit_exempt);
        if(!allocated.lease)return {{Code::invalid_request,"node_or_local_source_unavailable"},{},FileDescriptor{}};
        int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)!=0){(void)allocated.lease->release_after_cleanup();return {{Code::internal_failure,"browser_transport_failed"},{},FileDescriptor{}};}
        FileDescriptor service(pair[0]),client(pair[1]);const auto identity=allocated.lease->identity();
        try{(void)local_worker(std::move(service),std::move(*allocated.lease));return {{Code::ok,"browser_node_connected"},identity,std::move(client)};}
        catch(...){cleanup_failed=true;diagnostic="browser_node_start_failed";return {{Code::internal_failure,diagnostic},{},FileDescriptor{}};}
    }
    void accept_local() {
        std::lock_guard<std::mutex> lock(mutex);
        for(std::size_t index=0;index<local_reservations.size();++index){
            auto& reservation=local_reservations[index];if(!reservation)continue;
            if(stopping||draining||event_held||cleanup_failed||ports.clock.monotonic_millis()>=reservation->expires){(void)release_local(index);continue;}
            sockaddr_in peer{};socklen_t length=sizeof(peer);
            FileDescriptor accepted(accept4(reservation->listener.get(),reinterpret_cast<sockaddr*>(&peer),&length,SOCK_CLOEXEC));
            if(!accepted){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)(void)release_local(index);continue;}
            if(peer.sin_family!=AF_INET||peer.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)){accepted=FileDescriptor{};(void)release_local(index);continue;}
            // First accept consumes this exact lease. It cannot allocate a
            // different node or race public ingress into the reserved slot.
            reservation->listener=FileDescriptor{};
            auto lease=std::move(reservation->lease);reservation.reset();
            (void)local_worker(std::move(accepted),std::move(lease));
        }
    }
    bool cancel_local_node(application::NodeAssignmentIdentity identity){
        std::lock_guard<std::mutex> lock(mutex);
        if(!identity.valid()||identity.node>local_reservations.size())return false;
        const auto& reservation=local_reservations[identity.node-1];
        return reservation&&reservation->lease.identity()==identity?release_local(identity.node-1):false;
    }
