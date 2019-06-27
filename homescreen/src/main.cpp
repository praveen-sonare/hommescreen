/*
 * Copyright (C) 2016, 2017 Mentor Graphics Development (Deutschland) GmbH
 * Copyright (c) 2017, 2018 TOYOTA MOTOR CORPORATION
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickWindow>
#include <QThread>

#include <qlibwindowmanager.h>
#include <weather.h>
#include <bluetooth.h>
#include "applicationlauncher.h"
#include "statusbarmodel.h"
#include "afm_user_daemon_proxy.h"
#include "mastervolume.h"
#include "homescreenhandler.h"
#include "homescreenvoice.h"
#include "homescreenconnect.h"
#include "toucharea.h"
#include "shortcutappmodel.h"
#include "hmi-debug.h"

// XXX: We want this DBus connection to be shared across the different
// QML objects, is there another way to do this, a nice way, perhaps?
org::AGL::afm::user *afm_user_daemon_proxy;

namespace {

struct Cleanup {
    static inline void cleanup(org::AGL::afm::user *p) {
        delete p;
        afm_user_daemon_proxy = Q_NULLPTR;
    }
};

void noOutput(QtMsgType, const QMessageLogContext &, const QString &)
{
}

}

int main(int argc, char *argv[])
{
    QGuiApplication a(argc, argv);

    // use launch process
    QScopedPointer<org::AGL::afm::user, Cleanup> afm_user_daemon_proxy(new org::AGL::afm::user("org.AGL.afm.user",
                                                                                               "/org/AGL/afm/user",
                                                                                               QDBusConnection::sessionBus(),
                                                                                               0));
    ::afm_user_daemon_proxy = afm_user_daemon_proxy.data();

    QCoreApplication::setOrganizationDomain("LinuxFoundation");
    QCoreApplication::setOrganizationName("AutomotiveGradeLinux");
    QCoreApplication::setApplicationName("HomeScreen");
    QCoreApplication::setApplicationVersion("0.7.0");

    QCommandLineParser parser;
    parser.addPositionalArgument("port", a.translate("main", "port for binding"));
    parser.addPositionalArgument("secret", a.translate("main", "secret for binding"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(a);
    QStringList positionalArguments = parser.positionalArguments();

    int port = 1700;
    QString token = "wm";
    QString graphic_role = "homescreen"; // defined in layers.json in Window Manager

    if (positionalArguments.length() == 2) {
        port = positionalArguments.takeFirst().toInt();
        token = positionalArguments.takeFirst();
    }

    HMI_DEBUG("HomeScreen","port = %d, token = %s", port, token.toStdString().c_str());

    // import C++ class to QML
    // qmlRegisterType<ApplicationLauncher>("HomeScreen", 1, 0, "ApplicationLauncher");
    qmlRegisterType<StatusBarModel>("HomeScreen", 1, 0, "StatusBarModel");
    qmlRegisterType<MasterVolume>("MasterVolume", 1, 0, "MasterVolume");
    qmlRegisterType<ShortcutAppModel>("ShortcutAppModel", 1, 0, "ShortcutAppModel");

    ApplicationLauncher *launcher = new ApplicationLauncher();
    QLibWindowmanager* layoutHandler = new QLibWindowmanager();
    HomescreenHandler* homescreenHandler = new HomescreenHandler();
    ShortcutAppModel* shortcutAppModel = new ShortcutAppModel();
    HomescreenVoice* homescreenVoice = new HomescreenVoice();
    HomescreenConnect* homescreenConnect = new HomescreenConnect();

    if(layoutHandler->init(port,token) != 0){
        exit(EXIT_FAILURE);
    }

    AGLScreenInfo screenInfo(layoutHandler->get_scale_factor());

    if (layoutHandler->requestSurface(graphic_role) != 0) {
        exit(EXIT_FAILURE);
    }

    QUrl bindingAddress;
    bindingAddress.setScheme(QStringLiteral("ws"));
    bindingAddress.setHost(QStringLiteral("localhost"));
    bindingAddress.setPort(port);
    bindingAddress.setPath(QStringLiteral("/api"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), token);
    bindingAddress.setQuery(query);

    TouchArea* touchArea = new TouchArea();
    homescreenHandler->init(port, token.toStdString().c_str(), layoutHandler, graphic_role);
	homescreenVoice->init(port, token.toStdString().c_str());
    homescreenConnect->init(port, token.toStdString().c_str());

    // mail.qml loading
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bindingAddress", bindingAddress);
    engine.rootContext()->setContextProperty("layoutHandler", layoutHandler);
    engine.rootContext()->setContextProperty("homescreenHandler", homescreenHandler);
    engine.rootContext()->setContextProperty("homescreenVoice", homescreenVoice);
    engine.rootContext()->setContextProperty("homescreenConnect", homescreenConnect);
    engine.rootContext()->setContextProperty("touchArea", touchArea);
    engine.rootContext()->setContextProperty("shortcutAppModel", shortcutAppModel);
    engine.rootContext()->setContextProperty("launcher", launcher);
    engine.rootContext()->setContextProperty("weather", new Weather(bindingAddress));
    engine.rootContext()->setContextProperty("bluetooth", new Bluetooth(bindingAddress, engine.rootContext()));
    engine.rootContext()->setContextProperty("screenInfo", &screenInfo);
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    QObject *root = engine.rootObjects().first();
    QQuickWindow *window = qobject_cast<QQuickWindow *>(root);
    homescreenHandler->setQuickWindow(window);

    layoutHandler->set_event_handler(QLibWindowmanager::Event_SyncDraw, [layoutHandler, &graphic_role](json_object *object) {
        layoutHandler->endDraw(graphic_role);
    });

    layoutHandler->set_event_handler(QLibWindowmanager::Event_ScreenUpdated, [layoutHandler, launcher, homescreenHandler, shortcutAppModel, root](json_object *object) {
        json_object *jarray = json_object_object_get(object, "ids");
        HMI_DEBUG("HomeScreen","ids=%s", json_object_to_json_string(object));
        int arrLen = json_object_array_length(jarray);
        QString label = QString("");
        static bool first_start = true;
        for( int idx = 0; idx < arrLen; idx++)
        {
            label = QString(json_object_get_string(	json_object_array_get_idx(jarray, idx) ));
            HMI_DEBUG("HomeScreen","Event_ScreenUpdated application: %s.", label.toStdString().c_str());
            homescreenHandler->setCurrentApplication(label);
            QMetaObject::invokeMethod(launcher, "setCurrent", Qt::QueuedConnection, Q_ARG(QString, label));
        }
        if((arrLen == 1) && (QString("navigation") == label)){
            QMetaObject::invokeMethod(root, "changeSwitchState", Q_ARG(QVariant, true));
        }else{
            QMetaObject::invokeMethod(root, "changeSwitchState", Q_ARG(QVariant, false));
        }
	if(first_start) {
            if((arrLen == 1) && (QString("launcher") == label)) {
                first_start = false;
                shortcutAppModel->screenUpdated();
            }
	}
    });

    touchArea->setWindow(window);
    QThread* thread = new QThread;
    touchArea->moveToThread(thread);
    QObject::connect(thread, &QThread::started, touchArea, &TouchArea::init);

    thread->start();

    QList<QObject *> sobjs = engine.rootObjects();
    StatusBarModel *statusBar = sobjs.first()->findChild<StatusBarModel *>("statusBar");
    statusBar->init(bindingAddress, engine.rootContext());

    QObject::connect(homescreenHandler, SIGNAL(shortcutChanged(QString, QString, QString)), shortcutAppModel, SLOT(changeShortcut(QString, QString, QString)));
    QObject::connect(shortcutAppModel, SIGNAL(shortcutUpdated(QString, struct json_object*)), homescreenHandler, SLOT(updateShortcut(QString, struct json_object*)));
	QObject::connect(window, SIGNAL(frameSwapped()), layoutHandler, SLOT(slotActivateSurface()));

    return a.exec();
}
