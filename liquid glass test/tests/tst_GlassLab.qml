import QtQuick
import QtTest
import ".." as LabFiles

TestCase {
    id:test
    name:"GlassLabDrag"
    when:windowShown
    property url labSettingsFile:Qt.resolvedUrl("../../.codex-baseline/lab-drag-test.ini")
    property QtObject labController:QtObject {
        property url shaderUrl:Qt.resolvedUrl("../../core/launcher/project-glass/tests/.qsb/liquidglass.frag.qsb")
        property string status:"Automated drag test"
        function rebuild() {}
        function copy(text) {}
        function screenshot(window) {}
    }
    LabFiles.Lab {id:lab}
    function test_independent_slider_material() {
        var previous=lab.parameters()
        lab.reduceMotion=false;lab.sliderCustom=true;lab.sliderFrost=0.12;lab.sliderRefraction=0.35
        lab.sliderGlare=0.27;lab.sliderDensity=0.22;lab.sliderBevel=10;lab.sliderMagnification=1.2
        lab.sliderPressWidth=1.4;lab.sliderPressHeight=1.3;lab.sliderGain=2.1
        var slider=findChild(lab,"labGlassSlider")
        var thumb=findChild(slider,"labGlassSliderThumb")
        mousePress(slider,slider.width/2,slider.height/2)
        tryCompare(thumb,"width",56);tryCompare(thumb,"height",39)
        var effect=findChild(thumb,"liquidGlassShader")
        fuzzyCompare(effect.blurStrength,0.12,0.001);fuzzyCompare(effect.refractionStrength,0.35,0.001)
        fuzzyCompare(effect.materialDensity,0.22,0.001);fuzzyCompare(effect.glareStrength,0.27,0.001)
        fuzzyCompare(effect.optics.y,10,0.001);fuzzyCompare(effect.optics.w,1.2,0.001)
        mouseRelease(slider,slider.width/2,slider.height/2)
        var toggle=findChild(lab,"labGlassSwitch")
        var toggleEffect=findChild(findChild(toggle,"labGlassSwitchThumb"),"liquidGlassShader")
        fuzzyCompare(toggleEffect.materialDensity,lab.density,0.001)
        lab.apply(previous)
    }
    function test_new_shapes_resize_and_persist() {
        var shapes=findChild(lab,"labShapeModel")
        var initial=lab.savedShapes,oldWidth=lab.cardWidth,oldHeight=lab.cardHeight
        shapes.clear();lab.selectedShape=-1
        lab.resizeSelected(300,190);compare(lab.cardWidth,300);compare(lab.cardHeight,190)
        var cardHandle=findChild(lab,"labCardResizeHandle")
        verify(cardHandle!==null)
        mousePress(cardHandle,cardHandle.width/2,cardHandle.height/2)
        mouseMove(cardHandle,cardHandle.width/2+35,cardHandle.height/2+20,50)
        mouseRelease(cardHandle,cardHandle.width/2+35,cardHandle.height/2+20)
        verify(lab.cardWidth>310,"Original card resize handle must change width")
        lab.addShape("card");lab.addShape("circle");lab.addShape("capsule")
        compare(shapes.count,3);compare(lab.selectedName,"胶囊")
        var canvas=findChild(lab,"labStage")
        var capsule=findChild(canvas,"labCustomShape2")
        verify(capsule!==null)
        var initialX=capsule.x
        mousePress(capsule,capsule.width/2,capsule.height/2)
        mouseMove(capsule,capsule.width/2-20,capsule.height/2+10,30)
        mouseMove(capsule,capsule.width/2-55,capsule.height/2+35,50)
        mouseRelease(capsule,capsule.width/2-55,capsule.height/2+35)
        verify(Math.abs(capsule.x-initialX)>10,"Capsule follows mouse drag")
        fuzzyCompare(shapes.get(2).shapeX,capsule.x,0.01)
        var handle=findChild(capsule,"labShapeResizeHandle2")
        verify(handle!==null)
        var initialWidth=capsule.width
        mousePress(handle,handle.width/2,handle.height/2)
        mouseMove(handle,handle.width/2+30,handle.height/2+20,50)
        mouseRelease(handle,handle.width/2+30,handle.height/2+20)
        verify(capsule.width>initialWidth+10,"Resize handle changes capsule width")
        lab.resizeSelected(260,70);compare(lab.selectedWidth,260);compare(lab.selectedHeight,70)
        lab.selectedShape=1;lab.resizeSelected(160,110)
        compare(lab.selectedWidth,160);compare(lab.selectedHeight,160)
        var saved=lab.savedShapes
        shapes.clear();lab.savedShapes=saved;lab.restoreShapes()
        compare(shapes.count,3);compare(shapes.get(1).shapeWidth,160);compare(shapes.get(1).shapeHeight,160)
        lab.selectedShape=2;lab.deleteSelected();compare(shapes.count,2)
        shapes.clear();lab.savedShapes=initial;lab.restoreShapes();lab.selectedShape=-1
        lab.cardWidth=oldWidth;lab.cardHeight=oldHeight
    }
    function test_search_and_navigation_visibility() {
        lab.reduceMotion=true;lab.searchOpen=false;lab.showTopBar=true;lab.activeSection="Launch"
        var search=findChild(lab,"labSearch")
        tryCompare(search,"width",search.height)
        var nav=findChild(lab,"labTopNavigation")
        for (var i=0;i<3;i++) {
            var label=findChild(nav.labelsRepeater.itemAt(i),"labNavLabel"+i)
            verify(label!==null, "navigation label " + i + " was not created")
            verify(label.visible, "navigation label " + i + " is hidden")
            verify(label.text.length>0, "navigation label " + i + " is empty")
            compare(label.parent.parent.z,1)
        }
        lab.reduceMotion=false
    }
    function test_launcher_parameters_reach_shader() {
        lab.apply({})
        compare(lab.frost,0.3);compare(lab.lens,1);compare(lab.reflection,0.4)
        compare(lab.density,0.3);compare(lab.stroke,0.3);compare(lab.staticReflection,true)
        var card=findChild(lab,"labMergedGlass")
        tryVerify(function(){return findChild(card,"liquidGlassShader")!==null})
        var effect=findChild(card,"liquidGlassShader")
        compare(effect.staticReflection,1)
        lab.staticTopLight=1.5;lab.glassLift=2;lab.strokeOpacity=0.6
        tryVerify(function(){return Math.abs(effect.macStyle.x-1.5)<0.001 && Math.abs(effect.macStyle.w-2)<0.001 && Math.abs(effect.strokeColor.a-0.6)<0.01})
        var sidebar=findChild(lab,"labSideNavigation")
        var glow=findChild(lab,"labSidebarGlow")
        verify(glow!==null)
        lab.sidebarFrost=0.5;lab.sidebarGlowCoverage=0.4;lab.sidebarGlowWidth=200
        compare(sidebar.frostExtra,0.5);compare(glow.sampleWidth,200)
        tryVerify(function(){return Math.abs(glow.width-sidebar.width*0.4)<0.01})
        lab.apply({})
    }
    function test_settings_glass_controls() {
        lab.reduceMotion=false;lab.materialVariant=0
        var toggle=findChild(lab,"labSettingsToggle")
        verify(toggle!==null)
        var before=toggle.checked
        mousePress(toggle,12,toggle.height/2)
        var thumb=findChild(toggle,"labSettingsToggleThumb")
        tryVerify(function(){return thumb.interactionAmount>0.5})
        tryCompare(thumb,"width",104)
        tryCompare(thumb,"height",72)
        var toggleEffect=findChild(thumb,"liquidGlassShader")
        verify(toggleEffect!==null)
        verify(toggleEffect.sharpTextureSize.x>1 && toggleEffect.sharpTextureSize.y>1,"toggle needs a real backdrop texture size")
        verify(toggleEffect.blurredTextureSize.x>1 && toggleEffect.blurredTextureSize.y>1,"toggle needs a real blurred texture size")
        mouseRelease(toggle,12,toggle.height/2)
        compare(toggle.checked,!before)
        compare(lab.frostedMode,toggle.checked)
        var input=findChild(toggle,"labSettingsToggleInput")
        mousePress(input, input.width/2, input.height/2)
        mouseMove(input, toggle.checked ? 2 : input.width-2, input.height/2, 30)
        mouseRelease(input, toggle.checked ? 2 : input.width-2, input.height/2)
        compare(toggle.checked,before)
        var slider=findChild(lab,"labSettingsSlider")
        verify(slider!==null)
        var sliderThumb=findChild(slider,"labSettingsSliderThumb")
        mousePress(slider,slider.width/2,slider.height/2)
        tryVerify(function(){return sliderThumb.interactionAmount>0.5})
        tryCompare(sliderThumb,"width",50)
        tryCompare(sliderThumb,"height",36)
        var sliderEffect=findChild(sliderThumb,"liquidGlassShader")
        verify(sliderEffect!==null)
        verify(sliderEffect.sharpTextureSize.x>1 && sliderEffect.sharpTextureSize.y>1,"slider needs a real backdrop texture size")
        verify(sliderEffect.blurredTextureSize.x>1 && sliderEffect.blurredTextureSize.y>1,"slider needs a real blurred texture size")
        mouseRelease(slider,slider.width/2,slider.height/2)
    }
    function test_settings_lenses_sample_their_tracks() {
        var previous=lab.parameters()
        lab.reduceMotion=true;lab.opaque=false;lab.materialVariant=0
        lab.frost=0;lab.lens=0;lab.reflection=0;lab.density=0;lab.saturation=1;lab.darkGlass=false
        var toggle=findChild(lab,"labSettingsToggle")
        var toggleThumb=findChild(toggle,"labSettingsToggleThumb")
        var toggleTrack=findChild(toggle,"labSettingsToggleTrack")
        toggleTrack.color="#00FF00"
        mousePress(toggle,12,toggle.height/2)
        wait(200);waitForRendering(lab.contentItem)
        var p=toggleThumb.mapToItem(lab.contentItem,toggleThumb.width/2,toggleThumb.height/2)
        var green=grabImage(lab.contentItem)
        var pixelScaleX=green.width/lab.contentItem.width
        var pixelScaleY=green.height/lab.contentItem.height
        toggleTrack.color="#FF0000"
        wait(200);waitForRendering(lab.contentItem)
        var red=grabImage(lab.contentItem)
        verify(red.red(Math.floor(p.x*pixelScaleX),Math.floor(p.y*pixelScaleY)) > green.red(Math.floor(p.x*pixelScaleX),Math.floor(p.y*pixelScaleY))+20,
               "Settings switch lens must sample the current track")
        mouseRelease(toggle,12,toggle.height/2)
        toggleTrack.color=Qt.binding(function(){return toggle.checked?"#BEA1F6":"#3A3B40"})
        var slider=findChild(lab,"labSettingsSlider")
        var thumb=findChild(slider,"labSettingsSliderThumb")
        var fill=findChild(slider,"labSettingsSliderFill")
        fill.color="#0000FF"
        mousePress(slider,slider.width/2,slider.height/2)
        wait(200);waitForRendering(lab.contentItem)
        var sp=thumb.mapToItem(lab.contentItem,thumb.width/4,thumb.height/2)
        var blue=grabImage(lab.contentItem)
        fill.color="#FF0000"
        wait(200);waitForRendering(lab.contentItem)
        var changed=grabImage(lab.contentItem)
        verify(changed.red(Math.floor(sp.x*pixelScaleX),Math.floor(sp.y*pixelScaleY)) > blue.red(Math.floor(sp.x*pixelScaleX),Math.floor(sp.y*pixelScaleY))+20,
               "Settings slider lens must sample its filled track")
        mouseRelease(slider,slider.width/2,slider.height/2)
        fill.color="#BEA1F6"
        lab.apply(previous)
    }
    function test_material_variants_and_accessibility() {
        lab.fusionEnabled=false;lab.glassEnabled=true;lab.showTopBar=true;lab.opaque=false
        lab.materialVariant=0;lab.reduceBrightEffects=false;lab.increaseContrast=false
        var card=findChild(lab,"draggableGlassCard")
        tryVerify(function(){return findChild(card,"liquidGlassShader")!==null})
        var effect=findChild(card,"liquidGlassShader")
        var density=effect.materialDensity, frost=effect.blurStrength, glare=effect.glareStrength
        lab.materialVariant=2
        tryVerify(function(){return effect.materialDensity<density && effect.blurStrength<frost})
        lab.reduceBrightEffects=true
        tryVerify(function(){return effect.glareStrength<glare})
        lab.increaseContrast=true
        tryVerify(function(){return effect.materialDensity>=.65 && effect.strokeWidth>=1})
        lab.materialVariant=4
        tryVerify(function(){return findChild(card,"liquidGlassShader")===null})
        lab.materialVariant=0;lab.reduceBrightEffects=false;lab.increaseContrast=false;lab.fusionEnabled=true
        tryVerify(function(){return findChild(lab,"liquidGlassShader")!==null})
    }
    function test_drag_and_reset() {
        waitForRendering(lab.contentItem)
        for (var name of ["draggableGlassCard","draggableGlassBall"]) {
            var item=findChild(lab,name)
            verify(item!==null)
            var x=item.x, y=item.y
            mousePress(item,item.width/2,item.height/2)
            mouseMove(item,item.width/2-20,item.height/2-15,30)
            mouseMove(item,item.width/2-80,item.height/2-60,30)
            mouseRelease(item,item.width/2,item.height/2)
            verify(item.x<x-20 && item.y<y-20, name+" must move on real mouse drag")
            verify(item.x>=0 && item.y>=36)
        }
        lab.resetObjects()
        compare(findChild(lab,"draggableGlassCard").y,215)
        compare(findChild(lab,"draggableGlassBall").y,410)
    }
    function cleanupTestCase() {lab.visible=false}
    function test_navigation_capsule_drag() {
        lab.showSidebar=false;lab.showTopBar=true;lab.reduceMotion=false;lab.activeSection="Launch"
        tryCompare(lab,"sidebarProgress",0)
        tryCompare(lab,"topProgress",1)
        var nav=findChild(lab,"labTopNavigation")
        var area=findChild(lab,"labNavDragArea")
        var lens=findChild(lab,"labNavSelection")
        var capture=findChild(lab,"labNavContentTexture")
        var barCapture=findChild(lab,"labNavBarTexture")
        var composite=findChild(lab,"labNavComposite")
        var pressedLens=findChild(lab,"labNavPressedSelection")
        var effect=findChild(pressedLens,"liquidGlassShader")
        verify(effect!==null && capture.sourceItem===composite && barCapture.sourceItem===nav.materialLayer)
        compare(effect.sharpSource,capture)
        compare(effect.blurStrength,0)
        tryVerify(function(){return Math.abs(lens.x-6)<.2})
        var start=6+nav.tabWidth/2
        var middle=6+nav.tabWidth*1.5
        var initialCenter=lens.x+lens.width/2
        mousePress(area,start,28)
        tryVerify(function(){return lens.width>nav.tabWidth*1.1 && lens.height>nav.height})
        fuzzyCompare(lens.x+lens.width/2,initialCenter,.2)
        tryVerify(function(){return effect.interaction.w>=.99})
        mouseMove(area,start+25,28,30)
        mouseMove(area,middle+20,28,30)
        compare(nav.draggingSelection,true)
        tryVerify(function(){return lens.width>nav.tabWidth*1.1})
        compare(lab.activeSection,"Launch") // commit only after release
        mouseRelease(area,middle+20,28)
        compare(lab.activeSection,"Download")
        tryVerify(function(){return Math.abs(lens.x-6-nav.tabWidth)<.2})
        wait(600)
        mousePress(area,middle,28)
        mouseMove(area,nav.width+100,28,30)
        wait(120)
        verify(lens.centerPosition<=nav.width-6-nav.tabWidth/2+.1)
        verify(lens.x+lens.width>nav.width,"Pressed lens can extend past the menu bar")
        mouseRelease(area,nav.width+100,28)
        compare(lab.activeSection,"Settings")
        wait(600)
        // Reduced motion keeps dragging and snapping, without stretch/bounce.
        lab.reduceMotion=true
        mousePress(area,6+nav.tabWidth*2.5,28)
        mouseMove(area,start,28,30)
        compare(lens.width,nav.tabWidth)
        mouseRelease(area,start,28)
        compare(lab.activeSection,"Launch")
        mouseClick(area,middle,28)
        compare(lab.activeSection,"Download")
        lab.reduceMotion=false
    }
    function test_interaction_transitions() {
        lab.showSidebar=true;lab.showTopBar=false
        lab.switchNavigation()
        tryCompare(lab,"sidebarProgress",0)
        tryCompare(lab,"topProgress",1)
        var button=findChild(lab,"labPressButton")
        mousePress(button,button.width/2,button.height/2)
        tryVerify(function(){return button.scale>1.04})
        mouseRelease(button,button.width/2,button.height/2)
        tryCompare(lab,"searchProgress",1)
        var input=findChild(lab,"labSearchInput")
        tryCompare(input,"activeFocus",true)
        input.text="Fabric"
        var clear=findChild(lab,"labSearchClear")
        tryCompare(clear,"visible",true)
        mouseClick(clear,clear.width/2,clear.height/2)
        compare(input.text,"")
        tryCompare(input,"activeFocus",true)
        var cancel=findChild(lab,"labSearchCancel")
        mouseClick(cancel,cancel.width/2,cancel.height/2)
        tryCompare(lab,"searchProgress",0)
        var toggle=findChild(lab,"labGlassSwitch")
        mouseClick(toggle,20,20)
        tryCompare(lab,"glassProgress",0)
        mouseClick(toggle,20,20)
        tryCompare(lab,"glassProgress",1)
        lab.frostedMode=true
        tryCompare(lab,"frostProgress",1)
        lab.reduceMotion=true
        lab.showSidebar=true
        tryCompare(lab,"sidebarProgress",1)
        lab.frostedMode=false;lab.reduceMotion=false
        lab.bringTogether()
        var ball=findChild(lab,"draggableGlassBall")
        var card=findChild(lab,"draggableGlassCard")
        tryCompare(ball,"y",card.y+(card.height-ball.height)/2)
    }
    function test_glass_switch_and_slider() {
        lab.glassEnabled=true;lab.reduceMotion=false
        lab.density=.37;lab.stroke=0
        var toggle=findChild(lab,"labGlassSwitch")
        var thumb=findChild(lab,"labGlassSwitchThumb")
        tryCompare(toggle,"thumbPosition",1)
        var restingCenter=thumb.x+thumb.width/2
        mousePress(toggle,thumb.x+thumb.width/2,toggle.height/2)
        tryVerify(function(){return thumb.width>90})
        fuzzyCompare(thumb.x+thumb.width/2,restingCenter,.1)
        fuzzyCompare(thumb.y+thumb.height/2,toggle.height/2,.1)
        tryCompare(toggle,"interactionAmount",1)
        tryVerify(function(){return findChild(thumb,"liquidGlassShader").interaction.w>=.99})
        compare(findChild(toggle,"labSwitchFocusRing").border.width,0)
        fuzzyCompare(findChild(thumb,"liquidGlassShader").materialDensity,lab.density,.001)
        mouseMove(toggle,20,toggle.height/2,30)
        mouseRelease(toggle,20,toggle.height/2)
        compare(lab.glassEnabled,false)
        tryCompare(toggle,"thumbPosition",0)
        tryCompare(toggle,"interactionAmount",0)
        tryCompare(thumb,"width",74)
        var offCenter=thumb.x+thumb.width/2
        mousePress(toggle,offCenter,toggle.height/2)
        tryVerify(function(){return thumb.width>90})
        fuzzyCompare(thumb.x+thumb.width/2,offCenter,.1)
        mouseRelease(toggle,offCenter,toggle.height/2)
        compare(lab.glassEnabled,true)
        tryCompare(toggle,"thumbPosition",1)
        toggle.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(lab.glassEnabled,false)
        compare(findChild(toggle,"labSwitchFocusRing").border.width,2)
        keyClick(Qt.Key_Space)
        compare(lab.glassEnabled,true)
        lab.reduceMotion=true
        compare(toggle.thumbPosition,1)
        mousePress(toggle,thumb.x+thumb.width/2,toggle.height/2)
        compare(thumb.width,74)
        compare(toggle.interactionAmount,1)
        mouseRelease(toggle,thumb.x+thumb.width/2,toggle.height/2)
        compare(toggle.thumbPosition,0)
        lab.glassEnabled=true;lab.reduceMotion=false

        var slider=findChild(lab,"labGlassSlider")
        var sliderThumb=findChild(lab,"labGlassSliderThumb")
        lab.demoSliderValue=.25
        compare(slider.value,.25)
        var sliderCenter=sliderThumb.x+sliderThumb.width/2
        mousePress(slider,sliderCenter,slider.height/2)
        verify(findChild(slider,"labSliderPointer").active,"Slider must track the actual press point")
        tryCompare(sliderThumb,"interactionAmount",1)
        tryVerify(function(){return sliderThumb.width>48})
        // Mouse coordinates are integer pixels; Slider rounds its value to .01.
        fuzzyCompare(sliderThumb.x+sliderThumb.width/2,sliderCenter,.75)
        fuzzyCompare(sliderThumb.y+sliderThumb.height/2,slider.height/2,.1)
        tryVerify(function(){return findChild(sliderThumb,"liquidGlassShader").interaction.w>=.99})
        fuzzyCompare(findChild(sliderThumb,"liquidGlassShader").materialDensity,lab.density,.001)
        mouseMove(slider,slider.width*.8,slider.height/2,50)
        mouseRelease(slider,slider.width*.8,slider.height/2)
        tryCompare(sliderThumb,"interactionAmount",0)
        verify(lab.demoSliderValue>.65 && lab.demoSliderValue<=1,"Slider drag changes the value")
        slider.forceActiveFocus()
        var before=slider.value
        keyClick(Qt.Key_Left)
        verify(slider.value<before,"Slider responds to keyboard arrows")
        lab.density=.35
    }
    function test_thumb_samples_its_track() {
        var previous=lab.parameters()
        var previousSlider=lab.demoSliderValue
        lab.reduceMotion=true;lab.opaque=false;lab.glassEnabled=true
        lab.frost=0;lab.lens=0;lab.reflection=0;lab.density=0;lab.saturation=1;lab.darkGlass=false
        var stage=findChild(lab,"labStage")
        var toggle=findChild(lab,"labGlassSwitch")
        var switchThumb=findChild(lab,"labGlassSwitchThumb")
        var track=findChild(lab,"labSwitchTrack")
        mousePress(toggle,switchThumb.x+switchThumb.width/2,toggle.height/2)
        tryCompare(stage,"captureControls",true)
        wait(180)
        waitForRendering(lab.contentItem)
        var switchPoint=switchThumb.mapToItem(lab.contentItem,switchThumb.width/2,switchThumb.height/2)
        var green=grabImage(lab.contentItem)
        track.color="#FF0000"
        wait(180)
        waitForRendering(lab.contentItem)
        var red=grabImage(lab.contentItem)
        verify(red.red(Math.floor(switchPoint.x),Math.floor(switchPoint.y)) >
               green.red(Math.floor(switchPoint.x),Math.floor(switchPoint.y))+30,
               "Switch thumb must sample the underlying track")
        mouseRelease(toggle,switchThumb.x+switchThumb.width/2,toggle.height/2)
        track.color=Qt.binding(function(){return lab.glassEnabled?"#60C75F":"#DADADA"})
        tryCompare(stage,"captureControls",false)

        lab.demoSliderValue=.5
        var slider=findChild(lab,"labGlassSlider")
        var sliderThumb=findChild(lab,"labGlassSliderThumb")
        var fill=findChild(lab,"labSliderFill")
        mousePress(slider,slider.width/2,slider.height/2)
        tryCompare(stage,"captureControls",true)
        wait(180)
        waitForRendering(lab.contentItem)
        var sliderPoint=sliderThumb.mapToItem(lab.contentItem,sliderThumb.width/4,sliderThumb.height/2)
        var blue=grabImage(lab.contentItem)
        fill.color="#FF0000"
        wait(180)
        waitForRendering(lab.contentItem)
        var changed=grabImage(lab.contentItem)
        verify(changed.red(Math.floor(sliderPoint.x),Math.floor(sliderPoint.y)) >
               blue.red(Math.floor(sliderPoint.x),Math.floor(sliderPoint.y))+30,
               "Slider thumb must sample the filled track")
        mouseRelease(slider,slider.width/2,slider.height/2)
        fill.color="#398AF5"
        tryCompare(stage,"captureControls",false)
        lab.demoSliderValue=previousSlider
        lab.apply(previous)
    }
    function test_pressed_glass_distorts_pixels() {
        var previous=lab.parameters()
        var previousBackground=lab.backgroundChoice
        lab.reduceMotion=false;lab.opaque=false;lab.glassEnabled=true
        lab.frost=0;lab.lens=1;lab.reflection=0;lab.density=0;lab.darkGlass=false
        lab.interactionStrength=0;lab.backgroundChoice=2
        var toggle=findChild(lab,"labGlassSwitch")
        var thumb=findChild(lab,"labGlassSwitchThumb")
        tryCompare(toggle,"thumbPosition",1)
        mousePress(toggle,thumb.x+thumb.width*.75,toggle.height/2)
        tryCompare(toggle,"interactionAmount",1)
        tryVerify(function(){return findChild(thumb,"liquidGlassShader").interaction.w>=.99})
        wait(300);waitForRendering(lab.contentItem)
        var before=grabImage(lab.contentItem)
        lab.interactionStrength=2
        wait(300);waitForRendering(lab.contentItem)
        var after=grabImage(lab.contentItem)
        var p=thumb.mapToItem(lab.contentItem,0,0)
        var changed=0
        for(var yy=2;yy<thumb.height-2;yy+=2) for(var xx=2;xx<thumb.width-2;xx+=2) {
            var px=Math.floor(p.x+xx),py=Math.floor(p.y+yy)
            if(Math.abs(before.red(px,py)-after.red(px,py))+
               Math.abs(before.green(px,py)-after.green(px,py))+
               Math.abs(before.blue(px,py)-after.blue(px,py))>9)++changed
        }
        verify(changed>8,"Press interaction must visibly change the sampled glass, not just resize the thumb")
        mouseRelease(toggle,thumb.x+thumb.width/2,toggle.height/2)

        lab.glassEnabled=true;lab.demoSliderValue=.5;lab.interactionStrength=0
        var slider=findChild(lab,"labGlassSlider")
        var sliderThumb=findChild(lab,"labGlassSliderThumb")
        mousePress(slider,sliderThumb.x+sliderThumb.width*.7,slider.height/2)
        tryCompare(sliderThumb,"interactionAmount",1)
        wait(300);waitForRendering(lab.contentItem)
        var flatSlider=grabImage(lab.contentItem)
        lab.interactionStrength=2
        wait(300);waitForRendering(lab.contentItem)
        var warpedSlider=grabImage(lab.contentItem)
        var sp=sliderThumb.mapToItem(lab.contentItem,0,0)
        var sliderChanged=0
        for(var sy=2;sy<sliderThumb.height-2;sy+=2) for(var sx=2;sx<sliderThumb.width-2;sx+=2) {
            var ax=Math.floor(sp.x+sx),ay=Math.floor(sp.y+sy)
            if(Math.abs(flatSlider.red(ax,ay)-warpedSlider.red(ax,ay))+
               Math.abs(flatSlider.green(ax,ay)-warpedSlider.green(ax,ay))+
               Math.abs(flatSlider.blue(ax,ay)-warpedSlider.blue(ax,ay))>9)++sliderChanged
        }
        verify(sliderChanged>5,"Slider press must visibly affect the glass pixels")
        mouseRelease(slider,sliderThumb.x+sliderThumb.width/2,slider.height/2)
        lab.backgroundChoice=previousBackground
        lab.apply(previous)
    }
    function test_switch_lens_refracts_surroundings() {
        var previous=lab.parameters(), oldBackground=lab.backgroundChoice, oldWallpaper=lab.wallpaper
        lab.backgroundChoice=3;lab.wallpaper=Qt.resolvedUrl("white-background.svg")
        lab.darkGlass=false;lab.frost=.2;lab.lens=.85;lab.reflection=.55;lab.density=.1
        lab.interactionStrength=1;lab.reduceMotion=false;lab.opaque=false;lab.glassEnabled=true
        var toggle=findChild(lab,"labGlassSwitch"),thumb=findChild(lab,"labGlassSwitchThumb")
        tryCompare(toggle,"thumbPosition",1)
        wait(400)
        mousePress(toggle,thumb.x+thumb.width*.75,toggle.height/2)
        tryVerify(function(){return thumb.height>70})
        wait(400);waitForRendering(lab.contentItem)
        var frame=grabImage(lab.contentItem)
        var edge=thumb.mapToItem(lab.contentItem,thumb.width/2,6)
        var center=thumb.mapToItem(lab.contentItem,thumb.width/2,thumb.height/2)
        verify(frame.red(Math.floor(edge.x),Math.floor(edge.y)) > frame.red(Math.floor(center.x),Math.floor(center.y))+35,
               "The pressed switch must refract the bright surroundings into its glass edge")
        mouseRelease(toggle,thumb.x+thumb.width*.75,toggle.height/2)
        lab.backgroundChoice=oldBackground;lab.wallpaper=oldWallpaper;lab.apply(previous)
    }
}
