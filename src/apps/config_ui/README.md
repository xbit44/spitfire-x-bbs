# Spitfire-X BBS — Configuration Application

`SFXConfig.exe` is the native persistent-settings and backup-administration
application. It edits the implemented System, Nodes, Servers, External Client,
Backup & Restore and Scheduler settings while keeping `SFXRuntime.exe` as the
single owner of live BBS state.

When the matching Control Panel and Runtime are active, SFXConfig uses the
installation-scoped authenticated relay owned by that Control Panel. When they
are stopped, it uses the shared installation configuration lock and the same
revisioned configuration repository as Runtime. It does not elevate, search
`PATH`, start a second Runtime, or replace Setup/Repair.
