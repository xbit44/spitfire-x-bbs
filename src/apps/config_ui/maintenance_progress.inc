// Responsive native modal progress. The owner can request cancellation; closing
// the dialog never destroys a worker/context while an operation is still live.
// Kept out of Runtime: this is stopped-installation administrative presentation.
struct MaintenanceProgress final {
    std::function<void(std::function<bool()>)> work;
    std::atomic<bool> cancelled{false},done{false};
    std::exception_ptr failure;
    std::thread worker;
    ~MaintenanceProgress(){if(worker.joinable())worker.join();}
    static HRESULT CALLBACK callback(HWND window,UINT event,WPARAM button,LPARAM,LONG_PTR context){
        auto& value=*reinterpret_cast<MaintenanceProgress*>(context);
        if(event==TDN_CREATED){
            SendMessageW(window,TDM_ENABLE_BUTTON,101,FALSE);
            SendMessageW(window,TDM_SET_MARQUEE_PROGRESS_BAR,TRUE,0);
            SendMessageW(window,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,30);
            try{value.worker=std::thread([&value]{try{value.work([&value]{return value.cancelled.load();});}catch(...){value.failure=std::current_exception();}value.done.store(true);});}
            catch(...){value.failure=std::current_exception();value.done.store(true);}
        }else if(event==TDN_BUTTON_CLICKED && !value.done.load()){
            if(button==IDCANCEL){value.cancelled.store(true);SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_CONTENT,reinterpret_cast<LPARAM>(L"Cancellation requested. Waiting for the current transaction to finish or roll back; do not force-close SFX."));}
            return S_FALSE;
        }else if(event==TDN_TIMER && value.done.load()){
            SendMessageW(window,TDM_ENABLE_BUTTON,101,TRUE);SendMessageW(window,TDM_CLICK_BUTTON,101,0);
        }
        return S_OK;
    }
};
bool maintenance_progress(State& state,const wchar_t* title,std::function<void(std::function<bool()>)> work){
    if(state.account_action_active)return false;
    struct Active {bool& value;explicit Active(bool& v):value(v){value=true;}~Active(){value=false;}} active(state.account_action_active);
    MaintenanceProgress progress;progress.work=std::move(work);
    const TASKDIALOG_BUTTON close{101,L"Close"};TASKDIALOGCONFIG dialog{};dialog.cbSize=sizeof(dialog);dialog.hwndParent=state.window;
    dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW|TDF_CALLBACK_TIMER|TDF_SHOW_MARQUEE_PROGRESS_BAR;
    dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;dialog.pszWindowTitle=kWindowTitle;dialog.pszMainInstruction=title;
    dialog.pszContent=L"Exclusive stopped-system operation. Runtime cannot start while this operation owns the installation. Cancel requests a safe stop; committed work is reported honestly.";
    dialog.cButtons=1;dialog.pButtons=&close;dialog.nDefaultButton=IDCANCEL;dialog.pfCallback=MaintenanceProgress::callback;dialog.lpCallbackData=reinterpret_cast<LONG_PTR>(&progress);
    const auto shown=TaskDialogIndirect(&dialog,nullptr,nullptr,nullptr);
    if(progress.worker.joinable())progress.worker.join();
    if(FAILED(shown) || progress.failure){set_status(state,L"Failed — maintenance did not report a confirmed success. Inspect the current state and retained recovery backup; no automatic retry or repair.");return false;}
    return progress.done.load();
}
