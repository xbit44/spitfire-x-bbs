    for(const auto dimensions:std::vector<app::MessageEditorDimensions>{{true,80,24},{false,80,24},{true,40,20},{false,40,20},{true,20,10},{false,20,10},{true,132,50}}){
        for(const bool editable:{false,true})for(const unsigned variant:{0U,1U,2U,3U}){
            app::FileDetailView detail{"sfame-2026.zip","Main / 1 General Files","2026-10-29","xbit","",252679,0,true,editable};
            if(variant==1)detail.description="Short only @SYSOP@ |12 remains inert";
            if(variant==2)detail.description="======== SFAME v1.5 =========\nSFAME v1.50 2026 Preservation\n"+app::qwk_cp437_to_utf8("\xb0\xb1\xb2\xdb")+"\n  Exact spaces  \n";
            if(variant==3){
                detail.filename=std::string(120,'f')+".zip";detail.area=std::string(200,'a');detail.uploader.clear();
                detail.downloads=123456789;detail.downloadable=false;
                for(unsigned row=0;row<90;++row)detail.description+="DIZ row "+std::to_string(row)+" artwork @FILEAREA@ |12\n";
            }
            const auto program=app::prepare_file_information(detail,dimensions);
            check(program&&app::valid_terminal_display_program(*program),"detail program valid at actual bounded dimensions");
            std::string inert,wire;std::size_t column{};unsigned clears{},pages{};bool bright{};
            for(const auto& op:program->operations){
                if(op.kind==app::TerminalDisplayOperationKind::cp437_text){
                    if(bright)check(op.bytes.size()==1&&std::string("TRVEQ").find(op.bytes[0])!=std::string::npos,"only command hotkey is white");
                    check(op.bytes.find('\x1b')==std::string::npos,"description and fields cannot execute controls");
                    column+=op.bytes.size();check(column<dimensions.columns,"every content/footer row leaves right margin");inert+=op.bytes;wire+=op.bytes;
                }else if(op.kind==app::TerminalDisplayOperationKind::logical_newline){inert+='\n';wire+="\r\n";column=0;}
                else if(op.kind==app::TerminalDisplayOperationKind::ansi_sequence){check(dimensions.ansi,"plain has no generated ANSI");bright=op.bytes=="\x1b[1;37;40m";wire+=op.bytes;}
                else if(op.kind==app::TerminalDisplayOperationKind::clear_screen){check(dimensions.ansi,"plain has no ANSI clear");++clears;column=0;wire+="\x1b[2J\x1b[H";}
                else if(op.kind==app::TerminalDisplayOperationKind::paging_enabled){++pages;check(column==0,"continuation begins at left margin");wire+="<EXISTING FORCED CONTINUATION>\r\n";}
                else check(false,"detail uses only measured inert presentation operations");
            }
            check(column+2<dimensions.columns,"footer reserves one input space plus key echo");
            check(!dimensions.ansi || clears==pages+1,"each continuation redraws a clean screen");
            check(variant!=3 || pages>0,"long metadata and DIZ require real continuation points");
            check(inert.find("Uploaded:")==inert.npos&&inert.find("Published:")==inert.npos&&inert.find("Description:")==inert.npos,"no duplicate description or timestamp labels");
            if(dimensions.columns>=80){
                auto joined=inert;joined.erase(std::remove(joined.begin(),joined.end(),'\n'),joined.end());
                check(joined.find("Size: 246.8K (252,679 bytes)")!=joined.npos,"readable and exact size survives measured metadata wrapping");
                check(inert.find("Added:    2026-10-29")!=inert.npos,"Added label uses provided published date");
                check((inert.find("[E] Edit Description")!=inert.npos)==editable,"Edit exactly follows existing permission");
                if(variant==2)check(inert.find(detail.description.substr(0,57))!=inert.npos,"DIZ lines retained once without short description preface");
                if(variant==3)for(unsigned row=0;row<90;++row)check(inert.find("DIZ row "+std::to_string(row)+" artwork")!=inert.npos,"all long-description rows survive paging");
            }
            if(!captures.empty()&&variant==2){std::ofstream out(captures/("detail-"+std::to_string(dimensions.columns)+"x"+std::to_string(dimensions.rows)+(editable?"-edit":"")+(dimensions.ansi?".ans":".txt")),std::ios::binary);out<<wire;check(static_cast<bool>(out),"detail transcript retained, not native PNG evidence");}
        }
    }
