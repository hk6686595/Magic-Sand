//
//  SandSurfaceRenderer.cpp
//  GreatSand
//
//  Created by Thomas Wolf on 02/07/16.
//
//

#include "SandSurfaceRenderer.h"

using namespace ofxCSG;

SandSurfaceRenderer::SandSurfaceRenderer(std::shared_ptr<KinectProjector> const& k, std::shared_ptr<ofAppBaseWindow> const& p)
:settingsLoaded(false) {
    kinectProjector = k;
    projWindow = p;
}

void SandSurfaceRenderer::setup(bool sdisplayGui){
    ofAddListener(ofEvents().exit, this, &SandSurfaceRenderer::exit);
    
    // Sandbox contourlines
    drawContourLines = true; // Flag if topographic contour lines are enabled
	contourLineDistance = 10.0; // Elevation distance between adjacent topographic contour lines in millimiters
    
    // Initialize the fbos and images
    projResX = projWindow->getWidth();
    projResY = projWindow->getHeight();
    contourLineFramebufferObject.allocate(projResX+1, projResY+1, GL_RGBA);
    contourLineFramebufferObject.begin();
    ofClear(0,0,0,255);
    contourLineFramebufferObject.end();

    //Try to load settings file if possible
    if (loadSettings())
    {
        ofLogVerbose("SandSurfaceRenderer") << "SandSurfaceRenderer.setup(): sandSurfaceRendererSettings.xml loaded " ;
        settingsLoaded = true;
    } else {
        ofLogVerbose("SandSurfaceRenderer") << "SandSurfaceRenderer.setup(): sandSurfaceRendererSettings.xml could not be loaded " ;
    }

    // Load colormap folder and set heightmap
    colorMapPath = "colorMaps/";
    ofDirectory dir(colorMapPath);
    dir.allowExt("xml");
    dir.listDir();
    for(int i = 0; i < dir.size(); i++){
        colorMapFilesList.push_back(dir.getName(i));
    }
    
    bool heighMapFileLoaded = true;
    
    if (settingsLoaded)
        heighMapFileLoaded = heightMap.loadFile(colorMapPath+colorMapFile);
    
    if (!(settingsLoaded && heighMapFileLoaded) && dir.size() > 0)
    {
        heighMapFileLoaded = heightMap.loadFile(colorMapPath+colorMapFilesList[0]);
        colorMapFile = colorMapFilesList[0];
        saveSettings();
        settingsLoaded = true;
    }
    
    if (!heighMapFileLoaded)
    {
        heightMap.createFile(colorMapPath+"HeightColorMap.xml");
        colorMapFile = "HeightColorMap.xml";
        saveSettings();
        settingsLoaded = true;
    }
    
    //Set elevation Min and Max
    elevationMin = -heightMap.getScalarRangeMin();
    elevationMax = -heightMap.getScalarRangeMax();
    
    // Calculate the  height map elevation scaling and offset coefficients
	heightMapScale = (heightMap.getNumEntries()-1)/((elevationMax-elevationMin));
	heightMapOffset = 0.5/heightMap.getNumEntries()-heightMapScale*elevationMin;
    
    // Calculate the contourline fbo scaling and offset coefficients
	contourLineFboScale = elevationMin-elevationMax;
	contourLineFboOffset = elevationMax;
    contourLineFactor = contourLineFboScale/contourLineDistance;
    
    //setup the mesh
    setupMesh();
    
	// Load shaders
    bool loaded = true;
#ifdef TARGET_OPENGLES
    ofLogVerbose("SandSurfaceRenderer") << "setup(): Loading shadersES2";
	loaded = loaded && elevationShader.load("shaders/shadersES2/elevationShader");
	loaded = loaded && heightMapShader.load("shaders/shadersES2/heightMapShader");
#else
	if(ofIsGLProgrammableRenderer()){
        ofLogVerbose("SandSurfaceRenderer") << "setup(): Loading shadersGL3/elevationShader";
		loaded = loaded && elevationShader.load("shaders/shadersGL3/elevationShader");
        ofLogVerbose("SandSurfaceRenderer") << "setup(): Loading shadersGL3/heightMapShader";
		loaded = loaded && heightMapShader.load("shaders/shadersGL3/heightMapShader");
	}else{
        ofLogVerbose("SandSurfaceRenderer") << "setup(): Loading shadersGL2/elevationShader";
		loaded = loaded && elevationShader.load("shaders/shadersGL2/elevationShader");
        ofLogVerbose("SandSurfaceRenderer") << "setup(): Loading shadersGL2/heightMapShader";
		loaded = loaded && heightMapShader.load("shaders/shadersGL2/heightMapShader");
	}
#endif
    if (!loaded)
    {
        ofLogError("GreatSand") << "setup(): shader not loaded" ;
    }
    
    //Prepare fbo
    fboProjWindow.allocate(projResX, projResY, GL_RGBA);
    fboProjWindow.begin();
    ofClear(0,0,0,255);
    fboProjWindow.end();
    
    displayGui = sdisplayGui;
    if (displayGui)
        setupGui();
    
    // Setup range, base plane and conversion matrices
    updateConversionMatrices();
    updateRangesAndBasePlane();
}

void SandSurfaceRenderer::exit(ofEventArgs& e){
    if (saveSettings())
    {
        ofLogVerbose("SandSurfaceRenderer") << "exit(): Settings saved " ;
    } else {
        ofLogVerbose("SandSurfaceRenderer") << "exit(): Settings could not be saved " ;
    }
}

void SandSurfaceRenderer::updateConversionMatrices(){
    // Get conversion matrices
    transposedKinectProjMatrix = kinectProjector->getTransposedKinectProjMatrix();
    transposedKinectWorldMatrix = kinectProjector->getTransposedKinectWorldMatrix();
}

void SandSurfaceRenderer::updateRangesAndBasePlane(){
    basePlaneEq = kinectProjector->getBasePlaneEq();
    basePlaneNormal = kinectProjector->getBasePlaneNormal();
    basePlaneOffset = kinectProjector->getBasePlaneOffset();

    // Set the FilteredDepthImage native scale - converted to 0..1 when send to the shader
    kinectProjector->updateNativeScale(basePlaneOffset.z+elevationMax, basePlaneOffset.z+elevationMin);
    
    // Calculate the  FilteredDepthImage scaling and offset coefficients
	FilteredDepthScale = elevationMin-elevationMax;
	FilteredDepthOffset = basePlaneOffset.z+elevationMax;
    
    ofLogVerbose("SandSurfaceRenderer") << "setRangesAndBasePlaneEquation(): basePlaneOffset: " << basePlaneOffset ;
    ofLogVerbose("SandSurfaceRenderer") << "setRangesAndBasePlaneEquation(): basePlaneNormal: " << basePlaneNormal ;
}

void SandSurfaceRenderer::setupMesh(){
    // Initialise mesh
    //    float planeScale = 1;
    ofRectangle kinectROI = kinectProjector->getKinectROI();
    ofVec2f kinectRes = kinectProjector->getKinectRes();
    meshwidth = kinectROI.width;//kinectResX;
    meshheight = kinectROI.height;//kinectResY;
    mesh.clear();
    for(unsigned int y=0;y<meshheight;y++)
        for(unsigned int x=0;x<meshwidth;x++)
        {
            ofPoint pt = ofPoint(x*kinectRes.x/(meshwidth-1)+kinectROI.x,y*kinectRes.y/(meshheight-1)+kinectROI.y,0.0f)-ofPoint(0.5,0.5,0); // We move of a half pixel to center the color pixel (more beautiful)
            //        ofPoint pt = ofPoint(x*kinectResX*planeScale/(meshwidth-1)+kinectROI.x,y*kinectResY*planeScale/(meshheight-1)+kinectROI.y,0.0f)-kinectROI.getCenter()*planeScale+kinectROI.getCenter()-ofPoint(0.5,0.5,0); // with a planescaling
            mesh.addVertex(pt); // make a new vertex
            mesh.addTexCoord(pt);
        }
    for(unsigned int y=0;y<meshheight-1;y++)
        for(unsigned int x=0;x<meshwidth-1;x++)
        {
            mesh.addIndex(x+y*meshwidth);         // 0
            mesh.addIndex((x+1)+y*meshwidth);     // 1
            mesh.addIndex(x+(y+1)*meshwidth);     // 10
            
            mesh.addIndex((x+1)+y*meshwidth);     // 1
            mesh.addIndex((x+1)+(y+1)*meshwidth); // 11
            mesh.addIndex(x+(y+1)*meshwidth);     // 10
        }
}

void SandSurfaceRenderer::update(){
    // Update Renderer state if needed
    if (kinectProjector->isROIUpdated())
        setupMesh();
    if (kinectProjector->isBasePlaneUpdated())
        updateRangesAndBasePlane();
    if (kinectProjector->isCalibrationUpdated())
        updateConversionMatrices();
    
    // Draw sandbox
    if (drawContourLines)
        prepareContourLinesFbo();
    drawSandbox();
    
    // GUI
    if (displayGui)
        colorList->update();
}

void SandSurfaceRenderer::drawMainWindow(float x, float y, float width, float height){
    fboProjWindow.draw(x, y, width, height);
    
    if (displayGui)
        colorList->draw();
}

void SandSurfaceRenderer::drawProjectorWindow(){
    fboProjWindow.draw(0,0);
}

void SandSurfaceRenderer::drawSandbox() {
    fboProjWindow.begin();
    ofBackground(0);
    kinectProjector->bind();
    heightMapShader.begin();
    heightMapShader.setUniformMatrix4f("kinectProjMatrix",transposedKinectProjMatrix);
    heightMapShader.setUniformMatrix4f("kinectWorldMatrix",transposedKinectWorldMatrix);
    heightMapShader.setUniform2f("heightColorMapTransformation",ofVec2f(heightMapScale,heightMapOffset));
    heightMapShader.setUniform2f("depthTransformation",ofVec2f(FilteredDepthScale,FilteredDepthOffset));
    heightMapShader.setUniform4f("basePlaneEq", basePlaneEq);
    heightMapShader.setUniformTexture("heightColorMapSampler",heightMap.getTexture(), 2);
    heightMapShader.setUniformTexture("pixelCornerElevationSampler", contourLineFramebufferObject.getTexture(), 3);
    heightMapShader.setUniform1f("contourLineFactor", contourLineFactor);
    heightMapShader.setUniform1i("drawContourLines", drawContourLines);
    mesh.draw();
    heightMapShader.end();
    kinectProjector->unbind();
    fboProjWindow.end();
}

void SandSurfaceRenderer::prepareContourLinesFbo()
{
    contourLineFramebufferObject.begin();
    ofClear(255,255,255, 0);
    kinectProjector->bind();
	elevationShader.begin();
    elevationShader.setUniformMatrix4f("kinectProjMatrix",transposedKinectProjMatrix);
    elevationShader.setUniformMatrix4f("kinectWorldMatrix",transposedKinectWorldMatrix);
    elevationShader.setUniform2f("contourLineFboTransformation",ofVec2f(contourLineFboScale,contourLineFboOffset));
    elevationShader.setUniform2f("depthTransformation",ofVec2f(FilteredDepthScale,FilteredDepthOffset));
    elevationShader.setUniform4f("basePlaneEq", basePlaneEq);
    mesh.draw();
    elevationShader.end();
    kinectProjector->unbind();
    contourLineFramebufferObject.end();
}

void SandSurfaceRenderer::setupGui(){
    // instantiate the modal windows //
    auto theme = make_shared<ofxModalThemeProjKinect>();
    saveModal = make_shared<SaveModal>(theme);
    saveModal->addListener(this, &SandSurfaceRenderer::onSaveModalEvent);
    
    // instantiate the gui //
    gui = new ofxDatGui( ofxDatGuiAnchor::TOP_LEFT );
    gui->addToggle("Contour lines", drawContourLines);
    gui->addSlider("Contour lines distance", 1, 30, contourLineDistance);
    gui->addBreak();
    gui->addButton("Color #1")->setName("ColorName");
    gui->addSlider("Height", -300, 300, 0);
    gui->addColorPicker("ColorPicker", ofColor::black);
    gui->addButton("Move up");
    gui->addButton("Move down");
    gui->addButton("Insert new color after current color");
    gui->addButton("Remove color");
    gui->addButton("Reset colors");
    gui->addDropdown("Load Color Map", colorMapFilesList);
    gui->addButton("Save color map");
    gui->expand();
    gui->addHeader(":: Display ::", false);
    
    // once the gui has been assembled, register callbacks to listen for component specific events //
    gui->onButtonEvent(this, &SandSurfaceRenderer::onButtonEvent);
    gui->onToggleEvent(this, &SandSurfaceRenderer::onToggleEvent);
    gui->onSliderEvent(this, &SandSurfaceRenderer::onSliderEvent);
    gui->onColorPickerEvent(this, &SandSurfaceRenderer::onColorPickerEvent);
    gui->onDropdownEvent(this, &SandSurfaceRenderer::onDropdownEvent);
    
    int pos = find(colorMapFilesList.begin(), colorMapFilesList.end(), colorMapFile) - colorMapFilesList.begin();
    if (pos < colorMapFilesList.size())
        gui->getDropdown("Load Color Map")->select(pos);
    
    // add a scroll view to list colors //
    colorList = new ofxDatGuiScrollView("Colors", 8);
    colorList->setPosition(gui->getPosition().x, gui->getPosition().y+gui->getHeight());
    colorList->onScrollViewEvent(this, &SandSurfaceRenderer::onScrollViewEvent);
    populateColorList();
}

void SandSurfaceRenderer::updateColorListColor(int i, int j){
    ofColor kc = heightMap[j].color;
    ofColor kb = kc;
    float st = (kc.getSaturation() > 10) ? kc.getSaturation()-10 : 0;
    float bt = (kc.getBrightness() < 245) ? kc.getBrightness()+10 : 255;
    kb.setSaturation(st);
    kb.setBrightness(bt);
    colorList->get(i)->setBackgroundColors(kc, kb, kb);
    colorList->get(i)->setLabelColor(kc.getInverted());
}

void SandSurfaceRenderer::populateColorList(){
    colorList->clear();
    for (int i = 0 ; i < heightMap.size() ; i++){
        int j = heightMap.size()-1-i;
        colorList->add("color");
        updateColorListColor(i, j);
        colorList->get(i)->setLabel("Height: "+ofToString(heightMap[j].height));
    }
    //Initiate color controls
    selectedColor = 0;
    int j = heightMap.size()-1;
    gui->getColorPicker("ColorPicker")->setColor(heightMap[j].color);
    gui->getSlider("Height")->setValue(heightMap[j].height);
    gui->getSlider("Height")->setMax(heightMap[j].height+100);
    gui->getSlider("Height")->setMin(heightMap[j-1].height);
    colorList->get(0)->setLabelAlignment(ofxDatGuiAlignment::CENTER);
}

void SandSurfaceRenderer::onButtonEvent(ofxDatGuiButtonEvent e){
    if (e.target->is("Save color map")) {
        saveModal->show();
    } else if (e.target->is("Reset colors")) {
        heightMap.loadFile(colorMapPath+colorMapFile);
        populateColorList();
    } else if (e.target->is("Insert new color after current color")){
        int i = heightMap.size();
        int j = heightMap.size()-1-selectedColor;
        float newheight = (j > 0) ? (heightMap[j-1].height+heightMap[j].height)/2 : heightMap[j].height+1;
        colorList->add("color");
        updateColorListColor(i, j);
        colorList->get(i)->setLabel("Height: "+ofToString(newheight));
        colorList->move(i,selectedColor+1);
        heightMap.addKey(heightMap[j].color, newheight);
        onScrollViewEvent(ofxDatGuiScrollViewEvent(colorList, colorList->get(selectedColor+1), selectedColor+1));
    } else if (e.target->is("Remove color")){
        if (heightMap.size() > 1){
            int j = heightMap.size()-1-selectedColor;
            heightMap.removeKey(j);
            colorList->remove(selectedColor);
            int i = selectedColor;
            if (i == heightMap.size())
                i -= 1;
            selectedColor += 1; // To get i != selectedColor => update
            onScrollViewEvent(ofxDatGuiScrollViewEvent(colorList, colorList->get(i), i));
        }
    } else if (e.target->is("Move up")){
        int i = selectedColor;
        int j = heightMap.size()-1-i;
        if (i>0){
            heightMap.swapKeys(j, j+1);
            updateColorListColor(i, j);
            updateColorListColor(i-1, j+1);
            onScrollViewEvent(ofxDatGuiScrollViewEvent(colorList, colorList->get(i-1), i-1));
       }
    } else if (e.target->is("Move down")){
        int i = selectedColor;
        int j = heightMap.size()-1-i;
        if (j>0){
            heightMap.swapKeys(j, j-1);
            updateColorListColor(i, j);
            updateColorListColor(i+1, j-1);
            onScrollViewEvent(ofxDatGuiScrollViewEvent(colorList, colorList->get(i+1), i+1));
        }
    }
}

void SandSurfaceRenderer::onToggleEvent(ofxDatGuiToggleEvent e){
    if (e.target->is("Contour lines")) {
        drawContourLines = e.checked;
    }
}

void SandSurfaceRenderer::onColorPickerEvent(ofxDatGuiColorPickerEvent e){
    if (e.target->is("ColorPicker")) {
        int i = selectedColor;
        int j = heightMap.size()-1-i;
        heightMap.setColorKey(j, e.color);
        updateColorListColor(i, j);
    }
}

void SandSurfaceRenderer::onSliderEvent(ofxDatGuiSliderEvent e){
    if (e.target->is("Contour lines distance")) {
        contourLineDistance = e.value;
        contourLineFactor = contourLineFboScale/contourLineDistance;        
    } else if (e.target->is("Height")) {
        int i = selectedColor;
        int j = heightMap.size()-1-i;
        heightMap.setHeightKey(j, e.value);
        colorList->get(i)->setLabel("Height: "+ofToString(e.value));
    }
}

void SandSurfaceRenderer::onDropdownEvent(ofxDatGuiDropdownEvent e){
    colorMapFile = e.target->getLabel();
    heightMap.loadFile(colorMapPath+e.target->getLabel());
    populateColorList();
}

void SandSurfaceRenderer::onScrollViewEvent(ofxDatGuiScrollViewEvent e){
    int i = e.index;
    if (i != selectedColor){
        int j = heightMap.size()-1-i;
        e.target->setLabelAlignment(ofxDatGuiAlignment::CENTER);
        colorList->get(selectedColor)->setLabelAlignment(ofxDatGuiAlignment::LEFT);
        gui->getButton("ColorName")->setLabel("Color #"+ofToString(i+1));
        gui->getColorPicker("ColorPicker")->setColor(heightMap[j].color);
        ofxDatGuiSlider* hgt = gui->getSlider("Height");
        hgt->setMin(heightMap.getScalarRangeMin());
        hgt->setMax(heightMap.getScalarRangeMax());
        float nmax = (j < heightMap.size()-1) ? heightMap[j+1].height : heightMap[j].height+100;
        hgt->setMax(nmax);
        float nmin = (j > 0) ? heightMap[j-1].height : heightMap[j].height-100;
        hgt->setMin(nmin);
        hgt->setValue(heightMap[j].height);
        selectedColor = i;
    }
}

void SandSurfaceRenderer::onSaveModalEvent(ofxModalEvent e){
    if (e.type == ofxModalEvent::SHOWN){
        cout << "save modal window is open" << endl;
    }   else if (e.type == ofxModalEvent::HIDDEN){
        cout << "save modal window is closed" << endl;
    }   else if (e.type == ofxModalEvent::CANCEL){
        ofLogVerbose("SandSurfaceRenderer") << "save cancel button pressed: Aborting" ;
    }   else if (e.type == ofxModalEvent::CONFIRM){
        string filen = saveModal->getTextInput();
        std::size_t found = filen.find(".xml");
        if (found == std::string::npos)
            filen += ".xml";
        heightMap.saveFile(colorMapPath+filen);
        colorMapFilesList.push_back(filen);
        gui->getDropdown("Load Color Map")->setOptions(colorMapFilesList);
        ofLogVerbose("SandSurfaceRenderer") << "save confirm button pressed, filename: " << filen;
    }
}


//TODO: Save additionnal settings

bool SandSurfaceRenderer::loadSettings(){
    string settingsFile = "settings/sandSurfaceRendererSettings.xml";
    
    ofXml xml;
    if (!xml.load(settingsFile))
        return false;
    xml.setTo("SURFACERENDERERSETTINGS");
    colorMapFile = xml.getValue<string>("colorMapFile");
    drawContourLines = xml.getValue<bool>("drawContourLines");
    contourLineDistance = xml.getValue<float>("contourLineDistance");
    
    return true;
}

bool SandSurfaceRenderer::saveSettings(){
    string settingsFile = "settings/sandSurfaceRendererSettings.xml";

    ofXml xml;
    xml.addChild("SURFACERENDERERSETTINGS");
    xml.setTo("SURFACERENDERERSETTINGS");
    xml.addValue("colorMapFile", colorMapFile);
    xml.addValue("drawContourLines", drawContourLines);
    xml.addValue("contourLineDistance", contourLineDistance);
    xml.setToParent();
    return xml.save(settingsFile);
}


