// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "main_window.hpp"
#include "vcam_manager.hpp"

#include <QApplication>
#include <QString>

#include <windows.h>
#include <mfapi.h>

int main(int argc, char** argv)
{
    // Elevated helper invocations (relaunched via ShellExecute "runas").
    for (int i = 1; i < argc; i++)
    {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--vcam-install"))
            return cyclops_vcam::install_main();
        if (a == QStringLiteral("--vcam-uninstall"))
            return cyclops_vcam::uninstall_main();
        if (a == QStringLiteral("--vcam-enable"))
        {
            QString err;
            return cyclops_vcam::enable(&err) ? 0 : 30;
        }
        if (a == QStringLiteral("--vcam-disable"))
        {
            QString err;
            return cyclops_vcam::disable(&err) ? 0 : 31;
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Cyclops"));
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/icon_eye.png")));

    // MF device enumeration and IMFVirtualCamera calls need the runtime up.
    MFStartup(MF_VERSION);
    main_window w;
    w.show();
    const int rc = app.exec();
    MFShutdown();
    return rc;
}
