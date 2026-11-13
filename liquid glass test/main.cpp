// SPDX-License-Identifier: GPL-3.0-only
#include <QGuiApplication>
#include <QClipboard>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QDir>
#include <QLibraryInfo>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <atomic>
static QFile *labLog=nullptr;
static QMutex logMutex;
static std::atomic_int runtimeErrors{0};

class LabController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl shaderUrl READ shaderUrl NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    QUrl shaderUrl() const { return url; }
    QString status() const { return message; }
    LabController() {
        debounce.setSingleShot(true); debounce.setInterval(180);
        connect(&debounce,&QTimer::timeout,this,&LabController::rebuild);
        const QString source=QStringLiteral(GLASS_SOURCE_DIR)+"/shaders/liquidglass.frag";
        watcher.addPath(QFileInfo(source).absolutePath()); watcher.addPath(source);
        connect(&watcher,&QFileSystemWatcher::directoryChanged,this,[this]{debounce.start();});
        connect(&watcher,&QFileSystemWatcher::fileChanged,this,[this,source]{
            if(!watcher.files().contains(source))watcher.addPath(source);
            debounce.start();
        });
        connect(&compiler,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){
            message="Cannot start qsb: "+compiler.errorString(); emit changed(); emit compiled(false);
        });
        connect(&compiler,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus exit){
            const bool ok=code==0 && exit==QProcess::NormalExit;
            if(ok) {
                url=QUrl::fromLocalFile(output);
                message="Shader updated · "+QDateTime::currentDateTime().toString("HH:mm:ss");
            } else message="Shader error (keeping last good version):\n"+QString::fromLocal8Bit(compiler.readAllStandardError());
            emit changed(); emit compiled(ok);
            if(pending){pending=false;debounce.start();}
        });
        QTimer::singleShot(0,this,&LabController::rebuild);
    }
    Q_INVOKABLE void rebuild() {
        if(compiler.state()!=QProcess::NotRunning){pending=true;return;}
        if(!temp.isValid()){message="Cannot create shader temporary directory";emit changed();emit compiled(false);return;}
        output=temp.filePath(QString::number(++revision)+".frag.qsb");
        message="Compiling shader only…";emit changed();
        compiler.start(QLibraryInfo::path(QLibraryInfo::BinariesPath)+"/qsb.exe",
            {"--glsl","100 es,120,150","--hlsl","50","--msl","12","-o",output,
             QStringLiteral(GLASS_SOURCE_DIR)+"/shaders/liquidglass.frag"});
    }
    Q_INVOKABLE void copy(const QString &text){QGuiApplication::clipboard()->setText(text);}
    Q_INVOKABLE bool screenshot(QObject *object) {
        auto window=qobject_cast<QQuickWindow*>(object);if(!window)return false;
        const QString path=QCoreApplication::applicationDirPath()+"/captures";
        if(!QDir().mkpath(path))return false;
        const QString file=path+"/glass-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")+".png";
        const bool ok=window->grabWindow().save(file);
        message=ok?"Saved: "+file:"Screenshot failed";emit changed();return ok;
    }
signals:
    void changed();
    void compiled(bool success);
private:
    QUrl url;QString message="Starting…",output;
    QTemporaryDir temp;QFileSystemWatcher watcher;QTimer debounce;QProcess compiler;
    int revision=0;bool pending=false;
};

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
    QFile log(QCoreApplication::applicationDirPath()+"/glass-lab.log");
    if(log.open(QIODevice::WriteOnly|QIODevice::Truncate)) {
        labLog=&log;
        qInstallMessageHandler([](QtMsgType type,const QMessageLogContext &,const QString &message){
            QMutexLocker lock(&logMutex);
            labLog->write(message.toUtf8()+"\n");labLog->flush();
            if(type==QtWarningMsg || type==QtCriticalMsg)++runtimeErrors;
        });
    }
    app.setOrganizationName("LiquidLauncherTools");app.setApplicationName("LiquidGlassLab");
    QQuickStyle::setStyle("Basic");
    LabController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("labController",&controller);
    const bool smoke=app.arguments().contains("--smoke");
    engine.rootContext()->setContextProperty("labSettingsFile",QUrl::fromLocalFile(QCoreApplication::applicationDirPath()+(smoke?"/lab-settings-smoke.ini":"/lab-settings.ini")));
    QObject::connect(&engine,&QQmlApplicationEngine::objectCreationFailed,&app,[]{QCoreApplication::exit(1);},Qt::QueuedConnection);
    int compileCount=0;
    QUrl firstShader;
    if(smoke) {
        QObject::connect(&controller,&LabController::compiled,&app,[&](bool ok){
            if(!ok){app.exit(2);return;}
            if(++compileCount==1){firstShader=controller.shaderUrl();QTimer::singleShot(100,&controller,&LabController::rebuild);return;}
            if(firstShader==controller.shaderUrl()){app.exit(5);return;}
            QTimer::singleShot(1200,&app,[&]{
                app.exit(!engine.rootObjects().isEmpty() && runtimeErrors==0 && controller.screenshot(engine.rootObjects().first())?0:3);
            });
        });
        QTimer::singleShot(20000,&app,[]{QCoreApplication::exit(4);});
    }
    engine.load(QUrl::fromLocalFile(QStringLiteral(GLASS_LAB_DIR)+"/Lab.qml"));
    const int result=app.exec();
    qInstallMessageHandler(nullptr);
    return result;
}
#include "main.moc"
