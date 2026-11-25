// Independent decoded-wire inputs through the actual native host/helper/NTFS.
// Included inside the existing bounded fixture; not GUI/authentication proof.
    const auto xy_frame=[](Bytes& wire,std::string body,unsigned number){
        check(body.size()<=128,"bounded native peer frame");body.resize(128,'\x1a');
        wire.push_back(1);wire.push_back(static_cast<std::uint8_t>(number));wire.push_back(static_cast<std::uint8_t>(255-(number&255)));
        std::uint16_t value{};for(unsigned char byte:body){wire.push_back(byte);value=crc(value,byte);}
        wire.push_back(static_cast<std::uint8_t>(value>>8));wire.push_back(static_cast<std::uint8_t>(value));
    };
    for(auto protocol:{app::TransferProtocolPreference::xmodem,app::TransferProtocolPreference::ymodem,app::TransferProtocolPreference::ymodem_g})for(bool sending:{false,true}){
        const bool streaming=protocol==app::TransferProtocolPreference::ymodem_g;
        const bool xmodem=protocol==app::TransferProtocolPreference::xmodem;
        auto lease=fresh(256);const std::string body=std::string("exact native binary")+std::string("\0tail",5);
        if(sending)lease.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());
        if(sending)(void)lease.identity(); // Seal the ordinary read-only send capability, as in the existing ZMODEM cases.
        Peer peer;std::string metadata="native.bin";metadata+='\0';metadata+=std::to_string(body.size());metadata.resize(128,'\0');
        if(xmodem){
            if(!sending){
                xy_frame(peer.c.wire,body,1);peer.gate([&]{return peer.c.emitted.size()>=1;});
                peer.c.wire.push_back(4);peer.gate([&]{return peer.c.emitted.size()>=2;});
                peer.c.wire.push_back(4);peer.gate([&]{return peer.c.emitted.size()>=3;});
            }else{
                peer.c.wire.push_back('C');peer.gate([]{return true;});
                peer.c.wire.push_back(6);peer.gate([&]{return peer.c.emitted.size()>=133;});
                peer.c.wire.push_back(6);peer.gate([&]{return peer.c.emitted.size()>=134;});
            }
        }else if(!sending){
            xy_frame(peer.c.wire,metadata,0);peer.gate([&]{return peer.c.emitted.size()>=1;});
            xy_frame(peer.c.wire,body,1);peer.gate([&]{return peer.c.emitted.size()>=(streaming?2U:3U);});
            peer.c.wire.push_back(4);peer.gate([&]{return peer.c.emitted.size()>=(streaming?2U:4U);});
            if(!streaming){peer.c.wire.push_back(4);peer.gate([&]{return peer.c.emitted.size()>=5;});}
            xy_frame(peer.c.wire,std::string(128,'\0'),0);peer.gate([&]{return peer.c.emitted.size()>=(streaming?4U:7U);});
        }else{
            peer.c.wire.push_back(streaming?'G':'C');peer.gate([]{return true;});
            if(!streaming)peer.c.wire.push_back(6);
            peer.c.wire.push_back(streaming?'G':'C');peer.gate([&]{return peer.c.emitted.size()>=133;});
            if(!streaming){peer.c.wire.push_back(6);peer.gate([&]{return peer.c.emitted.size()>=1162;});}
            peer.c.wire.push_back(6);peer.c.wire.push_back(streaming?'G':'C');peer.gate([&]{return peer.c.emitted.size()>=1163;});
            if(!streaming){peer.c.wire.push_back(6);peer.gate([&]{return peer.c.emitted.size()>=1296;});}
        }
        const auto result=run_host(peer,sending?filewire::Direction::send:filewire::Direction::receive,{item(lease,!sending,"native.bin",256)},protocol);
        const auto expected_size=xmodem&&!sending?128U:body.size();
        check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==1&&result.files[0].completed&&result.files[0].bytes==expected_size,"actual X/Y/Y-G host receipt, partial output writes and exact completion");
        check(lease.identity().bytes==expected_size&&empty_work(),"X/Y/Y-G exact stored payload length and bounded control cleanup");
    }
