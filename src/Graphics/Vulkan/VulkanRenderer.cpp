#include "stdafx.h"
#if COMPILE_VULKAN

#include "Graphics/Vulkan/VulkanRenderer.h"
#include "Helpers.h"
#include "GameContext.h"
#include "Window/Window.h"
#include "Logger.h"
#include "FreeCamera.h"
#include "VertexBufferData.h"

#include <algorithm>	
#include <set>
#include <iostream>
#include <unordered_map>

#include <glm\gtc\matrix_transform.hpp>

#include <SOIL.h>

using namespace glm;

VulkanRenderer::VulkanRenderer(GameContext& gameContext) :
	m_VertexBuffer_Simple(m_Device),
	m_IndexBuffer_Simple(m_Device),
	m_VertexBuffer_Color(m_Device),
	m_IndexBuffer_Color(m_Device),
	m_DynamicAlignment(0)
{
	CreateInstance(gameContext);
	SetupDebugCallback();
	CreateSurface(gameContext.window);
	PickPhysicalDevice();
	CreateLogicalDevice();
	CreateSwapChain(gameContext.window);
	CreateImageViews();
	CreateRenderPass();
	
	CreateCommandPool();
	CreateDepthResources();
	CreateFramebuffers();

	LoadDefaultShaderCode();
}

void VulkanRenderer::PostInitialize()
{
	// Brick textures
	CreateTextureImage("resources/textures/brick_d.png", &m_BrickDiffuseTexture);
	if (m_BrickDiffuseTexture)
	{
		CreateTextureImageView(m_BrickDiffuseTexture);
		CreateTextureSampler(m_BrickDiffuseTexture);
	}

	CreateTextureImage("resources/textures/brick_n.png", &m_BrickNormalTexture);
	if (m_BrickNormalTexture)
	{
		CreateTextureImageView(m_BrickNormalTexture);
		CreateTextureSampler(m_BrickNormalTexture);
	}

	CreateTextureImage("resources/textures/brick_s.png", &m_BrickSpecularTexture);
	if (m_BrickSpecularTexture)
	{
		CreateTextureImageView(m_BrickSpecularTexture);
		CreateTextureSampler(m_BrickSpecularTexture);
	}

	// Work textures
	CreateTextureImage("resources/textures/work_d.jpg", &m_WorkDiffuseTexture);
	if (m_WorkDiffuseTexture)
	{
		CreateTextureImageView(m_WorkDiffuseTexture);
		CreateTextureSampler(m_WorkDiffuseTexture);
	}

	CreateTextureImage("resources/textures/work_n.jpg", &m_WorkNormalTexture);
	if (m_WorkNormalTexture)
	{
		CreateTextureImageView(m_WorkNormalTexture);
		CreateTextureSampler(m_WorkNormalTexture);
	}
	
	CreateTextureImage("resources/textures/work_s.jpg", &m_WorkSpecularTexture);
	if (m_WorkSpecularTexture)
	{
		CreateTextureImageView(m_WorkSpecularTexture);
		CreateTextureSampler(m_WorkSpecularTexture);
	}


	PrepareUniformBuffers();

	CreateDescriptorPool();

	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		CreateDescriptorSetLayout(m_RenderObjects[i]->shaderIndex);
	}

	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		RenderObject* renderObject = m_RenderObjects[i];

		if (renderObject->shaderIndex == 0)
		{
			if (i % 2 == 0)
			{
				renderObject->diffuseTexture = m_BrickDiffuseTexture;
				renderObject->normalTexture = m_BrickNormalTexture;
				renderObject->specularTexture = m_BrickSpecularTexture;
			}
			else
			{
				renderObject->diffuseTexture = m_WorkDiffuseTexture;
				renderObject->normalTexture = m_WorkNormalTexture;
				renderObject->specularTexture = m_WorkSpecularTexture;
			}
		}
		else
		{
			// No textures to set
		}

		CreateDescriptorSet(i, renderObject->descriptorSetLayoutIndex);
		CreateGraphicsPipeline(i);
	}

	CreateVertexBuffers();
	CreateIndexBuffers();

	CreateCommandBuffers();
	CreateSemaphores();
}

VulkanRenderer::~VulkanRenderer()
{
	//vkDestroyImageView(m_Device, m_TextureImageView, nullptr);
	//
	//vkDestroyImage(m_Device, m_TextureImage, nullptr);
	//vkFreeMemory(m_Device, m_TextureImageMemory, nullptr);

	auto iter = m_RenderObjects.begin();
	while (iter != m_RenderObjects.end())
	{
		UniformBufferData* dynamicUniformBufferData = &m_UniformBufferPairs[(*iter)->shaderIndex].dynamicBufferData;
		_aligned_free(dynamicUniformBufferData->data);
		SafeDelete(*iter);
		iter = m_RenderObjects.erase(iter);
	}
	m_RenderObjects.clear();

	for (auto iter = m_DescriptorSetLayouts.begin(); iter != m_DescriptorSetLayouts.end(); ++iter)
	{
		vkDestroyDescriptorSetLayout(m_Device, *iter, nullptr);
	}

	SafeDelete(m_BrickDiffuseTexture);
	SafeDelete(m_BrickNormalTexture);
	SafeDelete(m_BrickSpecularTexture);

	SafeDelete(m_WorkDiffuseTexture);
	SafeDelete(m_WorkNormalTexture);
	SafeDelete(m_WorkSpecularTexture);

	vkDeviceWaitIdle(m_Device);	

	glfwTerminate();
}

glm::uint VulkanRenderer::Initialize(const GameContext& gameContext, const RenderObjectCreateInfo* createInfo)
{
	size_t renderID = m_RenderObjects.size();
	RenderObject* renderObject = new RenderObject(m_Device);
	m_RenderObjects.push_back(renderObject);

	renderObject->vertexBufferData = createInfo->vertexBufferData;

	if (createInfo->indices != nullptr)
	{
		renderObject->indices = createInfo->indices;
		renderObject->indexed = true;
	}

	renderObject->descriptorSetLayoutIndex = renderID;
	renderObject->shaderIndex = createInfo->shaderIndex;

	return renderID;
}

void VulkanRenderer::SetTopologyMode(glm::uint renderID, TopologyMode topology)
{
	RenderObject* renderObject = GetRenderObject(renderID);
	VkPrimitiveTopology vkTopology = TopologyModeToVkPrimitiveTopology(topology);

	if (vkTopology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
	{
		Logger::LogError("Unsupported TopologyMode passed to VulkanRenderer::SetTopologyMode: " + (int)topology);
	}
	else
	{
		renderObject->topology = vkTopology;
	}
}

void VulkanRenderer::SetClearColor(float r, float g, float b)
{
	m_ClearColor = { r, g, b, 1.0f };
}

void VulkanRenderer::Update(const GameContext& gameContext)
{
	// Update uniform buffer
	UpdateConstantUniformBuffers(gameContext);

	// TODO: Only call this when objects change
	RebuildCommandBuffers();
}

void VulkanRenderer::Draw(const GameContext& gameContext, glm::uint renderID)
{
	UNREFERENCED_PARAMETER(gameContext);
	UNREFERENCED_PARAMETER(renderID);
}

void VulkanRenderer::ReloadShaders(GameContext& gameContext)
{
	// TODO: Implement
}

void VulkanRenderer::OnWindowSize(int width, int height)
{
	m_SwapChainNeedsRebuilding = true;
}

void VulkanRenderer::SetVSyncEnabled(bool enableVSync)
{
	m_VSyncEnabled = enableVSync;
}

void VulkanRenderer::Clear(int flags, const GameContext& gameContext)
{
	UNREFERENCED_PARAMETER(gameContext);
	UNREFERENCED_PARAMETER(flags);
}

void VulkanRenderer::SwapBuffers(const GameContext& gameContext)
{
	if (m_SwapChainNeedsRebuilding)
	{
		m_SwapChainNeedsRebuilding = false;
		RecreateSwapChain(gameContext.window);
	}
	else
	{
		DrawFrame(gameContext.window);
	}
}

void VulkanRenderer::UpdateTransformMatrix(const GameContext& gameContext, glm::uint renderID, const glm::mat4& model)
{
	UpdateUniformBufferDynamic(gameContext, renderID, model);
}

int VulkanRenderer::GetShaderUniformLocation(glm::uint program, const std::string uniformName)
{
	// TODO: Implement
	UNREFERENCED_PARAMETER(program);
	UNREFERENCED_PARAMETER(uniformName);
	return 0;
}

void VulkanRenderer::SetUniform1f(glm::uint location, float val)
{
	// TODO: Implement
	UNREFERENCED_PARAMETER(location);
	UNREFERENCED_PARAMETER(val);
}

glm::uint VulkanRenderer::GetProgram(glm::uint renderID)
{
	// TODO: Implement
	return 0;
}

void VulkanRenderer::DescribeShaderVariable(glm::uint renderID, glm::uint program, const std::string& variableName, int size, Renderer::Type renderType, bool normalized, int stride, void* pointer)
{
	// TODO: Implement
	UNREFERENCED_PARAMETER(renderID);
	UNREFERENCED_PARAMETER(program);
	UNREFERENCED_PARAMETER(variableName);
	UNREFERENCED_PARAMETER(size);
	UNREFERENCED_PARAMETER(renderType);
	UNREFERENCED_PARAMETER(normalized);
	UNREFERENCED_PARAMETER(stride);
	UNREFERENCED_PARAMETER(pointer);
}

void VulkanRenderer::Destroy(glm::uint renderID)
{
	for (auto iter = m_RenderObjects.begin(); iter != m_RenderObjects.end(); ++iter)
	{
		if ((*iter)->renderID == renderID)
		{
			SafeDelete(*iter);
			iter = m_RenderObjects.erase(iter);
			return;
		}
	}
}

RenderObject* VulkanRenderer::GetRenderObject(int renderID)
{
	return m_RenderObjects[renderID];
}

void VulkanRenderer::CreateInstance(const GameContext& gameContext)
{
	if (m_EnableValidationLayers && !CheckValidationLayerSupport())
	{
		throw std::runtime_error("validation layers requested, but not available!");
	}

	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	std::string applicationName = gameContext.window->GetTitle();
	appInfo.pApplicationName = applicationName.c_str();
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Rendering Engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	auto extensions = GetRequiredExtensions();
	createInfo.enabledExtensionCount = extensions.size();
	createInfo.ppEnabledExtensionNames = extensions.data();

	if (m_EnableValidationLayers)
	{
		createInfo.enabledLayerCount = m_ValidationLayers.size();
		createInfo.ppEnabledLayerNames = m_ValidationLayers.data();
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	VK_CHECK_RESULT(vkCreateInstance(&createInfo, nullptr, m_Instance.replace()));
}

void VulkanRenderer::SetupDebugCallback()
{
	if (!m_EnableValidationLayers) return;

	VkDebugReportCallbackCreateInfoEXT createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
	createInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
	createInfo.pfnCallback = DebugCallback;

	VK_CHECK_RESULT(CreateDebugReportCallbackEXT(m_Instance, &createInfo, nullptr, m_Callback.replace()));
}

void VulkanRenderer::CreateSurface(Window* window)
{
	VK_CHECK_RESULT(glfwCreateWindowSurface(m_Instance, ((VulkanWindowWrapper*)window)->GetWindow(), nullptr, m_Surface.replace()));
}

void VulkanRenderer::PickPhysicalDevice()
{
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

	if (deviceCount == 0)
	{
		throw std::runtime_error("failed to find GPUs with Vulkan support!");
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

	for (const auto& device : devices)
	{
		if (IsDeviceSuitable(device))
		{
			m_PhysicalDevice = device;
			break;
		}
	}

	if (m_PhysicalDevice == VK_NULL_HANDLE)
	{
		throw std::runtime_error("failed to find a suitable GPU!");
	}
}

void VulkanRenderer::CreateLogicalDevice()
{
	QueueFamilyIndices indices = FindQueueFamilies(m_PhysicalDevice);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<int> uniqueQueueFamilies = { indices.graphicsFamily, indices.presentFamily };

	float queuePriority = 1.0f;
	for (int queueFamily : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures = {};
	deviceFeatures.samplerAnisotropy = VK_TRUE;

	VkDeviceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();

	createInfo.pEnabledFeatures = &deviceFeatures;

	createInfo.enabledExtensionCount = m_DeviceExtensions.size();
	createInfo.ppEnabledExtensionNames = m_DeviceExtensions.data();

	if (m_EnableValidationLayers)
	{
		createInfo.enabledLayerCount = m_ValidationLayers.size();
		createInfo.ppEnabledLayerNames = m_ValidationLayers.data();
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	VK_CHECK_RESULT(vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, m_Device.replace()));

	vkGetPhysicalDeviceProperties(m_PhysicalDevice, &m_PhysicalDeviceProperties);

	vkGetDeviceQueue(m_Device, indices.graphicsFamily, 0, &m_GraphicsQueue);
	vkGetDeviceQueue(m_Device, indices.presentFamily, 0, &m_PresentQueue);
}

void VulkanRenderer::RecreateSwapChain(Window* window)
{
	vkDeviceWaitIdle(m_Device);

	CreateSwapChain(window);
	CreateImageViews();
	CreateRenderPass();
	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		CreateGraphicsPipeline(i);
	}
	CreateDepthResources();
	CreateFramebuffers();
	CreateCommandBuffers();
}

void VulkanRenderer::CreateSwapChain(Window* window)
{
	SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(m_PhysicalDevice);

	VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
	VkExtent2D extent = ChooseSwapExtent(window, swapChainSupport.capabilities);

	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount)
	{
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_Surface;

	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	QueueFamilyIndices indices = FindQueueFamilies(m_PhysicalDevice);
	uint32_t queueFamilyIndices[] = { (uint32_t)indices.graphicsFamily, (uint32_t)indices.presentFamily };

	if (indices.graphicsFamily != indices.presentFamily)
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;

	VkSwapchainKHR oldSwapChain = m_SwapChain;
	createInfo.oldSwapchain = oldSwapChain;

	VkSwapchainKHR newSwapChain;
	VK_CHECK_RESULT(vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &newSwapChain));

	m_SwapChain = newSwapChain;

	vkGetSwapchainImagesKHR(m_Device, m_SwapChain, &imageCount, nullptr);
	m_SwapChainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(m_Device, m_SwapChain, &imageCount, m_SwapChainImages.data());

	m_SwapChainImageFormat = surfaceFormat.format;
	m_SwapChainExtent = extent;
}

void VulkanRenderer::CreateImageViews()
{
	m_SwapChainImageViews.resize(m_SwapChainImages.size(), VDeleter<VkImageView>{ m_Device, vkDestroyImageView });

	for (uint32_t i = 0; i < m_SwapChainImages.size(); i++)
	{
		CreateImageView(m_SwapChainImages[i], m_SwapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, m_SwapChainImageViews[i]);
	}
}

void VulkanRenderer::CreateTextureImageView(VulkanTexture* texture)
{
	CreateImageView(texture->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, texture->imageView);
}

void VulkanRenderer::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, VDeleter<VkImageView>& imageView)
{
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VK_CHECK_RESULT(vkCreateImageView(m_Device, &viewInfo, nullptr, imageView.replace()));
}

void VulkanRenderer::CreateRenderPass()
{
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = m_SwapChainImageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachmentRef = {};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentDescription depthAttachment = {};
	depthAttachment.format = FindDepthFormat();
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef = {};
	depthAttachmentRef.attachment = 1;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = attachments.size();
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	VK_CHECK_RESULT(vkCreateRenderPass(m_Device, &renderPassInfo, nullptr, m_RenderPass.replace()));
}

void VulkanRenderer::CreateTextureSampler(VulkanTexture* texture)
{
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_TRUE;
	samplerInfo.maxAnisotropy = 16;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	VK_CHECK_RESULT(vkCreateSampler(m_Device, &samplerInfo, nullptr, texture->sampler.replace()));
}

void VulkanRenderer::CreateGraphicsPipeline(glm::uint renderID)
{
	RenderObject* renderObject = GetRenderObject(renderID);

	ShaderCode shaderCode = m_LoadedShaderCode[renderObject->shaderIndex];
	std::vector<char> vertShaderCode = shaderCode.vertexShaderCode;
	std::vector<char> fragShaderCode = shaderCode.fragmentShaderCode;

	VDeleter<VkShaderModule> vertShaderModule{ m_Device, vkDestroyShaderModule };
	CreateShaderModule(vertShaderCode, vertShaderModule);

	VDeleter<VkShaderModule> fragShaderModule{ m_Device, vkDestroyShaderModule };
	CreateShaderModule(fragShaderCode, fragShaderModule);

	VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
	vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertShaderStageInfo.module = vertShaderModule;
	vertShaderStageInfo.pName = "main";

	VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
	fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragShaderStageInfo.module = fragShaderModule;
	fragShaderStageInfo.pName = "main";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

	VkVertexInputBindingDescription bindingDescription = VulkanVertex::GetVertexBindingDescription(renderObject->vertexBufferData);
	std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	VulkanVertex::GetVertexAttributeDescriptions(renderObject->vertexBufferData, attributeDescriptions, renderObject->shaderIndex);

	VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.vertexAttributeDescriptionCount = attributeDescriptions.size();
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = renderObject->topology;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)m_SwapChainExtent.width;
	viewport.height = (float)m_SwapChainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor = {};
	scissor.offset = { 0, 0 };
	scissor.extent = m_SwapChainExtent;

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterizer = {};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling = {};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlending = {};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;
	colorBlending.blendConstants[0] = 0.0f;
	colorBlending.blendConstants[1] = 0.0f;
	colorBlending.blendConstants[2] = 0.0f;
	colorBlending.blendConstants[3] = 0.0f;

	VkDescriptorSetLayout setLayouts[] = { m_DescriptorSetLayouts[renderObject->descriptorSetLayoutIndex] };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = setLayouts;

	VK_CHECK_RESULT(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, renderObject->pipelineLayout.replace()));

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {};
	depthStencil.back = {};

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.layout = renderObject->pipelineLayout;
	pipelineInfo.renderPass = m_RenderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	VK_CHECK_RESULT(vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, renderObject->graphicsPipeline.replace()));
}

void VulkanRenderer::CreateFramebuffers()
{
	m_SwapChainFramebuffers.resize(m_SwapChainImageViews.size(), VDeleter<VkFramebuffer>{m_Device, vkDestroyFramebuffer});

	for (size_t i = 0; i < m_SwapChainImageViews.size(); i++)
	{
		std::array<VkImageView, 2> attachments = {
			m_SwapChainImageViews[i],
			m_DepthImageView
		};

		VkFramebufferCreateInfo framebufferInfo = {};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = m_RenderPass;
		framebufferInfo.attachmentCount = attachments.size();
		framebufferInfo.pAttachments = attachments.data();
		framebufferInfo.width = m_SwapChainExtent.width;
		framebufferInfo.height = m_SwapChainExtent.height;
		framebufferInfo.layers = 1;

		VK_CHECK_RESULT(vkCreateFramebuffer(m_Device, &framebufferInfo, nullptr, m_SwapChainFramebuffers[i].replace()));
	}
}

void VulkanRenderer::CreateCommandPool()
{
	QueueFamilyIndices queueFamilyIndices = FindQueueFamilies(m_PhysicalDevice);

	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily;

	VK_CHECK_RESULT(vkCreateCommandPool(m_Device, &poolInfo, nullptr, m_CommandPool.replace()));
}

VkCommandBuffer VulkanRenderer::BeginSingleTimeCommands()
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	// TODO: Create command pool just for these types of alocations, using VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
	allocInfo.commandPool = m_CommandPool; 
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(m_Device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

void VulkanRenderer::EndSingleTimeCommands(VkCommandBuffer commandBuffer)
{
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_GraphicsQueue);

	vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
}

void VulkanRenderer::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
	VkMemoryPropertyFlags properties, VDeleter<VkImage>& image, VDeleter<VkDeviceMemory>& imageMemory)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK_RESULT(vkCreateImage(m_Device, &imageInfo, nullptr, image.replace()));

	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(m_Device, image, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

	VK_CHECK_RESULT(vkAllocateMemory(m_Device, &allocInfo, nullptr, imageMemory.replace()));

	vkBindImageMemory(m_Device, image, imageMemory, 0);
}

VkFormat VulkanRenderer::FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, format, &properties);

		if (tiling == VK_IMAGE_TILING_LINEAR && (properties.linearTilingFeatures & features) == features)
		{
			return format;
		}
		else if (tiling == VK_IMAGE_TILING_OPTIMAL && (properties.optimalTilingFeatures & features) == features)
		{
			return format;
		}
	}

	throw std::runtime_error("failed to find supported formats!");
}

VkFormat VulkanRenderer::FindDepthFormat()
{
	return FindSupportedFormat(
	{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

bool VulkanRenderer::HasStencilComponent(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

void VulkanRenderer::CreateDepthResources()
{
	VkFormat depthFormat = FindDepthFormat();

	CreateImage(m_SwapChainExtent.width, m_SwapChainExtent.height, depthFormat, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_DepthImage, m_DepthImageMemory);
	CreateImageView(m_DepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, m_DepthImageView);

	TransitionImageLayout(m_DepthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void VulkanRenderer::CreateTextureImage(const std::string& filePath, VulkanTexture** texture)
{
	int textureWidth, textureHeight, textureChannels;
	unsigned char* pixels = SOIL_load_image(filePath.c_str(), &textureWidth, &textureHeight, &textureChannels, SOIL_LOAD_RGBA);

	if (!pixels)
	{
		Logger::LogError("SOIL loading error: " + std::string(SOIL_last_result()) + ", image filepath: " + filePath);
		return;
	}
	
	*texture = new VulkanTexture(m_Device);

	VkDeviceSize imageSize = textureWidth * textureHeight * 4;

	VulkanBuffer stagingBuffer{ m_Device };

	CreateAndAllocateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer);

	//VkImageSubresource subresource = {};
	//subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	//subresource.mipLevel = 0;
	//subresource.arrayLayer = 0;
	//
	//VkSubresourceLayout stagingImageLayout;
	//vkGetImageSubresourceLayout(m_Device, stagingBuffer.m_Memory, &subresource, &stagingImageLayout);

	void* data;
	vkMapMemory(m_Device, stagingBuffer.m_Memory, 0, imageSize, 0, &data);

	//if (stagingImageLayout.rowPitch == textureWidth * 4)
	//{
		memcpy(data, pixels, (size_t)imageSize);
	//}
	//else
	//{
	//	uint8_t* dataBytes = reinterpret_cast<uint8_t*>(data);
	//
	//	for (int y = 0; y < textureHeight; y++)
	//	{
	//		memcpy(&dataBytes[y * stagingImageLayout.rowPitch],
	//			&pixels[y * textureWidth * 4],
	//			textureWidth * 4);
	//	}
	//}

	vkUnmapMemory(m_Device, stagingBuffer.m_Memory);

	SOIL_free_image_data(pixels);

	CreateImage(textureWidth, textureHeight, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 
		(*texture)->image, (*texture)->imageMemory);

	TransitionImageLayout((*texture)->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	CopyBufferToImage(stagingBuffer.m_Buffer, (*texture)->image, (uint32_t)textureWidth, (uint32_t)textureHeight);
	TransitionImageLayout((*texture)->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanRenderer::RebuildCommandBuffers()
{
	if (!CheckCommandBuffers())
	{
		DestroyCommandBuffers();
		CreateCommandBuffers();
	}
	BuildCommandBuffers();
}

void VulkanRenderer::CreateCommandBuffers()
{
	// One command buffer per frame back buffer
	m_CommandBuffers.resize(m_SwapChainFramebuffers.size());

	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = m_CommandPool;
	allocInfo.commandBufferCount = (uint32_t)m_CommandBuffers.size();

	VK_CHECK_RESULT(vkAllocateCommandBuffers(m_Device, &allocInfo, m_CommandBuffers.data()));
}

void VulkanRenderer::BuildCommandBuffers()
{
	std::array<VkClearValue, 2> clearValues = {};
	clearValues[0].color = m_ClearColor;
	clearValues[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo renderPassBeginInfo = {};
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.renderPass = m_RenderPass;
	renderPassBeginInfo.renderArea.offset = { 0, 0 };
	renderPassBeginInfo.renderArea.extent = m_SwapChainExtent;
	renderPassBeginInfo.clearValueCount = clearValues.size();
	renderPassBeginInfo.pClearValues = clearValues.data();

	for (size_t i = 0; i < m_CommandBuffers.size(); i++)
	{
		renderPassBeginInfo.framebuffer = m_SwapChainFramebuffers[i];

		VkCommandBufferBeginInfo cmdBufferbeginInfo = {};
		cmdBufferbeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cmdBufferbeginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

		VK_CHECK_RESULT(vkBeginCommandBuffer(m_CommandBuffers[i], &cmdBufferbeginInfo));

		vkCmdBeginRenderPass(m_CommandBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = VkViewport{ 0.0f, 1.0f, (float)m_SwapChainExtent.width, (float)m_SwapChainExtent.height, 0.1f, 1000.0f };
		vkCmdSetViewport(m_CommandBuffers[i], 0, 1, &viewport);

		VkRect2D scissor = VkRect2D{ { 0u, 0u }, { m_SwapChainExtent.width, m_SwapChainExtent.height } };
		vkCmdSetScissor(m_CommandBuffers[i], 0, 1, &scissor);

		for (size_t j = 0; j < m_RenderObjects.size(); j++)
		{
			RenderObject* renderObject = m_RenderObjects[j];

			uint32_t dynamicOffset = renderObject->uniformBufferObjectIndex * static_cast<uint32_t>(m_DynamicAlignment);

			VkDeviceSize offsets[1] = { 0 };
			if (renderObject->shaderIndex == 0)
			{
				vkCmdBindVertexBuffers(m_CommandBuffers[i], 0, 1, &m_VertexBuffer_Simple.m_Buffer, offsets);

				if (m_IndexBuffer_Simple.m_Size != 0)
				{
					vkCmdBindIndexBuffer(m_CommandBuffers[i], m_IndexBuffer_Simple.m_Buffer, 0, VK_INDEX_TYPE_UINT32);
				}
			}
			else if (renderObject->shaderIndex == 1)
			{
				vkCmdBindVertexBuffers(m_CommandBuffers[i], 0, 1, &m_VertexBuffer_Color.m_Buffer, offsets);

				if (m_IndexBuffer_Color.m_Size != 0)
				{
					vkCmdBindIndexBuffer(m_CommandBuffers[i], m_IndexBuffer_Color.m_Buffer, 0, VK_INDEX_TYPE_UINT32);
				}
			}

			vkCmdBindPipeline(m_CommandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, renderObject->graphicsPipeline);

			vkCmdBindDescriptorSets(m_CommandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, renderObject->pipelineLayout,
				0, 1, &renderObject->descriptorSet,
				1, &dynamicOffset);

			if (renderObject->indexed)
			{
				vkCmdDrawIndexed(m_CommandBuffers[i], renderObject->indices->size(), 1, renderObject->indexOffset, 0, 0);
			}
			else
			{
				vkCmdDraw(m_CommandBuffers[i], renderObject->vertexBufferData->VertexCount, 1, renderObject->vertexOffset, 0);
			}
		}

		vkCmdEndRenderPass(m_CommandBuffers[i]);

		VK_CHECK_RESULT(vkEndCommandBuffer(m_CommandBuffers[i]));
	}
}

bool VulkanRenderer::CheckCommandBuffers()
{
	for (auto& cmdBuffer : m_CommandBuffers)
	{
		if (cmdBuffer == VK_NULL_HANDLE)
		{
			return false;
		}
	}
	return true;
}

void VulkanRenderer::DestroyCommandBuffers()
{
	vkFreeCommandBuffers(m_Device, m_CommandPool, static_cast<uint32_t>(m_CommandBuffers.size()), m_CommandBuffers.data());
}

uint32_t VulkanRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}

	throw std::runtime_error("failed to find any suitable memory type!");
}

void VulkanRenderer::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;

	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		if (HasStencilComponent(format))
		{
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	else
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	if (oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	else
	{
		throw std::invalid_argument("unsupported layout transition!");
	}

	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier);

	EndSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::CopyImage(VkImage srcImage, VkImage dstImage, uint32_t width, uint32_t height)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkImageSubresourceLayers subresource = {};
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource.baseArrayLayer = 0;
	subresource.mipLevel = 0;
	subresource.layerCount = 1;

	VkImageCopy region = {};
	region.srcSubresource = subresource;
	region.dstSubresource = subresource;
	region.srcOffset = { 0, 0, 0 };
	region.dstOffset = { 0, 0, 0 };
	region.extent.width = width;
	region.extent.height = height;
	region.extent.depth = 1;

	vkCmdCopyImage(commandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	EndSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = {
		width,
		height,
		1
	};

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	EndSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::CreateAndAllocateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
	VkMemoryPropertyFlags properties, VulkanBuffer& buffer)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK_RESULT(vkCreateBuffer(m_Device, &bufferInfo, nullptr, buffer.m_Buffer.replace()));

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(m_Device, buffer.m_Buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

	VK_CHECK_RESULT(vkAllocateMemory(m_Device, &allocInfo, nullptr, buffer.m_Memory.replace()));

	// Create the memory backing up the buffer handle
	buffer.m_Alignment = memRequirements.alignment;
	buffer.m_Size = allocInfo.allocationSize;
	buffer.m_UsageFlags = usage;
	buffer.m_MemoryPropertyFlags = properties;

	// Initialize a default descriptor that covers the whole buffer size
	buffer.m_DescriptorInfo.offset = 0;
	buffer.m_DescriptorInfo.range = VK_WHOLE_SIZE;
	buffer.m_DescriptorInfo.buffer = buffer.m_Buffer;

	vkBindBufferMemory(m_Device, buffer.m_Buffer, buffer.m_Memory, 0);
}

void VulkanRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkBufferCopy copyRegion = {};
	copyRegion.size = size;
	copyRegion.dstOffset = dstOffset;
	copyRegion.srcOffset = srcOffset;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	EndSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::CreateVertexBuffers()
{
	CreateVertexBuffer(m_VertexBuffer_Simple, 0);
	CreateVertexBuffer(m_VertexBuffer_Color, 1);
}

void VulkanRenderer::CreateVertexBuffer(VulkanBuffer& vertexBuffer, glm::uint shaderIndex)
{
	int requiredMemory = 0;

	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		RenderObject* renderObject = GetRenderObject(i);
		if (renderObject->shaderIndex == shaderIndex)
		{
			requiredMemory += renderObject->vertexBufferData->BufferSize;
		}
	}

	if (requiredMemory == 0)
	{
		return;
	}

	void* vertexDataStart = malloc(requiredMemory);
	if (!vertexDataStart)
	{
		Logger::LogError("Failed to allocate memory for vertex buffer " + std::to_string(shaderIndex) + "! Attempted to allocate " + std::to_string(requiredMemory) + " bytes");
		return;
	}

	void* vertexBufferData = vertexDataStart;

	glm::uint vertexCount = 0;
	glm::uint vertexBufferSize = 0;
	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		RenderObject* renderObject = GetRenderObject(i);
		if (renderObject->shaderIndex == shaderIndex)
		{
			renderObject->vertexOffset = vertexCount;

			memcpy(vertexBufferData, renderObject->vertexBufferData->pDataStart, renderObject->vertexBufferData->BufferSize);

			vertexCount += renderObject->vertexBufferData->VertexCount;
			vertexBufferSize += renderObject->vertexBufferData->BufferSize;

			vertexBufferData = (char*)vertexBufferData + renderObject->vertexBufferData->BufferSize;
		}
	}

	Logger::Assert(vertexBufferSize != 0);
	Logger::Assert(vertexCount != 0);

	VulkanBuffer stagingBuffer(m_Device);
	CreateAndAllocateBuffer(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer);

	stagingBuffer.Map(vertexBufferSize);
	memcpy(stagingBuffer.m_Mapped, vertexDataStart, vertexBufferSize);
	stagingBuffer.Unmap();

	CreateAndAllocateBuffer(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertexBuffer);

	CopyBuffer(stagingBuffer.m_Buffer, vertexBuffer.m_Buffer, vertexBufferSize);
}

void VulkanRenderer::CreateIndexBuffers()
{
	CreateIndexBuffer(m_IndexBuffer_Simple, 0);
	CreateIndexBuffer(m_IndexBuffer_Color, 1);
}

void VulkanRenderer::CreateIndexBuffer(VulkanBuffer& indexBuffer, glm::uint shaderIndex)
{
	std::vector<uint> indices;

	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		RenderObject* renderObject = GetRenderObject(i);
		if (renderObject->shaderIndex == shaderIndex && renderObject->indexed)
		{
			renderObject->indexOffset = indices.size();
			indices.insert(indices.end(), renderObject->indices->begin(), renderObject->indices->end());
		}
	}

	if (indices.empty()) return;
	const size_t bufferSize = sizeof(indices[0]) * indices.size();

	VulkanBuffer stagingBuffer(m_Device);
	CreateAndAllocateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer);

	stagingBuffer.Map(bufferSize);
	memcpy(stagingBuffer.m_Mapped, indices.data(), bufferSize);
	stagingBuffer.Unmap();

	CreateAndAllocateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, indexBuffer);

	CopyBuffer(stagingBuffer.m_Buffer, indexBuffer.m_Buffer, bufferSize);
}

void VulkanRenderer::PrepareUniformBuffers()
{
	std::vector<glm::uint> shaderIndexCount(m_LoadedShaderCode.size(), 0); // Stores how many of each render object use each shader
	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		const glm::uint shaderIndex = m_RenderObjects[i]->shaderIndex;
		m_RenderObjects[i]->uniformBufferObjectIndex = shaderIndexCount[shaderIndex]++;
	}

	// One uniform buffer pair for each shader
	m_UniformBufferPairs.resize(m_LoadedShaderCode.size(), { m_Device });

	// Simple
	glm::uint shaderIndex = 0;
	m_UniformBufferPairs[shaderIndex].constantBufferData.elements = Uniform::Type(
		Uniform::Type::PROJECTION_MAT4 |
		Uniform::Type::VIEW_MAT4 |
		Uniform::Type::CAM_POS_VEC4 |
		Uniform::Type::LIGHT_DIR_VEC4 |
		Uniform::Type::AMBIENT_COLOR_VEC4 |
		Uniform::Type::SPECULAR_COLOR_VEC4 |
		Uniform::Type::USE_DIFFUSE_TEXTURE_INT |
		Uniform::Type::USE_NORMAL_TEXTURE_INT |
		Uniform::Type::USE_SPECULAR_TEXTURE_INT);

	m_UniformBufferPairs[shaderIndex].dynamicBufferData.elements = Uniform::Type(
		Uniform::Type::MODEL_MAT4 | 
		Uniform::Type::MODEL_INV_TRANSPOSE_MAT4);

	// Color
	shaderIndex = 1;
	m_UniformBufferPairs[shaderIndex].constantBufferData.elements = Uniform::Type(
		Uniform::Type::VIEW_PROJECTION_MAT4);

	m_UniformBufferPairs[shaderIndex].dynamicBufferData.elements = Uniform::Type(
		Uniform::Type::MODEL_MAT4);

	for (size_t i = 0; i < m_UniformBufferPairs.size(); i++)
	{
		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[i];

		const glm::uint dynamicBufferSize = Uniform::CalculateSize(uniformBufferPair.dynamicBufferData.elements);
		const glm::uint allocatedDynamicBufferSize = AllocateUniformBuffer(dynamicBufferSize, uniformBufferPair.dynamicBufferData);

		const glm::uint constantBufferSize = Uniform::CalculateSize(uniformBufferPair.constantBufferData.elements);
		const glm::uint allocatedConstantBufferSize = AllocateUniformBuffer(constantBufferSize, uniformBufferPair.constantBufferData);

		// TODO: JANK: This should be changed, no? Used to be just sizeof(m_UniformBufferData)
		//const size_t bufferSize = m_DynamicAlignment * m_RenderObjects.size();

		PrepareUniformBuffer(uniformBufferPair.dynamicBuffer, allocatedDynamicBufferSize, /* sizeof(m_UniformBufferData), */
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

		PrepareUniformBuffer(uniformBufferPair.constantBuffer, allocatedConstantBufferSize, /* sizeof(m_UniformBufferData), */
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
}

glm::uint VulkanRenderer::AllocateUniformBuffer(size_t dynamicDataSize, UniformBufferData& uniformBufferData)
{
	size_t uboAlignment = (size_t)m_PhysicalDeviceProperties.limits.minUniformBufferOffsetAlignment;
	glm::uint dynamicAllignment = 
		(dynamicDataSize / uboAlignment) * uboAlignment +
		((dynamicDataSize % uboAlignment) > 0 ? uboAlignment : 0);

	if (dynamicAllignment > m_DynamicAlignment)
	{
		m_DynamicAlignment = dynamicAllignment;
	}

	size_t dynamicBufferSize = m_DynamicAlignment;

	uniformBufferData.data = _aligned_malloc(dynamicBufferSize, m_DynamicAlignment);
	assert(uniformBufferData.data);

	uniformBufferData.size = dynamicBufferSize;

	return dynamicBufferSize;
}

void VulkanRenderer::PrepareUniformBuffer(VulkanBuffer& buffer, glm::uint bufferSize,
	VkBufferUsageFlags bufferUseageFlagBits, VkMemoryPropertyFlags memoryPropertyHostFlagBits)
{
	CreateAndAllocateBuffer(bufferSize, bufferUseageFlagBits, memoryPropertyHostFlagBits, buffer);

	VK_CHECK_RESULT(vkMapMemory(m_Device, buffer.m_Memory, 0, VK_WHOLE_SIZE, 0, &buffer.m_Mapped));
}

void VulkanRenderer::CreateDescriptorPool()
{
	// TODO: Use more accurate count here (plus extra for new objects?)
	const size_t descriptorSetCount = m_RenderObjects.size();

	std::array<VkDescriptorPoolSize, 5> poolSizes = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = descriptorSetCount;

	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	poolSizes[1].descriptorCount = descriptorSetCount;

	poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // Diffuse map
	poolSizes[2].descriptorCount = descriptorSetCount;

	poolSizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // Normal map
	poolSizes[3].descriptorCount = descriptorSetCount;

	poolSizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // Specular map
	poolSizes[4].descriptorCount = descriptorSetCount;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = poolSizes.size();
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = descriptorSetCount;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // Allow descriptor sets to be added/removed often

	VK_CHECK_RESULT(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, m_DescriptorPool.replace()));
}

void VulkanRenderer::CreateDescriptorSet(glm::uint renderID, glm::uint descriptorSetLayoutIndex)
{
	RenderObject* renderObject = GetRenderObject(renderID);

	VkDescriptorSetLayout layouts[] = { m_DescriptorSetLayouts[descriptorSetLayoutIndex] };
	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_DescriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = layouts;

	VK_CHECK_RESULT(vkAllocateDescriptorSets(m_Device, &allocInfo, &renderObject->descriptorSet));

	// TODO: Do this automatically using HasAttribute
	if (renderObject->diffuseTexture != nullptr)
	{
		std::array<VkWriteDescriptorSet, 5> writeDescriptorSets = {};

		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[renderObject->shaderIndex];

		VkDescriptorBufferInfo uniformBufferInfo = {};
		uniformBufferInfo.buffer = uniformBufferPair.constantBuffer.m_Buffer;
		uniformBufferInfo.range = Uniform::CalculateSize(uniformBufferPair.constantBufferData.elements);

		writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[0].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[0].dstBinding = 0;
		writeDescriptorSets[0].dstArrayElement = 0;
		writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writeDescriptorSets[0].descriptorCount = 1;
		writeDescriptorSets[0].pBufferInfo = &uniformBufferInfo;

		VkDescriptorBufferInfo uniformBufferDynamicInfo = {};
		uniformBufferDynamicInfo.buffer = uniformBufferPair.dynamicBuffer.m_Buffer;
		uniformBufferDynamicInfo.range = Uniform::CalculateSize(uniformBufferPair.dynamicBufferData.elements);

		writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[1].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[1].dstBinding = 1;
		writeDescriptorSets[1].dstArrayElement = 0;
		writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writeDescriptorSets[1].descriptorCount = 1;
		writeDescriptorSets[1].pBufferInfo = &uniformBufferDynamicInfo;

		VkDescriptorImageInfo diffuseImageInfo = {};
		diffuseImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		diffuseImageInfo.imageView = renderObject->diffuseTexture->imageView;
		diffuseImageInfo.sampler = renderObject->diffuseTexture->sampler;

		writeDescriptorSets[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[2].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[2].dstBinding = 2;
		writeDescriptorSets[2].dstArrayElement = 0;
		writeDescriptorSets[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writeDescriptorSets[2].descriptorCount = 1;
		writeDescriptorSets[2].pImageInfo = &diffuseImageInfo;

		VkDescriptorImageInfo normalImageInfo = {};
		normalImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		normalImageInfo.imageView = renderObject->normalTexture->imageView;
		normalImageInfo.sampler = renderObject->normalTexture->sampler;

		writeDescriptorSets[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[3].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[3].dstBinding = 3;
		writeDescriptorSets[3].dstArrayElement = 0;
		writeDescriptorSets[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writeDescriptorSets[3].descriptorCount = 1;
		writeDescriptorSets[3].pImageInfo = &normalImageInfo;

		VkDescriptorImageInfo specularImageInfo = {};
		specularImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		specularImageInfo.imageView = renderObject->specularTexture->imageView;
		specularImageInfo.sampler = renderObject->specularTexture->sampler;

		writeDescriptorSets[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[4].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[4].dstBinding = 4;
		writeDescriptorSets[4].dstArrayElement = 0;
		writeDescriptorSets[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writeDescriptorSets[4].descriptorCount = 1;
		writeDescriptorSets[4].pImageInfo = &specularImageInfo;

		vkUpdateDescriptorSets(m_Device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);
	}
	else
	{
		std::array<VkWriteDescriptorSet, 2> writeDescriptorSets = {};

		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[renderObject->shaderIndex];

		VkDescriptorBufferInfo uniformBufferInfo = {};
		uniformBufferInfo.buffer = uniformBufferPair.constantBuffer.m_Buffer;
		uniformBufferInfo.range = Uniform::CalculateSize(uniformBufferPair.constantBufferData.elements);

		writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[0].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[0].dstBinding = 0;
		writeDescriptorSets[0].dstArrayElement = 0;
		writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writeDescriptorSets[0].descriptorCount = 1;
		writeDescriptorSets[0].pBufferInfo = &uniformBufferInfo;

		VkDescriptorBufferInfo uniformBufferDynamicInfo = {};
		uniformBufferDynamicInfo.buffer = uniformBufferPair.dynamicBuffer.m_Buffer;
		uniformBufferDynamicInfo.range = Uniform::CalculateSize(uniformBufferPair.dynamicBufferData.elements);

		writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSets[1].dstSet = renderObject->descriptorSet;
		writeDescriptorSets[1].dstBinding = 1;
		writeDescriptorSets[1].dstArrayElement = 0;
		writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writeDescriptorSets[1].descriptorCount = 1;
		writeDescriptorSets[1].pBufferInfo = &uniformBufferDynamicInfo;

		vkUpdateDescriptorSets(m_Device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);
	}
	//else
	//{
	//	Logger::LogError("Unhandled descriptor set layout index " + std::to_string(descriptorSetLayoutIndex));
	//}

}

void VulkanRenderer::CreateDescriptorSetLayout(glm::uint type)
{
	m_DescriptorSetLayouts.push_back(VkDescriptorSetLayout());
	VkDescriptorSetLayout* descriptorSetLayout = &m_DescriptorSetLayouts.back();

	//const size_t descriptorCount = m_RenderObjects.size();

	if (type == 0)
	{
		VkDescriptorSetLayoutBinding uniformBufferBinding = {};
		uniformBufferBinding.binding = 0;
		uniformBufferBinding.descriptorCount = 1;
		uniformBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		uniformBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutBinding uniformBufferDynamicBinding = {};
		uniformBufferDynamicBinding.binding = 1;
		uniformBufferDynamicBinding.descriptorCount = 1;
		uniformBufferDynamicBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		uniformBufferDynamicBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		VkDescriptorSetLayoutBinding diffuseSamplerLayoutBinding = {};
		diffuseSamplerLayoutBinding.binding = 2;
		diffuseSamplerLayoutBinding.descriptorCount = 1;
		diffuseSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		diffuseSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutBinding normalSamplerLayoutBinding = {};
		normalSamplerLayoutBinding.binding = 3;
		normalSamplerLayoutBinding.descriptorCount = 1;
		normalSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		normalSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutBinding specularSamplerLayoutBinding = {};
		specularSamplerLayoutBinding.binding = 4;
		specularSamplerLayoutBinding.descriptorCount = 1;
		specularSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		specularSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		std::array<VkDescriptorSetLayoutBinding, 5> bindings = {
			uniformBufferBinding,
			uniformBufferDynamicBinding,
			diffuseSamplerLayoutBinding,
			normalSamplerLayoutBinding,
			specularSamplerLayoutBinding
		};

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = bindings.size();
		layoutInfo.pBindings = bindings.data();

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, descriptorSetLayout));
	}
	else if (type == 1)
	{
		VkDescriptorSetLayoutBinding uniformBufferBinding = {};
		uniformBufferBinding.binding = 0;
		uniformBufferBinding.descriptorCount = 1;
		uniformBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		uniformBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		VkDescriptorSetLayoutBinding uniformBufferDynamicBinding = {};
		uniformBufferDynamicBinding.binding = 1;
		uniformBufferDynamicBinding.descriptorCount = 1;
		uniformBufferDynamicBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		uniformBufferDynamicBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
			uniformBufferBinding,
			uniformBufferDynamicBinding
		};

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = bindings.size();
		layoutInfo.pBindings = bindings.data();

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, descriptorSetLayout));
	}
	else
	{
		Logger::LogError("Unhandled descriptor set layout type: " + std::to_string(type));
	}
}

void VulkanRenderer::CreateSemaphores()
{
	VkSemaphoreCreateInfo semaphoreInfo = {};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VK_CHECK_RESULT(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, m_ImageAvailableSemaphore.replace()));
	VK_CHECK_RESULT(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, m_RenderFinishedSemaphore.replace()));
}

void VulkanRenderer::DrawFrame(Window* window)
{
	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(m_Device, m_SwapChain, std::numeric_limits<uint64_t>::max(), m_ImageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		RecreateSwapChain(window);
		return;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphore };
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_CommandBuffers[imageIndex];

	VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphore };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	VK_CHECK_RESULT(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));

	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = { m_SwapChain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;

	presentInfo.pImageIndices = &imageIndex;

	result = vkQueuePresentKHR(m_PresentQueue, &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
	{
		RecreateSwapChain(window);
	}
	else 
	{
		VK_CHECK_RESULT(result);
	}

	VK_CHECK_RESULT(vkQueueWaitIdle(m_PresentQueue));
}

void VulkanRenderer::CreateShaderModule(const std::vector<char>& code, VDeleter<VkShaderModule>& shaderModule)
{
	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = (uint32_t*)code.data();

	VK_CHECK_RESULT(vkCreateShaderModule(m_Device, &createInfo, nullptr, shaderModule.replace()));
}

VkSurfaceFormatKHR VulkanRenderer::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
{
	if (availableFormats.size() == 1 && availableFormats[0].format == VK_FORMAT_UNDEFINED)
	{
		return{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	}

	for (const auto& availableFormat : availableFormats)
	{
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}

	return availableFormats[0];
}

VkPresentModeKHR VulkanRenderer::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> availablePresentModes)
{
	VkPresentModeKHR bestMode = VK_PRESENT_MODE_FIFO_KHR;
	if (m_VSyncEnabled) bestMode = VK_PRESENT_MODE_MAILBOX_KHR;

	VkPresentModeKHR secondBestMode = bestMode;

	for (const auto& availablePresentMode : availablePresentModes)
	{
		if (availablePresentMode == bestMode) return availablePresentMode;

		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			secondBestMode = availablePresentMode;
		}
		else if (availablePresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			secondBestMode = availablePresentMode;
		}
	}

	return secondBestMode;
}

VkExtent2D VulkanRenderer::ChooseSwapExtent(Window* window, const VkSurfaceCapabilitiesKHR& capabilities)
{
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
	{
		return capabilities.currentExtent;
	}
	else
	{
		int width, height;
		glfwGetWindowSize(((VulkanWindowWrapper*)window)->GetWindow(), &width, &height);

		VkExtent2D actualExtent = { (uint32_t)width, (uint32_t)height };

		actualExtent.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, actualExtent.width));
		actualExtent.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, actualExtent.height));

		return actualExtent;
	}
}

SwapChainSupportDetails VulkanRenderer::QuerySwapChainSupport(VkPhysicalDevice device)
{
	SwapChainSupportDetails details;

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_Surface, &details.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_Surface, &formatCount, nullptr);

	if (formatCount != 0)
	{
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_Surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_Surface, &presentModeCount, nullptr);

	if (presentModeCount != 0)
	{
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_Surface, &presentModeCount, details.presentModes.data());
	}

	return details;
}

bool VulkanRenderer::IsDeviceSuitable(VkPhysicalDevice device)
{
	QueueFamilyIndices indices = FindQueueFamilies(device);

	bool extensionsSupported = CheckDeviceExtensionSupport(device);

	bool swapChainAdequate = false;
	if (extensionsSupported)
	{
		SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
		swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	VkPhysicalDeviceFeatures supportedFeatures;
	vkGetPhysicalDeviceFeatures(device, &supportedFeatures);
	
	return indices.IsComplete() && extensionsSupported && swapChainAdequate && supportedFeatures.samplerAnisotropy;
}

bool VulkanRenderer::CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

	std::set<std::string> requiredExtensions(m_DeviceExtensions.begin(), m_DeviceExtensions.end());

	for (const auto& extension : availableExtensions)
	{
		requiredExtensions.erase(extension.extensionName);
	}

	return requiredExtensions.empty();
}

QueueFamilyIndices VulkanRenderer::FindQueueFamilies(VkPhysicalDevice device)
{
	QueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	int i = 0;
	for (const auto& queueFamily : queueFamilies)
	{
		if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			indices.graphicsFamily = i;
		}

		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_Surface, &presentSupport);

		if (queueFamily.queueCount > 0 && presentSupport)
		{
			indices.presentFamily = i;
		}

		if (indices.IsComplete())
		{
			break;
		}

		i++;
	}

	return indices;
}

std::vector<const char*> VulkanRenderer::GetRequiredExtensions()
{
	std::vector<const char*> extensions;

	unsigned int glfwExtensionCount = 0;
	const char** glfwExtensions;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	for (unsigned int i = 0; i < glfwExtensionCount; i++)
	{
		extensions.push_back(glfwExtensions[i]);
	}

	if (m_EnableValidationLayers)
	{
		extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
	}

	return extensions;
}

bool VulkanRenderer::CheckValidationLayerSupport()
{
	uint32_t layerCount;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

	std::vector<VkLayerProperties> availableLayers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

	for (const char* layerName : m_ValidationLayers)
	{
		bool layerFound = false;

		for (const auto& layerProperties : availableLayers)
		{
			if (strcmp(layerName, layerProperties.layerName) == 0)
			{
				layerFound = true;
				break;
			}
		}

		if (!layerFound)
		{
			return false;
		}
	}

	return true;
}

void VulkanRenderer::UpdateConstantUniformBuffers(const GameContext& gameContext)
{
	glm::mat4 proj = gameContext.camera->GetProjection();
	glm::mat4 view = gameContext.camera->GetView();
	glm::mat4 viewInv = glm::inverse(view);
	glm::mat4 viewProjection = view * proj;
	glm::vec4 camPos = glm::vec4(gameContext.camera->GetPosition(), 0.0f);
	glm::vec4 viewDir = glm::vec4(gameContext.camera->GetViewDirection(), 0.0f);

	for (size_t i = 0; i < m_UniformBufferPairs.size(); i++)
	{
		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[i];
	
		float* dataStart = (float*)uniformBufferPair.constantBufferData.data;
		float* data = dataStart;
		 
		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::PROJECTION_MAT4))
		{
			*((glm::mat4*)data) = proj;
			data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::VIEW_MAT4))
		{
			*((glm::mat4*)data) = view;
			data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::VIEW_INV_MAT4))
		{
			*((glm::mat4*)data) = viewInv;
			data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::VIEW_PROJECTION_MAT4))
		{
			*((glm::mat4*)data) = viewProjection;
			data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::MODEL_MAT4))
		{
			Logger::LogError("Constant uniform buffer contains model matrix, which should be in the dynamic uniform buffer");
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::MODEL_INV_TRANSPOSE_MAT4))
		{
			Logger::LogError("Constant uniform buffer contains modelInvTranspose matrix, which should be in the dynamic uniform buffer");
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::MODEL_VIEW_PROJECTION_MAT4))
		{
			*((glm::mat4*)data) = view;
			data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::CAM_POS_VEC4))
		{
			*((glm::vec4*)data) = camPos;
			data += 4;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::VIEW_DIR_VEC4))
		{
			*((glm::vec4*)data) = viewDir;
			data += 4;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::LIGHT_DIR_VEC4))
		{
			*((glm::vec4*)data) = m_SceneInfo.m_LightDir;
			data += 4;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::AMBIENT_COLOR_VEC4))
		{
			*((glm::vec4*)data) = m_SceneInfo.m_AmbientColor;
			data += 4;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::SPECULAR_COLOR_VEC4))
		{
			*((glm::vec4*)data) = m_SceneInfo.m_SpecularColor;
			data += 4;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::USE_DIFFUSE_TEXTURE_INT))
		{
			*((glm::int32*)data) = 1;
			data += 1;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::USE_NORMAL_TEXTURE_INT))
		{
			*((glm::int32*)data) = 1;
			data += 1;
		}

		if (Uniform::HasUniform(uniformBufferPair.constantBufferData.elements, Uniform::Type::USE_SPECULAR_TEXTURE_INT))
		{
			*((glm::int32*)data) = 1;
			data += 1;
		}

		glm::uint countedSize = sizeof(float) * (data - dataStart);
		glm::uint calculatedSize = Uniform::CalculateSize(uniformBufferPair.constantBufferData.elements);
	
		if (countedSize != calculatedSize) Logger::LogWarning("Counted size does not equal calculated size! " + 
			std::to_string(countedSize) + " != " + std::to_string(calculatedSize));

		glm::uint size = uniformBufferPair.constantBufferData.size;

		memcpy(uniformBufferPair.constantBuffer.m_Mapped, 
			&uniformBufferPair.constantBufferData.data,
			size);
	}
}

void VulkanRenderer::UpdateUniformBufferDynamic(const GameContext& gameContext, glm::uint renderID, const glm::mat4& model)
{
	glm::mat4 modelInvTranspose = glm::transpose(glm::inverse(model));

	// Starts pointing at the start of each dynamic uniform buffer, then gets incremented
	// as we loop through the render objects and stores where to store the next bytes
	std::vector<float*> dynamicUniformBufferDataPtrs;

	const size_t uniformCount = m_UniformBufferPairs.size();
	dynamicUniformBufferDataPtrs.reserve(uniformCount);
	for (size_t i = 0; i < uniformCount; i++)
	{
		dynamicUniformBufferDataPtrs.push_back((float*)m_UniformBufferPairs[i].dynamicBufferData.data);
	}

	for (size_t i = 0; i < m_RenderObjects.size(); i++)
	{
		RenderObject* renderObject = m_RenderObjects[i];

		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[renderObject->shaderIndex];

		float** data = &dynamicUniformBufferDataPtrs[renderObject->shaderIndex];

		if (Uniform::HasUniform(uniformBufferPair.dynamicBufferData.elements, Uniform::Type::MODEL_MAT4))
		{
			*((glm::mat4*)*data) = model;
			*data += 16;
		}

		if (Uniform::HasUniform(uniformBufferPair.dynamicBufferData.elements, Uniform::Type::MODEL_INV_TRANSPOSE_MAT4))
		{
			*((glm::mat4*)*data) = modelInvTranspose;
			*data += 16;
		}
	}

	for (size_t i = 0; i < m_UniformBufferPairs.size(); i++)
	{
		UniformBufferPair& uniformBufferPair = m_UniformBufferPairs[i];

		// Aligned offset
		glm::uint calculatedSize = Uniform::CalculateSize(uniformBufferPair.dynamicBufferData.elements);
		size_t size = calculatedSize;// *m_RenderObjects.size();
		void* firstIndex = uniformBufferPair.dynamicBuffer.m_Mapped;
		uint64_t dest = (uint64_t)firstIndex + (renderID * m_DynamicAlignment); // TODO: Use seperate index into this specific buffer
		memcpy((void*)(dest), &uniformBufferPair.dynamicBufferData.data, size);

		// Flush to make changes visible to the host
		VkMappedMemoryRange mappedMemoryRange = {};
		mappedMemoryRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		mappedMemoryRange.memory = uniformBufferPair.dynamicBuffer.m_Memory;
		mappedMemoryRange.size = size;
		vkFlushMappedMemoryRanges(m_Device, 1, &mappedMemoryRange);
	}
}

void VulkanRenderer::LoadDefaultShaderCode()
{
	const std::string shaderDirectory = "resources/shaders/GLSL/spv/";

	m_ShaderFilePaths = { 
		{ shaderDirectory + "vk_simple_vert.spv", shaderDirectory + "vk_simple_frag.spv" },
		{ shaderDirectory + "vk_color_vert.spv", shaderDirectory + "vk_color_frag.spv" },
	};

	const size_t shaderCount = m_ShaderFilePaths.size();
	m_LoadedShaderCode.resize(shaderCount);
	for (size_t i = 0; i < shaderCount; i++)
	{
		m_LoadedShaderCode[i].vertexShaderCode = ReadFile(m_ShaderFilePaths[i].vertexShaderFilePath);
		m_LoadedShaderCode[i].fragmentShaderCode = ReadFile(m_ShaderFilePaths[i].fragmentShaderFilePath);
	}
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::DebugCallback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objType,
	uint64_t obj, size_t location, int32_t code, const char* layerPrefix, const char* msg, void* userData)
{
	std::cerr << "validation layer: " << msg << std::endl;

	return VK_FALSE;
}

VkPrimitiveTopology VulkanRenderer::TopologyModeToVkPrimitiveTopology(TopologyMode mode)
{
	switch (mode)
	{
	case TopologyMode::POINT_LIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case TopologyMode::LINE_LIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	//case TopologyMode::LINE_LOOP: return VkPrimitiveTopology::VK_PRIMITIVE_TOPOLOGY_LINE_LOOP; // Unsupported
	case TopologyMode::LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case TopologyMode::TRIANGLE_LIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case TopologyMode::TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	case TopologyMode::TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
	}
}

#endif // COMPILE_VULKAN
