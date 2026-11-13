import QtQuick
import QtTest
import ".." as LabFiles
TestCase {
    name:"LabVisualReview";when:windowShown
    property url labSettingsFile:Qt.resolvedUrl("../build/visual-pages.ini")
    property QtObject labController:QtObject {
        property url shaderUrl:Qt.resolvedUrl("../../core/launcher/project-glass/tests/.qsb/liquidglass.frag.qsb")
        property string status:"就绪"
        function rebuild() {} function copy(text) {} function screenshot(window) {}
    }
    LabFiles.Lab {id:lab}
    function capture(item,path) {var done=false;verify(item.grabToImage(function(r){verify(r.saveToFile(path));done=true}));tryVerify(function(){return done})}
    function test_pages() {
        lab.apply({});findChild(lab,"labShapeModel").clear();lab.savedShapes="[]";lab.sliderCustom=true
        var inspector=findChild(lab,"labInspector")
        var add=findChild(lab,"labAddCard")
        verify(add.visible);mouseClick(add,add.width/2,add.height/2)
        lab.addShape("circle");lab.addShape("capsule")
        wait(500)
        var canvas=findChild(lab,"labStage")
        var shape=findChild(canvas,"labCustomShape2");verify(shape!==null)
        var originalX=shape.x
        mousePress(shape,shape.width/2,shape.height/2);mouseMove(shape,shape.width/2-20,shape.height/2+10,30);mouseMove(shape,shape.width/2-55,shape.height/2+35,50);mouseRelease(shape,shape.width/2-55,shape.height/2+35)
        verify(Math.abs(shape.x-originalX)>10,"New shape should follow a real drag")
        var grip=findChild(shape,"labShapeResizeHandle2");verify(grip!==null)
        var initialWidth=shape.width
        mousePress(grip,grip.width/2,grip.height/2);mouseMove(grip,grip.width/2+30,grip.height/2+20,50);mouseRelease(grip,grip.width/2+30,grip.height/2+20)
        verify(shape.width>initialWidth+10,"Resize handle should change selected shape size")
        capture(canvas,"liquid glass test/build/captures/inspector-shapes.png")
        capture(inspector,"liquid glass test/build/captures/inspector-scene.png")
        var material=inspector.tabs.itemAt(1);mouseClick(material,material.width/2,material.height/2);compare(inspector.page,1)
        wait(200);capture(inspector,"liquid glass test/build/captures/inspector-material.png")
        var slider=inspector.tabs.itemAt(2);mouseClick(slider,slider.width/2,slider.height/2);compare(inspector.page,2)
        wait(200);capture(inspector,"liquid glass test/build/captures/inspector-slider.png")
        var access=inspector.tabs.itemAt(3);mouseClick(access,access.width/2,access.height/2);compare(inspector.page,3)
        wait(200);capture(inspector,"liquid glass test/build/captures/inspector-access.png")
    }
    function cleanupTestCase(){lab.visible=false}
}
