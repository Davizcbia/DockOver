#pragma once
#include "glass_params.h"
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>
#include <functional>
#include <cmath>

struct PreviewTuning {
    dock::GlassParameters surfaces[2]; // Search pill, application grid.
    float top=1.f, reach=.46f, bottom=.44f;
    explicit PreviewTuning(dock::GlassParameters base) : surfaces{base,base} {}
};

inline bool adjustable(const dock::FloatParam& p) {
    const QString group=QString::fromWCharArray(p.group);
    const QByteArray key=p.key;
    return group!="Interaction" && group!="Pressed control" && key!="radius" && key!="backdropBlur";
}

inline bool previewSettings(PreviewTuning& values, bool save, QString path={}) {
    if(path.isEmpty()) path=qEnvironmentVariable("LOCALAPPDATA")+"/LiquidDock/applications-tuning.ini";
    QSettings file(path,QSettings::IniFormat);
    auto number=[&](QString key,float& value,float low,float high) {
        if(save){file.setValue(key,value); return;}
        bool ok=false; const double v=file.value(key,value).toDouble(&ok);
        if(ok && std::isfinite(v)) value=std::clamp(float(v),low,high);
    };
    number("gradient/top",values.top,0,1);
    number("gradient/reach",values.reach,.01f,.95f);
    number("gradient/bottom",values.bottom,0,1);
    for(int i=0;i<2;++i){
        const QString prefix=i?"panel/":"search/";
        for(const auto& p:dock::FloatParams()) if(adjustable(p))
            number(prefix+p.key,values.surfaces[i].*p.field,p.minValue,p.maxValue);
        for(const auto& f:dock::FlagParams()){
            bool& value=values.surfaces[i].*f.field;
            if(save) file.setValue(prefix+f.key,value);
            else value=file.value(prefix+f.key,value).toBool();
        }
    }
    if(save) file.sync();
    return file.status()==QSettings::NoError;
}

class TuningDialog final : public QDialog {
    PreviewTuning& current;
    PreviewTuning defaults;
    std::function<void()> changed;
    std::vector<std::function<void()>> sync;
    void number(QFormLayout* group, QString label, QString key, float& target, float lo, float hi) {
        auto row=new QWidget;
        auto layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);
        auto slider=new QSlider(Qt::Horizontal);
        auto spin=new QDoubleSpinBox;
        const double step=hi-lo>100?1.:hi-lo<=.31?.001:.01;
        const int ticks=qRound((hi-lo)/step);
        slider->setRange(0,ticks); slider->setAccessibleName(label);
        spin->setRange(lo,hi);spin->setDecimals(step<.01?3:step<1?2:0);spin->setSingleStep(step);
        spin->setObjectName(key);spin->setAccessibleName(label);spin->setKeyboardTracking(false);
        layout->addWidget(slider,1);layout->addWidget(spin);group->addRow(label,row);
        auto refresh=[=,&target]{
            QSignalBlocker a(slider),b(spin); spin->setValue(target);slider->setValue(qRound((target-lo)/step));
        };
        sync.push_back(refresh);refresh();
        connect(slider,&QSlider::valueChanged,spin,[=](int n){spin->setValue(lo+n*step);});
        connect(spin,&QDoubleSpinBox::valueChanged,this,[=,&target](double v){target=float(v);refresh();changed();});
    }
public:
    TuningDialog(QWidget* parent,PreviewTuning& values,const PreviewTuning& initial,std::function<void()> notify):
        QDialog(parent,Qt::Tool),current(values),defaults(initial),changed(std::move(notify)) {
        setWindowTitle("Liquid Glass tuning");resize(450,610);
        setStyleSheet("QDialog,QScrollArea,QScrollArea>QWidget>QWidget{background:#202329;color:#eee} QLabel,QGroupBox,QCheckBox{color:#e9ebef} QGroupBox{border:1px solid #434750;border-radius:8px;margin-top:10px;padding:10px 3px 3px} QGroupBox::title{subcontrol-origin:margin;left:12px} QTabBar::tab,QPushButton,QDoubleSpinBox{background:#323740;color:#f3f4f7;padding:4px 7px;border:1px solid #555b67;border-radius:4px} QTabBar::tab:selected{background:#475362} QTabWidget::pane{border:0}");
        auto outer=new QVBoxLayout(this);auto tabs=new QTabWidget;outer->addWidget(tabs);
        for(int i=0;i<2;++i){
            auto scroll=new QScrollArea;scroll->setWidgetResizable(true);auto page=new QWidget;
            auto column=new QVBoxLayout(page);
            auto section=[&](QString title){auto box=new QGroupBox(title);column->addWidget(box);return new QFormLayout(box);};
            if(i==0){
                auto form=section("Dark gradient");
                number(form,"Top darkness","gradient/top",values.top,0,1);
                number(form,"Dark gradient reach","gradient/reach",values.reach,.01f,.95f);
                number(form,"Bottom darkness","gradient/bottom",values.bottom,0,1);
            }
            for(const auto& [id,title]:std::initializer_list<std::pair<QString,QString>>{{"Geometry","Geometry"},{"Optics","Optics"},{"Lighting","Lighting"},{"Material","Material"}}){
                auto form=section(title);
                for(const auto& f:dock::FlagParams()) if(id==QString::fromWCharArray(f.group)){
                    auto check=new QCheckBox(QString::fromWCharArray(f.label));form->addRow(check);
                    bool* value=&(values.surfaces[i].*f.field);
                    auto refresh=[check,value]{QSignalBlocker b(check);check->setChecked(*value);};
                    sync.push_back(refresh);refresh();
                    connect(check,&QCheckBox::toggled,this,[this,value](bool on){*value=on;changed();});
                }
                for(const auto& p:dock::FloatParams()) if(id==QString::fromWCharArray(p.group) && adjustable(p)){
                    number(form,QString::fromWCharArray(p.label),QString(i?"panel/":"search/")+p.key,values.surfaces[i].*p.field,p.minValue,p.maxValue);
                    if(p.note) form->itemAt(form->count()-1)->widget()->setToolTip(QString::fromWCharArray(p.note));
                }
            }
            column->addStretch();scroll->setWidget(page);tabs->addTab(scroll,i?"Applications panel":"Search field");
        }
        auto actions=new QHBoxLayout;outer->addLayout(actions);auto reset=new QPushButton("Reset this page");auto save=new QPushButton("Save parameters");auto status=new QLabel;
        actions->addWidget(reset);actions->addStretch();actions->addWidget(status);actions->addWidget(save);
        connect(reset,&QPushButton::clicked,this,[this,tabs]{int i=tabs->currentIndex();current.surfaces[i]=defaults.surfaces[i];if(!i){current.top=defaults.top;current.bottom=defaults.bottom;current.reach=defaults.reach;}for(auto& refresh:sync)refresh();changed();});
        connect(save,&QPushButton::clicked,this,[this,status]{status->setText(previewSettings(current,true)?"Saved":"Save failed");QTimer::singleShot(2500,status,&QLabel::clear);});
    }
};
