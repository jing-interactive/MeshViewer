#define _HAS_STD_BYTE 0
#include <cinder/CameraUi.h>
#include <cinder/Log.h>
#include <cinder/app/App.h>
#include <cinder/app/RendererGl.h>
#include <cinder/gl/gl.h>
#include <cinder/ObjLoader.h>
#include <cinder/FileWatcher.h>
#include <cinder/Timer.h>

#include "CZipFileSystem.h"
#include "CVirtualFileSystem.h"

// vnm
#include "AssetManager.h"
#include "MiniConfig.h"
#include "FontHelper.h"
#include "GLHelper.h"

// melo
#include "melo.h"
//#include "GltfNode.h"
#include "NodeExt.h"
#include "FirstPersonCamera.h"

// imgui
#include "MiniConfigImgui.h"
#include "CinderGuizmo.h"
#include "DearLogger.h"

#include "ShadowMap.h"
#include "postprocess/FXAA.h"
#include "postprocess/SMAA.h"

#include "RenderDocHelper.h"

#include "NvOptimusEnablement.h"
#include "CinderRemotery.h"
#include "GltfNode.h"

using namespace ci;
using namespace ci::app;
using namespace std;
using namespace vfspp;

struct ScopedMarker
{
    ScopedMarker(const string& msg, bool sample_opengl = false)
    {
        if (!_REMOTERY_ENABLED) return;
        this->sample_opengl = sample_opengl;
        _rmt_BeginCPUSample(msg.c_str(), 0, NULL);
        if (sample_opengl)
        {
            gl::pushDebugGroup(msg);
            _rmt_BeginOpenGLSample(msg.c_str(), NULL);
        }
    }

    ~ScopedMarker()
    {
        if (!_REMOTERY_ENABLED) return;
        _rmt_EndCPUSample();
        if (sample_opengl)
        {
            gl::popDebugGroup();
            _rmt_EndOpenGLSample();
        }
    }
    bool sample_opengl = false;
};

struct AAPass
{
    unique_ptr<FXAA> mFXAA;
    unique_ptr<SMAA> mSMAA;
    gl::FboRef fboDest;

    void setup()
    {
        mFXAA = make_unique<FXAA>();
        mSMAA = make_unique<SMAA>();

        // TODO: disconnect old signals
        getWindow()->getSignalResize().connect([&] {
            gl::Texture2d::Format tfmt;
            tfmt.setMinFilter(GL_NEAREST);
            tfmt.setMagFilter(GL_NEAREST);
            tfmt.setInternalFormat(GL_RGBA8);
            fboDest = gl::Fbo::create(APP_WIDTH, APP_HEIGHT, gl::Fbo::Format().colorTexture(tfmt).disableDepth());
            fboDest->setLabel("fboDest");
        });
    }

    const gl::Texture2dRef draw(gl::FboRef fboSrc)
    {
        gl::ScopedDebugGroup group("fboDest");
        gl::disableAlphaBlending();
        mSMAA->apply(fboDest, fboSrc);

        return fboDest->getColorTexture();
    }
};

struct ShadowMapPass
{
    gl::GlslProgRef				mShadowShader;
    gl::GlslProgRef				mPassthroughShader;
    ShadowMapRef				mShadowMap;
    int							mShadowMapSize;
    bool						mOnlyShadowmap;

    LightData					mLight;

    int							mShadowTechnique;

    float						mDepthBias;
    bool						mEnableNormSlopeOffset;
    float						mRandomOffset;
    int							mNumRandomSamples;
    float						mPolygonOffsetFactor, mPolygonOffsetUnits;

    void setup()
    {
        mShadowMapSize = 2048;

        mLight.distanceRadius = 100.0f;
        mLight.viewpoint = vec3(mLight.distanceRadius);
        mLight.fov = 10.0f;
        mLight.target = vec3(0);
        mLight.toggleViewpoint = false;

        mShadowTechnique = 1;
        mDepthBias = -0.0005f;
        mRandomOffset = 1.2f;
        mNumRandomSamples = 32;
        mEnableNormSlopeOffset = false;
        mOnlyShadowmap = false;
        mPolygonOffsetFactor = mPolygonOffsetUnits = 3.0f;

        mShadowShader = am::glslProg("shadow_mapping.vert", "shadow_mapping.frag");

        mShadowMap = ShadowMap::create(mShadowMapSize);
        mLight.camera.setPerspective(mLight.fov, mShadowMap->getAspectRatio(), 0.5, 500.0);

        mPassthroughShader = am::glslProg("passthrough");

        if (false)
        App::get()->getSignalUpdate().connect([&] {
            ImGui::Begin("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Separator();
            ImGui::Checkbox("Light viewpoint", &mLight.toggleViewpoint);
            ImGui::DragFloat("Light distance radius", &mLight.distanceRadius, 1.0f, 0.0f, 450.0f);
            ImGui::Checkbox("Render only shadow map", &mOnlyShadowmap);
            ImGui::Separator();
            vector<string> techniques = { "Hard", "PCF3x3", "PCF4x4", "Random" };
            ImGui::Combo("Technique", &mShadowTechnique, techniques);
            ImGui::Separator();
            ImGui::DragFloat("Polygon offset factor", &mPolygonOffsetFactor, 0.025f, 0.0f);
            ImGui::DragFloat("Polygon offset units", &mPolygonOffsetUnits, 0.025f, 0.0f);
            if (ImGui::DragInt("Shadow map size", &mShadowMapSize, 16, 16, 2048)) {
                mShadowMap->reset(mShadowMapSize);
            };
            ImGui::DragFloat("Depth bias", &mDepthBias, 0.00001f, -1.0f, 0.0f, "%.5f");
            ImGui::Text("(PCF radius is const: tweak in shader.)");
            ImGui::Separator();
            ImGui::Text("Random sampling params");
            ImGui::DragFloat("Offset radius", &mRandomOffset, 0.05f, 0.0f);
            ImGui::Checkbox("Auto normal slope offset", &mEnableNormSlopeOffset);
            ImGui::DragInt("Num samples", &mNumRandomSamples, 1.0f, 1, 256);
            ImGui::End();
        });
    }

    const gl::Texture2dRef& draw(melo::NodeRef scene)
    {
        gl::ScopedDepth enableDepthRW(true);
        ScopedMarker scp("shadowMap", true);

        // Offset to help combat surface acne (self-shadowing)
        gl::ScopedState enable(GL_POLYGON_OFFSET_FILL, GL_TRUE);
        glPolygonOffset(mPolygonOffsetFactor, mPolygonOffsetUnits);

        // Render scene into shadow map
        gl::ScopedMatrices setMatrices(mLight.camera);
        gl::ScopedViewport viewport(mShadowMap->getSize());
        {
            gl::ScopedFramebuffer bindFbo(mShadowMap->getFbo());
            gl::ScopedGlslProg glsl(mPassthroughShader);
            gl::clear();
            scene->treeDraw(melo::DRAW_SHADOW);
        }

        return mShadowMap->getTexture();
    }
};

struct MeloViewer : public App
{
    RenderDocHelper mRdc;
    bool mToCaptureRdc = false;
    Remotery* rmt = nullptr;

    CameraPersp2 mMayaCam;
    CameraUi mMayaCamUi;
    FirstPersonCamera mFpsCam;
    CameraPersp2* mCurrentCam = nullptr;
    bool mIsFpsCamera = false;

    // args
    bool mSnapshotMode = false;
    string mOutputFilename;

    AAPass mAAPass;
    ShadowMapPass mShadowMapPass;

    gl::FboRef mFboMain;
    gl::GlslProgRef mGlslProg;
    int mMeshFileId = -1;
    vector<string> mMeshFilenames;

    melo::NodeRef mScene;
    melo::NodeRef mSkyNode;
    melo::DirectionalLightNode::Ref mLightNode;
    melo::NodeRef mGridNode;

    melo::NodeRef mPickedNode, mMouseHitNode;
    //AnimationGLTF::Ref mPickedAnimation;
    mat4 mPickedTransform;

    FileWatcher mFileWatcher;

    shared_ptr<ImGui::DearLogger>  mUiLogger;

    void createDefaultScene()
    {
        mScene = melo::createRootNode();

        mSkyNode = melo::createSkyNode(RADIANCE_TEX);
        mScene->addChild(mSkyNode);

        mGridNode = melo::createGridNode(100.0f);
        mScene->addChild(mGridNode);

        mLightNode = melo::DirectionalLightNode::create(1, { 0.5, 0.5, 0.5 });
        mLightNode->setPosition({ 10,10,10 });
        mScene->addChild(mLightNode);
    }

    void deletePickedNode()
    {
        if (!mPickedNode) return;

        dispatchAsync([&] {
            mScene->removeChild(mPickedNode);
            setPickedNode(nullptr);
            });
    }

    void lookAtPickedNode()
    {
        if (!mPickedNode) return;
        AxisAlignedBox localBounds = { mPickedNode->mBoundBoxMin, mPickedNode->mBoundBoxMax };
        AxisAlignedBox worldBounds = localBounds.transformed(mPickedNode->getWorldTransform());
        mCurrentCam->lookAt(worldBounds.getCenter());
    }

    void drawSceneWidget()
    {
        ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
        if (ImGui::BeginTabBar("SceneTab", tab_bar_flags))
        {
            if (ImGui::BeginTabItem("Hierachy"))
            {
                auto path = getAppPath() / "melo.scene";
                if (ImGui::Button("New"))
                {
                    createDefaultScene();
                    setPickedNode(nullptr);
                }

                ImGui::SameLine();
                if (ImGui::Button("Load"))
                {
                    auto newScene = melo::loadScene(path.generic_string());
                    if (newScene)
                    {
                        mScene = newScene;
                        setPickedNode(nullptr);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Save"))
                {
                    melo::writeScene(mScene, path.generic_string());
                }

                if (ImGui::Button("+ Cube"))
                {
                    auto node = melo::createMeshNode("Cube");
                    mScene->addChild(node);
                    setPickedNode(node);
                }

                ImGui::SameLine();
                if (ImGui::Button("+ Sphere"))
                {
                    auto node = melo::createMeshNode("Sphere");
                    mScene->addChild(node);
                    setPickedNode(node);
                }

                ImGui::Separator();

                static int debugType = DEBUG_NONE;
#define ENTRY(flag, name) #name,
                vector<string> debugTypes = {
                    DebugTypeList
                };
#undef ENTRY
                if (ImGui::Combo("Debug Type", &debugType, debugTypes))
                {
                    if (mPickedNode)
                    {
                        auto gltfScene = (GltfScene*)mPickedNode.get();
                        gltfScene->createMaterials((DebugType)debugType);
                    }
                }

                // selectable list
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                applyTreeUI(mScene);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                vnm::drawFrameTime();
                if (RENDER_DOC_ENABLED)
                {
                    if (ImGui::Button("Capture RenderDoc"))
                    {
                        mToCaptureRdc = true;
                    }
                }
                vnm::drawMinicofigImgui();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void drawNodeWidget()
    {
        ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
        if (ImGui::BeginTabBar("NodeTab", tab_bar_flags))
        {
            if (ImGui::BeginTabItem("Property"))
            {
                //ImGui::ScopedGroup group;
                if (!mIsFpsCamera)
                {
                    ImGui::Text(mPickedNode->getName().c_str());
                    bool isVisible = mPickedNode->isVisible();
                    if (ImGui::Checkbox("Visible", &isVisible))
                    {
                        mPickedNode->setVisible(isVisible);
                    }
                    if (ImGui::Button("Reset Transform"))
                    {
                        mPickedTransform = {};
                        mPickedNode->setTransform({});
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("DEL"))
                    {
                        deletePickedNode();
                    }
                    if (ImGui::Button("CLONE"))
                    {
                        dispatchAsync([&] {
                            // TODO: 
                            auto cloned = melo::create(mPickedNode->getName());
                            vec3 T, R, S;
                            ImGui::DecomposeMatrixToComponents(mPickedTransform, T, R, S);
                            cloned->setPosition(T);
                            cloned->setRotation(R);
                            cloned->setScale(S);
                            cloned->setTransform(mPickedTransform);
                            mScene->addChild(cloned);
                            setPickedNode(cloned);
                            });
                    }

                    ImGui::EnableGizmo(!mMouseBeingDragged);
                    if (ImGui::EditGizmo(mCurrentCam->getViewMatrix(), mCurrentCam->getProjectionMatrix(), &mPickedTransform))
                    {
                        mMayaCamUi.disable();
                        vec3 T, R, S;
                        ImGui::DecomposeMatrixToComponents(mPickedTransform, T, R, S);
                        mPickedNode->setPosition(T);
                        mPickedNode->setRotation(R);
                        mPickedNode->setScale(S);
                        mPickedNode->setTransform(mPickedTransform);
                    }
                    else
                    {
                        mMayaCamUi.enable();
                    }
                }

#if 0
                ImGui::NewLine();
                ImGui::Text("Animation");
                if (mPickedNode->getName().find(".gltf") != string::npos)
                {
                    auto gltfNode = (ModelGLTF*)mPickedNode.get();
                    for (auto& anim : gltfNode->animations)
                    {
                        if (ImGui::Button(anim->name.c_str()))
                        {
                            mPickedAnimation = anim;
                            anim->startAnimation();
                        }
                    }

                    if (mPickedAnimation)
                    {
                        AnimatedValues values;
                        mPickedAnimation->getAnimatedValues(&values);
                        ImGui::Text("Time: %.2f / %.2f s", mPickedAnimation->animTime.value(), mPickedAnimation->animTime.getParent()->getDuration());
                        if (values.T_animated)
                        {
                            ImGui::DragFloat3("T", &values.T);
                            mPickedNode->setPosition(values.T);
                        }
                        if (values.R_animated)
                        {
                            ImGui::DragFloat4("R", (vec4*)&values.R);
                            mPickedNode->setRotation(values.R);
                        }
                        if (values.S_animated)
                        {
                            ImGui::DragFloat3("S", &values.S);
                            mPickedNode->setScale(values.S);
                        }
                        mPickedTransform = mPickedNode->getWorldTransform();
                    }
                }
#endif
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void drawGUI()
    {
        mUiLogger->Draw("Log");

        if (ImGui::Begin("Scene", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            drawSceneWidget();
            ImGui::End();
        }

        if (mPickedNode && ImGui::Begin("Node", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            drawNodeWidget();
            ImGui::End();
        }
    }

    vector<string> listGlTFFiles()
    {
        vector<string> files;
        auto assetModel = (getAppPath() / "../assets").generic_string();
        for (auto& p :
            fs::recursive_directory_iterator(assetModel
#ifdef CINDER_MSW_DESKTOP
                ,
                fs::directory_options::follow_directory_symlink
#endif
            ))
        {
            if (melo::isMeshPathSupported(p.path()))
            {
                auto filename = p.path().generic_string();
                filename.replace(filename.find(assetModel),
                    assetModel.size() + 1,
                    ""); // Left trim the assets prefix

                files.push_back(filename);
            }
        }

        return files;
    }

    void setPickedNode(melo::NodeRef newNode)
    {
        mPickedNode = newNode;
        if (newNode)
        {
            mPickedTransform = newNode->getTransform();
        }
    }

    void applyTreeUI(const melo::NodeRef& node)
    {
        if (!node) return;

        ImGuiTreeNodeFlags flag = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        flag |= node->getChildren().empty() ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_None;
        if (node == mPickedNode)
            flag |= ImGuiTreeNodeFlags_Selected;

        if (ImGui::TreeNodeEx(node->getName().c_str(), flag))
        {
            if (ImGui::IsItemClicked())
                setPickedNode(node);
            for (auto& child : node->getChildren())
                applyTreeUI(child);
            ImGui::TreePop();
        }
        else
        {
            if (ImGui::IsItemClicked())
                setPickedNode(node);
        }
    };

    bool mMouseBeingDragged = false;

    void setup() override
    {
        if (RENDER_DOC_ENABLED)
            mRdc.setup();

        log::makeLogger<log::LoggerFileRotating>(fs::path(), "melo-%Y-%m-%d.log");
        mUiLogger = log::makeLogger<ImGui::DearLogger>();

        CI_LOG_I("========================================================");

        am::addAssetDirectory(getAppPath() / "../assets");
        am::addAssetDirectory(getAppPath() / "../../assets");
        am::addAssetDirectory(getAppPath() / "../../../assets");

        rmtError error;
        if (_REMOTERY_ENABLED)
        {
            error = rmt_CreateGlobalInstance(&rmt);
            _rmt_SetCurrentThreadName("master");
            _rmt_BindOpenGL();
            if (RMT_ERROR_NONE != error) {
                CI_LOG_V("Error launching Remotery" << error);
            }
        }

        getSignalCleanup().connect([&] {
            if (rmt)
            {
                _rmt_UnbindOpenGL();
                rmt_DestroyGlobalInstance(rmt);
            }
        });

        mMayaCam.lookAt({ CAM_POS_X, CAM_POS_Y, CAM_POS_Z }, { CAM_DIR_X, CAM_DIR_Y, CAM_DIR_Z }, vec3(0, 1, 0));
        mFpsCam.lookAt({ CAM_POS_X, CAM_POS_Y, CAM_POS_Z }, { CAM_DIR_X, CAM_DIR_Y, CAM_DIR_Z }, vec3(0, 1, 0));
        mFpsCam.setEyePoint({ CAM_POS_X, CAM_POS_Y, CAM_POS_Z });
        mFpsCam.setViewDirection({ CAM_DIR_X, CAM_DIR_Y, CAM_DIR_Z });
        mMayaCamUi = CameraUi(&mMayaCam, getWindow(), -1);
        mMayaCamUi.setMouseWheelMultiplier(1.05f);
        mFpsCam.setup();

        GltfScene::radianceTexture = am::textureCubeMap(RADIANCE_TEX);
        GltfScene::irradianceTexture = am::textureCubeMap(IRRADIANCE_TEX);
        GltfScene::brdfLUTTexture = am::texture2d(BRDF_LUT_TEX);

        createDefaultScene();

        mAAPass.setup();
        mShadowMapPass.setup();

        mMeshFilenames = listGlTFFiles();
        parseArgs();

        createConfigImgui(getWindow(), false);
        //ADD_ENUM_TO_INT(mParams.get(), MESH_FILE_ID, mMeshFilenames);
        //mParams->addParam("MESH_ROTATION", &mMeshRotation);

        getSignalCleanup().connect([&] { writeConfig(); });

        getWindow()->getSignalResize().connect([&] {
            APP_WIDTH = getWindowWidth();
            APP_HEIGHT = getWindowHeight();
            mMayaCam.setAspectRatio(getWindowAspectRatio());

            mFboMain = gl::Fbo::create(APP_WIDTH, APP_HEIGHT, gl::Fbo::Format().colorTexture(gl::Texture2d::Format()));
            mFboMain->setLabel("mFboMain");
        });

        getWindow()->getSignalMouseDown().connect([&](MouseEvent& event) {
            mMouseBeingDragged = false;
            });

        getWindow()->getSignalMouseDrag().connect([&](MouseEvent& event) {
            mMouseBeingDragged = true;
            });

        getWindow()->getSignalMouseMove().connect([&](MouseEvent& event) {
            mMouseHitNode = pick(mScene, *mCurrentCam, event.getPos());
            });

        getWindow()->getSignalMouseUp().connect([&](MouseEvent& event) {
            ImGuiIO& io = ImGui::GetIO();
            if (!io.WantCaptureMouse)
            {
                if (event.isLeft() && !mMouseBeingDragged) {
                    auto hit = mMouseHitNode;
                    dispatchAsync([&, hit] {
                        setPickedNode(hit);
                        });
                }
            }

            mMouseBeingDragged = false;
            });
        if (!mSnapshotMode)
        {
            getWindow()->getSignalKeyUp().connect([&](KeyEvent& event) {
                auto code = event.getCode();
                switch (code)
                {
                case KeyEvent::KEY_ESCAPE:
                    quit(); break;
                case KeyEvent::KEY_w:
                    WIRE_FRAME = !WIRE_FRAME; break;
                case KeyEvent::KEY_e:
                    ENV_VISIBLE = !ENV_VISIBLE; break;
                case KeyEvent::KEY_x:
                    XYZ_VISIBLE = !XYZ_VISIBLE; break;
                case KeyEvent::KEY_g:
                    GUI_VISIBLE = !GUI_VISIBLE; break;
                case KeyEvent::KEY_RETURN:
                    setFullScreen(!isFullScreen()); break;
                case KeyEvent::KEY_f:
                    FPS_CAMERA = !FPS_CAMERA; break;
                case KeyEvent::KEY_DELETE:
                    deletePickedNode(); break;
                case KeyEvent::KEY_SPACE:
                    lookAtPickedNode(); break;
                default:
                    break;
                }
                });
        }

        getWindow()->getSignalFileDrop().connect([&](FileDropEvent& event) {

            static auto imageExts = ImageIo::getLoadExtensions();

            for (auto& filePath : event.getFiles())
            {
                if (fs::is_directory(filePath)) continue;
                if (!filePath.has_extension()) continue;

                if (melo::isMeshPathSupported(filePath))
                {
                    dispatchAsync([&, filePath] {
                        loadMeshFromFile(filePath);
                        });
                    break;
                }

                auto ext = filePath.extension().string();
                if (ext == ".zip")
                {
                    dispatchAsync([&, filePath] {
                        loadMeshFromZip(filePath);
                        });
                    break;
                }

                bool isImageType = find(imageExts.begin(), imageExts.end(), ext) != imageExts.end();
                if (isImageType)
                {
                    break;
                }
            }
            });

        getSignalUpdate().connect([&] {

            ScopedMarker scp("update", false);

            mSkyNode->setVisible(ENV_VISIBLE);
            mGridNode->setVisible(XYZ_VISIBLE);

            if (mIsFpsCamera != FPS_CAMERA)
            {
                if (FPS_CAMERA)
                {
                    mFpsCam.setActive(true);
                    mFpsCam.setEyePoint(mMayaCam.getEyePoint());
                    mFpsCam.setViewDirection(mMayaCam.getViewDirection());
                    //mFpsCam.look = mMayaCam.getPivotPoint();
                }
                else
                {
                    mFpsCam.setActive(false);
                    mMayaCam.lookAt(mFpsCam.getEyePoint(), mMayaCam.getPivotPoint());
                    mMayaCam.setViewDirection(mFpsCam.getViewDirection());
                    mMayaCam.setWorldUp(mFpsCam.getWorldUp());
                }
                mIsFpsCamera = FPS_CAMERA;
            }

            if (FPS_CAMERA)
            {
                mCurrentCam = &mFpsCam;
            }
            else
            {
                mCurrentCam = &mMayaCam;
            }
            CAM_POS_X = mCurrentCam->getEyePoint().x;
            CAM_POS_Y = mCurrentCam->getEyePoint().y;
            CAM_POS_Z = mCurrentCam->getEyePoint().z;
            CAM_DIR_X = mCurrentCam->getViewDirection().x;
            CAM_DIR_Y = mCurrentCam->getViewDirection().y;
            CAM_DIR_Z = mCurrentCam->getViewDirection().z;
            mCurrentCam->setNearClip(CAM_Z_NEAR);
            mCurrentCam->setFarClip(CAM_Z_FAR);

            mShadowMapPass.mLight.camera.lookAt(mLightNode->getPosition(), { 0,0,0 });

#if 0
            Frustumf frustrum(*mCurrentCam);

            for (auto& child : mScene->getChildren())
            {
                //mModel->flipV = FLIP_V;
                child->setVisible(child->isInsideFrustrum(frustrum));
            }
#endif
            if (GUI_VISIBLE)
            {
                ScopedMarker scp("drawGUI", false);
                drawGUI();
            }

            if (!mIsFpsCamera && mPickedNode != nullptr)
            {
                auto k = 128;
                auto pos = ivec2(getWindowWidth() - k, 0);
                auto size = ivec2(k, k);
                if (ImGui::ViewManipulate(mCurrentCam->getViewMatrixReference(), 8, pos, size))
                {
                    mMayaCamUi.disable();
                }
                else
                {
                    mMayaCamUi.enable();
                }
            }

            mScene->treeUpdate();
            });

        getWindow()->getSignalDraw().connect([&] {
            ScopedMarker scp(string("f") + toString(getElapsedFrames()), true);

            if (mToCaptureRdc)
                mRdc.startCapture();

            auto texShadowMap = mShadowMapPass.draw(mScene);

            {
                // main pass
                ScopedMarker scp("mFboMain", true);
                gl::ScopedFramebuffer fbo(mFboMain);
                if (mSnapshotMode)
                    gl::clear(ColorA::gray(0.0f, 0.0f));
                else
                    gl::clear(ColorA::gray(0.2f, 1.0f));

                gl::enableDepth();
                gl::context()->depthFunc(GL_LEQUAL);

                gl::setWireframeEnabled(WIRE_FRAME);

                if (mIsFpsCamera)
                    gl::setMatrices(mFpsCam);
                else
                    gl::setMatrices(mMayaCam);

                {
                    ScopedMarker scp("solid", true);

                    gl::enableDepthRead();
                    gl::disableAlphaBlending();
                    mScene->treeDraw(melo::DRAW_SOLID);
                }
                
                {
                    ScopedMarker scp("transparency", true);

                    gl::enableAlphaBlending();
                    gl::disableDepthRead();
                    mScene->treeDraw(melo::DRAW_TRANSPARENCY);
                }

                gl::disableWireframe();

                //gl::disable(GL_POLYGON_OFFSET_FILL);

                gl::enableDepthRead();
                if (mMouseHitNode)
                {
                    melo::drawBoundingBox(mMouseHitNode);
                }

                if (mPickedNode)
                {
                    melo::drawBoundingBox(mPickedNode, Color(1, 0, 0));
                }
            }

            auto blitTexture = mFboMain->getColorTexture();
            if (IS_SMAA)
            {
                ScopedMarker scp("SMAA", true);
                blitTexture = mAAPass.draw(mFboMain);
            }

            {
                // blit
                gl::disableDepthRead();
                gl::setMatricesWindow(getWindowSize());
                gl::draw(blitTexture, getWindowBounds());
            }

            if (mSnapshotMode)
            {
                auto windowSurf = copyWindowSurfaceWithAlpha();
                writeImage(mOutputFilename, windowSurf);
                quit();
            }

            if (mToCaptureRdc)
            {
                mRdc.endCapture();
                mToCaptureRdc = false;
            }
            });
    }

    void loadMeshFromFile(fs::path path)
    {
        Timer timer(true);
        
        auto newModel = GltfScene::create(path);
        if (newModel)
        {
            mScene->addChild(newModel);
            setPickedNode(newModel);
        }
        CI_LOG_I(path << " loaded in " << timer.getSeconds() << " seconds");
    }

    bool loadMeshFromZip(const fs::path& filePath)
    {
        {
            vfs_initialize();

            IFileSystemPtr zip_fs(new CZipFileSystem(filePath.string().c_str(), "/"));
            zip_fs->Initialize();

            CVirtualFileSystemPtr vfs = vfs_get_global();
            vfs->AddFileSystem("/", zip_fs);

            IFilePtr memFile = vfs->openFile(CFileInfo("/scene.gltf"), IFile::In);
            if (memFile && memFile->IsOpened())
            {
                loadMeshFromFile(filePath);
                char data[256];
                memFile->Read(reinterpret_cast<uint8_t*>(data), 256);

                printf("%s\n", data);
            }

            vfs_shutdown();
        }
#if 0
        miniz_zip_archive zip_archive;
        memset(&zip_archive, 0, sizeof(zip_archive));
        miniz_bool status = miniz_zip_reader_init_file(&zip_archive, filePath.string().c_str(), 0);
        if (!status)
        {
            CI_LOG_V("miniz_zip_reader_init_file() failed!");
            return false;
        }

        // Get and print information about each file in the archive.
        for (int i = 0; i < (int)miniz_zip_reader_get_num_files(&zip_archive); i++)
        {
            miniz_zip_archive_file_stat file_stat;
            if (!miniz_zip_reader_file_stat(&zip_archive, i, &file_stat))
            {
                CI_LOG_V("miniz_zip_reader_file_stat() failed!");
                miniz_zip_reader_end(&zip_archive);
                return false;
            }

            CI_LOG_V("Filename: " << file_stat.m_filename <<
                ", Uncompressed size: " << file_stat.m_uncomp_size <<
                ", Compressed size: " << file_stat.m_comp_size <<
                ", Is Dir: " << miniz_zip_reader_is_file_a_directory(&zip_archive, i));

            if (!strcmp(file_stat.m_filename, "textures/"))
            {
                if (!miniz_zip_reader_is_file_a_directory(&zip_archive, i))
                {
                    CI_LOG_V("miniz_zip_reader_is_file_a_directory() didn't return the expected results!");
                    miniz_zip_reader_end(&zip_archive);
                    return false;
                }
            }
        }

        // Close the archive, freeing any resources it was using
        miniz_zip_reader_end(&zip_archive);
        loadMeshFromFile(filePath);
#endif
        return true;
    }

    void parseArgs()
    {
        auto& args = getCommandLineArgs();

        if (args.size() > 1)
        {
            // /path/to/MeloViewer.exe file.obj
            auto filePath = fs::path(args[1]);
            if (melo::isMeshPathSupported(filePath))
            {
                dispatchAsync([&, filePath] {
                    loadMeshFromFile(filePath);
                });
            }
            else
            {
                auto ext = filePath.extension().string();
                if (ext == ".zip")
                {
                    dispatchAsync([&, filePath] {
                        loadMeshFromZip(filePath);
                    });
                }
            }

            if (args.size() > 2)
            {
                // MeloViewer.exe file.obj snapshot.png
                mSnapshotMode = true;
                GUI_VISIBLE = false;
                WIRE_FRAME = false;
                mOutputFilename = args[2];

                if (args.size() > 3)
                {
                    // MeloViewer.exe file.obj snapshot.png new_shining_texture.png
                }
            }
        }
    }
};

// TODO: move GltfScene::update and GltfNode::draw to GltfNode.cpp
void GltfScene::update(double elapsed)
{
    auto app = (MeloViewer*)App::get();
    for (auto& light : lights)
    {
        light.direction = - glm::normalize(app->mLightNode->getPosition());
        light.color= app->mLightNode->color;
        light.intensity = LIGHT0_INTENSITY;
    }

    if (isMaterialDirty)
        App::get()->dispatchAsync([this] {
        isMaterialDirty = false;
    });
}

void GltfNode::draw(melo::DrawOrder order)
{
    unique_ptr<ScopedMarker> scp;
    if (PROFILE_NODE_DRAW)
    {
        scp = make_unique<ScopedMarker>("nodeDraw", true);
    }

    auto app = (MeloViewer*)App::get();
    if (scene->isMaterialDirty)
    {
       reloadMaterial();
    }
    if (material && material->glsl)
    {
        mat3 rotMatrix3 = {};
        material->glsl->uniform("u_envRotation", rotMatrix3);
        material->glsl->uniform("u_Camera", app->mCurrentCam->getEyePoint());
        material->glsl->uniform("u_Exposure", EXPOSURE);
        material->glsl->uniform("u_MipCount", IBL_MIP);
        material->glsl->uniform("u_Lights[0].direction", scene->lights[0].direction);
        material->glsl->uniform("u_Lights[0].range", scene->lights[0].range);
        material->glsl->uniform("u_Lights[0].color", scene->lights[0].color);
        material->glsl->uniform("u_Lights[0].intensity", scene->lights[0].intensity);
        material->glsl->uniform("u_Lights[0].position", scene->lights[0].position);
        material->glsl->uniform("u_Lights[0].innerConeCos", scene->lights[0].innerConeCos);
        material->glsl->uniform("u_Lights[0].outerConeCos", scene->lights[0].outerConeCos);
        material->glsl->uniform("u_Lights[0].type", scene->lights[0].type);
        material->bind();
        gl::draw(mesh);
        material->unbind();
    }
    else
    {
        static auto glsl = am::glslProg("lambert");
        gl::ScopedGlslProg scopedGlsl(glsl);
        gl::draw(mesh);
    }
}

void preSettings(App::Settings* settings)
{
    readConfig();
#if defined( CINDER_MSW_DESKTOP )
    settings->setConsoleWindowEnabled(CONSOLE_ENABLED);
#endif
    settings->setWindowSize(APP_WIDTH, APP_HEIGHT);
    settings->setMultiTouchEnabled(false);
}

#if !defined(NDEBUG) && defined(CINDER_MSW)
auto gfxOption = RendererGl::Options().msaa(0).debug().debugLog(GL_DEBUG_SEVERITY_MEDIUM);// .debugBreak(GL_DEBUG_SEVERITY_HIGH);
#else
auto gfxOption = RendererGl::Options().msaa(0);
#endif
CINDER_APP(MeloViewer, RendererGl(gfxOption), preSettings)