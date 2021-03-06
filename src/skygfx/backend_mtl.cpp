#include "backend_mtl.h"

#ifdef SKYGFX_HAS_METAL

#include <unordered_map>

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Metal/Metal.hpp>
#include <AppKit/AppKit.hpp>
#include <MetalKit/MetalKit.hpp>

using namespace skygfx;

static NS::AutoreleasePool* gAutoreleasePool = nullptr;
static MTL::Device* gDevice = nullptr;
static MTK::View* gView = nullptr;
static MTL::CommandQueue* gCommandQueue = nullptr;
static MTL::CommandBuffer* gCommandBuffer = nullptr;
static MTL::RenderPassDescriptor* gRenderPassDescriptor = nullptr;
static MTL::RenderCommandEncoder* gRenderCommandEncoder = nullptr;
static MTL::PrimitiveType gPrimitiveType = MTL::PrimitiveType::PrimitiveTypeTriangle;
static MTL::IndexType gIndexType = MTL::IndexType::IndexTypeUInt16;
static MTL::Buffer* gIndexBuffer = nullptr;
static Buffer gVertexBuffer;
static MTL::Texture* gTexture = nullptr;
static MTL::SamplerState* gSamplerState = nullptr;
static std::unordered_map<int, MTL::Buffer*> gUniformBuffers;
static CullMode gCullMode = CullMode::None;
static const uint32_t gVertexBufferStageBinding = 30;

class ShaderDataMetal;

static ShaderDataMetal* gShader = nullptr;

class ShaderDataMetal
{
	friend class BackendMetal;
	
private:
	MTL::Library* vert_library = nullptr;
	MTL::Library* frag_library = nullptr;
	MTL::RenderPipelineState* pso = nullptr;
	
public:
	ShaderDataMetal(const Vertex::Layout& layout, const std::string& vertex_code, const std::string& fragment_code,
		std::vector<std::string> defines)
	{
		AddShaderLocationDefines(layout, defines);

		auto vertex_shader_spirv = CompileGlslToSpirv(ShaderStage::Vertex, vertex_code, defines);
		auto fragment_shader_spirv = CompileGlslToSpirv(ShaderStage::Fragment, fragment_code, defines);

		auto msl_vert = CompileSpirvToMsl(vertex_shader_spirv);
		auto msl_frag = CompileSpirvToMsl(fragment_shader_spirv);
		
		NS::Error* error = nullptr;
		
		vert_library = gDevice->newLibrary(NS::String::string(msl_vert.c_str(), NS::StringEncoding::UTF8StringEncoding), nullptr, &error);
		if (!vert_library)
		{
			auto reason = error->localizedDescription()->utf8String();
			throw std::runtime_error(reason);
		}

		frag_library = gDevice->newLibrary(NS::String::string(msl_frag.c_str(), NS::StringEncoding::UTF8StringEncoding), nullptr, &error);
		if (!frag_library)
		{
			auto reason = error->localizedDescription()->utf8String();
			throw std::runtime_error(reason);
		}

		auto vert_fn = vert_library->newFunction(NS::String::string("main0", NS::StringEncoding::UTF8StringEncoding));
		auto frag_fn = frag_library->newFunction(NS::String::string("main0", NS::StringEncoding::UTF8StringEncoding));

		static const std::unordered_map<Vertex::Attribute::Format, MTL::VertexFormat> Format = {
			{ Vertex::Attribute::Format::R32F, MTL::VertexFormat::VertexFormatFloat },
			{ Vertex::Attribute::Format::R32G32F, MTL::VertexFormat::VertexFormatFloat2 },
			{ Vertex::Attribute::Format::R32G32B32F, MTL::VertexFormat::VertexFormatFloat3 },
			{ Vertex::Attribute::Format::R32G32B32A32F, MTL::VertexFormat::VertexFormatFloat4 },
			//{ Vertex::Attribute::Format::R8UN, },
			//{ Vertex::Attribute::Format::R8G8UN, },
			//{ Vertex::Attribute::Format::R8G8B8UN, },
			//{ Vertex::Attribute::Format::R8G8B8A8UN, }
		};

		auto vertex_descriptor = MTL::VertexDescriptor::alloc()->init();
		
		for (int i = 0; i < layout.attributes.size(); i++)
		{
			const auto& attrib = layout.attributes.at(i);
			auto desc = vertex_descriptor->attributes()->object(i);
			desc->setFormat(Format.at(attrib.format));
			desc->setOffset(attrib.offset);
			desc->setBufferIndex(gVertexBufferStageBinding);
		}

		auto vertex_layout = vertex_descriptor->layouts()->object(gVertexBufferStageBinding);
		vertex_layout->setStride(layout.stride);
		vertex_layout->setStepRate(1);
		vertex_layout->setStepFunction(MTL::VertexStepFunction::VertexStepFunctionPerVertex);
		
		auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
		desc->setVertexFunction(vert_fn);
		desc->setFragmentFunction(frag_fn);
		desc->setVertexDescriptor(vertex_descriptor);
		desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
		//desc->setDepthAttachmentPixelFormat(MTL::PixelFormat::PixelFormatDepth16Unorm);
		desc->setVertexDescriptor(vertex_descriptor);
		
		pso = gDevice->newRenderPipelineState(desc, &error);
		if (!pso)
		{
			auto reason = error->localizedDescription()->utf8String();
			throw std::runtime_error(reason);
		}

		vertex_descriptor->release();
		vert_fn->release();
		frag_fn->release();
		desc->release();
	}

	~ShaderDataMetal()
	{
		pso->release();
		vert_library->release();
		frag_library->release();
	}
};

class TextureDataMetal
{
	friend class BackendMetal;
	
private:
	MTL::Texture* texture = nullptr;
	
public:
	TextureDataMetal(uint32_t width, uint32_t height, uint32_t channels, void* memory, bool mipmap)
	{
		auto desc = MTL::TextureDescriptor::alloc()->init();
		desc->setWidth(width);
		desc->setHeight(height);
		desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		desc->setTextureType(MTL::TextureType2D);
		desc->setStorageMode(MTL::StorageModeManaged);
		desc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

		if (mipmap)
		{
			int height_levels = ceil(log2(height));
			int width_levels = ceil(log2(width));
			int mip_count = (height_levels > width_levels) ? height_levels : width_levels;
			desc->setMipmapLevelCount(mip_count);
		}
		
		texture = gDevice->newTexture(desc);
		texture->replaceRegion(MTL::Region(0, 0, 0, width, height, 1), 0, memory, width * channels);
		
		if (mipmap)
		{
			auto cmd = gCommandQueue->commandBuffer();
			auto enc = cmd->blitCommandEncoder();
			enc->generateMipmaps(texture);
			enc->endEncoding();
			cmd->commit();
			cmd->waitUntilCompleted();
		}
		
		desc->release();
	}

	~TextureDataMetal()
	{
		texture->release();
	}
};

class RenderTargetDataMetal
{
public:
	RenderTargetDataMetal(uint32_t width, uint32_t height, TextureDataMetal* _texture_data)
	{
	}

	~RenderTargetDataMetal()
	{
	}
};

static NS::AutoreleasePool* gFrameAutoreleasePool = nullptr;

static void begin()
{
	gFrameAutoreleasePool = NS::AutoreleasePool::alloc()->init();
	
	gCommandBuffer = gCommandQueue->commandBuffer();
	gRenderPassDescriptor = gView->currentRenderPassDescriptor();
	gRenderCommandEncoder = gCommandBuffer->renderCommandEncoder(gRenderPassDescriptor);
}

static void end()
{
	gRenderCommandEncoder->endEncoding();
	gCommandBuffer->presentDrawable(gView->currentDrawable());
	gCommandBuffer->commit();
	
	gFrameAutoreleasePool->release();
}

BackendMetal::BackendMetal(void* window, uint32_t width, uint32_t height)
{
	gAutoreleasePool = NS::AutoreleasePool::alloc()->init();
	
	gDevice = MTL::CreateSystemDefaultDevice();
	
	auto frame = CGRect{ { 0.0, 0.0 }, { (float)width, (float)height } };
	
	gView = MTK::View::alloc()->init(frame, gDevice);
	gView->setColorPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);

	auto _window = (NS::Window*)window;
	_window->setContentView(gView);
	
	gCommandQueue = gDevice->newCommandQueue();
	
	auto sampler_desc = MTL::SamplerDescriptor::alloc()->init();
	sampler_desc->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
	sampler_desc->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
	sampler_desc->setMipFilter(MTL::SamplerMipFilter::SamplerMipFilterLinear);
	gSamplerState = gDevice->newSamplerState(sampler_desc);
	sampler_desc->release();
	
	begin();
}

BackendMetal::~BackendMetal()
{
	end();
	
	for (auto [slot, buffer] : gUniformBuffers)
	{
		buffer->release();
	}
	
	gSamplerState->release();
	gCommandQueue->release();
	gView->release();
	gDevice->release();
	gAutoreleasePool->release();
}

void BackendMetal::resize(uint32_t width, uint32_t height)
{
}

void BackendMetal::setTopology(Topology topology)
{
	const static std::unordered_map<Topology, MTL::PrimitiveType> TopologyMap = {
		{ Topology::PointList, MTL::PrimitiveType::PrimitiveTypePoint },
		{ Topology::LineList, MTL::PrimitiveType::PrimitiveTypeLine },
		{ Topology::LineStrip, MTL::PrimitiveType::PrimitiveTypeLineStrip },
		{ Topology::TriangleList, MTL::PrimitiveType::PrimitiveTypeTriangle },
		{ Topology::TriangleStrip, MTL::PrimitiveType::PrimitiveTypeTriangleStrip }
	};

	gPrimitiveType = TopologyMap.at(topology);
}

void BackendMetal::setViewport(const Viewport& viewport)
{
}

void BackendMetal::setScissor(const Scissor& value)
{
}

void BackendMetal::setScissor(std::nullptr_t value)
{
}

void BackendMetal::setTexture(TextureHandle* handle)
{
	auto texture = (TextureDataMetal*)handle;
	gTexture = texture->texture;
}

void BackendMetal::setRenderTarget(RenderTargetHandle* handle)
{
}

void BackendMetal::setRenderTarget(std::nullptr_t value)
{
}

void BackendMetal::setShader(ShaderHandle* handle)
{
	auto shader = (ShaderDataMetal*)handle;
	gShader = shader;
}

void BackendMetal::setVertexBuffer(const Buffer& buffer)
{
	gVertexBuffer = buffer;
}

void BackendMetal::setIndexBuffer(const Buffer& buffer)
{
	if (gIndexBuffer == nullptr)
	{
		gIndexBuffer = gDevice->newBuffer(buffer.size, MTL::ResourceStorageModeManaged);
	}
	
	if (gIndexBuffer->length() < buffer.size)
	{
		gIndexBuffer->release();
		gIndexBuffer = gDevice->newBuffer(buffer.size, MTL::ResourceStorageModeManaged);
	}

	memcpy(gIndexBuffer->contents(), buffer.data, buffer.size);
	gIndexBuffer->didModifyRange(NS::Range::Make(0, buffer.size));

	gIndexType = buffer.stride == 2 ? MTL::IndexType::IndexTypeUInt16 : MTL::IndexType::IndexTypeUInt32;
}

void BackendMetal::setUniformBuffer(int slot, void* memory, size_t size)
{
	auto createBuffer = [&] {
		return gDevice->newBuffer(size, MTL::ResourceStorageModeManaged);
	};
	
	if (!gUniformBuffers.contains(slot))
	{
		gUniformBuffers.insert({ slot, createBuffer() });
	}
	
	auto uniform_buffer = gUniformBuffers.at(slot);
	
	if (uniform_buffer->length() < size)
	{
		uniform_buffer->release();
		uniform_buffer = createBuffer();
		gUniformBuffers[slot] = uniform_buffer;
	}

	memcpy(uniform_buffer->contents(), memory, size);
	uniform_buffer->didModifyRange(NS::Range::Make(0, size));
}

void BackendMetal::setBlendMode(const BlendMode& value)
{
}

void BackendMetal::setDepthMode(const DepthMode& value)
{
}

void BackendMetal::setStencilMode(const StencilMode& value)
{
}

void BackendMetal::setCullMode(const CullMode& value)
{
	gCullMode = value;
}

void BackendMetal::setSampler(const Sampler& value)
{
}

void BackendMetal::setTextureAddressMode(const TextureAddress& value)
{
}

void BackendMetal::clear(const std::optional<glm::vec4>& color, const std::optional<float>& depth,
	const std::optional<uint8_t>& stencil)
{
	auto col = color.value();
	gView->setClearColor(MTL::ClearColor::Make(col.r, col.g, col.b, col.a));

	gRenderCommandEncoder->endEncoding();
	gRenderCommandEncoder = gCommandBuffer->renderCommandEncoder(gRenderPassDescriptor);
}

void BackendMetal::draw(uint32_t vertex_count, uint32_t vertex_offset)
{
	prepareForDrawing();
	gRenderCommandEncoder->drawPrimitives(gPrimitiveType, vertex_offset, vertex_count);
}

void BackendMetal::drawIndexed(uint32_t index_count, uint32_t index_offset)
{
	prepareForDrawing();
	gRenderCommandEncoder->drawIndexedPrimitives(gPrimitiveType, index_count, gIndexType, gIndexBuffer, index_offset);
}

void BackendMetal::readPixels(const glm::ivec2& pos, const glm::ivec2& size, TextureHandle* dst_texture_handle)
{
}

void BackendMetal::present()
{
	end();
	gView->draw();
	begin();
}

TextureHandle* BackendMetal::createTexture(uint32_t width, uint32_t height, uint32_t channels, void* memory, bool mipmap)
{
	auto texture = new TextureDataMetal(width, height, channels, memory, mipmap);
	return (TextureHandle*)texture;
}

void BackendMetal::destroyTexture(TextureHandle* handle)
{
	auto texture = (TextureDataMetal*)handle;
	delete texture;
}

RenderTargetHandle* BackendMetal::createRenderTarget(uint32_t width, uint32_t height, TextureHandle* texture_handle)
{
	auto texture = (TextureDataMetal*)texture_handle;
	auto render_target = new RenderTargetDataMetal(width, height, texture);
	return (RenderTargetHandle*)render_target;
}

void BackendMetal::destroyRenderTarget(RenderTargetHandle* handle)
{
	auto render_target = (RenderTargetDataMetal*)handle;
	delete render_target;
}

ShaderHandle* BackendMetal::createShader(const Vertex::Layout& layout, const std::string& vertex_code,
	const std::string& fragment_code, const std::vector<std::string>& defines)
{
	auto shader = new ShaderDataMetal(layout, vertex_code, fragment_code, defines);
	return (ShaderHandle*)shader;
}

void BackendMetal::destroyShader(ShaderHandle* handle)
{
	auto shader = (ShaderDataMetal*)handle;
	delete shader;
}

void BackendMetal::prepareForDrawing()
{
	if (gTexture)
	{
		gRenderCommandEncoder->setFragmentTexture(gTexture, 0);
	}
	
	if (gSamplerState)
	{
		gRenderCommandEncoder->setFragmentSamplerState(gSamplerState, 0);
	}
	
	gRenderCommandEncoder->setVertexBytes(gVertexBuffer.data, gVertexBuffer.size, gVertexBufferStageBinding);
	gRenderCommandEncoder->setRenderPipelineState(gShader->pso);

	for (auto [slot, buffer] : gUniformBuffers)
	{
		gRenderCommandEncoder->setVertexBuffer(buffer, 0, slot);
		gRenderCommandEncoder->setFragmentBuffer(buffer, 0, slot);
	}
	
	static const std::unordered_map<CullMode, MTL::CullMode> CullModes = {
		{ CullMode::None, MTL::CullMode::CullModeNone },
		{ CullMode::Back, MTL::CullMode::CullModeBack },
		{ CullMode::Front, MTL::CullMode::CullModeFront }
	};
	
	gRenderCommandEncoder->setCullMode(CullModes.at(gCullMode));
	gRenderCommandEncoder->setFrontFacingWinding(MTL::Winding::WindingClockwise);
}

#endif
