      _________________________________________________________________________
         ___________________                             ___________________
            ________________     S P I T F I R E - X     ________________
               _____________   -------------------------  _____________
                  __________    Bulletin Board System    __________
                     _______         for Windows         _______

                        ____      Personal Computers     ____
                           _______________________________
                             ____                    ____
                               _____  [VERSION]   ____
                                 ___________________



                         Copyright (C) 2026 by xbit ops
                        -------------------------------------
                                 Spitfire-X BBS
                               Sysop Quick Start
                                  MIT License
                                   
                         Win 7 SP1, Win10, Win11, Linux



                     ------------------------------------------
                      >>> Release Date   [RELEASE DATE]   <<<  
                     ------------------------------------------









                                                                         Page 1
        -----------------------------------------------------------------------
        -+ SPITFIRE-X BULLETIN BOARD SYSTEM - SYSOP README +-
        -----------------------------------------------------------------------

        INTRODUCTION
        ------------

             Welcome to Spitfire-X BBS (SFX), a new Windows BBS from xbit ops.
        If you have used SPITFIRE, the caller and Sysop menus should feel
        familiar.

             This README is a short starting point for this public beta.
        The Sysop Wiki and configuration reference are under Docs\Sysop.

             Windows test programs are unsigned. Verify the supplied archive
        SHA-256 before Setup. Beta updates are manual; see Update SFX and
        solve problems in the Wiki before updating an existing board.

        1.0 - GETTING STARTED
        ---------------------

             Run SFX-BBS-Setup.exe from your test package and choose the
        installation folder. A fresh install opens this board's First-Run
        Setup automatically after installation finishes. Choose Finish Later
        in the wizard if you want to configure the board later.

             Use the Spitfire-X BBS shortcut for this installation, if
        selected. Without a shortcut, open this file under the folder you
        installed:

            Bin\SFXControlPanel.exe

             Complete First-Run Setup before editing a new board in SFXConfig.
        Save Setup and Start BBS deliberately saves the board and System Sysop,
        reads the settings back and starts configured caller listeners. Finish
        Later leaves initial setup pending. Review the action before accepting
        it.

             Check saved/readable settings separately from actual listener
        status. A listener bind/start failure can follow a successful save;
        keep its diagnostic and check the listen address and port rather than
        deleting data or recreating the initial account.

             Then use SFXConfig to review connection settings and caller
        access. If it reports that initial setup is required, finish it in the
        same installation's Control Panel and reopen SFXConfig. Keep other
        verification or connection diagnostics for review instead of assuming
        every failure means an uninitialized board. Updates keep your saved
        board choices.

        2.0 - RUNNING YOUR BBS
        ----------------------

             A new installation starts in Desktop Mode. Use the Control
        Panel to run your BBS this way. To run it as a Windows Service, open
        SFXConfig, then System > Runtime Behavior. Choose the hosting mode
        and use the service controls there.

             SFX targets Windows 7 SP1, Windows 10 and Windows 11. The BBS
        program is a native 32-bit Windows application. Optional helpers and
        third-party Doors have their own requirements.

             Portable ClamAV is optional on supported Windows 10/11 hosts.
        Setup offers install now or skip, with later installation under
        SFXConfig > Security > ClamAV. Check engine and definition readiness
        and the separate Scan Uploads setting. Saved choices are kept; a
        required scan that cannot finish holds the upload privately. Windows 7
        SP1 has no ClamAV scanning support.

        3.0 - SYSOP DOCUMENTATION
        -------------------------

             Open the following page in your web browser:

            Docs\Sysop\wiki.html

             This public beta includes the offline Sysop Wiki,
        configuration reference and search. Keep its pages and other files
        together under Docs\Sysop. You can read and search without an Internet
        connection. More help will be added as the manual grows.







                                                                         Page 2
        -----------------------------------------------------------------------
        -+ SPITFIRE-X BULLETIN BOARD SYSTEM - SYSOP README +-
        -----------------------------------------------------------------------

        4.0 - BACKUPS AND RESTORE
        -------------------------

             Back up your whole SFX folder regularly. Stop the BBS before
        copying the folder, and keep all of it together when you restore it.
        Try restoring a backup before you need it, so you know it will work
        when the time comes.

             On a replacement computer, or after moving the folder, follow
        the Prepare SFX on this computer prompt when it appears. Your BBS
        stays stopped until you start it yourself. Check any reported
        problems before going online.

        5.0 - REPORTING A PROBLEM
        -------------------------

             When reporting a problem to xbit ops, include your SFX version,
        Windows version, the steps you took, what you expected and what
        happened. Include a screenshot or the relevant log message when
        available.

        6.0 - LICENSE AND ACKNOWLEDGMENTS
        ---------------------------------

             Spitfire-X BBS is provided under the MIT License. Read the
        included license and third-party notices for the full terms and
        applicable component licenses.

             The original SPITFIRE Bulletin Board System was created by Mike
        Woltz of Buffalo Creek Software. This README follows the text
        formatting of his original documentation as a tribute to that history.
        Its instructions describe Spitfire-X BBS.

             Thanks also to Rob Swindell (Digital Man), Stephen Hurd
        (Deuce), and the Synchronet contributors for the work behind SFX's
        separate transfer helpers and selected DOS Door support. DeuceSSH
        provides SFX's SSH support. More thanks and software credits are
        in the Sysop Wiki at Docs\Sysop\wiki.html.

