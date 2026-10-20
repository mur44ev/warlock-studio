// SPDX-License-Identifier: GPL-3.0-or-later
#include "Device.h"

#include "../PresetFormat.h"
#include "WarlockLayout.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
unsigned char byte(unsigned v){return static_cast<unsigned char>(v);}
constexpr int kLegacyCompactEffects=6;
bool isEffectMode(int mode){return mode>=AW60_MODE_FIRST_EFFECT && mode<AW60_MODE_FIRST_EFFECT+AW60_EFFECT_COUNT;}
}

Device::Packet Device::command(unsigned op,unsigned a,unsigned b){Packet p{};p[1]=byte(op);p[2]=byte(a);p[3]=byte(b);p[8]=byte(255-op-a-b);return p;}
void Device::raw(const Packet& p){if(!handle || hid_send_feature_report(handle,p.data(),65)!=65)throw std::runtime_error("USB write failed / device disconnected");}
Device::Packet Device::rawRead(){Packet p{};if(!handle || hid_get_feature_report(handle,p.data(),65)!=65)throw std::runtime_error("USB read failed / device disconnected");return p;}
Device::Packet Device::receiver(){raw(command(0xf7));QThread::msleep(10);auto p=rawRead();if(p[7]<1 || p[7]>3)throw std::runtime_error("Invalid receiver status");return p;}
void Device::send(const Packet& p){
 if(radio){QElapsedTimer wait;wait.start();for(;;){auto s=receiver();if(s[4]!=0)throw std::runtime_error("Keyboard is offline on 2.4 GHz");if(s[6]==1)break;if(wait.elapsed()>500)throw std::runtime_error("Receiver busy");QThread::msleep(5);}if(!selected){raw(command(0xf6,0x0a));QThread::msleep(10);selected=true;}}
 raw(p);
}
Device::Packet Device::query(Packet p){send(p);if(!radio){QThread::msleep(25);return rawRead();}QElapsedTimer wait;wait.start();while(receiver()[1]!=1){if(wait.elapsed()>1000)throw std::runtime_error("Radio response timed out");QThread::msleep(5);}raw(command(0xfc));QThread::msleep(10);return rawRead();}
Device::Packet Device::get(unsigned c,unsigned a,unsigned b){return query(command(c,a,b));}
void Device::fail(const std::exception& e){running=false;if(timer)timer->stop();emit message(QString::fromUtf8(e.what()),true);emit linkLost();}
void Device::backup(QString label,QJsonObject data){QString dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/backups";QDir().mkpath(dir);QSaveFile f(dir+"/"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")+"-"+label+".json");if(!f.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot save backup");f.write(QJsonDocument(data).toJson());if(!f.commit())throw std::runtime_error("Backup write failed");}
void Device::shutdown(){running=false;autonomousActive=false;if(timer)timer->stop();if(keyTimer)keyTimer->stop();if(handle){hid_close(handle);handle=nullptr;}selected=false;}

QByteArray Device::readSlotBytes(unsigned slot){
 if(!handle || slot>=AW60_PRESET_SLOT_COUNT)throw std::runtime_error("Invalid preset slot");
 QByteArray bytes;for(unsigned page=0;page<4;++page){auto p=get(0x8b,slot,page);bytes.append(reinterpret_cast<const char*>(p.data()+1),64);}return bytes;
}

void Device::writeSlotBytes(unsigned slot,const QByteArray& blob,QString label){
 if(!handle || slot>=AW60_PRESET_SLOT_COUNT || blob.size()!=AW60_PRESET_SLOT_BYTES)throw std::runtime_error("Invalid preset slot data");
 auto before=readSlotBytes(slot);backup(label+"-before",{{"preset",true},{"slot",int(slot)},{"bytes",QString(before.toHex())}});
 for(unsigned page=0;page<5;++page){auto p=command(0x16,slot,page);p[4]=56;p[5]=page==4?1:0;unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=byte(255-sum);for(unsigned j=0;j<56 && page*56+j<static_cast<unsigned>(blob.size());++j)p[9+j]=byte(blob[int(page*56+j)]);send(p);QThread::msleep(150);}QThread::msleep(2000);
 if(readSlotBytes(slot)!=blob)throw std::runtime_error("Preset readback mismatch; original backup retained");
}

void Device::open(bool preferRadio){
 shutdown();try{
  auto list=hid_enumerate(0x3151,0);QByteArray wired,receiverPath;
  for(auto p=list;p;p=p->next)if(p->usage_page==0xffff && p->usage==2){if(p->product_id==0x4015)wired=p->path;if(p->product_id==0x4011)receiverPath=p->path;}
  hid_free_enumeration(list);radio=preferRadio || wired.isEmpty();auto path=radio?receiverPath:wired;
  if(path.isEmpty())throw std::runtime_error("Keyboard / receiver not found");handle=hid_open_path(path.constData());if(!handle)throw std::runtime_error("Cannot open device");
  auto id=get(0x8f);unsigned model=id[2]|unsigned(id[3])<<8;if(id[1]!=0x8f || (model!=919 && model!=1126))throw std::runtime_error("Unknown keyboard identity");
  auto rev=get(0x80),cap=get(0xe9);aw60=cap[1]==0xe9 && cap[2]=='A' && cap[3]=='W' && cap[4]=='6' && cap[5]=='0';compact=aw60 && cap[24]>=3;autonomousSupported=aw60 && cap[24]>=4;reactiveSupported=aw60 && cap[24]>=6;revision=aw60?cap[24]:0;autonomousActive=autonomousSupported && cap[7]==2;
  auto pr=get(0x85);if(pr[1]!=0x85 || pr[2]>4)throw std::runtime_error("Invalid hardware profile");profile=pr[2];auto deb=get(0x91),options=get(0x86),sleep=get(0x92);
  readLayers();QJsonObject info{{"model",int(model)},{"version",QString("%1.%2").arg(rev[3]).arg(rev[2],2,10,QChar('0'))},{"radio",radio},{"aw60",aw60},{"compact",compact},{"revision",revision},{"reactive",reactiveSupported},{"profile",profile},{"debounce",deb[3]},{"base",QString(base.toHex())},{"fn",QString(fn.toHex())},{"options",options[3]},{"sleep",QString(QByteArray(reinterpret_cast<const char*>(sleep.data()+1),64).toHex())}};
  if(autonomousActive){info["effect"]=cap[29];info["effectSpeed"]=cap[30];info["effectBrightness"]=cap[31];info["effectFps"]=cap[32];info["effectColor"]=QColor(cap[33],cap[34],cap[35]).name();if(cap[24]>=5){info["effectScale"]=cap[36]?cap[36]:50;info["effectAmount"]=cap[37]?cap[37]:60;info["effectColor2"]=QColor(cap[38],cap[39],cap[40]).name();info["effectColor3"]=QColor(cap[41],cap[42],cap[43]).name();info["presetCount"]=cap[44];if(cap[45]!=0xff)info["presetSlot"]=cap[45];}}
  backup("connection",info);emit connected(info);emit message(radio?"Подключено через 2.4 GHz":"Подключено по USB");
  if(!timer){timer=new QTimer(this);timer->setTimerType(Qt::PreciseTimer);timer->setInterval(1);connect(timer,&QTimer::timeout,this,&Device::tick);}clock.start();stats.start();
 }catch(const std::exception& e){shutdown();fail(e);}
}

void Device::readLayers(){base.clear();fn.clear();for(unsigned page=0;page<8;++page){auto b=get(0x89,profile,page);base.append(reinterpret_cast<const char*>(b.data()+1),64);auto f=get(0x90,profile,page);fn.append(reinterpret_cast<const char*>(f.data()+1),64);}}

void Device::refresh(){if(!handle)return;bool resume=running;pause();try{auto p=get(0xe9);emit telemetry({{"frames",int(p[9]|unsigned(p[10])<<8|unsigned(p[11])<<16|unsigned(p[12])<<24)},{"errors",int(p[13]|unsigned(p[14])<<8)}});if(radio){auto s=receiver();emit telemetry({{"battery",s[2]}});}if(resume)apply(config);}catch(const std::exception& e){fail(e);}}

void Device::pause(){running=false;if(timer)timer->stop();if(autonomousActive){autonomousActive=false;try{send(command(0xeb,255));}catch(const std::exception& e){emit message(QString::fromUtf8(e.what()),true);}}}

void Device::apply(QJsonObject state){
 if(!handle){emit message("Сначала подключите клавиатуру",true);return;}
 pause();config=state;dirty=true;deadline=0;clock.restart();stats.restart();sent=0;
 int mode=config["mode"].toInt();
 if(mode==AW60_MODE_HARDWARE){try{auto c=QColor(config["color"].toString("#00aaff"));send(ArdorProtocol::Lighting(byte(config["hardware"].toInt(5)),byte(4-config["speed"].toInt(50)/25),byte(config["brightness"].toInt(255)*4/255),7,byte(c.red()),byte(c.green()),byte(c.blue())));QThread::msleep(1000);emit message("Аппаратный эффект сохранён в клавиатуру");}catch(const std::exception& e){fail(e);}return;}
 if(!aw60){emit message("Для Direct и новых эффектов нужна прошивка AW60",true);return;}
 if(isEffectMode(mode) && mode>=AW60_MODE_FIRST_EFFECT+AW60_FIRST_REACTIVE && !reactiveSupported){emit message("Эффекты с откликом на нажатия требуют прошивку AW60 rev6. Прошейте её по кабелю и повторите.",true);return;}
 if(autonomousSupported && isEffectMode(mode)){try{auto s=get(0xe9);previousFrames=s[9]|quint32(s[10])<<8|quint32(s[11])<<16|quint32(s[12])<<24;auto c1=QColor(config["color"].toString("#00aaff")),c2=QColor(config["color2"].toString("#ff4fd8")),c3=QColor(config["color3"].toString("#7bffef"));auto p=command(0xeb,mode-AW60_MODE_FIRST_EFFECT);p[3]=byte(std::clamp(config["speed"].toInt(35),1,100));p[4]=byte(std::clamp(config["brightness"].toInt(255),0,255));p[5]=byte(std::clamp(config["fps"].toInt(60),1,60));p[6]=byte(c1.red());p[7]=byte(c1.green());p[9]=byte(c1.blue());p[10]=byte(++sequence);p[11]=byte(sequence>>8);p[12]=byte(std::clamp(config["scale"].toInt(50),1,100));p[13]=byte(std::clamp(config["amount"].toInt(60),1,100));p[14]=byte(c2.red());p[15]=byte(c2.green());p[16]=byte(c2.blue());p[17]=byte(c3.red());p[18]=byte(c3.green());p[19]=byte(c3.blue());p[20]=AW60_FLAG_EXPLICIT_PALETTE;unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=byte(255-sum);send(p);autonomousActive=true;stats.restart();clock.restart();}catch(const std::exception& e){fail(e);return;}}
 else if(isEffectMode(mode) && mode>=AW60_MODE_FIRST_EFFECT+kLegacyCompactEffects){emit message("Для этих эффектов нужна прошивка AW60 rev5 или новее",true);return;}
 running=true;timer->start();emit message(autonomousActive?"Эффект выполняется на клавиатуре — Studio можно закрыть":"Подсветка включена");
}

void Device::tick(){if(!running || !handle)return;try{
 if(autonomousActive){if(stats.elapsed()<1000)return;auto s=get(0xe9);quint32 frames=s[9]|quint32(s[10])<<8|quint32(s[11])<<16|quint32(s[12])<<24;double rate=(frames-previousFrames)*1000./stats.elapsed();previousFrames=frames;stats.restart();emit telemetry({{"fps",rate},{"onboard",true}});return;}
 int mode=config["mode"].toInt(),fps=std::clamp(config["fps"].toInt(60),1,60),effect=mode-AW60_MODE_FIRST_EFFECT;if(radio && !compact)fps=std::min(fps,8);
 qint64 now=clock.nsecsElapsed();if(now<deadline)return;deadline+=1000000000/fps;if(deadline<now-1000000000/fps)deadline=now+1000000000/fps;
 if((mode==AW60_MODE_DIRECT || mode==AW60_MODE_OFF) && !dirty)return;dirty=false;
 unsigned brightness=std::clamp(config["brightness"].toInt(255),0,255);QColor baseColor(config["color"].toString("#00aaff"));double t=now/1e9*(.15+config["speed"].toInt(50)*.025);
 if(compact && effect>=0 && effect<kLegacyCompactEffects){auto p=command(0xea,effect);unsigned seq=++sequence;unsigned phase=unsigned(t*65536/6)&65535;p[3]=byte(seq);p[4]=byte(seq>>8);p[5]=byte(phase);p[6]=byte(phase>>8);p[7]=byte(brightness);unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=byte(255-sum);p[9]=byte(baseColor.red());p[10]=byte(baseColor.green());p[11]=byte(baseColor.blue());send(p);}
 else{std::array<unsigned char,240> rgb{};auto colors=config["colors"].toArray();for(unsigned i=0;i<80;++i){QColor c=mode==AW60_MODE_DIRECT?QColor(colors.size()==80?colors[int(i)].toString():baseColor.name()):baseColor;double intensity=1;double x=(WarlockLayout::keys[i].slot/6)/14.,y=(WarlockLayout::keys[i].slot%6)/5.;if(mode==AW60_MODE_OFF)intensity=0;else switch(effect){case 0:c=QColor::fromHsvF(std::fmod(x*.65+t*.18,1.),1,1);break;case 1:intensity=.08+.92*(std::sin(x*6.28-t*3)+1)/2;break;case 2:c=QColor::fromHsvF(std::fmod(x*.5+y*.2+t*.04,1.),.9,1);break;case 3:intensity=(std::sin(t*2)+1)/2;break;case 4:intensity=std::pow((std::sin(t*(1.3+(i%7)*.1)+i*2.399)+1)/2,8);break;case 5:{double d=(x-(std::sin(t*2)+1)/2)*5;intensity=std::exp(-d*d);break;}default:break;}rgb[i*3]=byte(unsigned(c.red()*intensity)*brightness/255);rgb[i*3+1]=byte(unsigned(c.green()*intensity)*brightness/255);rgb[i*3+2]=byte(unsigned(c.blue()*intensity)*brightness/255);}for(auto& p:ArdorProtocol::Frame(rgb,++sequence))send(p);}++sent;
 if(stats.elapsed()>=1000){emit telemetry({{"fps",sent*1000./stats.elapsed()},{"radio",radio},{"compact",compact}});stats.restart();sent=0;}
}catch(const std::exception& e){fail(e);}}

void Device::writeKeys(QJsonObject request){bool resume=running;pause();try{
 if(!handle || base.size()!=512 || fn.size()!=512)throw std::runtime_error("Read key layers first");bool fnLayer=request["fn"].toBool();QByteArray before=fnLayer?fn:base,after=before;auto value=QByteArray::fromHex(request["value"].toString().toLatin1());if(!request.contains("image") && value.size()!=4)throw std::runtime_error("Key action must be four bytes");
 if(request.contains("image")){if(request["profile"].toInt(-1)!=profile)throw std::runtime_error("Select the backup's hardware profile first");after=QByteArray::fromHex(request["image"].toString().toLatin1());if(after.size()!=512)throw std::runtime_error("Invalid backup size");}
 else{auto keys=request["keys"].toArray();if(keys.isEmpty())throw std::runtime_error("Select keys first");for(auto k:keys){int index=k.toInt(-1);if(index<0 || index>=80 || WarlockLayout::keys[index].slot==65)throw std::runtime_error("Invalid slot / Fn key is protected");after.replace(int(WarlockLayout::keys[index].slot)*4,4,value);}}
 backup("keymap-before",{{"profile",profile},{"fn",fnLayer},{"bytes",QString(before.toHex())}});
 for(unsigned page=0;page<9;++page){auto p=command(fnLayer?0x10:0x09,profile,248);p[4]=1;p[5]=byte(page);unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=byte(255-sum);for(unsigned j=0;j<56;++j)p[9+j]=byte(after[int(page*56+j)]);send(p);QThread::msleep(150);}QThread::msleep(2000);readLayers();if((fnLayer?fn:base).left(504)!=after.left(504))throw std::runtime_error("Keymap readback mismatch; backup retained");emit message("Переназначения записаны и проверены");emit connected({{"base",QString(base.toHex())},{"fn",QString(fn.toHex())}});if(resume)apply(config);
}catch(const std::exception& e){fail(e);}}

void Device::writePreset(QJsonObject request){bool resume=running;pause();try{unsigned slot=unsigned(request["slot"].toInt(-1));auto blob=QByteArray::fromHex(request["raw"].toString().toLatin1());if(blob.size()!=AW60_PRESET_SLOT_BYTES)throw std::runtime_error("Invalid preset size");writeSlotBytes(slot,blob,"preset");emit message("Пресет записан и проверен. Аппаратная смена подсветки листает все валидные слоты 0..49.");if(resume)apply(config);}catch(const std::exception& e){fail(e);}}

void Device::readPreset(int slot){bool resume=running;pause();try{auto blob=readSlotBytes(unsigned(slot));backup("preset-read",{{"preset",true},{"slot",slot},{"bytes",QString(blob.toHex())}});emit presetLoaded({{"slot",slot},{"raw",QString(blob.toHex())}});emit message("Слот пресета прочитан из клавиатуры");if(resume)apply(config);}catch(const std::exception& e){fail(e);}}

void Device::setting(QJsonObject request){bool resume=running;pause();try{QString kind=request["kind"].toString();unsigned v=unsigned(request["value"].toInt());if(kind=="debounce"){if(v<1 || v>10)throw std::runtime_error("Debounce range is 1..10");send(command(0x11,0,v));QThread::msleep(1200);if(get(0x91)[3]!=v)throw std::runtime_error("Debounce readback mismatch");}
 else if(kind=="profile"){if(v>4)throw std::runtime_error("Profile range is 0..4");send(command(0x05,v));QThread::msleep(1200);if(get(0x85)[2]!=v)throw std::runtime_error("Profile readback mismatch");profile=int(v);readLayers();emit connected({{"profile",profile},{"base",QString(base.toHex())},{"fn",QString(fn.toHex())}});}
 else if(kind=="winlock"){auto before=get(0x86);auto p=command(6);p[3]=byte((before[3]&~1u)|(v?1u:0u));p[4]=before[4];p[5]=before[5];unsigned sum=0;for(unsigned i=1;i<8;++i)sum+=p[i];p[8]=byte(255-sum);send(p);QThread::msleep(1200);if(bool(get(0x86)[3]&1)!=bool(v))throw std::runtime_error("Win-lock readback mismatch");}
 emit message("Настройка применена и проверена");if(resume)apply(config);
}catch(const std::exception& e){fail(e);}}

void Device::keyMonitor(bool on){
 if(!on){if(keyTimer)keyTimer->stop();return;}
 if(!handle){emit message("Сначала подключите клавиатуру",true);return;}
 if(radio){emit message("Монитор клавиш работает по USB-кабелю: радиоканал слишком медленный",true);return;}
 if(!reactiveSupported){emit message("Монитору клавиш нужна прошивка AW60 rev6",true);return;}
 if(!keyTimer){keyTimer=new QTimer(this);keyTimer->setInterval(40);connect(keyTimer,&QTimer::timeout,this,&Device::pollKeys);}
 keyTimer->start();
}

void Device::pollKeys(){
 if(!handle)return;
 try{
  auto p=get(0xec);
  if(p[1]!=0xec || p[2]!='K' || p[3]!='E' || p[4]!='Y'){keyTimer->stop();emit message("Прошивка не отвечает на монитор клавиш",true);return;}
  emit keyState({{"held",QString(QByteArray(reinterpret_cast<const char*>(p.data()+25),10).toHex())},{"raw",QString(QByteArray(reinterpret_cast<const char*>(p.data()+9),15).toHex())},{"presses",int(p[35]|unsigned(p[36])<<8|unsigned(p[37])<<16|unsigned(p[38])<<24)},{"tracking",p[6]!=0},{"active",int(p[7])}});
 }catch(const std::exception& e){if(keyTimer)keyTimer->stop();emit message(QString::fromUtf8(e.what()),true);}
}

void Device::scanPresets(){
 if(!handle){emit message("Сначала подключите клавиатуру",true);return;}
 bool resume=running;pause();
 try{
  for(int slot=0;slot<AW60_PRESET_SLOT_COUNT;++slot){emit presetScanned(slot,QString(readSlotBytes(unsigned(slot)).toHex()),slot==AW60_PRESET_SLOT_COUNT-1);}
  emit message("Слоты пресетов прочитаны");
  if(resume)apply(config);
 }catch(const std::exception& e){fail(e);}
}

void Device::perfReport(bool reset){
 if(!handle){emit message("Сначала подключите клавиатуру",true);return;}
 if(revision<8){emit message("Диагностика требует прошивку AW60 rev8",true);return;}
 try{
  auto word=[](const Packet& p,int payloadOffset){return quint32(p[1+payloadOffset])|quint32(p[2+payloadOffset])<<8|quint32(p[3+payloadOffset])<<16|quint32(p[4+payloadOffset])<<24;};
  auto page0=get(0xed,0,0),page1=get(0xed,1,0),page2=get(0xed,2,reset?1:0);
  for(auto& p:{page0,page1,page2})if(p[1]!=0xed || p[2]!='P' || p[3]!='F')throw std::runtime_error("Прошивка не отвечает на диагностику");
  QJsonArray hist;for(int i=0;i<8;++i)hist.append(int(page1[1+40+i*2]|unsigned(page1[2+40+i*2])<<8));
  QJsonArray isr;for(int i=0;i<4;++i)isr.append(QJsonObject{{"max",double(word(page2,8+i*12))},{"count",double(word(page2,12+i*12))},{"sum16",double(word(page2,16+i*12))}});
  QJsonObject r{{"hclkSel",double(word(page0,8))},{"reload",double(word(page0,12))},{"cyclesPerMs",double(word(page0,36))},{"prio0",double(word(page0,20))},{"prio3",double(word(page0,24))},{"prio4",double(word(page0,28))},
                {"gapMax",double(word(page1,8))},{"polls",double(word(page1,12))},{"scans",double(word(page1,36))},{"sliceMax",double(word(page1,16))},{"slices",double(word(page1,20))},{"frames",double(word(page1,24))},{"convMax",double(word(page1,28))},{"convCount",double(word(page1,32))},
                {"hist",hist},{"isr",isr},{"elapsedMs",double(perfClock.isValid()?perfClock.elapsed():0)}};
  if(reset)perfClock.restart();else if(!perfClock.isValid())perfClock.start();
  emit perfData(r);
 }catch(const std::exception& e){fail(e);}
}
