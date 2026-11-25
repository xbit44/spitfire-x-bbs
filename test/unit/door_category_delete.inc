namespace {
void category_delete_model_checks(){
    app::DoorCatalog c;c.menus.insert(c.menus.end(),{{1,"Delete"},{2,"Child"},{3,"Keep"},{4,"Unplaced"}});
    for(unsigned i=1;i<=4;++i){auto d=door(static_cast<std::uint16_t>(i));d.rlogin.protected_credential={1,2,3};c.doors.push_back(d);}
    c.entries={{0,7,app::DoorEntryKind::submenu,1,"Delete",""},{1,5,app::DoorEntryKind::submenu,2,"Child",""},
        {1,20,app::DoorEntryKind::door,1,"One",""},{2,22,app::DoorEntryKind::door,2,"Two",""},
        {0,31,app::DoorEntryKind::submenu,3,"Keep",""},{3,9,app::DoorEntryKind::door,1,"Shared Door",""},
        {3,10,app::DoorEntryKind::submenu,2,"Shared child",""},{3,50,app::DoorEntryKind::door,3,"Unrelated",""}};
    const auto bytes=app::encode_door_catalog(c);check(app::validate_door_catalog(c).valid(),"recursive delete fixture is valid");
    app::DoorConfigurationChange change;change.operation.bytes[0]=1;change.expected_revision=c.revision;change.action=app::DoorConfigurationAction::remove_menu;change.menu.id=1;
    const auto next=app::changed_door_catalog(c,change);check(next&&next->menus.size()==3&&next->doors.size()==2,"delete category, descendants and their Door definitions");
    check(!app::find_door_menu(*next,1)&&!app::find_door_menu(*next,2)&&!app::find_door(*next,1)&&!app::find_door(*next,2),"shared descendants and Doors are really deleted");
    check(next->entries.size()==2&&next->entries[0].number==31&&next->entries[1].number==50,"all appearances removed; unrelated numbers retained");
    check(app::find_door_menu(*next,4)&&app::find_door(*next,4)&&app::encode_door_catalog(c)==bytes,"unrelated unplaced configs and input snapshot preserved");
    change.menu.id=0;check(!app::changed_door_catalog(c,change),"fixed root deletion refused");change.menu.id=999;check(!app::changed_door_catalog(c,change),"missing category refused");
    change.menu.id=1;++change.expected_revision;check(!app::changed_door_catalog(c,change),"stale confirmation refused");
    check(app::without_door_category(c,4)->menus.size()==4,"empty unplaced category deleted");
    auto unplaced=c;unplaced.entries.erase(unplaced.entries.begin());check(app::without_door_category(unplaced,1)->doors.size()==2,"unplaced populated category deletes contents too");
}
}
