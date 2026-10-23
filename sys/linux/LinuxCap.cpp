#include "LinuxCap.h"

#include <QDebug>
#include <QProcess>
#include <QStandardPaths>

QString Linux_GetCapString(const QString &path) {
    QProcess p;
    p.setProgram(Linux_FindCapProgsExec("getcap"));
    p.setArguments({path});
    p.start();
    p.waitForFinished(500);
    return p.readAllStandardOutput();
}

QString Linux_FindCapProgsExec(const QString &name) {
    QString exec = QStandardPaths::findExecutable(name);
    if (exec.isEmpty())
        exec = QStandardPaths::findExecutable(name, {"/usr/sbin", "/sbin"});

    if (exec.isEmpty())
        qDebug() << "Executable" << name << "could not be resolved";
    else
        qDebug() << "Found exec" << name << "at" << exec;

    return exec.isEmpty() ? name : exec;
}
