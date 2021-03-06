#pragma once

#include <vector>
#include <glm/glm.hpp>

namespace skygfx::Vertex
{
	struct Attribute
	{
		enum class Type
		{
			Position,
			Color,
			TexCoord,
			Normal
		};

		enum class Format
		{
			R32F,
			R32G32F,
			R32G32B32F,
			R32G32B32A32F,
			R8UN,
			R8G8UN,
			R8G8B8UN,
			R8G8B8A8UN
		};

		Vertex::Attribute::Type type;
		Vertex::Attribute::Format format;
		size_t offset;
	};

	struct Layout
	{
		size_t stride;
		std::vector<Attribute> attributes;

		bool hasAttribute(Vertex::Attribute::Type type) const
		{
			return attributes.cend() != std::find_if(attributes.cbegin(), attributes.cend(), [type](const Vertex::Attribute& attrib) {
				return attrib.type == type;
			});
		}
	};

	// predefined vertex types:

	struct Position
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionColor
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec4 col = { 0.0f, 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionTexture 
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec2 tex = { 0.0f, 0.0f };
	
		static const Layout Layout;
	};

	struct PositionNormal
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec3 normal = { 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionColorNormal
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec4 col = { 0.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 normal = { 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionColorTexture
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec4 col = { 0.0f, 0.0f, 0.0f, 0.0f };
		glm::vec2 tex = { 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionTextureNormal
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec2 tex = { 0.0f, 0.0f };
		glm::vec3 normal = { 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};

	struct PositionColorTextureNormal
	{
		glm::vec3 pos = { 0.0f, 0.0f, 0.0f };
		glm::vec4 col = { 0.0f, 0.0f, 0.0f, 0.0f };
		glm::vec2 tex = { 0.0f, 0.0f };
		glm::vec3 normal = { 0.0f, 0.0f, 0.0f };

		static const Layout Layout;
	};
}