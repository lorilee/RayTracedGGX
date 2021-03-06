//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "RayTracer.h"

#define SizeOfInUint32(obj) ((sizeof(obj) - 1) / sizeof(uint32_t) + 1)

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t *RayTracer::HitGroupName = L"hitGroup";
const wchar_t *RayTracer::RaygenShaderName = L"raygenMain";
const wchar_t *RayTracer::ClosestHitShaderName = L"closestHitMain";
const wchar_t *RayTracer::MissShaderName = L"missMain";

RayTracer::RayTracer(const RayTracing::Device &device, const RayTracing::CommandList &commandList) :
	m_device(device),
	m_commandList(commandList),
	m_instances(nullptr)
{
	m_pipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device.Common);
	m_pipelineLayoutCache.SetDevice(device.Common);

	m_descriptorTableCache.SetName(L"RayTracerDescriptorTableCache");
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(uint32_t width, uint32_t height, Resource *vbUploads, Resource *ibUploads,
	Geometry *geometries, const char *fileName, const XMFLOAT4 &posScale)
{
	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), vbUploads[MODEL_OBJ]), false);
	N_RETURN(createIB(objLoader.GetNumIndices(), objLoader.GetIndices(), ibUploads[MODEL_OBJ]), false);

	N_RETURN(createGroundMesh(vbUploads[GROUND], ibUploads[GROUND]), false);

	// Create raytracing pipeline
	createPipelineLayouts();
	N_RETURN(createPipeline(), false);

	// Create output view and build acceleration structures
	for (auto &outputView : m_outputViews)
		outputView.Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	N_RETURN(buildAccelerationStructures(geometries), false);
	buildShaderTables();

	// Create the sampler
	{
		Util::DescriptorTable samplerTable;
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		samplerTable.SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache);
		m_samplerTable = samplerTable.GetSamplerTable(m_descriptorTableCache);
	}

	return true;
}

void RayTracer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		XMStoreFloat4x4(&m_cbRayGens[frameIndex].ProjToWorld, XMMatrixTranspose(projToWorld));
		XMStoreFloat3(&m_cbRayGens[frameIndex].EyePt, eyePt);

		m_rayGenShaderTables[frameIndex].Reset();
		m_rayGenShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			RaygenShaderName, &m_cbRayGens[frameIndex], sizeof(RayGenConstants)));
	}

	{
		static float angle = 0.0f;
		angle += 0.1f * XM_PI / 180.0f;
		XMStoreFloat4x4(&m_rot, XMMatrixRotationY(angle));

		m_hitGroupShaderTables[frameIndex].Reset();
		m_hitGroupShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			HitGroupName, &XMMatrixTranspose(XMLoadFloat4x4(&m_rot)), sizeof(XMFLOAT4X4)));
	}
}

void RayTracer::Render(uint32_t frameIndex, const Descriptor &dsv)
{
	m_outputViews[frameIndex].Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	updateAccelerationStructures();
	rayTrace(frameIndex);
}

const Texture2D &RayTracer::GetOutputView(uint32_t frameIndex, ResourceState dstState)
{
	if (dstState) m_outputViews[frameIndex].Barrier(m_commandList, dstState);

	return m_outputViews[frameIndex];
}

bool RayTracer::createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, Resource &vbUpload)
{
	auto &vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	N_RETURN(vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return vertexBuffer.Upload(m_commandList, vbUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(uint32_t numIndices, const uint32_t *pData, Resource &ibUpload)
{
	auto &indexBuffers = m_indexBuffers[MODEL_OBJ];
	N_RETURN(indexBuffers.Create(m_device.Common, sizeof(uint32_t) * numIndices, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return indexBuffers.Upload(m_commandList, ibUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createGroundMesh(Resource &vbUpload,Resource &ibUpload)
{
	// Vertex buffer
	{
		// Cube vertices positions and corresponding triangle normals.
		XMFLOAT3 vertices[][2] =
		{
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },

			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
		};

		auto &vertexBuffer = m_vertexBuffers[GROUND];
		N_RETURN(vertexBuffer.Create(m_device.Common, static_cast<uint32_t>(size(vertices)), sizeof(XMFLOAT3[2]),
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(vertexBuffer.Upload(m_commandList, vbUpload, vertices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	// Index Buffer
	{
		// Cube indices.
		uint32_t indices[] =
		{
			3,1,0,
			2,1,3,

			6,4,5,
			7,4,6,

			11,9,8,
			10,9,11,

			14,12,13,
			15,12,14,

			19,17,16,
			18,17,19,

			22,20,21,
			23,20,22
		};

		auto &indexBuffers = m_indexBuffers[GROUND];
		N_RETURN(indexBuffers.Create(m_device.Common, sizeof(indices), DXGI_FORMAT_R32_UINT,
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(indexBuffers.Upload(m_commandList, ibUpload, indices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

void RayTracer::createPipelineLayouts()
{
	// Global Root Signature
	// This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetRootSRV(ACCELERATION_STRUCTURE, 0);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
		pipelineLayout.SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
		m_pipelineLayouts[GLOBAL_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs, L"RayTracerGlobalPipelineLayout");
	}

	// Local Root Signature for RayGen shader
	// This is a root signature that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(RayGenConstants), 0);
		m_pipelineLayouts[RAY_GEN_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerRayGenPipelineLayout");
	}

	// Local Root Signature for Hit group
	// This is a root signature that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(XMFLOAT4X4), 1);
		m_pipelineLayouts[HIT_GROUP_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerHitGroupPipelineLayout");
	}
}

bool RayTracer::createPipeline()
{
	{
		V_RETURN(D3DReadFileToBlob(L"RayTracedTest.cso", &m_shaderLib), cerr, false);

		RayTracing::State state;
		state.SetShaderLibrary(m_shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetLocalPipelineLayout(1, m_pipelineLayouts[HIT_GROUP_LAYOUT],
			1, reinterpret_cast<const void**>(&HitGroupName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(3);
		m_pipelines[TEST] = state.GetPipeline(m_pipelineCache);
	}

	return true;
}

void RayTracer::createDescriptorTables()
{
	//m_descriptorTableCache.AllocateDescriptorPool(CBV_SRV_UAV_POOL, NumUAVs + NUM_MESH * 2);

	for (auto i = 0u; i < FrameCount; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[i].GetUAV());
		m_uavTables[i][UAV_TABLE_OUTPUT] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH + 1];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_bottomLevelASs[i].GetResult().GetUAV();
		descriptors[NUM_MESH] = m_topLevelAS.GetResult().GetUAV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_IB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_VB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}
}

bool RayTracer::buildAccelerationStructures(Geometry *geometries)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	// Set geometries
	VertexBufferView vertexBufferViews[NUM_MESH];
	IndexBufferView indexBufferViews[NUM_MESH];
	for (auto i = 0; i < NUM_MESH; ++i)
	{
		vertexBufferViews[i] = m_vertexBuffers[i].GetVBV();
		indexBufferViews[i] = m_indexBuffers[i].GetIBV();
	}
	BottomLevelAS::SetGeometries(geometries, NUM_MESH, DXGI_FORMAT_R32G32B32_FLOAT,
		vertexBufferViews, indexBufferViews);

	// Descriptor index in descriptor pool
	const uint32_t bottomLevelASIndex = FrameCount;
	const uint32_t topLevelASIndex = bottomLevelASIndex + NUM_MESH;

	// Prebuild
	for (auto i = 0; i < NUM_MESH; ++i)
		N_RETURN(m_bottomLevelASs[i].PreBuild(m_device, 1, &geometries[i],
			bottomLevelASIndex + i, NumUAVs), false);
	N_RETURN(m_topLevelAS.PreBuild(m_device, NUM_MESH, topLevelASIndex, NumUAVs,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS.GetScratchDataMaxSize();
	for (const auto &bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS.GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device, m_scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	createDescriptorTables();
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT4X4 matrices[NUM_MESH];
	XMStoreFloat4x4(&matrices[GROUND], XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))));
	XMStoreFloat4x4(&matrices[MODEL_OBJ], XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))));
	float *const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto &bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS.Build(m_commandList, m_scratch, descriptorPool, NumUAVs);

	// Build top level AS
	m_topLevelAS.Build(m_commandList, m_scratch, m_instances, descriptorPool, NumUAVs);

	return true;
}

void RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		// Ray gen shader table
		m_rayGenShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
			(L"RayGenShaderTable" + to_wstring(i)).c_str());
		m_rayGenShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			RaygenShaderName, &m_cbRayGens[i], sizeof(RayGenConstants)));

		// Hit group shader table
		m_hitGroupShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(XMFLOAT4X4), L"HitGroupShaderTable");
		m_hitGroupShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST], HitGroupName,
			&XMMatrixTranspose(XMLoadFloat4x4(&m_rot)), sizeof(XMFLOAT4X4)));
	}

	// Miss shader table
	m_missShaderTable.Create(m_device, 1, shaderIDSize, L"MissShaderTable");
	m_missShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST], MissShaderName));
}

void RayTracer::updateAccelerationStructures()
{
	// Set instance
	XMFLOAT4X4 matrices[NUM_MESH];
	XMStoreFloat4x4(&matrices[GROUND], XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))));
	XMStoreFloat4x4(&matrices[MODEL_OBJ], XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * XMLoadFloat4x4(&m_rot) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))));
	float *const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Update top level AS
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS.Build(m_commandList, m_scratch, m_instances, descriptorPool, NumUAVs, true);
}

void RayTracer::rayTrace(uint32_t frameIndex)
{
	m_commandList.SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	m_commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	m_commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[frameIndex][UAV_TABLE_OUTPUT]);
	m_commandList.SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS);
	m_commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);
	m_commandList.SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	m_commandList.SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);

	m_commandList.DispatchRays(m_pipelines[TEST], m_viewport.x, m_viewport.y, 1,
		m_hitGroupShaderTables[frameIndex], m_missShaderTable, m_rayGenShaderTables[frameIndex]);
}
