#pragma once

#include <glm\detail\type_int.hpp>

struct VertexBufferData
{
	VertexBufferData() :
		pDataStart(nullptr),
		BufferSize(0),
		VertexStride(0),
		VertexCount(0)
	{}

	enum class VertexAttribute : glm::uint
	{
		NONE = 0,
		POSITION = (1 << 0),
		COLOR = (1 << 1),
		TANGENT = (1 << 2),
		BITANGENT = (1 << 3),
		NORMAL = (1 << 4),
		TEXCOORD = (1 << 5)
	};

	bool HasAttribute(VertexAttribute attribute) const
	{
		return (Attributes & ((glm::uint)attribute));
	}

	void* pDataStart;
	glm::uint BufferSize;
	glm::uint VertexStride;
	glm::uint VertexCount;
	glm::uint Attributes;

	void VertexBufferData::Destroy()
	{
		free(pDataStart);
	}
};
