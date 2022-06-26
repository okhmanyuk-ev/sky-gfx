#include "backend_vk.h"

#ifdef SKYGFX_HAS_VULKAN

#include <vulkan/vulkan_raii.hpp>

using namespace skygfx;

static vk::raii::Context mContext;
static vk::raii::Instance mInstance = nullptr;
static vk::raii::PhysicalDevice mPhysicalDevice = nullptr;
static vk::raii::Queue mQueue = nullptr;
static vk::raii::Device mDevice = nullptr;
static uint32_t mQueueFamilyIndex = -1;
static vk::SurfaceFormatKHR mSurfaceFormat;
static vk::raii::SurfaceKHR mSurface = nullptr;
static vk::raii::SwapchainKHR mSwapchain = nullptr;
static vk::raii::CommandPool mCommandPool = nullptr;
static vk::raii::CommandBuffer mCommandBuffer = nullptr;
static vk::raii::Sampler mSampler = nullptr;
static bool mWorking = false;
static uint32_t mWidth = 0;
static uint32_t mHeight = 0;
static uint32_t mMinImageCount = 2; // TODO: https://github.com/nvpro-samples/nvpro_core/blob/f2c05e161bba9ab9a8c96c0173bf0edf7c168dfa/nvvk/swapchain_vk.cpp#L143

struct FrameVK
{
	vk::raii::CommandBuffer command_buffer = nullptr;
	vk::raii::Fence fence = nullptr;
	vk::raii::ImageView backbuffer_view = nullptr;
	vk::raii::Semaphore image_acquired_semaphore = nullptr;
	vk::raii::Semaphore render_complete_semaphore = nullptr;
};

static std::vector<FrameVK> mFrames;

static int mVertexBufferIndex = 0;
static int mIndexBufferIndex = 0;
static int mUniformBufferIndex = 0;

static uint32_t mSemaphoreIndex = 0;
static uint32_t mFrameIndex = 0;

struct DeviceBufferVK
{
	vk::raii::Buffer buffer = nullptr;
	vk::raii::DeviceMemory memory = nullptr;
	vk::DeviceSize size = 0;
};

static std::vector<DeviceBufferVK> mVertexBuffers;
static std::vector<DeviceBufferVK> mIndexBuffers;
static std::vector<DeviceBufferVK> mUniformBuffers;

static std::unordered_map<int, vk::ImageView> mTexturesPushQueue;
static std::unordered_map<int, vk::Buffer> mUniformBuffersPushQueue;

class ShaderDataVK;

static ShaderDataVK* mShader = nullptr;
static std::unordered_map<ShaderDataVK*, vk::raii::Pipeline> mPipelines;

static uint32_t GetMemoryType(vk::MemoryPropertyFlags properties, uint32_t type_bits, vk::raii::PhysicalDevice& physical_device)
{
	auto prop = physical_device.getMemoryProperties();

	for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
		if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1 << i))
			return i;

	return 0xFFFFFFFF; // Unable to find memoryType
}

template <typename Func>
static void oneTimeSubmit(vk::raii::CommandBuffer const& commandBuffer, vk::raii::Queue const& queue, Func const& func)
{
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	func(commandBuffer);
	commandBuffer.end();
	vk::SubmitInfo submitInfo(nullptr, nullptr, *commandBuffer);
	queue.submit(submitInfo, nullptr);
	queue.waitIdle();
}

template <typename Func>
static void oneTimeSubmit(vk::raii::Device const& device, vk::raii::CommandPool const& commandPool, vk::raii::Queue const& queue, Func const& func)
{
	vk::raii::CommandBuffers commandBuffers(device, { *commandPool, vk::CommandBufferLevel::ePrimary, 1 });
	oneTimeSubmit(commandBuffers.front(), queue, func);
}

static void setImageLayout(vk::raii::CommandBuffer const& commandBuffer, vk::Image image,
	vk::Format format, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout)
{
	vk::AccessFlags sourceAccessMask;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eTransferDstOptimal: sourceAccessMask = vk::AccessFlagBits::eTransferWrite; break;
	case vk::ImageLayout::ePreinitialized: sourceAccessMask = vk::AccessFlagBits::eHostWrite; break;
	case vk::ImageLayout::eGeneral:  // sourceAccessMask is empty
	case vk::ImageLayout::eUndefined: break;
	default: assert(false); break;
	}

	vk::PipelineStageFlags sourceStage;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePreinitialized: sourceStage = vk::PipelineStageFlagBits::eHost; break;
	case vk::ImageLayout::eTransferDstOptimal: sourceStage = vk::PipelineStageFlagBits::eTransfer; break;
	case vk::ImageLayout::eUndefined: sourceStage = vk::PipelineStageFlagBits::eTopOfPipe; break;
	default: assert(false); break;
	}

	vk::AccessFlags destinationAccessMask;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal: destinationAccessMask = vk::AccessFlagBits::eColorAttachmentWrite; break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		break;
	case vk::ImageLayout::eGeneral:  // empty destinationAccessMask
	case vk::ImageLayout::ePresentSrcKHR: break;
	case vk::ImageLayout::eShaderReadOnlyOptimal: destinationAccessMask = vk::AccessFlagBits::eShaderRead; break;
	case vk::ImageLayout::eTransferSrcOptimal: destinationAccessMask = vk::AccessFlagBits::eTransferRead; break;
	case vk::ImageLayout::eTransferDstOptimal: destinationAccessMask = vk::AccessFlagBits::eTransferWrite; break;
	default: assert(false); break;
	}

	vk::PipelineStageFlags destinationStage;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal: destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput; break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal: destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests; break;
	case vk::ImageLayout::eGeneral: destinationStage = vk::PipelineStageFlagBits::eHost; break;
	case vk::ImageLayout::ePresentSrcKHR: destinationStage = vk::PipelineStageFlagBits::eBottomOfPipe; break;
	case vk::ImageLayout::eShaderReadOnlyOptimal: destinationStage = vk::PipelineStageFlagBits::eFragmentShader; break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal: destinationStage = vk::PipelineStageFlagBits::eTransfer; break;
	default: assert(false); break;
	}

	vk::ImageAspectFlags aspectMask;
	if (newImageLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
	{
		aspectMask = vk::ImageAspectFlagBits::eDepth;
		if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint)
		{
			aspectMask |= vk::ImageAspectFlagBits::eStencil;
		}
	}
	else
	{
		aspectMask = vk::ImageAspectFlagBits::eColor;
	}

	vk::ImageSubresourceRange imageSubresourceRange(aspectMask, 0, 1, 0, 1);
	vk::ImageMemoryBarrier    imageMemoryBarrier(sourceAccessMask,
		destinationAccessMask,
		oldImageLayout,
		newImageLayout,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		image,
		imageSubresourceRange);
	return commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, nullptr, nullptr, imageMemoryBarrier);
}

const static std::unordered_map<ComparisonFunc, vk::CompareOp> CompareOpMap = {
	{ ComparisonFunc::Always, vk::CompareOp::eAlways },
	{ ComparisonFunc::Never, vk::CompareOp::eNever },
	{ ComparisonFunc::Less, vk::CompareOp::eLess },
	{ ComparisonFunc::Equal, vk::CompareOp::eEqual },
	{ ComparisonFunc::NotEqual, vk::CompareOp::eNotEqual },
	{ ComparisonFunc::LessEqual, vk::CompareOp::eLessOrEqual },
	{ ComparisonFunc::Greater, vk::CompareOp::eGreater },
	{ ComparisonFunc::GreaterEqual, vk::CompareOp::eGreaterOrEqual }
};

class ShaderDataVK
{
	friend class BackendVK;

private:
	vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
	vk::raii::PipelineLayout pipeline_layout = nullptr;
	vk::raii::ShaderModule vertex_shader_module = nullptr;
	vk::raii::ShaderModule fragment_shader_module = nullptr;
	vk::VertexInputBindingDescription vertex_input_binding_description;
	std::vector<vk::VertexInputAttributeDescription> vertex_input_attribute_descriptions;

public:
	ShaderDataVK(const Vertex::Layout& layout, const std::string& vertex_code, const std::string& fragment_code,
		std::vector<std::string> defines)
	{
		AddShaderLocationDefines(layout, defines);

		auto vertex_shader_spirv = CompileGlslToSpirv(ShaderStage::Vertex, vertex_code, defines);
		auto fragment_shader_spirv = CompileGlslToSpirv(ShaderStage::Fragment, fragment_code, defines);

		auto vertex_shader_reflection = MakeSpirvReflection(vertex_shader_spirv);
		auto fragment_shader_reflection = MakeSpirvReflection(fragment_shader_spirv);

		static const std::unordered_map<ShaderStage, vk::ShaderStageFlagBits> StageMap = {
			{ ShaderStage::Vertex, vk::ShaderStageFlagBits::eVertex },
			{ ShaderStage::Fragment, vk::ShaderStageFlagBits::eFragment }
		};

		static const std::unordered_map<ShaderReflection::DescriptorSet::Type, vk::DescriptorType> TypeMap = {
			{ ShaderReflection::DescriptorSet::Type::CombinedImageSampler, vk::DescriptorType::eCombinedImageSampler },
			{ ShaderReflection::DescriptorSet::Type::UniformBuffer, vk::DescriptorType::eUniformBuffer }
		};
		
		std::vector<vk::DescriptorSetLayoutBinding> bindings;

		for (const auto& reflection : { vertex_shader_reflection, fragment_shader_reflection })
		{
			for (const auto& descriptor_set : reflection.descriptor_sets)
			{
				bool overwritten = false;

				for (auto& binding : bindings)
				{
					if (binding.binding == descriptor_set.binding)
					{
						binding.stageFlags |= StageMap.at(reflection.stage);
						overwritten = true;
						break;
					}
				}

				if (overwritten)
					continue;

				auto binding = vk::DescriptorSetLayoutBinding()
					.setDescriptorType(TypeMap.at(descriptor_set.type))
					.setDescriptorCount(1)
					.setBinding(descriptor_set.binding)
					.setStageFlags(StageMap.at(reflection.stage));

				bindings.push_back(binding);
			}
		}

		auto descriptor_set_layout_create_info = vk::DescriptorSetLayoutCreateInfo()
			.setFlags(vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR)
			.setBindings(bindings);

		descriptor_set_layout = mDevice.createDescriptorSetLayout(descriptor_set_layout_create_info);

		auto pipeline_layout_create_info = vk::PipelineLayoutCreateInfo()
			.setSetLayoutCount(1)
			.setPSetLayouts(&*descriptor_set_layout);

		pipeline_layout = mDevice.createPipelineLayout(pipeline_layout_create_info);

		auto vertex_shader_module_create_info = vk::ShaderModuleCreateInfo()
			.setCode(vertex_shader_spirv);

		auto fragment_shader_module_create_info = vk::ShaderModuleCreateInfo()
			.setCode(fragment_shader_spirv);

		vertex_shader_module = mDevice.createShaderModule(vertex_shader_module_create_info);
		fragment_shader_module = mDevice.createShaderModule(fragment_shader_module_create_info);

		static const std::unordered_map<Vertex::Attribute::Format, vk::Format> Format = {
			{ Vertex::Attribute::Format::R32F, vk::Format::eR32Sfloat },
			{ Vertex::Attribute::Format::R32G32F, vk::Format::eR32G32Sfloat },
			{ Vertex::Attribute::Format::R32G32B32F, vk::Format::eR32G32B32Sfloat },
			{ Vertex::Attribute::Format::R32G32B32A32F, vk::Format::eR32G32B32A32Sfloat },
			{ Vertex::Attribute::Format::R8UN, vk::Format::eR8Unorm },
			{ Vertex::Attribute::Format::R8G8UN, vk::Format::eR8G8Unorm },
			{ Vertex::Attribute::Format::R8G8B8UN, vk::Format::eR8G8B8Unorm },
			{ Vertex::Attribute::Format::R8G8B8A8UN, vk::Format::eR8G8B8A8Unorm }
		};

		vertex_input_binding_description = vk::VertexInputBindingDescription()
			.setStride(static_cast<uint32_t>(layout.stride))
			.setInputRate(vk::VertexInputRate::eVertex)
			.setBinding(0);

		for (int i = 0; i < layout.attributes.size(); i++)
		{
			const auto& attrib = layout.attributes.at(i);

			auto vertex_input_attribute_description = vk::VertexInputAttributeDescription()
				.setBinding(0)
				.setLocation(i)
				.setFormat(Format.at(attrib.format))
				.setOffset(static_cast<uint32_t>(attrib.offset));

			vertex_input_attribute_descriptions.push_back(vertex_input_attribute_description);
		}
	}
};

class TextureDataVK
{
	friend class BackendVK;

private:
	vk::raii::Image image = nullptr;
	vk::raii::ImageView image_view = nullptr;
	vk::raii::DeviceMemory memory = nullptr;

public:
	TextureDataVK(uint32_t width, uint32_t height, uint32_t channels, void* data, bool mipmap)
	{
		auto image_create_info = vk::ImageCreateInfo()
			.setImageType(vk::ImageType::e2D)
			.setFormat(vk::Format::eR8G8B8A8Unorm)
			.setExtent({ width, height, 1 })
			.setMipLevels(1)
			.setArrayLayers(1)
			.setSamples(vk::SampleCountFlagBits::e1)
			.setTiling(vk::ImageTiling::eOptimal)
			.setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
			.setSharingMode(vk::SharingMode::eExclusive)
			.setInitialLayout(vk::ImageLayout::eUndefined);

		image = mDevice.createImage(image_create_info);

		auto memory_requirements = image.getMemoryRequirements();

		auto memory_allocate_info = vk::MemoryAllocateInfo()
			.setAllocationSize(memory_requirements.size)
			.setMemoryTypeIndex(GetMemoryType(vk::MemoryPropertyFlagBits::eDeviceLocal, 
				memory_requirements.memoryTypeBits, mPhysicalDevice));

		memory = mDevice.allocateMemory(memory_allocate_info);
	
		image.bindMemory(*memory, 0);

		auto image_subresource_range = vk::ImageSubresourceRange()
			.setAspectMask(vk::ImageAspectFlagBits::eColor)
			.setLevelCount(1)
			.setLayerCount(1);

		auto image_view_create_info = vk::ImageViewCreateInfo()
			.setImage(*image)
			.setViewType(vk::ImageViewType::e2D)
			.setFormat(vk::Format::eR8G8B8A8Unorm)
			.setSubresourceRange(image_subresource_range);

		image_view = mDevice.createImageView(image_view_create_info);

		if (data)
		{
			auto size = width * height * channels;

			auto buffer_create_info = vk::BufferCreateInfo()
				.setSize(size)
				.setUsage(vk::BufferUsageFlagBits::eTransferSrc)
				.setSharingMode(vk::SharingMode::eExclusive);

			auto upload_buffer = mDevice.createBuffer(buffer_create_info);

			auto req = upload_buffer.getMemoryRequirements();

			//bd->BufferMemoryAlignment = (bd->BufferMemoryAlignment > req.alignment) ? bd->BufferMemoryAlignment : req.alignment;

			auto memory_allocate_info = vk::MemoryAllocateInfo()
				.setAllocationSize(req.size)
				.setMemoryTypeIndex(GetMemoryType(vk::MemoryPropertyFlagBits::eHostVisible, req.memoryTypeBits, mPhysicalDevice));

			auto upload_buffer_memory = mDevice.allocateMemory(memory_allocate_info);

			upload_buffer.bindMemory(*upload_buffer_memory, 0);

			auto map = upload_buffer_memory.mapMemory(0, size);
			memcpy(map, data, size);
			upload_buffer_memory.unmapMemory();

			oneTimeSubmit(mDevice, mCommandPool, mQueue, [&](auto& cmd) {
				setImageLayout(cmd, *image, vk::Format::eUndefined, vk::ImageLayout::eUndefined,
					vk::ImageLayout::eTransferDstOptimal);

				auto image_subresource_layers = vk::ImageSubresourceLayers()
					.setAspectMask(vk::ImageAspectFlagBits::eColor)
					.setLayerCount(1);

				auto region = vk::BufferImageCopy()
					.setImageSubresource(image_subresource_layers)
					.setImageExtent({ width, height, 1 });

				cmd.copyBufferToImage(*upload_buffer, *image, vk::ImageLayout::eTransferDstOptimal, { region });

				setImageLayout(cmd, *image, vk::Format::eUndefined, vk::ImageLayout::eTransferDstOptimal,
					vk::ImageLayout::eShaderReadOnlyOptimal);
			});
		}
	}
};

class RenderTargetDataVK
{
public:
	RenderTargetDataVK(uint32_t width, uint32_t height, TextureDataVK* _texture_data)
	{
	}

	~RenderTargetDataVK()
	{
	}
};

BackendVK::BackendVK(void* window, uint32_t width, uint32_t height)
{
	auto all_extensions = mContext.enumerateInstanceExtensionProperties();

	for (auto extension : all_extensions)
	{
		//	std::cout << extension.extensionName << std::endl;
	}

	auto all_layers = mContext.enumerateInstanceLayerProperties();

	for (auto layer : all_layers)
	{
		//	std::cout << layer.layerName << std::endl;
	}

	auto extensions = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};

	auto layers = {
		"VK_LAYER_KHRONOS_validation"
	};

	auto version = mContext.enumerateInstanceVersion();

	auto major_version = VK_API_VERSION_MAJOR(version);
	auto minor_version = VK_API_VERSION_MINOR(version);

	//std::cout << "available vulkan version: " << major_version << "." << minor_version << std::endl;

	auto application_info = vk::ApplicationInfo()
		.setApiVersion(VK_API_VERSION_1_3);

	auto instance_info = vk::InstanceCreateInfo()
		.setPEnabledExtensionNames(extensions)
		.setPEnabledLayerNames(layers)
		.setPApplicationInfo(&application_info);

	mInstance = mContext.createInstance(instance_info);

	auto devices = mInstance.enumeratePhysicalDevices();
	size_t device_index = 0;
	for (size_t i = 0; i < devices.size(); i++)
	{
		auto properties = devices.at(i).getProperties();
		if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
		{
			device_index = i;
			break;
		}
	}

	mPhysicalDevice = std::move(devices.at(device_index));

	auto properties = mPhysicalDevice.getQueueFamilyProperties();

	for (size_t i = 0; i < properties.size(); i++)
	{
		if (properties[i].queueFlags & vk::QueueFlagBits::eGraphics)
		{
			mQueueFamilyIndex = static_cast<uint32_t>(i);
			break;
		}
	}

	auto all_device_extensions = mPhysicalDevice.enumerateDeviceExtensionProperties();

	for (auto device_extension : all_device_extensions)
	{
		//	std::cout << device_extension.extensionName << std::endl;
	}

	auto device_extensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
	};

	auto queue_priority = { 1.0f };

	auto queue_info = vk::DeviceQueueCreateInfo()
		.setQueueFamilyIndex(mQueueFamilyIndex)
		.setQueuePriorities(queue_priority);

	auto device_features = mPhysicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2,
		vk::PhysicalDeviceVulkan13Features>();

	//auto device_properties = mPhysicalDevice.getProperties2<vk::PhysicalDeviceProperties2, 
	//	vk::PhysicalDeviceVulkan13Properties>(); // TODO: unused

	auto device_info = vk::DeviceCreateInfo()
		.setQueueCreateInfoCount(1)
		.setPQueueCreateInfos(&queue_info)
		.setPEnabledExtensionNames(device_extensions)
		.setPEnabledFeatures(nullptr)
		.setPNext(&device_features.get<vk::PhysicalDeviceFeatures2>());

	mDevice = mPhysicalDevice.createDevice(device_info);

	mQueue = mDevice.getQueue(mQueueFamilyIndex, 0);

	auto surface_info = vk::Win32SurfaceCreateInfoKHR()
		.setHwnd((HWND)window);

	mSurface = vk::raii::SurfaceKHR(mInstance, surface_info);

	auto formats = mPhysicalDevice.getSurfaceFormatsKHR(*mSurface);

	if ((formats.size() == 1) && (formats.at(0).format == vk::Format::eUndefined))
	{
		mSurfaceFormat = {
			vk::Format::eB8G8R8A8Unorm,
			formats.at(0).colorSpace
		};
	}
	else
	{
		bool found = false;
		for (const auto& format : formats)
		{
			if (format.format == vk::Format::eB8G8R8A8Unorm)
			{
				mSurfaceFormat = format;
				found = true;
				break;
			}
		}
		if (!found)
		{
			mSurfaceFormat = formats.at(0);
		}
	}

	auto command_pool_info = vk::CommandPoolCreateInfo()
		.setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
		.setQueueFamilyIndex(mQueueFamilyIndex);

	mCommandPool = mDevice.createCommandPool(command_pool_info);

	auto command_buffer_allocate_info = vk::CommandBufferAllocateInfo()
		.setCommandBufferCount(1)
		.setLevel(vk::CommandBufferLevel::eSecondary)
		.setCommandPool(*mCommandPool);

	auto command_buffers = mDevice.allocateCommandBuffers(command_buffer_allocate_info);
	mCommandBuffer = std::move(command_buffers.at(0));

	auto sampler_create_info = vk::SamplerCreateInfo()
		.setMagFilter(vk::Filter::eLinear)
		.setMinFilter(vk::Filter::eLinear)
		.setMipmapMode(vk::SamplerMipmapMode::eLinear)
		.setAddressModeU(vk::SamplerAddressMode::eRepeat)
		.setAddressModeV(vk::SamplerAddressMode::eRepeat)
		.setAddressModeW(vk::SamplerAddressMode::eRepeat)
		.setMinLod(-1000)
		.setMaxLod(1000)
		.setMaxAnisotropy(1.0f);

	mSampler = mDevice.createSampler(sampler_create_info);

	createSwapchain(width, height);

	begin();
}

BackendVK::~BackendVK()
{
	end();
}

void BackendVK::resize(uint32_t width, uint32_t height)
{
}

void BackendVK::setTopology(Topology topology)
{
	static const std::unordered_map<Topology, vk::PrimitiveTopology> TopologyMap = {
		{ Topology::PointList, vk::PrimitiveTopology::ePointList },
		{ Topology::LineList, vk::PrimitiveTopology::eLineList },
		{ Topology::LineStrip, vk::PrimitiveTopology::eLineStrip },
		{ Topology::TriangleList, vk::PrimitiveTopology::eTriangleList },
		{ Topology::TriangleStrip, vk::PrimitiveTopology::eTriangleStrip },
	};

	mCommandBuffer.setPrimitiveTopology(TopologyMap.at(topology));
}

void BackendVK::setViewport(const Viewport& value)
{
	auto viewport = vk::Viewport()
		.setX(value.position.x)
		.setY(value.size.y - value.position.y)
		.setWidth(value.size.x)
		.setHeight(-value.size.y)
		.setMinDepth(value.min_depth)
		.setMaxDepth(value.max_depth);

	mCommandBuffer.setViewport(0, { viewport });
}

void BackendVK::setScissor(const Scissor& value)
{
	auto rect = vk::Rect2D()
		.setOffset({ static_cast<int32_t>(value.position.x), static_cast<int32_t>(value.position.y) })
		.setExtent({ static_cast<uint32_t>(value.size.x), static_cast<uint32_t>(value.size.y) });

	mCommandBuffer.setScissor(0, { rect });
}

void BackendVK::setScissor(std::nullptr_t value)
{
	setScissor({ { 0.0f, 0.0f }, { static_cast<float>(mWidth), static_cast<float>(mHeight) } });
}

void BackendVK::setTexture(TextureHandle* handle)
{
	auto texture = (TextureDataVK*)handle;
	int binding = 0;
	mTexturesPushQueue[binding] = *texture->image_view;
}

void BackendVK::setRenderTarget(RenderTargetHandle* handle)
{
}

void BackendVK::setRenderTarget(std::nullptr_t value)
{
}

void BackendVK::setShader(ShaderHandle* handle)
{
	mShader = (ShaderDataVK*)handle;
}

void BackendVK::setVertexBuffer(const Buffer& value)
{
	assert(value.size > 0);

	auto create_buffer = [&] {
		DeviceBufferVK buffer;

		auto buffer_create_info = vk::BufferCreateInfo()
			.setSize(value.size)
			.setUsage(vk::BufferUsageFlagBits::eVertexBuffer)
			.setSharingMode(vk::SharingMode::eExclusive);

		buffer.buffer = mDevice.createBuffer(buffer_create_info);

		auto memory_requirements = buffer.buffer.getMemoryRequirements();

		auto memory_allocate_info = vk::MemoryAllocateInfo()
			.setAllocationSize(memory_requirements.size)
			.setMemoryTypeIndex(GetMemoryType(vk::MemoryPropertyFlagBits::eHostVisible, memory_requirements.memoryTypeBits, mPhysicalDevice));

		buffer.memory = mDevice.allocateMemory(memory_allocate_info);
		buffer.size = memory_requirements.size;

		buffer.buffer.bindMemory(*buffer.memory, 0);

		return buffer;
	};

	if (mVertexBuffers.size() < mVertexBufferIndex + 1)
	{
		assert(mVertexBuffers.size() == mVertexBufferIndex);
		mVertexBuffers.push_back(create_buffer());
	}

	auto& buffer = mVertexBuffers[mVertexBufferIndex];

	if (buffer.size < value.size)
	{
		buffer = create_buffer();
	}

	auto mem = buffer.memory.mapMemory(0, VK_WHOLE_SIZE);
	memcpy(mem, value.data, value.size);
	buffer.memory.unmapMemory();

	mCommandBuffer.bindVertexBuffers2(0, { *buffer.buffer }, { 0 }, nullptr, { value.stride });

	mVertexBufferIndex += 1;
}

void BackendVK::setIndexBuffer(const Buffer& value)
{
	assert(value.size > 0);

	auto create_buffer = [&] {
		DeviceBufferVK buffer;

		auto buffer_create_info = vk::BufferCreateInfo()
			.setSize(value.size)
			.setUsage(vk::BufferUsageFlagBits::eIndexBuffer)
			.setSharingMode(vk::SharingMode::eExclusive);

		buffer.buffer = mDevice.createBuffer(buffer_create_info);

		auto memory_requirements = buffer.buffer.getMemoryRequirements();

		auto memory_allocate_info = vk::MemoryAllocateInfo()
			.setAllocationSize(memory_requirements.size)
			.setMemoryTypeIndex(GetMemoryType(vk::MemoryPropertyFlagBits::eHostVisible, memory_requirements.memoryTypeBits, mPhysicalDevice));

		buffer.memory = mDevice.allocateMemory(memory_allocate_info);
		buffer.size = memory_requirements.size;

		buffer.buffer.bindMemory(*buffer.memory, 0);

		return buffer;
	};

	if (mIndexBuffers.size() < mIndexBufferIndex + 1)
	{
		assert(mIndexBuffers.size() == mIndexBufferIndex);
		mIndexBuffers.push_back(create_buffer());
	}

	auto& buffer = mIndexBuffers[mIndexBufferIndex];

	if (buffer.size < value.size)
	{
		buffer = create_buffer();
	}

	auto mem = buffer.memory.mapMemory(0, VK_WHOLE_SIZE);
	memcpy(mem, value.data, value.size);
	buffer.memory.unmapMemory();

	mCommandBuffer.bindIndexBuffer(*buffer.buffer, 0, value.stride == 2 ? vk::IndexType::eUint16 : vk::IndexType::eUint32);

	mIndexBufferIndex += 1;
}

void BackendVK::setUniformBuffer(int slot, void* memory, size_t size)
{
	assert(size > 0);

	auto create_buffer = [&] {
		DeviceBufferVK buffer;

		auto buffer_create_info = vk::BufferCreateInfo()
			.setSize(size)
			.setUsage(vk::BufferUsageFlagBits::eUniformBuffer)
			.setSharingMode(vk::SharingMode::eExclusive);

		buffer.buffer = mDevice.createBuffer(buffer_create_info);

		auto memory_requirements = buffer.buffer.getMemoryRequirements();

		auto memory_allocate_info = vk::MemoryAllocateInfo()
			.setAllocationSize(memory_requirements.size)
			.setMemoryTypeIndex(GetMemoryType(vk::MemoryPropertyFlagBits::eHostVisible, memory_requirements.memoryTypeBits, mPhysicalDevice));

		buffer.memory = mDevice.allocateMemory(memory_allocate_info);
		buffer.size = memory_requirements.size;

		buffer.buffer.bindMemory(*buffer.memory, 0);

		return buffer;
	};

	if (mUniformBuffers.size() < mUniformBufferIndex + 1)
	{
		assert(mUniformBuffers.size() == mUniformBufferIndex);
		mUniformBuffers.push_back(create_buffer());
	}

	auto& buffer = mUniformBuffers[mUniformBufferIndex];

	if (buffer.size < size)
	{
		buffer = create_buffer();
	}

	auto mem = buffer.memory.mapMemory(0, VK_WHOLE_SIZE);
	memcpy(mem, memory, size);
	buffer.memory.unmapMemory();

	mUniformBuffersPushQueue[slot] = *buffer.buffer;
	mUniformBufferIndex += 1;
}

void BackendVK::setBlendMode(const BlendMode& value)
{
}

void BackendVK::setDepthMode(const DepthMode& value)
{
	mCommandBuffer.setDepthTestEnable(value.enabled);
	mCommandBuffer.setDepthCompareOp(CompareOpMap.at(value.func));
	mCommandBuffer.setDepthWriteEnable(true);
}

void BackendVK::setStencilMode(const StencilMode& value)
{
	mCommandBuffer.setStencilTestEnable(value.enabled);
}

void BackendVK::setCullMode(const CullMode& value)
{	
	const static std::unordered_map<CullMode, vk::CullModeFlags> CullModeMap = {
		{ CullMode::None, vk::CullModeFlagBits::eNone },
		{ CullMode::Front, vk::CullModeFlagBits::eFront },
		{ CullMode::Back, vk::CullModeFlagBits::eBack },
	};

	mCommandBuffer.setCullMode(CullModeMap.at(value));
	//mCommandBuffer.setFrontFace(vk::FrontFace::eCounterClockwise);
}

void BackendVK::setSampler(const Sampler& value)
{
}

void BackendVK::setTextureAddressMode(const TextureAddress& value)
{
}

void BackendVK::clear(const std::optional<glm::vec4>& color, const std::optional<float>& depth,
	const std::optional<uint8_t>& stencil)
{
		auto clear_rect = vk::ClearRect()
		.setBaseArrayLayer(0)
		.setLayerCount(1)
		.setRect({ { 0, 0 }, { mWidth, mHeight } });

	if (color.has_value())
	{
		auto value = color.value();

		auto clear_color_value = vk::ClearColorValue()
			.setFloat32({ value.r, value.g, value.b, value.a });

		auto clear_value = vk::ClearValue()
			.setColor(clear_color_value);
		
		auto attachment = vk::ClearAttachment()
			.setAspectMask(vk::ImageAspectFlagBits::eColor)
			.setColorAttachment(0)
			.setClearValue(clear_value);

		mCommandBuffer.clearAttachments({ attachment }, { clear_rect });
	}

	if (depth.has_value() || stencil.has_value())
	{
		auto clear_depth_stencil_value = vk::ClearDepthStencilValue()
			.setDepth(depth.value_or(1.0f))
			.setStencil((uint32_t)stencil.value_or(0)); // TODO: maybe we should change argument uint8_t -> uint32_t

		auto clear_value = vk::ClearValue()
			.setDepthStencil(clear_depth_stencil_value);

		auto aspect_mask = vk::ImageAspectFlags();

		if (depth.has_value())
			aspect_mask |= vk::ImageAspectFlagBits::eDepth;

		if (stencil.has_value())
			aspect_mask |= vk::ImageAspectFlagBits::eStencil;

		auto attachment = vk::ClearAttachment()
			.setAspectMask(aspect_mask)
			.setColorAttachment(0)
			.setClearValue(clear_value);

		mCommandBuffer.clearAttachments({ attachment }, { clear_rect });
	}
}

void BackendVK::draw(uint32_t vertex_count, uint32_t vertex_offset)
{
	prepareForDrawing();
	mCommandBuffer.draw(vertex_count, 0, vertex_offset, 0);
}

void BackendVK::drawIndexed(uint32_t index_count, uint32_t index_offset)
{
	prepareForDrawing();
	mCommandBuffer.drawIndexed(index_count, 1, index_offset, 0, 0);
}

void BackendVK::readPixels(const glm::ivec2& pos, const glm::ivec2& size, TextureHandle* dst_texture_handle)
{
}

void BackendVK::present()
{
	end(); 

	const auto& image_acquired_semaphore = mFrames.at(mSemaphoreIndex).image_acquired_semaphore;

	auto [result, image_index] = mSwapchain.acquireNextImage(UINT64_MAX, *image_acquired_semaphore);

	mFrameIndex = image_index;

	const auto& frame = mFrames.at(mFrameIndex);

	auto wait_result = mDevice.waitForFences({ *frame.fence }, true, UINT64_MAX);

	mDevice.resetFences({ *frame.fence });

	auto begin_info = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	const auto& cmd = frame.command_buffer;

	cmd.begin(begin_info);

	auto color_attachment = vk::RenderingAttachmentInfo()
		.setImageView(*frame.backbuffer_view)
		.setImageLayout(vk::ImageLayout::eAttachmentOptimal)
		.setLoadOp(vk::AttachmentLoadOp::eDontCare)
		.setStoreOp(vk::AttachmentStoreOp::eStore);

	auto rendering_info = vk::RenderingInfo()
		.setRenderArea({ { 0, 0 }, { mWidth, mHeight } })
		.setLayerCount(1)
		.setColorAttachmentCount(1)
		.setPColorAttachments(&color_attachment)
		.setFlags(vk::RenderingFlagBits::eContentsSecondaryCommandBuffers);

	cmd.beginRendering(rendering_info);
	cmd.executeCommands({ *mCommandBuffer });
	cmd.endRendering();
	cmd.end();

	const auto& render_complete_semaphore = mFrames.at(mSemaphoreIndex).render_complete_semaphore;

	vk::PipelineStageFlags wait_dst_stage_mask = vk::PipelineStageFlagBits::eColorAttachmentOutput;

	auto submit_info = vk::SubmitInfo()
		.setPWaitDstStageMask(&wait_dst_stage_mask)
		.setWaitSemaphoreCount(1)
		.setPWaitSemaphores(&*image_acquired_semaphore)
		.setCommandBufferCount(1)
		.setPCommandBuffers(&*frame.command_buffer)
		.setSignalSemaphoreCount(1)
		.setPSignalSemaphores(&*render_complete_semaphore);

	mQueue.submit({ submit_info }, *frame.fence); // TODO: can be called with no fence, check it out

	auto present_info = vk::PresentInfoKHR()
		.setWaitSemaphoreCount(1)
		.setPWaitSemaphores(&*render_complete_semaphore)
		.setSwapchainCount(1)
		.setPSwapchains(&*mSwapchain)
		.setPImageIndices(&mFrameIndex);

	auto present_result = mQueue.presentKHR(present_info);

	mQueue.waitIdle();

	mSemaphoreIndex = (mSemaphoreIndex + 1) % mFrames.size(); // TODO: maybe gFrameIndex can be used for both

	begin();
}

TextureHandle* BackendVK::createTexture(uint32_t width, uint32_t height, uint32_t channels, void* memory, bool mipmap)
{
	auto texture = new TextureDataVK(width, height, channels, memory, mipmap);
	return (TextureHandle*)texture;
}

void BackendVK::destroyTexture(TextureHandle* handle)
{
	auto texture = (TextureDataVK*)handle;
	delete texture;
}

RenderTargetHandle* BackendVK::createRenderTarget(uint32_t width, uint32_t height, TextureHandle* texture_handle)
{
	auto texture = (TextureDataVK*)texture_handle;
	auto render_target = new RenderTargetDataVK(width, height, texture);
	return (RenderTargetHandle*)render_target;
}

void BackendVK::destroyRenderTarget(RenderTargetHandle* handle)
{
	auto render_target = (RenderTargetDataVK*)handle;
	delete render_target;
}

ShaderHandle* BackendVK::createShader(const Vertex::Layout& layout, const std::string& vertex_code,
	const std::string& fragment_code, const std::vector<std::string>& defines)
{
	auto shader = new ShaderDataVK(layout, vertex_code, fragment_code, defines);
	return (ShaderHandle*)shader;
}

void BackendVK::destroyShader(ShaderHandle* handle)
{
	// TODO: remove pipeline connected with this shader

	auto shader = (ShaderDataVK*)handle;
	delete shader;
}

void BackendVK::createSwapchain(uint32_t width, uint32_t height)
{
	mWidth = width;
	mHeight = height;

	auto swapchain_info = vk::SwapchainCreateInfoKHR()
		.setSurface(*mSurface)
		.setMinImageCount(mMinImageCount)
		.setImageFormat(mSurfaceFormat.format)
		.setImageColorSpace(mSurfaceFormat.colorSpace)
		.setImageExtent({ mWidth, mHeight })
		.setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
		.setPreTransform(vk::SurfaceTransformFlagBitsKHR::eIdentity)
		.setImageArrayLayers(1)
		.setImageSharingMode(vk::SharingMode::eExclusive)
		.setQueueFamilyIndexCount(1)
		.setPQueueFamilyIndices(&mQueueFamilyIndex)
		.setPresentMode(vk::PresentModeKHR::eFifo)
		.setClipped(true)
		.setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
		.setOldSwapchain(*mSwapchain);

	mSwapchain = mDevice.createSwapchainKHR(swapchain_info);

	auto backbuffers = mSwapchain.getImages();

	mFrames.clear();

	for (auto& backbuffer : backbuffers)
	{
		auto frame = FrameVK();

		auto buffer_allocate_info = vk::CommandBufferAllocateInfo()
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary)
			.setCommandPool(*mCommandPool);

		auto command_buffers = mDevice.allocateCommandBuffers(buffer_allocate_info);
		frame.command_buffer = std::move(command_buffers.at(0));

		auto fence_info = vk::FenceCreateInfo()
			.setFlags(vk::FenceCreateFlagBits::eSignaled);

		frame.fence = mDevice.createFence(fence_info);

		frame.image_acquired_semaphore = mDevice.createSemaphore({});
		frame.render_complete_semaphore = mDevice.createSemaphore({});

		auto image_view_info = vk::ImageViewCreateInfo()
			.setViewType(vk::ImageViewType::e2D)
			.setFormat(mSurfaceFormat.format)
			.setComponents(vk::ComponentMapping()
				.setR(vk::ComponentSwizzle::eR)
				.setG(vk::ComponentSwizzle::eG)
				.setB(vk::ComponentSwizzle::eB)
				.setA(vk::ComponentSwizzle::eA)
			)
			.setSubresourceRange(vk::ImageSubresourceRange()
				.setAspectMask(vk::ImageAspectFlagBits::eColor)
				.setBaseMipLevel(0)
				.setLevelCount(1)
				.setBaseArrayLayer(0)
				.setLayerCount(1)
			)
			.setImage(backbuffer);

		frame.backbuffer_view = mDevice.createImageView(image_view_info);

		oneTimeSubmit(mDevice, mCommandPool, mQueue, [&](auto& cmd) {
			setImageLayout(cmd, backbuffer, mSurfaceFormat.format, vk::ImageLayout::eUndefined, 
				vk::ImageLayout::ePresentSrcKHR);
		});

		mFrames.push_back(std::move(frame));
	}
}

void BackendVK::begin()
{
	assert(!mWorking);
	mWorking = true;

	mVertexBufferIndex = 0;
	mIndexBufferIndex = 0;
	mUniformBufferIndex = 0;

	auto inheritance_rendering_info = vk::CommandBufferInheritanceRenderingInfo()
		.setColorAttachmentCount(1)
		.setPColorAttachmentFormats(&mSurfaceFormat.format)
		.setRasterizationSamples(vk::SampleCountFlagBits::e1);

	auto inheritance_info = vk::CommandBufferInheritanceInfo()
		.setPNext(&inheritance_rendering_info);

	auto begin_info = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eRenderPassContinue)
		.setPInheritanceInfo(&inheritance_info);

	mCommandBuffer.begin(begin_info);
}

void BackendVK::end()
{
	assert(mWorking);
	mWorking = false;

	mCommandBuffer.end();
}

void BackendVK::prepareForDrawing()
{
	assert(mShader);

	if (!mPipelines.contains(mShader))
	{
		auto pipeline_shader_stage_create_info = {
			vk::PipelineShaderStageCreateInfo()
				.setStage(vk::ShaderStageFlagBits::eVertex)
				.setModule(*mShader->vertex_shader_module)
				.setPName("main"),

			vk::PipelineShaderStageCreateInfo()
				.setStage(vk::ShaderStageFlagBits::eFragment)
				.setModule(*mShader->fragment_shader_module)
				.setPName("main")
		};

		auto pipeline_input_assembly_state_create_info = vk::PipelineInputAssemblyStateCreateInfo()
			.setTopology(vk::PrimitiveTopology::eTriangleList);

		auto pipeline_viewport_state_create_info = vk::PipelineViewportStateCreateInfo()
			.setViewportCount(1)
			.setScissorCount(1);

		auto pipeline_rasterization_state_create_info = vk::PipelineRasterizationStateCreateInfo()
			.setPolygonMode(vk::PolygonMode::eFill);

		auto pipeline_multisample_state_create_info = vk::PipelineMultisampleStateCreateInfo()
			.setRasterizationSamples(vk::SampleCountFlagBits::e1);

		auto pipeline_depth_stencil_state_create_info = vk::PipelineDepthStencilStateCreateInfo();

		auto pipeline_color_blent_attachment_state = vk::PipelineColorBlendAttachmentState()
			.setBlendEnable(true)
			.setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
			.setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
			.setColorBlendOp(vk::BlendOp::eAdd)
			.setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
			.setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
			.setAlphaBlendOp(vk::BlendOp::eAdd)
			.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
				vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);

		auto pipeline_color_blend_state_create_info = vk::PipelineColorBlendStateCreateInfo()
			.setAttachmentCount(1)
			.setPAttachments(&pipeline_color_blent_attachment_state);

		auto pipeline_vertex_input_state_create_info = vk::PipelineVertexInputStateCreateInfo()
			.setVertexBindingDescriptionCount(1)
			.setPVertexBindingDescriptions(&mShader->vertex_input_binding_description)
			.setVertexAttributeDescriptions(mShader->vertex_input_attribute_descriptions);

		auto dynamic_states = {
			vk::DynamicState::eViewport,
			vk::DynamicState::eScissor,
			vk::DynamicState::ePrimitiveTopology,
			vk::DynamicState::eLineWidth,
			vk::DynamicState::eCullMode,
			vk::DynamicState::eFrontFace,
			vk::DynamicState::eVertexInputBindingStride
		};

		auto pipeline_dynamic_state_create_info = vk::PipelineDynamicStateCreateInfo()
			.setDynamicStates(dynamic_states);

		auto pipeline_rendering_create_info = vk::PipelineRenderingCreateInfo()
			.setColorAttachmentCount(1)
			.setColorAttachmentFormats(mSurfaceFormat.format);

		auto graphics_pipeline_create_info = vk::GraphicsPipelineCreateInfo()
			.setLayout(*mShader->pipeline_layout)
			.setFlags(vk::PipelineCreateFlagBits())
			.setStages(pipeline_shader_stage_create_info)
			.setPVertexInputState(&pipeline_vertex_input_state_create_info)
			.setPInputAssemblyState(&pipeline_input_assembly_state_create_info)
			.setPViewportState(&pipeline_viewport_state_create_info)
			.setPRasterizationState(&pipeline_rasterization_state_create_info)
			.setPMultisampleState(&pipeline_multisample_state_create_info)
			.setPDepthStencilState(&pipeline_depth_stencil_state_create_info)
			.setPColorBlendState(&pipeline_color_blend_state_create_info)
			.setPDynamicState(&pipeline_dynamic_state_create_info)
			.setRenderPass(nullptr)
			.setPNext(&pipeline_rendering_create_info);

		mPipelines.insert({ mShader, mDevice.createGraphicsPipeline(nullptr, graphics_pipeline_create_info) });
	}

	auto pipeline = *mPipelines.at(mShader);

	mCommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

	auto pipeline_layout = *mShader->pipeline_layout;

	for (const auto& [slot, image_view] : mTexturesPushQueue)
	{
		auto descriptor_image_info = vk::DescriptorImageInfo()
			.setSampler(*mSampler)
			.setImageView(image_view)
			.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

		auto write_descriptor_set = vk::WriteDescriptorSet()
			.setDescriptorCount(1)
			.setDstBinding(slot)
			.setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
			.setPImageInfo(&descriptor_image_info);

		mCommandBuffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, { write_descriptor_set });
	}

	mTexturesPushQueue.clear();

	for (const auto& [slot, buffer] : mUniformBuffersPushQueue)
	{
		auto descriptor_buffer_info = vk::DescriptorBufferInfo()
			.setBuffer(buffer)
			.setRange(VK_WHOLE_SIZE);

		auto write_descriptor_set = vk::WriteDescriptorSet()
			.setDescriptorCount(1)
			.setDstBinding(slot)
			.setDescriptorType(vk::DescriptorType::eUniformBuffer)
			.setPBufferInfo(&descriptor_buffer_info);

		mCommandBuffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, { write_descriptor_set });
	}

	mUniformBuffersPushQueue.clear();
}

#endif