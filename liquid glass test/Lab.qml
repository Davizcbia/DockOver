// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Dialogs
import QtCore
import "../core/launcher/project-glass" as Production

ApplicationWindow {
    id: lab
    visible: true
    width: 1180; height: 800
    minimumWidth: 960; minimumHeight: 680
    title: "Liquid Launcher · 玻璃调试器"
    color: "#191B20"
    font.family: "Microsoft YaHei UI"
    palette.button: "#383D48"
    palette.buttonText: "#F3F5F8"
    palette.text: "#F3F5F8"
    palette.base: "#30343D"
    palette.highlight: "#58A9FF"
    property real frost: 0.30
    property real lens: 1
    property real reflection: 0.4
    property real density: 0.3
    property real corners: 28
    property real stroke: 0.3
    property real refractiveIndex: 1.45
    property real bevelWidth: 16
    property real dispersion: 0.035
    property real magnification: 1
    property real blurSpread: 0.85
    property real saturation: 1.1
    property real brightness: 1
    property bool staticReflection: true
    property real staticTopLight: 1
    property real staticBottomLight: 1
    property real staticBottomGlow: 1
    property real glassLift: 1
    property real strokeOpacity: 0.8
    property real lensLabelRange: 20
    property real sidebarFrost: 1
    property real sidebarGlow: 0.5
    property real sidebarGlowCoverage: 0.2
    property real sidebarGlowWidth: 100
    property real sidebarGlowBlur: 0.3
    property real sidebarGlowSaturation: 1
    property real fresnelAmount: 1
    property real specularAmount: 1
    property real edgeAmount: 1
    property real shadowAmount: 1
    property real lightAngle: -120
    property real interactionStrength: 1
    property real interactionRadius: 70
    property real responseTime: 80
    property bool sliderCustom: false
    property real sliderFrost: 0.3
    property real sliderRefraction: 1
    property real sliderGlare: 0.4
    property real sliderDensity: 0.3
    property real sliderBevel: 16
    property real sliderMagnification: 1
    property real sliderGain: 1.8
    property real sliderPressWidth: 1.25
    property real sliderPressHeight: 1.2
    readonly property var sliderOptions: [{"key":"sliderFrost","label":"滑块雾化","min":0,"max":1,"initial":0.3},{"key":"sliderRefraction","label":"滑块折射","min":0,"max":1,"initial":1},{"key":"sliderGlare","label":"滑块反光","min":0,"max":1,"initial":0.4},{"key":"sliderDensity","label":"滑块浓度","min":0,"max":1,"initial":0.3},{"key":"sliderBevel","label":"透镜边缘宽度 · px","min":1,"max":40,"initial":16},{"key":"sliderMagnification","label":"背景放大倍率","min":0.5,"max":2,"initial":1},{"key":"sliderGain","label":"按压响应倍率","min":0,"max":4,"initial":1.8},{"key":"sliderPressWidth","label":"按压宽度倍率","min":1,"max":1.6,"initial":1.25},{"key":"sliderPressHeight","label":"按压高度倍率","min":1,"max":1.6,"initial":1.2}]
    readonly property var advancedOptions: [{"key": "refractiveIndex", "label": "折射率", "min": 1, "max": 2.5, "initial": 1.45, "group": 0}, {"key": "bevelWidth", "label": "透镜边缘宽度 · px", "min": 1, "max": 40, "initial": 16, "group": 0}, {"key": "dispersion", "label": "色散", "min": 0, "max": 0.3, "initial": 0.035, "group": 0}, {"key": "magnification", "label": "背景放大倍率", "min": 0.5, "max": 2, "initial": 1, "group": 0}, {"key": "blurSpread", "label": "预模糊范围", "min": 0, "max": 1, "initial": 0.85, "group": 0}, {"key": "saturation", "label": "背景饱和度", "min": 0, "max": 2.5, "initial": 1.1, "group": 1}, {"key": "brightness", "label": "背景亮度", "min": 0, "max": 2, "initial": 1, "group": 1}, {"key": "fresnelAmount", "label": "菲涅耳边缘反射", "min": 0, "max": 3, "initial": 1, "group": 1}, {"key": "specularAmount", "label": "镜面反光", "min": 0, "max": 3, "initial": 1, "group": 1}, {"key": "edgeAmount", "label": "轮廓高光", "min": 0, "max": 3, "initial": 1, "group": 1}, {"key": "shadowAmount", "label": "投影强度", "min": 0, "max": 3, "initial": 1, "group": 1}, {"key": "lightAngle", "label": "光源方向 · °", "min": -180, "max": 180, "initial": -120, "group": 1}, {"key": "interactionStrength", "label": "指针 / 按压强度", "min": 0, "max": 3, "initial": 1, "group": 2}, {"key": "interactionRadius", "label": "交互影响半径 · px", "min": 10, "max": 200, "initial": 70, "group": 2}, {"key": "responseTime", "label": "指针跟随延迟 · ms", "min": 0, "max": 500, "initial": 80, "group": 2}, {"key": "lensLabelRange", "label": "透镜文字折射范围 · px", "min": 8, "max": 32, "initial": 20, "group": 2}, {"key": "staticTopLight", "label": "顶部高光", "min": 0, "max": 2, "initial": 1, "group": 3}, {"key": "staticBottomLight", "label": "底部高光", "min": 0, "max": 2, "initial": 1, "group": 3}, {"key": "staticBottomGlow", "label": "底部辉光", "min": 0, "max": 3, "initial": 1, "group": 3}, {"key": "glassLift", "label": "深色玻璃提亮", "min": 0, "max": 3, "initial": 1, "group": 1}, {"key": "strokeOpacity", "label": "描边深度", "min": 0, "max": 1, "initial": 0.8, "group": 1}, {"key": "sidebarFrost", "label": "侧边栏模糊", "min": 0, "max": 1, "initial": 1, "group": 4}, {"key": "sidebarGlow", "label": "侧边栏辉光", "min": 0, "max": 1, "initial": 0.5, "group": 4}, {"key": "sidebarGlowCoverage", "label": "右侧辉光覆盖", "min": 0.2, "max": 0.65, "initial": 0.2, "group": 4}, {"key": "sidebarGlowWidth", "label": "辉光采样宽度 · px", "min": 80, "max": 600, "initial": 100, "group": 4}, {"key": "sidebarGlowBlur", "label": "辉光柔和度", "min": 0, "max": 1, "initial": 0.3, "group": 4}, {"key": "sidebarGlowSaturation", "label": "辉光饱和度", "min": 0, "max": 1, "initial": 1, "group": 4}]
    property real pan: 0
    property bool darkGlass: true
    property bool reduceMotion: false
    property bool opaque: false
    // The custom mode preserves the user's existing tuning without alteration.
    // The other modes are lab-only approximations, not native Apple materials.
    property int materialVariant: 0
    property bool reduceBrightEffects: false
    property bool increaseContrast: false
    property bool animateBackdrop: false
    property int backgroundChoice: 0
    property url wallpaper: ""
    property string comparison: ""
    property int clicks: 0
    property bool showTopBar: true
    property bool searchOpen: false
    property bool glassEnabled: true
    property real demoSliderValue: 0.62
    property bool frostedMode: false
    property bool fusionEnabled: true
    property real fusionDistance: 15
    property real sidebarProgress: showSidebar ? 1 : 0
    property real topProgress: showTopBar ? 1 : 0
    property real searchProgress: searchOpen ? 1 : 0
    property real glassProgress: glassEnabled ? 1 : 0
    property real frostProgress: frostedMode ? 1 : 0
    Behavior on sidebarProgress {enabled:!lab.reduceMotion;NumberAnimation {duration:360;easing.type:Easing.OutCubic}}
    Behavior on topProgress {enabled:!lab.reduceMotion;NumberAnimation {duration:300;easing.type:Easing.OutCubic}}
    Behavior on searchProgress {enabled:!lab.reduceMotion;NumberAnimation {duration:320;easing.type:Easing.OutCubic}}
    Behavior on glassProgress {enabled:!lab.reduceMotion;NumberAnimation {duration:220}}
    Behavior on frostProgress {enabled:!lab.reduceMotion;NumberAnimation {duration:260}}
    function switchNavigation() {showTopBar=showSidebar;showSidebar=!showSidebar}
    function bringTogether() {
        fusionApproach.stop()
        if(reduceMotion) {
            glassBall.x=Math.min(stage.width-glassBall.width,dragCard.x+dragCard.width+12)
            glassBall.y=dragCard.y+(dragCard.height-glassBall.height)/2
        } else fusionApproach.start()
    }
    ParallelAnimation {
        id:fusionApproach
        NumberAnimation {target:glassBall;property:"x";to:Math.min(stage.width-glassBall.width,dragCard.x+dragCard.width+12);duration:650;easing.type:Easing.InOutCubic}
        NumberAnimation {target:glassBall;property:"y";to:dragCard.y+(dragCard.height-glassBall.height)/2;duration:650;easing.type:Easing.InOutCubic}
    }
    property bool sceneReady: false
    property real cardWidth: 260
    property real cardHeight: 180
    property int selectedShape: -1
    property int shapeRevision: 0
    property string savedShapes: "[]"
    ListModel { id:shapeModel; objectName:"labShapeModel" }
    readonly property real selectedWidth: { shapeRevision; return selectedShape<0 ? cardWidth : selectedShape<shapeModel.count ? shapeModel.get(selectedShape).shapeWidth : cardWidth }
    readonly property real selectedHeight: { shapeRevision; return selectedShape<0 ? cardHeight : selectedShape<shapeModel.count ? shapeModel.get(selectedShape).shapeHeight : cardHeight }
    readonly property string selectedName: { shapeRevision; return selectedShape<0 ? "示例卡片" : selectedShape<shapeModel.count ? ({card:"卡片",circle:"圆形",capsule:"胶囊"})[shapeModel.get(selectedShape).shapeType] : "示例卡片" }
    function saveShapes() {
        var items=[]
        for (var i=0;i<shapeModel.count;i++) {
            var item=shapeModel.get(i)
            items.push({shapeType:item.shapeType,shapeX:item.shapeX,shapeY:item.shapeY,shapeWidth:item.shapeWidth,shapeHeight:item.shapeHeight})
        }
        savedShapes=JSON.stringify(items);shapeRevision++
    }
    function restoreShapes() {
        try {
            var items=JSON.parse(savedShapes)
            if (!Array.isArray(items) || items.length>30) return
            for (var i=0;i<items.length;i++) {
                var s=items[i]
                if (!s || ["card","circle","capsule"].indexOf(s.shapeType)<0 || !isFinite(s.shapeX) || !isFinite(s.shapeY) || !isFinite(s.shapeWidth) || !isFinite(s.shapeHeight)) continue
                var w=Math.max(48,Math.min(500,s.shapeWidth)),h=Math.max(48,Math.min(400,s.shapeHeight))
                if (s.shapeType==="circle") h=w
                shapeModel.append({shapeType:s.shapeType,shapeX:Math.max(0,Math.min(stage.width-w,s.shapeX)),shapeY:Math.max(36,Math.min(stage.height-h,s.shapeY)),shapeWidth:w,shapeHeight:h})
            }
            shapeRevision++
        } catch(e) { console.warn("Cannot restore lab shapes:",e.message) }
    }
    function addShape(type) {
        if (shapeModel.count>=30) return
        var w=type==="card"?220:type==="circle"?130:200
        var h=type==="card"?145:type==="circle"?130:64
        var column=shapeModel.count%3,row=Math.floor(shapeModel.count/3)
        shapeModel.append({shapeType:type,shapeX:Math.max(0,Math.min(stage.width-w,200+column*200)),shapeY:Math.max(36,Math.min(stage.height-h,330+row*155)),shapeWidth:w,shapeHeight:h})
        selectedShape=shapeModel.count-1;saveShapes()
    }
    function resizeSelected(w,h) {
        w=Math.max(48,Math.min(500,w));h=Math.max(48,Math.min(400,h))
        if(selectedShape<0) {cardWidth=w;cardHeight=h;dragCard.x=Math.max(0,Math.min(dragCard.x,stage.width-w));dragCard.y=Math.max(36,Math.min(dragCard.y,stage.height-h));return}
        if(selectedShape>=shapeModel.count) return
        var s=shapeModel.get(selectedShape)
        if(s.shapeType==="circle") h=w
        shapeModel.setProperty(selectedShape,"shapeWidth",w);shapeModel.setProperty(selectedShape,"shapeHeight",h)
        shapeModel.setProperty(selectedShape,"shapeX",Math.max(0,Math.min(s.shapeX,stage.width-w)))
        shapeModel.setProperty(selectedShape,"shapeY",Math.max(36,Math.min(s.shapeY,stage.height-h)))
        saveShapes()
    }
    function deleteSelected() {if(selectedShape>=0 && selectedShape<shapeModel.count){shapeModel.remove(selectedShape);selectedShape=-1;saveShapes()}}
    property int screenshotDefaultsVersion: 0
    Component.onCompleted: {
        if (screenshotDefaultsVersion < 1) {
            apply({radius:corners,dark:darkGlass})
            screenshotDefaultsVersion = 1
        }
        sceneReady=true;resetObjects();restoreShapes()
    }
    property bool showSidebar: true
    property string activeSection: "Launch"
    readonly property color foreground: darkGlass ? "#FFFFFF" : "#18202A"
    function resetObjects() {
        dragCard.x = Math.min(230, stage.width-dragCard.width)
        dragCard.y = 215
        glassBall.x = stage.width-glassBall.width-35
        glassBall.y = 410
    }
    function parameters() {
        var result = {version:3, frost:frost, refraction:lens, glare:reflection, density:density,
                radius:corners, strokeWidth:stroke, dark:darkGlass,
                fusionDistance:fusionDistance,frostedMode:frostedMode,fusionEnabled:fusionEnabled,glassEnabled:glassEnabled,
                reducedMotion:reduceMotion, reducedTransparency:opaque,
                materialVariant:materialVariant,reduceBrightEffects:reduceBrightEffects,increaseContrast:increaseContrast,
                staticReflection:staticReflection,sliderCustom:sliderCustom}
        for (var item of advancedOptions) result[item.key] = lab[item.key]
        for (var sliderItem of sliderOptions) result[sliderItem.key] = lab[sliderItem.key]
        return result
    }
    function apply(p) {
        if (!p || typeof p !== "object" || Array.isArray(p)) throw new Error("请输入参数 JSON 对象")
        function number(key, fallback, low, high) {
            var value = p[key] === undefined ? fallback : p[key]
            if (typeof value !== "number" || !isFinite(value)) throw new Error(key+" 必须是有限数字")
            return Math.max(low, Math.min(high, value))
        }
        // Validate the whole payload before changing any live parameter.
        var next = {frost:number("frost",.3,0,1),lens:number("refraction",1,0,1),
            reflection:number("glare",.4,0,1),density:number("density",.3,0,1),
            corners:number("radius",28,0,80),stroke:number("strokeWidth",.3,0,3),fusionDistance:number("fusionDistance",15,1,100)}
        for (var item of advancedOptions) next[item.key]=number(item.key,item.initial,item.min,item.max)
        for (var sliderItem of sliderOptions) next[sliderItem.key]=number(sliderItem.key,sliderItem.initial,sliderItem.min,sliderItem.max)
        var variant=number("materialVariant",0,0,4)
        if (Math.floor(variant)!==variant) throw new Error("materialVariant 必须是整数")
        for (var key of ["dark","reducedMotion","reducedTransparency","frostedMode","fusionEnabled","glassEnabled","reduceBrightEffects","increaseContrast","staticReflection","sliderCustom"])
            if(p[key] !== undefined && typeof p[key] !== "boolean") throw new Error(key+" 必须是布尔值")
        for (var name in next) lab[name]=next[name]
        darkGlass=p.dark === undefined ? true : p.dark
        reduceMotion=p.reducedMotion === true; opaque=p.reducedTransparency === true
        materialVariant=variant;reduceBrightEffects=p.reduceBrightEffects===true;increaseContrast=p.increaseContrast===true
        frostedMode=p.frostedMode===true;fusionEnabled=p.fusionEnabled!==false;glassEnabled=p.glassEnabled!==false
        staticReflection=p.staticReflection===undefined ? true : p.staticReflection
        sliderCustom=p.sliderCustom===true
    }
    Settings {
        location: labSettingsFile
        property alias screenshotDefaultsVersion:lab.screenshotDefaultsVersion
        property alias glassEnabled:lab.glassEnabled
        property alias reduceMotion:lab.reduceMotion
        property alias opaque:lab.opaque
        property alias materialVariant:lab.materialVariant
        property alias reduceBrightEffects:lab.reduceBrightEffects
        property alias increaseContrast:lab.increaseContrast
        property alias frostedMode:lab.frostedMode
        property alias fusionEnabled:lab.fusionEnabled
        property alias fusionDistance:lab.fusionDistance
        property alias refractiveIndex: lab.refractiveIndex
        property alias bevelWidth: lab.bevelWidth
        property alias dispersion: lab.dispersion
        property alias magnification: lab.magnification
        property alias blurSpread: lab.blurSpread
        property alias saturation: lab.saturation
        property alias brightness: lab.brightness
        property alias fresnelAmount: lab.fresnelAmount
        property alias specularAmount: lab.specularAmount
        property alias edgeAmount: lab.edgeAmount
        property alias shadowAmount: lab.shadowAmount
        property alias lightAngle: lab.lightAngle
        property alias interactionStrength: lab.interactionStrength
        property alias interactionRadius: lab.interactionRadius
        property alias responseTime: lab.responseTime
        property alias frost: lab.frost
        property alias lens: lab.lens
        property alias reflection: lab.reflection
        property alias density: lab.density
        property alias corners: lab.corners
        property alias stroke: lab.stroke
        property alias darkGlass: lab.darkGlass
        property alias wallpaper: lab.wallpaper
        property alias backgroundChoice: lab.backgroundChoice
        property alias comparison: lab.comparison
        property alias staticReflection:lab.staticReflection
        property alias staticTopLight:lab.staticTopLight
        property alias staticBottomLight:lab.staticBottomLight
        property alias staticBottomGlow:lab.staticBottomGlow
        property alias glassLift:lab.glassLift
        property alias strokeOpacity:lab.strokeOpacity
        property alias lensLabelRange:lab.lensLabelRange
        property alias sidebarFrost:lab.sidebarFrost
        property alias sidebarGlow:lab.sidebarGlow
        property alias sidebarGlowCoverage:lab.sidebarGlowCoverage
        property alias sidebarGlowWidth:lab.sidebarGlowWidth
        property alias sidebarGlowBlur:lab.sidebarGlowBlur
        property alias sidebarGlowSaturation:lab.sidebarGlowSaturation
        property alias sliderCustom:lab.sliderCustom
        property alias sliderFrost:lab.sliderFrost
        property alias sliderRefraction:lab.sliderRefraction
        property alias sliderGlare:lab.sliderGlare
        property alias sliderDensity:lab.sliderDensity
        property alias sliderBevel:lab.sliderBevel
        property alias sliderMagnification:lab.sliderMagnification
        property alias sliderGain:lab.sliderGain
        property alias sliderPressWidth:lab.sliderPressWidth
        property alias sliderPressHeight:lab.sliderPressHeight
        property alias cardWidth:lab.cardWidth
        property alias cardHeight:lab.cardHeight
        property alias savedShapes:lab.savedShapes
    }
    Shortcut { sequence: "F5"; onActivated: labController.rebuild() }
    Shortcut {sequence:"Escape";enabled:lab.searchOpen;onActivated:lab.searchOpen=false}
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: labController.screenshot(lab) }
    FileDialog { id: chooser; nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.bmp)"]; onAccepted: {lab.wallpaper=selectedFile;lab.backgroundChoice=3} }

    Popup {
        id: importDialog; anchors.centerIn:parent;width:580;height:460;modal:true;focus:true
        ColumnLayout {
            anchors.fill:parent
            Label {text:"粘贴参数 JSON（支持旧版）";color:"white"}
            ScrollView {Layout.fillWidth:true;Layout.fillHeight:true
                TextArea {id:jsonInput;wrapMode:TextEdit.Wrap;selectByMouse:true;placeholderText:"{ \"frost\": 0.72, ... }"}
            }
            Label {id:importError;Layout.fillWidth:true;color:"#FF9494";wrapMode:Text.Wrap}
            RowLayout {
                Button {text:"应用";onClicked:{try {lab.apply(JSON.parse(jsonInput.text));importError.text="";importDialog.close()} catch(e){importError.text=e.message}}}
                Button {text:"取消";onClicked:importDialog.close()}
            }
        }
    }

    component Sample: Item {
        id: sample
        property alias text: caption.text
        property bool draggable: false
        property bool circular: false
        property real corner: lab.corners
        property bool clickable: true
        property bool materialVisible: true
        property bool alwaysMaterial: false
        property bool useControlBackdrop: false
        property Item materialViewport: stage
        property var materialSharp: sharp
        property var materialBlurred: frosted
        property real materialOpacity: 1
        property bool interactiveScale: false
        property bool fused: false
        property vector4d shapeOne:Qt.vector4d(0,0,width,height)
        property vector4d shapeTwo:Qt.vector4d(0,0,0,0)
        property real materialDensity:lab.density
        property real frostScale: 1
        property real frostExtra: 0
        property real frostOverride: -1
        property real refractionOverride: -1
        property real shadowOverride: -1
        property real lensPressAmount: 0
        property var materialForeground: null
        property bool foregroundBackdrop: true
        property alias materialLayer: materialLoader
        property real refractionDirection: 1
        property bool externalInteractionEnabled: false
        property vector4d externalInteraction: Qt.vector4d(width/2,height/2,0,0)
        property real interactionGain: 1
        property bool sliderMaterial: false
        signal activated()
        signal interactionStarted()
        signal interactionFinished()
        scale:!lab.reduceMotion && interactiveScale ? (mover.pressed?1.07:mover.containsMouse?1.025:1) : 1
        Behavior on scale {enabled:!lab.reduceMotion;NumberAnimation {duration:180;easing.type:Easing.OutBack;easing.overshoot:.6}}
        z: mover.drag.active ? 120 : draggable ? 100 : 1
        Loader {
            id:materialLoader
            anchors.fill: parent
            active: sample.materialVisible && lab.materialVariant!==4 && labController.shaderUrl.toString().length>0
            opacity:(sample.alwaysMaterial?1:lab.glassProgress)*sample.materialOpacity
            sourceComponent: Production.LiquidGlass {
                surface: sample; viewport: sample.materialViewport
                sharpTexture: sample.useControlBackdrop && stage.captureControls ? controlSharp : sample.materialSharp
                blurredTexture: sample.useControlBackdrop && stage.captureControls ? controlFrost : sample.materialBlurred
                foregroundTexture: sample.materialForeground || (sample.foregroundBackdrop && sample.materialViewport===stage && !sample.useControlBackdrop ? backdropLabelsTexture : null)
                foregroundBevel: sample.materialForeground ? lab.lensLabelRange : Math.min(4,lab.lensLabelRange)
                radius: Math.min(sample.corner, sample.height/2, sample.width/2)
                circular:sample.circular
                fusion:sample.fused;firstShape:sample.shapeOne;secondShape:sample.shapeTwo;fusionDistance:lab.fusionDistance
                density: sample.sliderMaterial && lab.sliderCustom ? (lab.increaseContrast ? Math.max(.65,lab.sliderDensity) : lab.sliderDensity) : lab.increaseContrast ? Math.max(.65,sample.materialDensity) :
                         lab.materialVariant===1 ? Math.max(.38,sample.materialDensity) :
                         lab.materialVariant===2 ? sample.materialDensity*.35 :
                         lab.materialVariant===3 ? Math.max(.68,sample.materialDensity) : sample.materialDensity
                frost: sample.sliderMaterial && lab.sliderCustom ? lab.sliderFrost : sample.frostOverride >= 0 ? sample.frostOverride : Math.min(1, (lab.materialVariant===2 ? lab.frost*.25 :
                        lab.materialVariant===1 ? Math.max(.65,lab.frost) :
                        lab.materialVariant===3 ? Math.max(.8,lab.frost) :
                        lab.frost+(1-lab.frost)*lab.frostProgress)*sample.frostScale+sample.frostExtra)
                refraction:sample.sliderMaterial && lab.sliderCustom ? lab.sliderRefraction : sample.refractionOverride>=0 ? sample.refractionOverride : lab.lens*(1-.85*lab.frostProgress)
                glare:(sample.sliderMaterial && lab.sliderCustom ? lab.sliderGlare : lab.reflection*(1-.65*lab.frostProgress))*(lab.reduceBrightEffects ? .3 : 1)
                refractionDirection:sample.refractionDirection
                dark:lab.darkGlass;reducedMotion:lab.reduceMotion;reducedTransparency:lab.opaque
                strokeWidth:lab.increaseContrast ? Math.max(1,lab.stroke) : lab.stroke
                stroke:lab.increaseContrast ? (lab.darkGlass ? "#B0FFFFFF" : "#A0000000") : Qt.rgba(0,0,0,lab.darkGlass ? lab.strokeOpacity : lab.strokeOpacity*0.5)
                staticReflection:lab.staticReflection
                macStyle:Qt.vector4d(lab.staticTopLight,lab.staticBottomLight,lab.staticBottomGlow,lab.glassLift)
                refractiveIndex:lab.refractiveIndex
                bevelWidth:(sample.sliderMaterial && lab.sliderCustom ? lab.sliderBevel : lab.bevelWidth)*(1-.4*sample.lensPressAmount)
                dispersion:lab.dispersion
                magnification:(sample.sliderMaterial && lab.sliderCustom ? lab.sliderMagnification : lab.magnification)*(1-.2*sample.lensPressAmount)
                flatBevel:sample.lensPressAmount
                saturation:lab.saturation
                brightness:lab.brightness
                fresnelAmount:lab.fresnelAmount
                specularAmount:lab.specularAmount*(lab.reduceBrightEffects ? .3 : 1)
                edgeAmount:lab.edgeAmount*(lab.reduceBrightEffects ? .4 : 1)
                shadowAmount:sample.shadowOverride>=0 ? sample.shadowOverride : lab.shadowAmount
                lightAngle:lab.lightAngle
                interactionRadius:lab.interactionRadius
                responseTime:lab.responseTime
                externalInteractionEnabled:sample.externalInteractionEnabled
                externalInteraction:sample.externalInteraction
                interactionStrength:lab.interactionStrength * sample.interactionGain
                shader:labController.shaderUrl
            }
        }
        Text { id:caption;anchors.centerIn:parent;color:lab.darkGlass?"#FFFFFF":"#18202A";font.pixelSize:18;horizontalAlignment:Text.AlignHCenter }
        MouseArea {
            id:mover;anchors.fill:parent;enabled:sample.clickable
            hoverEnabled:true
            onPressed:{sample.interactionStarted();if(sample.draggable)fusionApproach.stop()}
            onReleased:sample.interactionFinished()
            drag.target:sample.draggable?sample:null
            drag.minimumX:0;drag.maximumX:Math.max(0,stage.width-sample.width)
            drag.minimumY:36;drag.maximumY:Math.max(36,stage.height-sample.height)
            cursorShape:sample.draggable?(pressed?Qt.ClosedHandCursor:Qt.OpenHandCursor):Qt.PointingHandCursor
            onClicked:{lab.clicks++;sample.activated()}
        }
    }
    Item {
        id: stage
        objectName: "labStage"
        width: lab.width - 330; height: lab.height
        clip: true
        onWidthChanged: {
            if (!lab.sceneReady || !dragCard || !glassBall) return
            dragCard.x=Math.max(0,Math.min(dragCard.x,width-dragCard.width))
            glassBall.x=Math.max(0,Math.min(glassBall.x,width-glassBall.width))
        }
        onHeightChanged: {
            if (!lab.sceneReady || !dragCard || !glassBall) return
            dragCard.y=Math.max(36,Math.min(dragCard.y,height-dragCard.height))
            glassBall.y=Math.max(36,Math.min(glassBall.y,height-glassBall.height))
        }
        Item {
            id: backdrop
            anchors.fill: parent
            Rectangle {
                anchors.fill: parent
                color: lab.backgroundChoice === 1 ? "#1E1E1E" : "#E4E8ED"
                gradient: lab.backgroundChoice === 0 ? colorGradient : null
                Gradient { id: colorGradient; GradientStop {position:0;color:"#183C56"} GradientStop {position:1;color:"#8A5676"} }
            }
            Image { anchors.fill: parent; visible: lab.backgroundChoice===3; source: lab.wallpaper; fillMode: Image.PreserveAspectCrop }
            Item {
                x: lab.pan; width: parent.width; height: parent.height
                visible: lab.backgroundChoice !== 3
                Repeater {
                    model: 30
                    Rectangle {
                        required property int index
                        x: (index%6)*180-180; y: Math.floor(index/6)*190-60
                        width: 154; height: 160; radius: lab.backgroundChoice===2 ? 0 : 22
                        color: lab.backgroundChoice===2 ? (index%2 ? "#17232E" : "#DCE6EB") : ["#BB437273","#BBCA8A48","#BB755997"][index%3]
                        visible: lab.backgroundChoice !== 1
                        Text { anchors.centerIn: parent; text: "Aa 0123\n背景文字"; color: "white"; font.pixelSize: 20 }
                    }
                }
            }
        }
        ShaderEffectSource { id: sharp; sourceItem: backdrop; width:stage.width;height:stage.height;visible:false; textureSize:Qt.size(stage.width,stage.height); smooth:true }
        // Keep the test scene's labels as a separate foreground, just as the
        // launcher's navigation does, so white glyphs are not darkened by the glass.
        Item {
            id:backdropLabels;visible:false;width:stage.width;height:stage.height
            Item {
                x:lab.pan;width:parent.width;height:parent.height;visible:lab.backgroundChoice!==3 && lab.backgroundChoice!==1
                Repeater {
                    model:30
                    Item {
                        required property int index
                        x:(index%6)*180-180;y:Math.floor(index/6)*190-60;width:154;height:160
                        Text {anchors.centerIn:parent;text:"Aa 0123\n背景文字";color:"white";opacity:0.7;font.pixelSize:20}
                    }
                }
            }
        }
        ShaderEffectSource {id:backdropLabelsTexture;sourceItem:backdropLabels;textureSize:Qt.size(stage.width,stage.height);live:true;visible:false;smooth:true}
        // MultiEffect's own "blur" property shadows this id inside other effects.
        readonly property real backdropBlur: blur.blur
        MultiEffect { id: blur; width:stage.width;height:stage.height;source:sharp;blurEnabled:true;blur:lab.blurSpread+(Math.max(.95,lab.blurSpread)-lab.blurSpread)*lab.frostProgress;blurMax:40;autoPaddingEnabled:false;visible:false }
        ShaderEffectSource { id: frosted; sourceItem:blur;visible:false;textureSize:Qt.size(stage.width/2,stage.height/2);smooth:true }

        // The two thumb lenses need the underlying controls as well as the
        // wallpaper. Reuse the existing scene texture, then composite only the
        // two tracks into an offscreen tree: neither thumb is captured here.
        readonly property bool captureControls: !lab.opaque && lab.materialVariant!==4 &&
            (glassSwitch.interactionAmount>0.001 || sliderThumb.interactionAmount>0.001)
        Item {
            id:controlBackdrop
            width:stage.width;height:stage.height;visible:false
            ShaderEffect {anchors.fill:parent;property var source:sharp}
            Rectangle {
                x:glassSwitch.x;y:glassSwitch.y;width:glassSwitch.width;height:glassSwitch.height
                radius:switchTrack.radius;color:switchTrack.color
            }
            Rectangle {
                x:glassSlider.x+sliderTrack.x;y:glassSlider.y+(glassSlider.height-height)/2
                width:sliderTrack.width;height:sliderTrack.height;radius:sliderTrack.radius;color:sliderTrack.color
                Rectangle {width:parent.width*glassSlider.visualPosition;height:parent.height;radius:parent.radius;color:sliderFill.color}
            }
        }
        ShaderEffectSource {
            id:controlSharp;sourceItem:stage.captureControls?controlBackdrop:null
            textureSize:Qt.size(stage.width,stage.height);live:stage.captureControls;smooth:true;visible:false
        }
        MultiEffect {
            id:controlBlur;width:stage.width;height:stage.height;source:controlSharp
            blurEnabled:true;blur:stage.backdropBlur;blurMax:40;autoPaddingEnabled:false;visible:false
        }
        ShaderEffectSource {
            id:controlFrost;sourceItem:stage.captureControls?controlBlur:null
            textureSize:Qt.size(stage.width/2,stage.height/2);live:stage.captureControls;smooth:true;visible:false
        }

        // The lens may grow outside the bar. Compose a small padded region
        // from the existing scene texture and the real bar, excluding the lens.
        readonly property bool captureNavigation:lab.topProgress>.001 && lab.glassProgress>.001 && !lab.opaque && lab.materialVariant!==4
        Item {
            id:navComposite;objectName:"labNavComposite";visible:false
            width:topNavigation.width+64;height:topNavigation.height+64
            ShaderEffectSource {
                anchors.fill:parent;sourceItem:stage.captureNavigation?sharp:null
                sourceRect:Qt.rect(topNavigation.x-32,topNavigation.y-32,width,height)
                live:stage.captureNavigation;smooth:true
            }
            ShaderEffectSource {
                id:navBaseTexture;objectName:"labNavBarTexture"
                x:32;y:32;width:topNavigation.width;height:topNavigation.height
                sourceItem:stage.captureNavigation?topNavigation.materialLayer:null
                live:stage.captureNavigation;hideSource:false;smooth:true
            }
        }
        ShaderEffectSource {
            id:navContent;objectName:"labNavContentTexture"
            sourceItem:stage.captureNavigation?navComposite:null
            textureSize:Qt.size(Math.max(1,Math.ceil(navComposite.width)),Math.max(1,Math.ceil(navComposite.height)))
            live:sourceItem!==null;hideSource:false;visible:false;smooth:true
        }
        Item {
            id:navForeground;visible:false;width:navComposite.width;height:navComposite.height
            ShaderEffectSource {x:38;y:38;width:navLabels.width;height:navLabels.height;sourceItem:stage.captureNavigation?navLabels:null;live:sourceItem!==null;smooth:true}
        }
        ShaderEffectSource {
            id:navForegroundTexture;sourceItem:stage.captureNavigation?navForeground:null
            textureSize:navContent.textureSize;live:sourceItem!==null;visible:false;smooth:true
        }
        Item {
            id:navOverlay;x:topNavigation.x;y:topNavigation.y;width:topNavigation.width;height:topNavigation.height
            z:topNavigation.z+1;visible:topNavigation.visible;opacity:topNavigation.opacity;enabled:topNavigation.enabled
            Item {id:navSampleViewport;x:-32;y:-32;width:parent.width+64;height:parent.height+64;visible:false}
        }
        Sample {
            id:topNavigation;objectName:"labTopNavigation";visible:lab.topProgress>.001;opacity:lab.topProgress;enabled:lab.showTopBar
            width:Math.min(412,stage.width-32-188*lab.sidebarProgress);height:56
            x:(stage.width-width)/2+lab.sidebarProgress*70;y:54-(1-lab.topProgress)*25
            corner:28;clickable:false
            readonly property real tabWidth:(width-12)/3
            property alias labelsRepeater:navLabelRepeater
            readonly property int selectedIndex:Math.max(0,["Launch","Download","Settings"].indexOf(lab.activeSection))
            readonly property bool pressingSelection:navPointer.pressed && navPointer.grabbedLens
            property real lensAmount:pressingSelection && !lab.reduceMotion ? 1 : 0
            Behavior on lensAmount {NumberAnimation {duration:140;easing.type:Easing.OutCubic}}
            property int previewIndex:selectedIndex
            property bool draggingSelection:false
            property real dragCenter:0
            property real dragStretch:0
            function commitTab(index) {lab.activeSection=["Launch","Download","Settings"][Math.max(0,Math.min(2,index))]}
            Sample {
                id:navLens;objectName:"labNavSelection"
                parent:navOverlay
                materialOpacity:1-topNavigation.lensAmount;materialDensity:1;frostOverride:1;refractionOverride:0;shadowOverride:0;foregroundBackdrop:false
                property real centerPosition:topNavigation.draggingSelection
                    ? Math.max(6+topNavigation.tabWidth/2,Math.min(topNavigation.width-6-topNavigation.tabWidth/2,topNavigation.dragCenter))
                    : 6+((topNavigation.pressingSelection ? topNavigation.previewIndex : topNavigation.selectedIndex)+.5)*topNavigation.tabWidth
                width:topNavigation.pressingSelection && !lab.reduceMotion ? topNavigation.tabWidth*(1.12+topNavigation.dragStretch) : topNavigation.tabWidth
                height:topNavigation.pressingSelection && !lab.reduceMotion ? 72 : 44
                x:centerPosition-width/2
                y:(topNavigation.height-height)/2;corner:height/2;clickable:false;z:0
                externalInteractionEnabled:topNavigation.pressingSelection
                externalInteraction:Qt.vector4d(navPointer.mouseX-x,navPointer.mouseY-y,1,1)
                Behavior on centerPosition {enabled:!topNavigation.draggingSelection && !topNavigation.pressingSelection && !lab.reduceMotion;NumberAnimation {duration:550;easing.type:Easing.OutElastic;easing.period:.7}}
                Behavior on width {enabled:!lab.reduceMotion;NumberAnimation {duration:topNavigation.pressingSelection?120:550;easing.type:topNavigation.pressingSelection?Easing.OutCubic:Easing.OutElastic;easing.period:.7}}
                Behavior on height {enabled:!lab.reduceMotion;NumberAnimation {duration:topNavigation.pressingSelection?160:550;easing.type:topNavigation.pressingSelection?Easing.OutCubic:Easing.OutElastic;easing.period:.7}}
                Rectangle {anchors.fill:parent;radius:height/2;color:"#38000000";opacity:1-topNavigation.lensAmount}
            }
            Sample {
                id:navPressedLens;objectName:"labNavPressedSelection";parent:navOverlay;z:2
                x:navLens.x;y:navLens.y;width:navLens.width;height:navLens.height;corner:height/2;clickable:false
                materialViewport:navSampleViewport;materialSharp:navContent;materialBlurred:navContent;materialForeground:navForegroundTexture
                frostOverride:0;materialOpacity:topNavigation.lensAmount;lensPressAmount:topNavigation.lensAmount
                externalInteractionEnabled:topNavigation.pressingSelection
                externalInteraction:Qt.vector4d(navPointer.mouseX-x,navPointer.mouseY-y,1,1)
            }
            Row {
                id:navLabels;parent:navOverlay;z:1
                x:6;y:6;width:topNavigation.width-12;height:44
                Repeater {
                    id:navLabelRepeater
                    model:["Launch","Download","Settings"]
                    delegate:Item {
                        required property string modelData
                        required property int index
                        width:(topNavigation.width-12)/3;height:44
                        Text {objectName:"labNavLabel"+parent.index;anchors.centerIn:parent;text:parent.modelData;color:lab.activeSection===parent.modelData ? "#D7C1FF" : lab.foreground;font.pixelSize:13;font.bold:lab.activeSection===parent.modelData}
                        activeFocusOnTab:true
                        Accessible.role:Accessible.Button;Accessible.name:modelData
                        Keys.onReturnPressed:lab.activeSection=modelData
                        Keys.onSpacePressed:lab.activeSection=modelData
                    }
                }
            }
            MouseArea {
                id:navPointer;objectName:"labNavDragArea";anchors.fill:parent;z:3
                hoverEnabled:true;preventStealing:true
                property bool grabbedLens:false
                property real startX:0
                property real startCenter:0
                property real lastX:0
                cursorShape:grabbedLens && pressed?Qt.ClosedHandCursor:Qt.OpenHandCursor
                onPressed:function(mouse) {
                    startX=mouse.x;lastX=mouse.x;startCenter=navLens.x+navLens.width/2
                    topNavigation.previewIndex=Math.max(0,Math.min(2,Math.floor((mouse.x-6)/topNavigation.tabWidth)))
                    grabbedLens=true
                    startCenter=6+(topNavigation.previewIndex+.5)*topNavigation.tabWidth
                    topNavigation.dragStretch=0
                    topNavigation.dragCenter=startCenter
                }
                onPositionChanged:function(mouse) {
                    if(!pressed || !grabbedLens)return
                    if(!topNavigation.draggingSelection && Math.abs(mouse.x-startX)<Qt.styleHints.startDragDistance)return
                    topNavigation.dragCenter=startCenter+mouse.x-startX
                    topNavigation.draggingSelection=true
                    topNavigation.dragStretch=Math.min(.35,Math.abs(mouse.x-lastX)*.02)
                    lastX=mouse.x
                }
                onReleased:function(mouse) {
                    var index=topNavigation.draggingSelection ? Math.round((navLens.x+navLens.width/2-6)/topNavigation.tabWidth-.5) : Math.floor((mouse.x-6)/topNavigation.tabWidth)
                    topNavigation.commitTab(index)
                    topNavigation.draggingSelection=false;topNavigation.dragStretch=0;grabbedLens=false
                }
                onCanceled:{topNavigation.draggingSelection=false;topNavigation.dragStretch=0;grabbedLens=false}
                onEnabledChanged:if(!enabled){topNavigation.draggingSelection=false;grabbedLens=false}
            }
        }
        Sample {
            id:sideNavigation;objectName:"labSideNavigation";visible:lab.sidebarProgress>.001;opacity:lab.sidebarProgress;enabled:lab.showSidebar
            x:12-(1-lab.sidebarProgress)*200;y:122;width:184;height:stage.height-248;corner:24;clickable:false
            frostExtra:lab.sidebarFrost
            Production.SidebarGlow {
                objectName:"labSidebarGlow"
                anchors.right:parent.right;anchors.top:parent.top;anchors.bottom:parent.bottom
                width:parent.width*lab.sidebarGlowCoverage
                scene:backdrop;sceneLeft:sideNavigation.x+sideNavigation.width
                sampleWidth:lab.sidebarGlowWidth;softness:lab.sidebarGlowBlur;saturation:lab.sidebarGlowSaturation
                opacity:lab.sidebarGlow*0.35
                active:lab.glassEnabled && lab.materialVariant!==4 && !lab.opaque && lab.sidebarGlow>0.001
                shader:Qt.resolvedUrl("build/sidebarglow.frag.qsb")
            }
            Sample {
                x:14;y:48+Math.max(0,["Launch","Download","Settings"].indexOf(lab.activeSection))*54
                width:parent.width-28;height:42;corner:21;clickable:false;materialDensity:.75
                Behavior on y {enabled:!lab.reduceMotion;NumberAnimation {duration:240;easing.type:Easing.OutBack;easing.overshoot:.45}}
            }
            Column {
                z:2
                anchors.fill:parent;anchors.margins:14;spacing:12
                Text {text:"Project Glass";color:lab.foreground;font.pixelSize:18;font.bold:true}
                Repeater {
                    model:["Launch","Download","Settings"]
                    delegate:Rectangle {
                        required property string modelData
                        width:sideNavigation.width-28;height:42;radius:21
                        color:"transparent"
                        Text {anchors.centerIn:parent;text:parent.modelData;color:lab.foreground}
                        MouseArea {anchors.fill:parent;onClicked:lab.activeSection=parent.modelData}
                    }
                }
                Text {text:"Instances";color:lab.foreground;opacity:.6}
                Text {text:"The Rebirth\n\nSurvival\n\nQuick Download";color:lab.foreground;font.pixelSize:13}
            }
            Text {anchors.bottom:parent.bottom;anchors.left:parent.left;anchors.margins:18;text:"◉  Player";color:lab.foreground}
        }
        Sample {
            id:dragCard;objectName:"draggableGlassCard";x:230;y:215;width:lab.cardWidth;height:lab.cardHeight;draggable:true;materialVisible:!lab.fusionEnabled;text:"可拖动玻璃卡片\n靠近玻璃球观察融合"
            onInteractionStarted:lab.selectedShape=-1
            Rectangle {
                anchors.right:parent.right;anchors.bottom:parent.bottom;width:20;height:20;radius:10;z:100
                visible:lab.selectedShape<0;color:"#66FFFFFF";border.color:"#A0FFFFFF"
                MouseArea {objectName:"labCardResizeHandle";anchors.fill:parent;cursorShape:Qt.SizeFDiagCursor;onPressed:lab.selectedShape=-1;onPositionChanged:if(pressed){var p=mapToItem(stage,mouseX,mouseY);lab.resizeSelected(p.x-dragCard.x,p.y-dragCard.y)}}
            }
        }
        Sample { objectName:"labPressButton";x:stage.width-235;y:145;width:200;height:48;interactiveScale:true;text:"按压放大 · 搜索";onActivated:lab.searchOpen=!lab.searchOpen }
        Sample { id:glassBall;objectName:"draggableGlassBall";x:stage.width-185;y:410;width:150;height:150;draggable:true;circular:true;materialVisible:!lab.fusionEnabled;text:"玻璃球\n拖动我" }
        Sample {
            id:merged;objectName:"labMergedGlass";visible:lab.fusionEnabled;clickable:false;fused:true;z:99
            x:Math.min(dragCard.x,glassBall.x)-lab.fusionDistance/4
            y:Math.min(dragCard.y,glassBall.y)-lab.fusionDistance/4
            width:Math.max(dragCard.x+dragCard.width,glassBall.x+glassBall.width)-x+lab.fusionDistance/4
            height:Math.max(dragCard.y+dragCard.height,glassBall.y+glassBall.height)-y+lab.fusionDistance/4
            shapeOne:Qt.vector4d(dragCard.x-x,dragCard.y-y,dragCard.width,dragCard.height)
            shapeTwo:Qt.vector4d(glassBall.x-x,glassBall.y-y,glassBall.width,glassBall.height)
        }
        Repeater {
            model:shapeModel
            delegate: Sample {
                id:customShape
                required property int index
                required property string shapeType
                required property real shapeX
                required property real shapeY
                required property real shapeWidth
                required property real shapeHeight
                objectName:"labCustomShape"+index
                x:shapeX;y:shapeY;width:shapeWidth;height:shapeHeight;draggable:true
                corner:shapeType==="capsule" ? height/2 : lab.corners
                circular:shapeType==="circle"
                text:shapeType==="card"?"卡片":shapeType==="circle"?"圆形":"胶囊"
                onInteractionStarted:lab.selectedShape=index
                onInteractionFinished:{shapeModel.setProperty(index,"shapeX",x);shapeModel.setProperty(index,"shapeY",y);lab.saveShapes()}
                Rectangle {
                    anchors.fill:parent;radius:customShape.circular?width/2:Math.min(customShape.corner,width/2,height/2)
                    color:"transparent";border.width:lab.selectedShape===customShape.index?1:0;border.color:"#B0FFFFFF"
                }
                Rectangle {
                    anchors.right:parent.right;anchors.bottom:parent.bottom;width:20;height:20;radius:10;z:100
                    visible:lab.selectedShape===customShape.index;color:"#66FFFFFF";border.color:"#A0FFFFFF"
                    MouseArea {
                        objectName:"labShapeResizeHandle"+customShape.index
                        anchors.fill:parent;cursorShape:Qt.SizeFDiagCursor
                        onPressed:lab.selectedShape=customShape.index
                        onPositionChanged:if(pressed){var p=mapToItem(stage,mouseX,mouseY);lab.resizeSelected(p.x-customShape.x,p.y-customShape.y)}
                    }
                }
            }
        }
        Sample {
            objectName:"labSearch";z:150;x:stage.width-width-24;y:112
            width:52+(Math.min(420,stage.width-48)-52)*lab.searchProgress;height:52;corner:26
            clickable:!lab.searchOpen;onActivated:lab.searchOpen=true
            Text {x:15;anchors.verticalCenter:parent.verticalCenter;text:"⌕";color:lab.foreground;font.pixelSize:28}
            TextField {
                id:searchInput;objectName:"labSearchInput";visible:lab.searchProgress>.5
                x:43;y:6;width:Math.max(0,parent.width-130);height:40;opacity:lab.searchProgress
                placeholderText:"搜索实例…";color:lab.foreground;background:null
                onVisibleChanged:if(visible && lab.searchOpen)forceActiveFocus()
                onAccepted:lab.clicks++
            }
            Connections {target:lab;function onSearchOpenChanged(){if(lab.searchOpen)searchInput.forceActiveFocus()}}
            ToolButton {
                objectName:"labSearchClear";visible:lab.searchOpen && searchInput.text.length>0
                anchors.right:searchCancel.left;anchors.verticalCenter:parent.verticalCenter
                width:30;height:36;text:"×";Accessible.name:"清除搜索"
                onClicked:{searchInput.clear();searchInput.forceActiveFocus()}
            }
            ToolButton {
                id:searchCancel;objectName:"labSearchCancel";visible:lab.searchOpen
                anchors.right:parent.right;anchors.rightMargin:4;anchors.verticalCenter:parent.verticalCenter
                width:48;height:36;text:"取消";Accessible.name:"关闭搜索"
                onClicked:lab.searchOpen=false
            }
        }
        Sample {
            id:glassSwitch;objectName:"labGlassSwitch";x:stage.width-152;y:stage.height-190;width:116;height:50;corner:25;clickable:false;materialVisible:false;z:160
            property bool dragging:false
            property bool pointerFocus:false
            property real dragPosition:0
            property real thumbPosition:dragging?dragPosition:(lab.glassEnabled?1:0)
            property real interactionAmount:switchInput.pressed?1:0
            Behavior on thumbPosition {enabled:!glassSwitch.dragging && !lab.reduceMotion;NumberAnimation {duration:420;easing.type:Easing.OutBack;easing.overshoot:.65}}
            Behavior on interactionAmount {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
            activeFocusOnTab:true
            Accessible.role:Accessible.CheckBox;Accessible.name:"液态玻璃开关";Accessible.checked:lab.glassEnabled
            Keys.onSpacePressed:{pointerFocus=false;lab.glassEnabled=!lab.glassEnabled}
            Keys.onReturnPressed:{pointerFocus=false;lab.glassEnabled=!lab.glassEnabled}
            onActiveFocusChanged:if(!activeFocus)pointerFocus=false
            Rectangle {
                id:switchTrack;objectName:"labSwitchTrack"
                anchors.fill:parent;radius:25
                color:lab.glassEnabled?"#60C75F":"#DADADA"
                Behavior on color {enabled:!lab.reduceMotion;ColorAnimation {duration:220}}
            }
            Sample {
                id:switchThumb;objectName:"labGlassSwitchThumb"
                // Keep the resting thumb's centre fixed as the lens expands;
                // otherwise a press shifts it toward the middle of the track.
                x:3+74/2+Math.max(0,Math.min(1,glassSwitch.thumbPosition))*(glassSwitch.width-74-6)-width/2
                y:(parent.height-height)/2;width:switchInput.pressed && !lab.reduceMotion?104:74;height:switchInput.pressed && !lab.reduceMotion?72:44;corner:height/2
                clickable:false;alwaysMaterial:true;useControlBackdrop:true;materialOpacity:glassSwitch.interactionAmount
                refractionDirection:-1
                externalInteractionEnabled:switchInput.pressed
                externalInteraction:Qt.vector4d(switchInput.mouseX-x,switchInput.mouseY-y,1,1)
                interactionGain:3
                Behavior on width {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                Behavior on height {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                Rectangle {
                    anchors.fill:parent;anchors.margins:1;radius:height/2
                    color:"white";opacity:lab.materialVariant===4 ? 1 : 1-glassSwitch.interactionAmount
                }
            }
            MouseArea {
                id:switchInput;objectName:"labGlassSwitchInput";anchors.fill:parent;z:3;cursorShape:Qt.PointingHandCursor
                property real pressX:0
                property real startPosition:0
                onPressed:function(mouse){glassSwitch.pointerFocus=true;glassSwitch.forceActiveFocus();pressX=mouse.x;startPosition=lab.glassEnabled?1:0;glassSwitch.dragPosition=startPosition}
                onPositionChanged:function(mouse){
                    if(!pressed)return
                    if(!glassSwitch.dragging && Math.abs(mouse.x-pressX)<Qt.styleHints.startDragDistance)return
                    glassSwitch.dragPosition=Math.max(0,Math.min(1,startPosition+(mouse.x-pressX)/(glassSwitch.width-74-6)))
                    glassSwitch.dragging=true
                }
                onReleased:{lab.glassEnabled=glassSwitch.dragging?glassSwitch.dragPosition>=.5:!lab.glassEnabled;glassSwitch.dragging=false}
                onCanceled:glassSwitch.dragging=false
            }
            Text {anchors.bottom:parent.top;anchors.bottomMargin:6;anchors.horizontalCenter:parent.horizontalCenter;text:lab.glassEnabled?"玻璃已开启":"玻璃已关闭";color:lab.foreground}
            Rectangle {objectName:"labSwitchFocusRing";anchors.fill:parent;radius:25;color:"transparent";border.width:glassSwitch.activeFocus && !glassSwitch.pointerFocus?2:0;border.color:lab.darkGlass?"#D0FFFFFF":"#A0000000"}
        }
        Slider {
            id:glassSlider;objectName:"labGlassSlider";z:160
            x:Math.max(208,(stage.width-width)/2-35);y:stage.height-193;width:Math.min(290,stage.width-365);height:48
            from:0;to:1;stepSize:0.01;value:lab.demoSliderValue
            onMoved:lab.demoSliderValue=value
            Accessible.name:"玻璃滑块示例"
            PointHandler {id:sliderPointer;objectName:"labSliderPointer";target:null;enabled:!lab.reduceMotion}
            background:Item {
                z:0;x:0;y:(glassSlider.height-height)/2;width:glassSlider.width;height:6
                Rectangle {
                    id:sliderTrack;objectName:"labSliderTrack"
                    x:20;width:Math.max(0,parent.width-40);height:6;radius:3
                    color:lab.darkGlass?"#4B4B4D":"#DADADA"
                    Rectangle {id:sliderFill;objectName:"labSliderFill";width:parent.width*glassSlider.visualPosition;height:6;radius:3;color:"#398AF5"}
                }
            }
            handle:Sample {
            id:sliderThumb;objectName:"labGlassSliderThumb";sliderMaterial:true
                z:2
                property real interactionAmount:glassSlider.pressed?1:0
                Behavior on interactionAmount {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                // Preserve the 40-pixel resting thumb's centre while it grows.
                x:glassSlider.leftPadding+20+glassSlider.visualPosition*(glassSlider.availableWidth-40)-width/2
                y:(glassSlider.height-height)/2;width:glassSlider.pressed && !lab.reduceMotion?40*(lab.sliderCustom?lab.sliderPressWidth:1.25):40;height:glassSlider.pressed && !lab.reduceMotion?30*(lab.sliderCustom?lab.sliderPressHeight:1.2):30;corner:height/2
                clickable:false;alwaysMaterial:true;useControlBackdrop:true;materialOpacity:interactionAmount
                externalInteractionEnabled:glassSlider.pressed
                externalInteraction:sliderPointer.active
                    ? Qt.vector4d(sliderPointer.point.position.x-x,sliderPointer.point.position.y-y,1,1)
                    : Qt.vector4d(width/2,height/2,1,1)
                interactionGain:lab.sliderCustom?lab.sliderGain:1.8
                Behavior on width {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                Behavior on height {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                Rectangle {anchors.fill:parent;radius:height/2;color:"white";opacity:lab.materialVariant===4 ? 1 : 1-sliderThumb.interactionAmount}
            }
            Text {anchors.bottom:parent.top;anchors.bottomMargin:6;anchors.horizontalCenter:parent.horizontalCenter;text:"玻璃滑块  "+Math.round(glassSlider.value*100)+"%";color:lab.foreground}
        }
        Sample { x:(stage.width-width)/2;y:stage.height-110;width:Math.min(560,stage.width-60);height:66;text:"The Rebirth                         ▸ Launch" }
        Text { x:24;y:14;text:"实际 LiquidGlass.qml · 点击计数："+lab.clicks;color:"white";font.pixelSize:13 }
        SequentialAnimation {
            running:lab.animateBackdrop && !lab.reduceMotion;loops:Animation.Infinite
            NumberAnimation {target:lab;property:"pan";to:90;duration:2200;easing.type:Easing.InOutSine}
            NumberAnimation {target:lab;property:"pan";to:-90;duration:2200;easing.type:Easing.InOutSine}
        }
    }
    Rectangle {
        x:stage.width;width:330;height:parent.height;color:"#202127"
        property int page: 0
        property alias tabs:inspectorTabs
        id:inspector;objectName:"labInspector"
        Column {
            x:18;y:18;width:parent.width-36;spacing:7
            Label {text:"玻璃实验室";color:"white";font.pixelSize:22;font.bold:true}
            Label {text:"材质、控件与形状";color:"#A9ABB7";font.pixelSize:12}
            Rectangle {
                width:parent.width;height:38;radius:10;color:"#17181D"
                Row {
                    anchors.fill:parent;anchors.margins:3;spacing:2
                    Repeater {
                        id:inspectorTabs
                        model:["场景","材质","滑块","辅助"]
                        delegate: Rectangle {
                            required property int index;required property string modelData
                            width:(parent.width-6)/4;height:parent.height;radius:7
                            color:inspector.page===index?"#565965":"transparent"
                            Text {anchors.centerIn:parent;text:parent.modelData;color:"#F4F4F8";font.pixelSize:12;font.bold:inspector.page===parent.index}
                            MouseArea {anchors.fill:parent;onClicked:{inspector.page=parent.index;inspectorScroll.contentItem.contentY=0}}
                        }
                    }
                }
            }
        }
        ScrollView {
            id:inspectorScroll
            anchors.fill:parent;anchors.leftMargin:18;anchors.rightMargin:18;anchors.topMargin:135;anchors.bottomMargin:18;contentWidth:availableWidth
            ScrollBar.horizontal: ScrollBar {policy:ScrollBar.AlwaysOff}
            ScrollBar.vertical: ScrollBar {policy:ScrollBar.AlwaysOff}
            ColumnLayout {
                width:inspectorScroll.availableWidth;spacing:12
                component LabButton: Button {
                    id:labButton
                    implicitHeight:34
                    contentItem:Text {text:labButton.text;color:labButton.enabled?"#F2F2F7":"#73747C";font.pixelSize:12;horizontalAlignment:Text.AlignHCenter;verticalAlignment:Text.AlignVCenter;elide:Text.ElideRight}
                    background:Rectangle {radius:9;color:labButton.down?"#585C68":labButton.hovered?"#474A54":"#353740";border.width:1;border.color:"#20FFFFFF"}
                }
                component LabCombo: ComboBox {
                    id:labCombo
                    implicitHeight:36
                    contentItem:Text {leftPadding:11;rightPadding:22;text:labCombo.displayText;color:"#F2F2F7";font.pixelSize:12;verticalAlignment:Text.AlignVCenter;elide:Text.ElideRight}
                    indicator:Text {x:labCombo.width-width-11;y:(labCombo.height-height)/2;text:"⌄";font.pixelSize:16;color:"#C4C6D0"}
                    background:Rectangle {radius:9;color:labCombo.down?"#4B4E59":"#353740";border.width:1;border.color:"#20FFFFFF"}
                    delegate:ItemDelegate {
                        required property int index;required property var modelData
                        width:labCombo.width;text:modelData;highlighted:labCombo.highlightedIndex===index
                        contentItem:Text {text:parent.text;color:"#F2F2F7";font.pixelSize:12;verticalAlignment:Text.AlignVCenter}
                        background:Rectangle {radius:6;color:parent.highlighted?"#5C5F6E":"transparent"}
                    }
                    popup:Popup {
                        y:labCombo.height+4;width:labCombo.width;padding:5
                        implicitHeight:Math.min(contentItem.implicitHeight+10,300)
                        contentItem:ListView {clip:true;implicitHeight:contentHeight;model:labCombo.popup.visible?labCombo.delegateModel:null;currentIndex:labCombo.highlightedIndex;ScrollIndicator.vertical:ScrollIndicator {}}
                        background:Rectangle {radius:10;color:"#363842";border.width:1;border.color:"#45FFFFFF"}
                    }
                }
                Label {Layout.fillWidth:true;text:["场景与形状","玻璃材质","滑块按压","辅助与操作"][inspector.page];color:"#D8D9E1";font.pixelSize:14;font.bold:true;topPadding:5}
                RowLayout {
                    visible:inspector.page===3
                    LabButton {text:"复制参数";onClicked:labController.copy(JSON.stringify(lab.parameters(),null,2))}
                    LabButton {text:"保存截图";onClicked:labController.screenshot(lab)}
                }
                RowLayout {
                    visible:inspector.page===3
                    LabButton {Layout.fillWidth:true;Layout.preferredWidth:0;Layout.minimumWidth:0;text:"导入参数";onClicked:importDialog.open()}
                    LabButton {Layout.fillWidth:true;Layout.preferredWidth:0;Layout.minimumWidth:0;text:"记录 A";onClicked:lab.comparison=JSON.stringify(lab.parameters())}
                    LabButton {Layout.fillWidth:true;Layout.preferredWidth:0;Layout.minimumWidth:0;text:"切换 A / B";enabled:lab.comparison!=="";onClicked:{var current=JSON.stringify(lab.parameters());lab.apply(JSON.parse(lab.comparison));lab.comparison=current}}
                }
                Label {visible:inspector.page===0;text:"背景";color:"#A9ABB7";font.pixelSize:12}
                LabCombo {visible:inspector.page===0;Layout.fillWidth:true;model:["彩色背景","纯深色背景","高对比网格","自选图片"];currentIndex:lab.backgroundChoice;onActivated:lab.backgroundChoice=currentIndex}
                LabButton {visible:inspector.page===0;text:"选择背景图片…";onClicked:chooser.open()}
                Label {visible:inspector.page===1;text:"材质对照";color:"#A9ABB7";font.pixelSize:12}
                LabCombo {
                    visible:inspector.page===1
                    objectName:"labMaterialVariant";Layout.fillWidth:true
                    model:["自定义 · 当前调参","Regular · 常规","Clear · 清晰","Tinted · 增加不透明度","Identity · 关闭材质"]
                    currentIndex:lab.materialVariant;onActivated:lab.materialVariant=currentIndex
                }
                Label {visible:inspector.page===1;Layout.fillWidth:true;text:lab.materialVariant===2?"清晰材质适合图片背景；请检查文字对比度。":"材质模式不改动下方参数，可随时切回自定义。";color:"#A9ABB7";wrapMode:Text.Wrap;font.pixelSize:12}
                Label {visible:inspector.page===0;text:"预览";color:"#A9ABB7";font.pixelSize:12}
                LabButton {visible:inspector.page===0;text:"顶部栏 ↔ 侧边栏";Layout.fillWidth:true;onClicked:lab.switchNavigation()}
                RowLayout {
                    visible:inspector.page===0
                    LabButton {text:"搜索展开";onClicked:lab.searchOpen=!lab.searchOpen}
                    LabButton {text:"让玻璃靠近";onClicked:lab.bringTogether()}
                }
                component SettingsToggle: CheckBox {
                    id: toggle
                    objectName:"labSettingsToggle"
                    implicitHeight:28
                    spacing:8
                    leftPadding:0;rightPadding:0
                    contentItem: Text {
                        leftPadding:toggle.indicator.width+toggle.spacing
                        text:toggle.text;color:"white";font:toggle.font
                        verticalAlignment:Text.AlignVCenter
                    }
                    // The stage's glassSwitch at its own 116×50 geometry, scaled as a
                    // whole so the lens maps onto its backdrop exactly as on the stage.
                    indicator: Item {
                        id:toggleIndicator
                        readonly property real controlScale:0.4
                        x:0;y:(toggle.height-height)/2;width:116*controlScale;height:50*controlScale
                        Item {
                            id:toggleControl;width:116;height:50;scale:toggleIndicator.controlScale;transformOrigin:Item.TopLeft
                            Item {
                                id:toggleScene;visible:false;x:-48;y:-48;width:toggleControl.width+96;height:toggleControl.height+96
                                Rectangle {anchors.fill:parent;color:inspector.color}
                                Rectangle {x:48;y:48;width:toggleControl.width;height:toggleControl.height;radius:25;color:toggleTrack.color}
                            }
                            ShaderEffectSource {id:toggleTexture;sourceItem:toggleThumb.interactionAmount>0.001?toggleScene:null;textureSize:Qt.size(Math.ceil(toggleScene.width),Math.ceil(toggleScene.height));live:sourceItem!==null;visible:false;smooth:true}
                            MultiEffect {id:toggleBlur;width:toggleScene.width;height:toggleScene.height;source:toggleTexture;blurEnabled:true;blur:stage.backdropBlur;blurMax:40;autoPaddingEnabled:false;visible:false}
                            ShaderEffectSource {id:toggleFrost;sourceItem:toggleThumb.interactionAmount>0.001?toggleBlur:null;textureSize:Qt.size(Math.ceil(toggleScene.width/2),Math.ceil(toggleScene.height/2));live:sourceItem!==null;visible:false;smooth:true}
                            Rectangle {
                                id:toggleTrack;objectName:"labSettingsToggleTrack"
                                anchors.fill:parent;radius:25;color:toggle.checked?"#BEA1F6":"#3A3B40"
                                Behavior on color {enabled:!lab.reduceMotion;ColorAnimation {duration:220}}
                            }
                            Sample {
                                id:toggleThumb;objectName:"labSettingsToggleThumb"
                                property real position:toggleInput.dragging?toggleInput.dragPosition:(toggle.checked?1:0)
                                property real interactionAmount:toggleInput.pressed?1:0
                                x:3+74/2+Math.max(0,Math.min(1,position))*(toggleControl.width-74-6)-width/2
                                y:(parent.height-height)/2;width:toggleInput.pressed && !lab.reduceMotion?104:74;height:toggleInput.pressed && !lab.reduceMotion?72:44;corner:height/2
                                clickable:false;alwaysMaterial:true;materialViewport:toggleScene;materialSharp:toggleTexture;materialBlurred:toggleFrost;materialOpacity:interactionAmount
                                refractionDirection:-1;interactionGain:3
                                externalInteractionEnabled:toggleInput.pressed
                                externalInteraction:Qt.vector4d(toggleInput.mouseX-x,toggleInput.mouseY-y,1,1)
                                Behavior on position {enabled:!toggleInput.dragging && !lab.reduceMotion;NumberAnimation {duration:420;easing.type:Easing.OutBack;easing.overshoot:.65}}
                                Behavior on interactionAmount {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Behavior on width {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Behavior on height {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Rectangle {anchors.fill:parent;anchors.margins:1;radius:height/2;color:"white";opacity:lab.materialVariant===4 || labController.shaderUrl.toString().length===0?1:1-toggleThumb.interactionAmount}
                            }
                            MouseArea {
                                id:toggleInput;objectName:"labSettingsToggleInput";anchors.fill:parent;z:3;cursorShape:Qt.PointingHandCursor
                                property real pressX:0
                                property real startPosition:0
                                property real dragPosition:0
                                property bool dragging:false
                                onPressed:function(mouse){toggle.forceActiveFocus();pressX=mouse.x;startPosition=toggle.checked?1:0;dragPosition=startPosition}
                                onPositionChanged:function(mouse){
                                    if(!pressed)return
                                    if(!dragging && Math.abs(mouse.x-pressX)<Qt.styleHints.startDragDistance)return
                                    dragPosition=Math.max(0,Math.min(1,startPosition+(mouse.x-pressX)/(toggleControl.width-74-6)))
                                    dragging=true
                                }
                                onReleased:{var next=dragging?dragPosition>=.5:!toggle.checked;dragging=false;if(next!==toggle.checked){toggle.toggle();toggle.toggled()}}
                                onCanceled:dragging=false
                            }
                            Rectangle {anchors.fill:parent;radius:25;color:"transparent";border.width:toggle.visualFocus?2/toggleIndicator.controlScale:0;border.color:"white"}
                        }
                    }
                }
                SettingsToggle {visible:inspector.page===0;text:"Frosted Glass · 磨砂材质";checked:lab.frostedMode;onToggled:lab.frostedMode=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===0;text:"靠近时融合";checked:lab.fusionEnabled;onToggled:lab.fusionEnabled=checked;palette.windowText:"white"}
                Adjust {visible:inspector.page===0;label:"融合距离 · px";value:lab.fusionDistance;minimum:1;limit:100;onEdited:n=>lab.fusionDistance=n}
                SettingsToggle {visible:inspector.page===1;text:"反光不跟随指针";checked:lab.staticReflection;onToggled:lab.staticReflection=checked}
                RowLayout {
                    visible:inspector.page===0
                    SettingsToggle {text:"顶部栏";checked:lab.showTopBar;onToggled:lab.showTopBar=checked;palette.windowText:"white"}
                    SettingsToggle {text:"侧边栏";checked:lab.showSidebar;onToggled:lab.showSidebar=checked;palette.windowText:"white"}
                    LabButton {text:"归位";onClicked:lab.resetObjects()}
                }
                Label {visible:inspector.page===0;text:"形状";color:"#A9ABB7";font.pixelSize:12;topPadding:6}
                RowLayout {
                    visible:inspector.page===0;Layout.fillWidth:true
                    LabButton {objectName:"labAddCard";Layout.fillWidth:true;text:"＋ 卡片";onClicked:lab.addShape("card")}
                    LabButton {objectName:"labAddCircle";Layout.fillWidth:true;text:"＋ 圆形";onClicked:lab.addShape("circle")}
                    LabButton {objectName:"labAddCapsule";Layout.fillWidth:true;text:"＋ 胶囊";onClicked:lab.addShape("capsule")}
                }
                Label {visible:inspector.page===0;Layout.fillWidth:true;text:"选中："+lab.selectedName+" · 拖动右下角调整";color:"#D7D7E0";font.pixelSize:12;wrapMode:Text.Wrap}
                Adjust {visible:inspector.page===0;objectName:"labShapeWidth";label:"宽度 · px";value:lab.selectedWidth;minimum:48;limit:500;onEdited:n=>lab.resizeSelected(n,lab.selectedHeight)}
                Adjust {visible:inspector.page===0;objectName:"labShapeHeight";label:"高度 · px";value:lab.selectedHeight;minimum:48;limit:400;enabled:lab.selectedShape<0 || lab.selectedShape>=shapeModel.count || shapeModel.get(lab.selectedShape).shapeType!=="circle";onEdited:n=>lab.resizeSelected(lab.selectedWidth,n)}
                LabButton {visible:inspector.page===0 && lab.selectedShape>=0;Layout.fillWidth:true;text:"删除选中形状";onClicked:lab.deleteSelected()}
                component Adjust: ColumnLayout {
                    id: adjustRoot
                    property string label
                    property real value
                    property real minimum:0
                    property real limit:1
                    signal edited(real number)
                    Layout.fillWidth:true;spacing:0
                    RowLayout {
                        Layout.fillWidth:true
                        Label {Layout.fillWidth:true;text:adjustRoot.label;color:"#E8ECF4";font.pixelSize:12}
                        TextField {
                            Layout.preferredWidth:70;implicitHeight:28
                            text:Number(adjustRoot.value).toFixed(3)
                            Accessible.name:adjustRoot.label
                            color:"#F2F2F7";selectByMouse:true
                            background:Rectangle {radius:6;color:"#30323A";border.width:1;border.color:parent.activeFocus?"#BEA1F6":"#4D505B"}
                            validator:DoubleValidator {bottom:adjustRoot.minimum;top:adjustRoot.limit;decimals:3;locale:"C"}
                            onEditingFinished:{if(acceptableInput)adjustRoot.edited(Number(text));text=Qt.binding(function(){return Number(adjustRoot.value).toFixed(3)})}
                        }
                    }
                    // The stage's glassSlider at its own 48 px geometry, scaled as a
                    // whole so the lens maps onto its backdrop exactly as on the stage.
                    Item {
                        id:knobFrame
                        readonly property real controlScale:28/48
                        Layout.fillWidth:true;implicitHeight:28
                        Slider {
                            id: knob
                            objectName: "labSettingsSlider"
                            width:knobFrame.width/knobFrame.controlScale;height:48
                            scale:knobFrame.controlScale;transformOrigin:Item.TopLeft
                            from:adjustRoot.minimum;to:adjustRoot.limit;value:adjustRoot.value
                            onMoved:adjustRoot.edited(value)
                            Accessible.name:adjustRoot.label
                            PointHandler {id:settingsPointer;target:null}
                            Item {
                                id:sliderScene;visible:false;x:-32;y:-32;width:knob.width+64;height:knob.height+64
                                Rectangle {anchors.fill:parent;color:inspector.color}
                                Rectangle {
                                    x:32+20;y:32+(knob.height-height)/2;width:Math.max(0,knob.width-40);height:6;radius:3;color:knobTrack.color
                                    Rectangle {width:parent.width*knob.visualPosition;height:parent.height;radius:3;color:knobFill.color}
                                }
                            }
                            ShaderEffectSource {id:sliderSceneTexture;sourceItem:settingsThumb.interactionAmount>0.001?sliderScene:null;textureSize:Qt.size(Math.ceil(sliderScene.width),Math.ceil(sliderScene.height));live:sourceItem!==null;visible:false;smooth:true}
                            MultiEffect {id:settingsSliderBlur;width:sliderScene.width;height:sliderScene.height;source:sliderSceneTexture;blurEnabled:true;blur:stage.backdropBlur;blurMax:40;autoPaddingEnabled:false;visible:false}
                            ShaderEffectSource {id:settingsSliderFrost;sourceItem:settingsThumb.interactionAmount>0.001?settingsSliderBlur:null;textureSize:Qt.size(Math.ceil(sliderScene.width/2),Math.ceil(sliderScene.height/2));live:sourceItem!==null;visible:false;smooth:true}
                            background: Item {
                                z:0;x:0;y:(knob.height-height)/2;width:knob.width;height:6
                                Rectangle {
                                    id:knobTrack;objectName:"labSettingsSliderTrack"
                                    x:20;width:Math.max(0,parent.width-40);height:6;radius:3
                                    color:lab.darkGlass?"#4B4B4D":"#DADADA"
                                    Rectangle {id:knobFill;objectName:"labSettingsSliderFill";width:parent.width*knob.visualPosition;height:6;radius:3;color:"#BEA1F6"}
                                }
                            }
                            handle: Sample {
                                id:settingsThumb;objectName:"labSettingsSliderThumb";sliderMaterial:true
                                z:2
                                property real interactionAmount:knob.pressed?1:0
                                x:knob.leftPadding+20+knob.visualPosition*(knob.availableWidth-40)-width/2
                                y:(knob.height-height)/2;width:knob.pressed && !lab.reduceMotion?40*(lab.sliderCustom?lab.sliderPressWidth:1.25):40;height:knob.pressed && !lab.reduceMotion?30*(lab.sliderCustom?lab.sliderPressHeight:1.2):30;corner:height/2
                                clickable:false;alwaysMaterial:true;materialViewport:sliderScene;materialSharp:sliderSceneTexture;materialBlurred:settingsSliderFrost;materialOpacity:interactionAmount
                                externalInteractionEnabled:knob.pressed
                                externalInteraction:settingsPointer.active
                                    ? Qt.vector4d(settingsPointer.point.position.x-x,settingsPointer.point.position.y-y,1,1)
                                    : Qt.vector4d(width/2,height/2,1,1)
                                interactionGain:lab.sliderCustom?lab.sliderGain:1.8
                                Behavior on interactionAmount {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Behavior on width {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Behavior on height {enabled:!lab.reduceMotion;NumberAnimation {duration:150;easing.type:Easing.OutCubic}}
                                Rectangle {anchors.fill:parent;radius:height/2;color:"white";opacity:lab.materialVariant===4 || labController.shaderUrl.toString().length===0?1:1-settingsThumb.interactionAmount}
                            }
                        }
                    }
                }
                Adjust {visible:inspector.page===1;label:"雾化";value:lab.frost;onEdited:n=>lab.frost=n}
                Adjust {visible:inspector.page===1;label:"透镜深度";value:lab.lens;onEdited:n=>lab.lens=n}
                Adjust {visible:inspector.page===1;label:"反光强度";value:lab.reflection;onEdited:n=>lab.reflection=n}
                Adjust {visible:inspector.page===1;label:"材质浓度";value:lab.density;onEdited:n=>lab.density=n}
                Adjust {visible:inspector.page===1;label:"连续圆角半径";value:lab.corners;limit:80;onEdited:n=>lab.corners=n}
                Adjust {visible:inspector.page===1;label:"描边宽度";value:lab.stroke;limit:3;onEdited:n=>lab.stroke=n}
                Label {visible:inspector.page===1;text:"高级调节";color:"#A9ABB7";font.pixelSize:12;topPadding:6}
                LabCombo {id: advancedGroup;visible:inspector.page===1;Layout.fillWidth:true;model:["光学 / 模糊","光照 / 颜色","交互 / 动态","固定反光","侧边栏"]}
                Repeater {
                    model: lab.advancedOptions
                    delegate: Adjust {
                        required property var modelData
                        visible: inspector.page===1 && modelData.group === advancedGroup.currentIndex
                        label: modelData.label; minimum:modelData.min;limit:modelData.max
                        value:lab[modelData.key]
                        onEdited:n=>lab[modelData.key]=n
                    }
                }
                SettingsToggle {visible:inspector.page===2;objectName:"labSliderCustomToggle";text:"独立调整滑块按压材质";checked:lab.sliderCustom;onToggled:lab.sliderCustom=checked}
                Label {visible:inspector.page===2;Layout.fillWidth:true;text:"关闭时沿用全局材质；开启后只影响滑块的按压状态。";color:"#A9ABB7";wrapMode:Text.Wrap;font.pixelSize:12}
                Repeater {
                    model:lab.sliderOptions
                    delegate:Adjust {
                        required property var modelData
                        visible:inspector.page===2;enabled:lab.sliderCustom
                        label:modelData.label;minimum:modelData.min;limit:modelData.max;value:lab[modelData.key]
                        onEdited:n=>lab[modelData.key]=n
                    }
                }
                SettingsToggle {visible:inspector.page===3;text:"深色玻璃";checked:lab.darkGlass;onToggled:lab.darkGlass=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===3;text:"背景来回移动";checked:lab.animateBackdrop;onToggled:lab.animateBackdrop=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===3;text:"减少动态效果";checked:lab.reduceMotion;onToggled:lab.reduceMotion=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===3;text:"减少透明度";checked:lab.opaque;onToggled:lab.opaque=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===3;objectName:"labReduceBrightEffects";text:"减少高亮效果";checked:lab.reduceBrightEffects;onToggled:lab.reduceBrightEffects=checked;palette.windowText:"white"}
                SettingsToggle {visible:inspector.page===3;objectName:"labIncreaseContrast";text:"提高对比度";checked:lab.increaseContrast;onToggled:lab.increaseContrast=checked;palette.windowText:"white"}
                LabButton {visible:inspector.page===3;text:"重新编译 shader · F5";Layout.fillWidth:true;onClicked:labController.rebuild()}
                LabButton {visible:inspector.page===3;text:"恢复截图参数";Layout.fillWidth:true;onClicked:lab.apply({})}
                Label {visible:inspector.page===3;Layout.fillWidth:true;text:labController.status;color:"#B8CBDD";wrapMode:Text.WrapAnywhere;font.pixelSize:12}
                Label {visible:inspector.page===3;Layout.fillWidth:true;text:"调参仅保存在调试器中。满意后可复制参数，再应用到启动器。";color:"#AEB5C1";wrapMode:Text.Wrap;font.pixelSize:12}
            }
        }
        Rectangle {
            anchors.right:parent.right;anchors.rightMargin:4;anchors.top:parent.top;anchors.topMargin:135;anchors.bottom:parent.bottom;anchors.bottomMargin:18
            width:10;color:"transparent"
            readonly property var area:inspectorScroll.contentItem.visibleArea
            visible:area && area.heightRatio<0.99
            Rectangle {id:inspectorScrollThumb;anchors.horizontalCenter:parent.horizontalCenter;width:3;radius:1.5;color:"#6B6D78";height:Math.max(22,parent.height*parent.area.heightRatio);y:Math.min(parent.height-height,parent.height*parent.area.yPosition)}
            MouseArea {
                anchors.fill:parent;preventStealing:true
                function scrollTo(y) {
                    var f=inspectorScroll.contentItem
                    f.contentY=Math.max(0,Math.min(f.contentHeight-f.height,(y-inspectorScrollThumb.height/2)/Math.max(1,height-inspectorScrollThumb.height)*(f.contentHeight-f.height)))
                }
                onPressed:function(mouse){scrollTo(mouse.y)}
                onPositionChanged:function(mouse){if(pressed)scrollTo(mouse.y)}
            }
        }
    }
}
