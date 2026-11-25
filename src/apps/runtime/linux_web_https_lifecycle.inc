    std::thread watcher([&]{
        std::unique_ptr<httplib::SSLServer> secure;std::thread secure_thread;std::atomic<bool> secure_finished{false};std::string active_key;
        const auto retire=[&]{if(secure)secure->stop();if(secure_thread.joinable())secure_thread.join();secure.reset();remote_sessions.revoke_all();std::lock_guard<std::mutex> lock(web_access_mutex);lan_active=false;};
        unsigned poll{};
        while(!interrupted.load(std::memory_order_relaxed)){
            runtime.tick_controls();
            if(++poll>=10){poll=0;
                try{
                    rt::web::WebAccessSettings settings;rt::web::WebTlsIdentity tls;std::string wanted;
                    {std::lock_guard<std::mutex> lock(web_access_mutex);settings=rt::web::read_web_access(root);if(settings.enabled){tls=rt::web::load_web_tls(root,settings);wanted=rt::web::web_access_json(settings).dump()+tls.fingerprint;}}
                    if(wanted!=active_key||(secure&&secure_finished.load())){
                        retire();active_key=wanted;
                        if(settings.enabled){
                            secure=std::make_unique<httplib::SSLServer>([&](httplib::tls::ctx_t raw){
                                auto* context=static_cast<SSL_CTX*>(raw);if(SSL_CTX_set_min_proto_version(context,TLS1_2_VERSION)!=1)return false;
                                SSL_CTX_set_options(context,SSL_OP_NO_COMPRESSION);
                                if(SSL_CTX_use_certificate(context,SSL_CTX_get0_certificate(tls.context.get()))!=1||SSL_CTX_use_PrivateKey(context,SSL_CTX_get0_privatekey(tls.context.get()))!=1||SSL_CTX_check_private_key(context)!=1)return false;
                                STACK_OF(X509)* chain{};SSL_CTX_get_extra_chain_certs(tls.context.get(),&chain);
                                for(int i=0;chain&&i<sk_X509_num(chain);++i){auto* cert=sk_X509_value(chain,i);if(X509_up_ref(cert)!=1)return false;if(SSL_CTX_add_extra_chain_cert(context,cert)!=1){X509_free(cert);return false;}}
                                return true;
                            });
                            if(!secure->is_valid()||!secure->bind_to_port(settings.address,settings.port))throw std::runtime_error("HTTPS could not bind. Check the address, port conflict and any required low-port permission.");
                            const auto name=settings.server_name.empty()?settings.address:settings.server_name;auto* selected=secure.get();secure_finished=false;
                            secure_thread=std::thread([&,selected,name,selected_port=settings.port]{try{(void)serve_endpoint(*selected,true,selected_port,name);}catch(const std::exception&){}secure_finished=true;});
                            std::lock_guard<std::mutex> lock(web_access_mutex);lan_active=true;lan_fingerprint=tls.fingerprint;
                            const auto authority=name.find(':')==std::string::npos?name:"["+name+"]";
                            lan_detail="Open https://"+authority+(settings.port==443?"":":"+std::to_string(settings.port))+"/ from an approved computer. Verify the certificate fingerprint before signing in.";
                        }else{std::lock_guard<std::mutex> lock(web_access_mutex);lan_detail="LAN access is off.";lan_fingerprint.clear();}
                    }
                }catch(const std::exception& error){retire();active_key.clear();std::lock_guard<std::mutex> lock(web_access_mutex);lan_detail=std::string("LAN controls are unavailable. Local access remains available. ")+rt::web::web_access_error(error.what());lan_fingerprint.clear();}
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        retire();server.stop();
    });
