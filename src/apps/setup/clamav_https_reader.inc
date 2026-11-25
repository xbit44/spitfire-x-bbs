// Included in the downloader's private namespace. Same pinned curl/OpenSSL/CA
// as BotGate, but one bounded pull buffer, never a whole-package string.
struct CurlEasyClose {void operator()(CURL* p)const noexcept{if(p)curl_easy_cleanup(p);}};
struct CurlMultiClose {void operator()(CURLM* p)const noexcept{if(p)curl_multi_cleanup(p);}};
class OfficialReader final:public ClamAvDownloadReader {
public:
    OfficialReader(const ClamAvDownloadPump& pump,const char* url,const std::string* fixture_ca,long deadline,bool fixture)
        :pump_(pump),deadline_(deadline),fixture_(fixture){
        tick();
        static const bool initialized=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
        ins::require(initialized&&deadline>0&&deadline<=600000,"clamav_https_initialization");
        easy_.reset(curl_easy_init());multi_.reset(curl_multi_init());
        ins::require(easy_&&multi_,"clamav_https_allocation");
        curl_blob ca{const_cast<unsigned char*>(sfx_botgate_ca),sizeof(sfx_botgate_ca),CURL_BLOB_COPY};
        if(fixture_ca){ca.data=const_cast<char*>(fixture_ca->data());ca.len=fixture_ca->size();}
        const auto set=[&](CURLoption key,auto value){return curl_easy_setopt(easy_.get(),key,value)==CURLE_OK;};
        // Build disables proxies, auth/cookies, native/default CA fallback and
        // environment-selected TLS key logging. Bounded HTTPS-only redirects
        // retain original Setup's three-hop limit, with a narrower host list.
        ins::require(set(CURLOPT_URL,url)&&set(CURLOPT_PROTOCOLS_STR,"https")&&
            set(CURLOPT_FOLLOWLOCATION,1L)&&set(CURLOPT_MAXREDIRS,3L)&&set(CURLOPT_REDIR_PROTOCOLS_STR,"https")&&
            set(CURLOPT_SSL_VERIFYPEER,1L)&&set(CURLOPT_SSL_VERIFYHOST,2L)&&
            set(CURLOPT_SSLVERSION,long(CURL_SSLVERSION_TLSv1_2))&&set(CURLOPT_SSL_OPTIONS,0L)&&
            set(CURLOPT_CAINFO,static_cast<const char*>(nullptr))&&set(CURLOPT_CAPATH,static_cast<const char*>(nullptr))&&
            set(CURLOPT_CAINFO_BLOB,&ca)&&set(CURLOPT_SSL_SESSIONID_CACHE,0L)&&
            set(CURLOPT_CONNECTTIMEOUT_MS,15000L)&&set(CURLOPT_TIMEOUT_MS,deadline)&&
            set(CURLOPT_LOW_SPEED_LIMIT,1L)&&set(CURLOPT_LOW_SPEED_TIME,5L)&&
            set(CURLOPT_MAXFILESIZE_LARGE,curl_off_t(kClamAvDownloadBytes))&&
            set(CURLOPT_NOSIGNAL,1L)&&set(CURLOPT_NOPROGRESS,0L)&&
            set(CURLOPT_XFERINFOFUNCTION,&OfficialReader::progress)&&set(CURLOPT_XFERINFODATA,this)&&
            set(CURLOPT_WRITEFUNCTION,&OfficialReader::write)&&set(CURLOPT_WRITEDATA,this)&&
            set(CURLOPT_HEADERFUNCTION,&OfficialReader::header)&&set(CURLOPT_HEADERDATA,this)&&
            set(CURLOPT_HTTP_CONTENT_DECODING,0L)&&set(CURLOPT_HTTP_VERSION,long(CURL_HTTP_VERSION_1_1))&&
            set(CURLOPT_USERAGENT,"Spitfire-X/" SFX_PRODUCT_VERSION_SHORT_STRING " ClamAV-Setup"),"clamav_https_options");
        ins::require(curl_multi_add_handle(multi_.get(),easy_.get())==CURLM_OK,"clamav_https_attach");attached_=true;
        try{
            fill();
            curl_off_t length=-1;
            ins::require(curl_easy_getinfo(easy_.get(),CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length)==CURLE_OK,"clamav_https_content_length");
            if(length>=0)length_=static_cast<std::uint64_t>(length);
        }catch(...){detach();throw;}
    }
    ~OfficialReader(){detach();}
    std::optional<std::uint64_t> content_length()const override{return length_;}
    std::size_t read(std::uint8_t* out,std::size_t maximum)override{
        ins::require(out&&maximum,"clamav_https_read_buffer");tick();
        if(at_==used_){at_=used_=0;fill();}
        const auto n=std::min(maximum,used_-at_);std::memcpy(out,buffer_.data()+at_,n);at_+=n;return n;
    }
private:
    void detach()noexcept{if(attached_){(void)curl_multi_remove_handle(multi_.get(),easy_.get());attached_=false;}}
    void tick(){
        if(pump_&&!pump_())throw ClamAvDownloadCancelled();
        if(GetTickCount64()-started_>=static_cast<ULONGLONG>(deadline_))failure("timed out");
    }
    [[noreturn]]void failure(const std::string& phase)const{
        throw std::runtime_error("Portable ClamAV download failed ("+phase+"). Your installation was not changed.");
    }
    static int progress(void* p,curl_off_t,curl_off_t,curl_off_t,curl_off_t)noexcept{
        auto& self=*static_cast<OfficialReader*>(p);return GetTickCount64()-self.started_>=static_cast<ULONGLONG>(self.deadline_)?1:0;
    }
    bool redirect_allowed(const char* data,std::size_t length)noexcept{
        try{
            std::string value(data,length);while(!value.empty()&&(value.back()=='\r'||value.back()=='\n'||value.back()==' '||value.back()=='\t'))value.pop_back();
            if(value.empty()||value.size()>8192)return false;
            char* current{};if(curl_easy_getinfo(easy_.get(),CURLINFO_EFFECTIVE_URL,&current)!=CURLE_OK||!current)return false;
            struct UrlClose{void operator()(CURLU* p)const noexcept{if(p)curl_url_cleanup(p);}};
            struct TextClose{void operator()(char* p)const noexcept{curl_free(p);}};
            std::unique_ptr<CURLU,UrlClose> url(curl_url());
            if(!url||curl_url_set(url.get(),CURLUPART_URL,current,CURLU_DISALLOW_USER)!=CURLUE_OK||
                curl_url_set(url.get(),CURLUPART_URL,value.c_str(),CURLU_DISALLOW_USER)!=CURLUE_OK)return false;
            char *s{},*h{},*p{};const auto sc=curl_url_get(url.get(),CURLUPART_SCHEME,&s,0),hc=curl_url_get(url.get(),CURLUPART_HOST,&h,0),pc=curl_url_get(url.get(),CURLUPART_PORT,&p,0);
            std::unique_ptr<char,TextClose> scheme(s),host(h),port(p);
            if(sc!=CURLUE_OK||hc!=CURLUE_OK||_stricmp(s,"https")!=0)return false;
            if(fixture_)return (_stricmp(h,"127.0.0.1")==0||_stricmp(h,"localhost")==0)&&pc==CURLUE_OK&&(std::strcmp(p,"26060")==0||std::strcmp(p,"26061")==0);
            return (_stricmp(h,"www.clamav.net")==0||_stricmp(h,"clamav-site.s3.amazonaws.com")==0)&&
                (pc==CURLUE_NO_PORT||(pc==CURLUE_OK&&std::strcmp(p,"443")==0));
        }catch(...){return false;}
    }
    static size_t header(char* data,size_t size,size_t count,void* p)noexcept{
        auto& self=*static_cast<OfficialReader*>(p);
        if(size&&count>65536/size){self.header_limit_=true;return 0;}const auto n=size*count;
        if(n>65536-self.header_bytes_){self.header_limit_=true;return 0;}self.header_bytes_+=n;
        if(n>=9&&_strnicmp(data,"Location:",9)==0){
            long status{};(void)curl_easy_getinfo(self.easy_.get(),CURLINFO_RESPONSE_CODE,&status);
            auto at=std::size_t{9};while(at<n&&(data[at]==' '||data[at]=='\t'))++at;
            if(status>=300&&status<400&&!self.redirect_allowed(data+at,n-at)){self.redirect_refused_=true;return 0;}
        }
        return n;
    }
    static size_t write(char* data,size_t size,size_t count,void* p)noexcept{
        auto& self=*static_cast<OfficialReader*>(p);long status{};
        if(curl_easy_getinfo(self.easy_.get(),CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK){self.bad_status_=0;return 0;}
        if(status>=300&&status<400){
            if(size&&count>65536/size){self.body_limit_=true;return 0;}const auto n=size*count;
            if(n>65536-self.redirect_bytes_){self.body_limit_=true;return 0;}self.redirect_bytes_+=n;return n;
        }
        if(status!=200){self.bad_status_=status;return 0;}
        if(size&&count>self.buffer_.size()/size){self.body_limit_=true;return 0;}const auto n=size*count;
        if(n>kClamAvDownloadBytes-self.received_){self.body_limit_=true;return 0;}
        if(self.used_){self.paused_=true;return CURL_WRITEFUNC_PAUSE;}
        std::memcpy(self.buffer_.data(),data,n);self.used_=n;self.received_+=n;return n;
    }
    void result(){
        if(redirect_refused_)failure("HTTPS redirect refused");
        if(bad_status_>=0)failure("HTTP "+std::to_string(bad_status_));
        if(header_limit_||body_limit_)failure("HTTPS size limit");
        if(!done_)return;
        if(code_!=CURLE_OK){
            long verify{};(void)curl_easy_getinfo(easy_.get(),CURLINFO_SSL_VERIFYRESULT,&verify);
            if(code_==CURLE_PEER_FAILED_VERIFICATION)failure("TLS certificate validation; curl "+std::to_string(code_)+"; verify "+std::to_string(verify));
            if(code_==CURLE_OPERATION_TIMEDOUT||code_==CURLE_ABORTED_BY_CALLBACK)failure("timed out");
            failure("HTTPS transport; curl "+std::to_string(code_));
        }
        long status{};ins::require(curl_easy_getinfo(easy_.get(),CURLINFO_RESPONSE_CODE,&status)==CURLE_OK,"clamav_https_status");
        if(status!=200)failure("HTTP "+std::to_string(status));
    }
    void fill(){
        while(!used_&&!done_){
            tick();
            if(paused_){paused_=false;ins::require(curl_easy_pause(easy_.get(),CURLPAUSE_CONT)==CURLE_OK,"clamav_https_resume");}
            int running{};ins::require(curl_multi_perform(multi_.get(),&running)==CURLM_OK,"clamav_https_poll");
            int pending{};while(auto* message=curl_multi_info_read(multi_.get(),&pending))if(message->msg==CURLMSG_DONE){done_=true;code_=message->data.result;}
            result();tick();
            if(!used_&&!done_){int sockets{};ins::require(curl_multi_wait(multi_.get(),nullptr,0,100,&sockets)==CURLM_OK,"clamav_https_wait");if(!sockets)Sleep(10);}
        }
        result();
    }
    ClamAvDownloadPump pump_;long deadline_;ULONGLONG started_{GetTickCount64()};
    std::unique_ptr<CURL,CurlEasyClose> easy_;std::unique_ptr<CURLM,CurlMultiClose> multi_;
    std::array<std::uint8_t,65536> buffer_{};std::size_t at_{},used_{},header_bytes_{},redirect_bytes_{};
    std::uint64_t received_{};std::optional<std::uint64_t> length_;
    bool fixture_{},attached_{},paused_{},done_{},header_limit_{},body_limit_{},redirect_refused_{};long bad_status_{-1};CURLcode code_{CURLE_OK};
};
