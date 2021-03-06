#include "skygfx.h"
#include "backend.h"
#include "backend_d3d11.h"
#include "backend_gl44.h"
#include "backend_vk.h"
#include "backend_mtl.h"

#include <stdexcept>
#include <cassert>

using namespace skygfx;

static Backend* gBackend = nullptr;

// texture

Texture::Texture(uint32_t width, uint32_t height, uint32_t channels, void* memory, bool mipmap) :
	mWidth(width),
	mHeight(height)
{
	mTextureHandle = gBackend->createTexture(width, height, channels, memory, mipmap);
}

Texture::~Texture()
{
	gBackend->destroyTexture(mTextureHandle);
}

RenderTarget::RenderTarget(uint32_t width, uint32_t height) : Texture(width, height, 4, nullptr)
{
	mRenderTargetHandle = gBackend->createRenderTarget(width, height, *this);
}

RenderTarget::~RenderTarget()
{
	gBackend->destroyRenderTarget(mRenderTargetHandle);
}

// shader

Shader::Shader(const Vertex::Layout& layout, const std::string& vertex_code, const std::string& fragment_code, const std::vector<std::string>& defines)
{
	mShaderHandle = gBackend->createShader(layout, vertex_code, fragment_code, defines);
}

Shader::~Shader()
{
	gBackend->destroyShader(mShaderHandle);
}

// device

Device::Device(BackendType type, void* window, uint32_t width, uint32_t height)
{
	assert(gBackend == nullptr);

#ifdef SKYGFX_HAS_D3D11
	if (type == BackendType::D3D11)
		gBackend = new BackendD3D11(window, width, height);
#endif
#ifdef SKYGFX_HAS_OPENGL
	if (type == BackendType::OpenGL44)
		gBackend = new BackendGL44(window, width, height);
#endif
#ifdef SKYGFX_HAS_VULKAN
	if (type == BackendType::Vulkan)
		gBackend = new BackendVK(window, width, height);
#endif
#ifdef SKYGFX_HAS_METAL
	if (type == BackendType::Metal)
		gBackend = new BackendMetal(window, width, height);
#endif

	if (gBackend == nullptr)
		throw std::runtime_error("backend not implemented");
}

Device::~Device()
{
	delete gBackend;
	gBackend = nullptr;
}

void Device::resize(uint32_t width, uint32_t height)
{
	gBackend->resize(width, height);
}

void Device::setTopology(Topology topology)
{
	gBackend->setTopology(topology);
}

void Device::setViewport(std::optional<Viewport> viewport)
{
	gBackend->setViewport(viewport);
}

void Device::setScissor(std::optional<Scissor> scissor)
{
	gBackend->setScissor(scissor);
}

void Device::setTexture(const Texture& texture, uint32_t slot)
{
	gBackend->setTexture(const_cast<Texture&>(texture), slot);
}

void Device::setRenderTarget(const RenderTarget& value)
{
	gBackend->setRenderTarget(const_cast<RenderTarget&>(value));
}

void Device::setRenderTarget(std::nullptr_t value)
{
	gBackend->setRenderTarget(value);
}

void Device::setShader(const Shader& shader)
{
	gBackend->setShader(const_cast<Shader&>(shader));
}

void Device::setVertexBuffer(const Buffer& buffer)
{
	gBackend->setVertexBuffer(buffer);
}

void Device::setIndexBuffer(const Buffer& buffer)
{
	gBackend->setIndexBuffer(buffer);
}

void Device::setUniformBuffer(int slot, void* memory, size_t size)
{
	gBackend->setUniformBuffer(slot, memory, size);
}

void Device::setBlendMode(const BlendMode& value)
{
	gBackend->setBlendMode(value);
}

void Device::setDepthMode(std::optional<DepthMode> depth_mode)
{
	gBackend->setDepthMode(depth_mode);
}

void Device::setStencilMode(std::optional<StencilMode> stencil_mode)
{
	gBackend->setStencilMode(stencil_mode);
}

void Device::setCullMode(CullMode cull_mode)
{
	gBackend->setCullMode(cull_mode);
}

void Device::setSampler(const Sampler& value)
{
	gBackend->setSampler(value);
}

void Device::setTextureAddressMode(const TextureAddress& value)
{
	gBackend->setTextureAddressMode(value);
}

void Device::clear(const std::optional<glm::vec4>& color, const std::optional<float>& depth, 
	const std::optional<uint8_t>& stencil)
{
	gBackend->clear(color, depth, stencil);
}

void Device::draw(uint32_t vertex_count, uint32_t vertex_offset)
{
	gBackend->draw(vertex_count, vertex_offset);
}

void Device::drawIndexed(uint32_t index_count, uint32_t index_offset)
{
	gBackend->drawIndexed(index_count, index_offset);
}

void Device::readPixels(const glm::ivec2& pos, const glm::ivec2& size, Texture& dst_texture)
{
	gBackend->readPixels(pos, size, dst_texture);
}

void Device::present()
{
	gBackend->present();
}
