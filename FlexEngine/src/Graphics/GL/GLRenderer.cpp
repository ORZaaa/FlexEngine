#include "stdafx.hpp"
#if COMPILE_OPEN_GL

#include "Graphics/GL/GLRenderer.hpp"

IGNORE_WARNINGS_PUSH
#include <BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h>

#include <glm/gtx/quaternion.hpp> // for rotate

#if COMPILE_IMGUI
#include "imgui_internal.h" // For columns API

#include "ImGui/imgui_impl_glfw.h"
#include "ImGui/imgui_impl_opengl3.h"
#endif

#include <freetype/ftbitmap.h>
IGNORE_WARNINGS_POP

#include "Cameras/BaseCamera.hpp"
#include "Cameras/CameraManager.hpp"
#include "FlexEngine.hpp"
#include "Graphics/BitmapFont.hpp"
#include "Graphics/GL/GLHelpers.hpp"
#include "Graphics/GL/GLPhysicsDebugDraw.hpp"
#include "Graphics/VertexAttribute.hpp"
#include "Helpers.hpp"
#include "InputManager.hpp"
#include "JSONParser.hpp"
#include "JSONTypes.hpp"
#include "Physics/PhysicsWorld.hpp"
#include "Physics/RigidBody.hpp"
#include "Profiler.hpp"
#include "Scene/BaseScene.hpp"
#include "Scene/GameObject.hpp"
#include "Scene/MeshComponent.hpp"
#include "Scene/LoadedMesh.hpp"
#include "Scene/SceneManager.hpp"
#include "Window/GLFWWindowWrapper.hpp"
#include "Window/Monitor.hpp"
#include "Window/Window.hpp"

namespace flex
{
	namespace gl
	{
		GLRenderer::GLRenderer() :
			m_KeyEventCallback(this, &GLRenderer::OnKeyEvent),
			m_ActionCallback(this, &GLRenderer::OnActionEvent)
		{
		}

		GLRenderer::~GLRenderer()
		{
		}

		void GLRenderer::Initialize()
		{
			Renderer::Initialize();

			LoadSettingsFromDisk();

			SetVSyncEnabled(g_Window->GetVSyncEnabled());

			g_InputManager->BindKeyEventCallback(&m_KeyEventCallback, 13);
			g_InputManager->BindActionCallback(&m_ActionCallback, 13);

			m_OffscreenTexture0Handle = {};
			m_OffscreenTexture0Handle.internalFormat = GL_RGBA16F;
			m_OffscreenTexture0Handle.format = GL_RGBA;
			m_OffscreenTexture0Handle.type = GL_FLOAT;

			m_OffscreenTexture1Handle = {};
			m_OffscreenTexture1Handle.internalFormat = GL_RGBA16F;
			m_OffscreenTexture1Handle.format = GL_RGBA;
			m_OffscreenTexture1Handle.type = GL_FLOAT;


			m_ShadowMapTexture = {};
			m_ShadowMapTexture.internalFormat = GL_DEPTH_COMPONENT;
			m_ShadowMapTexture.format = GL_DEPTH_COMPONENT;
			m_ShadowMapTexture.type = GL_FLOAT;

			m_gBuffer_PositionMetallicHandle = {};
			m_gBuffer_PositionMetallicHandle.internalFormat = GL_RGBA16F;
			m_gBuffer_PositionMetallicHandle.format = GL_RGBA;
			m_gBuffer_PositionMetallicHandle.type = GL_FLOAT;

			m_gBuffer_NormalRoughnessHandle = {};
			m_gBuffer_NormalRoughnessHandle.internalFormat = GL_RGBA16F;
			m_gBuffer_NormalRoughnessHandle.format = GL_RGBA;
			m_gBuffer_NormalRoughnessHandle.type = GL_FLOAT;

			m_gBuffer_AlbedoAOHandle = {};
			m_gBuffer_AlbedoAOHandle.internalFormat = GL_RGBA16F;
			m_gBuffer_AlbedoAOHandle.format = GL_RGBA;
			m_gBuffer_AlbedoAOHandle.type = GL_FLOAT;

			LoadShaders();

			glEnable(GL_DEPTH_TEST);
			glClearDepth(0.0f);

			glFrontFace(GL_CW);
			glLineWidth(3.0f);

			// Prevent seams from appearing on lower mip map levels of cubemaps
			glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

			assert(GL_VERSION_4_5);
			// TODO: Handle lack of GL_ARB_clip_control (in GL < 4.5)
			glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);


			// Capture framebuffer (TODO: Merge with offscreen frame buffer?)
			{
				glGenFramebuffers(1, &m_CaptureFBO);
				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);

				glGenRenderbuffers(1, &m_CaptureRBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, 512, 512); // TODO: Remove 512
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_CaptureRBO);

				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				{
					PrintError("Capture frame buffer is incomplete!\n");
				}
			}

			// Offscreen framebuffers
			{
				glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
				CreateOffscreenFrameBuffer(&m_Offscreen0FBO, &m_Offscreen0RBO, frameBufferSize, m_OffscreenTexture0Handle);
				CreateOffscreenFrameBuffer(&m_Offscreen1FBO, &m_Offscreen1RBO, frameBufferSize, m_OffscreenTexture1Handle);
			}

			const real captureProjectionNearPlane = g_CameraManager->CurrentCamera()->GetZNear();
			const real captureProjectionFarPlane = g_CameraManager->CurrentCamera()->GetZFar();
			m_CaptureProjection = glm::perspective(glm::radians(90.0f), 1.0f, captureProjectionFarPlane, captureProjectionNearPlane);
			m_CaptureViews =
			{
				glm::lookAt(VEC3_ZERO, glm::vec3(-1.0f, 0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
				glm::lookAt(VEC3_ZERO, glm::vec3(1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
				glm::lookAt(VEC3_ZERO, glm::vec3(0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
				glm::lookAt(VEC3_ZERO, glm::vec3(0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
				glm::lookAt(VEC3_ZERO, glm::vec3(0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
				glm::lookAt(VEC3_ZERO, glm::vec3(0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))
			};

			m_AlphaBGTextureID = InitializeTexture(RESOURCE_LOCATION  "textures/alpha-bg.png", 3, false, false, false);
			m_LoadingTextureID = InitializeTexture(RESOURCE_LOCATION  "textures/loading_1.png", 3, false, false, false);
			m_WorkTextureID = InitializeTexture(RESOURCE_LOCATION  "textures/work_d.jpg", 3, false, true, false);
			m_PointLightIconID = InitializeTexture(RESOURCE_LOCATION  "textures/icons/point-light-icon-256.png", 4, false, true, false);
			m_DirectionalLightIconID = InitializeTexture(RESOURCE_LOCATION  "textures/icons/directional-light-icon-256.png", 4, false, true, false);

			// Shadow map texture
			{
				// TODO: Add option to initialize empty texture using public virtual

				glGenFramebuffers(1, &m_ShadowMapFBO);
				glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowMapFBO);

				glGenTextures(1, &m_ShadowMapTexture.id);
				glBindTexture(GL_TEXTURE_2D, m_ShadowMapTexture.id);
				glTexImage2D(GL_TEXTURE_2D, 0, m_ShadowMapTexture.internalFormat, m_ShadowMapSize, m_ShadowMapSize, 0, m_ShadowMapTexture.format, m_ShadowMapTexture.type, NULL);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
				real borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
				glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor); // Prevents areas not covered by map to be in shadow
				glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_ShadowMapTexture.id, 0);

				// No color buffer is written to
				glDrawBuffer(GL_NONE);

				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				{
					PrintError("Shadow depth buffer is incomplete!\n");
				}

				if (m_DirectionalLight != nullptr)
				{
					m_DirectionalLight->shadowTextureID = m_ShadowMapTexture.id;
				}
			}

			Renderer::InitializeMaterials();

			// TODO: Move to Renderer::InitializeMaterials
			MaterialCreateInfo shadowMatCreateInfo = {};
			shadowMatCreateInfo.shaderName = "shadow";
			shadowMatCreateInfo.name = "Shadow";
			shadowMatCreateInfo.engineMaterial = true;
			m_ShadowMaterialID = InitializeMaterial(&shadowMatCreateInfo);
			//i32 program = m_Shaders[m_Materials[m_ShadowMaterialID].material.shaderID].program;

			MaterialCreateInfo spriteMatCreateInfo = {};
			spriteMatCreateInfo.name = "Sprite material";
			spriteMatCreateInfo.shaderName = "sprite";
			spriteMatCreateInfo.engineMaterial = true;
			m_SpriteMatID = InitializeMaterial(&spriteMatCreateInfo);

			MaterialCreateInfo postProcessMatCreateInfo = {};
			postProcessMatCreateInfo.name = "Post process material";
			postProcessMatCreateInfo.shaderName = "post_process";
			postProcessMatCreateInfo.engineMaterial = true;
			m_PostProcessMatID = InitializeMaterial(&postProcessMatCreateInfo);

			MaterialCreateInfo postFXAAMatCreateInfo = {};
			postFXAAMatCreateInfo.name = "FXAA";
			postFXAAMatCreateInfo.shaderName = "post_fxaa";
			postFXAAMatCreateInfo.engineMaterial = true;
			m_PostFXAAMatID = InitializeMaterial(&postFXAAMatCreateInfo);


			{
				const std::string gridMatName = "Grid";
				// TODO: Don't rely on material names!
				if (!GetMaterialID(gridMatName, m_GridMaterialID))
				{
					MaterialCreateInfo gridMatInfo = {};
					gridMatInfo.shaderName = "color";
					gridMatInfo.name = gridMatName;
					gridMatInfo.engineMaterial = true;
					m_GridMaterialID = InitializeMaterial(&gridMatInfo);
				}

				m_Grid = new GameObject("Grid", GameObjectType::OBJECT);
				MeshComponent* gridMesh = m_Grid->SetMeshComponent(new MeshComponent(m_GridMaterialID, m_Grid, false));
				RenderObjectCreateInfo createInfo = {};
				createInfo.bEditorObject = true;
				gridMesh->LoadPrefabShape(MeshComponent::PrefabShape::GRID, &createInfo);
				m_Grid->GetTransform()->Translate(0.0f, -0.1f, 0.0f);
				m_Grid->SetSerializable(false);
				m_Grid->SetStatic(true);
				m_Grid->SetVisibleInSceneExplorer(false);
				m_Grid->Initialize();
				m_EditorObjects.push_back(m_Grid);
			}


			{
				const std::string worldOriginMatName = "World origin";
				// TODO: Don't rely on material names!
				if (!GetMaterialID(worldOriginMatName, m_WorldAxisMaterialID))
				{
					MaterialCreateInfo worldAxisMatInfo = {};
					worldAxisMatInfo.shaderName = "color";
					worldAxisMatInfo.name = worldOriginMatName;
					worldAxisMatInfo.engineMaterial = true;
					m_WorldAxisMaterialID = InitializeMaterial(&worldAxisMatInfo);
				}

				m_WorldOrigin = new GameObject("World origin", GameObjectType::OBJECT);
				MeshComponent* orignMesh = m_WorldOrigin->SetMeshComponent(new MeshComponent(m_WorldAxisMaterialID, m_WorldOrigin, false));
				RenderObjectCreateInfo createInfo = {};
				createInfo.bEditorObject = true;
				orignMesh->LoadPrefabShape(MeshComponent::PrefabShape::WORLD_AXIS_GROUND, &createInfo);
				m_WorldOrigin->GetTransform()->Translate(0.0f, -0.09f, 0.0f);
				m_WorldOrigin->SetSerializable(false);
				m_WorldOrigin->SetStatic(true);
				m_WorldOrigin->SetVisibleInSceneExplorer(false);
				m_WorldOrigin->Initialize();
				m_EditorObjects.push_back(m_WorldOrigin);
			}


			// 2D Quad
			{
				VertexBufferData::CreateInfo quad2DVertexBufferDataCreateInfo = {};
				quad2DVertexBufferDataCreateInfo.positions_2D = {
					glm::vec2(-1.0f,  -1.0f),
					glm::vec2(-1.0f, 3.0f),
					glm::vec2(3.0f,  -1.0f)
				};

				quad2DVertexBufferDataCreateInfo.texCoords_UV = {
					glm::vec2(0.0f, 0.0f),
					glm::vec2(0.0f, 2.0f),
					glm::vec2(2.0f, 0.0f)
				};

				quad2DVertexBufferDataCreateInfo.attributes =
					(u32)VertexAttribute::POSITION_2D |
					(u32)VertexAttribute::UV;

				m_Quad2DVertexBufferData = {};
				m_Quad2DVertexBufferData.Initialize(&quad2DVertexBufferDataCreateInfo);


				GameObject* quad2DGameObject = new GameObject("Sprite Quad 2D", GameObjectType::_NONE);
				m_PersistentObjects.push_back(quad2DGameObject);
				quad2DGameObject->SetVisible(false);

				RenderObjectCreateInfo quad2DCreateInfo = {};
				quad2DCreateInfo.vertexBufferData = &m_Quad2DVertexBufferData;
				quad2DCreateInfo.materialID = m_PostProcessMatID;
				quad2DCreateInfo.bDepthWriteEnable = false;
				quad2DCreateInfo.gameObject = quad2DGameObject;
				quad2DCreateInfo.cullFace = CullFace::NONE;
				quad2DCreateInfo.visibleInSceneExplorer = false;
				quad2DCreateInfo.depthTestReadFunc = DepthTestFunc::ALWAYS;
				m_Quad2DRenderID = InitializeRenderObject(&quad2DCreateInfo);

				m_Quad2DVertexBufferData.DescribeShaderVariables(this, m_Quad2DRenderID);
			}

			// 3D Quad
			{
				VertexBufferData::CreateInfo quad3DVertexBufferDataCreateInfo = {};
				quad3DVertexBufferDataCreateInfo.positions_3D = {
					glm::vec3(-1.0f, -1.0f, 0.0f),
					glm::vec3(-1.0f, 1.0f, 0.0f),
					glm::vec3(1.0f, -1.0f, 0.0f),

					glm::vec3(1.0f, -1.0f, 0.0f),
					glm::vec3(-1.0f, 1.0f, 0.0f),
					glm::vec3(1.0f, 1.0f, 0.0f),
				};

				quad3DVertexBufferDataCreateInfo.texCoords_UV = {
					glm::vec2(0.0f, 0.0f),
					glm::vec2(0.0f, 1.0f),
					glm::vec2(1.0f, 0.0f),

					glm::vec2(1.0f, 0.0f),
					glm::vec2(0.0f, 1.0f),
					glm::vec2(1.0f, 1.0f),
				};

				quad3DVertexBufferDataCreateInfo.attributes =
					(u32)VertexAttribute::POSITION |
					(u32)VertexAttribute::UV;

				m_Quad3DVertexBufferData = {};
				m_Quad3DVertexBufferData.Initialize(&quad3DVertexBufferDataCreateInfo);


				GameObject* quad3DGameObject = new GameObject("Sprite Quad 3D", GameObjectType::_NONE);
				m_PersistentObjects.push_back(quad3DGameObject);
				quad3DGameObject->SetVisible(false);

				RenderObjectCreateInfo quad3DCreateInfo = {};
				quad3DCreateInfo.vertexBufferData = &m_Quad3DVertexBufferData;
				quad3DCreateInfo.materialID = m_SpriteMatID;
				quad3DCreateInfo.bDepthWriteEnable = false;
				quad3DCreateInfo.gameObject = quad3DGameObject;
				quad3DCreateInfo.cullFace = CullFace::NONE;
				quad3DCreateInfo.visibleInSceneExplorer = false;
				quad3DCreateInfo.depthTestReadFunc = DepthTestFunc::ALWAYS;
				quad3DCreateInfo.bEditorObject = true; // TODO: Create other quad which is identical but is not an editor object for gameplay objects?
				m_Quad3DRenderID = InitializeRenderObject(&quad3DCreateInfo);

				m_Quad3DVertexBufferData.DescribeShaderVariables(this, m_Quad3DRenderID);
			}

			DrawLoadingTextureQuad();
			SwapBuffers();

			MaterialCreateInfo selectedObjectMatCreateInfo = {};
			selectedObjectMatCreateInfo.name = "Selected Object";
			selectedObjectMatCreateInfo.shaderName = "color";
			selectedObjectMatCreateInfo.engineMaterial = true;
			selectedObjectMatCreateInfo.colorMultiplier = VEC4_ONE;
			m_SelectedObjectMatID = InitializeMaterial(&selectedObjectMatCreateInfo);

			if (!m_BRDFTexture)
			{
				i32 brdfSize = 512;
				i32 internalFormat = GL_RG16F;
				GLenum format = GL_RG;
				GLenum type = GL_FLOAT;

				m_BRDFTexture = new GLTexture("BRDF",
											  brdfSize,
											  brdfSize,
											  2,
											  internalFormat,
											  format,
											  type);
				if (m_BRDFTexture->CreateEmpty())
				{
					m_LoadedTextures.push_back(m_BRDFTexture);
					GenerateBRDFLUT(m_BRDFTexture->handle, brdfSize);
				}
				else
				{
					PrintError("Failed to generate BRDF texture\n");
				}
			}

			// G-buffer objects
			glGenFramebuffers(1, &m_gBufferHandle);
			glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferHandle);

			const glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();

			GenerateFrameBufferTexture(&m_gBuffer_PositionMetallicHandle.id,
									   0,
									   m_gBuffer_PositionMetallicHandle.internalFormat,
									   m_gBuffer_PositionMetallicHandle.format,
									   m_gBuffer_PositionMetallicHandle.type,
									   frameBufferSize);

			GenerateFrameBufferTexture(&m_gBuffer_NormalRoughnessHandle.id,
									   1,
									   m_gBuffer_NormalRoughnessHandle.internalFormat,
									   m_gBuffer_NormalRoughnessHandle.format,
									   m_gBuffer_NormalRoughnessHandle.type,
									   frameBufferSize);

			GenerateFrameBufferTexture(&m_gBuffer_AlbedoAOHandle.id,
									   2,
									   m_gBuffer_AlbedoAOHandle.internalFormat,
									   m_gBuffer_AlbedoAOHandle.format,
									   m_gBuffer_AlbedoAOHandle.type,
									   frameBufferSize);

			// Create and attach depth buffer
			glGenRenderbuffers(1, &m_gBufferDepthHandle);
			glBindRenderbuffer(GL_RENDERBUFFER, m_gBufferDepthHandle);
			glRenderbufferStorage(GL_RENDERBUFFER, m_OffscreenDepthBufferInternalFormat, frameBufferSize.x, frameBufferSize.y);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_gBufferDepthHandle);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			{
				PrintError("Framebuffer not complete!\n");
			}
		}

		void GLRenderer::PostInitialize()
		{
			GenerateGBuffer();

			LoadFonts(false);

			GLFWWindowWrapper* castedWindow = dynamic_cast<GLFWWindowWrapper*>(g_Window);
			if (castedWindow == nullptr)
			{
				PrintError("GLRenderer::PostInitialize expects g_Window to be of type GLFWWindowWrapper!\n");
				return;
			}

			const char* glsl_version = "#version 130";
			ImGui_ImplGlfw_InitForOpenGL(castedWindow->GetWindow(), false);
			ImGui_ImplOpenGL3_Init(glsl_version);

			m_PhysicsDebugDrawer = new GLPhysicsDebugDraw();
			m_PhysicsDebugDrawer->Initialize();

			for (GameObject* editorObject : m_EditorObjects)
			{
				editorObject->PostInitialize();
			}

			if (m_DirectionalLight != nullptr)
			{
				m_DirectionalLight->shadowTextureID = m_ShadowMapTexture.id;
			}
		}

		void GLRenderer::Destroy()
		{
			Renderer::Destroy();

			g_InputManager->UnbindKeyEventCallback(&m_KeyEventCallback);
			g_InputManager->UnbindActionCallback(&m_ActionCallback);

			glDeleteVertexArrays(1, &m_TextQuadSS_VAO);
			glDeleteBuffers(1, &m_TextQuadSS_VBO);

			glDeleteVertexArrays(1, &m_TextQuadWS_VAO);
			glDeleteBuffers(1, &m_TextQuadWS_VBO);

			glDeleteBuffers(1, &m_CaptureRBO);
			glDeleteBuffers(1, &m_CaptureFBO);

			glDeleteFramebuffers(1, &m_Offscreen0FBO);
			glDeleteRenderbuffers(1, &m_Offscreen0RBO);

			glDeleteFramebuffers(1, &m_Offscreen1FBO);
			glDeleteRenderbuffers(1, &m_Offscreen1RBO);

			glDeleteFramebuffers(1, &m_ShadowMapFBO);

			delete screenshotAsyncTextureSave;

			for (GameObject* editorObject : m_EditorObjects)
			{
				editorObject->Destroy();
				delete editorObject;
			}
			m_EditorObjects.clear();

			for (BitmapFont* font : m_FontsSS)
			{
				delete font;
			}
			m_FontsSS.clear();

			for (BitmapFont* font : m_FontsWS)
			{
				delete font;
			}
			m_FontsWS.clear();

			for (GLTexture* texture : m_LoadedTextures)
			{
				if (texture)
				{
					texture->Destroy();
					delete texture;
				}
			}
			m_LoadedTextures.clear();

			ImGui_ImplOpenGL3_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();

			if (m_1x1_NDC_QuadVertexBufferData.vertexData)
			{
				m_1x1_NDC_QuadVertexBufferData.Destroy();
			}

			if (m_1x1_NDC_Quad)
			{
				DestroyRenderObject(m_1x1_NDC_Quad->renderID);
				m_1x1_NDC_Quad = nullptr;
			}

			for (GameObject* obj : m_PersistentObjects)
			{
				if (obj->GetRenderID() != InvalidRenderID)
				{
					DestroyRenderObject(obj->GetRenderID());
				}
				delete obj;
			}
			m_PersistentObjects.clear();

			for (GameObject* editorObject : m_EditorObjects)
			{
				if (editorObject->GetRenderID() != InvalidRenderID)
				{
					DestroyRenderObject(editorObject->GetRenderID());
				}
				delete editorObject;
			}

			DestroyRenderObject(m_Quad3DRenderID);
			DestroyRenderObject(m_Quad2DRenderID);

			DestroyRenderObject(m_GBufferQuadRenderID);

			u32 activeRenderObjectCount = GetActiveRenderObjectCount();
			if (activeRenderObjectCount > 0)
			{
				PrintError("=====================================================\n");
				PrintError("%i render objects were not destroyed before GL render:\n", activeRenderObjectCount);

				for (GLRenderObject* renderObject : m_RenderObjects)
				{
					if (renderObject)
					{
						PrintError("render object with material name: %s\n", renderObject->materialName.c_str());
						DestroyRenderObject(renderObject->renderID);
					}
				}
				PrintError("=====================================================\n");
			}
			m_RenderObjects.clear();

			UnloadShaders();
			ClearMaterials(true);

			m_SkyBoxMesh = nullptr;

			if (m_PhysicsDebugDrawer)
			{
				m_PhysicsDebugDrawer->Destroy();
				delete m_PhysicsDebugDrawer;
			}

			m_gBufferQuadVertexBufferData.Destroy();
			m_Quad2DVertexBufferData.Destroy();
			m_Quad3DVertexBufferData.Destroy();

			glfwTerminate();
		}

		MaterialID GLRenderer::InitializeMaterial(const MaterialCreateInfo* createInfo, MaterialID matToReplace /* = InvalidMaterialID */)
		{
			MaterialID matID;
			if (matToReplace != InvalidMaterialID)
			{
				matID = matToReplace;
			}
			else
			{
				matID = GetNextAvailableMaterialID();
				m_Materials.insert(std::pair<MaterialID, GLMaterial>(matID, {}));
			}
			GLMaterial& mat = m_Materials.at(matID);
			mat.material = {};
			mat.material.name = createInfo->name;

			if (mat.material.name.empty())
			{
				PrintWarn("Material doesn't have a name!\n");
			}

			if (!GetShaderID(createInfo->shaderName, mat.material.shaderID))
			{
				if (createInfo->shaderName.empty())
				{
					PrintError("MaterialCreateInfo::shaderName must be filled in!\n");
				}
				else
				{
					PrintError("Material's shader name couldn't be found! shader name: %s\n", createInfo->shaderName.c_str());
				}
			}

			GLShader& shader = m_Shaders[mat.material.shaderID];

			glUseProgram(shader.program);


			// TODO: Is this really needed? (do things dynamically instead?)
			UniformInfo uniformInfo[] = {
				{ U_MODEL,							"model", 						&mat.uniformIDs.model },
				{ U_MODEL_INV_TRANSPOSE, 			"modelInvTranspose", 			&mat.uniformIDs.modelInvTranspose },
				{ U_COLOR_MULTIPLIER, 				"colorMultiplier", 				&mat.uniformIDs.colorMultiplier },
				{ U_LIGHT_VIEW_PROJ, 				"lightViewProj",				&mat.uniformIDs.lightViewProjection },
				{ U_EXPOSURE,						"exposure",						&mat.uniformIDs.exposure },
				{ U_VIEW, 							"view", 						&mat.uniformIDs.view },
				{ U_VIEW_PROJECTION, 				"viewProjection", 				&mat.uniformIDs.viewProjection },
				{ U_PROJECTION, 					"projection", 					&mat.uniformIDs.projection },
				{ U_CAM_POS, 						"camPos", 						&mat.uniformIDs.camPos },
				{ U_NORMAL_SAMPLER, 				"enableNormalSampler", 			&mat.uniformIDs.enableNormalTexture },
				{ U_CUBEMAP_SAMPLER, 				"enableCubemapSampler", 		&mat.uniformIDs.enableCubemapTexture },
				{ U_ALBEDO_SAMPLER,	 				"enableAlbedoSampler", 			&mat.uniformIDs.enableAlbedoSampler },
				{ U_CONST_ALBEDO, 					"constAlbedo", 					&mat.uniformIDs.constAlbedo },
				{ U_METALLIC_SAMPLER,		 		"enableMetallicSampler", 		&mat.uniformIDs.enableMetallicSampler },
				{ U_CONST_METALLIC, 				"constMetallic", 				&mat.uniformIDs.constMetallic },
				{ U_ROUGHNESS_SAMPLER,	 			"enableRoughnessSampler", 		&mat.uniformIDs.enableRoughnessSampler },
				{ U_CONST_ROUGHNESS, 				"constRoughness", 				&mat.uniformIDs.constRoughness },
				{ U_AO_SAMPLER,						"enableAOSampler",				&mat.uniformIDs.enableAOSampler },
				{ U_CONST_AO,						"constAO",						&mat.uniformIDs.constAO },
				{ U_HDR_EQUIRECTANGULAR_SAMPLER,	"hdrEquirectangularSampler",	&mat.uniformIDs.hdrEquirectangularSampler },
				{ U_IRRADIANCE_SAMPLER,				"enableIrradianceSampler",		&mat.uniformIDs.enableIrradianceSampler },
				{ U_TRANSFORM_MAT,					"transformMat",					&mat.uniformIDs.transformMat },
				{ U_TEX_SIZE,						"texSize",						&mat.uniformIDs.texSize },
				{ U_TIME,							"time",							&mat.uniformIDs.time },
			};

			for (const UniformInfo& uniform : uniformInfo)
			{
				// TODO: CLEANUP: Get rid of HasUniform in place of -1 check! :O
				//if (shader.shader.dynamicBufferUniforms.HasUniform(uniform.uniform) ||
				//	shader.shader.constantBufferUniforms.HasUniform(uniform.uniform))
				//{
					*uniform.id = glGetUniformLocation(shader.program, uniform.name);
					//if (*uniform.id == -1)
					//{
					//		PrintWarn("uniform %s was not found for material %s (shader: %s)\n",
					//				  uniform.name, createInfo->name.c_str(), createInfo->shaderName.c_str());
					//}
				//}
			}

			if (shader.shader.bNeedShadowMap)
			{
				mat.uniformIDs.castShadows = glGetUniformLocation(shader.program, "castShadows");
				mat.uniformIDs.shadowDarkness = glGetUniformLocation(shader.program, "shadowDarkness");
			}

			mat.material.normalTexturePath = createInfo->normalTexturePath;
			mat.material.generateNormalSampler = createInfo->generateNormalSampler;
			mat.material.enableNormalSampler = createInfo->enableNormalSampler;

			mat.material.frameBuffers = createInfo->frameBuffers;

			mat.material.enableCubemapTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;

			mat.material.enableCubemapSampler = createInfo->enableCubemapSampler;
			mat.material.generateCubemapSampler = createInfo->generateCubemapSampler || createInfo->generateHDRCubemapSampler;
			mat.material.cubemapSamplerSize = createInfo->generatedCubemapSize;
			mat.material.cubeMapFilePaths = createInfo->cubeMapFilePaths;

			assert(mat.material.cubemapSamplerSize.x <= MAX_TEXTURE_DIM);
			assert(mat.material.cubemapSamplerSize.y <= MAX_TEXTURE_DIM);

			mat.material.constAlbedo = glm::vec4(createInfo->constAlbedo, 0);
			mat.material.generateAlbedoSampler = createInfo->generateAlbedoSampler;
			mat.material.albedoTexturePath = createInfo->albedoTexturePath;
			mat.material.enableAlbedoSampler = createInfo->enableAlbedoSampler;

			mat.material.constMetallic = createInfo->constMetallic;
			mat.material.generateMetallicSampler = createInfo->generateMetallicSampler;
			mat.material.metallicTexturePath = createInfo->metallicTexturePath;
			mat.material.enableMetallicSampler = createInfo->enableMetallicSampler;

			mat.material.constRoughness = createInfo->constRoughness;
			mat.material.generateRoughnessSampler = createInfo->generateRoughnessSampler;
			mat.material.roughnessTexturePath = createInfo->roughnessTexturePath;
			mat.material.enableRoughnessSampler = createInfo->enableRoughnessSampler;

			mat.material.constAO = createInfo->constAO;
			mat.material.generateAOSampler = createInfo->generateAOSampler;
			mat.material.aoTexturePath = createInfo->aoTexturePath;
			mat.material.enableAOSampler = createInfo->enableAOSampler;

			mat.material.enableHDREquirectangularSampler = createInfo->enableHDREquirectangularSampler;
			mat.material.generateHDREquirectangularSampler = createInfo->generateHDREquirectangularSampler;
			mat.material.hdrEquirectangularTexturePath = createInfo->hdrEquirectangularTexturePath;

			mat.material.generateHDRCubemapSampler = createInfo->generateHDRCubemapSampler;

			mat.material.enableIrradianceSampler = createInfo->enableIrradianceSampler;
			mat.material.generateIrradianceSampler = createInfo->generateIrradianceSampler;
			mat.material.irradianceSamplerSize = createInfo->generatedIrradianceCubemapSize;

			mat.material.environmentMapPath = createInfo->environmentMapPath;

			mat.material.generateReflectionProbeMaps = createInfo->generateReflectionProbeMaps;

			mat.material.colorMultiplier = createInfo->colorMultiplier;

			mat.material.engineMaterial = createInfo->engineMaterial;

			mat.material.textureScale = createInfo->textureScale;

			if (shader.shader.bNeedIrradianceSampler)
			{
				if (createInfo->irradianceSamplerMatID == InvalidID)
				{
					mat.irradianceSamplerID = InvalidID;
				}
				else
				{
					mat.irradianceSamplerID = m_Materials[createInfo->irradianceSamplerMatID].irradianceSamplerID;
				}
			}

			if (shader.shader.bNeedBRDFLUT)
			{
				if (!m_BRDFTexture)
				{
					Print("BRDF LUT has not been generated before material which uses it!\n");
				}
				mat.brdfLUTSamplerID = m_BRDFTexture->handle;
			}

			if (shader.shader.bNeedPrefilteredMap)
			{
				if (createInfo->prefilterMapSamplerMatID == InvalidID)
				{
					mat.prefilteredMapSamplerID = InvalidID;
				}
				else
				{
					mat.prefilteredMapSamplerID = m_Materials[createInfo->prefilterMapSamplerMatID].prefilteredMapSamplerID;
				}
			}

			mat.material.enablePrefilteredMap = createInfo->enablePrefilteredMap;
			mat.material.generatePrefilteredMap = createInfo->generatePrefilteredMap;
			mat.material.prefilteredMapSize = createInfo->generatedPrefilteredCubemapSize;

			mat.material.enableBRDFLUT = createInfo->enableBRDFLUT;

			struct SamplerCreateInfo
			{
				bool needed;
				bool create;
				u32* id;
				std::string filepath;
				std::string textureName;
				i32 channelCount;
				bool flipVertically;
				bool generateMipMaps;
				bool hdr;
			};

			// Samplers that need to be loaded from file
			SamplerCreateInfo samplerCreateInfos[] =
			{
				{ shader.shader.bNeedAlbedoSampler, mat.material.generateAlbedoSampler, &mat.albedoSamplerID,
				createInfo->albedoTexturePath, "albedoSampler", 3, false, true, false },
				{ shader.shader.bNeedMetallicSampler, mat.material.generateMetallicSampler, &mat.metallicSamplerID,
				createInfo->metallicTexturePath, "metallicSampler", 3, false, true, false },
				{ shader.shader.bNeedRoughnessSampler, mat.material.generateRoughnessSampler, &mat.roughnessSamplerID,
				createInfo->roughnessTexturePath, "roughnessSampler", 3, false, true, false },
				{ shader.shader.bNeedAOSampler, mat.material.generateAOSampler, &mat.aoSamplerID,
				createInfo->aoTexturePath, "aoSampler", 3, false, true, false },
				{ shader.shader.bNeedNormalSampler, mat.material.generateNormalSampler, &mat.normalSamplerID,
				createInfo->normalTexturePath, "normalSampler", 3, false, true, false },
				{ shader.shader.bNeedHDREquirectangularSampler, mat.material.generateHDREquirectangularSampler, &mat.hdrTextureID,
				createInfo->hdrEquirectangularTexturePath, "hdrEquirectangularSampler", 4, true, false, true },
			};

			i32 binding = 0;
			for (SamplerCreateInfo& samplerCreateInfo : samplerCreateInfos)
			{
				if (samplerCreateInfo.needed)
				{
					if (samplerCreateInfo.create)
					{
						if (samplerCreateInfo.filepath.empty())
						{
							PrintError("Empty file path specified in SamplerCreateInfo for texture %s in material %s\n",
								samplerCreateInfo.textureName.c_str(), mat.material.name.c_str());
						}
						else
						{
							GLTexture* loadedTexture = nullptr;
							if (GetLoadedTexture(samplerCreateInfo.filepath, &loadedTexture))
							{
								// TODO: not just this
								*samplerCreateInfo.id = loadedTexture->handle;
							}
							else
							{
								std::string fileNameClean = samplerCreateInfo.filepath;
								StripLeadingDirectories(fileNameClean);
								std::string textureProfileBlockName = "load texture " + fileNameClean;
								PROFILE_BEGIN(textureProfileBlockName);

								GLTexture* newTexture = new GLTexture(samplerCreateInfo.filepath,
																	  samplerCreateInfo.channelCount,
																	  samplerCreateInfo.flipVertically,
																	  samplerCreateInfo.generateMipMaps,
																	  samplerCreateInfo.hdr);

								newTexture->LoadFromFile();

								PROFILE_END(textureProfileBlockName);
								Profiler::PrintBlockDuration(textureProfileBlockName);

								if (newTexture->bLoaded)
								{
									*samplerCreateInfo.id = newTexture->handle;
									m_LoadedTextures.push_back(newTexture);
								}
							}

							i32 uniformLocation = glGetUniformLocation(shader.program, samplerCreateInfo.textureName.c_str());
							if (uniformLocation == -1)
							{
								PrintWarn("texture uniform %s was not found in material %s (shader: %s)\n",
										  samplerCreateInfo.textureName.c_str(), mat.material.name.c_str(), shader.shader.name.c_str());
							}
							else
							{
								glUniform1i(uniformLocation, binding);
							}
						}
					}
					// Always increment the binding, even when not binding anything
					++binding;
				}
			}

			for (auto& frameBufferPair : createInfo->frameBuffers)
			{
				const char* frameBufferName = frameBufferPair.first.c_str();
				i32 positionLocation = glGetUniformLocation(shader.program, frameBufferName);
				if (positionLocation == -1)
				{
					PrintWarn("%s was not found in material %s, (shader %s)\n",
							  frameBufferPair.first.c_str(), mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(positionLocation, binding);
				}
				++binding;
			}


			if (createInfo->generateCubemapSampler)
			{
				GLCubemapCreateInfo cubemapCreateInfo = {};
				cubemapCreateInfo.program = shader.program;
				cubemapCreateInfo.textureID = &mat.cubemapSamplerID;
				cubemapCreateInfo.HDR = false;
				cubemapCreateInfo.generateMipmaps = false;
				cubemapCreateInfo.enableTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;
				cubemapCreateInfo.filePaths = mat.material.cubeMapFilePaths;

				if (createInfo->cubeMapFilePaths[0].empty())
				{
					cubemapCreateInfo.textureSize = createInfo->generatedCubemapSize;
					GenerateGLCubemap(cubemapCreateInfo);
				}
				else
				{
					// Load from file
					GenerateGLCubemap(cubemapCreateInfo);

					i32 uniformLocation = glGetUniformLocation(shader.program, "cubemapSampler");
					if (uniformLocation == -1)
					{
						PrintWarn("uniform cubemapSampler was not found in material %s (shader %s)\n",
								  mat.material.name.c_str(), shader.shader.name.c_str());
					}
					else
					{
						glUniform1i(uniformLocation, binding);
					}
					++binding;
				}
			}

			if (mat.material.generateReflectionProbeMaps)
			{
				mat.cubemapSamplerGBuffersIDs = {
					{ 0, "positionMetallicFrameBufferSampler", GL_RGBA16F, GL_RGBA },
					{ 0, "normalRoughnessFrameBufferSampler", GL_RGBA16F, GL_RGBA },
					{ 0, "albedoAOFrameBufferSampler", GL_RGBA, GL_RGBA },
				};

				GLCubemapCreateInfo cubemapCreateInfo = {};
				cubemapCreateInfo.program = shader.program;
				cubemapCreateInfo.textureID = &mat.cubemapSamplerID;
				cubemapCreateInfo.textureGBufferIDs = &mat.cubemapSamplerGBuffersIDs;
				cubemapCreateInfo.depthTextureID = &mat.cubemapDepthSamplerID;
				cubemapCreateInfo.HDR = true;
				cubemapCreateInfo.enableTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;
				cubemapCreateInfo.generateMipmaps = false;
				cubemapCreateInfo.textureSize = createInfo->generatedCubemapSize;
				cubemapCreateInfo.generateDepthBuffers = createInfo->generateCubemapDepthBuffers;

				GenerateGLCubemap(cubemapCreateInfo);
			}
			else if (createInfo->generateHDRCubemapSampler)
			{
				GLCubemapCreateInfo cubemapCreateInfo = {};
				cubemapCreateInfo.program = shader.program;
				cubemapCreateInfo.textureID = &mat.cubemapSamplerID;
				cubemapCreateInfo.textureGBufferIDs = &mat.cubemapSamplerGBuffersIDs;
				cubemapCreateInfo.depthTextureID = &mat.cubemapDepthSamplerID;
				cubemapCreateInfo.HDR = true;
				cubemapCreateInfo.enableTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;
				cubemapCreateInfo.generateMipmaps = false;
				cubemapCreateInfo.textureSize = createInfo->generatedCubemapSize;
				cubemapCreateInfo.generateDepthBuffers = createInfo->generateCubemapDepthBuffers;

				GenerateGLCubemap(cubemapCreateInfo);
			}

			if (shader.shader.bNeedCubemapSampler)
			{
				// TODO: Save location for binding later?
				const char* uniformName = "cubemapSampler";
				i32 uniformLocation = glGetUniformLocation(shader.program, uniformName);
				if (uniformLocation == -1)
				{
					PrintWarn("uniform %s was not found in material %s (shader %s)\n",
							  uniformName, mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(uniformLocation, binding);
				}
				++binding;
			}

			if (shader.shader.bNeedBRDFLUT)
			{
				const char* uniformName = "brdfLUT";
				i32 uniformLocation = glGetUniformLocation(shader.program, uniformName);
				if (uniformLocation == -1)
				{
					PrintWarn("uniform %s was not found in material %s (shader %s)\n",
							  uniformName, mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(uniformLocation, binding);
				}
				++binding;
			}

			if (shader.shader.bNeedShadowMap)
			{
				const char* uniformName = "shadowMap";
				i32 uniformLocation = glGetUniformLocation(shader.program, uniformName);
				if (uniformLocation == -1)
				{
					PrintWarn("uniform %s was not found in material %s (shader %s)\n",
						uniformName, mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(uniformLocation, binding);
				}
				++binding;
			}

			if (mat.material.generateIrradianceSampler)
			{
				GLCubemapCreateInfo cubemapCreateInfo = {};
				cubemapCreateInfo.program = shader.program;
				cubemapCreateInfo.textureID = &mat.irradianceSamplerID;
				cubemapCreateInfo.HDR = true;
				cubemapCreateInfo.enableTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;
				cubemapCreateInfo.generateMipmaps = false;
				cubemapCreateInfo.textureSize = createInfo->generatedIrradianceCubemapSize;

				GenerateGLCubemap(cubemapCreateInfo);
			}

			if (shader.shader.bNeedIrradianceSampler)
			{
				const char* uniformName = "irradianceSampler";
				i32 uniformLocation = glGetUniformLocation(shader.program, uniformName);
				if (uniformLocation == -1)
				{
					PrintWarn("uniform %s was not found in material %s (shader %s)\n",
							  uniformName, mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(uniformLocation, binding);
				}
				++binding;
			}

			if (mat.material.generatePrefilteredMap)
			{
				GLCubemapCreateInfo cubemapCreateInfo = {};
				cubemapCreateInfo.program = shader.program;
				cubemapCreateInfo.textureID = &mat.prefilteredMapSamplerID;
				cubemapCreateInfo.HDR = true;
				cubemapCreateInfo.enableTrilinearFiltering = createInfo->enableCubemapTrilinearFiltering;
				cubemapCreateInfo.generateMipmaps = true;
				cubemapCreateInfo.textureSize = createInfo->generatedPrefilteredCubemapSize;

				GenerateGLCubemap(cubemapCreateInfo);
			}

			if (shader.shader.bNeedPrefilteredMap)
			{
				const char* uniformName = "prefilterMap";
				i32 uniformLocation = glGetUniformLocation(shader.program, uniformName);
				if (uniformLocation == -1)
				{
					PrintWarn("uniform %s was not found in material %s (shader %s)\n",
							  uniformName, mat.material.name.c_str(), shader.shader.name.c_str());
				}
				else
				{
					glUniform1i(uniformLocation, binding);
				}
				++binding;
			}

			return matID;
		}

		TextureID GLRenderer::InitializeTexture(const std::string& relativeFilePath, i32 channelCount, bool bFlipVertically, bool bGenerateMipMaps, bool bHDR)
		{
			GLTexture* newTexture = new GLTexture(relativeFilePath, channelCount, bFlipVertically, bGenerateMipMaps, bHDR);
			if (newTexture->LoadFromFile())
			{
				m_LoadedTextures.push_back(newTexture);
			}

			TextureID id = m_LoadedTextures.size() - 1;
			return id;
		}

		u32 GLRenderer::InitializeRenderObject(const RenderObjectCreateInfo* createInfo)
		{
			const RenderID renderID = GetNextAvailableRenderID();

			assert(createInfo->materialID != InvalidMaterialID);

			m_bRebatchRenderObjects = true;

			GLRenderObject* renderObject = new GLRenderObject();
			renderObject->renderID = renderID;
			InsertNewRenderObject(renderObject);

			renderObject->materialID = createInfo->materialID;
			renderObject->vertexBufferData = createInfo->vertexBufferData;
			renderObject->indices = createInfo->indices;
			renderObject->gameObject = createInfo->gameObject;
			renderObject->cullFace = CullFaceToGLCullFace(createInfo->cullFace);
			renderObject->depthTestReadFunc = DepthTestFuncToGlenum(createInfo->depthTestReadFunc);
			renderObject->bDepthWriteEnable = BoolToGLBoolean(createInfo->bDepthWriteEnable);
			renderObject->bEditorObject = createInfo->bEditorObject;

			if (renderObject->materialID == InvalidMaterialID)
			{
				PrintError("Render object's materialID has not been set in its createInfo!\n");

				// TODO: Use INVALID material here (Bright pink)
				// Hopefully the first material works out okay! Should be better than crashing
				renderObject->materialID = 0;
				renderObject->materialName = m_Materials[renderObject->materialID].material.name;
			}
			else
			{
				renderObject->materialName = m_Materials[renderObject->materialID].material.name;

				if (renderObject->materialName.empty())
				{
					PrintWarn("Render object created with empty material name!\n");
				}
			}

			if (renderObject->gameObject->GetName().empty())
			{
				PrintWarn("Render object created with empty name!\n");
			}

			if (m_Materials.empty())
			{
				PrintError("Render object is being created before any materials have been created!\n");
			}

			if (m_Materials.find(renderObject->materialID) == m_Materials.end())
			{
				PrintError("Uninitialized material with MaterialID %i\n", renderObject->materialID);
				return renderID;
			}

			GLMaterial& material = m_Materials[renderObject->materialID];
			GLShader& shader = m_Shaders[material.material.shaderID];

			glUseProgram(shader.program);

			if (createInfo->vertexBufferData)
			{
				glGenVertexArrays(1, &renderObject->VAO);
				glBindVertexArray(renderObject->VAO);

				glGenBuffers(1, &renderObject->VBO);
				glBindBuffer(GL_ARRAY_BUFFER, renderObject->VBO);
				glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)createInfo->vertexBufferData->VertexBufferSize, createInfo->vertexBufferData->vertexData, GL_STATIC_DRAW);
			}

			if (createInfo->indices != nullptr &&
				!createInfo->indices->empty())
			{
				renderObject->bIndexed = true;

				glGenBuffers(1, &renderObject->IBO);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderObject->IBO);
				GLsizeiptr count = (GLsizeiptr)(sizeof(u32) * createInfo->indices->size());
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, count, createInfo->indices->data(), GL_STATIC_DRAW);
			}

			glBindVertexArray(0);
			glUseProgram(0);

			m_bRebatchRenderObjects = true;

			return renderID;
		}

		void GLRenderer::GenerateReflectionProbeMaps(RenderID cubemapRenderID, MaterialID materialID)
		{
			// NOTE: glFlush calls help RenderDoc replay frames without crashing

			CaptureSceneToCubemap(cubemapRenderID);
			//glFlush();

			GenerateIrradianceSamplerFromCubemap(materialID);
			//glFlush();

			GeneratePrefilteredMapFromCubemap(materialID);
			//glFlush();
		}

		void GLRenderer::GenerateIrradianceSamplerMaps(MaterialID materialID)
		{
			// NOTE: glFlush calls help RenderDoc replay frames without crashing

			const GLMaterial* material = &m_Materials[materialID];

			GenerateCubemapFromHDREquirectangular(materialID, material->material.environmentMapPath);
			//glFlush();

			GenerateIrradianceSamplerFromCubemap(materialID);
			//glFlush();

			GeneratePrefilteredMapFromCubemap(materialID);
			//glFlush();
		}

		void GLRenderer::PostInitializeRenderObject(RenderID renderID)
		{
			const GLRenderObject* renderObject = GetRenderObject(renderID);
			const MaterialID materialID = renderObject->materialID;
			const GLMaterial* material = &m_Materials[materialID];

			if (material->material.generateReflectionProbeMaps)
			{
				BatchRenderObjects();

				GenerateReflectionProbeMaps(renderID, materialID);

				// Display captured cubemap as skybox
				//m_Materials[m_RenderObjects[cubemapID]->materialID].cubemapSamplerID =
				//	m_Materials[m_RenderObjects[renderID]->materialID].cubemapSamplerID;
			}
			else if (material->material.generateIrradianceSampler)
			{
				GenerateIrradianceSamplerMaps(materialID);
			}
		}

		void GLRenderer::ClearMaterials(bool bDestroyEngineMats /* = false */)
		{
			auto iter = m_Materials.begin();
			while (iter != m_Materials.end())
			{
				if (bDestroyEngineMats || iter->second.material.engineMaterial == false)
				{
					iter = m_Materials.erase(iter);
				}
				else
				{
					++iter;
				}
			}
		}

		void GLRenderer::GenerateCubemapFromHDREquirectangular(MaterialID cubemapMaterialID,
			const std::string& environmentMapPath)
		{
			const std::string profileBlockName = "generating cubemap for material: " + m_Materials[cubemapMaterialID].material.name;
			{
				PROFILE_AUTO(profileBlockName.c_str());

				if (!m_SkyBoxMesh)
				{
					PrintError("Attempted to generate cubemap before skybox object was created!\n");
					return;
				}

				bool bRandomizeSkybox = !m_AvailableHDRIs.empty();

				MaterialID equirectangularToCubeMatID = InvalidMaterialID;
				// TODO: Don't rely on material names!
				if (!GetMaterialID("Equirectangular to Cube", equirectangularToCubeMatID) || bRandomizeSkybox)
				{
					std::string equirectangularProfileBlockName = "generating equirectangular mat";
					PROFILE_BEGIN(equirectangularProfileBlockName);
					MaterialCreateInfo equirectangularToCubeMatCreateInfo = {};
					equirectangularToCubeMatCreateInfo.name = "Equirectangular to Cube";
					equirectangularToCubeMatCreateInfo.shaderName = "equirectangular_to_cube";
					equirectangularToCubeMatCreateInfo.enableHDREquirectangularSampler = true;
					equirectangularToCubeMatCreateInfo.generateHDREquirectangularSampler = true;
					equirectangularToCubeMatCreateInfo.engineMaterial = true;
					// TODO: Make cyclable at runtime
					equirectangularToCubeMatCreateInfo.hdrEquirectangularTexturePath = environmentMapPath;

					if (bRandomizeSkybox && !m_AvailableHDRIs.empty())
					{
						equirectangularToCubeMatCreateInfo.hdrEquirectangularTexturePath = PickRandomSkyboxTexture();
					}

					equirectangularToCubeMatID = InitializeMaterial(&equirectangularToCubeMatCreateInfo, equirectangularToCubeMatID);
					PROFILE_END(equirectangularProfileBlockName);
					Profiler::PrintBlockDuration(equirectangularProfileBlockName);
				}

				GLMaterial& equirectangularToCubemapMaterial = m_Materials[equirectangularToCubeMatID];
				GLShader& equirectangularToCubemapShader = m_Shaders[equirectangularToCubemapMaterial.material.shaderID];

				// TODO: Handle no skybox being set gracefully
				GLRenderObject* skyboxRenderObject = GetRenderObject(m_SkyBoxMesh->GetRenderID());
				GLMaterial& skyboxGLMaterial = m_Materials[skyboxRenderObject->materialID];

				glUseProgram(equirectangularToCubemapShader.program);

				// TODO: Store what location this texture is at (might not be 0)
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, m_Materials[equirectangularToCubeMatID].hdrTextureID);

				// Update object's uniforms under this shader's program
				glUniformMatrix4fv(equirectangularToCubemapMaterial.uniformIDs.model, 1, GL_FALSE,
					&m_SkyBoxMesh->GetTransform()->GetWorldTransform()[0][0]);

				glUniformMatrix4fv(equirectangularToCubemapMaterial.uniformIDs.projection, 1, GL_FALSE,
					&m_CaptureProjection[0][0]);

				glm::vec2 cubemapSize = skyboxGLMaterial.material.cubemapSamplerSize;

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				glViewport(0, 0, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				glBindVertexArray(skyboxRenderObject->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, skyboxRenderObject->VBO);

				if (skyboxRenderObject->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(skyboxRenderObject->cullFace);
				}

				glDepthFunc(skyboxRenderObject->depthTestReadFunc);

				for (u32 i = 0; i < 6; ++i)
				{
					glUniformMatrix4fv(equirectangularToCubemapMaterial.uniformIDs.view, 1, GL_FALSE,
						&m_CaptureViews[i][0][0]);

					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_Materials[cubemapMaterialID].cubemapSamplerID, 0);

					glDepthMask(GL_TRUE);

					glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

					glDepthMask(skyboxRenderObject->bDepthWriteEnable);

					glDrawArrays(skyboxRenderObject->topology, 0,
						(GLsizei)skyboxRenderObject->vertexBufferData->VertexCount);
				}

				// Generate mip maps for generated cubemap
				glBindTexture(GL_TEXTURE_CUBE_MAP, m_Materials[cubemapMaterialID].cubemapSamplerID);
				glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
			}
			Profiler::PrintBlockDuration(profileBlockName);
		}

		void GLRenderer::GeneratePrefilteredMapFromCubemap(MaterialID cubemapMaterialID)
		{
			const std::string profileBlockName = "generating prefiltered map for material: " + m_Materials[cubemapMaterialID].material.name;
			{
				PROFILE_AUTO(profileBlockName.c_str());

				if (!m_SkyBoxMesh)
				{
					PrintError("Attempted to generate prefiltered map before skybox object was created!\n");
					return;
				}

				MaterialID prefilterMatID = InvalidMaterialID;
				// TODO: Don't rely on material names!
				if (!GetMaterialID("Prefilter", prefilterMatID))
				{
					MaterialCreateInfo prefilterMaterialCreateInfo = {};
					prefilterMaterialCreateInfo.name = "Prefilter";
					prefilterMaterialCreateInfo.shaderName = "prefilter";
					prefilterMaterialCreateInfo.engineMaterial = true;
					prefilterMatID = InitializeMaterial(&prefilterMaterialCreateInfo);
				}

				GLMaterial& prefilterMat = m_Materials[prefilterMatID];
				GLShader& prefilterShader = m_Shaders[prefilterMat.material.shaderID];

				GLRenderObject* skybox = GetRenderObject(m_SkyBoxMesh->GetRenderID());

				glUseProgram(prefilterShader.program);

				glUniformMatrix4fv(prefilterMat.uniformIDs.model, 1, GL_FALSE,
					&m_SkyBoxMesh->GetTransform()->GetWorldTransform()[0][0]);

				glUniformMatrix4fv(prefilterMat.uniformIDs.projection, 1, GL_FALSE, &m_CaptureProjection[0][0]);

				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_CUBE_MAP, m_Materials[cubemapMaterialID].cubemapSamplerID);

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);

				glBindVertexArray(skybox->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, skybox->VBO);

				if (skybox->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(skybox->cullFace);
				}

				glDepthFunc(skybox->depthTestReadFunc);
				glDepthMask(skybox->bDepthWriteEnable);

				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				i32 roughnessUniformLocation = glGetUniformLocation(prefilterShader.program, "roughness");

				u32 maxMipLevels = 5;
				for (u32 mip = 0; mip < maxMipLevels; ++mip)
				{
					u32 mipWidth = (u32)(m_Materials[cubemapMaterialID].material.prefilteredMapSize.x * pow(0.5f, mip));
					u32 mipHeight = (u32)(m_Materials[cubemapMaterialID].material.prefilteredMapSize.y * pow(0.5f, mip));
					assert(mipWidth <= MAX_TEXTURE_DIM);
					assert(mipHeight <= MAX_TEXTURE_DIM);

					glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, mipWidth, mipHeight);

					glViewport(0, 0, mipWidth, mipHeight);

					real roughness = (real)mip / (real(maxMipLevels - 1));
					glUniform1f(roughnessUniformLocation, roughness);
					for (u32 i = 0; i < 6; ++i)
					{
						glUniformMatrix4fv(prefilterMat.uniformIDs.view, 1, GL_FALSE, &m_CaptureViews[i][0][0]);

						glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_Materials[cubemapMaterialID].prefilteredMapSamplerID, mip);

						glDrawArrays(skybox->topology, 0, (GLsizei)skybox->vertexBufferData->VertexCount);
					}
				}


				// Visualize prefiltered map as skybox:
				//m_Materials[renderObject->materialID].cubemapSamplerID = m_Materials[renderObject->materialID].prefilteredMapSamplerID;
			}
			Profiler::PrintBlockDuration(profileBlockName);
		}

		void GLRenderer::GenerateBRDFLUT(u32 brdfLUTTextureID, i32 brdfLUTSize)
		{
			const std::string profileBlockName = "generating BRDF LUT (" + std::to_string(brdfLUTSize) + "x" + std::to_string(brdfLUTSize) + ")";
			{
				PROFILE_AUTO(profileBlockName.c_str());

				if (m_1x1_NDC_Quad)
				{
					// Don't re-create material or object
					return;
				}

				MaterialCreateInfo brdfMaterialCreateInfo = {};
				brdfMaterialCreateInfo.name = "BRDF";
				brdfMaterialCreateInfo.shaderName = "brdf";
				brdfMaterialCreateInfo.engineMaterial = true;
				MaterialID brdfMatID = InitializeMaterial(&brdfMaterialCreateInfo);

				// Generate 1x1 NDC quad
				{
					VertexBufferData::CreateInfo quadVertexBufferDataCreateInfo = {};
					quadVertexBufferDataCreateInfo.positions_3D = {
						{ -1.0f,  1.0f, 0.0f },
						{ -1.0f, -1.0f, 0.0f },
						{ 1.0f,  1.0f, 0.0f },
						{ 1.0f, -1.0f, 0.0f },
					};
					quadVertexBufferDataCreateInfo.texCoords_UV = {
						{ 0.0f, 1.0f },
						{ 0.0f, 0.0f },
						{ 1.0f, 1.0f },
						{ 1.0f, 0.0f },
					};
					quadVertexBufferDataCreateInfo.attributes =
						(u32)VertexAttribute::POSITION |
						(u32)VertexAttribute::UV;

					m_1x1_NDC_QuadVertexBufferData = {};
					m_1x1_NDC_QuadVertexBufferData.Initialize(&quadVertexBufferDataCreateInfo);

					GameObject* oneByOneQuadGameObject = new GameObject("1x1 Quad", GameObjectType::_NONE);
					m_PersistentObjects.push_back(oneByOneQuadGameObject);
					// Don't render this normally, we'll draw it manually
					oneByOneQuadGameObject->SetVisible(false);

					RenderObjectCreateInfo quadCreateInfo = {};
					quadCreateInfo.materialID = brdfMatID;
					quadCreateInfo.vertexBufferData = &m_1x1_NDC_QuadVertexBufferData;
					quadCreateInfo.gameObject = oneByOneQuadGameObject;
					quadCreateInfo.depthTestReadFunc = DepthTestFunc::ALWAYS;
					quadCreateInfo.bDepthWriteEnable = false;
					quadCreateInfo.visibleInSceneExplorer = false;

					RenderID quadRenderID = InitializeRenderObject(&quadCreateInfo);
					m_1x1_NDC_Quad = GetRenderObject(quadRenderID);

					if (!m_1x1_NDC_Quad)
					{
						PrintError("Failed to create 1x1 NDC quad!\n");
					}
					else
					{
						SetTopologyMode(quadRenderID, TopologyMode::TRIANGLE_STRIP);
						m_1x1_NDC_QuadVertexBufferData.DescribeShaderVariables(this, quadRenderID);
					}
				}

				glUseProgram(m_Shaders[m_Materials[brdfMatID].material.shaderID].program);

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);

				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdfLUTTextureID, 0);

				glBindVertexArray(m_1x1_NDC_Quad->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, m_1x1_NDC_Quad->VBO);

				glViewport(0, 0, brdfLUTSize, brdfLUTSize);

				if (m_1x1_NDC_Quad->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(m_1x1_NDC_Quad->cullFace);
				}

				glDepthFunc(GL_ALWAYS);

				glDepthMask(GL_FALSE);

				// Render quad
				glDrawArrays(m_1x1_NDC_Quad->topology, 0, (GLsizei)m_1x1_NDC_Quad->vertexBufferData->VertexCount);
			}
			Profiler::PrintBlockDuration(profileBlockName);
		}

		void GLRenderer::GenerateIrradianceSamplerFromCubemap(MaterialID cubemapMaterialID)
		{
			const std::string profileBlockName = "generating irradiance sampler from cubemap";
			{
				PROFILE_AUTO(profileBlockName.c_str());

				if (!m_SkyBoxMesh)
				{
					PrintError("Attempted to generate irradiance sampler before skybox object was created!\n");
					return;
				}

				MaterialID irrandianceMatID = InvalidMaterialID;
				// TODO: Don't rely on material names!
				if (!GetMaterialID("Irradiance", irrandianceMatID))
				{
					std::string irradianceProfileBlockName = "generating irradiance mat";
					PROFILE_BEGIN(irradianceProfileBlockName);
					MaterialCreateInfo irrandianceMatCreateInfo = {};
					irrandianceMatCreateInfo.name = "Irradiance";
					irrandianceMatCreateInfo.shaderName = "irradiance";
					irrandianceMatCreateInfo.enableCubemapSampler = true;
					irrandianceMatCreateInfo.engineMaterial = true;
					irrandianceMatID = InitializeMaterial(&irrandianceMatCreateInfo);
					PROFILE_END(irradianceProfileBlockName);
					Profiler::PrintBlockDuration(irradianceProfileBlockName);
				}

				GLMaterial& irradianceMat = m_Materials[irrandianceMatID];
				GLShader& shader = m_Shaders[irradianceMat.material.shaderID];

				GLRenderObject* skybox = GetRenderObject(m_SkyBoxMesh->GetRenderID());

				glUseProgram(shader.program);

				glUniformMatrix4fv(irradianceMat.uniformIDs.model, 1, GL_FALSE,
					&m_SkyBoxMesh->GetTransform()->GetWorldTransform()[0][0]);

				glUniformMatrix4fv(irradianceMat.uniformIDs.projection, 1, GL_FALSE, &m_CaptureProjection[0][0]);

				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_CUBE_MAP, m_Materials[cubemapMaterialID].cubemapSamplerID);

				glm::vec2 cubemapSize = m_Materials[cubemapMaterialID].material.irradianceSamplerSize;

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				glViewport(0, 0, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				if (skybox->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(skybox->cullFace);
				}

				glDepthFunc(skybox->depthTestReadFunc);

				glDepthMask(skybox->bDepthWriteEnable);

				glBindVertexArray(skybox->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, skybox->VBO);

				for (u32 i = 0; i < 6; ++i)
				{
					glUniformMatrix4fv(irradianceMat.uniformIDs.view, 1, GL_FALSE, &m_CaptureViews[i][0][0]);

					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_Materials[cubemapMaterialID].irradianceSamplerID, 0);

					// Should be drawing cube here, not object (reflection probe's sphere is being drawn
					glDrawArrays(skybox->topology, 0, (GLsizei)skybox->vertexBufferData->VertexCount);
				}
			}
			Profiler::PrintBlockDuration(profileBlockName);
		}

		void GLRenderer::CaptureSceneToCubemap(RenderID cubemapRenderID)
		{
			const char* profilerBlockName = "Capture scene to cubemap";
			{
				PROFILE_AUTO(profilerBlockName);

				DrawCallInfo drawCallInfo = {};
				drawCallInfo.cubemapObjectRenderID = cubemapRenderID;
				drawCallInfo.bRenderToCubemap = true;

				// Clear cubemap faces
				GLRenderObject* cubemapRenderObject = GetRenderObject(drawCallInfo.cubemapObjectRenderID);
				GLMaterial* cubemapMaterial = &m_Materials[cubemapRenderObject->materialID];

				glm::vec2 cubemapSize = cubemapMaterial->material.cubemapSamplerSize;

				// Must be enabled to clear depth buffer
				glDepthMask(GL_TRUE);
				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				glViewport(0, 0, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				for (u32 face = 0; face < 6; ++face)
				{
					// Clear all G-Buffers
					if (!cubemapMaterial->cubemapSamplerGBuffersIDs.empty())
					{
						// Skip first buffer, it'll be cleared below
						for (u32 i = 1; i < cubemapMaterial->cubemapSamplerGBuffersIDs.size(); ++i)
						{
							glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
								GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapSamplerGBuffersIDs[i].id, 0);

							glClear(GL_COLOR_BUFFER_BIT);
						}
					}

					// Clear base cubemap framebuffer + depth buffer
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapSamplerID, 0);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapDepthSamplerID, 0);

					glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				}

				drawCallInfo.bDeferred = true;
				DrawDeferredObjects(drawCallInfo);
				drawCallInfo.bDeferred = false;
				drawCallInfo.bWriteToDepth = false;
				drawCallInfo.depthTestFunc = DepthTestFunc::ALWAYS;
				ShadeDeferredObjects(drawCallInfo);
				drawCallInfo.bWriteToDepth = true;
				drawCallInfo.depthTestFunc = DepthTestFunc::GEQUAL;
				DrawForwardObjects(drawCallInfo);
			}
			Profiler::PrintBlockDuration(profilerBlockName);
		}

		void GLRenderer::SwapBuffers()
		{
			PROFILE_AUTO("Renderer SwapBuffers");

			glfwSwapBuffers(static_cast<GLFWWindowWrapper*>(g_Window)->GetWindow());
		}

		void GLRenderer::RecaptureReflectionProbe()
		{
			const char* profilerBlockName = "Recapture reflection probe";
			PROFILE_BEGIN(profilerBlockName);
			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject &&
					m_Materials[renderObject->materialID].material.generateReflectionProbeMaps)
				{
					CaptureSceneToCubemap(renderObject->renderID);
					GenerateIrradianceSamplerFromCubemap(renderObject->materialID);
					GeneratePrefilteredMapFromCubemap(renderObject->materialID);
				}
			}
			PROFILE_END(profilerBlockName);
			Profiler::PrintBlockDuration(profilerBlockName);

			AddEditorString("Captured reflection probe");
		}

		u32 GLRenderer::GetTextureHandle(TextureID textureID) const
		{
			assert(textureID < m_LoadedTextures.size());
			return m_LoadedTextures[textureID]->handle;
		}

		void GLRenderer::RenderObjectStateChanged()
		{
			m_bRebatchRenderObjects = true;
		}

		bool GLRenderer::GetShaderID(const std::string& shaderName, ShaderID& shaderID)
		{
			// TODO: Store shaders using sorted data structure?
			for (u32 i = 0; i < m_Shaders.size(); ++i)
			{
				if (m_Shaders[i].shader.name.compare(shaderName) == 0)
				{
					shaderID = i;
					return true;
				}
			}

			return false;
		}

		bool GLRenderer::GetMaterialID(const std::string& materialName, MaterialID& materialID)
		{
			// TODO: Store materials using sorted data structure?
			for (auto& materialPair : m_Materials)
			{
				if (materialPair.second.material.name.compare(materialName) == 0)
				{
					materialID = materialPair.first;
					return true;
				}
			}

			for (auto& materialObj : BaseScene::s_ParsedMaterials)
			{
				if (materialObj.GetString("name").compare(materialName) == 0)
				{
					// Material exists in library, but hasn't been initialized yet
					MaterialCreateInfo matCreateInfo = {};
					Material::ParseJSONObject(materialObj, matCreateInfo);

					materialID = InitializeMaterial(&matCreateInfo);
					g_SceneManager->CurrentScene()->AddMaterialID(materialID);

					return true;
				}
			}

			return false;
		}

		MaterialID GLRenderer::GetMaterialID(RenderID renderID)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (renderObject != nullptr)
			{
				return renderObject->materialID;
			}
			return InvalidMaterialID;
		}

		std::vector<Pair<std::string, MaterialID>> GLRenderer::GetValidMaterialNames() const
		{
			std::vector<Pair<std::string, MaterialID>> result;

			for (auto& matPair : m_Materials)
			{
				if (!matPair.second.material.engineMaterial)
				{
					result.emplace_back(matPair.second.material.name, matPair.first);
				}
			}

			return result;
		}

		void GLRenderer::SetTopologyMode(RenderID renderID, TopologyMode topology)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (!renderObject)
			{
				PrintError("Invalid renderID passed to SetTopologyMode: %i\n", renderID);
				return;
			}

			GLenum glMode = TopologyModeToGLMode(topology);

			if (glMode == GL_INVALID_ENUM)
			{
				PrintError("Unhandled TopologyMode passed to GLRenderer::SetTopologyMode: %i\n", (i32)topology);
				renderObject->topology = GL_TRIANGLES;
			}
			else
			{
				renderObject->topology = glMode;
			}
		}

		void GLRenderer::SetClearColor(real r, real g, real b)
		{
			m_ClearColor = { r, g, b };
			glClearColor(r, g, b, 1.0f);
		}

		void GLRenderer::GenerateFrameBufferTexture(u32* handle, i32 index, GLint internalFormat, GLenum format, GLenum type, const glm::vec2i& size)
		{
			glGenTextures(1, handle);
			glBindTexture(GL_TEXTURE_2D, *handle);
			glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size.x, size.y, 0, format, type, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, *handle, 0);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		void GLRenderer::ResizeFrameBufferTexture(u32 handle, GLint internalFormat, GLenum format, GLenum type, const glm::vec2i& size)
		{
			glBindTexture(GL_TEXTURE_2D, handle);
			glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size.x, size.y, 0, format, type, NULL);
		}

		void GLRenderer::ResizeRenderBuffer(u32 handle, const glm::vec2i& size, GLenum internalFormat)
		{
			glBindRenderbuffer(GL_RENDERBUFFER, handle);
			glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, size.x, size.y);
		}

		void GLRenderer::Update()
		{
			PROFILE_AUTO("Renderer update");

			// TODO: Hook into callback
			// TODO: Generate fonts asynchronously
			// TODO: Allow fonts to be rendered using different DPI than originally created on
			// TODO: Or, specify DPI on rendered image to prevent issue when resolution is changed
			// while game isn't running
			m_MonitorResCheckTimeRemaining -= g_DeltaTime;
			if (m_MonitorResCheckTimeRemaining <= 0.0f)
			{
				m_MonitorResCheckTimeRemaining = 2.0f;
				static const char* blockName = "Renderer update > retrieve monitor info";
				{
					PROFILE_BEGIN(blockName);
					real pDPIx = g_Monitor->DPI.x;
					real pDPIy = g_Monitor->DPI.y;
					g_Window->RetrieveMonitorInfo();
					real newDPIx = g_Monitor->DPI.x;
					real newDPIy = g_Monitor->DPI.y;
					if (newDPIx != pDPIx || newDPIy != pDPIy)
					{
						LoadFonts(true);
					}
					PROFILE_END(blockName);
				}
				//Profiler::PrintBlockDuration(blockName);
			}

			if (m_EditorStrSecRemaining > 0.0f)
			{
				m_EditorStrSecRemaining -= g_DeltaTime;
				if (m_EditorStrSecRemaining <= 0.0f)
				{
					m_EditorStrSecRemaining = 0.0f;
				}
			}

			if (screenshotAsyncTextureSave != nullptr)
			{
				if (screenshotAsyncTextureSave->TickStatus())
				{
					std::string fileName = screenshotAsyncTextureSave->absoluteFilePath;
					StripLeadingDirectories(fileName);

					AddEditorString("Saved screenshot");

					if (screenshotAsyncTextureSave->bSuccess)
					{
						Print("Saved screenshot to %s (took %.2fs asynchronously)\n", fileName.c_str(), screenshotAsyncTextureSave->totalSecWaiting);
					}
					else
					{
						PrintWarn("Failed to asynchronously save screenshot to %s\n", fileName.c_str());
					}

					delete screenshotAsyncTextureSave;
					screenshotAsyncTextureSave = nullptr;
				}
			}

			// Fade grid out when far away
			{
				real maxHeightVisible = 350.0f;
				BaseCamera* camera = g_CameraManager->CurrentCamera();
				real distCamToGround = camera->GetPosition().y;
				real maxDistVisible = 300.0f;
				real distCamToOrigin = glm::distance(camera->GetPosition(), glm::vec3(0, 0, 0));

				glm::vec4 gridColorMutliplier = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f - glm::clamp(distCamToGround / maxHeightVisible, -1.0f, 1.0f));
				glm::vec4 axisColorMutliplier = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f - glm::clamp(distCamToOrigin / maxDistVisible, -1.0f, 1.0f));;
				GetMaterial(m_WorldAxisMaterialID).colorMultiplier = axisColorMutliplier;
				GetMaterial(m_GridMaterialID).colorMultiplier = gridColorMutliplier;
			}

#if 0 // Auto-rotate directional light
			m_DirectionalLight->rotation = glm::rotate(m_DirectionalLight->rotation,
				g_DeltaTime * 0.5f,
				glm::vec3(0.0f, 0.5f, 0.5f));
#endif

			m_PhysicsDebugDrawer->UpdateDebugMode();

			// TODO: Only ever use the static skybox image to avoid endlessly being out of date
			// Capture probe again using freshly rendered skybox
			if (m_FramesRendered == 1)
			{
				RecaptureReflectionProbe();
			}

			if (m_bCaptureReflectionProbes)
			{
				m_bCaptureReflectionProbes = false;
				RecaptureReflectionProbe();
			}
		}

		void GLRenderer::Draw()
		{
			DrawCallInfo drawCallInfo = {};

			UpdateAllMaterialUniforms();

			if (m_bRebatchRenderObjects)
			{
				m_bRebatchRenderObjects = false;
				BatchRenderObjects();
			}

			DrawShadowDepthMaps();

			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);

			// World-space objects
			drawCallInfo.bDeferred = true;
			DrawDeferredObjects(drawCallInfo);
			drawCallInfo.bDeferred = false;
			drawCallInfo.bWriteToDepth = false;
			drawCallInfo.depthTestFunc = DepthTestFunc::ALWAYS;
			ShadeDeferredObjects(drawCallInfo);
			drawCallInfo.bWriteToDepth = true;
			drawCallInfo.depthTestFunc = DepthTestFunc::GEQUAL;
			DrawForwardObjects(drawCallInfo);
			DrawWorldSpaceSprites();
			ApplyPostProcessing();

			if (!m_PhysicsDebuggingSettings.DisableAll)
			{
				PhysicsDebugRender();
			}

			SetFont(SID("editor-02-ws"));
			real s = g_SecElapsedSinceProgramStart * 3.5f;
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(1.0f), 1.0f), glm::vec3(2.0f, 1.5f, 0.0f), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.95f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 1) * 0.05f, 1.5f + sin(s + 0.3f * 1) * 0.05f, -0.075f * 1), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.90f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 2) * 0.07f, 1.5f + sin(s + 0.3f * 2) * 0.07f, -0.075f * 2), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.85f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 3) * 0.10f, 1.5f + sin(s + 0.3f * 3) * 0.10f, -0.075f * 3), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.80f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 4) * 0.12f, 1.5f + sin(s + 0.3f * 4) * 0.12f, -0.075f * 4), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.75f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 5) * 0.15f, 1.5f + sin(s + 0.3f * 5) * 0.15f, -0.075f * 5), QUAT_UNIT, 0.0f);
			DrawStringWS("THREE DIMENSIONAL TEXT!", glm::vec4(glm::vec3(0.70f), 1.0f), glm::vec3(2.0f + cos(s * 0.3f + 0.3f * 6) * 0.17f, 1.5f + sin(s + 0.3f * 6) * 0.17f, -0.075f * 6), QUAT_UNIT, 0.0f);

			{
				std::vector<TextVertex3D> textVerticesWS;
				UpdateTextBufferWS(textVerticesWS);

				u32 bufferByteCount = (u32)(textVerticesWS.size() * sizeof(TextVertex3D));

				glBindVertexArray(m_TextQuadWS_VAO);
				glBindBuffer(GL_ARRAY_BUFFER, m_TextQuadWS_VBO);
				glBufferData(GL_ARRAY_BUFFER, bufferByteCount, textVerticesWS.data(), GL_DYNAMIC_DRAW);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindVertexArray(0);
			}

			DrawTextWS();

			bool bUsingGameplayCam = g_CameraManager->CurrentCamera()->bIsGameplayCam;
			if (g_EngineInstance->IsRenderingEditorObjects() && !bUsingGameplayCam)
			{
				DrawDepthAwareEditorObjects(drawCallInfo);
				DrawSelectedObjectWireframe(drawCallInfo);

				glDepthMask(GL_TRUE);
				glClear(GL_DEPTH_BUFFER_BIT);

				// Depth unaware objects write to a cleared depth buffer so they
				// draw on top of previous geometry but are still eclipsed by other
				// depth unaware objects
				DrawDepthUnawareEditorObjects(drawCallInfo);
			}

			DrawScreenSpaceSprites();


			// Screen-space objects
			SetFont(SID("editor-02"));
			static const glm::vec4 color(0.95f);
			DrawStringSS("FLEX ENGINE", color, AnchorPoint::TOP_RIGHT, glm::vec2(-0.03f, -0.05f), 0.0f);
			if (g_EngineInstance->IsSimulationPaused())
			{
				DrawStringSS("PAUSED", color, AnchorPoint::TOP_RIGHT, glm::vec2(-0.03f, -0.09f), 0.0f);
			}
			//DrawStringSS("1+/'TEST' \"TEST\"? ABCDEFGHIJKLMNOPQRSTUVWXYZ", glm::vec4(0.95f), AnchorPoint::CENTER, VEC2_ZERO, 1.5f, false);
			//DrawStringSS("#WOWIE# @LIQWIDICE FILE_NAME.ZIP * 17 (0)", glm::vec4(0.95f), AnchorPoint::CENTER, glm::vec2(0.0f, 0.1f), 1.5f, false);
			//DrawStringSS("[2+6=? M,M W.W ~`~ \\/ <A>]", glm::vec4(0.95f), AnchorPoint::CENTER, glm::vec2(0.0f, 0.2f), 1.5f, false);

			// Text stress test:
#if 0
			SetFont(SID("editor-02");
			real yO = -1.0f;
			std::string str;
			for (i32 i = 0; i < 5; ++i)
			{
				str = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
				DrawStringSS(str, glm::vec4(0.95f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 3.5f);
				yO += 0.05f;
				str = std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
				DrawStringSS(str, glm::vec4(0.95f, 0.6f, 0.95f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 3.5f);
				yO += 0.05f;
				str = std::string("0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"");
				DrawStringSS(str, glm::vec4(0.8f, 0.9f, 0.1f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 3.5f);
				yO += 0.05f;
				str = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
				DrawStringSS(str, glm::vec4(0.95f, 0.1f, 0.5f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 3.5f);
				yO += 0.05f;
			}

			SetFont(SID("editor-01"));
			yO = 0.0f;
			for (i32 i = 0; i < 3; ++i)
			{
				str = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
				DrawStringSS(str, glm::vec4(0.95f, 0.5f, 0.1f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 6);
				yO += 0.1f;
				str = std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
				DrawStringSS(str, glm::vec4(0.55f, 0.6f, 0.95f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 6);
				yO += 0.1f;
				str = std::string("0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"0123456789 -=!@#$%^&*()_+`~\\|/?<>,.*;:[]{}\'\"");
				DrawStringSS(str, glm::vec4(0.0f, 0.9f, 0.7f, 1.0f), AnchorPoint::CENTER, glm::vec2(0.0f, yO), 6);
				yO += 0.1f;
			}

			//std::string str = std::string("XYZ");
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::TOP_LEFT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::TOP, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::TOP_RIGHT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::RIGHT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::BOTTOM_RIGHT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::BOTTOM, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::BOTTOM_LEFT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::LEFT, VEC2_ZERO, 3, &letterYOffsetsEmpty);
			//DrawStringSS(str, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::CENTER, VEC2_ZERO, 3, &letterYOffsetsEmpty);

			//std::string fxaaEnabledStr = std::string("FXAA: ") + (m_PostProcessSettings.bEnableFXAA ? "1" : "0");
			//DrawStringSS(fxaaEnabledStr, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::TOP_RIGHT, glm::vec2(-0.01f, 0.0f), 5, &letterYOffsetsEmpty);
			//glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
			//std::string resolutionStr = "Frame buffer size: " +  IntToString(frameBufferSize.x) + "x" + IntToString(frameBufferSize.y);
			//DrawStringSS(resolutionStr, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), AnchorPoint::TOP_RIGHT, glm::vec2(-0.01f, 0.04f), 5, &letterYOffsetsEmpty);
#endif

			if (m_EditorStrSecRemaining > 0.0f)
			{
				SetFont(SID("editor-01"));
				real alpha = glm::clamp(m_EditorStrSecRemaining / (m_EditorStrSecDuration*m_EditorStrFadeDurationPercent),
					0.0f, 1.0f);
				DrawStringSS(m_EditorMessage, glm::vec4(1.0f, 1.0f, 1.0f, alpha), AnchorPoint::CENTER, VEC2_ZERO, 3);
			}

			{
				std::vector<TextVertex2D> textVerticesSS;
				UpdateTextBufferSS(textVerticesSS);

				u32 bufferByteCount = (u32)(textVerticesSS.size() * sizeof(TextVertex2D));

				glBindVertexArray(m_TextQuadSS_VAO);
				glBindBuffer(GL_ARRAY_BUFFER, m_TextQuadSS_VBO);
				glBufferData(GL_ARRAY_BUFFER, bufferByteCount, textVerticesSS.data(), GL_DYNAMIC_DRAW);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindVertexArray(0);
			}


			DrawTextSS();

			if (g_EngineInstance->IsRenderingImGui())
			{
				PROFILE_AUTO("ImGuiRender");

				GL_PUSH_DEBUG_GROUP("ImGui");

				ImGui::Render();
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

				GL_POP_DEBUG_GROUP();
			}

			SwapBuffers();
			++m_FramesRendered;

			if (m_bCaptureScreenshot && screenshotAsyncTextureSave == nullptr)
			{
				m_bCaptureScreenshot = false;

				const glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();

				TextureHandle handle;
				handle.internalFormat = GL_RGBA16F;
				handle.format = GL_RGBA;
				handle.type = GL_FLOAT;

				GLuint newFrameBufferFBO;
				glGenFramebuffers(1, &newFrameBufferFBO);
				glBindFramebuffer(GL_FRAMEBUFFER, newFrameBufferFBO);

				GenerateFrameBufferTexture(&handle.id,
					0,
					handle.internalFormat,
					handle.format,
					handle.type,
					frameBufferSize);

				glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, newFrameBufferFBO);
				glBlitFramebuffer(0, 0, frameBufferSize.x, frameBufferSize.y, 0, 0, frameBufferSize.x, frameBufferSize.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

				std::string dateTimeStr = GetDateString_YMDHMS();
				std::string relativeFilePath = ROOT_LOCATION "screenshots/";
				relativeFilePath += dateTimeStr + ".png";
				const std::string absoluteFilePath = RelativePathToAbsolute(relativeFilePath);
				StartAsyncTextureSaveToFile(absoluteFilePath, ImageFormat::PNG, handle.id, frameBufferSize.x, frameBufferSize.y, 3, true, &screenshotAsyncTextureSave);

				glDeleteTextures(1, &handle.id);
				glDeleteFramebuffers(1, &newFrameBufferFBO);
			}
		}

		void GLRenderer::BatchRenderObjects()
		{
			PROFILE_AUTO("BatchRenderObjects");

			m_bRebatchRenderObjects = false;

			m_DeferredRenderObjectBatches.clear();
			m_ForwardRenderObjectBatches.clear();
			m_DepthAwareEditorRenderObjectBatch.clear();
			m_DepthUnawareEditorRenderObjectBatch.clear();

			// Sort render objects into deferred + forward buckets
			for (const auto& materialPair : m_Materials)
			{
				MaterialID matID = materialPair.first;
				ShaderID shaderID = materialPair.second.material.shaderID;
				if (shaderID == InvalidShaderID)
				{
					PrintWarn("GLRenderer::BatchRenderObjects > Material has invalid shaderID: %s\n",
							  materialPair.second.material.name.c_str());
					continue;
				}
				GLShader* shader = &m_Shaders[shaderID];

				if (shader->shader.bDeferred)
				{
					GLRenderObjectBatch batch = {};
					for (GLRenderObject* renderObject : m_RenderObjects)
					{
						if (renderObject &&
							renderObject->gameObject->IsVisible() &&
							renderObject->materialID == matID &&
							!renderObject->bEditorObject &&
							renderObject->vertexBufferData)
						{
							batch.push_back(renderObject);
						}
					}
					if (!batch.empty())
					{
						m_DeferredRenderObjectBatches.push_back(batch);
					}
				}
				else
				{
					GLRenderObjectBatch batch = {};
					for (GLRenderObject* renderObject : m_RenderObjects)
					{
						if (renderObject &&
							renderObject->gameObject->IsVisible() &&
							renderObject->materialID == matID &&
							!renderObject->bEditorObject &&
							renderObject->vertexBufferData)
						{
							batch.push_back(renderObject);
						}
					}
					if (!batch.empty())
					{
						m_ForwardRenderObjectBatches.push_back(batch);
					}
				}
			}

			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject &&
					renderObject->gameObject->IsVisible() &&
					renderObject->bEditorObject &&
					renderObject->vertexBufferData)
				{
					if (renderObject->bDepthWriteEnable)
					{
						m_DepthAwareEditorRenderObjectBatch.push_back(renderObject);
					}
					else
					{
						m_DepthUnawareEditorRenderObjectBatch.push_back(renderObject);
					}
				}
			}

#if DEBUG
			u32 visibleObjectCount = 0;
			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject &&
					renderObject->gameObject->IsVisible() &&
					!renderObject->bEditorObject &&
					renderObject->vertexBufferData)
				{
					++visibleObjectCount;
				}
			}

			u32 accountedForObjectCount = 0;
			for (const GLRenderObjectBatch& batch : m_DeferredRenderObjectBatches)
			{
				accountedForObjectCount += batch.size();
			}

			for (const GLRenderObjectBatch& batch : m_ForwardRenderObjectBatches)
			{
				accountedForObjectCount += batch.size();
			}

			if (visibleObjectCount != accountedForObjectCount)
			{
				PrintError("BatchRenderObjects didn't account for every visible object!\n");
			}
#endif
		}

		void GLRenderer::DrawShadowDepthMaps()
		{
			PROFILE_AUTO("DrawShadowDepthMaps");

			GL_PUSH_DEBUG_GROUP("Shadow Map Depths");

			if (m_DirectionalLight != nullptr && m_DirectionalLight->bCastShadow && m_DirectionalLight->data.enabled)
			{
				GLMaterial* material = &m_Materials[m_ShadowMaterialID];
				GLShader* shader = &m_Shaders[material->material.shaderID];
				glUseProgram(shader->program);

				glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowMapFBO);

				glViewport(0, 0, m_ShadowMapSize, m_ShadowMapSize);

				glDepthMask(GL_TRUE);
				glDrawBuffer(GL_NONE);

				glClear(GL_DEPTH_BUFFER_BIT);

				DrawCallInfo drawCallInfo = {};
				drawCallInfo.materialOverride = m_ShadowMaterialID;

				glm::mat4 view, proj;
				ComputeDirLightViewProj(view, proj);

				glm::mat4 lightViewProj = proj * view;
				glUniformMatrix4fv(material->uniformIDs.lightViewProjection, 1, GL_FALSE, &lightViewProj[0][0]);

				glCullFace(GL_FRONT);

				for (const GLRenderObjectBatch& batch : m_DeferredRenderObjectBatches)
				{
					DrawRenderObjectBatch(batch, drawCallInfo);
				}

				glCullFace(GL_BACK);
			}

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawDeferredObjects(const DrawCallInfo& drawCallInfo)
		{
			const char* profileBlockName = drawCallInfo.bRenderToCubemap ? "DrawDeferredObjectsCubemap" : "DrawDeferredObjects";
			PROFILE_AUTO(profileBlockName);

			GL_PUSH_DEBUG_GROUP("Deferred Objects");

			if (!drawCallInfo.bDeferred)
			{
				PrintError("DrawDeferredObjects was called with a drawCallInfo which isn't set to be deferred!\n");
			}

			if (drawCallInfo.bRenderToCubemap)
			{
				// TODO: Bind depth buffer to cubemap's depth buffer (needs to generated?)

				GLRenderObject* cubemapRenderObject = GetRenderObject(drawCallInfo.cubemapObjectRenderID);
				GLMaterial* cubemapMaterial = &m_Materials[cubemapRenderObject->materialID];

				glm::vec2 cubemapSize = cubemapMaterial->material.cubemapSamplerSize;

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

				glViewport(0, 0, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);
			}
			else
			{
				glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
				glViewport(0, 0, (GLsizei)frameBufferSize.x, (GLsizei)frameBufferSize.y);

				glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferHandle);
				glBindRenderbuffer(GL_RENDERBUFFER, m_gBufferDepthHandle);
			}

			{
				const i32 FRAMEBUFFER_COUNT = 3;
				GLenum attachments[FRAMEBUFFER_COUNT] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
				glDrawBuffers(FRAMEBUFFER_COUNT, attachments);
			}

			glDepthMask(GL_TRUE);

			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			for (GLRenderObjectBatch& batch : m_DeferredRenderObjectBatches)
			{
				DrawRenderObjectBatch(batch, drawCallInfo);
			}

			glUseProgram(0);
			glBindVertexArray(0);

			{
				const i32 FRAMEBUFFER_COUNT = 1;
				u32 attachments[FRAMEBUFFER_COUNT] = { GL_COLOR_ATTACHMENT0 };
				glDrawBuffers(FRAMEBUFFER_COUNT, attachments);
			}

			// Copy depth from G-Buffer to default render target
			if (drawCallInfo.bRenderToCubemap)
			{
				// No blit is needed, since we already drew to the cubemap depth
			}
			else
			{
				const glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();

				glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gBufferHandle);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_Offscreen0RBO);
				glBlitFramebuffer(0, 0, frameBufferSize.x, frameBufferSize.y, 0, 0, frameBufferSize.x,
					frameBufferSize.y, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferHandle);
			glBindRenderbuffer(GL_RENDERBUFFER, m_gBufferDepthHandle);

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::ShadeDeferredObjects(const DrawCallInfo& drawCallInfo)
		{
			const char* profileBlockName = drawCallInfo.bRenderToCubemap ? "DrawGBufferContentsCubemap" : "ShadeDeferredObjects";
			PROFILE_AUTO(profileBlockName);

			GL_PUSH_DEBUG_GROUP("Shade Deferred");

			if (drawCallInfo.bDeferred)
			{
				PrintError("ShadeDeferredObjects was called with a drawCallInfo set to deferred!\n");
			}

			if (!m_SkyBoxMesh)
			{
				PrintError("Attempted to draw GBuffer contents to cubemap before skybox object was created!\n");
				return;
			}

			if (!m_gBufferQuadVertexBufferData.vertexData)
			{
				// Generate GBuffer if not already generated
				GenerateGBuffer();
			}

			if (drawCallInfo.bRenderToCubemap)
			{
				GLRenderObject* skybox = GetRenderObject(m_SkyBoxMesh->GetRenderID());

				GLRenderObject* cubemapObject = GetRenderObject(drawCallInfo.cubemapObjectRenderID);
				GLMaterial* cubemapMaterial = &m_Materials[cubemapObject->materialID];
				GLShader* cubemapShader = &m_Shaders[cubemapMaterial->material.shaderID];

				glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat,
					(GLsizei)cubemapMaterial->material.cubemapSamplerSize.x,
					(GLsizei)cubemapMaterial->material.cubemapSamplerSize.y);

				glUseProgram(cubemapShader->program);

				glBindVertexArray(skybox->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, skybox->VBO);

				UpdatePerObjectUniforms(cubemapMaterial, skybox->gameObject->GetTransform()->GetWorldTransform());

				u32 bindingOffset = BindDeferredFrameBufferTextures(cubemapMaterial);
				BindTextures(&cubemapShader->shader, cubemapMaterial, bindingOffset);

				if (skybox->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(skybox->cullFace);
				}

				glDepthFunc(DepthTestFuncToGlenum(drawCallInfo.depthTestFunc));
				glDepthMask(BoolToGLBoolean(drawCallInfo.bWriteToDepth));

				glUniformMatrix4fv(cubemapMaterial->uniformIDs.projection, 1, GL_FALSE, &m_CaptureProjection[0][0]);

				glBindRenderbuffer(GL_RENDERBUFFER, 0);

				// Override cam pos with reflection probe pos
				glm::vec3 reflectionProbePos(cubemapObject->gameObject->GetTransform()->GetWorldPosition());
				glUniform4fv(cubemapMaterial->uniformIDs.camPos, 1, &reflectionProbePos.x);

				for (i32 face = 0; face < 6; ++face)
				{
					glUniformMatrix4fv(cubemapMaterial->uniformIDs.view, 1, GL_FALSE, &m_CaptureViews[face][0][0]);

					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapSamplerID, 0);

					// Draw cube (TODO: Use "cube" object to be less confusing)
					glDrawArrays(skybox->topology, 0, (GLsizei)skybox->vertexBufferData->VertexCount);
				}
			}
			else
			{
				glBindFramebuffer(GL_FRAMEBUFFER, m_Offscreen0FBO);
				glBindRenderbuffer(GL_RENDERBUFFER, m_Offscreen0RBO);

				glDepthMask(GL_TRUE);

				glClear(GL_COLOR_BUFFER_BIT); // Don't clear depth - we just copied it over from the GBuffer

				GLRenderObject* gBufferQuad = GetRenderObject(m_GBufferQuadRenderID);

				GLMaterial* material = &m_Materials[gBufferQuad->materialID];
				GLShader* glShader = &m_Shaders[material->material.shaderID];
				Shader* shader = &glShader->shader;
				glUseProgram(glShader->program);

				glBindVertexArray(gBufferQuad->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, gBufferQuad->VBO);

				UpdatePerObjectUniforms(material, gBufferQuad->gameObject->GetTransform()->GetWorldTransform());

				u32 bindingOffset = BindFrameBufferTextures(material);
				BindTextures(shader, material, bindingOffset);

				if (gBufferQuad->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(gBufferQuad->cullFace);
				}

				glDepthFunc(DepthTestFuncToGlenum(drawCallInfo.depthTestFunc));
				glDepthMask(BoolToGLBoolean(drawCallInfo.bWriteToDepth));

				glDrawArrays(gBufferQuad->topology, 0, (GLsizei)gBufferQuad->vertexBufferData->VertexCount);
			}

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawForwardObjects(const DrawCallInfo& drawCallInfo)
		{
			const char* profileBlockName = drawCallInfo.bRenderToCubemap ? "DrawForwardObjectsCubemap" : "DrawForwardObjects";
			PROFILE_AUTO(profileBlockName);

			GL_PUSH_DEBUG_GROUP("Forward Objects");

			if (drawCallInfo.bDeferred)
			{
				PrintError("DrawForwardObjects was called with a drawCallInfo which is set to be deferred!\n");
			}

			for (GLRenderObjectBatch& batch : m_ForwardRenderObjectBatches)
			{
				DrawRenderObjectBatch(batch, drawCallInfo);
			}

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawDepthAwareEditorObjects(const DrawCallInfo& drawCallInfo)
		{
			if (!m_DepthAwareEditorRenderObjectBatch.empty())
			{
				PROFILE_AUTO("DrawDepthAwareEditorObjects");

				GL_PUSH_DEBUG_GROUP("Depth Aware Editor Objects");

				// TODO: Put in drawCallInfo
				glCullFace(GL_BACK);
				glEnable(GL_CULL_FACE);

				DrawRenderObjectBatch(m_DepthAwareEditorRenderObjectBatch, drawCallInfo);

				GL_POP_DEBUG_GROUP();
			}
		}

		void GLRenderer::DrawSelectedObjectWireframe(const DrawCallInfo& drawCallInfo)
		{
			UNREFERENCED_PARAMETER(drawCallInfo);

			const std::vector<GameObject*> selectedObjects = g_EngineInstance->GetSelectedObjects();
			if (!selectedObjects.empty())
			{
				GL_PUSH_DEBUG_GROUP("Selected Object Wireframe");

				GLRenderObjectBatch selectedObjectRenderBatch;
				for (GameObject* selectedObject : selectedObjects)
				{
					RenderID renderID = selectedObject->GetRenderID();
					if (renderID != InvalidRenderID)
					{
						GLRenderObject* renderObject = GetRenderObject(renderID);
						if (renderObject)
						{
							selectedObjectRenderBatch.push_back(renderObject);
						}
					}
				}

				GetMaterial(m_SelectedObjectMatID).colorMultiplier = GetSelectedObjectColorMultiplier();

				DrawCallInfo selectedObjectsDrawInfo = {};
				selectedObjectsDrawInfo.materialOverride = m_SelectedObjectMatID;
				selectedObjectsDrawInfo.bWireframe = true;
				selectedObjectsDrawInfo.depthTestFunc = DepthTestFunc::ALWAYS;
				selectedObjectsDrawInfo.bWriteToDepth = false;
				DrawRenderObjectBatch(selectedObjectRenderBatch, selectedObjectsDrawInfo);

				GL_POP_DEBUG_GROUP();
			}
		}

		void GLRenderer::DrawDepthUnawareEditorObjects(const DrawCallInfo& drawCallInfo)
		{
			if (!m_DepthAwareEditorRenderObjectBatch.empty())
			{
				PROFILE_AUTO("DrawDepthUnawareEditorObjects");

				GL_PUSH_DEBUG_GROUP("Depth Unaware Editor Objects");

				// TODO: Put in drawCallInfo
				glCullFace(GL_BACK);
				glEnable(GL_CULL_FACE);

				DrawRenderObjectBatch(m_DepthUnawareEditorRenderObjectBatch, drawCallInfo);

				GL_POP_DEBUG_GROUP();
			}
		}

		void GLRenderer::ApplyPostProcessing()
		{
			PROFILE_AUTO("ApplyPostProcessing");

			GL_PUSH_DEBUG_GROUP("Post Processing");

			SpriteQuadDrawInfo drawInfo = {};

			drawInfo.FBO = m_Offscreen1FBO;
			drawInfo.RBO = m_Offscreen1RBO;

			bool bFXAAEnabled = (m_bPostProcessingEnabled && m_PostProcessSettings.bEnableFXAA);

			if (!bFXAAEnabled)
			{
				drawInfo.FBO = 0;
				drawInfo.RBO = 0;
			}

			glm::vec3 pos(0.0f);
			glm::quat rot = glm::quat(glm::vec3(0.0));
			glm::vec3 scale(1.0f);
			glm::vec4 color(1.0f);

			drawInfo.bScreenSpace = true;
			drawInfo.bReadDepth = false;
			drawInfo.bWriteDepth = false;
			drawInfo.pos = pos;
			drawInfo.rotation = rot;
			drawInfo.scale = scale;
			drawInfo.materialID = m_PostProcessMatID;
			drawInfo.color = color;
			drawInfo.anchor = AnchorPoint::CENTER;
			drawInfo.textureHandleID = m_OffscreenTexture0Handle.id;
			drawInfo.spriteObjectRenderID = m_Quad2DRenderID;

			DrawSpriteQuad(drawInfo);

			if (bFXAAEnabled)
			{
				drawInfo.FBO = 0;
				drawInfo.RBO = 0;
				scale.y = -1.0f;

				drawInfo.textureHandleID = m_OffscreenTexture1Handle.id;
				drawInfo.materialID = m_PostFXAAMatID;
				DrawSpriteQuad(drawInfo);
			}
			GL_POP_DEBUG_GROUP();

			{
				GL_PUSH_DEBUG_GROUP("Blit Depth");

				const glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
				glBindFramebuffer(GL_READ_FRAMEBUFFER, m_Offscreen0RBO);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
				glBlitFramebuffer(0, 0, frameBufferSize.x, frameBufferSize.y,
								  0, 0, frameBufferSize.x, frameBufferSize.y,
								  GL_DEPTH_BUFFER_BIT, GL_NEAREST);

				GL_POP_DEBUG_GROUP();
			}
		}

		void GLRenderer::DrawScreenSpaceSprites()
		{
			GL_PUSH_DEBUG_GROUP("Screen-space Sprites");

			{
				PROFILE_AUTO("DrawScreenSpaceSprites > Display profiler frame");
				Profiler::DrawDisplayedFrame();
			}

			PROFILE_AUTO("DrawScreenSpaceSprites");

			for (const SpriteQuadDrawInfo& drawInfo : m_QueuedSSSprites)
			{
				DrawSpriteQuad(drawInfo);
			}
			m_QueuedSSSprites.clear();

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawWorldSpaceSprites()
		{
			PROFILE_AUTO("DrawWorldSpaceSprites");

			GL_PUSH_DEBUG_GROUP("World-space Sprites");

			for (SpriteQuadDrawInfo& drawInfo : m_QueuedWSSprites)
			{
				drawInfo.FBO = m_Offscreen0FBO;
				drawInfo.RBO = m_Offscreen0RBO;
				DrawSpriteQuad(drawInfo);
			}
			m_QueuedWSSprites.clear();

			BaseCamera* cam = g_CameraManager->CurrentCamera();
			if (!cam->bIsGameplayCam)
			{
				glm::vec3 scale(1.0f, -1.0f, 1.0f);

				SpriteQuadDrawInfo drawInfo = {};
				drawInfo.FBO = m_Offscreen0FBO;
				drawInfo.RBO = m_Offscreen0RBO;
				drawInfo.bScreenSpace = false;
				drawInfo.bReadDepth = true;
				drawInfo.bWriteDepth = true;
				drawInfo.scale = scale;
				drawInfo.materialID = m_SpriteMatID;
				drawInfo.spriteObjectRenderID = m_Quad3DRenderID;

				glm::vec3 camPos = cam->GetPosition();
				glm::vec3 camUp = cam->GetUp();
				for (i32 i = 0; i < m_NumPointLightsEnabled; ++i)
				{
					if (m_PointLights[i].enabled)
					{
						// TODO: Sort back to front? Or clear depth and then enable depth test
						drawInfo.pos = m_PointLights[i].pos;
						drawInfo.color = glm::vec4(m_PointLights[i].color * 1.5f, 1.0f);
						drawInfo.textureHandleID = m_LoadedTextures[m_PointLightIconID]->handle;
						glm::mat4 rotMat = glm::lookAt(camPos, glm::vec3(m_PointLights[i].pos), camUp);
						drawInfo.rotation = glm::conjugate(glm::toQuat(rotMat));
						DrawSpriteQuad(drawInfo);
					}
				}

				if (m_DirectionalLight != nullptr && m_DirectionalLight->data.enabled)
				{
					drawInfo.color = glm::vec4(m_DirectionalLight->data.color * 1.5f, 1.0f);
					drawInfo.pos = m_DirectionalLight->pos;
					drawInfo.textureHandleID = m_LoadedTextures[m_DirectionalLightIconID]->handle;
					glm::mat4 rotMat = glm::lookAt(camPos, (glm::vec3)m_DirectionalLight->pos, camUp);
					drawInfo.rotation = glm::conjugate(glm::toQuat(rotMat));
					DrawSpriteQuad(drawInfo);

					glm::vec3 dirLightForward = m_DirectionalLight->data.dir;
					m_PhysicsDebugDrawer->drawLine(
						ToBtVec3(m_DirectionalLight->pos),
						ToBtVec3(m_DirectionalLight->pos - dirLightForward * 2.5f),
						btVector3(0.0f, 0.0f, 1.0f));
				}
			}

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawSpriteQuad(const SpriteQuadDrawInfo& drawInfo)
		{
			RenderID spriteRenderID = drawInfo.spriteObjectRenderID;
			if (spriteRenderID == InvalidRenderID)
			{
				if (drawInfo.bScreenSpace)
				{
					spriteRenderID = m_Quad2DRenderID;
				}
				else
				{
					spriteRenderID = m_Quad3DRenderID;
				}
			}
			GLRenderObject* spriteRenderObject = GetRenderObject(spriteRenderID);
			if (!spriteRenderObject ||
				(spriteRenderObject->bEditorObject && !g_EngineInstance->IsRenderingEditorObjects()))
			{
				return;
			}

			spriteRenderObject->materialID = drawInfo.materialID;
			if (spriteRenderObject->materialID == InvalidMaterialID)
			{
				spriteRenderObject->materialID = m_SpriteMatID;
			}

			GLMaterial& spriteMaterial = m_Materials[spriteRenderObject->materialID];
			GLShader& spriteShader = m_Shaders[spriteMaterial.material.shaderID];

			glUseProgram(spriteShader.program);

			const glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
			const real aspectRatio = (real)frameBufferSize.x / (real)frameBufferSize.y;

			if (spriteShader.shader.dynamicBufferUniforms.HasUniform(U_MODEL))
			{
				glm::vec3 translation = drawInfo.pos;
				glm::quat rotation = drawInfo.rotation;
				glm::vec3 scale = drawInfo.scale;

				if (!drawInfo.bRaw)
				{
					if (drawInfo.bScreenSpace)
					{
						glm::vec2 normalizedTranslation;
						glm::vec2 normalizedScale;
						NormalizeSpritePos(translation, drawInfo.anchor, scale, normalizedTranslation, normalizedScale);

						translation = glm::vec3(normalizedTranslation, 0.0f);
						scale = glm::vec3(normalizedScale, 1.0f);
					}
					else
					{
						translation.x /= aspectRatio;
					}
				}

				glm::mat4 model = (glm::translate(MAT4_IDENTITY, translation) *
								   glm::mat4(rotation) *
								   glm::scale(MAT4_IDENTITY, scale));

				glUniformMatrix4fv(spriteMaterial.uniformIDs.model, 1, GL_TRUE, &model[0][0]);
			}

			if (spriteShader.shader.constantBufferUniforms.HasUniform(U_VIEW))
			{
				if (drawInfo.bScreenSpace)
				{
					glm::mat4 view = MAT4_IDENTITY;

					glUniformMatrix4fv(spriteMaterial.uniformIDs.view, 1, GL_FALSE, &view[0][0]);
				}
				else
				{
					glm::mat4 view = g_CameraManager->CurrentCamera()->GetView();

					glUniformMatrix4fv(spriteMaterial.uniformIDs.view, 1, GL_FALSE, &view[0][0]);
				}
			}

			if (spriteShader.shader.constantBufferUniforms.HasUniform(U_PROJECTION))
			{
				if (drawInfo.bScreenSpace)
				{
					real r = aspectRatio;
					real t = 1.0f;
					glm::mat4 projection = glm::ortho(-r, r, -t, t);

					glUniformMatrix4fv(spriteMaterial.uniformIDs.projection, 1, GL_FALSE, &projection[0][0]);
				}
				else
				{
					glm::mat4 projection = g_CameraManager->CurrentCamera()->GetProjection();

					glUniformMatrix4fv(spriteMaterial.uniformIDs.projection, 1, GL_FALSE, &projection[0][0]);
				}
			}

			if (spriteShader.shader.dynamicBufferUniforms.HasUniform(U_COLOR_MULTIPLIER))
			{
				glUniform4fv(spriteMaterial.uniformIDs.colorMultiplier, 1, &drawInfo.color.r);
			}

			bool bEnableAlbedoSampler = (drawInfo.textureHandleID != 0 && drawInfo.bEnableAlbedoSampler);
			if (spriteShader.shader.dynamicBufferUniforms.HasUniform(U_ALBEDO_SAMPLER))
			{
				// TODO: glUniform1ui vs glUniform1i ?
				glUniform1ui(spriteMaterial.uniformIDs.enableAlbedoSampler, bEnableAlbedoSampler ? 1 : 0);
			}

			// http://www.graficaobscura.com/matrix/
			GLint cBSLocation = glGetUniformLocation(spriteShader.program, "contrastBrightnessSaturation");
			if (cBSLocation != -1)
			{
				glm::mat4 contrastBrightnessSaturation = GetPostProcessingMatrix();
				glUniformMatrix4fv(cBSLocation, 1, GL_FALSE, &contrastBrightnessSaturation[0][0]);
			}

			glViewport(0, 0, (GLsizei)frameBufferSize.x, (GLsizei)frameBufferSize.y);

			glBindFramebuffer(GL_FRAMEBUFFER, drawInfo.FBO);
			glBindRenderbuffer(GL_RENDERBUFFER, drawInfo.RBO);

			glBindVertexArray(spriteRenderObject->VAO);
			glBindBuffer(GL_ARRAY_BUFFER, spriteRenderObject->VBO);

			if (bEnableAlbedoSampler)
			{
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, drawInfo.textureHandleID);
			}

			// TODO: Use member
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			if (drawInfo.bReadDepth)
			{
				glDepthFunc(GL_GEQUAL);
			}
			else
			{
				glDepthFunc(GL_ALWAYS);
			}

			if (drawInfo.bWriteDepth)
			{
				glDepthMask(GL_TRUE);
			}
			else
			{
				glDepthMask(GL_FALSE);
			}

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			if (spriteRenderObject->cullFace == GL_NONE)
			{
				glDisable(GL_CULL_FACE);
			}
			else
			{
				glEnable(GL_CULL_FACE);
				glCullFace(spriteRenderObject->cullFace);
			}

			// TODO: Remove (use draw call info member)
			glDepthMask(spriteRenderObject->bDepthWriteEnable);
			glDrawArrays(spriteRenderObject->topology, 0, (GLsizei)spriteRenderObject->vertexBufferData->VertexCount);
		}

		void GLRenderer::DrawTextSS()
		{
			PROFILE_AUTO("DrawTextSS");

			GL_PUSH_DEBUG_GROUP("Text SS");

			bool bHasText = false;
			for (BitmapFont* font : m_FontsSS)
			{
				if (font->GetBufferSize() > 0)
				{
					bHasText = true;
					break;
				}
			}

			if (!bHasText)
			{
				return;
			}

			GLMaterial& fontMaterial = m_Materials[m_FontMatSSID];
			GLShader& fontShader = m_Shaders[fontMaterial.material.shaderID];

			glUseProgram(fontShader.program);

			// TODO: Allow per-string shadows? (currently only per-font is doable)
			//if (fontShader.shader.dynamicBufferUniforms.HasUniform("shadow"))
			//{
			//	static glm::vec2 shadow(0.01f, 0.1f);
			//	glUniform2f(glGetUniformLocation(fontShader.program, "shadow"), shadow.x, shadow.y);
			//}

			//if (fontShader.shader.dynamicBufferUniforms.HasUniform("colorMultiplier"))
			//{
			//	glm::vec4 color(1.0f);
			//	glUniform4fv(fontMaterial.uniformIDs.colorMultiplier, 1, &color.r);
			//}

			glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();

			glViewport(0, 0, (GLsizei)(frameBufferSize.x), (GLsizei)(frameBufferSize.y));

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindRenderbuffer(GL_RENDERBUFFER, 0);

			glBindVertexArray(m_TextQuadSS_VAO);
			glBindBuffer(GL_ARRAY_BUFFER, m_TextQuadSS_VBO);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glDisable(GL_CULL_FACE);

			glDepthFunc(GL_ALWAYS);
			glDepthMask(GL_FALSE);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			for (BitmapFont* font : m_FontsSS)
			{
				if (font->GetBufferSize() > 0)
				{
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, font->GetTexture()->handle);

					real aspectRatio = (real)frameBufferSize.x / (real)frameBufferSize.y;
					real r = aspectRatio;
					real t = 1.0f;
					glm::mat4 ortho = glm::ortho(-r, r, -t, t);

					// TODO: Find out how font sizes actually work
					//real scale = ((real)font->GetFontSize()) / 12.0f + sin(g_SecElapsedSinceProgramStart) * 2.0f;
					glm::vec3 scaleVec(1.0f);

					glm::mat4 transformMat = glm::scale(MAT4_IDENTITY, scaleVec) * ortho;
					glUniformMatrix4fv(fontMaterial.uniformIDs.transformMat, 1, GL_TRUE, &transformMat[0][0]);

					glm::vec2 texSize = (glm::vec2)font->GetTexture()->GetResolution();
					glUniform2fv(fontMaterial.uniformIDs.texSize, 1, &texSize.r);

					glDrawArrays(GL_POINTS, font->GetBufferStart(), font->GetBufferSize());
				}
			}

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawTextWS()
		{
			// TODO: Consolidate with DrawTextSS

			PROFILE_AUTO("DrawTextWS");

			GL_PUSH_DEBUG_GROUP("Text WS");

			bool bHasText = false;
			for (BitmapFont* font : m_FontsWS)
			{
				if (font->GetBufferSize() > 0)
				{
					bHasText = true;
					break;
				}
			}

			if (!bHasText)
			{
				return;
			}

			GLMaterial& fontMaterial = m_Materials[m_FontMatWSID];
			GLShader& fontShader = m_Shaders[fontMaterial.material.shaderID];

			glUseProgram(fontShader.program);

			glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();

			glViewport(0, 0, (GLsizei)(frameBufferSize.x), (GLsizei)(frameBufferSize.y));

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindRenderbuffer(GL_RENDERBUFFER, 0);

			glBindVertexArray(m_TextQuadWS_VAO);
			glBindBuffer(GL_ARRAY_BUFFER, m_TextQuadWS_VBO);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glDisable(GL_CULL_FACE);

			glDepthFunc(GL_GEQUAL);
			glDepthMask(GL_TRUE);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			glActiveTexture(GL_TEXTURE0);

			glm::mat4 transformMat = g_CameraManager->CurrentCamera()->GetViewProjection();

			for (BitmapFont* font : m_FontsWS)
			{
				if (font->GetBufferSize() > 0)
				{
					glBindTexture(GL_TEXTURE_2D, font->GetTexture()->handle);

					glUniformMatrix4fv(fontMaterial.uniformIDs.transformMat, 1, GL_FALSE, &transformMat[0][0]);

					glm::vec2 texSize = (glm::vec2)font->GetTexture()->GetResolution();
					glUniform2fv(fontMaterial.uniformIDs.texSize, 1, &texSize.r);

					glDrawArrays(GL_POINTS, font->GetBufferStart(), font->GetBufferSize());
				}
			}

			GL_POP_DEBUG_GROUP();
		}

		bool GLRenderer::LoadFont(BitmapFont** font,
								  i16 size,
								  const std::string& fontFilePath,
								  const std::string& renderedFontFilePath,
								  bool bForceRender,
								  bool bScreenSpace)
		{
			FT_Library ft;
			// TODO: Only do once per session?
			if (FT_Init_FreeType(&ft) != FT_Err_Ok)
			{
				PrintError("Failed to initialize FreeType\n");
				return false;
			}

			std::map<i32, FontMetric*> characters;
			std::array<glm::vec2i, 4> maxPos;

			std::vector<char> fileMemory;
			ReadFile(fontFilePath, fileMemory, true);

			FT_Face face = {};
			if (!LoadFontMetrics(fileMemory, fontFilePath, ft, font, size, bScreenSpace, &characters, &maxPos, &face))
			{
				return false;
			}

			std::string fileName = fontFilePath;
			StripLeadingDirectories(fileName);

			BitmapFont* newFont = *font;

			// TODO: Save in common place
			u32 sampleDensity = 32;
			u32 padding = 1;
			u32 spread = 5;

			bool bUsingPreRenderedTexture = false;
			if (!bForceRender)
			{
				if (FileExists(renderedFontFilePath))
				{
					GLTexture* fontTex = newFont->SetTexture(new GLTexture(renderedFontFilePath, 4, false, false, false));

					if (fontTex->LoadFromFile())
					{
						bUsingPreRenderedTexture = true;

						for (auto& charPair : characters)
						{
							FontMetric* metric = charPair.second;
							if (isspace(metric->character) ||
								metric->character == '\0')
							{
								continue;
							}

							u32 glyphIndex = FT_Get_Char_Index(face, metric->character);
							if (glyphIndex == 0)
							{
								continue;
							}

							metric->texCoord = metric->texCoord / glm::vec2((real)fontTex->width, (real)fontTex->height);
						}
					}
					else
					{
						newFont->ClearTexture();
						fontTex->Destroy();
						delete fontTex;
					}
				}
			}

			if (!bUsingPreRenderedTexture)
			{
				// Render to glyph atlas

				glm::vec2i textureSize(
					std::max(std::max(maxPos[0].x, maxPos[1].x), std::max(maxPos[2].x, maxPos[3].x)),
					std::max(std::max(maxPos[0].y, maxPos[1].y), std::max(maxPos[2].y, maxPos[3].y)));

				TextureParameters params(false);
				params.wrapS = GL_CLAMP_TO_EDGE;
				params.wrapT = GL_CLAMP_TO_EDGE;

				GLTexture* fontTex = newFont->SetTexture(new GLTexture(fileName, textureSize.x, textureSize.y, 4, GL_RGBA16F, GL_RGBA, GL_FLOAT));
				fontTex->CreateEmpty();
				fontTex->Build();
				fontTex->SetParameters(params);

				GLuint captureFBO;
				glGenFramebuffers(1, &captureFBO);
				glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fontTex->handle, 0);

				glViewport(0, 0, textureSize.x, textureSize.y);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

				ShaderID computeSDFShaderID;
				GetShaderID("compute_sdf", computeSDFShaderID);
				GLShader& computeSDFShader = m_Shaders[computeSDFShaderID];

				glUseProgram(computeSDFShader.program);

				glUniform1i(glGetUniformLocation(computeSDFShader.program, "highResTex"), 0);
				GLint texChannel = glGetUniformLocation(computeSDFShader.program, "texChannel");
				GLint charResolution = glGetUniformLocation(computeSDFShader.program, "charResolution");
				glUniform1f(glGetUniformLocation(computeSDFShader.program, "spread"), (real)spread);
				glUniform1f(glGetUniformLocation(computeSDFShader.program, "sampleDensity"), (real)sampleDensity);

				glEnable(GL_BLEND);
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_ONE, GL_ONE);

				GLRenderObject* gBufferRenderObject = GetRenderObject(m_GBufferQuadRenderID);

				for (auto& charPair : characters)
				{
					FontMetric* metric = charPair.second;

					u32 glyphIndex = FT_Get_Char_Index(face, metric->character);
					if (glyphIndex == 0)
					{
						continue;
					}

					if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER))
					{
						PrintError("Failed to load glyph with index %i\n", glyphIndex);
						continue;
					}

					if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP)
					{
						if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
						{
							PrintError("Failed to render glyph with index %i\n", glyphIndex);
							continue;
						}
					}

					if (face->glyph->bitmap.width == 0 ||
						face->glyph->bitmap.rows == 0)
					{
						continue;
					}

					FT_Bitmap alignedBitmap = {};
					if (FT_Bitmap_Convert(ft, &face->glyph->bitmap, &alignedBitmap, 4))
					{
						PrintError("Couldn't align free type bitmap size\n");
						continue;
					}

					u32 width = alignedBitmap.width;
					u32 height = alignedBitmap.rows;

					assert(width != 0 && height != 0);

					GLuint texHandle;
					glGenTextures(1, &texHandle);

					glActiveTexture(GL_TEXTURE0);

					glBindTexture(GL_TEXTURE_2D, texHandle);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, alignedBitmap.buffer);

					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glm::vec4 borderColor(0.0f);
					glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, &borderColor.r);

					if (metric->width > 0 && metric->height > 0)
					{
						glm::vec2i res = glm::vec2i(metric->width - padding * 2, metric->height - padding * 2);
						glm::vec2i viewportTL = glm::vec2i(metric->texCoord) + glm::vec2i(padding);

						glViewport(viewportTL.x, viewportTL.y, res.x, res.y);
						glActiveTexture(GL_TEXTURE0);
						glBindTexture(GL_TEXTURE_2D, texHandle);

						glUniform1i(texChannel, metric->channel);
						glUniform2f(charResolution, (real)res.x, (real)res.y);
						glActiveTexture(GL_TEXTURE0);
						glBindVertexArray(gBufferRenderObject->VAO);
						glBindBuffer(GL_ARRAY_BUFFER, gBufferRenderObject->VBO);
						glDrawArrays(gBufferRenderObject->topology, 0,
							(GLsizei)gBufferRenderObject->vertexBufferData->VertexCount);
					}

					glDeleteTextures(1, &texHandle);

					metric->texCoord = metric->texCoord / glm::vec2((real)textureSize.x, (real)textureSize.y);

					FT_Bitmap_Done(ft, &alignedBitmap);
				}

				glBindVertexArray(0);

				std::string savedSDFTextureAbsFilePath = RelativePathToAbsolute(renderedFontFilePath);
				fontTex->SaveToFileAsync(savedSDFTextureAbsFilePath, ImageFormat::PNG, false);

				// Cleanup
				glDisable(GL_BLEND);
				glDeleteFramebuffers(1, &captureFBO);
			}

			FT_Done_Face(face);
			FT_Done_FreeType(ft);


			// Initialize font shaders
			{
				MaterialID matID = bScreenSpace ? m_FontMatSSID : m_FontMatWSID;
				GLMaterial& mat = m_Materials[matID];
				GLShader& shader = m_Shaders[mat.material.shaderID];
				glUseProgram(shader.program);

				GLuint* VAO = bScreenSpace ? &m_TextQuadSS_VAO : &m_TextQuadWS_VAO;
				GLuint* VBO = bScreenSpace ? &m_TextQuadSS_VBO : &m_TextQuadWS_VBO;

				glGenVertexArrays(1, VAO);
				glGenBuffers(1, VBO);

				glBindVertexArray(*VAO);
				glBindBuffer(GL_ARRAY_BUFFER, *VBO);

				// TODO: Set this value to previous high water mark?
				i32 bufferSize = 500;
				glBufferData(GL_ARRAY_BUFFER, bufferSize, NULL, GL_DYNAMIC_DRAW);


				if (bScreenSpace)
				{
					glEnableVertexAttribArray(0);
					glEnableVertexAttribArray(1);
					glEnableVertexAttribArray(2);
					glEnableVertexAttribArray(3);
					glEnableVertexAttribArray(4);

					glVertexAttribPointer(0, (GLint)2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex2D), (GLvoid*)offsetof(TextVertex2D, pos));
					glVertexAttribPointer(1, (GLint)4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex2D), (GLvoid*)offsetof(TextVertex2D, color));
					glVertexAttribPointer(2, (GLint)2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex2D), (GLvoid*)offsetof(TextVertex2D, uv));
					glVertexAttribPointer(3, (GLint)4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex2D), (GLvoid*)offsetof(TextVertex2D, charSizePixelsCharSizeNorm));
					glVertexAttribIPointer(4, (GLint)1, GL_INT, (GLsizei)sizeof(TextVertex2D), (GLvoid*)offsetof(TextVertex2D, channel));
				}
				else
				{
					glEnableVertexAttribArray(0);
					glEnableVertexAttribArray(1);
					glEnableVertexAttribArray(2);
					glEnableVertexAttribArray(3);
					glEnableVertexAttribArray(4);
					glEnableVertexAttribArray(5);

					glVertexAttribPointer(0, (GLint)3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, pos));
					glVertexAttribPointer(1, (GLint)4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, color));
					glVertexAttribPointer(2, (GLint)3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, tangent));
					glVertexAttribPointer(3, (GLint)2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, uv));
					glVertexAttribPointer(4, (GLint)4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, charSizePixelsCharSizeNorm));
					glVertexAttribIPointer(5, (GLint)1, GL_INT, (GLsizei)sizeof(TextVertex3D), (GLvoid*)offsetof(TextVertex3D, channel));
				}
			}

			if (g_bEnableLogging_Loading)
			{
				if (bUsingPreRenderedTexture)
				{
					std::string textureFilePath = renderedFontFilePath;
					StripLeadingDirectories(textureFilePath);
					Print("Loaded font atlas texture from %s\n", textureFilePath.c_str());
				}
				else
				{
					Print("Rendered font atlas for %s\n", fileName.c_str());
				}
			}

			return true;
		}

		void GLRenderer::DrawStringSS(const std::string& str,
			const glm::vec4& color,
			AnchorPoint anchor,
			const glm::vec2& pos, // Positional offset from anchor
			real spacing,
			bool bRaw)
		{
			assert(m_CurrentFont != nullptr);

			TextCache newCache(str, anchor, pos, color, spacing, bRaw);
			m_CurrentFont->AddTextCache(newCache);
		}

		void GLRenderer::DrawStringWS(const std::string& str,
			const glm::vec4& color,
			const glm::vec3& pos,
			const glm::quat& rot,
			real spacing,
			bool bRaw)
		{
			assert(m_CurrentFont != nullptr);

			TextCache newCache(str, pos, rot, color, spacing, bRaw);
			m_CurrentFont->AddTextCache(newCache);
		}

		void GLRenderer::ComputeDirLightViewProj(glm::mat4& outView, glm::mat4& outProj)
		{
			if (m_DirectionalLight == nullptr)
			{
				outView = glm::lookAt(VEC3_ZERO, VEC3_FORWARD, VEC3_UP);
				outProj = glm::ortho(-1234.0f, 1234.0f, -4567.0f, 4567.0f, 1.0f, 0.01f);
				return;
			}

			outView = glm::lookAt(VEC3_ZERO, m_DirectionalLight->data.dir, VEC3_UP);

			real zoom = m_DirectionalLight->shadowMapZoom;
			outProj = glm::ortho(-zoom, zoom, -zoom, zoom, m_DirectionalLight->shadowMapNearPlane, m_DirectionalLight->shadowMapFarPlane);
		}

		void GLRenderer::DrawRenderObjectBatch(const GLRenderObjectBatch& batchedRenderObjects, const DrawCallInfo& drawCallInfo)
		{
			if (batchedRenderObjects.empty())
			{
				return;
			}

			MaterialID materialID = InvalidMaterialID;
			if (drawCallInfo.materialOverride == InvalidMaterialID)
			{
				materialID = (*batchedRenderObjects.begin())->materialID;
			}
			else
			{
				materialID = drawCallInfo.materialOverride;
			}
			GLMaterial* material = &m_Materials[materialID];
			if (material->material.shaderID == InvalidShaderID)
			{
				PrintWarn("Attempted to draw render object batch which uses invalid shader ID!\n");
				return;
			}
			GLShader* glShader = &m_Shaders[material->material.shaderID];
			Shader* shader = &glShader->shader;
			u32 boundProgram = glShader->program;
			glUseProgram(boundProgram);

			if (drawCallInfo.bWireframe)
			{
				glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			}

			for (GLRenderObject* renderObject : batchedRenderObjects)
			{
				if (drawCallInfo.materialOverride == InvalidMaterialID)
				{
					materialID = renderObject->materialID;
					material = &m_Materials[materialID];
					glShader = &m_Shaders[material->material.shaderID];
					shader = &glShader->shader;
				}

				assert(glShader->program == boundProgram);

				glBindVertexArray(renderObject->VAO);
				glBindBuffer(GL_ARRAY_BUFFER, renderObject->VBO);

				if (renderObject->cullFace == GL_NONE)
				{
					glDisable(GL_CULL_FACE);
				}
				else
				{
					glEnable(GL_CULL_FACE);
					glCullFace(renderObject->cullFace);
				}

				if (shader->bTranslucent)
				{
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
				else
				{
					glDisable(GL_BLEND);
				}

				glDepthFunc(DepthTestFuncToGlenum(drawCallInfo.depthTestFunc));
				glDepthMask(BoolToGLBoolean(drawCallInfo.bWriteToDepth));

				UpdatePerObjectUniforms(renderObject->renderID, material);

				BindTextures(shader, material);

				if (drawCallInfo.bRenderToCubemap)
				{
					// renderObject->gameObject->IsStatic()

					GLRenderObject* cubemapRenderObject = GetRenderObject(drawCallInfo.cubemapObjectRenderID);
					GLMaterial* cubemapMaterial = &m_Materials[cubemapRenderObject->materialID];

					glm::vec2 cubemapSize = cubemapMaterial->material.cubemapSamplerSize;

					glBindFramebuffer(GL_FRAMEBUFFER, m_CaptureFBO);
					glBindRenderbuffer(GL_RENDERBUFFER, m_CaptureRBO);
					glRenderbufferStorage(GL_RENDERBUFFER, m_CaptureDepthInternalFormat, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);
					glViewport(0, 0, (GLsizei)cubemapSize.x, (GLsizei)cubemapSize.y);

					if (material->uniformIDs.projection == 0)
					{
						PrintWarn("Attempted to draw object to cubemap but uniformIDs.projection is not set on object: %s\n",
								  renderObject->gameObject->GetName().c_str());
						continue;
					}

					// Use capture projection matrix
					glUniformMatrix4fv(material->uniformIDs.projection, 1, GL_FALSE, &m_CaptureProjection[0][0]);

					// TODO: Test if this is actually correct
					glm::vec3 cubemapTranslation = -cubemapRenderObject->gameObject->GetTransform()->GetWorldPosition();
					for (u32 face = 0; face < 6; ++face)
					{
						glm::mat4 view = glm::translate(m_CaptureViews[face], cubemapTranslation);

						// This doesn't work because it flips the winding order of things (I think), maybe just account for that?
						// Flip vertically to match cubemap, cubemap shouldn't even be captured here eventually?
						//glm::mat4 view = glm::translate(glm::scale(m_CaptureViews[face], glm::vec3(1.0f, -1.0f, 1.0f)), cubemapTranslation);

						glUniformMatrix4fv(material->uniformIDs.view, 1, GL_FALSE, &view[0][0]);

						if (drawCallInfo.bDeferred)
						{
							//constexpr i32 numBuffers = 3;
							//u32 attachments[numBuffers] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
							//glDrawBuffers(numBuffers, attachments);

							for (u32 j = 0; j < cubemapMaterial->cubemapSamplerGBuffersIDs.size(); ++j)
							{
								glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + j,
									GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapSamplerGBuffersIDs[j].id, 0);
							}
						}
						else
						{
							glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
								GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapSamplerID, 0);
						}

						glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
							GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemapMaterial->cubemapDepthSamplerID, 0);

						if (renderObject->bIndexed)
						{
							glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderObject->IBO);
							GLsizei count = (GLsizei)renderObject->indices->size();
							glDrawElements(renderObject->topology, count, GL_UNSIGNED_INT, (void*)0);
						}
						else
						{
							glDrawArrays(renderObject->topology, 0, (GLsizei)renderObject->vertexBufferData->VertexCount);
						}
					}
				}
				else
				{
					if (shader->bTranslucent)
					{
						glEnable(GL_BLEND);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					}
					else
					{
						glDisable(GL_BLEND);
					}

					if (renderObject->bIndexed)
					{
						glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderObject->IBO);
						GLsizei count = (GLsizei)renderObject->indices->size();
						glDrawElements(renderObject->topology, count, GL_UNSIGNED_INT, (void*)0);
					}
					else
					{
						glDrawArrays(renderObject->topology, 0, (GLsizei)renderObject->vertexBufferData->VertexCount);
					}
				}

				if (m_bDisplayBoundingVolumes && renderObject->gameObject)
				{
					MeshComponent* mesh = renderObject->gameObject->GetMeshComponent();
					if (mesh)
					{
						btVector3 centerWS = ToBtVec3(mesh->GetBoundingSphereCenterPointWS());
						m_PhysicsDebugDrawer->drawSphere(centerWS, 0.1f, btVector3(0.8f, 0.2f, 0.1f));
						m_PhysicsDebugDrawer->drawSphere(centerWS, mesh->GetScaledBoundingSphereRadius(), btVector3(0.2f, 0.8f, 0.1f));

						Transform* transform = renderObject->gameObject->GetTransform();

						//glm::vec3 transformedMin = glm::vec3(transform->GetWorldTransform() * glm::vec4(mesh->m_MinPoint, 1.0f));
						//glm::vec3 transformedMax = glm::vec3(transform->GetWorldTransform() * glm::vec4(mesh->m_MaxPoint, 1.0f));
						//btVector3 minPos = ToBtVec3(transformedMin);
						//btVector3 maxPos = ToBtVec3(transformedMax);
						//m_PhysicsDebugDrawer->drawSphere(minPos, 0.1f, btVector3(0.2f, 0.8f, 0.1f));
						//m_PhysicsDebugDrawer->drawSphere(maxPos, 0.1f, btVector3(0.2f, 0.8f, 0.1f));

						btVector3 scaledMin = ToBtVec3(transform->GetWorldScale() * mesh->m_MinPoint);
						btVector3 scaledMax = ToBtVec3(transform->GetWorldScale() * mesh->m_MaxPoint);

						btTransform transformBT = ToBtTransform(*transform);
						m_PhysicsDebugDrawer->drawBox(scaledMin, scaledMax, transformBT, btVector3(0.85f, 0.8f, 0.85f));
					}
				}
			}

			if (drawCallInfo.bWireframe)
			{
				glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			}
		}

		u32 GLRenderer::BindTextures(Shader* shader, GLMaterial* glMaterial, u32 startingBinding)
		{
			Material* material = &glMaterial->material;

			struct Tex
			{
				bool needed;
				bool enabled;
				u32 textureID;
				GLenum target;
			};

			Tex textures[] = {
				{ shader->bNeedAlbedoSampler, material->enableAlbedoSampler, glMaterial->albedoSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedMetallicSampler, material->enableMetallicSampler, glMaterial->metallicSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedRoughnessSampler, material->enableRoughnessSampler, glMaterial->roughnessSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedAOSampler, material->enableAOSampler, glMaterial->aoSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedNormalSampler, material->enableNormalSampler, glMaterial->normalSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedBRDFLUT, material->enableBRDFLUT, glMaterial->brdfLUTSamplerID, GL_TEXTURE_2D },
				{ shader->bNeedShadowMap, true, m_ShadowMapTexture.id, GL_TEXTURE_2D },
				{ shader->bNeedIrradianceSampler, material->enableIrradianceSampler, glMaterial->irradianceSamplerID, GL_TEXTURE_CUBE_MAP },
				{ shader->bNeedPrefilteredMap, material->enablePrefilteredMap, glMaterial->prefilteredMapSamplerID, GL_TEXTURE_CUBE_MAP },
				{ shader->bNeedCubemapSampler, material->enableCubemapSampler, glMaterial->cubemapSamplerID, GL_TEXTURE_CUBE_MAP },
			};
			// TODO: Update reserve count when adding more textures

			u32 binding = startingBinding;
			for (const Tex& tex : textures)
			{
				if (tex.needed)
				{
					if (tex.enabled)
					{
						if (tex.textureID == InvalidID)
						{
							PrintError("TextureID is invalid! material: %s, binding: %i\n",
									   glMaterial->material.name.c_str(), binding);
						}
						else
						{
							GLenum activeTexture = (GLenum)(GL_TEXTURE0 + (GLuint)binding);
							glActiveTexture(activeTexture);
							glBindTexture(tex.target, (GLuint)tex.textureID);
						}
					}
					++binding;
				}
			}

			return binding;
		}

		u32 GLRenderer::BindFrameBufferTextures(GLMaterial* glMaterial, u32 startingBinding)
		{
			Material* material = &glMaterial->material;

			struct Tex
			{
				bool needed;
				bool enabled;
				u32 textureID;
			};

			if (material->frameBuffers.empty())
			{
				PrintWarn("Attempted to bind frame buffers on material that doesn't contain any framebuffers!\n");
				return startingBinding;
			}

			u32 binding = startingBinding;
			for (auto& frameBufferPair : material->frameBuffers)
			{
				GLenum activeTexture = (GLenum)(GL_TEXTURE0 + (GLuint)binding);
				glActiveTexture(activeTexture);
				glBindTexture(GL_TEXTURE_2D, *((GLuint*)frameBufferPair.second));
				++binding;
			}

			return binding;
		}

		u32 GLRenderer::BindDeferredFrameBufferTextures(GLMaterial* glMaterial, u32 startingBinding)
		{
			struct Tex
			{
				bool needed;
				bool enabled;
				u32 textureID;
			};

			if (glMaterial->cubemapSamplerGBuffersIDs.empty())
			{
				PrintWarn("Attempted to bind GBuffer samplers on material doesn't contain any GBuffer samplers!\n");
				return startingBinding;
			}

			u32 binding = startingBinding;
			for (gl::GLCubemapGBuffer& cubemapGBuffer : glMaterial->cubemapSamplerGBuffersIDs)
			{
				GLenum activeTexture = (GLenum)(GL_TEXTURE0 + (GLuint)binding);
				glActiveTexture(activeTexture);
				glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapGBuffer.id);
				++binding;
			}

			return binding;
		}

		void GLRenderer::CreateOffscreenFrameBuffer(u32* FBO, u32* RBO, const glm::vec2i& size, TextureHandle& handle)
		{
			glGenFramebuffers(1, FBO);
			glBindFramebuffer(GL_FRAMEBUFFER, *FBO);

			glGenRenderbuffers(1, RBO);
			glBindRenderbuffer(GL_RENDERBUFFER, *RBO);
			glRenderbufferStorage(GL_RENDERBUFFER, m_OffscreenDepthBufferInternalFormat, size.x, size.y);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, *RBO);

			GenerateFrameBufferTexture(&handle.id,
									   0,
									   handle.internalFormat,
									   handle.format,
									   handle.type,
									   size);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			{
				PrintError("Offscreen frame buffer is incomplete!\n");
			}
		}

		void GLRenderer::RemoveMaterial(MaterialID materialID)
		{
			assert(materialID != InvalidMaterialID);

			m_Materials.erase(materialID);
		}

		bool GLRenderer::DoTextureSelector(const char* label,
										   const std::vector<GLTexture*>& textures,
										   i32* selectedIndex,
										   bool* bGenerateSampler)
		{
			bool bValueChanged = false;

			std::string currentTexName = (*selectedIndex == 0 ? "NONE" : textures[*selectedIndex - 1]->GetName().c_str());
			if (ImGui::BeginCombo(label, currentTexName.c_str()))
			{
				for (i32 i = 0; i < (i32)textures.size() + 1; i++)
				{
					bool bTextureSelected = (*selectedIndex == i);

					if (i == 0)
					{
						if (ImGui::Selectable("NONE", bTextureSelected))
						{
							*bGenerateSampler = false;

							*selectedIndex = i;
							bValueChanged = true;
						}
					}
					else
					{
						std::string textureName = textures[i - 1]->GetName();
						if (ImGui::Selectable(textureName.c_str(), bTextureSelected))
						{
							if (*selectedIndex == 0)
							{
								*bGenerateSampler = true;
							}

							*selectedIndex = i;
							bValueChanged = true;
						}

						if (ImGui::IsItemHovered())
						{
							DoTexturePreviewTooltip(textures[i - 1]);
						}
					}
					if (bTextureSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}

			return bValueChanged;
		}

		void GLRenderer::ImGuiUpdateTextureIndexOrMaterial(bool bUpdateTextureMaterial,
			const std::string& texturePath,
			std::string& matTexturePath,
			GLTexture* texture,
			i32 i,
			i32* textureIndex,
			u32* samplerID)
		{
			if (bUpdateTextureMaterial)
			{
				if (*textureIndex == 0)
				{
					matTexturePath = "";
					*samplerID = 0;
				}
				else if (i == *textureIndex - 1)
				{
					matTexturePath = texturePath;
					if (texture)
					{
						*samplerID = texture->handle;
					}
				}
			}
			else
			{
				if (matTexturePath.empty())
				{
					*textureIndex = 0;
				}
				else if (texturePath.compare(matTexturePath) == 0)
				{
					*textureIndex = i + 1;
				}
			}
		}

		void GLRenderer::DoTexturePreviewTooltip(GLTexture* texture)
		{
			ImGui::BeginTooltip();

			ImVec2 cursorPos = ImGui::GetCursorPos();

			real textureAspectRatio = (real)texture->width / (real)texture->height;
			real texSize = 128.0f;

			if (texture->channelCount == 4)
			{
				real tiling = 3.0f;
				ImVec2 uv0(0.0f, 0.0f);
				ImVec2 uv1(tiling * textureAspectRatio, tiling);
				GLTexture* alphaBGTexture = m_LoadedTextures[m_AlphaBGTextureID];
				ImGui::Image((void*)alphaBGTexture->handle, ImVec2(texSize * textureAspectRatio, texSize), uv0, uv1);
			}

			ImGui::SetCursorPos(cursorPos);

			ImGui::Image((void*)texture->handle, ImVec2(texSize * textureAspectRatio, texSize));

			ImGui::EndTooltip();
		}

		void GLRenderer::DrawLoadingTextureQuad()
		{
			SpriteQuadDrawInfo drawInfo = {};
			GLTexture* loadingTexture = m_LoadedTextures[m_LoadingTextureID];
			real textureAspectRatio = loadingTexture->width / (real)loadingTexture->height;
			drawInfo.scale = glm::vec3(textureAspectRatio, -1.0f, 1.0f);
			drawInfo.bScreenSpace = true;
			drawInfo.bReadDepth = false;
			drawInfo.bWriteDepth = false;
			drawInfo.materialID = m_SpriteMatID;
			drawInfo.anchor = AnchorPoint::WHOLE;
			drawInfo.textureHandleID = m_LoadedTextures[m_LoadingTextureID]->handle;
			drawInfo.spriteObjectRenderID = m_Quad3DRenderID;

			DrawSpriteQuad(drawInfo);
		}

		bool GLRenderer::GetLoadedTexture(const std::string& filePath, GLTexture** texture)
		{
			for (GLTexture* tex : m_LoadedTextures)
			{
				if (tex->GetRelativeFilePath().compare(filePath) == 0)
				{
					*texture = tex;
					return true;
				}
			}

			return false;
		}

		void GLRenderer::ReloadShaders()
		{
			UnloadShaders();
			LoadShaders();

			AddEditorString("Reloaded shaders");
		}

		void GLRenderer::UnloadShaders()
		{
			for (GLShader& shader : m_Shaders)
			{
				glDeleteProgram(shader.program);
			}
			m_Shaders.clear();
		}

		void GLRenderer::LoadShaders()
		{
			m_Shaders = {
				{ "deferred_combine", RESOURCE_LOCATION  "shaders/deferred_combine.vert", RESOURCE_LOCATION  "shaders/deferred_combine.frag" },
				{ "deferred_combine_cubemap", RESOURCE_LOCATION  "shaders/deferred_combine_cubemap.vert", RESOURCE_LOCATION  "shaders/deferred_combine_cubemap.frag" },
				{ "color", RESOURCE_LOCATION  "shaders/color.vert", RESOURCE_LOCATION  "shaders/color.frag" },
				{ "pbr", RESOURCE_LOCATION  "shaders/pbr.vert", RESOURCE_LOCATION  "shaders/pbr.frag" },
				{ "pbr_ws", RESOURCE_LOCATION  "shaders/pbr_ws.vert", RESOURCE_LOCATION  "shaders/pbr_ws.frag" },
				{ "skybox", RESOURCE_LOCATION  "shaders/skybox.vert", RESOURCE_LOCATION  "shaders/skybox.frag" },
				{ "equirectangular_to_cube", RESOURCE_LOCATION  "shaders/skybox.vert", RESOURCE_LOCATION  "shaders/equirectangular_to_cube.frag" },
				{ "irradiance", RESOURCE_LOCATION  "shaders/skybox.vert", RESOURCE_LOCATION  "shaders/irradiance.frag" },
				{ "prefilter", RESOURCE_LOCATION  "shaders/skybox.vert", RESOURCE_LOCATION  "shaders/prefilter.frag" },
				{ "brdf", RESOURCE_LOCATION  "shaders/brdf.vert", RESOURCE_LOCATION  "shaders/brdf.frag" },
				{ "sprite", RESOURCE_LOCATION  "shaders/sprite.vert", RESOURCE_LOCATION  "shaders/sprite.frag" },
				{ "post_process", RESOURCE_LOCATION  "shaders/post_process.vert", RESOURCE_LOCATION  "shaders/post_process.frag" },
				{ "post_fxaa", RESOURCE_LOCATION  "shaders/post_fxaa.vert", RESOURCE_LOCATION  "shaders/post_fxaa.frag" },
				{ "compute_sdf", RESOURCE_LOCATION  "shaders/compute_sdf.vert", RESOURCE_LOCATION  "shaders/compute_sdf.frag" },
				{ "font_ss", RESOURCE_LOCATION  "shaders/font_ss.vert", RESOURCE_LOCATION  "shaders/font_ss.frag",  RESOURCE_LOCATION  "shaders/font_ss.geom" },
				{ "font_ws", RESOURCE_LOCATION  "shaders/font_ws.vert", RESOURCE_LOCATION  "shaders/font_ws.frag",  RESOURCE_LOCATION  "shaders/font_ws.geom" },
				{ "shadow", RESOURCE_LOCATION  "shaders/shadow.vert", RESOURCE_LOCATION  "shaders/shadow.frag" },
			};

			ShaderID shaderID = 0;

			// TOOD: Determine this info automatically when parsing shader code

			// Deferred combine
			m_Shaders[shaderID].shader.bDeferred = false; // Sounds strange but this isn't deferred
			// m_Shaders[shaderID].shader.subpass = 0;
			m_Shaders[shaderID].shader.bDepthWriteEnable = false; // Disable depth writing
			m_Shaders[shaderID].shader.bNeedBRDFLUT = true;
			m_Shaders[shaderID].shader.bNeedShadowMap = true;
			m_Shaders[shaderID].shader.bNeedIrradianceSampler = true;
			m_Shaders[shaderID].shader.bNeedPrefilteredMap = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_LIGHT_VIEW_PROJ);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_CAM_POS);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_EXPOSURE);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_POINT_LIGHTS);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_DIR_LIGHT);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_IRRADIANCE_SAMPLER);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PREFILTER_MAP);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_BRDF_LUT);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_0_SAMPLER);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_1_SAMPLER);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_2_SAMPLER);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_IRRADIANCE_SAMPLER);
			++shaderID;

			// Deferred combine cubemap
			m_Shaders[shaderID].shader.bDeferred = false; // Sounds strange but this isn't deferred
			// m_Shaders[shaderID].shader.subpass = 0;
			m_Shaders[shaderID].shader.bDepthWriteEnable = false; // Disable depth writing
			m_Shaders[shaderID].shader.bNeedBRDFLUT = true;
			//m_Shaders[shaderID].shader.bNeedShadowMap = true;
			m_Shaders[shaderID].shader.bNeedIrradianceSampler = true;
			m_Shaders[shaderID].shader.bNeedPrefilteredMap = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION; // Used as 3D texture coord into cubemap

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_LIGHT_VIEW_PROJ);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_CAM_POS);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_EXPOSURE);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_POINT_LIGHTS);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_DIR_LIGHT);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_IRRADIANCE_SAMPLER);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PREFILTER_MAP);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_BRDF_LUT);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_0_SAMPLER);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_1_SAMPLER);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_FB_2_SAMPLER);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_IRRADIANCE_SAMPLER);
			++shaderID;

			// Color
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.bTranslucent = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::COLOR_R32G32B32A32_SFLOAT;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_COLOR_MULTIPLIER);
			++shaderID;

			// PBR
			m_Shaders[shaderID].shader.bDeferred = true;
			m_Shaders[shaderID].shader.bNeedAlbedoSampler = true;
			m_Shaders[shaderID].shader.bNeedMetallicSampler = true;
			m_Shaders[shaderID].shader.bNeedRoughnessSampler = true;
			m_Shaders[shaderID].shader.bNeedAOSampler = true;
			m_Shaders[shaderID].shader.bNeedNormalSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::COLOR_R32G32B32A32_SFLOAT |
				(u32)VertexAttribute::UV |
				(u32)VertexAttribute::TANGENT |
				(u32)VertexAttribute::BITANGENT |
				(u32)VertexAttribute::NORMAL;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_ALBEDO);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_ALBEDO_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_METALLIC);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_METALLIC_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_ROUGHNESS);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_ROUGHNESS_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_AO);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_AO_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_NORMAL_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			++shaderID;

			// PBR - WORLD SPACE
			m_Shaders[shaderID].shader.bDeferred = true;
			m_Shaders[shaderID].shader.bNeedAlbedoSampler = true;
			m_Shaders[shaderID].shader.bNeedNormalSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::COLOR_R32G32B32A32_SFLOAT |
				(u32)VertexAttribute::TANGENT |
				(u32)VertexAttribute::BITANGENT |
				(u32)VertexAttribute::NORMAL;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_ALBEDO);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_ALBEDO_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_METALLIC);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_ROUGHNESS);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONST_AO);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_NORMAL_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TEXTURE_SCALE);
			++shaderID;

			// Skybox
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.bNeedCubemapSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_EXPOSURE);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_TIME);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_ENABLE_CUBEMAP_SAMPLER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CUBEMAP_SAMPLER);
			++shaderID;

			// Equirectangular to Cube
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.bNeedHDREquirectangularSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_HDR_EQUIRECTANGULAR_SAMPLER);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			++shaderID;

			// Irradiance
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.bNeedCubemapSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_CUBEMAP_SAMPLER);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			++shaderID;

			// Prefilter
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.bNeedCubemapSampler = true;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION;

			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_CUBEMAP_SAMPLER);

			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			++shaderID;

			// BRDF
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			++shaderID;

			// Sprite
			m_Shaders[shaderID].shader.bDeferred = false;

			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_VIEW);
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_PROJECTION);

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_COLOR_MULTIPLIER);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_ALBEDO_SAMPLER);
			++shaderID;

			// Post processing
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION_2D |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_COLOR_MULTIPLIER);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CONTRAST_BRIGHTNESS_SATURATION);
			++shaderID;

			// Post FXAA (Fast approximate anti-aliasing)
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION_2D |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_LUMA_THRESHOLD_MIN);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_LUMA_THRESHOLD_MAX);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_MUL_REDUCE);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_MIN_REDUCE);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_MAX_SPAN);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_TEXEL_STEP);
			//m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_SHOW_EDGES);

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			++shaderID;

			// Compute SDF
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::UV;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			// TODO: Use same format as VulkanRenderer
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_CHAR_RESOLUTION);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_SPREAD);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_HIGH_RES_TEX);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TEX_CHANNEL);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_SDF_RESOLUTION);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_HIGH_RES);
			++shaderID;

			// Font SS
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION_2D |
				(u32)VertexAttribute::COLOR_R32G32B32A32_SFLOAT |
				(u32)VertexAttribute::UV |
				(u32)VertexAttribute::EXTRA_VEC4 |
				(u32)VertexAttribute::EXTRA_INT;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TRANSFORM_MAT);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TEX_SIZE);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_THRESHOLD);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_SHADOW);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_SOFTEN);
			++shaderID;

			// Font WS
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION |
				(u32)VertexAttribute::COLOR_R32G32B32A32_SFLOAT |
				(u32)VertexAttribute::UV |
				(u32)VertexAttribute::EXTRA_VEC4 |
				(u32)VertexAttribute::EXTRA_INT;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TRANSFORM_MAT);
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_TEX_SIZE);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_THRESHOLD);
			//m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_SHADOW);
			++shaderID;

			// Shadow
			m_Shaders[shaderID].shader.bDeferred = false;
			m_Shaders[shaderID].shader.vertexAttributes =
				(u32)VertexAttribute::POSITION;

			m_Shaders[shaderID].shader.constantBufferUniforms = {};
			m_Shaders[shaderID].shader.constantBufferUniforms.AddUniform(U_LIGHT_VIEW_PROJ);

			m_Shaders[shaderID].shader.dynamicBufferUniforms = {};
			m_Shaders[shaderID].shader.dynamicBufferUniforms.AddUniform(U_MODEL);
			++shaderID;

			for (GLShader& shader : m_Shaders)
			{
				shader.program = glCreateProgram();

				if (!LoadGLShaders(shader.program, shader))
				{
					std::string fileNames = shader.shader.vertexShaderFilePath + " & " + shader.shader.fragmentShaderFilePath;
					if (!shader.shader.geometryShaderFilePath.empty())
					{
						fileNames += " & " + shader.shader.geometryShaderFilePath;
					}
					PrintError("Couldn't load/compile shaders: %s\n", fileNames.c_str());
				}

				LinkProgram(shader.program);

#if 0
				PrintShaderInfo(shader.program, shader.shader.name.c_str());
#endif

#if DEBUG
				if (!IsProgramValid(shader.program))
				{
					PrintError("Shader program is invalid!\n");
				}
#endif

				// No need to keep the code in memory
				shader.shader.vertexShaderCode.clear();
				shader.shader.vertexShaderCode.shrink_to_fit();
				shader.shader.fragmentShaderCode.clear();
				shader.shader.fragmentShaderCode.shrink_to_fit();
				shader.shader.geometryShaderCode.clear();
				shader.shader.geometryShaderCode.shrink_to_fit();
			}
		}

		void GLRenderer::LoadFonts(bool bForceRender)
		{
			PROFILE_AUTO("Load fonts");

			m_FontsSS.clear();
			m_FontsWS.clear();

			for (auto& pair : m_Fonts)
			{
				FontMetaData& fontMetaData = pair.second;

				std::string fontName = fontMetaData.filePath;
				StripLeadingDirectories(fontName);
				StripFileType(fontName);

				LoadFont(&fontMetaData.bitmapFont,
					fontMetaData.size,
					fontMetaData.filePath,
					fontMetaData.renderedTextureFilePath,
					bForceRender,
					fontMetaData.bScreenSpace);
			}
		}

		void GLRenderer::ReloadSkybox(bool bRandomizeTexture)
		{
			if (bRandomizeTexture && !m_AvailableHDRIs.empty())
			{
				for (i32 i = 0; i < (i32)m_Materials.size(); ++i)
				{
					if (m_Materials[i].material.generateIrradianceSampler)
					{
						GenerateIrradianceSamplerMaps((MaterialID)i);
						GeneratePrefilteredMapFromCubemap((MaterialID)i);
					}
				}
			}
		}

		void GLRenderer::UpdateAllMaterialUniforms()
		{
			PROFILE_AUTO("Update material uniforms");

			glm::mat4 proj = g_CameraManager->CurrentCamera()->GetProjection();
			glm::mat4 view = g_CameraManager->CurrentCamera()->GetView();
			glm::mat4 viewProj = proj * view;
			glm::vec4 camPos = glm::vec4(g_CameraManager->CurrentCamera()->GetPosition(), 0.0f);
			real exposure = g_CameraManager->CurrentCamera()->exposure;

			glm::mat4 lightView, lightProj;
			ComputeDirLightViewProj(lightView, lightProj);

			glm::mat4 biasedLightViewProj = lightProj * lightView;

			for (auto& materialPair : m_Materials)
			{
				GLMaterial* material = &materialPair.second;

				if (material->material.shaderID == InvalidShaderID)
				{
					// TODO: Find out why this element still exists in the map
					continue;
				}

				GLShader* shader = &m_Shaders[material->material.shaderID];

				glUseProgram(shader->program);

				if (m_DirectionalLight != nullptr && shader->shader.bNeedShadowMap)
				{
					glUniform1i(material->uniformIDs.castShadows, m_DirectionalLight->bCastShadow);
					glUniform1f(material->uniformIDs.shadowDarkness, m_DirectionalLight->shadowDarkness);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_VIEW))
				{
					glUniformMatrix4fv(material->uniformIDs.view, 1, GL_FALSE, &view[0][0]);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_PROJECTION))
				{
					glUniformMatrix4fv(material->uniformIDs.projection, 1, GL_FALSE, &proj[0][0]);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_VIEW_PROJECTION))
				{
					glUniformMatrix4fv(material->uniformIDs.viewProjection, 1, GL_FALSE, &viewProj[0][0]);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_LIGHT_VIEW_PROJ))
				{
					glUniformMatrix4fv(material->uniformIDs.lightViewProjection, 1, GL_FALSE, &biasedLightViewProj[0][0]);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_CAM_POS))
				{
					glUniform4f(material->uniformIDs.camPos,
						camPos.x,
						camPos.y,
						camPos.z,
						camPos.w);
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_EXPOSURE))
				{
					glUniform1f(material->uniformIDs.exposure, exposure);
				}

				if (m_DirectionalLight != nullptr && shader->shader.constantBufferUniforms.HasUniform(U_DIR_LIGHT))
				{
					if (m_DirectionalLight->data.enabled == 0)
					{
						SetInt(material->material.shaderID, "dirLight.enabled", 0);
					}
					else
					{
						SetInt(material->material.shaderID, "dirLight.enabled", 1);
						SetVec3f(material->material.shaderID, "dirLight.direction", m_DirectionalLight->data.dir);
						SetVec3f(material->material.shaderID, "dirLight.color", m_DirectionalLight->data.color);
						SetFloat(material->material.shaderID, "dirLight.brightness", m_DirectionalLight->data.brightness);
					}
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_POINT_LIGHTS))
				{
					for (u32 i = 0; i < MAX_NUM_POINT_LIGHTS; ++i)
					{
						const std::string numberStr(std::to_string(i));
						const char* numberCStr = numberStr.c_str();
						static const i32 strStartLen = 16;
						static_assert(MAX_NUM_POINT_LIGHTS <= 99, "More than 99 point lights are allowed, strStartLen must be larger to compensate for more digits");
						char pointLightStrStart[strStartLen];
						// TODO: Replace with safer alternative
						strcpy_s(pointLightStrStart, "pointLights[");
						strcat_s(pointLightStrStart, numberCStr);
						strcat_s(pointLightStrStart, "]");
						strcat_s(pointLightStrStart, "\0");

						char enabledStr[strStartLen + 8];
						strcpy_s(enabledStr, pointLightStrStart);
						strcat_s(enabledStr, ".enabled");
						if (i < (u32)m_NumPointLightsEnabled)
						{
							if (m_PointLights[i].enabled == 0)
							{
								SetInt(material->material.shaderID, enabledStr, 0);
							}
							else
							{
								SetInt(material->material.shaderID, enabledStr, 1);

								char positionStr[strStartLen + 9];
								strcpy_s(positionStr, pointLightStrStart);
								strcat_s(positionStr, ".position");
								SetVec3f(material->material.shaderID, positionStr, m_PointLights[i].pos);

								char colorStr[strStartLen + 6];
								strcpy_s(colorStr, pointLightStrStart);
								strcat_s(colorStr, ".color");
								SetVec3f(material->material.shaderID, colorStr, m_PointLights[i].color);

								char brightnessStr[strStartLen + 11];
								strcpy_s(brightnessStr, pointLightStrStart);
								strcat_s(brightnessStr, ".brightness");
								SetFloat(material->material.shaderID, brightnessStr, m_PointLights[i].brightness);
							}
						}
						else
						{
							SetInt(material->material.shaderID, enabledStr, 0);
						}
					}
				}

				if (shader->shader.constantBufferUniforms.HasUniform(U_TIME))
				{
					glUniform1f(material->uniformIDs.time, g_SecElapsedSinceProgramStart);
				}

				static const char* texelStepStr = "texelStep";
				if (shader->shader.constantBufferUniforms.HasUniform(U_TEXEL_STEP))
				{
					glm::vec2i frameBufferSize = g_Window->GetFrameBufferSize();
					glm::vec2 texelStep(1.0f / frameBufferSize.x, 1.0f / frameBufferSize.y);
					SetVec2f(material->material.shaderID, texelStepStr, texelStep);
				}

				static const char* bDEBUGShowEdgesStr = "bDEBUGShowEdges";
				GLint location = glGetUniformLocation(shader->program, bDEBUGShowEdgesStr);
				if (location != -1)
				{
					SetInt(material->material.shaderID, bDEBUGShowEdgesStr, m_PostProcessSettings.bEnableFXAADEBUGShowEdges ? 1 : 0);
				}
			}
		}

		void GLRenderer::UpdatePerObjectUniforms(RenderID renderID, GLMaterial* materialOverride /* = nullptr */)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (!renderObject)
			{
				PrintError("Invalid renderID passed to UpdatePerObjectUniforms: %i\n", renderID);
				return;
			}

			const glm::mat4& model = renderObject->gameObject->GetTransform()->GetWorldTransform();
			if (materialOverride == nullptr)
			{
				materialOverride = &m_Materials[renderObject->materialID];
			}
			UpdatePerObjectUniforms(materialOverride, model);
		}

		void GLRenderer::UpdatePerObjectUniforms(GLMaterial* material, const glm::mat4& model)
		{
			// TODO: OPTIMIZATION: Investigate performance impact of caching each uniform and preventing updates to data that hasn't changed

			GLShader* shader = &m_Shaders[material->material.shaderID];

			//if (material->uniformIDs.textureScale != -1)
			//{
			//	glUniform1f(material->uniformIDs.textureScale, material->material.textureScale);
			//}

			// TODO: Use set functions here (SetFloat, SetMatrix, ...)
			if (shader->shader.dynamicBufferUniforms.HasUniform(U_MODEL))
			{
				glUniformMatrix4fv(material->uniformIDs.model, 1, GL_FALSE, &model[0][0]);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_MODEL_INV_TRANSPOSE))
			{
				glm::mat4 modelInv = glm::inverse(model);
				// OpenGL will transpose for us if we set the third param to true
				glUniformMatrix4fv(material->uniformIDs.modelInvTranspose, 1, GL_TRUE, &modelInv[0][0]);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_COLOR_MULTIPLIER))
			{
				glm::vec4 colorMultiplier = material->material.colorMultiplier;
				glUniform4fv(material->uniformIDs.colorMultiplier, 1, &colorMultiplier[0]);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_NORMAL_SAMPLER))
			{
				glUniform1i(material->uniformIDs.enableNormalTexture, material->material.enableNormalSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_ENABLE_CUBEMAP_SAMPLER))
			{
				glUniform1i(material->uniformIDs.enableCubemapTexture, material->material.enableCubemapSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_ALBEDO_SAMPLER))
			{
				glUniform1ui(material->uniformIDs.enableAlbedoSampler, material->material.enableAlbedoSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_CONST_ALBEDO))
			{
				glUniform4f(material->uniformIDs.constAlbedo, material->material.constAlbedo.x, material->material.constAlbedo.y, material->material.constAlbedo.z, 0);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_METALLIC_SAMPLER))
			{
				glUniform1ui(material->uniformIDs.enableMetallicSampler, material->material.enableMetallicSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_CONST_METALLIC))
			{
				glUniform1f(material->uniformIDs.constMetallic, material->material.constMetallic);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_ROUGHNESS_SAMPLER))
			{
				glUniform1ui(material->uniformIDs.enableRoughnessSampler, material->material.enableRoughnessSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_CONST_ROUGHNESS))
			{
				glUniform1f(material->uniformIDs.constRoughness, material->material.constRoughness);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_AO_SAMPLER))
			{
				glUniform1ui(material->uniformIDs.enableAOSampler, material->material.enableAOSampler);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_CONST_AO))
			{
				glUniform1f(material->uniformIDs.constAO, material->material.constAO);
			}

			if (shader->shader.dynamicBufferUniforms.HasUniform(U_IRRADIANCE_SAMPLER))
			{
				glUniform1i(material->uniformIDs.enableIrradianceSampler, material->material.enableIrradianceSampler);
			}
		}

		void GLRenderer::OnWindowSizeChanged(i32 width, i32 height)
		{
			if (width == 0 || height == 0 || m_gBufferHandle == 0)
			{
				return;
			}

			const glm::vec2i newFrameBufferSize(width, height);

			glBindFramebuffer(GL_FRAMEBUFFER, m_Offscreen0FBO);
			ResizeFrameBufferTexture(m_OffscreenTexture0Handle.id,
									 m_OffscreenTexture0Handle.internalFormat,
									 m_OffscreenTexture0Handle.format,
									 m_OffscreenTexture0Handle.type,
									 newFrameBufferSize);

			ResizeRenderBuffer(m_Offscreen0RBO, newFrameBufferSize, m_OffscreenDepthBufferInternalFormat);


			glBindFramebuffer(GL_FRAMEBUFFER, m_Offscreen1FBO);
			ResizeFrameBufferTexture(m_OffscreenTexture1Handle.id,
									 m_OffscreenTexture1Handle.internalFormat,
									 m_OffscreenTexture1Handle.format,
									 m_OffscreenTexture1Handle.type,
									 newFrameBufferSize);

			ResizeRenderBuffer(m_Offscreen1RBO, newFrameBufferSize, m_OffscreenDepthBufferInternalFormat);


			glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferHandle);
			ResizeFrameBufferTexture(m_gBuffer_PositionMetallicHandle.id,
				m_gBuffer_PositionMetallicHandle.internalFormat,
				m_gBuffer_PositionMetallicHandle.format,
				m_gBuffer_PositionMetallicHandle.type,
				newFrameBufferSize);

			ResizeFrameBufferTexture(m_gBuffer_NormalRoughnessHandle.id,
				m_gBuffer_NormalRoughnessHandle.internalFormat,
				m_gBuffer_NormalRoughnessHandle.format,
				m_gBuffer_NormalRoughnessHandle.type,
				newFrameBufferSize);

			ResizeFrameBufferTexture(m_gBuffer_AlbedoAOHandle.id,
				m_gBuffer_AlbedoAOHandle.internalFormat,
				m_gBuffer_AlbedoAOHandle.format,
				m_gBuffer_AlbedoAOHandle.type,
				newFrameBufferSize);

			ResizeRenderBuffer(m_gBufferDepthHandle, newFrameBufferSize, m_OffscreenDepthBufferInternalFormat);
		}

		void GLRenderer::OnPreSceneChange()
		{
			// G-Buffer needs to be regenerated using new scene's reflection probe mat ID
			GenerateGBuffer();
			if (m_DirectionalLight != nullptr)
			{
				m_DirectionalLight->shadowTextureID = m_ShadowMapTexture.id;
			}
		}

		void GLRenderer::OnPostSceneChange()
		{

		}

		bool GLRenderer::GetRenderObjectCreateInfo(RenderID renderID, RenderObjectCreateInfo& outInfo)
		{
			outInfo = {};

			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (!renderObject)
			{
				return false;
			}

			outInfo.materialID = renderObject->materialID;
			outInfo.vertexBufferData = renderObject->vertexBufferData;
			outInfo.indices = renderObject->indices;
			outInfo.gameObject = renderObject->gameObject;
			outInfo.visible = renderObject->gameObject->IsVisible();
			outInfo.visibleInSceneExplorer = renderObject->gameObject->IsVisibleInSceneExplorer();
			outInfo.cullFace = GLCullFaceToCullFace(renderObject->cullFace);
			outInfo.depthTestReadFunc = GlenumToDepthTestFunc(renderObject->depthTestReadFunc);
			outInfo.bDepthWriteEnable = (renderObject->bDepthWriteEnable == GL_TRUE);
			outInfo.bEditorObject = renderObject->bEditorObject;

			return true;
		}

		void GLRenderer::SetVSyncEnabled(bool bEnableVSync)
		{
			glfwSwapInterval(bEnableVSync ? 1 : 0);
		}

		void GLRenderer::SetFloat(ShaderID shaderID, const char* valName, real val)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, valName);
			if (location == -1)
			{
				PrintWarn("Float %s couldn't be found!\n", valName);
			}

			glUniform1f(location, val);
		}

		void GLRenderer::SetInt(ShaderID shaderID, const char* valName, i32 val)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, valName);
			if (location == -1)
			{
				PrintWarn("i32 %s couldn't be found!\n", valName);
			}

			glUniform1i(location, val);
		}

		void GLRenderer::SetUInt(ShaderID shaderID, const char* valName, u32 val)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, valName);
			if (location == -1)
			{
				PrintWarn("u32 %s couldn't be found!\n", valName);
			}

			glUniform1ui(location, val);
		}

		void GLRenderer::SetVec2f(ShaderID shaderID, const char* vecName, const glm::vec2& vec)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, vecName);
			if (location == -1)
			{
				PrintWarn("Vec2f %s couldn't be found!\n", vecName);
			}

			glUniform2f(location, vec[0], vec[1]);
		}

		void GLRenderer::SetVec3f(ShaderID shaderID, const char* vecName, const glm::vec3& vec)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, vecName);
			if (location == -1)
			{
				PrintWarn("Vec3f %s couldn't be found!\n", vecName);
			}

			glUniform3f(location, vec[0], vec[1], vec[2]);
		}

		void GLRenderer::SetVec4f(ShaderID shaderID, const char* vecName, const glm::vec4& vec)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, vecName);
			if (location == -1)
			{
				PrintWarn("Vec4f %s couldn't be found!\n", vecName);
			}

			glUniform4f(location, vec[0], vec[1], vec[2], vec[3]);
		}

		void GLRenderer::SetMat4f(ShaderID shaderID, const char* matName, const glm::mat4& mat)
		{
			GLint location = glGetUniformLocation(m_Shaders[shaderID].program, matName);
			if (location == -1)
			{
				PrintWarn("Mat4f %s couldn't be found!\n", matName);
			}

			glUniformMatrix4fv(location, 1, GL_FALSE, &mat[0][0]);
		}


		void GLRenderer::GenerateGBufferVertexBuffer()
		{
			if (m_gBufferQuadVertexBufferData.vertexData == nullptr)
			{
				VertexBufferData::CreateInfo gBufferQuadVertexBufferDataCreateInfo = {};

				gBufferQuadVertexBufferDataCreateInfo.positions_3D = {
					glm::vec3(-1.0f,  -1.0f, 0.0f),
					glm::vec3(-1.0f, 3.0f, 0.0f),
					glm::vec3(3.0f,  -1.0f, 0.0f),
				};

				gBufferQuadVertexBufferDataCreateInfo.texCoords_UV = {
					glm::vec2(0.0f, 0.0f),
					glm::vec2(0.0f, 2.0f),
					glm::vec2(2.0f, 0.0f),
				};

				gBufferQuadVertexBufferDataCreateInfo.attributes = (u32)VertexAttribute::POSITION | (u32)VertexAttribute::UV;

				m_gBufferQuadVertexBufferData.Initialize(&gBufferQuadVertexBufferDataCreateInfo);
			}
		}

		void GLRenderer::GenerateGBuffer()
		{
			if (m_gBufferQuadVertexBufferData.vertexData == nullptr)
			{
				GenerateGBufferVertexBuffer();
			}

			// TODO: Allow user to not set this and have a backup plan (disable deferred rendering?)
			assert(m_ReflectionProbeMaterialID != InvalidMaterialID);

			std::string gBufferMatName = "GBuffer material";
			std::string gBufferName = "GBuffer quad";
			// Remove existing material if present (this will be true when reloading the scene)
			{
				MaterialID existingGBufferMatID = InvalidMaterialID;
				// TODO: Don't rely on material names!
				if (GetMaterialID(gBufferMatName, existingGBufferMatID))
				{
					RemoveMaterial(existingGBufferMatID);
				}

				for (auto iter = m_PersistentObjects.begin(); iter != m_PersistentObjects.end(); ++iter)
				{
					GameObject* gameObject = *iter;
					if (gameObject->GetName().compare(gBufferName) == 0)
					{
						delete gameObject;
						m_PersistentObjects.erase(iter);
						break;
					}
				}

				if (m_GBufferQuadRenderID != InvalidID)
				{
					DestroyRenderObject(m_GBufferQuadRenderID);
				}
			}

			MaterialCreateInfo gBufferMaterialCreateInfo = {};
			gBufferMaterialCreateInfo.name = gBufferMatName;
			gBufferMaterialCreateInfo.shaderName = "deferred_combine";
			gBufferMaterialCreateInfo.enableIrradianceSampler = true;
			gBufferMaterialCreateInfo.irradianceSamplerMatID = m_ReflectionProbeMaterialID;
			gBufferMaterialCreateInfo.enablePrefilteredMap = true;
			gBufferMaterialCreateInfo.prefilterMapSamplerMatID = m_ReflectionProbeMaterialID;
			gBufferMaterialCreateInfo.enableBRDFLUT = true;
			gBufferMaterialCreateInfo.engineMaterial = true;
			gBufferMaterialCreateInfo.frameBuffers = {
				{ "positionMetallicFrameBufferSampler",  &m_gBuffer_PositionMetallicHandle.id },
				{ "normalRoughnessFrameBufferSampler",  &m_gBuffer_NormalRoughnessHandle.id },
				{ "albedoAOFrameBufferSampler",  &m_gBuffer_AlbedoAOHandle.id },
			};

			MaterialID gBufferMatID = InitializeMaterial(&gBufferMaterialCreateInfo);


			GameObject* gBufferQuadGameObject = new GameObject(gBufferName, GameObjectType::_NONE);
			m_PersistentObjects.push_back(gBufferQuadGameObject);
			// Don't render the g buffer normally, we'll handle it separately
			gBufferQuadGameObject->SetVisible(false);

			RenderObjectCreateInfo gBufferQuadCreateInfo = {};
			gBufferQuadCreateInfo.materialID = gBufferMatID;
			gBufferQuadCreateInfo.gameObject = gBufferQuadGameObject;
			gBufferQuadCreateInfo.vertexBufferData = &m_gBufferQuadVertexBufferData;
			gBufferQuadCreateInfo.depthTestReadFunc = DepthTestFunc::ALWAYS; // Ignore previous depth values
			gBufferQuadCreateInfo.bDepthWriteEnable = false; // Don't write GBuffer quad to depth buffer
			gBufferQuadCreateInfo.visibleInSceneExplorer = false;

			m_GBufferQuadRenderID = InitializeRenderObject(&gBufferQuadCreateInfo);

			m_gBufferQuadVertexBufferData.DescribeShaderVariables(this, m_GBufferQuadRenderID);
		}

		u32 GLRenderer::GetRenderObjectCount() const
		{
			u32 count = 0;

			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject)
				{
					++count;
				}
			}

			return count;
		}

		u32 GLRenderer::GetRenderObjectCapacity() const
		{
			return m_RenderObjects.capacity();
		}

		u32 GLRenderer::GetActiveRenderObjectCount() const
		{
			u32 capacity = 0;
			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject)
				{
					++capacity;
				}
			}
			return capacity;
		}

		void GLRenderer::DescribeShaderVariable(RenderID renderID, const std::string& variableName, i32 size, DataType dataType, bool normalized, i32 stride, void* pointer)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (!renderObject)
			{
				PrintError("Invalid renderID passed to DescribeShaderVariable: %i\n", renderID);
				return;
			}

			GLMaterial* material = &m_Materials[renderObject->materialID];
			u32 program = m_Shaders[material->material.shaderID].program;


			glUseProgram(program);

			glBindVertexArray(renderObject->VAO);

			GLint location = glGetAttribLocation(program, variableName.c_str());
			if (location == -1)
			{
				//PrintWarn("Invalid shader variable name: " + variableName);
				glBindVertexArray(0);
				return;
			}
			glEnableVertexAttribArray((GLuint)location);

			GLenum glRenderType = DataTypeToGLType(dataType);
			glVertexAttribPointer((GLuint)location, size, glRenderType, (GLboolean)normalized, stride, pointer);
		}

		void GLRenderer::SetSkyboxMesh(GameObject* skyboxMesh)
		{
			m_SkyBoxMesh = skyboxMesh;

			if (skyboxMesh == nullptr)
			{
				return;
			}

			MaterialID skyboxMaterialID = m_SkyBoxMesh->GetMeshComponent()->GetMaterialID();
			if (skyboxMaterialID == InvalidMaterialID)
			{
				PrintError("Skybox doesn't have a valid material! Irradiance textures can't be generated\n");
				return;
			}

			for (GLRenderObject* renderObject : m_RenderObjects)
			{
				if (renderObject)
				{
					GLMaterial& material = m_Materials[renderObject->materialID];
					GLShader& shader = m_Shaders[material.material.shaderID];
					if (shader.shader.bNeedPrefilteredMap)
					{
						material.irradianceSamplerID = m_Materials[skyboxMaterialID].irradianceSamplerID;
						material.prefilteredMapSamplerID = m_Materials[skyboxMaterialID].prefilteredMapSamplerID;
					}
				}
			}

			m_bRebatchRenderObjects = true;
		}

		GameObject* GLRenderer::GetSkyboxMesh()
		{
			return m_SkyBoxMesh;
		}

		void GLRenderer::SetRenderObjectMaterialID(RenderID renderID, MaterialID materialID)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			if (renderObject)
			{
				renderObject->materialID = materialID;
				m_bRebatchRenderObjects = true;
			}
			else
			{
				PrintError("SetRenderObjectMaterialID couldn't find render object with ID %i\n", renderID);
			}
		}

		Material& GLRenderer::GetMaterial(MaterialID matID)
		{
			assert(matID != InvalidMaterialID);

			return m_Materials[matID].material;
		}

		Shader& GLRenderer::GetShader(ShaderID shaderID)
		{
			assert(shaderID != InvalidShaderID);

			return m_Shaders[shaderID].shader;
		}

		void GLRenderer::DestroyRenderObject(RenderID renderID)
		{
			GLRenderObject* renderObject = GetRenderObject(renderID);
			DestroyRenderObject(renderID, renderObject);
		}

		void GLRenderer::DestroyRenderObject(RenderID renderID, GLRenderObject* renderObject)
		{
			if (renderObject)
			{
				if (renderObject->VAO != InvalidID)
				{
					glDeleteVertexArrays(1, &renderObject->VAO);
				}
				if (renderObject->VBO != InvalidID)
				{
					glDeleteBuffers(1, &renderObject->VBO);
				}
				if (renderObject->IBO != InvalidID)
				{
					glDeleteBuffers(1, &renderObject->IBO);
				}

				renderObject->gameObject = nullptr;
				delete renderObject;
			}

			m_RenderObjects[renderID] = nullptr;
			m_bRebatchRenderObjects = true;
		}

		void GLRenderer::NewFrame()
		{
			if (m_PhysicsDebugDrawer)
			{
				m_PhysicsDebugDrawer->ClearLines();
			}

			if (g_EngineInstance->IsRenderingImGui())
			{
				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();
			}
		}

		PhysicsDebugDrawBase* GLRenderer::GetDebugDrawer()
		{
			return m_PhysicsDebugDrawer;
		}

		void GLRenderer::PhysicsDebugRender()
		{
			PROFILE_AUTO("PhysicsDebugRender");

			GL_PUSH_DEBUG_GROUP("Physics Debug");

			btDiscreteDynamicsWorld* physicsWorld = g_SceneManager->CurrentScene()->GetPhysicsWorld()->GetWorld();
			physicsWorld->debugDrawWorld();

			GL_POP_DEBUG_GROUP();
		}

		void GLRenderer::DrawAssetBrowserImGui(bool* bShowing)
		{
			ImGui::SetNextWindowSize(ImVec2(400.0f, 350.0f), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Asset browser", bShowing))
			{
				if (ImGui::CollapsingHeader("Materials"))
				{
					static bool bUpdateFields = true;
					const i32 MAX_NAME_LEN = 128;
					static i32 selectedMaterialIndexShort = 0; // Index into shortened array
					static MaterialID selectedMaterialID = 0;
					while (m_Materials[selectedMaterialID].material.engineMaterial &&
						   selectedMaterialID < m_Materials.size() - 1)
					{
						++selectedMaterialID;
					}
					static std::string matName = "";
					static i32 selectedShaderIndex = 0;
					// Texture index values of 0 represent no texture, 1 = first index into textures array and so on
					static i32 albedoTextureIndex = 0;
					static bool bUpdateAlbedoTextureMaterial = false;
					static i32 metallicTextureIndex = 0;
					static bool bUpdateMetallicTextureMaterial = false;
					static i32 roughnessTextureIndex = 0;
					static bool bUpdateRoughessTextureMaterial = false;
					static i32 normalTextureIndex = 0;
					static bool bUpdateNormalTextureMaterial = false;
					static i32 aoTextureIndex = 0;
					static bool bUpdateAOTextureMaterial = false;
					GLMaterial& mat = m_Materials[selectedMaterialID];

					if (bUpdateFields)
					{
						bUpdateFields = false;

						matName = mat.material.name;
						matName.resize(MAX_NAME_LEN);

						i32 i = 0;
						for (GLTexture* texture : m_LoadedTextures)
						{
							std::string texturePath = texture->GetRelativeFilePath();

							ImGuiUpdateTextureIndexOrMaterial(bUpdateAlbedoTextureMaterial,
														 texturePath,
														 mat.material.albedoTexturePath,
														 texture,
														 i,
														 &albedoTextureIndex,
														 &mat.albedoSamplerID);

							ImGuiUpdateTextureIndexOrMaterial(bUpdateMetallicTextureMaterial,
														 texturePath,
														 mat.material.metallicTexturePath,
														 texture,
														 i,
														 &metallicTextureIndex,
														 &mat.metallicSamplerID);

							ImGuiUpdateTextureIndexOrMaterial(bUpdateRoughessTextureMaterial,
														 texturePath,
														 mat.material.roughnessTexturePath,
														 texture,
														 i,
														 &roughnessTextureIndex,
														 &mat.roughnessSamplerID);

							ImGuiUpdateTextureIndexOrMaterial(bUpdateNormalTextureMaterial,
														 texturePath,
														 mat.material.normalTexturePath,
														 texture,
														 i,
														 &normalTextureIndex,
														 &mat.normalSamplerID);

							ImGuiUpdateTextureIndexOrMaterial(bUpdateAOTextureMaterial,
														 texturePath,
														 mat.material.aoTexturePath,
														 texture,
														 i,
														 &aoTextureIndex,
														 &mat.aoSamplerID);

							++i;
						}

						mat.material.enableAlbedoSampler = (albedoTextureIndex > 0);
						mat.material.enableMetallicSampler = (metallicTextureIndex > 0);
						mat.material.enableRoughnessSampler = (roughnessTextureIndex > 0);
						mat.material.enableNormalSampler = (normalTextureIndex > 0);
						mat.material.enableAOSampler = (aoTextureIndex > 0);

						selectedShaderIndex = mat.material.shaderID;
					}

					ImGui::PushItemWidth(160.0f);
					if (ImGui::InputText("Name", (char*)matName.data(), MAX_NAME_LEN, ImGuiInputTextFlags_EnterReturnsTrue))
					{
						// Remove trailing \0 characters
						matName = std::string(matName.c_str());
						mat.material.name = matName;
					}
					ImGui::PopItemWidth();

					ImGui::SameLine();

					struct ShaderFunctor
					{
						static bool GetShaderName(void* data, int idx, const char** out_str)
						{
							*out_str = ((GLShader*)data)[idx].shader.name.c_str();
							return true;
						}
					};
					ImGui::PushItemWidth(240.0f);
					if (ImGui::Combo("Shader", &selectedShaderIndex, &ShaderFunctor::GetShaderName,
						(void*)m_Shaders.data(), m_Shaders.size()))
					{
						mat = m_Materials[selectedMaterialID];
						mat.material.shaderID = selectedShaderIndex;

						bUpdateFields = true;
					}
					ImGui::PopItemWidth();

					ImGui::NewLine();

					ImGui::Columns(2);
					ImGui::SetColumnWidth(0, 240.0f);

					ImGui::ColorEdit3("Albedo", &mat.material.constAlbedo.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueWheel);

					if (mat.material.enableMetallicSampler)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					}
					ImGui::SliderFloat("Metallic", &mat.material.constMetallic, 0.0f, 1.0f, "%.2f");
					if (mat.material.enableMetallicSampler)
					{
						ImGui::PopStyleColor();
					}

					if (mat.material.enableRoughnessSampler)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					}
					ImGui::SliderFloat("Roughness", &mat.material.constRoughness, 0.0f, 1.0f, "%.2f");
					if (mat.material.enableRoughnessSampler)
					{
						ImGui::PopStyleColor();
					}

					if (mat.material.enableAOSampler)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					}
					ImGui::SliderFloat("AO", &mat.material.constAO, 0.0f, 1.0f, "%.2f");
					if (mat.material.enableAOSampler)
					{
						ImGui::PopStyleColor();
					}

					ImGui::DragFloat("Texture scale", &mat.material.textureScale, 0.1f);

					ImGui::NextColumn();

					struct TextureFunctor
					{
						static bool GetTextureFileName(void* data, i32 idx, const char** out_str)
						{
							if (idx == 0)
							{
								*out_str = "NONE";
							}
							else
							{
								*out_str = ((GLTexture**)data)[idx - 1]->GetName().c_str();
							}
							return true;
						}
					};

					std::vector<GLTexture*> textures;
					textures.reserve(m_LoadedTextures.size());
					for (GLTexture* texture : m_LoadedTextures)
					{
						textures.push_back(texture);
					}

					bUpdateAlbedoTextureMaterial = DoTextureSelector("Albedo texture", textures,
						&albedoTextureIndex, &mat.material.generateAlbedoSampler);
					bUpdateFields |= bUpdateAlbedoTextureMaterial;
					bUpdateMetallicTextureMaterial = DoTextureSelector("Metallic texture", textures,
						&metallicTextureIndex, &mat.material.generateMetallicSampler);
					bUpdateFields |= bUpdateMetallicTextureMaterial;
					bUpdateRoughessTextureMaterial = DoTextureSelector("Roughness texture", textures,
						&roughnessTextureIndex, &mat.material.generateRoughnessSampler);
					bUpdateFields |= bUpdateRoughessTextureMaterial;
					bUpdateNormalTextureMaterial = DoTextureSelector("Normal texture", textures,
						&normalTextureIndex, &mat.material.generateNormalSampler);
					bUpdateFields |= bUpdateNormalTextureMaterial;
					bUpdateAOTextureMaterial = DoTextureSelector("AO texture", textures, &aoTextureIndex,
						&mat.material.generateAOSampler);
					bUpdateFields |= bUpdateAOTextureMaterial;

					ImGui::NewLine();

					ImGui::EndColumns();

					if (ImGui::BeginChild("material list", ImVec2(0.0f, 120.0f), true))
					{
						i32 matShortIndex = 0;
						for (i32 i = 0; i < (i32)m_Materials.size(); ++i)
						{
							if (m_Materials[i].material.engineMaterial)
							{
								continue;
							}

							bool bSelected = (matShortIndex == selectedMaterialIndexShort);
							if (ImGui::Selectable(m_Materials[i].material.name.c_str(), &bSelected))
							{
								if (selectedMaterialIndexShort != matShortIndex)
								{
									selectedMaterialIndexShort = matShortIndex;
									selectedMaterialID = i;
									bUpdateFields = true;
								}
							}

							if (ImGui::BeginPopupContextItem())
							{
								if (ImGui::Button("Duplicate"))
								{
									const Material& dupMat = m_Materials[i].material;

									MaterialCreateInfo createInfo = {};
									createInfo.name = GetIncrementedPostFixedStr(dupMat.name, "new material 00");
									createInfo.shaderName = m_Shaders[dupMat.shaderID].shader.name;
									createInfo.constAlbedo = dupMat.constAlbedo;
									createInfo.constRoughness = dupMat.constRoughness;
									createInfo.constMetallic = dupMat.constMetallic;
									createInfo.constAO = dupMat.constAO;
									createInfo.colorMultiplier = dupMat.colorMultiplier;
									// TODO: Copy other fields
									MaterialID newMaterialID = InitializeMaterial(&createInfo);

									g_SceneManager->CurrentScene()->AddMaterialID(newMaterialID);

									ImGui::CloseCurrentPopup();
								}

								ImGui::EndPopup();
							}

							if (ImGui::IsItemActive())
							{
								if (ImGui::BeginDragDropSource())
								{
									MaterialID draggedMaterialID = i;
									const void* data = (void*)(&draggedMaterialID);
									u32 size = sizeof(MaterialID);

									ImGui::SetDragDropPayload(m_MaterialPayloadCStr, data, size);

									ImGui::Text("%s", m_Materials[i].material.name.c_str());

									ImGui::EndDragDropSource();
								}
							}

							++matShortIndex;
						}
					}
					ImGui::EndChild(); // Material list

					const i32 MAX_MAT_NAME_LEN = 128;
					static std::string newMaterialName = "";

					const char* createMaterialPopupStr = "Create material##popup";
					if (ImGui::Button("Create material"))
					{
						ImGui::OpenPopup(createMaterialPopupStr);
						newMaterialName = "New Material 01";
						newMaterialName.resize(MAX_MAT_NAME_LEN);
					}

					if (ImGui::BeginPopupModal(createMaterialPopupStr, NULL, ImGuiWindowFlags_NoResize))
					{
						ImGui::Text("Name:");
						ImGui::InputText("##NameText", (char*)newMaterialName.data(), MAX_MAT_NAME_LEN);

						ImGui::Text("Shader:");
						static i32 newMatShaderIndex = 0;
						if (ImGui::BeginChild("Shader", ImVec2(0, 120), true))
						{
							i32 i = 0;
							for (GLShader& shader : m_Shaders)
							{
								bool bSelectedShader = (i == newMatShaderIndex);
								if (ImGui::Selectable(shader.shader.name.c_str(), &bSelectedShader))
								{
									newMatShaderIndex = i;
								}

								++i;
							}
						}
						ImGui::EndChild(); // Shader list

						if (ImGui::Button("Create new material"))
						{
							// Remove trailing /0 characters
							newMaterialName = std::string(newMaterialName.c_str());

							MaterialCreateInfo createInfo = {};
							createInfo.name = newMaterialName;
							createInfo.shaderName = m_Shaders[newMatShaderIndex].shader.name;
							MaterialID newMaterialID = InitializeMaterial(&createInfo);

							g_SceneManager->CurrentScene()->AddMaterialID(newMaterialID);

							ImGui::CloseCurrentPopup();
						}

						ImGui::SameLine();

						if (ImGui::Button("Cancel"))
						{
							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}

					ImGui::SameLine();

					ImGui::PushStyleColor(ImGuiCol_Button, g_WarningButtonColor);
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_WarningButtonHoveredColor);
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_WarningButtonActiveColor);
					if (ImGui::Button("Delete material"))
					{
						g_SceneManager->CurrentScene()->RemoveMaterialID(selectedMaterialID);
						RemoveMaterial(selectedMaterialID);
					}
					ImGui::PopStyleColor();
					ImGui::PopStyleColor();
					ImGui::PopStyleColor();
				}

				if (ImGui::CollapsingHeader("Textures"))
				{
					static i32 selectedTextureIndex = 0;
					if (ImGui::BeginChild("texture list", ImVec2(0.0f, 120.0f), true))
					{
						i32 i = 0;
						for (GLTexture* texture : m_LoadedTextures)
						{
							bool bSelected = (i == selectedTextureIndex);
							std::string textureFileName = texture->GetName();
							if (ImGui::Selectable(textureFileName.c_str(), &bSelected))
							{
								selectedTextureIndex = i;
							}

							if (ImGui::BeginPopupContextItem())
							{
								if (ImGui::Button("Reload"))
								{
									texture->Reload();
									ImGui::CloseCurrentPopup();
								}

								ImGui::EndPopup();
							}

							if (ImGui::IsItemHovered())
							{
								DoTexturePreviewTooltip(texture);
							}
							++i;
						}
					}
					ImGui::EndChild();

					if (ImGui::Button("Import Texture"))
					{
						// TODO: Not all textures are directly in this directory! CLEANUP to make more robust
						std::string relativeDirPath = RESOURCE_LOCATION  "textures/";
						std::string absoluteDirectoryStr = RelativePathToAbsolute(relativeDirPath);
						std::string selectedAbsFilePath;
						if (OpenFileDialog("Import texture", absoluteDirectoryStr, selectedAbsFilePath))
						{
							std::string fileNameAndExtension = selectedAbsFilePath;
							StripLeadingDirectories(fileNameAndExtension);
							std::string relativeFilePath = relativeDirPath + fileNameAndExtension;

							Print("Importing texture: %s\n", relativeFilePath.c_str());

							GLTexture* newTexture = new GLTexture(relativeFilePath, 3, false, false, false);
							if (newTexture->LoadFromFile())
							{
								m_LoadedTextures.push_back(newTexture);
							}

							ImGui::CloseCurrentPopup();
						}
					}
				}

				if (ImGui::CollapsingHeader("Meshes"))
				{
					static i32 selectedMeshIndex = 0;
					//const i32 MAX_NAME_LEN = 128;
					// TODO: Implement mesh naming
					//static std::string selectedMeshName = "";
					static bool bUpdateName = true;

					std::string selectedMeshRelativeFilePath;
					LoadedMesh* selectedMesh = nullptr;
					i32 meshIdx = 0;
					for (auto meshPair : MeshComponent::m_LoadedMeshes)
					{
						if (meshIdx == selectedMeshIndex)
						{
							selectedMesh = meshPair.second;
							selectedMeshRelativeFilePath = meshPair.first;
							break;
						}
						++meshIdx;
					}

					ImGui::Text("Import settings");

					ImGui::Columns(2, "import settings columns", false);
					ImGui::Separator();
					ImGui::Checkbox("Flip U", &selectedMesh->importSettings.bFlipU); ImGui::NextColumn();
					ImGui::Checkbox("Flip V", &selectedMesh->importSettings.bFlipV); ImGui::NextColumn();
					ImGui::Checkbox("Swap Normal YZ", &selectedMesh->importSettings.bSwapNormalYZ); ImGui::NextColumn();
					ImGui::Checkbox("Flip Normal Z", &selectedMesh->importSettings.bFlipNormalZ); ImGui::NextColumn();
					ImGui::Columns(1);

					if (ImGui::Button("Re-import"))
					{
						for (GLRenderObject* renderObject : m_RenderObjects)
						{
							if (renderObject && renderObject->gameObject)
							{
								MeshComponent* gameObjectMesh = renderObject->gameObject->GetMeshComponent();
								if (gameObjectMesh &&  gameObjectMesh->GetRelativeFilePath().compare(selectedMeshRelativeFilePath) == 0)
								{
									MeshImportSettings importSettings = selectedMesh->importSettings;

									MaterialID matID = renderObject->materialID;
									GameObject* gameObject = renderObject->gameObject;

									DestroyRenderObject(gameObject->GetRenderID());
									gameObject->SetRenderID(InvalidRenderID);

									gameObjectMesh->Destroy();
									gameObjectMesh->SetOwner(gameObject);
									gameObjectMesh->SetRequiredAttributesFromMaterialID(matID);
									gameObjectMesh->LoadFromFile(selectedMeshRelativeFilePath, &importSettings);
								}
							}
						}
					}

					ImGui::SameLine();

					if (ImGui::Button("Save"))
					{
						g_SceneManager->CurrentScene()->SerializeToFile(true);
					}

					if (ImGui::BeginChild("mesh list", ImVec2(0.0f, 120.0f), true))
					{
						i32 i = 0;
						for (const auto& meshIter : MeshComponent::m_LoadedMeshes)
						{
							bool bSelected = (i == selectedMeshIndex);
							std::string meshFilePath = meshIter.first;
							std::string meshFileName = meshIter.first;
							StripLeadingDirectories(meshFileName);
							if (ImGui::Selectable(meshFileName.c_str(), &bSelected))
							{
								selectedMeshIndex = i;
								bUpdateName = true;
							}

							if (ImGui::BeginPopupContextItem())
							{
								if (ImGui::Button("Reload"))
								{
									MeshComponent::LoadMesh(meshIter.second->relativeFilePath);

									for (GLRenderObject* renderObject : m_RenderObjects)
									{
										if (renderObject)
										{
											MeshComponent* mesh = renderObject->gameObject->GetMeshComponent();
											if (mesh && mesh->GetRelativeFilePath().compare(meshFilePath) == 0)
											{
												mesh->Reload();
											}
										}
									}

									ImGui::CloseCurrentPopup();
								}

								ImGui::EndPopup();
							}
							else
							{
								if (ImGui::IsItemActive())
								{
									if (ImGui::BeginDragDropSource())
									{
										const void* data = (void*)(meshIter.first.c_str());
										u32 size = strlen(meshIter.first.c_str()) * sizeof(char);

										ImGui::SetDragDropPayload(m_MeshPayloadCStr, data, size);

										ImGui::Text("%s", meshFileName.c_str());

										ImGui::EndDragDropSource();
									}
								}
							}

							++i;
						}
					}
					ImGui::EndChild();

					if (ImGui::Button("Import Mesh"))
					{
						// TODO: Not all models are directly in this directory! CLEANUP to make more robust
						std::string relativeDirPath = RESOURCE_LOCATION  "meshes/";
						std::string absoluteDirectoryStr = RelativePathToAbsolute(relativeDirPath);
						std::string selectedAbsFilePath;
						if (OpenFileDialog("Import mesh", absoluteDirectoryStr, selectedAbsFilePath))
						{
							Print("Importing mesh: %s\n", selectedAbsFilePath.c_str());

							std::string fileNameAndExtension = selectedAbsFilePath;
							StripLeadingDirectories(fileNameAndExtension);
							std::string relativeFilePath = relativeDirPath + fileNameAndExtension;

							LoadedMesh* existingMesh = nullptr;
							if (MeshComponent::FindPreLoadedMesh(relativeFilePath, &existingMesh))
							{
								i32 j = 0;
								for (auto meshPair : MeshComponent::m_LoadedMeshes)
								{
									if (meshPair.first.compare(relativeFilePath) == 0)
									{
										selectedMeshIndex = j;
										break;
									}

									++j;
								}
							}
							else
							{
								MeshComponent::LoadMesh(relativeFilePath);
							}

							ImGui::CloseCurrentPopup();
						}
					}
				}
			}

			ImGui::End();
		}

		void GLRenderer::DrawImGuiForRenderObject(RenderID renderID)
		{
			UNREFERENCED_PARAMETER(renderID);
		}

		void GLRenderer::UpdateVertexData(RenderID renderID, VertexBufferData* vertexBufferData)
		{
			PROFILE_AUTO("Update Vertex Data");

			GLRenderObject* renderObject = GetRenderObject(renderID);

			glBindVertexArray(renderObject->VAO);
			glBindBuffer(GL_ARRAY_BUFFER, renderObject->VBO);
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertexBufferData->VertexBufferSize, vertexBufferData->vertexData, GL_DYNAMIC_DRAW);
		}

		void GLRenderer::DrawUntexturedQuad(const glm::vec2& pos,
			AnchorPoint anchor,
			const glm::vec2& size,
			const glm::vec4& color)
		{
			SpriteQuadDrawInfo drawInfo = {};

			drawInfo.spriteObjectRenderID = m_Quad3DRenderID;
			drawInfo.materialID = m_SpriteMatID;
			drawInfo.scale = glm::vec3(size.x, size.y, 1.0f);
			drawInfo.bScreenSpace = true;
			drawInfo.bReadDepth = false;
			drawInfo.bWriteDepth = false;
			drawInfo.anchor = anchor;
			drawInfo.color = color;
			drawInfo.pos = glm::vec3(pos.x, pos.y, 1.0f);
			drawInfo.bEnableAlbedoSampler = false;

			DrawSprite(drawInfo);
		}

		void GLRenderer::DrawUntexturedQuadRaw(const glm::vec2& pos,
			const glm::vec2& size,
			const glm::vec4& color)
		{
			SpriteQuadDrawInfo drawInfo = {};

			drawInfo.spriteObjectRenderID = m_Quad3DRenderID;
			drawInfo.materialID = m_SpriteMatID;
			drawInfo.scale = glm::vec3(size.x, size.y, 1.0f);
			drawInfo.bScreenSpace = true;
			drawInfo.bReadDepth = false;
			drawInfo.bWriteDepth = false;
			drawInfo.bRaw = true;
			drawInfo.color = color;
			drawInfo.pos = glm::vec3(pos.x, pos.y, 1.0f);
			drawInfo.bEnableAlbedoSampler = false;

			DrawSprite(drawInfo);
		}

		void GLRenderer::DrawSprite(const SpriteQuadDrawInfo& drawInfo)
		{
			if (drawInfo.bScreenSpace)
			{
				m_QueuedSSSprites.push_back(drawInfo);
			}
			else
			{
				m_QueuedWSSprites.push_back(drawInfo);
			}
		}

		GLRenderObject* GLRenderer::GetRenderObject(RenderID renderID)
		{
#if DEBUG
			if (renderID > m_RenderObjects.size() ||
				renderID == InvalidRenderID)
			{
				PrintError("Invalid renderID passed to GetRenderObject: %i\n", renderID);
				return nullptr;
			}
#endif

			return m_RenderObjects[renderID];
		}

		void GLRenderer::InsertNewRenderObject(GLRenderObject* renderObject)
		{
			if (renderObject->renderID < m_RenderObjects.size())
			{
				assert(m_RenderObjects[renderObject->renderID] == nullptr);
				m_RenderObjects[renderObject->renderID] = renderObject;
			}
			else
			{
				m_RenderObjects.resize(renderObject->renderID + 1);
				m_RenderObjects[renderObject->renderID] = renderObject;
			}
		}

		MaterialID GLRenderer::GetNextAvailableMaterialID()
		{
			// Return lowest available ID
			MaterialID result = 0;
			while (m_Materials.find(result) != m_Materials.end())
			{
				++result;
			}
			return result;
		}

		RenderID GLRenderer::GetNextAvailableRenderID() const
		{
			for (i32 i = (i32)m_RenderObjects.size() - 1; i >= 0; --i)
			{
				if (m_RenderObjects[i] == nullptr)
				{
					return (RenderID)i;
				}
			}

			return m_RenderObjects.size();
		}

		EventReply GLRenderer::OnKeyEvent(KeyCode keyCode, KeyAction action, i32 modifiers)
		{
			if (action == KeyAction::PRESS)
			{
				if (keyCode == KeyCode::KEY_U && modifiers == 0)
				{
					m_bCaptureReflectionProbes = true;
					return EventReply::CONSUMED;
				}

			}

			return EventReply::UNCONSUMED;
		}

		EventReply GLRenderer::OnActionEvent(Action action)
		{
			if (action == Action::TAKE_SCREENSHOT)
			{
				m_bCaptureScreenshot = true;
				return EventReply::CONSUMED;
			}
			return EventReply::UNCONSUMED;
		}

		void SetClipboardText(void* userData, const char* text)
		{
			GLFWWindowWrapper* glfwWindow = static_cast<GLFWWindowWrapper*>(userData);
			glfwWindow->SetClipboardText(text);
		}

		const char* GetClipboardText(void* userData)
		{
			GLFWWindowWrapper* glfwWindow = static_cast<GLFWWindowWrapper*>(userData);
			return glfwWindow->GetClipboardText();
		}

	} // namespace gl
} // namespace flex

#endif // COMPILE_OPEN_GL
