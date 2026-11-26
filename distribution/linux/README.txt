      _________________________________________________________________________
         ___________________                             ___________________
            ________________     S P I T F I R E - X     ________________
               _____________   -------------------------  _____________
                  __________    Bulletin Board System    __________
                     _______         for Linux           _______

                        ____      Personal Computers     ____
                           _______________________________
                             ____                    ____
                               _____  1.0.0b3   ____
                                 ___________________



                         Copyright (C) 2026 by xbit ops
                        -------------------------------------
                                 Spitfire-X BBS
                               Sysop Quick Start
                                  MIT License
                                   
                         Linux AMD64 v1.0.0b3 Public Beta



                     ------------------------------------------
                      >>> Release Date   November 24, 2026   <<<  
                     ------------------------------------------





























                                                                         Page 1

        -----------------------------------------------------------------------
        -+ SPITFIRE-X BULLETIN BOARD SYSTEM - SYSOP README +-
        -----------------------------------------------------------------------

             Run SFX as your normal Linux account. This ready-built package
        needs native x86-64 / AMD64 Linux with Python 3.8 or newer, glibc 2.30
        or newer, libstdc++ providing GLIBCXX_3.4.26 and CXXABI_1.3.11, and
        libgcc. No compiler is needed to install it.

             Native SFX testing has used Pop!_OS 22.04 and Mint 22.3 at the
        recorded scopes. See the release notes for this package's actual
        fresh-install, upgrade and runtime checks. Other glibc x86-64
        distributions meeting these requirements are expected to be portable,
        not universally tested or guaranteed. This package is not for ARM,
        32-bit Linux or Alpine/musl.

             The normal setup flow below uses a terminal opened on the Linux
        desktop where SFX will run, followed by that desktop's browser. For
        desktop use, you need a default browser and xdg-open (usually from
        xdg-utils). Use section 6 only when connecting by SSH from another
        computer.

        1.0 - FRESH INSTALL ON THE LINUX DESKTOP
        ----------------------------------------

             For a new board, choose a new, empty folder owned by your normal
        account. Fresh Linux installations use lowercase SFX paths. This
        installer does not convert an older uppercase board. To update an
        existing lowercase board, use section 5 instead; keep its
        current folder and setup.

             Download the single Linux .tar.gz file. Open a terminal on the
        Linux desktop in your download folder, extract it once, then enter its
        versioned folder. For this release:

            tar -xzf spitfire-x-1.0.0b3-linux-x86_64.tar.gz
            cd spitfire-x-1.0.0b3-linux-x86_64
            python3 install.py

             Keep two folders separate. The extracted versioned folder is
        staging: it contains install.py and payload/. Your installed board is
        the destination you select, for example $HOME/sfx. Run the installer
        from staging; it copies the package into that separate destination.
        Do not choose the staging folder or a parent containing that package
        as a fresh destination. A fresh destination must be new or empty.

             The installer asks for the installation folder. Press Enter for
        the shown default (your home folder plus /sfx), or enter a full path.
        Its parent must already exist. Review the exact path and confirm;
        cancelling changes no files. Run install.py without sudo.

             If a fresh destination already holds files or contains the
        extracted package, choose another empty, owned destination. An actual
        existing SFX board needs the update steps in section 5 instead. Keep
        the package and board files intact while checking which folder is
        which; do not delete a board or use sudo to bypass a refusal.

             A permissions refusal names the exact path that failed its
        ownership or group/other-write check. Use an appropriate private folder
        owned by your normal account. Correct only the intended path or select
        another destination; do not change a whole home/system tree blindly.

             Installation finishes by printing the exact launcher command for
        the folder you selected. Run that command in the same Linux desktop
        terminal. With the default folder, it is:

            "$HOME/sfx/spitfire-x"

             The launcher opens or reuses the local Control Panel in your
        desktop browser. On a new board, Set Up Your BBS asks for the BBS
        name, time zone, nodes, listen address and caller ports, then your
        Sysop name and caller password and the separate System Password.
        Repeat both passwords and choose Create My BBS.

             Review the saved settings, then choose Start BBS when you want
        callers to connect. Installing, creating the board or opening the
        controls does not start the BBS. Choose unused caller ports; the
        defaults are Telnet 2323 and SSH 2222. If setup has an uncertain
        result, use Check Setup Result before another attempt.

             If the desktop browser does not open, read the launcher's
        reported failure and its private management/browser-launch.log under
        your installed board. Genuine failures identify that log and offer
        the local --print-url route. With the default installation, use:

            "$HOME/sfx/spitfire-x" --print-url

             Open the complete printed link in a browser on that same
        computer. Keep the link private: it contains temporary local access.
        Browser diagnostics are captured in that private log; they are not
        proof of an SFX integrity or antivirus failure. Do not post the whole
        log or launch URL publicly. For a remote SSH session, use section 6's
        separate forwarding steps instead of opening a remote loopback link.

             To skip the folder prompt for automation, select the destination
        explicitly:

            python3 install.py --prefix "$HOME/sfx"

             A location under /home or /opt may require an administrator to
        create only the NEW leaf folder and give your account ownership first.
        Do not adopt an existing folder blindly, change /home or /opt
        ownership, or run the installer as root. The default folder under your
        own home avoids that extra step.

             Source builds remain available through the matching Full-Source
        archive and REBUILD.txt. It documents the build, verified runtime
        assets, staging and installer. Source builds use the same fresh-
        install and upgrade choices; vendor components retain their own
        runtime requirements.

        2.0 - HELP AND WHAT COMES WITH SFX
        ----------------------------------

             Choose Help > SFX Wiki in the Control Panel, or open
        docs/sysop/wiki.html. The Linux Controls page explains setup, callers,
        File Manager, backup, restore, sounds, services and copied boards. The
        Wiki works offline.

             Fresh installs include SFX Guess a Number, NewsWire-X RSS and the
        existing X-Gate Game Server gateway menu entry. Updates preserve your
        menus and Door settings. Original DOS games and their emulator are not
        included in this package.

             For a DOS game, open Doors > Set up DOS doors. The wizard checks
        compatible installed DOSEMU2 and guides the game and caller settings.
        Optional preparation of a separate SFX copy currently uses the Pop!_OS
        22.04 AMD64 profile. Mint checks reused a compatible installed runtime.
        Read the Doors guide and test your game before opening it to callers.

             The portable ClamAV 1.4.7 engine is included. Initial definitions
        need an Internet connection. Check Security > ClamAV for the actual
        update result; having the engine alone does not mean definitions are
        ready. If a required scan cannot finish, SFX does not publish the
        upload. An already received file can remain held privately for Sysop
        review. An ordinary stopped update with this matched package can
        repair its engine. Existing definitions and your saved scan choice
        are preserved.



                                                                         Page 2

        -----------------------------------------------------------------------
        -+ SPITFIRE-X BULLETIN BOARD SYSTEM - SYSOP README +-
        -----------------------------------------------------------------------

             docs/thirdparty contains the original software credits and
        licenses. The matching transfer-helper and upstream ClamAV source
        companions are separate downloads supplied with this package; they are
        not needed to run your BBS.

             The original SPITFIRE Bulletin Board System was created by Mike
        Woltz of Buffalo Creek Software. This README follows the text
        formatting of his original documentation as a tribute to that history.
        Its instructions describe Spitfire-X BBS.

             Thanks also to Rob Swindell (Digital Man), Stephen Hurd
        (Deuce), and the Synchronet contributors for the work behind SFX's
        separate transfer helpers and selected DOS Door support. DeuceSSH
        provides SFX's SSH support. More thanks and software credits are
        in the Sysop Wiki at docs/sysop/wiki.html.

        3.0 - LAN BROWSER ACCESS
        ------------------------

             Open Security > Web Access locally. Enable the HTTPS listener and
        add only the computer addresses you want to allow. Set the certificate
        identity to the address or hostname you actually use. Save and review
        the HTTPS address. LAN sign-in uses your System Password. A new
        self-signed certificate needs your browser's own review; SFX does not
        change your computer's trust store. Do not expose the management page
        to the public Internet.

        4.0 - OPTIONAL STARTUP WITH LINUX
        ---------------------------------

             Stop the BBS and close management before preparing its service:

            "$HOME/sfx/spitfire-x" --stop-management
            "$HOME/sfx/prepare-sfx" --service-plan

             Review the generated plan. It prints the exact administrator
        commands for that board. Preparing a plan does not install a service.
        Add --low-ports only if your chosen caller ports need it. SFX still
        runs as your account; Doors and helpers do not receive the service's
        bind capability. System > Runtime Behavior separately controls whether
        the BBS starts when the Linux service starts. Leave it off until you
        are ready.

        5.0 - UPDATING OR MOVING A BOARD
        --------------------------------

             These upgrade steps are for an existing lowercase-layout board,
        including an existing Pop!_OS installation. Keep a usable
        whole-board backup and use the same account and installation folder.
        Older uppercase boards and backups are not migrated.

             In the Control Panel, choose Stop BBS and wait for callers and
        maintenance to finish. Closing the browser does not stop management.
        For ordinary desktop management, run the installed command,
        substituting your real folder:

            "$HOME/sfx/spitfire-x" --stop-management

             If management runs as a service, use its exact administrator stop
        command printed by that tool instead. Do not update while that board's
        management is still running. Extract the new single TAR into a
        separate staging folder, then run the new installer against your
        EXISTING board:

            tar -xzf spitfire-x-1.0.0b3-linux-x86_64.tar.gz
            cd spitfire-x-1.0.0b3-linux-x86_64
            python3 install.py --prefix "$HOME/sfx" --upgrade

             Replace $HOME/sfx with the actual existing folder if different.
        Programs, web pages and unchanged stock help are refreshed; your board
        database, callers, settings, messages, files, artwork, menus and
        Door/BotGate choices are retained. Edited guides under docs are
        preserved. Keep recovery journals and follow any refused or incomplete
        result; do not delete them to force the update.

             After success, run the exact launcher command printed by the
        installer on the Linux desktop. Check the version and saved settings,
        then choose Start BBS when ready. An established board keeps its
        setup; do not create a new board or repeat first-run setup to perform
        an upgrade. For remote management use section 6.

             To move a stopped lowercase-layout board, copy the whole SFX
        folder to its new location. Keep backups with their companion archives
        and ownership files. Run prepare-sfx there, then choose Prepare Copied
        Files in SFXConfig when requested. Check the actual result before
        starting. Preparation does not recreate missing files or overwrite
        changed files. See Linux Controls for the separate Core Restore and
        missing-only File Contents restore steps.

        6.0 - SSH ACCESS FROM ANOTHER COMPUTER
        ----------------------------------------

             Use this separate route only when you are connected remotely. It
        requires an already working operating-system SSH login for the account
        that owns SFX and permission to forward a port. SFX does not install
        or configure that SSH service. The Linux host needs no desktop browser
        for --print-url; use the browser on your other computer. This is
        different from a caller connecting to the BBS SSH port.

             Install or upgrade on the Linux host using the steps above. In
        that SSH shell, use your selected installation path to print a fresh
        private controls link without opening a browser:

            "$HOME/sfx/spitfire-x" --print-url

             Keep the complete printed http://127.0.0.1:PORT/#launch=... link
        private. Note its actual numeric port. On the computer running your
        browser, open another terminal and forward that SAME port through your
        normal host SSH login. If the printed port is 39123, for example:

            ssh -N -L 127.0.0.1:39123:127.0.0.1:39123 yourname@linux-host

             Replace both 39123 values with the actual printed port, and
        yourname@linux-host with your real SSH account and host. Keep that
        terminal open. Bind only to 127.0.0.1; do not expose the forward on
        all interfaces. A local port conflict or an SSH forwarding refusal
        must be resolved before continuing.

             In your browser on that computer, open the complete printed link.
        Keep its 127.0.0.1 address, port and full launch token unchanged. The
        SSH tunnel carries the connection to SFX's loopback controls. For a
        new board, finish the same Set Up Your BBS flow, review settings, then
        Start BBS; an existing board keeps its saved setup.

             A launch link expires and is not a shareable LAN URL. If it
        expires before use, rerun --print-url and open the new complete link.
        Closing the tunnel ends this browser access, not the BBS or
        management. Use the normal Stop BBS and --stop-management steps when
        you intend to stop them. After setup, LAN HTTPS/Web Access with
        approved addresses and the System Password remains a separate option.











































                                                                         Page 4

