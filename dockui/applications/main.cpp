#include "menu_glass.h"
#include "ui_language.h"
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QMenu>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <numeric>
#include <mutex>
#include "tuning.h"
#include <QFont>
#include <QColorSpace>
#include <QDir>
#include <QImage>
#include <QScreen>
#include <QGuiApplication>
#include <QLinearGradient>
#include <cmath>
#include "icon_mask.h"
#include "app_registry.h"
#include "shell_items.h"
#include "settings_motion.h"
#include "motion.h"
#include <shellapi.h>
#include <QThreadPool>
#include <QMessageBox>
#include <atomic>
#include <QRadialGradient>
#include <QFileInfo>
#include <QVector>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QHash>
#include <QDateTime>
#include <QTextLayout>
#include <QHelpEvent>
#include <QToolTip>
#include <QStandardPaths>
#include <algorithm>

static QString Ui(const wchar_t* text) { return QString::fromWCharArray(docky::ui::Text(text)); }

struct Item {QString id,name;std::wstring target;QImage icon;};

class Applications final:public QWidget {
 PreviewTuning tuning{dock::GlassParameters{}},defaults=tuning;
 TuningDialog* editor=nullptr;
 bool liveEnabled=false;
 QTimer editTimer;
 MenuGlass material; QImage wall,wallBlur,glassFrame,stillFrame; bool dark=false; int hover=-1; QString query;
 QVector<QPair<QRectF,int>> hits;
 double s=1,ox=0,oy=0;
 QLineEdit* search=new QLineEdit(this);
 QPointF pressPoint;
 struct Usage { int count=0; qint64 last=0; };
 QStringList pins;
 QHash<QString,Usage> usage;
 bool catalogReady=false,pinsWritable=true;
 int category=-1,sortOrder=0;
 float chipOffset=0,chipStartOffset=0,chipMax=0;
 bool chipDown=false;
 QVector<QPair<QRectF,int>> chips;
 // Layout in design units. The window is `canvasW` x `canvasH` of them (default 1836 x 1380), drawn at `s` pixels per
 // unit with a fixed offset (ox, oy). Resizing changes the canvas, not the scale: the panel grows, more columns fit
 // (at least 5), and the search field stays centred over the panel.
 qreal canvasW=1836,canvasH=1380;
 static constexpr qreal kDefaultW=1836,kDefaultH=1380,kMinW=1376,kMinH=1380;  // minimum: 5 columns x 4 rows
 QRectF panelRect{95,300,1686,1040};
 QRectF searchRect{418,108,1040,172};
 QPainterPath panelOutline=glassOutline(panelRect,86);
 QPainterPath searchOutline=glassOutline(searchRect,86);
 int columns=7;qreal colPitch=230;
 void relayout(){
  panelRect=QRectF(95,300,canvasW-150,canvasH-340);
  searchRect=QRectF(panelRect.center().x()-520,108,1040,172);
  chipViewport=QRectF(95,326,panelRect.width(),64);
  const qreal grid=panelRect.width()-78;
  columns=std::max(5,int((grid+2)/230));colPitch=(grid+2)/columns;
  panelOutline=glassOutline(panelRect,86);searchOutline=glassOutline(searchRect,86);
  outlineMask=QImage();
 }
 qreal gridRight()const{return panelRect.right()-39;}
 QImage composedFrame,outlineMask;
 QRectF chipViewport{95,326,1686,64};
 static constexpr int rowPitch=218,gridTop=431,featuredGap=36;
 QRectF gridViewport{134,gridTop,1608,4*rowPitch};
 liquid::settings::ScrollMotion scroll;
 QElapsedTimer scrollClock;
 QTimer scrollTimer;
 // The grid scrolls through the whole panel: icons come in at its bottom edge and leave at its top edge, passing
 // under the category tabs, where they are drawn blurred above a divider (macOS scroll-edge effect).
 static constexpr qreal dividerY=404;
 QImage gridLayer;
 // Opening (macOS): the panel springs in (fade + a slight scale-up), then the icons rise in row by row once the first
 // screen of icons has loaded (at most 0.7 s after opening), so loading never shows as icons popping in.
 docky::motion::Spring openMotion,contentMotion;
 QElapsedTimer openClock,animClock;
 QTimer animTimer;
 bool contentStarted=false,opened=false,waitingFirstFrame=false,clearing=false;
 // Edge shades (left, right, bottom): opacity eased toward 0 or 1 at each frame; a timer keeps frames coming until settled.
 float shade[3]{0,0,0};QElapsedTimer shadeClock;QTimer shadeTimer;
 void stepShades(bool left,bool right,bool bottom){
  const bool want[3]{left,right,bottom};
  const float dt=shadeClock.isValid()?std::min(0.05f,shadeClock.nsecsElapsed()/1e9f):0.016f;shadeClock.restart();
  bool moving=false;
  for(int i=0;i<3;++i){const float target=want[i]?1.f:0.f,k=1-std::exp(-dt/0.05f);shade[i]+=(target-shade[i])*k;
   if(std::abs(target-shade[i])<0.004f)shade[i]=target;else moving=true;}
  if(moving&&!shadeTimer.isActive())shadeTimer.start();else if(!moving)shadeTimer.stop();
 }
 qint64 catalogAt=-1;  // ms after opening when the catalog arrived
 bool reduced()const{BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);return !animations;}
 void startOpening(){
  opened=true;contentStarted=false;
  // Critically damped (no bounce), quick: settles in ~0.25 s.
  openMotion.SetSpec(docky::motion::SpringSpec::FromResponse(0.30,1.0));openMotion.SnapTo(0);openMotion.SetTarget(1);
  contentMotion.SetSpec(docky::motion::SpringSpec::FromResponse(0.32,1.0));contentMotion.SnapTo(0);
  if(reduced()){openMotion.SnapTo(1);contentMotion.SnapTo(1);contentStarted=true;update();return;}
  // The clock starts once the first frame is on screen (paintEvent), so a slow first frame cannot eat the start of
  // the animation and make it jump.
  waitingFirstFrame=true;openClock.start();update();
 }
 bool firstScreenLoaded(){
  if(!catalogReady)return false;
  std::lock_guard lock(iconPriorityLock);
  if(visibleIconPriority.isEmpty())return items.isEmpty()||!status.isEmpty();
  for(const auto& id:visibleIconPriority)for(const auto& item:items)if(item.id==id&&item.icon.isNull())return false;
  return true;
 }
 void stepOpening(){
  const double dt=std::min(0.05,animClock.nsecsElapsed()/1.e9);animClock.restart();
  bool moving=openMotion.Step(dt);
  // Reveal when the catalog and the first screen of icons are in; at most 0.45 s after the catalog (slow icons), and
  // regardless after 3 s (a catalog error shows its message).
  const qint64 now=openClock.elapsed();
  if(!contentStarted&&(firstScreenLoaded()||(catalogReady&&catalogAt>=0&&now-catalogAt>450)||now>3000)){contentStarted=true;contentMotion.SetTarget(1);}
  moving|=contentMotion.Step(dt);
  if(!moving&&contentStarted&&openMotion.AtRest()&&contentMotion.AtRest())animTimer.stop();
  update();
 }
 bool hadFocus=false, checkFocusPending=false;
 // Resident (--resident, started by the Dock): the window stays loaded and hidden between openings, so the Windows key
 // shows it at once instead of starting a process (~0.7 s). The Dock toggles it and asks it to quit with these messages.
 bool resident=QCoreApplication::arguments().contains("--resident"),quitting=false;
 const UINT toggleMessage=RegisterWindowMessageW(L"DockOverApplicationsToggle"),quitMessage=RegisterWindowMessageW(L"DockOverApplicationsQuit");
 int menuDepth=0;
 void animateScroll(){
  BOOL animations=TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
  if(!animations){scroll.Step(0,true);scrollTimer.stop();}
  else if(scroll.Animating()&&!scrollTimer.isActive()){scrollClock.start();scrollTimer.start();}
  update();
 }
 void checkFocus(){
  if(!hadFocus||checkFocusPending)return;
  checkFocusPending=true;
  QTimer::singleShot(0,this,[this]{
   checkFocusPending=false;
   const QWidget* active=QApplication::activeWindow();
   if(!isVisible()||menuDepth||active==this||(editor&&active==editor))return;
   close();
  });
 }
 void showMenu(QMenu& menu,const QPoint& point){
  ++menuDepth;
  menu.exec(point);
  --menuDepth;
  checkFocus();
 }
 static constexpr const char* categories[]={"Utilities","Productivity & Finance","Entertainment","Developer Tools","Creativity","Games","Social","Information","Other"};
 static int categoryFor(QString text){
   static const QStringList words[]={
    {"settings","calculator","terminal","explorer","utility","notepad","计算器","设置"},
    {"office","excel","word","powerpoint","outlook","calendar","finance","notion"},
    {"music","video","player","spotify","netflix","vlc"},
    {"visual studio","python","git","sdk","developer","code","cmake","node.js","docker","jetbrains"},
    {"adobe","photoshop","blender","affinity","paint"},
    {"steam","game","xbox","epic"},
    {"discord","teams","slack","wechat","telegram"},
    {"chrome","firefox","edge","browser","news"}};
   for(int i=0;i<8;++i)for(const auto& word:words[i])if(text.contains(word,Qt::CaseInsensitive))return i;
   return 8;
 }
 QString pinsPath()const{return QFileInfo(sharedSettingsPath()).dir().filePath("applications-pins.json");}
 void loadPins(){
   QFile file(pinsPath());if(!file.exists())return;
   if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024){pinsWritable=false;return;}
   const auto root=QJsonDocument::fromJson(file.readAll()).object();
   if(root.value("version").toInt()!=1||!root.value("pins").isArray()){pinsWritable=false;return;}
   for(const auto& value:root.value("pins").toArray())
    if(value.isString()&&!pins.contains(value.toString())&&pins.size()<1000)pins.push_back(value.toString());
   const auto entries=root.value("usage").toObject();
   for(auto it=entries.begin();it!=entries.end();++it){
    const auto value=it.value().toObject();
    usage.insert(it.key(),{std::clamp(value.value("count").toInt(),0,1000000),value.value("last").toInteger()});
   }
 }
 bool savePins(){
   if(!pinsWritable)return false;
   QDir().mkpath(QFileInfo(pinsPath()).absolutePath());QSaveFile file(pinsPath());if(!file.open(QIODevice::WriteOnly))return false;
   QJsonArray list;for(const auto& id:pins)list.append(id);
   QJsonObject history;for(auto it=usage.begin();it!=usage.end();++it)
    history.insert(it.key(),QJsonObject{{"count",it.value().count},{"last",it.value().last}});
   const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"pins",list},{"usage",history}}).toJson();
   return file.write(bytes)==bytes.size()&&file.commit();
 }
 bool togglePin(int index){
   if(index<0||index>=items.size())return false;
   const auto before=pins;
   if(pins.contains(items[index].id))pins.removeAll(items[index].id);else pins.push_back(items[index].id);
   if(!savePins()){pins=before;QMessageBox::warning(this,"DockOver",Ui(L"Could not save pinned apps."));return false;}
   scroll.Reset();update();return true;
 }
 void migrateShortcutKeys(){
   QHash<QString,QString> uniqueNames;
   for(const auto& item:items){
    const auto key=item.name.toCaseFolded();
    if(uniqueNames.contains(key))uniqueNames[key].clear();else uniqueNames.insert(key,item.id);
   }
   auto nativeId=[&](const QString& old){
    if(!old.startsWith("start:"))return old;
    const auto match=uniqueNames.value(QFileInfo(QDir::fromNativeSeparators(old.mid(6))).completeBaseName().toCaseFolded());
    return match.isEmpty()?old:match;
   };
   for(auto& pin:pins)pin=nativeId(pin);
   pins.removeDuplicates();
   const auto previous=usage;
   for(auto it=previous.begin();it!=previous.end();++it){
    const auto id=nativeId(it.key());if(id==it.key())continue;
    auto& entry=usage[id];entry.count=std::min(1000000,entry.count+it.value().count);entry.last=std::max(entry.last,it.value().last);
    usage.remove(it.key());
   }
 }
 QVector<int> featuredItems()const{
   QVector<int> featured;
   if(!query.isEmpty())return featured;
   for(const auto& id:pins)for(int i=0;i<items.size();++i)if(items[i].id==id){featured.push_back(i);break;}
   if(featured.empty()){
    int latest=-1;
    for(int i=0;i<items.size();++i)if(usage.value(items[i].id).last>0&&(latest<0||usage.value(items[i].id).last>usage.value(items[latest].id).last))latest=i;
    if(latest>=0)featured.push_back(latest);
   }
   return featured;
 }
 QVector<int> visibleItems(const QVector<int>& featured)const{
   QVector<int> result;
   for(int i=0;i<items.size();++i)
    if(!featured.contains(i)&&(category<0||categoryFor(items[i].name)==category)&&items[i].name.contains(query,Qt::CaseInsensitive))result.push_back(i);
   if(sortOrder)std::stable_sort(result.begin(),result.end(),[&](int a,int b){
    const auto first=usage.value(items[a].id),second=usage.value(items[b].id);
    if(sortOrder==1&&first.count!=second.count)return first.count>second.count;
    return first.last>second.last;
   });
   return result;
 }
 void layoutSearch(){
    ox=-20*s;oy=-34*s;
    search->setGeometry(qRound(ox+(searchRect.left()+146)*s),qRound(oy+139*s),qRound(738*s),qRound(110*s));
    QFont font("Segoe UI Variable Text");font.setPixelSize(qMax(1,qRound(43*s)));font.setWeight(QFont::Medium);
    font.setLetterSpacing(QFont::AbsoluteSpacing,-1.8*s);search->setFont(font);
 }
 QVector<Item> items;
 QThreadPool discovery;
 std::mutex iconPriorityLock;
 QStringList visibleIconPriority;  // ids of the icons on screen: loaded first
 std::atomic<bool> stopping{false};
 QString status=Ui(L"Loading apps…");
 // The app list from the previous opening (applications-catalog.json next to the pins): reading the Start menu takes
 // ~0.8 s, so the window shows the cached list at once and refreshes it in the background (changes swap in).
 QString catalogPath()const{return QFileInfo(sharedSettingsPath()).dir().filePath("applications-catalog.json");}
 QVector<Item> loadCatalogCache()const{
   QVector<Item> result;QFile file(catalogPath());
   if(!file.open(QIODevice::ReadOnly)||file.size()>8*1024*1024)return result;
   const auto root=QJsonDocument::fromJson(file.readAll()).object();
   if(root.value("version").toInt()!=1)return result;
   for(const auto& value:root.value("apps").toArray()){
    const auto o=value.toObject();Item item;item.id=o.value("id").toString();item.name=o.value("name").toString();item.target=o.value("target").toString().toStdWString();
    if(!item.id.isEmpty()&&!item.target.empty())result.push_back(std::move(item));
   }
   return result;
 }
 static void saveCatalogCache(const QString& path,const QVector<Item>& list){
   QJsonArray apps;for(const auto& item:list)apps.append(QJsonObject{{"id",item.id},{"name",item.name},{"target",QString::fromStdWString(item.target)}});
   QDir().mkpath(QFileInfo(path).absolutePath());QSaveFile file(path);
   if(file.open(QIODevice::WriteOnly)){const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"apps",apps}}).toJson(QJsonDocument::Compact);if(file.write(bytes)==bytes.size())file.commit();}
 }
 std::atomic<int> iconGeneration{0};
 // Loads the icons of `list` (visible ones first) until done or a newer list replaced it; icons go to items by id.
 void loadIcons(QVector<QPair<QString,std::wstring>> list){
   const int generation=++iconGeneration;
   const auto pinnedAtStart=pins;
   discovery.start([this,list=std::move(list),generation,pinnedAtStart]{
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    QVector<int> order;for(int i=0;i<list.size();++i)order.push_back(i);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return pinnedAtStart.contains(list[a].first)>pinnedAtStart.contains(list[b].first);});
    std::vector<bool> requested(list.size(),false);qsizetype cursor=0;
    while(!stopping.load()&&iconGeneration.load()==generation){
     int index=-1;
     {
      std::lock_guard lock(iconPriorityLock);
      for(const auto& id:visibleIconPriority){
       for(int i=0;i<list.size();++i)if(!requested[i]&&list[i].first==id){index=i;break;}
       if(index>=0)break;
      }
     }
     if(index<0){
      while(cursor<order.size()&&requested[order[cursor]])++cursor;
      if(cursor==order.size())break;
      index=order[cursor++];
     }
     requested[index]=true;
     std::vector<uint32_t> pixels;UINT w=0,h=0;
     if(dock::LoadShellIconPixels(list[index].second,128,pixels,w,h)&&w&&h){
      QImage icon=QImage(reinterpret_cast<const uchar*>(pixels.data()),w,h,w*4,QImage::Format_ARGB32_Premultiplied).copy();
      QMetaObject::invokeMethod(this,[this,id=list[index].first,icon=std::move(icon)]{for(auto& item:items)if(item.id==id){item.icon=icon;break;}update();},Qt::QueuedConnection);
     }
    }
    if(SUCCEEDED(co))CoUninitialize();
   });
 }
 static QVector<QPair<QString,std::wstring>> iconJobs(const QVector<Item>& list){
   QVector<QPair<QString,std::wstring>> jobs;for(const auto& item:list)if(item.icon.isNull())jobs.push_back({item.id,item.target});return jobs;
 }
 void loadApps(){
    const auto cached=loadCatalogCache();
    if(!cached.isEmpty()){items=cached;migrateShortcutKeys();catalogReady=true;catalogAt=0;loadIcons(iconJobs(items));}
    refreshCatalog(!cached.isEmpty());
 }
 // Reads the Start menu again (off the UI thread) and swaps in a changed list; also used each time the resident window
 // is shown, so apps installed meanwhile appear.
 void refreshCatalog(bool haveList){
    const QString cachePath=catalogPath();
    discovery.start([this,cachePath,hadCache=haveList]{
      const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);std::vector<dock::apps::AppEntry> apps;std::wstring error;
      if(dock::apps::DiscoverStartMenuApps(apps,error)){
       QVector<Item> metadata;for(const auto& app:apps){Item item;item.id=QString::fromStdString(app.id);item.name=QString::fromStdWString(app.name);item.target=app.Target();metadata.push_back(std::move(item));}
       saveCatalogCache(cachePath,metadata);
       QMetaObject::invokeMethod(this,[this,part=std::move(metadata),hadCache]() mutable{
        // Same list as the cache: nothing changes on screen. Otherwise swap it in, keeping the icons already loaded.
        bool same=hadCache&&part.size()==items.size();
        for(int i=0;same&&i<part.size();++i)same=part[i].id==items[i].id&&part[i].name==items[i].name;
        if(same)return;
        for(auto& item:part)for(const auto& old:items)if(old.id==item.id){item.icon=old.icon;break;}
        items=std::move(part);migrateShortcutKeys();
        if(!catalogReady){catalogReady=true;catalogAt=openClock.isValid()?openClock.elapsed():0;}
        status=pinsWritable?QString():QString("Could not read pinned apps. The existing file was preserved.");
        hover=-1;loadIcons(iconJobs(items));update();
       },Qt::QueuedConnection);
      }else QMetaObject::invokeMethod(this,[this,error]{if(!catalogReady){catalogReady=true;status=QString::fromStdWString(error);}update();},Qt::QueuedConnection);
      if(SUCCEEDED(co))CoUninitialize();
    });
 }
 void launch(int i){
    if(i<0||i>=items.size())return;
    SHELLEXECUTEINFOW info{sizeof(info)};info.hwnd=reinterpret_cast<HWND>(winId());info.lpFile=items[i].target.c_str();
    info.fMask=SEE_MASK_NOASYNC;info.nShow=SW_SHOWNORMAL;
    if(ShellExecuteExW(&info)){
      auto& entry=usage[items[i].id];entry.count=std::min(1000000,entry.count+1);entry.last=QDateTime::currentMSecsSinceEpoch();savePins();close();
    }
    else QMessageBox::warning(this,"DockOver",Ui(L"Could not open %1").arg(items[i].name));
 }
 QPointF design(QPoint p)const{return {(p.x()-ox)/s,(p.y()-oy)/s};}
 static QPainterPath continuous(QRectF r,qreal extent,qreal exponent=3.05){
   const qreal corner=std::min({extent,r.width()/2,r.height()/2});
   if(corner<=0){QPainterPath rectangle;rectangle.addRect(r);return rectangle;}
   if(exponent==2){QPainterPath rounded;rounded.addRoundedRect(r,corner,corner);return rounded;}
   constexpr qreal pi=3.14159265358979323846;
   constexpr int samples=64;
   const QPointF centers[]={{r.right()-corner,r.top()+corner},{r.right()-corner,r.bottom()-corner},
                            {r.left()+corner,r.bottom()-corner},{r.left()+corner,r.top()+corner}};
   auto power=[exponent](qreal value){return std::copysign(std::pow(std::abs(value),2./exponent),value);};
   QPainterPath path;path.moveTo(r.left()+corner,r.top());
   for(int index=0;index<4;++index){
    for(int j=0;j<=samples;++j){
     const qreal angle=(index-1+j/qreal(samples))*pi/2;
     path.lineTo(centers[index]+QPointF(corner*power(std::cos(angle)),corner*power(std::sin(angle))));
    }
   }
   path.closeSubpath();return path;
 }
 static QPainterPath glassOutline(QRectF rect,qreal radius){
   // Match the shared shader's Lp corner including its blend to circular capsule ends.
   const qreal half=std::min(rect.width(),rect.height())/2;
   const qreal r=std::clamp(radius,0.,half);
   const qreal capsule=std::clamp((r/half-.85)/.15,0.,1.);
   return continuous(rect,r*std::lerp(qreal(dock::kGlassCornerExtent),1.,capsule),
                     std::lerp(qreal(dock::kGlassCornerExponent),2.,capsule));
 }
 void round(QPainter& p,QRectF r,qreal radius,QColor fill,QColor edge=Qt::transparent){p.setPen(edge.alpha()?QPen(edge,1.0):Qt::NoPen);p.setBrush(fill);p.drawPath(continuous(r,radius));}
 void prepareOutlineMask(){
   const qreal dpr=devicePixelRatioF();
   const QSize pixels(qCeil(width()*dpr),qCeil(height()*dpr));
   if(outlineMask.size()==pixels&&qFuzzyCompare(outlineMask.devicePixelRatio(),dpr))return;
   outlineMask=QImage(pixels,QImage::Format_ARGB32_Premultiplied);
   composedFrame=QImage(pixels,QImage::Format_ARGB32_Premultiplied);
   outlineMask.setDevicePixelRatio(dpr);composedFrame.setDevicePixelRatio(dpr);
   outlineMask.fill(Qt::transparent);
   QPainter mask(&outlineMask);
   mask.setRenderHint(QPainter::Antialiasing);
   mask.translate(ox,oy);mask.scale(s,s);
   mask.fillPath(panelOutline,Qt::white);
   mask.fillPath(searchOutline,Qt::white);
 }
 void glass(QPainter& p,const QRectF& r,const QPainterPath& shape,bool search){
   p.save();p.setClipRect(r);
   if(glassFrame.isNull()&&!dark&&!wallBlur.isNull())p.drawImage(QRectF(0,0,canvasW,canvasH),wallBlur);
   QLinearGradient shade(r.topLeft(),r.bottomLeft());
   if(search){
       shade.setColorAt(0,QColor::fromRgbF(0,0,0,tuning.top));
       shade.setColorAt(tuning.reach,QColor::fromRgbF(0,0,0,tuning.top*.99));
       shade.setColorAt(1,QColor::fromRgbF(0,0,0,tuning.bottom));
   }
   else if(dark){shade.setColorAt(0,QColor(13,14,15,250));shade.setColorAt(1,QColor(23,23,24,248));}
   else{shade.setColorAt(0,QColor(9,20,29,145));shade.setColorAt(.52,QColor(15,25,32,95));shade.setColorAt(1,QColor(31,27,29,80));}
   p.fillRect(r,shade);p.restore();p.setBrush(Qt::NoBrush);p.setPen(QPen(QColor(255,255,255,search?32:42),1.2));p.drawPath(shape);
 }

 void drawTile(QPainter& p,const Item& item,int index,qreal x,qreal y){
   const QRectF icon(x+2,y,118,118);
   if(item.icon.isNull()){
    QFont initial("Segoe UI Variable Text");initial.setPixelSize(38);p.setFont(initial);
    p.setPen(QColor(231,234,240));p.drawText(icon,Qt::AlignCenter,item.name.left(1));
   }else{
    const QSizeF size=QSizeF(item.icon.size()).scaled(icon.size(),Qt::KeepAspectRatio);
    p.drawImage(QRectF(icon.center()-QPointF(size.width()/2,size.height()/2),size),item.icon);
   }
   const QRectF label(x-44,y+127,210,89);
   QFont font("Segoe UI Variable Text");font.setPixelSize(24);font.setWeight(QFont::Medium);
   QTextLayout layout(item.name,font);QTextOption option;option.setAlignment(Qt::AlignHCenter);
   option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
   layout.beginLayout();
   qreal height=0;
   QString lastLine;
   for(int lineIndex=0;lineIndex<3;++lineIndex){
    auto line=layout.createLine();if(!line.isValid())break;
    line.setLineWidth(label.width());
    if(lineIndex==2&&line.textStart()+line.textLength()<item.name.size()){
     lastLine=QFontMetrics(font).elidedText(item.name.mid(line.textStart()),Qt::ElideRight,qFloor(label.width()));
     line.setPosition(QPointF(0,label.height()+50));
     break;
    }
    line.setPosition(QPointF(0,height));height+=line.height();
   }
   layout.endLayout();
   p.save();p.setClipRect(label,Qt::IntersectClip);p.setPen(Qt::white);layout.draw(&p,label.topLeft());
   if(!lastLine.isEmpty()){p.setFont(font);p.drawText(QRectF(label.left(),label.top()+height,label.width(),32),Qt::AlignTop|Qt::AlignHCenter,lastLine);}
   p.restore();
   if(hover==index&&hasFocus()){p.setPen(Qt::white);p.drawLine(QPointF(icon.center().x()-9,y+123),QPointF(icon.center().x()+9,y+123));}
   const QRectF hit=QRectF(label.left(),y,label.width(),rowPitch).intersected(gridViewport);
   if(!hit.isEmpty())hits.push_back({hit,index});
 }
 int uiPercent=-1;
 QString sharedSettingsPath()const{const auto args=QCoreApplication::arguments();const int i=args.indexOf("--settings-path");return i>=0&&i+1<args.size()?args[i+1]:QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("LiquidDesktop/settings.json");}
 // The window size the user dragged to (design units), next to the pins; Settings > Appearance resets it (deletes it).
 QString windowSizePath()const{return QFileInfo(sharedSettingsPath()).dir().filePath("applications-window.json");}
 bool hadSizeFile=false;
 void loadWindowSize(){
  canvasW=kDefaultW;canvasH=kDefaultH;QFile file(windowSizePath());hadSizeFile=file.exists();
  if(!file.open(QIODevice::ReadOnly)||file.size()>4096)return;
  const auto root=QJsonDocument::fromJson(file.readAll()).object();
  if(root.value("version").toInt()!=1)return;
  const qreal w=root.value("width").toDouble(),h=root.value("height").toDouble();
  if(w>=kMinW&&w<=20000&&h>=kMinH&&h<=20000){canvasW=w;canvasH=h;}
 }
 void saveWindowSize(){
  QDir().mkpath(QFileInfo(windowSizePath()).absolutePath());QSaveFile file(windowSizePath());
  if(file.open(QIODevice::WriteOnly)){const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"width",qRound(canvasW)},{"height",qRound(canvasH)}}).toJson(QJsonDocument::Compact);if(file.write(bytes)==bytes.size()&&file.commit())hadSizeFile=true;}
 }
 // Largest window (logical px) centred on the screen whose visible part (search bar to panel bottom: 74 units in from
 // the top and bottom, 75 from the sides) stays inside the work area: below the menu bar, above the Dock.
 QSizeF maxWindow(QScreen* screen)const{
  const QRect g=screen->geometry(),work=screen->availableGeometry();const QPointF c=QRectF(g).center();
  const qreal w=2*std::min(c.x()-work.left(),work.right()+1-c.x())+2*75*s,h=2*std::min(c.y()-work.top(),work.bottom()+1-c.y())+2*74*s;
  return {w,h};
 }
 QScreen* currentScreen()const{auto screen=QGuiApplication::screenAt(isVisible()?frameGeometry().center():QCursor::pos());return screen?screen:QGuiApplication::primaryScreen();}
 void applyGeometry(){
  auto screen=currentScreen();const QRect g=screen->geometry(),work=screen->availableGeometry();
  // Settings' Applications size sets the scale (100 % = the former 1380-wide window); a screen too small for the
  // minimum 5 x 4 layout lowers it.
  s=0.75163*uiPercent/100.;
  {const QPointF c=QRectF(g).center();const qreal room=std::min(2*std::min(c.x()-work.left(),work.right()+1-c.x())/(kMinW-150.),2*std::min(c.y()-work.top(),work.bottom()+1-c.y())/(kMinH-148.));s=std::max(0.2,std::min(s,room));}
  const QSizeF most=maxWindow(screen);
  canvasW=std::clamp(canvasW,kMinW,std::max(kMinW,most.width()/s));canvasH=std::clamp(canvasH,kMinH,std::max(kMinH,most.height()/s));
  relayout();
  const QSize size(qRound(canvasW*s),qRound(canvasH*s));
  setGeometry(QRect(QPoint(qRound(QRectF(g).center().x()-size.width()/2.),qRound(QRectF(g).center().y()-size.height()/2.)),size));  // always centred
  layoutSearch();
  if(isVisible()&&!resizing)redrawMaterial();  // while dragging, the 50 ms live refresh follows the new size
  update();
 }
 void syncUiSize(){
  int percent=80,order=0;QFile file(sharedSettingsPath());if(file.open(QIODevice::ReadOnly)){const auto root=QJsonDocument::fromJson(file.readAll()).object();const auto appearance=root.value("appearance").toObject();const auto value=appearance.value("applicationsScalePercent");order=appearance.value("applicationsSort").toInt();docky::ui::SetLanguage(appearance.value("language").toString().toStdString());if(root.value("schemaVersion").toInt()==1&&value.isDouble()&&value.toDouble()==value.toInt()&&value.toInt()>=60&&value.toInt()<=120)percent=value.toInt();}
  if(order>=0&&order<=2&&order!=sortOrder){sortOrder=order;scroll.Reset();hover=-1;update();}
  const bool sizeFile=QFile::exists(windowSizePath());
  const bool reset=hadSizeFile&&!sizeFile&&!resizing;  // Settings > Reset size
  if(uiPercent==percent&&!reset)return;
  if(reset){hadSizeFile=false;canvasW=kDefaultW;canvasH=kDefaultH;}
  uiPercent=percent;applyGeometry();
 }
 // Edge resizing (macOS window): drag any edge or corner of the panel; the window grows on both sides so it stays
 // centred on the screen. Saved on release.
 int resizeEdges=0;bool resizing=false;QPointF resizeGlobal;qreal resizeStartW=0,resizeStartH=0;
 int edgesAt(QPointF pt)const{
  const qreal m=16;const QRectF r=panelRect;int e=0;
  // Corners: along the inside of the rounded corner (its outside is transparent, so Windows sends those clicks to the
  // window below). R approximates the continuous corner's radius.
  {const qreal R=80,band=26;
   const QPointF centres[4]={{r.left()+R,r.top()+R},{r.right()-R,r.top()+R},{r.left()+R,r.bottom()-R},{r.right()-R,r.bottom()-R}};
   const int bits[4]={1|4,2|4,1|8,2|8};
   for(int i=0;i<4;++i){const bool beyondX=i%2?pt.x()>centres[i].x():pt.x()<centres[i].x(),beyondY=i<2?pt.y()<centres[i].y():pt.y()>centres[i].y();
    if(beyondX&&beyondY){const qreal d=std::hypot(pt.x()-centres[i].x(),pt.y()-centres[i].y());if(d>R-band&&d<R+m)return bits[i];}}}
  const bool inY=pt.y()>r.top()-m&&pt.y()<r.bottom()+m,inX=pt.x()>r.left()-m&&pt.x()<r.right()+m;
  if(inY&&std::abs(pt.x()-r.left())<m)e|=1;if(inY&&std::abs(pt.x()-r.right())<m)e|=2;
  if(inX&&std::abs(pt.y()-r.top())<m)e|=4;if(inX&&std::abs(pt.y()-r.bottom())<m)e|=8;
  return e;
 }
 static Qt::CursorShape cursorFor(int e){
  if((e&1&&e&4)||(e&2&&e&8))return Qt::SizeFDiagCursor;if((e&2&&e&4)||(e&1&&e&8))return Qt::SizeBDiagCursor;
  if(e&3)return Qt::SizeHorCursor;if(e&12)return Qt::SizeVerCursor;return Qt::ArrowCursor;
 }
 inline static Applications* hookOwner=nullptr;
 HHOOK mouseHook=nullptr;
 static LRESULT CALLBACK MouseHook(int code,WPARAM message,LPARAM data){
  if(code==HC_ACTION&&hookOwner&&(message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN)){
   const POINT pt=reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt;
   QMetaObject::invokeMethod(hookOwner,[pt]{if(hookOwner)hookOwner->clickedAt(pt);},Qt::QueuedConnection);
  }
  return CallNextHookEx(nullptr,code,message,data);
 }
 void watchClicks(bool on){
  if(on&&!mouseHook){hookOwner=this;mouseHook=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);}
  else if(!on&&mouseHook){UnhookWindowsHookEx(mouseHook);mouseHook=nullptr;}
 }
 // A click anywhere outside the search bar and the panel closes the window (macOS); not while one of its menus, the
 // tuning dialog or a resize is in use.
 void clickedAt(POINT screen){
  if(!isVisible()||menuDepth||resizing||(editor&&editor->isVisible()))return;
  const qreal dpr=devicePixelRatioF();
  const QPointF pt=design(mapFromGlobal(QPoint(qRound(screen.x/dpr),qRound(screen.y/dpr))));
  if(!panelOutline.contains(pt)&&!searchOutline.contains(pt))close();
 }
 bool nativeEvent(const QByteArray& type,void* message,qintptr* result)override{
  const auto* m=static_cast<MSG*>(message);
  if(m->message==toggleMessage&&toggleMessage){if(isVisible())close();else present();*result=0;return true;}
  if(m->message==quitMessage&&quitMessage){quitting=true;QCoreApplication::quit();*result=0;return true;}
  return QWidget::nativeEvent(type,message,result);
 }
public:
 // Shows the resident window as if just opened: reset to the top of all apps, centred on the screen under the pointer,
 // glass of what is behind it now, then the opening animation (icons are already in memory).
 void present(){
  search->clear();query.clear();category=-1;chipOffset=0;scroll.Reset();hover=-1;contentStarted=false;
  uiPercent=-1;syncUiSize();
  // A live frame of what is behind it now before it appears: the capture may need a moment to start (up to ~0.12 s);
  // without it the glass would show the picture from when the window was loaded (often the bare desktop).
  if(liveEnabled){
   QElapsedTimer wait;wait.start();
   do{material.forceNext=true;if(refreshDesktop(true))break;Sleep(8);}while(wait.elapsed()<120);
  }
  refreshCatalog(true);
  show();raise();activateWindow();SetForegroundWindow(reinterpret_cast<HWND>(winId()));setFocus();
 }
public:
 ~Applications() override {watchClicks(false);hookOwner=nullptr;stopping=true;discovery.waitForDone();}
 Applications(){setWindowTitle("DockOver Applications");setWindowFlags(Qt::FramelessWindowHint|Qt::Tool);
  setAttribute(Qt::WA_TranslucentBackground);auto screen=QGuiApplication::screenAt(QCursor::pos());if(!screen)screen=QGuiApplication::primaryScreen();
  loadWindowSize();syncUiSize();loadPins();
  scrollTimer.setTimerType(Qt::PreciseTimer);scrollTimer.setInterval(8);
  animTimer.setTimerType(Qt::PreciseTimer);animTimer.setInterval(8);
  shadeTimer.setTimerType(Qt::PreciseTimer);shadeTimer.setInterval(8);connect(&shadeTimer,&QTimer::timeout,this,[this]{update();});
  connect(&animTimer,&QTimer::timeout,this,[this]{stepOpening();});
  connect(&scrollTimer,&QTimer::timeout,this,[this]{
   const double dt=scrollClock.nsecsElapsed()/1.e9;scrollClock.restart();
   if(!scroll.Step(dt,false))scrollTimer.stop();
   update();
  });
  connect(qApp,&QApplication::focusChanged,this,[this](QWidget*,QWidget* now){
   if(now&&now->window()==this)hadFocus=true;
   else checkFocus();
  });
  auto sizePoll=new QTimer(this);connect(sizePoll,&QTimer::timeout,this,[this]{syncUiSize();});sizePoll->start(300);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
  search->setFrame(false);search->installEventFilter(this);
  connect(search,&QLineEdit::returnPressed,this,[this]{if(!hits.isEmpty())launch(hits.front().second);});
  search->setPlaceholderText(Ui(L"Applications"));search->setAccessibleName(Ui(L"Search apps"));
  search->setStyleSheet("QLineEdit{background:transparent;border:0;padding:0;color:white;selection-background-color:#3566a3;}");
  auto palette=search->palette();palette.setColor(QPalette::PlaceholderText,QColor(237,235,231,160));search->setPalette(palette);
  connect(search,&QLineEdit::textChanged,this,[this](const QString& text){query=text;category=-1;scroll.Reset();hover=-1;update();});
  layoutSearch();
  QImage raw;if(!QCoreApplication::arguments().contains("--dev-static"))raw=screen->grabWindow(0,x()-screen->geometry().left(),y()-screen->geometry().top(),width(),height()).toImage();
  if(raw.isNull()){raw=QImage(qRound(canvasW),qRound(canvasH),QImage::Format_RGB32);raw.fill(QColor("#242931"));}
  loadApps();
  wall=raw.scaled(qRound(canvasW),qRound(canvasH),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);wallBlur=wall.scaled(85,64,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).scaled(qRound(canvasW),qRound(canvasH),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
  const bool ready=material.init();
  defaults=PreviewTuning(dock::RoleGlass(dock::GlassRole::Menu));
  previewSettings(defaults,false,QCoreApplication::applicationDirPath()+"/applications-defaults.ini");
  tuning=defaults;
  previewSettings(tuning,false);
  applySurfaces();
  if(ready) stillFrame=material.still(wall.scaled(qRound(canvasW),qRound(canvasH)),panelRect,searchRect);
  editTimer.setSingleShot(true);
  connect(&editTimer,&QTimer::timeout,this,&Applications::redrawMaterial);
  glassFrame=stillFrame;
  if(!QCoreApplication::arguments().contains("--dev-static")&&ready && SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()),WDA_EXCLUDEFROMCAPTURE)){
    liveEnabled=true;
    auto timer=new QTimer(this);
    connect(timer,&QTimer::timeout,this,[this]{refreshDesktop();});
    timer->start(16);  // only renders when what is behind changed (DXGI dirty rectangles)
  } }
 void applySurfaces(){
    std::copy(std::begin(tuning.surfaces),std::end(tuning.surfaces),material.surfaces);
 }
 void redrawMaterial(){
    applySurfaces();
    if(liveEnabled && !dark){
        material.forceNext=true;
        refreshDesktop();
        if(!material.forceNext) return;
    }
    QImage backdrop=wall;
    if(dark){backdrop=QImage(wall.size(),QImage::Format_RGB32);backdrop.fill(Qt::black);}
    glassFrame=material.still(backdrop.scaled(qRound(canvasW),qRound(canvasH)),panelRect,searchRect);
    update();
 }
 TuningDialog* openEditor(){
    if(!editor){
        editor=new TuningDialog(this,tuning,defaults,[this]{if(!editTimer.isActive()) editTimer.start(16);update();});
        const QRect work=screen()->availableGeometry();
        editor->move(work.right()-editor->width()-12,work.top()+48);
    }
    editor->show();editor->raise();editor->activateWindow();return editor;
 }
 bool checkCatalog(){
    auto* poll=new QTimer(this);
    connect(poll,&QTimer::timeout,this,[this,poll]{
        if(!catalogReady)return;
        poll->stop();grab();
        bool ok=items.size()>=2&&!hits.empty();
        if(ok){
            const auto previousPins=pins;
            const auto previousUsage=usage;
            pins.clear();usage.clear();
            usage.insert(items[0].id,{5,100});
            usage.insert(items[1].id,{2,200});
            ok=featuredItems()==QVector<int>{1};
            sortOrder=1;ok=ok&&visibleItems({}).value(0,-1)==0;
            sortOrder=2;ok=ok&&visibleItems({}).value(0,-1)==1;
            ok=togglePin(0)&&ok&&featuredItems()==QVector<int>{0};
            pins.clear();usage.clear();loadPins();
            ok=ok&&pins==QStringList{items[0].id}&&usage.value(items[1].id).last==200;
            pins=previousPins;usage=previousUsage;ok=savePins()&&ok;
        }
        QCoreApplication::exit(ok?0:3);
    });
    poll->start(150);QTimer::singleShot(30000,this,[]{QCoreApplication::exit(4);});return true;
 }
 bool refreshDesktop(bool beforeShow=false) {
    if(dark || isMinimized() || (!isVisible()&&!beforeShow)) return false;
    HWND hwnd=reinterpret_cast<HWND>(winId());
    RECT client{};GetClientRect(hwnd,&client);
    POINT origin{};ClientToScreen(hwnd,&origin);
    const double dpi=devicePixelRatioF(),scale=s*dpi;  // physical px per design unit
    const LONG w=qRound(canvasW*scale),h=qRound(canvasH*scale);
    const LONG x=origin.x-qRound(20*scale),y=origin.y-qRound(34*scale);
    const double layout=s;  // DIP per design unit
    auto dip=[&](const QRectF& r){return QRectF(r.left()*layout,r.top()*layout,r.width()*layout,r.height()*layout);};
    QImage frame=material.live(hwnd,{x,y,x+w,y+h},dip(panelRect),dip(searchRect),dpi,86*layout);
    if(frame.isNull()) return false;
    glassFrame=std::move(frame);
    wallBlur=glassFrame.scaled(85,64,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    update();
    return true;
 }
protected:
 void closeEvent(QCloseEvent* event) override {
  if(resident&&!quitting){  // resident (started by the Dock): hide, ready to show again at once
   // An empty frame first, so the next showing cannot flash this one before its own first frame.
   event->ignore();clearing=true;repaint();clearing=false;hide();hadFocus=false;opened=false;waitingFirstFrame=false;animTimer.stop();scrollTimer.stop();setCursor(Qt::ArrowCursor);
   return;
  }
  QWidget::closeEvent(event);
  if(event->isAccepted())QCoreApplication::quit();
 }
 bool eventFilter(QObject* object,QEvent* event)override{if(object==search&&event->type()==QEvent::KeyPress){auto* key=static_cast<QKeyEvent*>(event);if(key->key()==Qt::Key_Down||(key->key()==Qt::Key_Escape&&query.isEmpty())){setFocus(Qt::TabFocusReason);keyPressEvent(key);return true;}}return QWidget::eventFilter(object,event);}
 void resizeEvent(QResizeEvent* e) override {QWidget::resizeEvent(e);layoutSearch();}
 void showEvent(QShowEvent* e) override {QWidget::showEvent(e);watchClicks(true);if(!opened)startOpening();}
 void hideEvent(QHideEvent* e) override {QWidget::hideEvent(e);watchClicks(false);}
 // Development: DOCKOVER_APPS_PERF=<file> appends one line per frame (paint ms, opening and reveal progress).
 QFile* perfLog=nullptr;QElapsedTimer perfClock;
 void perfLine(double ms){
  if(!perfLog){const QString path=qEnvironmentVariable("DOCKOVER_APPS_PERF");if(path.isEmpty())return;perfLog=new QFile(path,this);if(!perfLog->open(QIODevice::Append|QIODevice::Text)){perfLog=nullptr;return;}perfClock.start();}
  perfLog->write(QString("%1 paint=%2 open=%3 reveal=%4\n").arg(perfClock.elapsed()).arg(ms,0,'f',2).arg(openMotion.F(),0,'f',3).arg(contentMotion.F(),0,'f',3).toUtf8());perfLog->flush();
 }
 void paintEvent(QPaintEvent*) override {
  if(clearing){QPainter clear(this);clear.setCompositionMode(QPainter::CompositionMode_Source);clear.fillRect(rect(),Qt::transparent);return;}
  QElapsedTimer paintTimer;paintTimer.start();
  ox=-20*s;oy=-34*s;
  prepareOutlineMask();
  // Render the material and all its content together. Clip their shared edge once,
  // after composition, so scrolling text cannot change the visible window silhouette.
  composedFrame.fill(Qt::black);
  QPainter p(&composedFrame);
  p.setRenderHints(QPainter::Antialiasing|QPainter::SmoothPixmapTransform);
  p.translate(ox,oy);p.scale(s,s);hits.clear();
  p.drawImage(QRectF(0,0,canvasW,canvasH),glassFrame.isNull()?wall:glassFrame);
  glass(p,searchRect,searchOutline,true);
  p.save();
  p.translate(searchRect.left()-418,0);  // icon and dots were drawn for the default search bar at x 418
  p.setPen(QPen(QColor(242,237,226,160),7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  p.drawLine(QPointF(491,213),QPointF(493,210));
  p.drawLine(QPointF(498,201),QPointF(515,173));
  p.drawLine(QPointF(505,173),QPointF(529,213));
  p.drawLine(QPointF(486,202),QPointF(533,202));
  p.setPen(QColor(225,225,225,140));p.setBrush(QColor(225,225,225,140));for(int i=0;i<3;++i)p.drawEllipse(QPointF(1352+i*14,193),4,4);
  p.restore();
  if(glassFrame.isNull())glass(p,panelRect,panelOutline,false);
  p.save();p.setClipRect(panelRect);
  const auto featured=featuredItems(),visible=visibleItems(featured);
  const int featuredRows=(featured.size()+columns-1)/columns;
  const int rows=(visible.size()+columns-1)/columns;
  const int gap=featuredRows&&rows?featuredGap:0;
  // Sharp (and clickable) from the divider to the panel's bottom edge; the last row may stop 24 above that edge.
  gridViewport=QRectF(134,dividerY,gridRight()-134,panelRect.bottom()-dividerY);
  const qreal visibleHeight=panelRect.bottom()-24-gridTop;
  const float maxScroll=std::max(0.f,float((featuredRows+rows)*rowPitch+gap-visibleHeight));
  scroll.Bounds(maxScroll);
  const qreal position=scroll.Position();
  // Content reveal: rows rise 36 and fade in one after another (row k starts 0.05 later).
  const qreal reveal=std::clamp<qreal>(contentMotion.F(),0,1.2);
  {
   const qreal dpr=composedFrame.devicePixelRatio();
   if(gridLayer.size()!=composedFrame.size()){gridLayer=QImage(composedFrame.size(),QImage::Format_ARGB32_Premultiplied);gridLayer.setDevicePixelRatio(dpr);}
   gridLayer.fill(Qt::transparent);
   QPainter g(&gridLayer);
   g.setRenderHints(QPainter::Antialiasing|QPainter::SmoothPixmapTransform);
   g.translate(ox,oy);g.scale(s,s);g.setClipRect(panelRect);
   int screenRow=0;
   auto grid=[&](const QVector<int>& list,int offset){
    const qreal top=gridTop+offset-position;
    const int firstRow=std::max(0,qFloor((panelRect.top()-top)/rowPitch));
    const int last=qCeil((panelRect.bottom()-top)/rowPitch)*columns;
    int rowSeen=-1;
    for(int at=firstRow*columns;at<list.size()&&at<last;++at){
     const int row=at/columns;if(row!=rowSeen){rowSeen=row;++screenRow;}
     const qreal t=std::clamp<qreal>(reveal*1.2-0.035*(screenRow-1),0,1),ease=1-(1-t)*(1-t)*(1-t);
     g.save();g.setOpacity(t);
     drawTile(g,items[list[at]],list[at],134+colPitch*((at%columns)+0.5)-61,top+row*rowPitch+24*(1-ease));
     g.restore();
    }
   };
   grid(featured,0);
   if(gap){  // midway between the pinned row's labels (two lines end ~189 below its top) and the next row's icons
    const qreal lastPinned=gridTop+(featuredRows-1)*rowPitch-position,y=(lastPinned+189+lastPinned+rowPitch+gap)/2;
    g.setOpacity(std::clamp<qreal>(reveal,0,1));g.setPen(QPen(QColor(255,255,255,60),2));g.drawLine(QPointF(134,y),QPointF(gridRight(),y));g.setOpacity(1);
   }
   grid(visible,featuredRows*rowPitch+gap);
   if(featured.empty()&&visible.empty()){
    // "Loading apps…" only when loading is slow (after 0.9 s); a message after the reveal at full strength.
    g.setOpacity(contentStarted?1.0:std::clamp<qreal>((openClock.isValid()?openClock.elapsed()-900:0)/300.0,0,1));
    QFont empty("Segoe UI Variable Text");empty.setPixelSize(28);empty.setWeight(QFont::Medium);g.setFont(empty);g.setPen(QColor(255,255,255,190));
    g.drawText(QRectF(194,510,gridRight()-254,160),Qt::AlignCenter,!catalogReady?"Loading apps…":!status.isEmpty()?status:"No matching apps.");
   }
  }
  {
   std::lock_guard lock(iconPriorityLock);
   visibleIconPriority.clear();
   for(const auto& hit:hits)if(hit.second>=0&&hit.second<items.size())visibleIconPriority.push_back(items[hit.second].id);
  }
  // Composite: sharp below the divider; above it a blurred, slightly darkened copy (icons sliding under the tabs).
  {
   p.save();p.resetTransform();
   const qreal dpr=gridLayer.devicePixelRatio();
   const int dividerPx=qRound((oy+dividerY*s)*dpr),topPx=std::max(0,qFloor((oy+panelRect.top()*s)*dpr));
   p.setClipRect(QRectF(0,dividerPx/dpr,width(),height()));
   p.drawImage(QPointF(0,0),gridLayer);
   if(dividerPx>topPx){
    // Blur with margin below the divider, so the strip's edge does not pull in transparent pixels.
    const int margin=qRound(40*s*dpr),h=dividerPx-topPx+margin;
    QImage strip=gridLayer.copy(0,topPx,gridLayer.width(),std::min(h,gridLayer.height()-topPx));
    strip.setDevicePixelRatio(1);
    const QSize reduced(std::max(1,strip.width()/12),std::max(1,strip.height()/12));
    QImage blurred=strip.scaled(reduced,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).scaled(strip.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    blurred.setDevicePixelRatio(dpr);
    p.setClipRect(QRectF(0,topPx/dpr,width(),(dividerPx-topPx)/dpr));
    p.setOpacity(0.85);p.drawImage(QPointF(0,topPx/dpr),blurred);p.setOpacity(1);
   }
   p.restore();
  }
  // Tabs over the blurred band, then the divider.
  chips.clear();
  p.save();p.setClipRect(chipViewport,Qt::IntersectClip);
  QFont font("Segoe UI Variable Text");font.setPixelSize(30);font.setWeight(QFont::Medium);p.setFont(font);
  float chipX=134-chipOffset;
  for(int index=0;index<9;++index){
   const QString name=QString::fromUtf8(categories[index]);const float width=p.fontMetrics().horizontalAdvance(name)+36;
   const QRectF chip(chipX,334,width,48);round(p,chip,24,QColor(255,255,255,category==index?48:19));
   p.setPen(category==index?QColor(Qt::white):QColor(225,232,239,205));p.drawText(chip,Qt::AlignCenter,name);
   chips.push_back({chip.intersected(chipViewport),index});chipX+=width+16;
  }
  chipMax=std::max(0.f,float(chipX+chipOffset-gridRight()));
  chipOffset=std::clamp(chipOffset,0.f,chipMax);
  // Tabs cut off at an edge: a light black fade there, so it reads as "more this way".
  p.restore();
  // Tabs cut off at a side: a light black fade over the whole band above the divider on that side, so it reads as
  // "more this way" (the panel outline clips its corner).
  auto fade=[&](bool left){
   const qreal w=110,x0=left?panelRect.left():panelRect.right()-w;
   QLinearGradient g(QPointF(left?x0:x0+w,0),QPointF(left?x0+w:x0,0));
   g.setColorAt(0,QColor(0,0,0,105));g.setColorAt(0.55,QColor(0,0,0,38));g.setColorAt(1,QColor(0,0,0,0));
   p.fillRect(QRectF(x0,panelRect.top(),w,dividerY-panelRect.top()),g);
  };
  // The shades fade in and out (~0.15 s) as content appears or disappears beyond an edge.
  stepShades(chipOffset>0.5f,chipOffset<chipMax-0.5f,scroll.Position()<maxScroll-0.5f);
  if(shade[0]>0.003f){p.setOpacity(shade[0]);fade(true);}
  if(shade[1]>0.003f){p.setOpacity(shade[1]);fade(false);}
  // Same at the bottom edge while more apps are below.
  if(shade[2]>0.003f){
   p.setOpacity(shade[2]);
   const qreal h=110;QLinearGradient g(QPointF(0,panelRect.bottom()),QPointF(0,panelRect.bottom()-h));
   g.setColorAt(0,QColor(0,0,0,105));g.setColorAt(0.55,QColor(0,0,0,38));g.setColorAt(1,QColor(0,0,0,0));
   p.fillRect(QRectF(panelRect.left(),panelRect.bottom()-h,panelRect.width(),h),g);
  }
  p.setOpacity(1);
  p.setPen(QPen(QColor(255,255,255,46),1.5));p.drawLine(QPointF(134,dividerY),QPointF(gridRight(),dividerY));
  p.restore();
  p.resetTransform();
  p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
  p.drawImage(QPointF(0,0),outlineMask);
  p.end();

  QPainter window(this);
  window.setCompositionMode(QPainter::CompositionMode_Source);
  const qreal t=std::clamp<qreal>(openMotion.F(),0,1.1);
  if(opened&&t<0.999){
   window.fillRect(rect(),Qt::transparent);
   window.setCompositionMode(QPainter::CompositionMode_SourceOver);
   window.setRenderHint(QPainter::SmoothPixmapTransform);
   const QPointF c(ox+panelRect.center().x()*s,oy+panelRect.center().y()*s);
   const qreal k=0.96+0.04*t;
   window.translate(c);window.scale(k,k);window.translate(-c);
   window.setOpacity(std::clamp<qreal>(t/0.45,0,1));
  }
  window.drawImage(QPointF(0,0),composedFrame);
  perfLine(paintTimer.nsecsElapsed()/1e6);
  if(waitingFirstFrame&&isVisible()){waitingFirstFrame=false;animClock.start();animTimer.start();update();}
 }
 bool event(QEvent* e)override{
   if(e->type()==QEvent::WindowActivate)hadFocus=true;
   if(e->type()==QEvent::WindowDeactivate)checkFocus();
   if(e->type()==QEvent::ToolTip){
    const auto* help=static_cast<QHelpEvent*>(e);
    const auto pos=design(help->pos());
    for(const auto& hit:hits)if(hit.first.contains(pos)){QToolTip::showText(help->globalPos(),items[hit.second].name,this);return true;}
    QToolTip::hideText();return true;
   }
   return QWidget::event(e);
 }
 void mouseMoveEvent(QMouseEvent* e)override{
   if(resizing){
    const QPointF d=e->globalPosition()-resizeGlobal;
    qreal w=resizeStartW,h=resizeStartH;  // logical px; both sides move, so twice the pointer's travel
    if(resizeEdges&1)w-=2*d.x();if(resizeEdges&2)w+=2*d.x();
    if(resizeEdges&4)h-=2*d.y();if(resizeEdges&8)h+=2*d.y();
    canvasW=w/s;canvasH=h/s;applyGeometry();return;
   }
   if(!chipDown){const int edges=edgesAt(design(e->position().toPoint()));setCursor(cursorFor(edges));return;}
   const auto pt=design(e->position().toPoint());
   chipOffset=std::clamp(chipStartOffset+float(pressPoint.x()-pt.x()),0.f,chipMax);
   update();
 }
 void mousePressEvent(QMouseEvent* e)override{const QPointF pt=design(e->position().toPoint());pressPoint=pt;
  if(e->button()==Qt::LeftButton){const int edges=edgesAt(pt);if(edges){resizeEdges=edges;resizing=true;resizeGlobal=e->globalPosition();resizeStartW=width();resizeStartH=height();setCursor(cursorFor(edges));return;}}
  if(!panelOutline.contains(pt)&&!searchOutline.contains(pt)){close();return;}
  if(e->button()==Qt::LeftButton&&chipViewport.contains(pt)&&panelOutline.contains(pt)){chipDown=true;chipStartOffset=chipOffset;return;}
  if(QRectF(searchRect.left()+892,150,95,85).contains(pt)){QMenu menu(this);menu.addAction("Liquid Glass tuning…",this,[this]{openEditor();});menu.addAction(dark?"Use desktop background":"Use black background",this,[this]{dark=!dark;redrawMaterial();});showMenu(menu,e->globalPosition().toPoint());return;}
  if(e->button()==Qt::RightButton&&gridViewport.contains(pt)){for(const auto& hit:hits)if(hit.first.contains(pt)){const int index=hit.second;QMenu menu(this);menu.addAction("Open",this,[this,index]{launch(index);});menu.addSeparator();menu.addAction(pins.contains(items[index].id)?"Unpin from Start":"Pin to Start",this,[this,index]{togglePin(index);});showMenu(menu,e->globalPosition().toPoint());return;}}
  if(e->button()==Qt::LeftButton){if(searchRect.contains(pt))search->setFocus(Qt::MouseFocusReason);else setFocus(Qt::MouseFocusReason);}
 }
 void mouseReleaseEvent(QMouseEvent* e)override{if(e->button()!=Qt::LeftButton)return;
  if(resizing){resizing=false;resizeEdges=0;saveWindowSize();redrawMaterial();return;}
  const auto pt=design(e->position().toPoint());if(chipDown){chipDown=false;if(panelOutline.contains(pt)&&std::abs(pt.x()-pressPoint.x())*s<QApplication::startDragDistance())for(const auto& chip:chips)if(chip.first.contains(pt)&&chip.first.contains(pressPoint)){category=category==chip.second?-1:chip.second;scroll.Reset();update();break;}return;}if(gridViewport.contains(pt))for(const auto& hit:hits)if(hit.first.contains(pt)&&hit.first.contains(pressPoint)){launch(hit.second);return;}}
 void wheelEvent(QWheelEvent* e)override{
   const auto pt=design(e->position().toPoint());
   const auto delta=e->pixelDelta().isNull()?e->angleDelta():e->pixelDelta();
   if(chipViewport.contains(pt)){
    chipOffset=std::clamp(chipOffset-float(delta.x()?delta.x():delta.y())*.5f,0.f,chipMax);
   }else if(panelOutline.contains(pt)){
    const float distance=e->pixelDelta().isNull()?delta.y()*(rowPitch*0.5f)/120.f:delta.y()/s;  // half a row per notch
    scroll.By(-distance);hover=-1;animateScroll();
   }else return;
   update();e->accept();
 }
 void keyPressEvent(QKeyEvent* e)override{if(e->key()==Qt::Key_F2){openEditor();return;}if(e->key()==Qt::Key_Escape){if(!query.isEmpty())search->clear();else if(category>=0){category=-1;scroll.Reset();update();}else close();return;}
  const auto featured=featuredItems();auto visible=featured;visible+=visibleItems(featured);
  if(!visible.empty()&&(e->key()==Qt::Key_Left||e->key()==Qt::Key_Right||e->key()==Qt::Key_Up||e->key()==Qt::Key_Down)){
   int at=visible.indexOf(hover);
   if(at<0)at=0;else at=std::clamp(at+(e->key()==Qt::Key_Left?-1:e->key()==Qt::Key_Right?1:e->key()==Qt::Key_Up?-columns:columns),0,static_cast<int>(visible.size())-1);
   hover=visible[at];
   const int featuredRows=(featured.size()+columns-1)/columns;
   const int top=at<featured.size()?at/columns*rowPitch:
    (featuredRows+(at-featured.size())/columns)*rowPitch+(featuredRows?featuredGap:0);
   scroll.Ensure(top,top+rowPitch,0,float(panelRect.bottom()-24-gridTop));
   animateScroll();return;
  }
  if((e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter)&&hover>=0){launch(hover);return;}
  if(!search->hasFocus()&&!e->text().isEmpty()&&e->text().front().isPrint()&&!(e->modifiers()&(Qt::ControlModifier|Qt::AltModifier))){search->setFocus();search->insert(e->text());return;}
  QWidget::keyPressEvent(e);
 }

};
int main(int argc,char** argv){
 QApplication app(argc,argv);Applications window;
 if(!app.arguments().contains("--background")){window.show();window.raise();window.activateWindow();}
 else{window.winId();  // resident: loaded, hidden; render off screen twice (catalog, then icons) so the first visible frame is cheap
  QTimer::singleShot(900,&window,[&window]{if(!window.isVisible())window.grab();});
  QTimer::singleShot(3000,&window,[&window]{if(!window.isVisible())window.grab();});}if(app.arguments().contains("--check-catalog")&&app.arguments().contains("--dev-static")&&app.arguments().contains("--settings-path"))window.checkCatalog();
 const auto args=app.arguments();const int snap=args.indexOf("--snapshot"),exit=args.indexOf("--exit-after");
 if(args.contains("--dev-static")&&snap>=0&&snap+1<args.size()){const auto path=args[snap+1];QTimer::singleShot(1200,&window,[&window,path]{window.grab().save(path);});}
 if(args.contains("--dev-static")&&exit>=0&&exit+1<args.size()){const int seconds=args[exit+1].toInt();if(seconds>0&&seconds<=120)QTimer::singleShot(seconds*1000,&app,&QCoreApplication::quit);}
 return app.exec();
}
