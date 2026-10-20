// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>
#include <hidapi.h>

#include "Protocol.h"

class Device:public QObject {
 Q_OBJECT
public:
 explicit Device(QObject* parent=nullptr):QObject(parent){}
public slots:
 void open(bool preferRadio=false);
 void shutdown();
 void apply(QJsonObject state);
 void pause();
 void refresh();
 void writeKeys(QJsonObject request);
 void writePreset(QJsonObject request);
 void readPreset(int slot);
 void setting(QJsonObject request);
 void keyMonitor(bool on);
 void scanPresets();
 void perfReport(bool reset);
signals:
 void message(QString text,bool error=false);
 void connected(QJsonObject info);
 void telemetry(QJsonObject info);
 void presetLoaded(QJsonObject preset);
 void keyState(QJsonObject state);
 void presetScanned(int slot,QString raw,bool done);
 void perfData(QJsonObject report);
 void linkLost();
private:
 using Packet=ArdorProtocol::Packet;
 hid_device* handle=nullptr;
 bool radio=false,selected=false,aw60=false,compact=false,running=false,dirty=false;
 bool autonomousSupported=false,autonomousActive=false,reactiveSupported=false;
 int revision=0;
 quint32 previousFrames=0;
 int profile=0;
 unsigned short sequence=0;
 QTimer *timer=nullptr,*keyTimer=nullptr;
 QElapsedTimer clock,stats,perfClock;
 qint64 deadline=0;
 int sent=0;
 QJsonObject config;
 QByteArray base,fn;
 void raw(const Packet& packet);
 Packet rawRead();
 Packet receiver();
 void send(const Packet& packet);
 Packet query(Packet packet);
 Packet get(unsigned command,unsigned a=0,unsigned b=0);
 static Packet command(unsigned op,unsigned a=0,unsigned b=0);
 void tick();
 void pollKeys();
 void backup(QString label,QJsonObject data);
 void fail(const std::exception& error);
 void readLayers();
 QByteArray readSlotBytes(unsigned slot);
 void writeSlotBytes(unsigned slot,const QByteArray& blob,QString label);
};
