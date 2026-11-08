// Wrapper for the ID3D11DeviceContext1.
//
// Object				OS				D3D11 version	Feature level
// ID3D11DeviceContext	Win7			11.0			11.0
// ID3D11DeviceContext1	Platform update	11.1			11.1
// ID3D11DeviceContext2	Win8.1			11.2
// ID3D11DeviceContext3					11.3

// This gives us access to every D3D11 call for a Context, and override the pieces needed.
// Hierarchy:
//  HackerContext <- ID3D11DeviceContext1 <- ID3D11DeviceContext <- ID3D11DeviceChild <- IUnknown

#include "HackerContext.h"

//#include "HookedContext.h"

// v1.44: para el breakpoint de hardware sobre el contador del pasto —
// enumerar hilos (tlhelp32) y traducir RIP a offset del exe (psapi).
#include <tlhelp32.h>
#include <psapi.h>

#include "log.h"
#include "Globals.h"

#include "HackerDevice.h"
#include "D3D11Wrapper.h"
//#include "ResourceHash.h"
//#include "Override.h"
#include "ShaderRegex.h"
#include "FrameAnalysis.h"
#include "profiling.h"

// -----------------------------------------------------------------------------------------------

HackerContext* HackerContextFactory(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1)
{
	// We can either create a straight HackerContext, or a souped up
	// FrameAnalysisContext that provides more functionality, at the cost
	// of a slight performance penalty that may marginally (1fps) reduces
	// the framerate in CPU bound games. We can't change this decision
	// later, so we need to decide here and now which it will be, and a
	// game restart will be required to change this.
	//
	// FrameAnalysisContext provides two similar, but separate features -
	// it provides the frame analysis log activated with the F8 key, and
	// can log all calls on the context to the debug log. Therefore, if
	// debug logging is enabled we clearly need the FrameAnalysisContext.
	//
	// Without the debug log we use hunting to decide whether to use the
	// FrameAnalysisContext or not - generally speaking we aim to allow
	// most of the d3dx.ini options to be changed live (where possible) if
	// hunting is enabled (=1 or 2), and that includes enabling and
	// disabling frame analysis on the fly, so if we keyed this off
	// analyse_frame instead of hunting we would be preventing that for no
	// good reason.
	//
	// It's also worth remembering that the frame_analysis key binding only
	// works when hunting=1 (partially as a safety measure, partially
	// because frame analysis resource dumping still has some dependencies
	// on stat collection), so G->hunting is already a pre-requisite for
	// frame analysis:
	if (G->hunting || gLogDebug) {
		LogInfo("  Creating FrameAnalysisContext\n");
		return new FrameAnalysisContext(pDevice1, pContext1);
	}

	LogInfo("  Creating HackerContext - frame analysis log will not be available\n");
	return new HackerContext(pDevice1, pContext1);
}

HackerContext::HackerContext(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1)
{
	mOrigDevice1 = pDevice1;
	mOrigContext1 = pContext1;
	mRealOrigContext1 = pContext1;
	mHackerDevice = NULL;

	memset(mCurrentVertexBuffers, 0, sizeof(mCurrentVertexBuffers));
	memset(mTripleScissorShadow, 0, sizeof(mTripleScissorShadow));
	mTripleScissorShadowCount = 0;
	mCurrentIndexBuffer = 0;
	mTripleIB = 0;
	mCurrentVertexShader = 0;
	mCurrentVertexShaderHandle = NULL;
	mCurrentPixelShader = 0;
	mCurrentPixelShaderHandle = NULL;
	mCurrentComputeShader = 0;
	mCurrentComputeShaderHandle = NULL;
	mCurrentGeometryShader = 0;
	mCurrentGeometryShaderHandle = NULL;
	mCurrentDomainShader = 0;
	mCurrentDomainShaderHandle = NULL;
	mCurrentHullShader = 0;
	mCurrentHullShaderHandle = NULL;
	mCurrentDepthTarget = NULL;
	mCurrentPSUAVStartSlot = 0;
	mCurrentPSNumUAVs = 0;
}


// Save the corresponding HackerDevice, as we need to use it periodically to get
// access to the StereoParams.

void HackerContext::SetHackerDevice(HackerDevice *pDevice)
{
	mHackerDevice = pDevice;
}

HackerDevice* HackerContext::GetHackerDevice()
{
	return mHackerDevice;
}

// Returns the "real" DirectX object. Note that if hooking is enabled calls
// through this object will go back into 3DMigoto, which would then subject
// them to extra logging and any processing 3DMigoto applies, which may be
// undesirable in some cases. This used to cause a crash if a command list
// issued a draw call, since that would then trigger the command list and
// recurse until the stack ran out:
ID3D11DeviceContext1* HackerContext::GetPossiblyHookedOrigContext1(void)
{
	return mRealOrigContext1;
}

// Use this one when you specifically don't want calls through this object to
// ever go back into 3DMigoto. If hooking is disabled this is identical to the
// above, but when hooking this will be the trampoline object instead:
ID3D11DeviceContext1* HackerContext::GetPassThroughOrigContext1(void)
{
	return mOrigContext1;
}

void HackerContext::HookContext()
{
	// This will install hooks in the original context (if they have not
	// already been installed from a prior context) which will call the
	// equivalent function in this HackerContext. It returns a trampoline
	// interface which we use in place of mOrigContext1 to call the real
	// original context, thereby side stepping the problem that calling the
	// old mOrigContext1 would be hooked and call back into us endlessly:
	mOrigContext1 = hook_context(mOrigContext1, this);
}

// -----------------------------------------------------------------------------


// Records the hash of this shader resource view for later lookup. Returns the
// handle to the resource, but be aware that it no longer has a reference and
// should only be used for map lookups.
ID3D11Resource* HackerContext::RecordResourceViewStats(ID3D11View *view, std::set<uint32_t> *resource_info)
{
	ID3D11Resource *resource = NULL;
	uint32_t orig_hash = 0;

	if (!view)
		return NULL;

	view->GetResource(&resource);
	if (!resource)
		return NULL;

	EnterCriticalSectionPretty(&G->mCriticalSection);

		// We are using the original resource hash for stat collection - things
		// get tricky otherwise
		orig_hash = GetOrigResourceHash(resource);

		resource->Release();

		if (orig_hash)
			resource_info->insert(orig_hash);

	LeaveCriticalSection(&G->mCriticalSection);

	return resource;
}

static ResourceSnapshot SnapshotResource(ID3D11Resource *handle)
{
	uint32_t hash = 0, orig_hash = 0;

	ResourceHandleInfo *info = GetResourceHandleInfo(handle);
	if (info) {
		hash = info->hash;
		orig_hash = info->orig_hash;
	}

	return ResourceSnapshot(handle, hash, orig_hash);
}

void HackerContext::_RecordShaderResourceUsage(ShaderInfoData *shader_info, ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT])
{
	ID3D11Resource *resource;
	int i;

	for (i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++) {
		if (views[i]) {
			resource = RecordResourceViewStats(views[i], &G->mShaderResourceInfo);
			if (resource)
				shader_info->ResourceRegisters[i].insert(SnapshotResource(resource));
			views[i]->Release();
		}
	}
}

void HackerContext::RecordPeerShaders(std::set<UINT64> *PeerShaders, UINT64 this_shader_hash)
{
	if (mCurrentVertexShader && mCurrentVertexShader != this_shader_hash)
		PeerShaders->insert(mCurrentVertexShader);

	if (mCurrentHullShader && mCurrentHullShader != this_shader_hash)
		PeerShaders->insert(mCurrentHullShader);

	if (mCurrentDomainShader && mCurrentDomainShader != this_shader_hash)
		PeerShaders->insert(mCurrentDomainShader);

	if (mCurrentGeometryShader && mCurrentGeometryShader != this_shader_hash)
		PeerShaders->insert(mCurrentGeometryShader);

	if (mCurrentPixelShader && mCurrentPixelShader != this_shader_hash)
		PeerShaders->insert(mCurrentPixelShader);
}


template <void (__stdcall ID3D11DeviceContext::*GetShaderResources)(THIS_
		UINT StartSlot,
		UINT NumViews,
		ID3D11ShaderResourceView **ppShaderResourceViews)>
void HackerContext::RecordShaderResourceUsage(std::map<UINT64, ShaderInfoData> &ShaderInfo, UINT64 currentShader)
{
	ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
	ShaderInfoData *info;

	(mOrigContext1->*GetShaderResources)(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);

	EnterCriticalSectionPretty(&G->mCriticalSection);

		info = &ShaderInfo[currentShader];
		_RecordShaderResourceUsage(info, views);
		RecordPeerShaders(&info->PeerShaders, currentShader);

	LeaveCriticalSection(&G->mCriticalSection);
}

void HackerContext::RecordGraphicsShaderStats()
{
	ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT]; // DX11: 8, DX11.1: 64
	UINT selectedRenderTargetPos;
	ShaderInfoData *info;
	ID3D11Resource *resource;
	UINT i;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	if (mCurrentVertexShader) {
		RecordShaderResourceUsage<&ID3D11DeviceContext::VSGetShaderResources>
			(G->mVertexShaderInfo, mCurrentVertexShader);
	}

	if (mCurrentHullShader) {
		RecordShaderResourceUsage<&ID3D11DeviceContext::HSGetShaderResources>
			(G->mHullShaderInfo, mCurrentHullShader);
	}

	if (mCurrentDomainShader) {
		RecordShaderResourceUsage<&ID3D11DeviceContext::DSGetShaderResources>
			(G->mDomainShaderInfo, mCurrentDomainShader);
	}

	if (mCurrentGeometryShader) {
		RecordShaderResourceUsage<&ID3D11DeviceContext::GSGetShaderResources>
			(G->mGeometryShaderInfo, mCurrentGeometryShader);
	}

	if (mCurrentPixelShader) {
		// This API is poorly designed, because we have to know the
		// current UAV start slot.
		OMGetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, mCurrentPSUAVStartSlot, mCurrentPSNumUAVs, uavs);

		RecordShaderResourceUsage<&ID3D11DeviceContext::PSGetShaderResources>
			(G->mPixelShaderInfo, mCurrentPixelShader);

		EnterCriticalSectionPretty(&G->mCriticalSection);
			info = &G->mPixelShaderInfo[mCurrentPixelShader];

			for (selectedRenderTargetPos = 0; selectedRenderTargetPos < mCurrentRenderTargets.size(); ++selectedRenderTargetPos) {
				if (selectedRenderTargetPos >= info->RenderTargets.size())
					info->RenderTargets.push_back(std::set<ResourceSnapshot>());

				info->RenderTargets[selectedRenderTargetPos].insert(SnapshotResource(mCurrentRenderTargets[selectedRenderTargetPos]));
			}

			if (mCurrentDepthTarget)
				info->DepthTargets.insert(SnapshotResource(mCurrentDepthTarget));

			if (mCurrentPSNumUAVs) {
				for (i = 0; i < mCurrentPSNumUAVs; i++) {
					if (uavs[i]) {
						resource = RecordResourceViewStats(uavs[i], &G->mUnorderedAccessInfo);
						if (resource)
							info->UAVs[i + mCurrentPSUAVStartSlot].insert(SnapshotResource(resource));

						uavs[i]->Release();
					}
				}
			}
		LeaveCriticalSection(&G->mCriticalSection);
	}

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::stat_overhead);
}

void HackerContext::RecordComputeShaderStats()
{
	ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT]; // DX11: 8, DX11.1: 64
	ShaderInfoData *info;
	D3D_FEATURE_LEVEL level = mOrigDevice1->GetFeatureLevel();
	UINT num_uavs = (level >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT);
	ID3D11Resource *resource;
	UINT i;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	mOrigContext1->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
	mOrigContext1->CSGetUnorderedAccessViews(0, num_uavs, uavs);

	EnterCriticalSectionPretty(&G->mCriticalSection);

		info = &G->mComputeShaderInfo[mCurrentComputeShader];
		_RecordShaderResourceUsage(info, srvs);

		for (i = 0; i < num_uavs; i++) {
			if (uavs[i]) {
				resource = RecordResourceViewStats(uavs[i], &G->mUnorderedAccessInfo);
				if (resource)
					info->UAVs[i].insert(SnapshotResource(resource));

				uavs[i]->Release();
			}
		}

		if (Profiling::mode == Profiling::Mode::SUMMARY)
			Profiling::end(&profiling_state, &Profiling::stat_overhead);

	LeaveCriticalSection(&G->mCriticalSection);
}

void HackerContext::RecordRenderTargetInfo(ID3D11RenderTargetView *target, UINT view_num)
{
	D3D11_RENDER_TARGET_VIEW_DESC desc;
	ID3D11Resource *resource = NULL;
	uint32_t orig_hash = 0;

	target->GetDesc(&desc);

	LogDebug("  View #%d, Format = %d, Is2D = %d\n",
		view_num, desc.Format, D3D11_RTV_DIMENSION_TEXTURE2D == desc.ViewDimension);

	target->GetResource(&resource);
	if (!resource)
		return;

	EnterCriticalSectionPretty(&G->mCriticalSection);

		// We are using the original resource hash for stat collection - things
		// get tricky otherwise
		orig_hash = GetOrigResourceHash((ID3D11Texture2D *)resource);

		resource->Release();

		if (!resource)
			goto out_unlock;

		mCurrentRenderTargets.push_back(resource);
		G->mVisitedRenderTargets.insert(resource);
		G->mRenderTargetInfo.insert(orig_hash);

out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);
}

void HackerContext::RecordDepthStencil(ID3D11DepthStencilView *target)
{
	D3D11_DEPTH_STENCIL_VIEW_DESC desc;
	ID3D11Resource *resource = NULL;
	uint32_t orig_hash = 0;

	if (!target)
		return;

	target->GetResource(&resource);
	if (!resource)
		return;

	target->GetDesc(&desc);

	EnterCriticalSectionPretty(&G->mCriticalSection);

		// We are using the original resource hash for stat collection - things
		// get tricky otherwise
		orig_hash = GetOrigResourceHash(resource);

		resource->Release();

		mCurrentDepthTarget = resource;
		G->mDepthTargetInfo.insert(orig_hash);

	LeaveCriticalSection(&G->mCriticalSection);
}

ID3D11VertexShader* HackerContext::SwitchVSShader(ID3D11VertexShader *shader)
{

	ID3D11VertexShader *pVertexShader;
	ID3D11ClassInstance *pClassInstances;
	UINT NumClassInstances = 0, i;

	// We can possibly save the need to get the current shader by saving the ClassInstances
	mOrigContext1->VSGetShader(&pVertexShader, &pClassInstances, &NumClassInstances);
	mOrigContext1->VSSetShader(shader, &pClassInstances, NumClassInstances);

	for (i = 0; i < NumClassInstances; i++)
		pClassInstances[i].Release();

	return pVertexShader;
}

ID3D11PixelShader* HackerContext::SwitchPSShader(ID3D11PixelShader *shader)
{

	ID3D11PixelShader *pPixelShader;
	ID3D11ClassInstance *pClassInstances;
	UINT NumClassInstances = 0, i;

	// We can possibly save the need to get the current shader by saving the ClassInstances
	mOrigContext1->PSGetShader(&pPixelShader, &pClassInstances, &NumClassInstances);
	mOrigContext1->PSSetShader(shader, &pClassInstances, NumClassInstances);

	for (i = 0; i < NumClassInstances; i++)
		pClassInstances[i].Release();

	return pPixelShader;
}

#define ENABLE_LEGACY_FILTERS 1
void HackerContext::ProcessShaderOverride(ShaderOverride *shaderOverride, bool isPixelShader, DrawContext *data)
{
	bool use_orig = false;

	LogDebug("  override found for shader\n");

	// We really want to start deprecating all the old filters and switch
	// to using the command list for much greater flexibility. This if()
	// will be optimised out by the compiler, but is here to remind anyone
	// looking at this that we don't want to extend this code further.
	if (ENABLE_LEGACY_FILTERS) {
		// Deprecated: The texture filtering support in the command
		// list can match oD for the depth buffer, which will return
		// negative zero -0.0 if no depth buffer is assigned.
		if (shaderOverride->depth_filter != DepthBufferFilter::NONE) {
			ID3D11DepthStencilView *pDepthStencilView = NULL;

			mOrigContext1->OMGetRenderTargets(0, NULL, &pDepthStencilView);

			// Remember - we are NOT switching to the original shader when the condition is true
			if (shaderOverride->depth_filter == DepthBufferFilter::DEPTH_ACTIVE && !pDepthStencilView) {
				use_orig = true;
			}
			else if (shaderOverride->depth_filter == DepthBufferFilter::DEPTH_INACTIVE && pDepthStencilView) {
				use_orig = true;
			}

			if (pDepthStencilView)
				pDepthStencilView->Release();

			// TODO: Add alternate filter type where the depth
			// buffer state is passed as an input to the shader
		}

		// Deprecated: Partner filtering can already be achieved with
		// the command list with far more flexibility than this allows
		if (shaderOverride->partner_hash) {
			if (isPixelShader) {
				if (mCurrentVertexShader != shaderOverride->partner_hash)
					use_orig = true;
			}
			else {
				if (mCurrentPixelShader != shaderOverride->partner_hash)
					use_orig = true;
			}
		}
	}

	RunCommandList(mHackerDevice, this, &shaderOverride->command_list, &data->call_info, false);

	if (ENABLE_LEGACY_FILTERS) {
		// Deprecated since the logic can be moved into the shaders with far more flexibility
		if (use_orig) {
			if (isPixelShader) {
				ShaderReplacementMap::iterator i = lookup_original_shader(mCurrentPixelShaderHandle);
				if (i != G->mOriginalShaders.end())
					data->oldPixelShader = SwitchPSShader((ID3D11PixelShader*)i->second);
			}
			else {
				ShaderReplacementMap::iterator i = lookup_original_shader(mCurrentVertexShaderHandle);
				if (i != G->mOriginalShaders.end())
					data->oldVertexShader = SwitchVSShader((ID3D11VertexShader*)i->second);
			}
		}
	}
}

// This function will run the ShaderRegex engine to automatically patch shaders
// on the fly and/or insert command lists on the fly. This may seem like a
// slightly unusual place to run this, but the reason is because of another
// upcoming feature that may decide to patch shaders at the last possible
// moment once the pipeline state is known, and this is the best point to do
// that. Later we can look into running the ShaderRegex engine earlier (at
// least on initial load when not hunting), but for now to keep things simpler
// we will do all the auto patching from one place.
//
// We do want to avoid replacing a shader that has already been replaced from
// ShaderFixes, either at shader creation time, or dynamically by the
// mReloadedShaders map - user replaced shaders should always take priority
// over automatically replaced shaders.
template <class ID3D11Shader,
	void (__stdcall ID3D11DeviceContext::*GetShaderVS2013BUGWORKAROUND)(ID3D11Shader**, ID3D11ClassInstance**, UINT*),
	void (__stdcall ID3D11DeviceContext::*SetShaderVS2013BUGWORKAROUND)(ID3D11Shader*, ID3D11ClassInstance*const*, UINT),
	HRESULT (__stdcall ID3D11Device::*CreateShader)(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11Shader**)
>
void HackerContext::DeferredShaderReplacement(ID3D11DeviceChild *shader, UINT64 hash, wchar_t *shader_type)
{
	ID3D11Shader *orig_shader = NULL, *patched_shader = NULL;
	ID3D11ClassInstance *class_instances[256];
	ShaderReloadMap::iterator orig_info_i;
	OriginalShaderInfo *orig_info = NULL;
	UINT num_instances = 0;
	string asm_text;
	bool patch_regex = false;
	HRESULT hr;
	unsigned i;
	wstring tagline(L"//");
	vector<byte> patched_bytecode;
	vector<char> asm_vector;

	EnterCriticalSectionPretty(&G->mCriticalSection);

	// Faster than catching an out_of_range exception from .at():
	orig_info_i = lookup_reloaded_shader(shader);
	if (orig_info_i == G->mReloadedShaders.end())
		goto out_drop;
	orig_info = &orig_info_i->second;

	if (!orig_info->deferred_replacement_candidate || orig_info->deferred_replacement_processed)
		goto out_drop;

	// Remember that we have analysed this one so we don't check it again
	// (until config reload) regardless of whether we patch it or not:
	orig_info->deferred_replacement_processed = true;

	switch (load_shader_regex_cache(hash, shader_type, &patched_bytecode, &tagline)) {
	case ShaderRegexCache::NO_MATCH:
		LogInfo("%S %016I64x has cached ShaderRegex miss\n", shader_type, hash);
		goto out_drop;
	case ShaderRegexCache::MATCH:
		LogInfo("Loaded %S %016I64x command list from ShaderRegex cache\n", shader_type, hash);
		goto out_drop;
	case ShaderRegexCache::PATCH:
		LogInfo("Loaded %S %016I64x bytecode from ShaderRegex cache\n", shader_type, hash);
		break;
	case ShaderRegexCache::NO_CACHE:
		LogInfo("Performing deferred shader analysis on %S %016I64x...\n", shader_type, hash);

		asm_text = BinaryToAsmText(orig_info->byteCode->GetBufferPointer(),
				orig_info->byteCode->GetBufferSize(),
				G->patch_cb_offsets,
				G->disassemble_undecipherable_custom_data);
		if (asm_text.empty())
			goto out_drop;

		try {
			patch_regex = apply_shader_regex_groups(&asm_text, shader_type, &orig_info->shaderModel, hash, &tagline);
		} catch (...) {
			LogInfo("    *** Exception while patching shader\n");
			goto out_drop;
		}

		if (!patch_regex) {
			LogInfo("Patch did not apply\n");
			goto out_drop;
		}

		// No longer logging this since we can output to ShaderFixes
		// via hunting if marking_actions = regex, or it could be
		// disassembled from the regex cache with cmd_Decompiler
		// LogInfo("Patched Shader:\n%s\n", asm_text.c_str());

		asm_vector.assign(asm_text.begin(), asm_text.end());

		try {
			vector<AssemblerParseError> parse_errors;
			hr = AssembleFluganWithSignatureParsing(&asm_vector, &patched_bytecode, &parse_errors);
			if (FAILED(hr)) {
				LogInfo("    *** Assembling patched shader failed\n");
				goto out_drop;
			}
			// Parse errors are currently being treated as non-fatal on
			// creation time replacement and ShaderRegex for backwards
			// compatibility (live shader reload is fatal).
			for (auto &parse_error : parse_errors)
				LogOverlay(LOG_NOTICE, "%016I64x-%S %S: %s\n",
						hash, shader_type, tagline.c_str(), parse_error.what());
		} catch (const exception &e) {
			LogOverlay(LOG_WARNING, "Error assembling ShaderRegex patched %016I64x-%S\n%S\n%s\n",
					hash, shader_type, tagline.c_str(), e.what());
			goto out_drop;
		}

		save_shader_regex_cache_bin(hash, shader_type, &patched_bytecode);
	}

	hr = (mOrigDevice1->*CreateShader)(patched_bytecode.data(), patched_bytecode.size(),
			orig_info->linkage, &patched_shader);
	CleanupShaderMaps(patched_shader);
	if (FAILED(hr)) {
		LogInfo("    *** Creating replacement shader failed\n");
		goto out_drop;
	}

	// Update replacement map so we don't have to repeat this process.
	// Not updating the bytecode in the replaced shader map - we do that
	// elsewhere, but I think that is a bug. Need to untangle that first.
	if (orig_info->replacement)
		orig_info->replacement->Release();
	orig_info->replacement = patched_shader;
	orig_info->infoText = tagline;

	// Now that we've finished updating our data structures we can drop the
	// critical section before calling into DirectX to bind the replacement
	// shader. This was necessary to avoid a deadlock with the resource
	// release tracker, but that now uses a different lock.
	LeaveCriticalSection(&G->mCriticalSection);

	// And bind the replaced shader in time for this draw call:
	// VSBUGWORKAROUND: VS2013 toolchain has a bug that mistakes a member
	// pointer called "SetShader" for the SetShader we have in
	// HackerContext, even though the member pointer we were passed very
	// clearly points to a member function of ID3D11DeviceContext. VS2015
	// toolchain does not suffer from this bug.
	(mOrigContext1->*GetShaderVS2013BUGWORKAROUND)(&orig_shader, class_instances, &num_instances);
	(mOrigContext1->*SetShaderVS2013BUGWORKAROUND)(patched_shader, class_instances, num_instances);
	if (orig_shader)
		orig_shader->Release();
	for (i = 0; i < num_instances; i++) {
		if (class_instances[i])
			class_instances[i]->Release();
	}
	return;

out_drop:
	LeaveCriticalSection(&G->mCriticalSection);
}

void HackerContext::DeferredShaderReplacementBeforeDraw()
{
	Profiling::State profiling_state;

	if (shader_regex_groups.empty())
		return;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	if (mCurrentVertexShaderHandle) {
		DeferredShaderReplacement<ID3D11VertexShader,
			&ID3D11DeviceContext::VSGetShader,
			&ID3D11DeviceContext::VSSetShader,
			&ID3D11Device::CreateVertexShader>
			(mCurrentVertexShaderHandle, mCurrentVertexShader, L"vs");
	}
	if (mCurrentHullShaderHandle) {
		DeferredShaderReplacement<ID3D11HullShader,
			&ID3D11DeviceContext::HSGetShader,
			&ID3D11DeviceContext::HSSetShader,
			&ID3D11Device::CreateHullShader>
			(mCurrentHullShaderHandle, mCurrentHullShader, L"hs");
	}
	if (mCurrentDomainShaderHandle) {
		DeferredShaderReplacement<ID3D11DomainShader,
			&ID3D11DeviceContext::DSGetShader,
			&ID3D11DeviceContext::DSSetShader,
			&ID3D11Device::CreateDomainShader>
			(mCurrentDomainShaderHandle, mCurrentDomainShader, L"ds");
	}
	if (mCurrentGeometryShaderHandle) {
		DeferredShaderReplacement<ID3D11GeometryShader,
			&ID3D11DeviceContext::GSGetShader,
			&ID3D11DeviceContext::GSSetShader,
			&ID3D11Device::CreateGeometryShader>
			(mCurrentGeometryShaderHandle, mCurrentGeometryShader, L"gs");
	}
	if (mCurrentPixelShaderHandle) {
		DeferredShaderReplacement<ID3D11PixelShader,
			&ID3D11DeviceContext::PSGetShader,
			&ID3D11DeviceContext::PSSetShader,
			&ID3D11Device::CreatePixelShader>
			(mCurrentPixelShaderHandle, mCurrentPixelShader, L"ps");
	}

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::shaderregex_overhead);
}

void HackerContext::DeferredShaderReplacementBeforeDispatch()
{
	if (shader_regex_groups.empty())
		return;

	if (!mCurrentComputeShaderHandle)
		return;

	DeferredShaderReplacement<ID3D11ComputeShader,
		&ID3D11DeviceContext::CSGetShader,
		&ID3D11DeviceContext::CSSetShader,
		&ID3D11Device::CreateComputeShader>
		(mCurrentComputeShaderHandle, mCurrentComputeShader, L"cs");
}


// En CommandList.cpp. Se declara aca y no junto a TripleCaptureCameraCB
// porque aquella esta mas abajo en el archivo que este uso.
void TripleTrackCarMesh(UINT64 vs_hash, uint32_t ib_hash, UINT index_count);

void HackerContext::BeforeDraw(DrawContext &data)
{
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	// Identificacion del auto por su malla — solo TELEMETRIA de log desde
	// v1.85 (la identidad es manual, $auto_elegido). Va ANTES del gate de
	// hunting a proposito: tiene que funcionar en juego normal, no solo
	// analizando.
	TripleTrackCarMesh(mCurrentVertexShader, mTripleIB, data.call_info.IndexCount);

	// If we are not hunting shaders, we should skip all of this shader management for a performance bump.
	if (G->hunting == HUNTING_MODE_ENABLED)
	{
		UINT selectedVertexBufferPos;
		UINT selectedRenderTargetPos;
		UINT i;

		// In some cases stat collection can have a significant
		// performance impact or may result in a runaway memory leak,
		// so only do it if dump_usage is enabled.
		if (G->DumpUsage)
			RecordGraphicsShaderStats();

		EnterCriticalSectionPretty(&G->mCriticalSection);
		{
			// Selection
			for (selectedVertexBufferPos = 0; selectedVertexBufferPos < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++selectedVertexBufferPos) {
				if (mCurrentVertexBuffers[selectedVertexBufferPos] == G->mSelectedVertexBuffer)
					break;
			}
			for (selectedRenderTargetPos = 0; selectedRenderTargetPos < mCurrentRenderTargets.size(); ++selectedRenderTargetPos) {
				if (mCurrentRenderTargets[selectedRenderTargetPos] == G->mSelectedRenderTarget)
					break;
			}
			if (mCurrentIndexBuffer == G->mSelectedIndexBuffer ||
				mCurrentVertexShader == G->mSelectedVertexShader ||
				mCurrentPixelShader == G->mSelectedPixelShader ||
				mCurrentGeometryShader == G->mSelectedGeometryShader ||
				mCurrentDomainShader == G->mSelectedDomainShader ||
				mCurrentHullShader == G->mSelectedHullShader ||
				selectedVertexBufferPos < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
				selectedRenderTargetPos < mCurrentRenderTargets.size())
			{
				LogDebug("  Skipping selected operation. CurrentIndexBuffer = %08lx, CurrentVertexShader = %016I64x, CurrentPixelShader = %016I64x\n",
					mCurrentIndexBuffer, mCurrentVertexShader, mCurrentPixelShader);

				// Snapshot render target list.
				if (G->mSelectedRenderTargetSnapshot != G->mSelectedRenderTarget)
				{
					G->mSelectedRenderTargetSnapshotList.clear();
					G->mSelectedRenderTargetSnapshot = G->mSelectedRenderTarget;
				}
				G->mSelectedRenderTargetSnapshotList.insert(mCurrentRenderTargets.begin(), mCurrentRenderTargets.end());
				// Snapshot info.
				for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
					if (mCurrentVertexBuffers[i] == G->mSelectedVertexBuffer) {
						G->mSelectedVertexBuffer_VertexShader.insert(mCurrentVertexShader);
						G->mSelectedVertexBuffer_PixelShader.insert(mCurrentPixelShader);
					}
				}
				if (mCurrentIndexBuffer == G->mSelectedIndexBuffer)
				{
					G->mSelectedIndexBuffer_VertexShader.insert(mCurrentVertexShader);
					G->mSelectedIndexBuffer_PixelShader.insert(mCurrentPixelShader);
				}
				if (mCurrentVertexShader == G->mSelectedVertexShader) {
					for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
						if (mCurrentVertexBuffers[i])
							G->mSelectedVertexShader_VertexBuffer.insert(mCurrentVertexBuffers[i]);
					}
					if (mCurrentIndexBuffer)
						G->mSelectedVertexShader_IndexBuffer.insert(mCurrentIndexBuffer);
				}
				if (mCurrentPixelShader == G->mSelectedPixelShader) {
					for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
						if (mCurrentVertexBuffers[i])
							G->mSelectedVertexShader_VertexBuffer.insert(mCurrentVertexBuffers[i]);
					}
					if (mCurrentIndexBuffer)
						G->mSelectedPixelShader_IndexBuffer.insert(mCurrentIndexBuffer);
				}
				if (G->marking_mode == MarkingMode::MONO && mHackerDevice->mStereoHandle)
				{
					LogDebug("  setting separation=0 for hunting\n");

					if (NVAPI_OK != Profiling::NvAPI_Stereo_GetSeparation(mHackerDevice->mStereoHandle, &data.oldSeparation))
						LogDebug("    Stereo_GetSeparation failed.\n");

					NvAPIOverride();
					if (NVAPI_OK != Profiling::NvAPI_Stereo_SetSeparation(mHackerDevice->mStereoHandle, 0))
						LogDebug("    Stereo_SetSeparation failed.\n");
				}
				else if (G->marking_mode == MarkingMode::SKIP)
				{
					data.call_info.skip = true;

					// If we have transferred the draw call to a custom shader via "handling =
					// skip" and "draw = from_caller" we still want a way to skip it for hunting.
					// We can't reuse call_info.skip for that, as that is also set by
					// "handling=skip", which may happen before the "draw=from_caller", so we
					// use a second skip flag specifically for hunting:
					data.call_info.hunting_skip = true;
				}
				else if (G->marking_mode == MarkingMode::PINK)
				{
					if (G->mPinkingShader)
						data.oldPixelShader = SwitchPSShader(G->mPinkingShader);
				}
			}
		}
		LeaveCriticalSection(&G->mCriticalSection);
	}

	if (!G->fix_enabled)
		goto out_profile;

	DeferredShaderReplacementBeforeDraw();

	// Override settings?
	if (!G->mShaderOverrideMap.empty()) {
		ShaderOverrideMap::iterator i;

		i = lookup_shaderoverride(mCurrentVertexShader);
		if (i != G->mShaderOverrideMap.end()) {
			data.post_commands[0] = &i->second.post_command_list;
			ProcessShaderOverride(&i->second, false, &data);
		}

		if (mCurrentHullShader) {
			i = lookup_shaderoverride(mCurrentHullShader);
			if (i != G->mShaderOverrideMap.end()) {
				data.post_commands[1] = &i->second.post_command_list;
				ProcessShaderOverride(&i->second, false, &data);
			}
		}

		if (mCurrentDomainShader) {
			i = lookup_shaderoverride(mCurrentDomainShader);
			if (i != G->mShaderOverrideMap.end()) {
				data.post_commands[2] = &i->second.post_command_list;
				ProcessShaderOverride(&i->second, false, &data);
			}
		}

		if (mCurrentGeometryShader) {
			i = lookup_shaderoverride(mCurrentGeometryShader);
			if (i != G->mShaderOverrideMap.end()) {
				data.post_commands[3] = &i->second.post_command_list;
				ProcessShaderOverride(&i->second, false, &data);
			}
		}

		i = lookup_shaderoverride(mCurrentPixelShader);
		if (i != G->mShaderOverrideMap.end()) {
			data.post_commands[4] = &i->second.post_command_list;
			ProcessShaderOverride(&i->second, true, &data);
		}
	}

out_profile:
	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::draw_overhead);
}

void HackerContext::AfterDraw(DrawContext &data)
{
	int i;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	if (data.call_info.skip)
		Profiling::skipped_draw_calls++;

	for (i = 0; i < 5; i++) {
		if (data.post_commands[i]) {
			RunCommandList(mHackerDevice, this, data.post_commands[i], &data.call_info, true);
		}
	}

	if (mHackerDevice->mStereoHandle && data.oldSeparation != FLT_MAX) {
		NvAPIOverride();
		if (NVAPI_OK != Profiling::NvAPI_Stereo_SetSeparation(mHackerDevice->mStereoHandle, data.oldSeparation))
			LogDebug("    Stereo_SetSeparation failed.\n");
	}

	if (data.oldVertexShader) {
		ID3D11VertexShader *ret;
		ret = SwitchVSShader(data.oldVertexShader);
		data.oldVertexShader->Release();
		if (ret)
			ret->Release();
	}
	if (data.oldPixelShader) {
		ID3D11PixelShader *ret;
		ret = SwitchPSShader(data.oldPixelShader);
		data.oldPixelShader->Release();
		if (ret)
			ret->Release();
	}

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::draw_overhead);
}

// -----------------------------------------------------------------------------------------------

ULONG STDMETHODCALLTYPE HackerContext::AddRef(void)
{
	return mOrigContext1->AddRef();
}


// Must set the reference that the HackerDevice uses to null, because otherwise
// we see that dead reference reused in GetImmediateContext, in FC4.

STDMETHODIMP_(ULONG) HackerContext::Release(THIS)
{
	ULONG ulRef = mOrigContext1->Release();
	LogDebug("HackerContext::Release counter=%d, this=%p\n", ulRef, this);

	if (ulRef <= 0)
	{
		LogInfo("  deleting self\n");

		if (mHackerDevice != nullptr) {
			if (mHackerDevice->GetHackerContext() == this) {
				LogInfo("  clearing mHackerDevice->mHackerContext\n");
				mHackerDevice->SetHackerContext(nullptr);
			}
		} else
			LogInfo("HackerContext::Release - mHackerDevice is NULL\n");

		delete this;
		return 0L;
	}
	return ulRef;
}

// In a strange case, Mafia 3 calls this hacky interface with the request for
// the ID3D11DeviceContext.  That's right, it calls to get the exact
// same object that it is using to call.  I swear.

HRESULT STDMETHODCALLTYPE HackerContext::QueryInterface(
	/* [in] */ REFIID riid,
	/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject)
{
	LogDebug("HackerContext::QueryInterface(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(riid).c_str());

	if (ppvObject && IsEqualIID(riid, IID_HackerContext)) {
		// This is a special case - only 3DMigoto itself should know
		// this IID, so this is us checking if it has a HackerContext.
		// There's no need to call through to DX for this one.
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	HRESULT hr = mOrigContext1->QueryInterface(riid, ppvObject);
	if (FAILED(hr))
	{
		LogDebug("  failed result = %x for %p\n", hr, ppvObject);
		return hr;
	}

	// To avoid letting the game bypass our hooked object, we need to return the 
	// HackerContext/this in this case.
	if (riid == __uuidof(ID3D11DeviceContext))
	{
		*ppvObject = this;
		LogDebug("  return HackerContext(%s@%p) wrapper of %p\n", type_name(this), this, mOrigContext1);
	}
	else if (riid == __uuidof(ID3D11DeviceContext1))
	{
		if (!G->enable_platform_update) 
		{
			LogInfo("***  returns E_NOINTERFACE as error for ID3D11DeviceContext1 (try allow_platform_update=1 if the game refuses to run).\n");
			*ppvObject = NULL;
			return E_NOINTERFACE;
		}

		// For Batman: TellTale games, they call this to fetch the DeviceContext1.
		// We need to return a hooked version as part of fleshing it out for games like this
		// that require the evil-update to run.

		// In this case, we are already an ID3D11DeviceContext1, so just return self.
		*ppvObject = this;
		LogDebug("  return HackerContext(%s@%p) wrapper of %p\n", type_name(this), this, ppvObject);
	}

	LogDebug("  returns result = %x for %p\n", hr, ppvObject);
	return hr;
}

// -----------------------------------------------------------------------------------------------

// ******************* ID3D11DeviceChild interface

// Returns our subclassed version of the Device.
// But the method signature cannot be altered.
// Since we can't alter what the object has stored, this returns just
// the superclass call.  If the object was created correctly, that should
// be a HackerDevice object.
// The previous version of this call would fetch the HackerDevice from a list
// and thus this new approach may be broken.

STDMETHODIMP_(void) HackerContext::GetDevice(THIS_
	/* [annotation] */
	__out  ID3D11Device **ppDevice)
{
	LogDebug("HackerContext::GetDevice(%s@%p) returns %p\n", type_name(this), this, mHackerDevice);

	// Fix ref counting bug that slowly eats away at the device until we
	// crash. In FC4 this can happen after about 10 minutes, or when
	// running in windowed mode during launch.
	
	// Follow our rule of always calling the original call first to ensure that
	// any side-effects (including ref counting) are activated.
	mOrigContext1->GetDevice(ppDevice);

	// Return our wrapped device though.
	if (!(G->enable_hooks & EnableHooks::DEVICE))
		*ppDevice = mHackerDevice;
}

STDMETHODIMP HackerContext::GetPrivateData(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__inout  UINT *pDataSize,
	/* [annotation] */
	__out_bcount_opt(*pDataSize)  void *pData)
{
	LogDebug("HackerContext::GetPrivateData(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(guid).c_str());

	HRESULT hr = mOrigContext1->GetPrivateData(guid, pDataSize, pData);
	LogDebug("  returns result = %x, DataSize = %d\n", hr, *pDataSize);

	return hr;
}

STDMETHODIMP HackerContext::SetPrivateData(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__in  UINT DataSize,
	/* [annotation] */
	__in_bcount_opt(DataSize)  const void *pData)
{
	LogInfo("HackerContext::SetPrivateData(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(guid).c_str());
	LogInfo("  DataSize = %d\n", DataSize);

	HRESULT hr = mOrigContext1->SetPrivateData(guid, DataSize, pData);
	LogInfo("  returns result = %x\n", hr);

	return hr;
}

STDMETHODIMP HackerContext::SetPrivateDataInterface(THIS_
	/* [annotation] */
	__in  REFGUID guid,
	/* [annotation] */
	__in_opt  const IUnknown *pData)
{
	LogInfo("HackerContext::SetPrivateDataInterface(%s@%p) called with IID: %s\n", type_name(this), this, NameFromIID(guid).c_str());

	HRESULT hr = mOrigContext1->SetPrivateDataInterface(guid, pData);
	LogInfo("  returns result = %x\n", hr);

	return hr;
}

// -----------------------------------------------------------------------------------------------

// ******************* ID3D11DeviceContext interface

// These first routines all the boilerplate ones that just pass through to the original context.
// They need to be here in order to pass along the calls, since there is no proper object where
// it would normally go to the superclass. 

// v1.63 M1: buffer bindeado en VS slot 3 (cb3, camara) y PS slot 0 (cb0/$Globals),
// por contexto. Los Get* mienten en contextos diferidos (regla 3 de CLAUDE.md),
// por eso se sombrea aca. El snapshot de su contenido y la medicion viven mas
// abajo (cb_snap_* / triple_medir_scatter).
static thread_local ID3D11Resource *tls_vs_cb3 = NULL;
static thread_local ID3D11Resource *tls_ps_cb0 = NULL;
static thread_local ID3D11Resource *tls_ps_cb5 = NULL;	// v1.70: ShadowParameters

STDMETHODIMP_(void) HackerContext::VSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	// v1.63 M1: sombrear el buffer del VS slot 3 (cb3, camara). Se usa para medir
	// el fovH de la siembra del pasto en el draw del scatter VS.
	if (ppConstantBuffers && StartSlot <= 3 && StartSlot + NumBuffers > 3)
		tls_vs_cb3 = ppConstantBuffers[3 - StartSlot];

	mOrigContext1->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

bool HackerContext::MapDenyCPURead(
	ID3D11Resource *pResource,
	UINT Subresource,
	D3D11_MAP MapType,
	UINT MapFlags,
	D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
	uint32_t hash;
	TextureOverrideMap::iterator i;

	// Currently only replacing first subresource to simplify map type, and
	// only on read access as it is unclear how to handle a read/write access.
	if (Subresource != 0)
		return false;

	if (G->mTextureOverrideMap.empty())
		return false;

	EnterCriticalSectionPretty(&G->mCriticalSection);
		hash = GetResourceHash(pResource);
	LeaveCriticalSection(&G->mCriticalSection);

	i = lookup_textureoverride(hash);
	if (i == G->mTextureOverrideMap.end())
		return false;

	return i->second.begin()->deny_cpu_read;
}

void HackerContext::TrackAndDivertMap(HRESULT map_hr, ID3D11Resource *pResource,
		UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
		D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
	D3D11_RESOURCE_DIMENSION dim;
	ID3D11Buffer *buf = NULL;
	ID3D11Texture1D *tex1d = NULL;
	ID3D11Texture2D *tex2d = NULL;
	ID3D11Texture3D *tex3d = NULL;
	D3D11_BUFFER_DESC buf_desc;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;
	MappedResourceInfo *map_info = NULL;
	void *replace = NULL;
	bool divertable = false, divert = false, track = false;
	bool write = false, read = false, deny = false;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	if (FAILED(map_hr) || !pResource || !pMappedResource || !pMappedResource->pData)
		goto out_profile;

	switch (MapType) {
		case D3D11_MAP_READ_WRITE:
			read = true;
			// Fall through
		case D3D11_MAP_WRITE_DISCARD:
			divertable = true;
			// Fall through
		case D3D11_MAP_WRITE:
		case D3D11_MAP_WRITE_NO_OVERWRITE:
			write = true;
			// We can't divert these last two since we have no way
			// to know which addresses the application wrote to,
			// and trying anyway crashes FC4. We still need the
			// hash tracking code to run on these though (necessary
			// for FC4), so we still go ahead and track the
			// mapping. We might actually be able to get rid of
			// diverting altogether for all these and only use
			// tracking - seems like it might be safe to read from
			// all these IO mapped addresses, but not sure about
			// performance or if there might be any unintended
			// consequences like uninitialised data:
			divert = track = MapTrackResourceHashUpdate(pResource, Subresource);
			break;

		case D3D11_MAP_READ:
			read = divertable = true;
			divert = deny = MapDenyCPURead(pResource, Subresource, MapType, MapFlags, pMappedResource);
			break;
	}

	if (!track && !divert)
		goto out_profile;

	map_info = &mMappedResources[pResource];
	map_info->mapped_writable = write;
	memcpy(&map_info->map, pMappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));

	if (!divertable || !divert)
		goto out_profile;

	pResource->GetType(&dim);
	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			buf = (ID3D11Buffer*)pResource;
			buf->GetDesc(&buf_desc);
			map_info->size = buf_desc.ByteWidth;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)pResource;
			tex1d->GetDesc(&tex1d_desc);
			map_info->size = dxgi_format_size(tex1d_desc.Format) * tex1d_desc.Width;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)pResource;
			tex2d->GetDesc(&tex2d_desc);
			map_info->size = pMappedResource->RowPitch * tex2d_desc.Height;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)pResource;
			tex3d->GetDesc(&tex3d_desc);
			map_info->size = pMappedResource->DepthPitch * tex3d_desc.Depth;
			break;
		default:
			goto out_profile;
	}

	replace = malloc(map_info->size);
	if (!replace) {
		LogInfo("TrackAndDivertMap out of memory\n");
		goto out_profile;
	}

	if (read && !deny)
		memcpy(replace, pMappedResource->pData, map_info->size);
	else
		memset(replace, 0, map_info->size);

	map_info->orig_pData = pMappedResource->pData;
	map_info->map.pData = replace;
	pMappedResource->pData = replace;

out_profile:
	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::map_overhead);
}

void HackerContext::TrackAndDivertUnmap(ID3D11Resource *pResource, UINT Subresource)
{
	MappedResources::iterator i;
	MappedResourceInfo *map_info = NULL;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	if (mMappedResources.empty())
		goto out_profile;

	i = mMappedResources.find(pResource);
	if (i == mMappedResources.end())
		goto out_profile;
	map_info = &i->second;

	if (G->track_texture_updates == 1 && Subresource == 0 && map_info->mapped_writable)
		UpdateResourceHashFromCPU(pResource, map_info->map.pData, map_info->map.RowPitch, map_info->map.DepthPitch);

	if (map_info->orig_pData) {
		// TODO: Measure performance vs. not diverting:
		if (map_info->mapped_writable)
			memcpy(map_info->orig_pData, map_info->map.pData, map_info->size);

		free(map_info->map.pData);
	}

	mMappedResources.erase(i);

out_profile:
	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::map_overhead);
}

// ===== v1.44: BREAKPOINT DE HARDWARE sobre el contador del pasto ============
//
// A donde llegamos: el pasto (ground cover) se dibuja con draw indirecto, y la
// cantidad sale de g_gcMeshCountsBuffer, que llena la CPU. Ese contador ES el
// resultado del culling. No pasa por planos en memoria (abrimos los 4 frustums
// vivos y no se inmuta), ni por constant buffers, ni se puede destapar desde el
// draw (forzar maxInstances no revelo nada: la geometria directamente no se
// siembra). Lo unico que queda es preguntarle al procesador QUIEN escribe ese
// contador.
//
// Como: los registros de depuracion (DR0-DR3) del x86 disparan una excepcion
// cuando se escribe una direccion concreta. Se arma DR0 sobre el buffer en el
// momento en que el juego lo mapea, y un Vectored Exception Handler captura el
// RIP de la instruccion culpable. Con ese offset dentro de dirtrally2.exe se
// llega a la funcion de siembra y a su test de culling.
//
// Los DR son POR HILO, asi que hay que sembrarlos en todos los hilos del
// proceso. Es one-shot: apenas junta unos hits se desarma solo. Diagnostico
// puro — no cambia nada de lo que el juego hace.

static thread_local ID3D11Resource *tls_cs_srv[2] = { NULL, NULL };
// El scatter de ground cover lee ZoneData desde t20. El diagnostico historico
// solo sombreaba t0..t7 (alcanzaba para el draw final de clutter), por lo que
// no podia demostrar que buffer de zonas alimentaba el draw de siembra.
static thread_local ID3D11Resource *tls_vs_srv[32] = { NULL };
// v1.50: volcado del buffer del pasto, una sola vez por sesion.
static thread_local bool tls_triple_dump_gc = false;
static thread_local ID3D11Resource *tls_triple_dump_gc_res = NULL;
static thread_local void *tls_triple_dump_gc_ptr = NULL;
static thread_local UINT tls_triple_dump_gc_size = 0;
static thread_local UINT tls_triple_dump_gc_stride = 0;
static thread_local void *tls_gc_meshinfo_ptr = NULL;
static thread_local UINT tls_gc_meshinfo_size = 0;
static thread_local int tls_gc_counts_uav_slot = -1;
static ID3D11Resource *g_gc_counts_res = NULL;		// g_gcMeshCountsBuffer
// v1.46: EL objetivo de verdad. El VS del pasto lee sus instancias de t1
// (stride 112) y NO descarta ninguna: proyecta todo lo que le den (se leyo el
// ASM entero). El contador tampoco es el filtro — nadie lo escribe en todo un
// frame, o sea que es estatico. Por descarte, el culling del pasto esta en
// QUIEN LLENA ESTE BUFFER. Ahi va la trampa.
static ID3D11Resource *g_gc_instances_res = NULL;	// g_gcInstanceBuffer
static ID3D11Resource *g_gc_meshinfo_res = NULL;	// g_gcMeshNumIndicesBuffer

// v1.45: quien toca el contador del pasto. Un solo reporte por via/shader.
static void gc_reportar(const char *via, UINT64 hash)
{
	static UINT64 vistos[24];
	static int n = 0;
	static volatile LONG lock = 0;
	while (InterlockedCompareExchange(&lock, 1, 0) != 0)
		YieldProcessor();
	UINT64 clave = hash ^ (UINT64)(uintptr_t)via;
	for (int i = 0; i < n; i++) {
		if (vistos[i] == clave) {
			InterlockedExchange(&lock, 0);
			return;
		}
	}
	if (n < 24) {
		vistos[n++] = clave;
		LogInfo("CULLBP: el contador del pasto lo escribe %s (cs=%016llX)\n",
			via, hash);
	}
	InterlockedExchange(&lock, 0);
}
// ===========================================================================
//  BREAKPOINTS POR HARDWARE — COMPILADOS AFUERA A PROPOSITO (2026-08-12)
// ===========================================================================
//
//  Lo que hay abajo instala un breakpoint de hardware para encontrar quien
//  escribe los planos de culling: enumera los hilos del proceso, los suspende,
//  les escribe los registros DR0-DR7 y los reanuda. Es, literalmente, lo que
//  hace un debugger.
//
//  Ya estaba INERTE (`CULL_BP_ACTIVO 0`) pero seguia LINKEADO: el `d3d11.dll`
//  que se distribuye importaba `SetThreadContext`, `SuspendThread`, `OpenThread`
//  y `CreateToolhelp32Snapshot`. De esos, `SetThreadContext` **no existe en el
//  3Dmigoto original** — era exclusivamente nuestro.
//
//  Un escaneo estatico no ejecuta nada: ve un modulo grafico que importa la API
//  de manipulacion de contextos de hilo, y eso solo ya llama la atencion. Como
//  el codigo no corre, sacarlo del binario no cuesta nada; dejarlo adentro si
//  costaba.
//
//  El fuente se conserva porque fue la unica forma de encontrar la funcion del
//  engine que reconstruye los planos. Si alguna vez hay que volver a cazar algo
//  asi, esto es el como — ver `docs/MEMORIA.md`.
//
#define CULL_BP_ACTIVO 0

// Esta queda AFUERA del bloque: la lee `UpdateSubresource` para no pedir la
// trampa dos veces, y ese no es codigo de depuracion. Es un LONG y no arrastra
// ningun import.
static volatile LONG g_bp_estado = 0;	// 0=dormido 1=armando 2=armado 3=terminado

#if CULL_BP_ACTIVO

static void *g_bp_addr = NULL;
static PVOID g_bp_veh = NULL;
static volatile LONG g_bp_hits = 0;
// v1.48: la caceria es en dos escalones. Fase 1 = trampa en el buffer que se
// sube a GPU (cae en el memcpy que lo llena). Fase 2 = trampa en el ORIGEN de
// ese memcpy, que es donde el generador dejo las instancias ya culleadas.
static volatile LONG g_bp_fase = 1;
static volatile LONG g_bp_repostear = 0;
static void *g_bp_origen = NULL;

// Vuelca las direcciones de retorno plausibles del stack: los qwords que caen
// dentro de dirtrally2.exe son, casi siempre, la cadena de llamadas. Es lo que
// convierte "una instruccion suelta" en "esta funcion llama a esta otra".
static void bp_volcar_pila(const CONTEXT *ctx, uintptr_t base, uintptr_t fin)
{
	const uintptr_t *sp = (const uintptr_t *)ctx->Rsp;
	int encontrados = 0;
	for (int i = 0; i < 96 && encontrados < 12; i++) {
		uintptr_t v = 0;
		__try {
			v = sp[i];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			break;
		}
		if (v > base && v < fin) {
			encontrados++;
			LogInfo("CULLBP     retorno #%d: dirtrally2.exe+0x%llX\n",
				encontrados, (unsigned long long)(v - base));
		}
	}
}

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
	if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;
	// bit 0 de DR6 = disparo DR0 (el nuestro). Cualquier otro no nos incumbe.
	if (!(ep->ContextRecord->Dr6 & 0x1))
		return EXCEPTION_CONTINUE_SEARCH;

	LONG n = InterlockedIncrement(&g_bp_hits);
	if (n <= 4) {
		HMODULE hm = GetModuleHandleA(NULL);
		uintptr_t base = (uintptr_t)hm, fin = base + 0x2000000;
		MODULEINFO mi;
		if (GetModuleInformation(GetCurrentProcess(), hm, &mi, sizeof(mi)))
			fin = base + mi.SizeOfImage;
		const CONTEXT *c = ep->ContextRecord;
		uintptr_t rip = (uintptr_t)c->Rip;
		LogInfo("CULLBP[fase %d] hit #%d: dirtrally2.exe+0x%llX (hilo %u)\n",
			(int)g_bp_fase, n, (unsigned long long)(rip - base),
			GetCurrentThreadId());
		// v1.48: los registros. El hit de la v1.47 cayo en un memcpy en linea
		// (movups de a 128 bytes), o sea que NO es el culler: es la subida de
		// datos ya decididos. RBX/RSI apuntan al ORIGEN de esa copia — la
		// memoria donde el generador dejo las instancias. Ese es el objetivo
		// de la fase 2.
		LogInfo("CULLBP     rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
			(void *)c->Rax, (void *)c->Rbx, (void *)c->Rcx,
			(void *)c->Rdx, (void *)c->Rsi, (void *)c->Rdi);
		// La pila cruda con indices: sin esto no se distingue la direccion de
		// retorno real de los restos de llamadas anteriores.
		const uintptr_t *sp = (const uintptr_t *)c->Rsp;
		for (int i = 0; i < 12; i++) {
			uintptr_t v = 0;
			__try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
			if (v > base && v < fin)
				LogInfo("CULLBP     [rsp+0x%02X] = dirtrally2.exe+0x%llX\n",
					i * 8, (unsigned long long)(v - base));
		}
		if (g_bp_fase == 1 && n == 1) {
			// Fase 2: mover la trampa al ORIGEN de la copia. Quien escriba ahi
			// es el que genera las instancias — y por lo tanto el que cullea.
			//
			// Se escala con el PRIMER hit a proposito: el buffer de GPU se
			// mapea con WRITE_DISCARD, o sea direccion nueva cada frame, asi
			// que la trampa de fase 1 dispara UNA sola vez y esperar un segundo
			// hit deja la caceria colgada (paso en la v1.48).
			//
			// La direccion: el bucle arranca con rbx = this+0x60 y avanza de a
			// 0x80. rdi es ese `this` (verificado: rbx == rdi+0xE0 en el hit
			// tras una vuelta). Se apunta a this+0x60 —el principio del bloque
			// de instancias— en vez de al rbx del momento, que ya avanzo y
			// podria caer fuera si el bloque se recorre distinto.
			g_bp_origen = (void *)(c->Rdi + 0x60);
			LogInfo("CULLBP     -> objeto del pasto en %p, instancias desde "
				"%p (this+0x60)\n", (void *)c->Rdi, g_bp_origen);
			InterlockedExchange(&g_bp_repostear, 1);
		}
	}
	ep->ContextRecord->Dr6 = 0;
	if (n >= 4) {
		ep->ContextRecord->Dr7 = 0;
		InterlockedExchange(&g_bp_estado, 3);
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Siembra DR0 en todos los hilos del proceso. Los DR son estado por hilo: si
// solo lo pusieramos en el hilo que llama a Map, nos perderiamos al que de
// verdad escribe si fuera otro (el culling suele vivir en el hilo de update).
static void bp_armar(void *addr)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	DWORD pid = GetCurrentProcessId(), yo = GetCurrentThreadId();
	int armados = 0;

	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID != pid)
				continue;
			HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
				THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
			if (!h)
				continue;
			bool otro = (te.th32ThreadID != yo);
			if (otro)
				SuspendThread(h);
			CONTEXT ctx;
			memset(&ctx, 0, sizeof(ctx));
			ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(h, &ctx)) {
				ctx.Dr0 = (DWORD64)(uintptr_t)addr;
				ctx.Dr6 = 0;
				// L0=1 (activo local) | RW0=01 (romper al ESCRIBIR) |
				// LEN0=11 (4 bytes, que es el tamano del contador)
				ctx.Dr7 = (ctx.Dr7 & ~(DWORD64)0xF0003) | 0x1 | (0x1 << 16) | (0x3 << 18);
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (SetThreadContext(h, &ctx))
					armados++;
			}
			if (otro)
				ResumeThread(h);
			CloseHandle(h);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	LogInfo("CULLBP: trampa (fase %d) puesta en %p sobre %d hilo(s).\n",
		(int)g_bp_fase, addr, armados);
}

// Pasar a la fase 2: mover la trampa al origen de la copia. NO se puede hacer
// desde el manejador de excepciones (bp_armar suspende hilos y el handler corre
// dentro de uno de ellos: deadlock asegurado), asi que el handler solo deja la
// bandera y el cambio se hace desde el hilo de render.
static void bp_repostear_si_toca()
{
	if (InterlockedCompareExchange(&g_bp_repostear, 0, 1) != 1)
		return;
	LogInfo("CULLBP: === FASE 2 === el hit anterior era un memcpy; muevo la "
		"trampa al ORIGEN de la copia (%p) para cazar a quien GENERA las "
		"instancias.\n", g_bp_origen);
	InterlockedExchange(&g_bp_fase, 2);
	InterlockedExchange(&g_bp_hits, 0);
	bp_armar(g_bp_origen);
}

// ⚠️ APAGADO (v1.54). El breakpoint de hardware cumplió su papel de descarte y
// dejó de hacer falta, pero **hacía crashear el juego al salir**: los DR quedan
// armados sobre memoria que se libera en el cierre, y el manejador de
// excepciones sigue instalado mientras el proceso se desarma. El usuario vio
// "Access violation ... dirtrally2.exe + 0xc830f7" al cerrar.
// Si alguna vez vuelve a hacer falta, hay que desarmar los DR y quitar el VEH
// en el DllMain de descarga (o en un hook de salida) antes de reactivarlo.
#endif   // CULL_BP_ACTIVO

// Con los breakpoints compilados afuera esto no hace nada, pero el llamador
// (`Map`) no tiene por que enterarse.
#if !CULL_BP_ACTIVO
static void bp_repostear_si_toca() {}
#endif

// Llamado desde Map cuando el juego mapea el buffer del contador del pasto.
static void bp_intentar(void *addr)
{
#if !CULL_BP_ACTIVO
	(void)addr;
	return;
#else
	if (InterlockedCompareExchange(&g_bp_estado, 1, 0) != 0)
		return;
	g_bp_addr = addr;
	if (!g_bp_veh)
		g_bp_veh = AddVectoredExceptionHandler(1, bp_handler);
	if (!g_bp_veh) {
		LogInfo("CULLBP: no pude instalar el manejador de excepciones.\n");
		InterlockedExchange(&g_bp_estado, 3);
		return;
	}
	bp_armar(addr);
	InterlockedExchange(&g_bp_estado, 2);
#endif
}

// ---- Triple-monitor: capture the camera constant buffer at write time -------
// Reading the game's cb3 via a GPU copy+Map(READ) is illegal on deferred
// contexts (DiRT Rally 2.0 uses 7 of them), so instead we snoop the CPU-side
// data the game writes into a constant buffer via Map/Unmap. Each context is
// used by a single thread, so thread_local tracking is race-free.
void TripleCaptureCameraCB(const void *data, unsigned size);  // in CommandList.cpp
void TripleRestaurarM00(void *data, unsigned size);          // in CommandList.cpp (bit 64 de $cull_widen)
int TripleScanPlanesInCB(void *data, unsigned size, void *res_id,
	unsigned bindFlags, int allow_patch);                     // in CommandList.cpp
int TripleGetCameraPos(float *out3);                          // in CommandList.cpp
int TripleProjectMainCamera(const float *world3, float *out4);// in CommandList.cpp
static thread_local ID3D11Resource *tls_triple_map_res = NULL;
static thread_local void *tls_triple_map_ptr = NULL;
static thread_local UINT tls_triple_map_size = 0;

// v1.39: buffers mapeados para escritura que podrian traer planos de frustum
// (culling en GPU). Se apuntan en Map y se escanean/parchean en Unmap, antes
// de que la GPU vea los datos. Por hilo (cada contexto diferido es un hilo).
#define TRIPLE_SNIFF_MAX 8
static thread_local ID3D11Resource *tls_sniff_res[TRIPLE_SNIFF_MAX];
static thread_local void *tls_sniff_ptr[TRIPLE_SNIFF_MAX];
static thread_local UINT tls_sniff_size[TRIPLE_SNIFF_MAX];
static thread_local UINT tls_sniff_bind[TRIPLE_SNIFF_MAX];
static thread_local int tls_sniff_n = 0;

static bool triple_buffer_desc(ID3D11Resource *res, D3D11_BUFFER_DESC *out)
{
	if (!res)
		return false;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
		return false;
	ID3D11Buffer *buf = NULL;
	if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf)) || !buf)
		return false;
	buf->GetDesc(out);
	buf->Release();
	return true;
}

// ===== v1.63: MEDICION M1 — ¿el gate del pasto es el rasterizador o el PS? ====
//
// El pasto se siembra con: VS scatter 3518fc8325dc58fb (proyecta con cb3[9..12])
// + PS a8b1f59dc16a9a33 (testea contra cb0[19..22]). El corte a ±75° puede venir
// de cualquiera de los dos, y ambos dan el mismo angulo si las dos matrices son
// mono. Para elegir el fix hay que saber el fovH de CADA una.
//
// Se sombrea el buffer bindeado en VS slot 3 y PS slot 0, se snapshotea el
// contenido de los constant buffers en Unmap/UpdateSubresource, y en el draw de
// la siembra se vuelca m00/m11 -> fovH/fovV de ambas. SOLO LECTURA.
//
// Del descriptor de camara del juego sabemos: m00=0.265 -> fovH=150.3,
// m11=1.428 -> fovV=70. Si el cb3 del scatter da ~150 -> gate = rasterizador
// (hay que ensanchar la siembra). Si da mas ancho -> gate = PS.

#include <math.h>

// v1.70: f[256] (antes 128) para que quepa cb5=ShadowParameters (232 floats,
// 928 bytes) y poder medir las cascadas de sombra.
struct CBSnap { ID3D11Resource *buf; float f[256]; UINT n; };
#define CB_SNAP_MAX 96
static CBSnap g_cb_snap[CB_SNAP_MAX];
static volatile LONG g_cb_snap_lock = 0;
static int g_cb_snap_next = 0;

static void cb_snap_store(ID3D11Resource *buf, const void *data, UINT bytes)
{
	if (!buf || !data)
		return;
	static volatile LONG anunciado = 0;
	if (InterlockedCompareExchange(&anunciado, 1, 0) == 0)
		LogInfo("CULLDIAG: build experimental de medicion pasiva activo.\n");
	UINT n = bytes / 4;
	if (n > 256) n = 256;	// v1.70: cabe cb5 (232 floats)
	while (InterlockedCompareExchange(&g_cb_snap_lock, 1, 0) != 0)
		YieldProcessor();
	int slot = -1;
	for (int i = 0; i < CB_SNAP_MAX; i++)
		if (g_cb_snap[i].buf == buf) { slot = i; break; }
	if (slot < 0) { slot = g_cb_snap_next; g_cb_snap_next = (g_cb_snap_next + 1) % CB_SNAP_MAX; }
	__try {
		memcpy(g_cb_snap[slot].f, data, n * 4);
		g_cb_snap[slot].buf = buf;
		g_cb_snap[slot].n = n;
		// v1.72: cb5=ShadowParameters mide EXACTO 928 bytes (58 regs). Volcar
		// sus cascadas AQUI mismo (una vez), apenas se sube, sin depender de que
		// el draw del ShadowMask dispare ni de que el snapshot sobreviva. Es la
		// medicion robusta de las cascadas de sombra.
		if (bytes == 928) {
			// v1.73: NO agarrar el primero (sale en ceros, sin inicializar, como
			// paso con la tabla del pasto). Volcar solo cuando tenga datos REALES
			// de cascada: numCascades plausible Y clip2depth[0] no todo cero.
			const float *f = g_cb_snap[slot].f;
			int nc = (int)f[229];
			bool plausible = (nc >= 2 && nc <= 4) &&
				(f[128] != 0.0f || f[132] != 0.0f || f[136] != 0.0f ||
				 f[140] != 0.0f || f[144] != 0.0f);
			static volatile LONG dumped = 0;
			if (plausible && InterlockedCompareExchange(&dumped, 1, 0) == 0) {
				LogInfo("SOMBRAMED === cb5=ShadowParameters (928 B) : %d cascadas "
					"| config[57]={%.4f %.4f %.4f %.4f}\n",
					nc, f[228], f[229], f[230], f[231]);
				for (int c = 0; c < nc; c++) {
					int fb = 128 + 16 * c;
					LogInfo("SOMBRAMED  cascada %d clip2depth U{%.5f %.5f %.5f %.5f}"
						" V{%.5f %.5f %.5f %.5f} | clip(near=%.2f far=%.2f) "
						"blendDist=%.2f\n", c,
						f[fb], f[fb + 4], f[fb + 8], f[fb + 12],
						f[fb + 1], f[fb + 5], f[fb + 9], f[fb + 13],
						f[192 + 4 * c], f[193 + 4 * c], f[216 + c]);
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	InterlockedExchange(&g_cb_snap_lock, 0);
}

static bool cb_snap_find(ID3D11Resource *buf, float *out, UINT need)
{
	if (!buf)
		return false;
	bool ok = false;
	while (InterlockedCompareExchange(&g_cb_snap_lock, 1, 0) != 0)
		YieldProcessor();
	for (int i = 0; i < CB_SNAP_MAX; i++) {
		if (g_cb_snap[i].buf == buf && g_cb_snap[i].n >= need) {
			memcpy(out, g_cb_snap[i].f, need * 4);
			ok = true;
			break;
		}
	}
	InterlockedExchange(&g_cb_snap_lock, 0);
	return ok;
}

// Vuelca el FOV de una matriz de proyeccion column-major cuyos m00/m11 estan en
// f[i00] y f[i11]. fovH = 2*atan(1/m00), fovV = 2*atan(1/m11).
static void medir_fov(const char *quien, const float *f, int i00, int i11)
{
	float m00 = f[i00], m11 = f[i11];
	float fovH = (m00 > 1e-4f) ? 2.0f * atanf(1.0f / m00) * 57.29578f : -1.0f;
	float fovV = (m11 > 1e-4f) ? 2.0f * atanf(1.0f / m11) * 57.29578f : -1.0f;
	LogInfo("CULLMED   %s: m00=%.5f m11=%.5f -> fovH=%.2f fovV=%.2f "
		"| matriz[%d..%d]={%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f}\n",
		quien, m00, m11, fovH, fovV, i00, i11 + 3,
		f[i00], f[i00 + 1], f[i00 + 2], f[i00 + 3],
		f[i11 - 1], f[i11], f[i11 + 1], f[i11 + 2]);
}

// Llamado desde los draws: si el VS actual es el scatter del pasto, vuelca una
// vez el fovH del cb3 (VS) y del cb0 (PS). Eso zanja rasterizador-vs-PS.
static void triple_medir_scatter(UINT64 vs_hash)
{
	if (vs_hash != 0x3518fc8325dc58fbULL)
		return;
	static volatile LONG veces = 0;
	if (InterlockedIncrement(&veces) > 3)
		return;

	LogInfo("CULLMED === draw de siembra del pasto (scatter VS 3518fc8) ===\n");
	float f[128];
	// cb3[9..12]: m00 = cb3[9].x = float 36 ; m11 = cb3[10].y = float 41
	if (cb_snap_find(tls_vs_cb3, f, 52))
		medir_fov("VS cb3[9..12] (proyeccion de siembra)", f, 36, 41);
	else
		LogInfo("CULLMED   VS cb3 no capturado (buf=%p). Se actualiza por otra "
			"via?\n", (void *)tls_vs_cb3);
	// cb0[19..22]: m00 = cb0[19].x = float 76 ; m11 = cb0[20].y = float 81
	if (cb_snap_find(tls_ps_cb0, f, 92))
		medir_fov("PS cb0[19..22] (test de frustum del PS)", f, 76, 81);
	else
		LogInfo("CULLMED   PS cb0 no capturado (buf=%p).\n", (void *)tls_ps_cb0);
	LogInfo("CULLMED   ref camara mono del juego: fovH=150.31 fovV=70.00. "
		"Si el VS cb3 da ~150 -> gate = RASTERIZADOR (ensanchar siembra); "
		"si da mas ancho -> gate = PS.\n");
}

// ===== v1.70: MEDICION de las cascadas de sombra (cb5=ShadowParameters) =======
//
// Las sombras desaparecen en los laterales a distancia porque las CASCADAS de
// sombra estan ajustadas al frustum mono ±75°: mas alla, ninguna cascada cubre
// el pixel -> salida=1=sin sombra. El ajuste vive en cb5 (slot b5, 928 bytes =
// 232 floats): clip2depth[c] (regs 32+4c..35+4c = floats 128+16c..) codifica la
// caja XY de cada cascada; numCascades=(int)f[229]; splits de profundidad en
// f[192+4c]/f[193+4c]; umbral de distancia (blend) en f[216+c].
//
// Esta medicion SOLO VUELCA los datos crudos de cada cascada (una vez por
// shader) para calcular offline exactamente cuanto y cual ensanchar. NO
// modifica nada. SEH, sin objetos C++, imposible de crashear.

static bool ps_es_shadowmask(UINT64 h)
{
	// 8 hashes (se quito a006a3e7a8141b30: es TDepthBufferMinMax, no ShadowMask).
	static const UINT64 H[8] = {
		0xf73d7cade6a47680ULL, 0xf9651d4a758602bbULL, 0x04993ade0791e26eULL,
		0x26ff02d2e14c5266ULL, 0x0b28e88c06b5ca31ULL, 0xbfb33192c8120414ULL,
		0x9cca7d03aa24d9f2ULL, 0xb64fa09f1ee479bbULL };
	for (int i = 0; i < 8; i++)
		if (H[i] == h)
			return true;
	return false;
}

static void triple_medir_sombras(UINT64 ps)
{
	if (!ps_es_shadowmask(ps))
		return;
	// one-shot POR HASH (el mod redibuja cada ShadowMask 3x por region; un tope
	// global se lo comeria el primer shader).
	static UINT64 vistos[16];
	static int nv = 0;
	static volatile LONG lk = 0;
	bool nuevo = false;
	while (InterlockedCompareExchange(&lk, 1, 0) != 0)
		YieldProcessor();
	bool seen = false;
	for (int i = 0; i < nv; i++)
		if (vistos[i] == ps) { seen = true; break; }
	if (!seen && nv < 16) { vistos[nv++] = ps; nuevo = true; }
	InterlockedExchange(&lk, 0);
	if (!nuevo)
		return;

	float f[256];
	if (!cb_snap_find(tls_ps_cb5, f, 232)) {
		LogInfo("SOMBRAMED PS %016llX: cb5 no capturado (buf=%p)\n",
			(unsigned long long)ps, (void *)tls_ps_cb5);
		return;
	}
	__try {
		int nc = (int)f[229];
		if (nc < 1 || nc > 4)
			nc = 4;
		LogInfo("SOMBRAMED === PS %016llX : %d cascadas | config[57]="
			"{%.4f %.4f %.4f %.4f}\n", (unsigned long long)ps, nc,
			f[228], f[229], f[230], f[231]);
		for (int c = 0; c < nc; c++) {
			int fb = 128 + 16 * c;
			// clip2depth[c]: columna U (out.x) = f[fb],f[fb+4],f[fb+8],f[fb+12];
			// columna V (out.y) = f[fb+1],f[fb+5],f[fb+9],f[fb+13].
			LogInfo("SOMBRAMED  cascada %d clip2depth U{%.5f %.5f %.5f %.5f} "
				"V{%.5f %.5f %.5f %.5f} | clip(near=%.2f far=%.2f) blendDist=%.2f\n",
				c, f[fb], f[fb + 4], f[fb + 8], f[fb + 12],
				f[fb + 1], f[fb + 5], f[fb + 9], f[fb + 13],
				f[192 + 4 * c], f[193 + 4 * c], f[216 + c]);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===== v1.71: MEDICION del pipeline del pasto — ¿por que los BLOQUES? =========
//
// Tras parchear el PS, el pasto se extiende a los lados pero por BLOQUES. Dos
// causas posibles, con fix distinto:
//  A) el instance buffer se DESBORDA (mas candidatos que capacidad) -> se cortan
//     por tope; se veria: candidatos >> capacidad.
//  B) el juego solo GENERA candidatos en ciertas zonas (seleccion CPU de tiles)
//     -> se veria: candidatos MUCHO menos que la capacidad, y no crecen al abrir.
// El scatter VS 3518fc8 se dibuja DIRECTO: su vertexCount = cantidad de
// candidatos generados ese frame. Comparado con la capacidad del instance
// buffer (g_gc_instances_res) distingue A de B. SOLO LECTURA.
static void triple_medir_pasto_scatter(UINT64 vs, UINT total_candidatos)
{
	if (vs != 0x3518fc8325dc58fbULL)
		return;
	static volatile LONG v = 0;
	LONG n = InterlockedIncrement(&v);
	if (n > 6)
		return;
	UINT cap = 0, capbytes = 0;
	if (g_gc_instances_res) {
		D3D11_BUFFER_DESC bd;
		if (triple_buffer_desc(g_gc_instances_res, &bd) && bd.StructureByteStride) {
			cap = bd.ByteWidth / bd.StructureByteStride;
			capbytes = bd.ByteWidth;
		}
	}
	LogInfo("PASTOMED scatter #%d: candidatos=%u | instance buffer=%u instancias "
		"(%u bytes) | %s\n", (int)n, total_candidatos, cap, capbytes,
		(cap && total_candidatos > cap) ? "CANDIDATOS > CAPACIDAD -> se desborda (caso A)"
		: (cap ? "cabe -> los bloques NO son desborde (caso B: generacion por zona)"
		       : "capacidad desconocida"));
}

// Rama experimental de culling: medir el buffer de zonas que alimenta al
// sembrador. Es lectura pura y se auto-limita; no cambia recursos ni estado D3D.
static thread_local bool tls_zone_dump = false;
static thread_local ID3D11Resource *tls_zone_res = NULL;
static thread_local void *tls_zone_ptr = NULL;
static thread_local UINT tls_zone_size = 0;

static void triple_medir_zonas(UINT64 vs, UINT elementos, UINT instancias)
{
	if (vs != 0x3518fc8325dc58fbULL)
		return;
	// Durante carga aparecen Draw(0, 1). No deben consumir el cupo: lo que
	// decide si la CPU ya recorto la lista es el primer draw con trabajo real.
	if (!elementos || !instancias)
		return;
	static volatile LONG veces = 0;
	LONG n = InterlockedIncrement(&veces);
	if (n > 8)
		return;
	ID3D11Resource *t20 = tls_vs_srv[20];
	D3D11_BUFFER_DESC bd = {};
	if (t20 && triple_buffer_desc(t20, &bd))
		LogInfo("ZONAMED scatter UTIL #%d: elementos=%u instancias=%u | "
			"t20=%p stride=%u bytes=%u slots=%u\n", (int)n, elementos,
			instancias, (void *)t20, bd.StructureByteStride, bd.ByteWidth,
			bd.StructureByteStride ? bd.ByteWidth / bd.StructureByteStride : 0);
	else
		LogInfo("ZONAMED scatter UTIL #%d: elementos=%u instancias=%u | "
			"t20=%p (sin descriptor de buffer)\n", (int)n, elementos,
			instancias, (void *)t20);
}

static void triple_medir_zonas_census(ID3D11Resource *res, const void *ptr, UINT bytes)
{
	if (!res || !ptr || bytes < 48 || bytes > 4000000 || bytes % 48)
		return;

	float cam[3];
	if (!TripleGetCameraPos(cam) ||
		cam[0]*cam[0] + cam[1]*cam[1] + cam[2]*cam[2] <= 1.0f)
		return;

	UINT slots = bytes / 48;
	if (slots > 76800)
		slots = 76800;

	bool hay_datos = false;
	__try {
		UINT muestra = slots < 64 ? slots : 64;
		for (UINT i = 0; i < muestra; i++) {
			const float *z = (const float *)((const BYTE *)ptr + (size_t)i * 48);
			if (_finite(z[0]) && _finite(z[1]) && _finite(z[2]) &&
				(z[0] != 0.0f || z[1] != 0.0f || z[2] != 0.0f)) {
				hay_datos = true;
				break;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (!hay_datos)
		return;

	static volatile LONG volcados = 0;
	LONG numero = InterlockedIncrement(&volcados);
	if (numero > 4)
		return;

	__try {
		UINT activos = 0, frente = 0, dentro_x = 0, fuera_x = 0, detras = 0;
		UINT tipos[8] = { 0 };
		float ndc_min = 1e30f, ndc_max = -1e30f;
		for (UINT i = 0; i < slots; i++) {
			const BYTE *fila = (const BYTE *)ptr + (size_t)i * 48;
			const float *z = (const float *)fila;
			const UINT *u = (const UINT *)fila;
			if (!_finite(z[0]) || !_finite(z[1]) || !_finite(z[2]) ||
				(z[0] == 0.0f && z[1] == 0.0f && z[2] == 0.0f))
				continue;
			activos++;
			if (u[3] < 8)
				tipos[u[3]]++;

			float clip[4];
			if (!TripleProjectMainCamera(z, clip))
				continue;
			if (clip[3] <= 0.0001f) {
				detras++;
				continue;
			}
			frente++;
			float nx = clip[0] / clip[3];
			if (_finite(nx)) {
				if (nx < ndc_min) ndc_min = nx;
				if (nx > ndc_max) ndc_max = nx;
			}
			if (fabsf(clip[0]) <= clip[3] + 1.0f)
				dentro_x++;
			else
				fuera_x++;
		}
		LogInfo("ZONAMED buffer #%d res=%p: %u slots, %u activos | "
			"camara frente=%u dentroX=%u fueraX=%u detras=%u ndcX=[%.3f,%.3f] | "
			"tipos0-7={%u %u %u %u %u %u %u %u}\n",
			(int)numero, (void *)res, slots, activos, frente, dentro_x, fuera_x,
			detras, ndc_min, ndc_max, tipos[0], tipos[1], tipos[2], tipos[3],
			tipos[4], tipos[5], tipos[6], tipos[7]);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		LogInfo("ZONAMED: lectura protegida fallo; la medicion se omite.\n");
	}
}

static bool triple_is_camera_cb_candidate(ID3D11Resource *res, UINT *out_size)
{
	D3D11_BUFFER_DESC d;
	if (!triple_buffer_desc(res, &d))
		return false;
	if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
		return false;
	if (d.ByteWidth < 256 || d.ByteWidth > 320)
		return false;
	*out_size = d.ByteWidth;
	return true;
}

STDMETHODIMP HackerContext::Map(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	/* [annotation] */
	__in  UINT Subresource,
	/* [annotation] */
	__in  D3D11_MAP MapType,
	/* [annotation] */
	__in  UINT MapFlags,
	/* [annotation] */
	__out D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
	HRESULT hr;

	hr = mOrigContext1->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);

	TrackAndDivertMap(hr, pResource, Subresource, MapType, MapFlags, pMappedResource);

	// v1.84: ESPIA de buffers dinamicos (diagnostico para la proxima
	// iteracion). Cada auto tiene exactamente TRES buffers dinamicos propios
	// que el juego actualiza con Map(WRITE_DISCARD) en cada frame, en TODAS
	// las vistas (incluso paragolpes, donde el auto no se dibuja), nunca se
	// bindean a ningun shader, y sus hashes sobreviven a setup, pintura,
	// graficos y reinicio: la futura identidad total del auto. Lo que falta
	// para reconocerlos ESTRUCTURALMENTE es su ByteWidth y sus flags — eso es
	// lo que esta linea revela, una sola vez por hash de recurso.
	//
	// Map es camino CALIENTE (regla dura 2): O(1) por llamada y sin alocar
	// por llamada (el set reserva de entrada y solo inserta hashes nuevos,
	// con tope duro: a las 512 entradas deja de loguear para siempre en la
	// sesion). Con el log apagado el costo es cero: ni se mira el set. El
	// hash sale del mapa recursos->hash que 3Dmigoto llena al CREAR el
	// recurso (GetResourceHash); no se calcula ningun hash nuevo.
	// Sin candado a proposito: el hook corre en los 7 contextos del juego y
	// una carrera puede duplicar algun log — tolerable; un mutex en el camino
	// de Map no lo es.
	if (LogFile && SUCCEEDED(hr) && pResource) {
		D3D11_BUFFER_DESC sd;
		if (triple_buffer_desc(pResource, &sd) && sd.Usage == D3D11_USAGE_DYNAMIC) {
			static std::unordered_set<uint32_t> vistos;
			static bool lleno = false;
			if (!lleno) {
				if (vistos.empty())
					vistos.reserve(512);
				uint32_t hash = GetResourceHash(pResource);
				if (hash && !vistos.count(hash)) {
					if (vistos.size() >= 512) {
						lleno = true;
					} else {
						vistos.insert(hash);
						LogInfo("TRIPLE mapbuf: hash=%08x bytewidth=%u bind=0x%x misc=0x%x stride=%u\n",
							hash, sd.ByteWidth, sd.BindFlags,
							sd.MiscFlags, sd.StructureByteStride);
					}
				}
			}
		}
	}

	if (SUCCEEDED(hr) && pMappedResource && pMappedResource->pData &&
	    (MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE)) {
		D3D11_BUFFER_DESC zone_desc;
		if (triple_buffer_desc(pResource, &zone_desc) &&
			zone_desc.StructureByteStride == 48 &&
			zone_desc.ByteWidth >= 4096 && zone_desc.ByteWidth <= 4000000 &&
			zone_desc.ByteWidth % 48 == 0) {
			tls_zone_dump = true;
			tls_zone_res = pResource;
			tls_zone_ptr = pMappedResource->pData;
			tls_zone_size = zone_desc.ByteWidth;
		}
		UINT sz = 0;
		if (triple_is_camera_cb_candidate(pResource, &sz)) {
			tls_triple_map_res = pResource;
			tls_triple_map_ptr = pMappedResource->pData;
			tls_triple_map_size = sz;
		}
		// v1.47: identificar el buffer de instancias del pasto por su FIRMA, no
		// por quien lo bindea. El VS del pasto lo declara como
		// `dcl_resource_structured t1, 112`: un structured buffer de stride 112
		// es rarisimo y no hace falta nada mas para reconocerlo.
		//
		// El intento anterior (recordar el t1 del VS y esperar al draw) nunca
		// llego a identificarlo: entre el binding y el draw hay decenas de
		// otros draws que pisan el slot, asi que el estado trackeado ya no era
		// el del pasto. La firma por stride no depende del orden de nada.
		// v1.48: si el handler pidio pasar a la fase 2, este es el lugar seguro
		// para re-armar (fuera del manejador de excepciones).
		bp_repostear_si_toca();

		// Censo de structured buffers mapeados: si el del pasto no aparece,
		// esto dice que SI se sube y con que forma. Sin esto, un silencio no
		// distingue "no se mapea" de "no lo estoy mirando bien".
		{
			D3D11_BUFFER_DESC cd;
			if (triple_buffer_desc(pResource, &cd) && cd.StructureByteStride) {
				static UINT vistos[48];
				static int nv = 0;
				static volatile LONG lk = 0;
				while (InterlockedCompareExchange(&lk, 1, 0) != 0)
					YieldProcessor();
				bool nuevo = true;
				for (int i = 0; i < nv; i++)
					if (vistos[i] == cd.StructureByteStride) { nuevo = false; break; }
				if (nuevo && nv < 48) {
					vistos[nv++] = cd.StructureByteStride;
					LogInfo("CULLBP censo: structured buffer mapeado stride=%u "
						"(%u bytes, %u elementos)\n", cd.StructureByteStride,
						cd.ByteWidth, cd.ByteWidth / cd.StructureByteStride);
				}
				InterlockedExchange(&lk, 0);
			}
		}

		// v1.54: la tabla de mallas del pasto, si es esta.
		if (pResource && pResource == g_gc_meshinfo_res) {
			D3D11_BUFFER_DESC md;
			if (triple_buffer_desc(pResource, &md)) {
				tls_gc_meshinfo_ptr = pMappedResource->pData;
				tls_gc_meshinfo_size = md.ByteWidth;
			}
		}

		// v1.51: volcar una muestra de CADA structured buffer distinto.
		//
		// La v1.50 apunto solo al stride 112 (como lo declara el VS del pasto)
		// y salio la TABLA DE TIPOS de vegetacion, no las instancias: 6 filas
		// con cantidades (15000, 4000, 2250...) y cajas de tamano donde ancho
		// == profundidad (7/3/7, 16/9/16). Util, pero no es lo que buscamos:
		// hay mas de un buffer con ese stride y agarramos el primero.
		//
		// Asi que ahora se muestrean todos y se busca cual tiene coordenadas de
		// mundo. Candidato fuerte del censo anterior: stride 12 (3 floats =
		// una posicion) con 1.020.980 elementos.
		if (pResource) {
			D3D11_BUFFER_DESC gd;
			if (triple_buffer_desc(pResource, &gd) && gd.StructureByteStride >= 8 &&
			    gd.ByteWidth >= gd.StructureByteStride) {
				static UINT vistos[64];
				static int nv = 0;
				static volatile LONG lk = 0;
				bool nuevo = false;
				// clave = stride + tamano: distingue dos buffers del mismo
				// stride, que es exactamente lo que nos confundio en la v1.50
				UINT clave = gd.StructureByteStride * 31 + gd.ByteWidth;
				while (InterlockedCompareExchange(&lk, 1, 0) != 0)
					YieldProcessor();
				nuevo = true;
				for (int i = 0; i < nv; i++)
					if (vistos[i] == clave) { nuevo = false; break; }
				if (nuevo && nv < 64)
					vistos[nv++] = clave;
				else
					nuevo = false;
				InterlockedExchange(&lk, 0);

				if (nuevo) {
					tls_triple_dump_gc = true;
					tls_triple_dump_gc_res = pResource;
					tls_triple_dump_gc_ptr = pMappedResource->pData;
					tls_triple_dump_gc_size = gd.ByteWidth;
					tls_triple_dump_gc_stride = gd.StructureByteStride;
				}
			}
		}

		// v1.39: apuntar cualquier buffer que por tamano pueda traer un juego
		// de 6 planos; el escaneo real (barato, con quick-reject) va en Unmap.
		D3D11_BUFFER_DESC bd;
		if (tls_sniff_n < TRIPLE_SNIFF_MAX && triple_buffer_desc(pResource, &bd) &&
		    bd.ByteWidth >= 96 && bd.ByteWidth <= 65536) {
			tls_sniff_res[tls_sniff_n] = pResource;
			tls_sniff_ptr[tls_sniff_n] = pMappedResource->pData;
			tls_sniff_size[tls_sniff_n] = bd.ByteWidth;
			tls_sniff_bind[tls_sniff_n] = bd.BindFlags;
			tls_sniff_n++;
		}
	}

	return hr;
}

STDMETHODIMP_(void) HackerContext::Unmap(THIS_
	/* [annotation] */
	__in ID3D11Resource *pResource,
	/* [annotation] */
	__in  UINT Subresource)
{
	if (tls_zone_dump && pResource == tls_zone_res) {
		ID3D11Resource *zone_res = tls_zone_res;
		void *zone_ptr = tls_zone_ptr;
		UINT zone_size = tls_zone_size;
		tls_zone_dump = false;
		tls_zone_res = NULL;
		tls_zone_ptr = NULL;
		tls_zone_size = 0;
		triple_medir_zonas_census(zone_res, zone_ptr, zone_size);
	}

	// v1.50: LEER el pasto en vez de perseguir a quien lo escribe.
	//
	// Las trampas de hardware se agotaron: la fase 1 cayo en un memcpy que
	// copiaba ~1400 bytes (no los 7168 del pasto), y la fase 2 sobre el origen
	// no disparo nunca. Con WRITE_DISCARD el driver recicla direcciones de un
	// pool, asi que una trampa fija atrapa a cualquier buffer que caiga ahi:
	// ese hit probablemente no era del pasto.
	//
	// Esto es directo: el buffer mapeado TIENE las instancias que el juego
	// decidio dibujar. Volcarlas y compararlas con la posicion del coche
	// responde la pregunta de fondo —¿que criterio las descarta?— sin depender
	// de atrapar a nadie. Una sola vez por sesion.
	// v1.54: LA TABLA DE MALLAS del pasto, como ENTEROS (verla como float no
	// dice nada: son indices y cantidades). Si sus campos cambian entre frames,
	// esta tabla ES el culling — y se reescribe facil.
	if (tls_gc_meshinfo_ptr && pResource == g_gc_meshinfo_res) {
		const unsigned *u = (const unsigned *)tls_gc_meshinfo_ptr;
		unsigned n = tls_gc_meshinfo_size / 16;
		tls_gc_meshinfo_ptr = NULL;
		static volatile LONG veces = 0;
		LONG v = InterlockedIncrement(&veces);
		if (v <= 6) {
			char linea[512];
			int p = _snprintf_s(linea, sizeof(linea), _TRUNCATE,
				"GCMESH #%d (%u mallas):", (int)v, n);
			for (unsigned i = 0; i < n && i < 8; i++)
				p += _snprintf_s(linea + p, sizeof(linea) - p, _TRUNCATE,
					" [%u: cnt=%u off=%u max=%u]", i,
					u[i * 4], u[i * 4 + 1], u[i * 4 + 2]);
			LogInfo("%s\n", linea);
		}
	}

	if (tls_triple_dump_gc && pResource == tls_triple_dump_gc_res) {
		tls_triple_dump_gc = false;
		const float *f = (const float *)tls_triple_dump_gc_ptr;
		unsigned stride = tls_triple_dump_gc_stride;
		unsigned nfloats = stride / 4;
		if (nfloats > 28)
			nfloats = 28;
		unsigned n = tls_triple_dump_gc_size / stride;
		if (n > 6)
			n = 6;
		float campos[3] = { 0, 0, 0 };
		int haycam = TripleGetCameraPos(campos);
		LogInfo("GCDUMP stride=%u (%u elementos). camara=%.2f %.2f %.2f%s\n",
			stride, tls_triple_dump_gc_size / stride,
			campos[0], campos[1], campos[2],
			haycam ? "" : " (sin camara aun)");
		char linea[600];
		for (unsigned i = 0; i < n; i++) {
			const float *v = f + (size_t)i * (stride / 4);
			int p = _snprintf_s(linea, sizeof(linea), _TRUNCATE,
				"GCDUMP  s%u[%02u]", stride, i);
			for (unsigned k = 0; k < nfloats; k++) {
				float x = v[k];
				if (x > -1e9f && x < 1e9f)
					p += _snprintf_s(linea + p, sizeof(linea) - p, _TRUNCATE,
						" %.2f", x);
				else
					p += _snprintf_s(linea + p, sizeof(linea) - p, _TRUNCATE,
						" 0x%08X", *(const unsigned *)&x);
			}
			LogInfo("%s\n", linea);
		}
	}

	// v1.39: escanear (y con el bit 4 de $cull_widen, parchear) los buffers
	// apuntados en Map, ANTES del divert y del Unmap real: el puntero sigue
	// vivo y la GPU todavia no vio los datos.
	if (tls_sniff_n > 0) {
		for (int i = 0; i < tls_sniff_n; i++) {
			if (tls_sniff_res[i] == pResource) {
				TripleScanPlanesInCB(tls_sniff_ptr[i], tls_sniff_size[i],
					(void *)pResource, tls_sniff_bind[i], 1);
				// v1.63 M1: snapshot del contenido si es constant buffer, para
				// leer el fovH de cb3/cb0 en el draw de la siembra.
				if (tls_sniff_bind[i] & D3D11_BIND_CONSTANT_BUFFER)
					cb_snap_store(pResource, tls_sniff_ptr[i], tls_sniff_size[i]);
				tls_sniff_n--;
				tls_sniff_res[i] = tls_sniff_res[tls_sniff_n];
				tls_sniff_ptr[i] = tls_sniff_ptr[tls_sniff_n];
				tls_sniff_size[i] = tls_sniff_size[tls_sniff_n];
				tls_sniff_bind[i] = tls_sniff_bind[tls_sniff_n];
				break;
			}
		}
	}

	TrackAndDivertUnmap(pResource, Subresource);

	// Triple-monitor: snoop the camera cb the game just wrote (see Map above).
	if (pResource == tls_triple_map_res && tls_triple_map_ptr) {
		// bit 64: el juego escribio el cbuffer con la matriz ANCHA; se le devuelve el m00
		// original antes de que la captura lo lea y antes de que la GPU lo vea.
		TripleRestaurarM00(tls_triple_map_ptr, tls_triple_map_size);
		TripleCaptureCameraCB(tls_triple_map_ptr, tls_triple_map_size);
		tls_triple_map_res = NULL;
		tls_triple_map_ptr = NULL;
	}

	mOrigContext1->Unmap(pResource, Subresource);
}

STDMETHODIMP_(void) HackerContext::PSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	// v1.63 M1: sombrear el buffer del PS slot 0 (cb0/$Globals). Se usa para
	// medir el fovH del test de frustum del PS de siembra del pasto.
	if (ppConstantBuffers && StartSlot == 0 && NumBuffers > 0)
		tls_ps_cb0 = ppConstantBuffers[0];
	// v1.70: slot 5 = cb5=ShadowParameters (cascadas de sombra).
	if (ppConstantBuffers && StartSlot <= 5 && StartSlot + NumBuffers > 5)
		tls_ps_cb5 = ppConstantBuffers[5 - StartSlot];

	 mOrigContext1->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::IASetInputLayout(THIS_
	/* [annotation] */
	__in_opt ID3D11InputLayout *pInputLayout)
{
	 mOrigContext1->IASetInputLayout(pInputLayout);
}

STDMETHODIMP_(void) HackerContext::IASetVertexBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers)  ID3D11Buffer *const *ppVertexBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers)  const UINT *pStrides,
	/* [annotation] */
	__in_ecount(NumBuffers)  const UINT *pOffsets)
{
	 mOrigContext1->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);

	 if (G->hunting == HUNTING_MODE_ENABLED) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
		for (UINT i = StartSlot; (i < StartSlot + NumBuffers) && (i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT); i++) {
			if (ppVertexBuffers && ppVertexBuffers[i]) {
				mCurrentVertexBuffers[i] = GetResourceHash(ppVertexBuffers[i]);
				G->mVisitedVertexBuffers.insert(mCurrentVertexBuffers[i]);
			} else
				mCurrentVertexBuffers[i] = 0;
		}
		LeaveCriticalSection(&G->mCriticalSection);
	 }
}

STDMETHODIMP_(void) HackerContext::GSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	 mOrigContext1->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::GSSetShader(THIS_
	/* [annotation] */
	__in_opt ID3D11GeometryShader *pShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11GeometryShader, &ID3D11DeviceContext::GSSetShader>
		(pShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedGeometryShaders,
		 G->mSelectedGeometryShader,
		 &mCurrentGeometryShader,
		 &mCurrentGeometryShaderHandle);
}

STDMETHODIMP_(void) HackerContext::IASetPrimitiveTopology(THIS_
	/* [annotation] */
	__in D3D11_PRIMITIVE_TOPOLOGY Topology)
{
	 mOrigContext1->IASetPrimitiveTopology(Topology);
}

STDMETHODIMP_(void) HackerContext::VSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	 mOrigContext1->VSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::PSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	mOrigContext1->PSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::Begin(THIS_
	/* [annotation] */
	__in  ID3D11Asynchronous *pAsync)
{
	mOrigContext1->Begin(pAsync);
}

STDMETHODIMP_(void) HackerContext::End(THIS_
	/* [annotation] */
	__in  ID3D11Asynchronous *pAsync)
{
	 mOrigContext1->End(pAsync);
}

STDMETHODIMP HackerContext::GetData(THIS_
	/* [annotation] */
	__in  ID3D11Asynchronous *pAsync,
	/* [annotation] */
	__out_bcount_opt(DataSize)  void *pData,
	/* [annotation] */
	__in  UINT DataSize,
	/* [annotation] */
	__in  UINT GetDataFlags)
{
	return mOrigContext1->GetData(pAsync, pData, DataSize, GetDataFlags);
}

STDMETHODIMP_(void) HackerContext::SetPredication(THIS_
	/* [annotation] */
	__in_opt ID3D11Predicate *pPredicate,
	/* [annotation] */
	__in  BOOL PredicateValue)
{
	return mOrigContext1->SetPredication(pPredicate, PredicateValue);
}

STDMETHODIMP_(void) HackerContext::GSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	SetShaderResources<&ID3D11DeviceContext::GSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::GSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	 mOrigContext1->GSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::OMSetBlendState(THIS_
	/* [annotation] */
	__in_opt  ID3D11BlendState *pBlendState,
	/* [annotation] */
	__in_opt  const FLOAT BlendFactor[4],
	/* [annotation] */
	__in  UINT SampleMask)
{
	 mOrigContext1->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
}

STDMETHODIMP_(void) HackerContext::OMSetDepthStencilState(THIS_
	/* [annotation] */
	__in_opt  ID3D11DepthStencilState *pDepthStencilState,
	/* [annotation] */
	__in  UINT StencilRef)
{
	 mOrigContext1->OMSetDepthStencilState(pDepthStencilState, StencilRef);
}

STDMETHODIMP_(void) HackerContext::SOSetTargets(THIS_
	/* [annotation] */
	__in_range(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount_opt(NumBuffers)  ID3D11Buffer *const *ppSOTargets,
	/* [annotation] */
	__in_ecount_opt(NumBuffers)  const UINT *pOffsets)
{
	 mOrigContext1->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
}

bool HackerContext::BeforeDispatch(DispatchContext *context)
{
	if (G->hunting == HUNTING_MODE_ENABLED) {
		if (G->DumpUsage)
			RecordComputeShaderStats();

		if (mCurrentComputeShader == G->mSelectedComputeShader) {
			if (G->marking_mode == MarkingMode::SKIP)
				return false;
		}
	}

	if (!G->fix_enabled)
		return true;

	DeferredShaderReplacementBeforeDispatch();

	// Override settings?
	if (!G->mShaderOverrideMap.empty()) {
		ShaderOverrideMap::iterator i;

		i = lookup_shaderoverride(mCurrentComputeShader);
		if (i != G->mShaderOverrideMap.end()) {
			context->post_commands = &i->second.post_command_list;
			// XXX: Not using ProcessShaderOverride() as a
			// lot of it's logic doesn't really apply to
			// compute shaders. The main thing we care
			// about is the command list, so just run that:
			RunCommandList(mHackerDevice, this, &i->second.command_list, &context->call_info, false);
			return !context->call_info.skip;
		}
	}

	return true;
}

void HackerContext::AfterDispatch(DispatchContext *context)
{
	if (context->post_commands)
		RunCommandList(mHackerDevice, this, context->post_commands, &context->call_info, true);
}

// v1.42: censo de compute shaders. Abrir el frustum trajo objetos pero NO el
// pasto, y ningun otro juego de planos en memoria esta vivo: queda un filtro
// aguas abajo del frustum. El exe nombra sus etapas —HZBOcclusionGenerateHZB,
// HZBOcclusionProcessQueries, "%5.1f%% of scene visible"— asi que el culling
// por oclusion existe y corre en compute. Este censo lista cada CS distinto con
// su hash y sus thread groups; el generador de la piramide HZB se delata solo
// (serie de dispatches que se van reduciendo a la mitad). Con el hash, apagarlo
// es una seccion [ShaderOverride] en el ini, sin tocar C++.
// v1.53: los 9 vertex shaders cuyo volcado menciona clutter/grass. El pasto no
// siempre usa el mismo (varia por tipo de vegetacion y por pista), asi que
// apuntar a uno solo —como hacia la v1.52— deja el volcado sin disparar.
static bool es_vs_clutter(UINT64 h)
{
	static const UINT64 lista[] = {
		0xe607507692aaa09eULL, 0x1b9c62ffa1f4c6bbULL, 0xf0ad90fed798641bULL,
		0xabfaf1657f01416cULL, 0x62ee9bd9071402e5ULL, 0xa36e45bb333b43e9ULL,
		0xb70e609a4e36aca0ULL, 0xf5ebad284a8bd772ULL, 0x51c4c0874c6cda5bULL,
	};
	for (int i = 0; i < 9; i++)
		if (lista[i] == h)
			return true;
	return false;
}

// Vuelca los recursos que el draw del pasto tiene en la mano. El structured
// buffer con stride 112 es g_gcInstanceBuffer (las instancias); el de stride 16
// es g_gcMeshNumIndicesBuffer, cuyos campos {elementCount, instanceOffset,
// maxInstances} definen QUE RANGO se dibuja — el filtro que buscamos.
static void triple_volcar_srv_clutter(UINT64 vs_hash)
{
	if (!es_vs_clutter(vs_hash))
		return;
	static volatile LONG hechos = 0;
	if (InterlockedIncrement(&hechos) > 3)
		return;
	LogInfo("GCSRV: === recursos del draw de CLUTTER vs=%016llX ===\n", vs_hash);
	for (int s = 0; s < 8; s++) {
		if (!tls_vs_srv[s]) {
			LogInfo("GCSRV  t%d = (vacio)\n", s);
			continue;
		}
		D3D11_BUFFER_DESC bd;
		if (triple_buffer_desc(tls_vs_srv[s], &bd)) {
			LogInfo("GCSRV  t%d = %p buffer stride=%u bytes=%u (%u elementos)\n",
				s, (void *)tls_vs_srv[s], bd.StructureByteStride, bd.ByteWidth,
				bd.StructureByteStride ? bd.ByteWidth / bd.StructureByteStride : 0);
			if (!g_gc_instances_res && bd.StructureByteStride == 112)
				g_gc_instances_res = tls_vs_srv[s];
			// v1.54: t0 (stride 16) es g_gcMeshNumIndicesBuffer. Sus campos
			// {elementCount, instanceOffset, maxInstances} deciden QUE TRAMO
			// del millon de instancias se dibuja. Si eso cambia por frame,
			// AHI esta el culling del pasto — y es un buffer de 128 bytes que
			// podemos reescribir en el Unmap.
			if (!g_gc_meshinfo_res && bd.StructureByteStride == 16 &&
			    bd.ByteWidth <= 4096)
				g_gc_meshinfo_res = tls_vs_srv[s];
		} else {
			LogInfo("GCSRV  t%d = %p (no es buffer: textura)\n",
				s, (void *)tls_vs_srv[s]);
		}
	}
}

// ===== v1.57: dibujar los TRAMOS de pasto que el juego se saltea ============
//
// Estado del caso: el buffer de instancias es estatico y tiene el pasto de toda
// la pista (118.536 matas). El juego emite un draw indirecto por cada tramo que
// quiere dibujar; cada tramo son 5 uints (20 bytes) dentro del buffer de
// argumentos que arma el CS 15bb7321c81952a4 (8 grupos x 32 hilos = hasta 256
// tramos).
//
// Probado y descartado: subir el CONTEO. Duplicarlo trajo matas de otras zonas
// —flotando, sobre la pista, hasta en el garage— pero **la linea de corte
// lateral quedo intacta**. O sea que las matas laterales no estan a
// continuacion del tramo que se dibuja: viven en OTRO tramo, uno que el juego
// decidio no emitir.
//
// Asi que en vez de pedir mas cantidad, se repite el draw apuntando a los otros
// tramos del mismo buffer de argumentos. Si el pasto lateral existe en alguno,
// aparece. Controlado por $gc_tramos (tecla F2): 0 = off.
static thread_local int tls_gc_repitiendo = 0;

static void triple_gc_repetir_tramos(HackerContext *ctx, ID3D11Buffer *args,
	UINT offset, UINT64 vs_hash, bool indexado)
{
	if (tls_gc_repitiendo || !es_vs_clutter(vs_hash) || !args)
		return;

	int n = (int)TripleReadIniGlobal(L"$gc_tramos").fval;
	if (n <= 0)
		return;
	if (n > 64)
		n = 64;

	// Reentra por el WRAPPER (no por mOrigContext1) a proposito: asi cada draw
	// extra pasa por la maquinaria del triple y se redibuja por region. Yendo
	// directo al contexto original apareceria solo con la proyeccion del juego,
	// que es justo el bug que estamos tratando de compensar.
	tls_gc_repitiendo = 1;
	for (int k = 1; k <= n; k++) {
		UINT off = offset + (UINT)k * 20;
		if (indexado)
			ctx->DrawIndexedInstancedIndirect(args, off);
		else
			ctx->DrawInstancedIndirect(args, off);
	}
	tls_gc_repitiendo = 0;
}

static void triple_censo_draw_indirecto(UINT64 vs_hash)
{
	static UINT64 vistos[32];
	static int n = 0;
	static volatile LONG lock = 0;
	while (InterlockedCompareExchange(&lock, 1, 0) != 0)
		YieldProcessor();
	for (int i = 0; i < n; i++) {
		if (vistos[i] == vs_hash) {
			InterlockedExchange(&lock, 0);
			return;
		}
	}
	if (n < 32) {
		vistos[n++] = vs_hash;
		LogInfo("CSCENSO: draw INDIRECTO con vs=%016llX (geometria generada en "
			"GPU: el conteo de instancias lo decide un compute shader)\n",
			vs_hash);
	}
	InterlockedExchange(&lock, 0);
}

static void triple_censo_cs(UINT64 hash, UINT x, UINT y, UINT z)
{
	if (!hash)
		return;
	static UINT64 vistos[64];
	static UINT conteo[64];
	static int n = 0;
	static volatile LONG lock = 0;

	while (InterlockedCompareExchange(&lock, 1, 0) != 0)
		YieldProcessor();
	for (int i = 0; i < n; i++) {
		if (vistos[i] == hash) {
			// relogear cada 600 dispatches para ver como varian los grupos
			if (++conteo[i] % 600 == 0)
				LogInfo("CSCENSO: %016llX otra vez, grupos=%ux%ux%u (n=%u)\n",
					hash, x, y, z, conteo[i]);
			InterlockedExchange(&lock, 0);
			return;
		}
	}
	if (n < 64) {
		vistos[n] = hash;
		conteo[n] = 1;
		n++;
		LogInfo("CSCENSO: compute shader NUEVO %016llX grupos=%ux%ux%u\n",
			hash, x, y, z);
	}
	InterlockedExchange(&lock, 0);
}

STDMETHODIMP_(void) HackerContext::Dispatch(THIS_
	/* [annotation] */
	__in  UINT ThreadGroupCountX,
	/* [annotation] */
	__in  UINT ThreadGroupCountY,
	/* [annotation] */
	__in  UINT ThreadGroupCountZ)
{
	DispatchContext context{ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ};


	// v1.44: 15bb7321c81952a4 es el CS que arma el draw indirecto del pasto.
	// Su t1 es g_gcMeshCountsBuffer: el conteo que dejo el culler. Guardamos su
	// identidad para reconocerlo cuando la CPU lo mapee.
	if (mCurrentComputeShader == 0x15bb7321c81952a4ULL && tls_cs_srv[1] &&
	    !g_gc_counts_res) {
		g_gc_counts_res = tls_cs_srv[1];
		LogInfo("CULLBP: identificado g_gcMeshCountsBuffer = %p (t1 del CS del "
			"pasto).\n", (void *)g_gc_counts_res);
	}

	// v1.45: si el contador esta bindeado como UAV, ESTE dispatch lo escribe:
	// es el culler del pasto que veniamos buscando.
	if (tls_gc_counts_uav_slot >= 0)
		gc_reportar("un COMPUTE SHADER (UAV)", mCurrentComputeShader);

	if (BeforeDispatch(&context))
		mOrigContext1->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	else
		Profiling::skipped_draw_calls++;

	AfterDispatch(&context);
}

STDMETHODIMP_(void) HackerContext::DispatchIndirect(THIS_
	/* [annotation] */
	__in  ID3D11Buffer *pBufferForArgs,
	/* [annotation] */
	__in  UINT AlignedByteOffsetForArgs)
{
	DispatchContext context{&pBufferForArgs, AlignedByteOffsetForArgs};

	// indirecto: los grupos los decide la GPU, se marcan con 0 para distinguirlo

	if (BeforeDispatch(&context))
		mOrigContext1->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
	else
		Profiling::skipped_draw_calls++;

	AfterDispatch(&context);
}

STDMETHODIMP_(void) HackerContext::RSSetState(THIS_
	/* [annotation] */
	__in_opt  ID3D11RasterizerState *pRasterizerState)
{
	 mOrigContext1->RSSetState(pRasterizerState);
}

STDMETHODIMP_(void) HackerContext::RSSetViewports(THIS_
	/* [annotation] */
	__in_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumViewports,
	/* [annotation] */
	__in_ecount_opt(NumViewports)  const D3D11_VIEWPORT *pViewports)
{
	 mOrigContext1->RSSetViewports(NumViewports, pViewports);
}

STDMETHODIMP_(void) HackerContext::RSSetScissorRects(THIS_
	/* [annotation] */
	__in_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumRects,
	/* [annotation] */
	__in_ecount_opt(NumRects)  const D3D11_RECT *pRects)
{
	// TRIPLE v1.26: SHADOW-TRACKING de los scissor rects del JUEGO.
	// Los Get* son ilegales/no fiables en contextos diferidos (leccion v1.5b),
	// asi que para poder RESTAURAR el rect del juego tras nuestros 3 redibujos
	// lo registramos aca, al pasar. Sin esto, los paneles de la UI que confian
	// en un rect que setearon antes (listas de menus, recortes de paneles) se
	// recortaban con nuestro rect neutro de 16384 -> menus rotos.
	if (pRects && NumRects <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
		memcpy(mTripleScissorShadow, pRects, NumRects * sizeof(D3D11_RECT));
		mTripleScissorShadowCount = NumRects;
	} else {
		mTripleScissorShadowCount = 0;
	}

    // Diagnostic018: identify the scissors used to populate the sun shadow
    // array. cull_site=100 tests full-map scissors; 0 preserves the game.
    // Keep the original rectangle above for the triple/UI restoration path.
    // Restrict this to square R32 depth arrays (3/4 cascades), never screen/UI.
    if (NumRects == 1 && pRects) {
        ID3D11DepthStencilView *depthView = nullptr;
        mOrigContext1->OMGetRenderTargets(0, nullptr, &depthView);
        if (depthView) {
            ID3D11Resource *resource = nullptr;
            depthView->GetResource(&resource);
            ID3D11Texture2D *texture = nullptr;
            if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture))) {
                D3D11_TEXTURE2D_DESC desc = {};
                texture->GetDesc(&desc);
                if (desc.Width == desc.Height && desc.Width >= 1024 && desc.Width <= 4096 &&
                    desc.ArraySize >= 3 && desc.ArraySize <= 4 && desc.SampleDesc.Count == 1 &&
                    (desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT)) {
                    bool open = TripleReadIniGlobal(L"$cull_site").fval == 100.0f;
                    static LONG reports = 0;
                    if (InterlockedIncrement(&reports) <= 24) {
                        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
                        depthView->GetDesc(&viewDesc);
                        LogInfo("SHADOWSCISSOR018 size=%u array=%u slice=%u rect=%ld,%ld,%ld,%ld open=%d\n",
                            desc.Width, desc.ArraySize,
                            viewDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? viewDesc.Texture2DArray.FirstArraySlice : 0,
                            pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom, open);
                    }
                    if (open) {
                        D3D11_RECT full = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
                        mOrigContext1->RSSetScissorRects(1, &full);
                        texture->Release(); resource->Release(); depthView->Release();
                        return;
                    }
                }
                texture->Release();
            }
            if (resource) resource->Release();
            depthView->Release();
        }
    }
	 mOrigContext1->RSSetScissorRects(NumRects, pRects);
}

/*
 * Used for CryEngine games like Lichdom that copy a 2D rectangle from the
 * colour render target to a texture as an input for transparent refraction
 * effects. Expands the rectange to the full width.
 */
bool HackerContext::ExpandRegionCopy(ID3D11Resource *pDstResource, UINT DstX,
		UINT DstY, ID3D11Resource *pSrcResource, const D3D11_BOX *pSrcBox,
		UINT *replaceDstX, D3D11_BOX *replaceBox)
{
	ID3D11Texture2D *srcTex = (ID3D11Texture2D*)pSrcResource;
	ID3D11Texture2D *dstTex = (ID3D11Texture2D*)pDstResource;
	D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
	D3D11_RESOURCE_DIMENSION srcDim, dstDim;
	uint32_t srcHash, dstHash;
	TextureOverrideMap::iterator i;

	if (!pSrcResource || !pDstResource || !pSrcBox)
		return false;

	pSrcResource->GetType(&srcDim);
	pDstResource->GetType(&dstDim);
	if (srcDim != dstDim || srcDim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		return false;

	srcTex->GetDesc(&srcDesc);
	dstTex->GetDesc(&dstDesc);
	EnterCriticalSectionPretty(&G->mCriticalSection);
		srcHash = GetResourceHash(srcTex);
		dstHash = GetResourceHash(dstTex);
	LeaveCriticalSection(&G->mCriticalSection);

	LogDebug("CopySubresourceRegion %08lx (%u:%u x %u:%u / %u x %u) -> %08lx (%u x %u / %u x %u)\n",
			srcHash, pSrcBox->left, pSrcBox->right, pSrcBox->top, pSrcBox->bottom, srcDesc.Width, srcDesc.Height, 
			dstHash, DstX, DstY, dstDesc.Width, dstDesc.Height);

	i = lookup_textureoverride(dstHash);
	if (i == G->mTextureOverrideMap.end())
		return false;

	if (!i->second.begin()->expand_region_copy)
		return false;

	memcpy(replaceBox, pSrcBox, sizeof(D3D11_BOX));
	*replaceDstX = 0;
	replaceBox->left = 0;
	replaceBox->right = dstDesc.Width;

	return true;
}

STDMETHODIMP_(void) HackerContext::CopySubresourceRegion(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pDstResource,
	/* [annotation] */
	__in  UINT DstSubresource,
	/* [annotation] */
	__in  UINT DstX,
	/* [annotation] */
	__in  UINT DstY,
	/* [annotation] */
	__in  UINT DstZ,
	/* [annotation] */
	__in  ID3D11Resource *pSrcResource,
	/* [annotation] */
	__in  UINT SrcSubresource,
	/* [annotation] */
	__in_opt  const D3D11_BOX *pSrcBox)
{
	D3D11_BOX replaceSrcBox;
	UINT replaceDstX = DstX;

	if (pDstResource && (pDstResource == g_gc_counts_res || pDstResource == g_gc_instances_res))
		gc_reportar("CopySubresourceRegion", 0);

	if (G->hunting && G->track_texture_updates != 2) { // Any hunting mode - want to catch hash contamination even while soft disabled
		MarkResourceHashContaminated(pDstResource, DstSubresource, pSrcResource, SrcSubresource, 'S', DstX, DstY, DstZ, pSrcBox);
	}

	if (ExpandRegionCopy(pDstResource, DstX, DstY, pSrcResource, pSrcBox, &replaceDstX, &replaceSrcBox))
		pSrcBox = &replaceSrcBox;

	 mOrigContext1->CopySubresourceRegion(pDstResource, DstSubresource, replaceDstX, DstY, DstZ,
		pSrcResource, SrcSubresource, pSrcBox);

	// We only update the destination resource hash when the entire
	// subresource 0 is updated and pSrcBox is NULL. We could check if the
	// pSrcBox fills the entire resource, but if the game is using pSrcBox
	// it stands to reason that it won't always fill the entire resource
	// and the hashes might be less predictable. Possibly something to
	// enable as an option in the future if there is a proven need.
	if (G->track_texture_updates == 1 && DstSubresource == 0 && DstX == 0 && DstY == 0 && DstZ == 0 && pSrcBox == NULL)
		PropagateResourceHash(pDstResource, pSrcResource);
}

STDMETHODIMP_(void) HackerContext::CopyResource(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pDstResource,
	/* [annotation] */
	__in  ID3D11Resource *pSrcResource)
{
	if (pDstResource && (pDstResource == g_gc_counts_res || pDstResource == g_gc_instances_res))
		gc_reportar("CopyResource desde otro buffer", 0);

	if (G->hunting && G->track_texture_updates != 2) { // Any hunting mode - want to catch hash contamination even while soft disabled
		MarkResourceHashContaminated(pDstResource, 0, pSrcResource, 0, 'C', 0, 0, 0, NULL);
	}

	 mOrigContext1->CopyResource(pDstResource, pSrcResource);

	if (G->track_texture_updates == 1)
		PropagateResourceHash(pDstResource, pSrcResource);
}

STDMETHODIMP_(void) HackerContext::UpdateSubresource(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pDstResource,
	/* [annotation] */
	__in  UINT DstSubresource,
	/* [annotation] */
	__in_opt  const D3D11_BOX *pDstBox,
	/* [annotation] */
	__in  const void *pSrcData,
	/* [annotation] */
	__in  UINT SrcRowPitch,
	/* [annotation] */
	__in  UINT SrcDepthPitch)
{
	if (G->hunting && G->track_texture_updates != 2) { // Any hunting mode - want to catch hash contamination even while soft disabled
		MarkResourceHashContaminated(pDstResource, DstSubresource, NULL, 0, 'U', 0, 0, 0, NULL);
	}

	// v1.39: los planos de culling tambien pueden subir por aca. Deteccion
	// barata sobre los datos del juego; solo si aparecen planos Y el parcheo
	// esta pedido (bit 4 de $cull_widen) se copia y se reenvia lo nuestro.
	// v1.44/46: los buffers del pasto tambien pueden subir por aca. En ese caso
	// pSrcData es memoria PROPIA del juego —donde el culler escribio— y es
	// mejor objetivo que un buffer mapeado: persiste entre frames.
	if (pDstResource && g_bp_estado == 0 && pSrcData) {
		D3D11_BUFFER_DESC gd;
		if (triple_buffer_desc(pDstResource, &gd) &&
		    (gd.StructureByteStride == 112 || pDstResource == g_gc_counts_res)) {
			if (gd.StructureByteStride == 112 && !g_gc_instances_res)
				g_gc_instances_res = pDstResource;
			LogInfo("CULLBP: UpdateSubresource sobre el buffer del pasto "
				"(stride=%u, %u bytes) -> trampa en la memoria FUENTE %p "
				"(persiste entre frames)\n", gd.StructureByteStride,
				gd.ByteWidth, (void *)pSrcData);
			bp_intentar((void *)pSrcData);
		}
	}

	const void *triple_src = pSrcData;
	void *triple_tmp = NULL;
	if (pSrcData && !pDstBox) {
		D3D11_BUFFER_DESC bd;
		if (triple_buffer_desc(pDstResource, &bd) &&
		    bd.ByteWidth >= 96 && bd.ByteWidth <= 65536) {
			// v1.63 M1: snapshot de constant buffers actualizados por esta via.
			if (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
				cb_snap_store(pDstResource, pSrcData, bd.ByteWidth);
			if (TripleScanPlanesInCB((void *)pSrcData, bd.ByteWidth,
					(void *)pDstResource, bd.BindFlags, 0) == 1) {
				triple_tmp = malloc(bd.ByteWidth);
				if (triple_tmp) {
					memcpy(triple_tmp, pSrcData, bd.ByteWidth);
					if (TripleScanPlanesInCB(triple_tmp, bd.ByteWidth,
							(void *)pDstResource, bd.BindFlags, 1) == 2)
						triple_src = triple_tmp;
				}
			}
		}
	}

	 mOrigContext1->UpdateSubresource(pDstResource, DstSubresource, pDstBox, triple_src, SrcRowPitch,
		SrcDepthPitch);

	if (triple_tmp)
		free(triple_tmp);

	// We only update the destination resource hash when the entire
	// subresource 0 is updated and pDstBox is NULL. We could check if the
	// pDstBox fills the entire resource, but if the game is using pDstBox
	// it stands to reason that it won't always fill the entire resource
	// and the hashes might be less predictable. Possibly something to
	// enable as an option in the future if there is a proven need.
	if (G->track_texture_updates == 1 && DstSubresource == 0 && pDstBox == NULL)
		UpdateResourceHashFromCPU(pDstResource, pSrcData, SrcRowPitch, SrcDepthPitch);
}

STDMETHODIMP_(void) HackerContext::CopyStructureCount(THIS_
	/* [annotation] */
	__in  ID3D11Buffer *pDstBuffer,
	/* [annotation] */
	__in  UINT DstAlignedByteOffset,
	/* [annotation] */
	__in  ID3D11UnorderedAccessView *pSrcView)
{
	// v1.45: CopyStructureCount es LA via clasica para publicar "cuantos
	// elementos agrego un append buffer". Si el destino es el contador del
	// pasto, el culling es un append en GPU y el productor es el CS que tenia
	// ese UAV bindeado.
	if (pDstBuffer && ((ID3D11Resource *)pDstBuffer == g_gc_counts_res ||
	                   (ID3D11Resource *)pDstBuffer == g_gc_instances_res))
		gc_reportar("CopyStructureCount (append buffer en GPU)", 0);

	 mOrigContext1->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
}

STDMETHODIMP_(void) HackerContext::ClearUnorderedAccessViewUint(THIS_
	/* [annotation] */
	__in  ID3D11UnorderedAccessView *pUnorderedAccessView,
	/* [annotation] */
	__in  const UINT Values[4])
{
	RunViewCommandList(mHackerDevice, this, &G->clear_uav_uint_command_list, pUnorderedAccessView, false);
	mOrigContext1->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
	RunViewCommandList(mHackerDevice, this, &G->post_clear_uav_uint_command_list, pUnorderedAccessView, true);
}

STDMETHODIMP_(void) HackerContext::ClearUnorderedAccessViewFloat(THIS_
	/* [annotation] */
	__in  ID3D11UnorderedAccessView *pUnorderedAccessView,
	/* [annotation] */
	__in  const FLOAT Values[4])
{
	RunViewCommandList(mHackerDevice, this, &G->clear_uav_float_command_list, pUnorderedAccessView, false);
	mOrigContext1->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
	RunViewCommandList(mHackerDevice, this, &G->post_clear_uav_float_command_list, pUnorderedAccessView, true);
}

STDMETHODIMP_(void) HackerContext::ClearDepthStencilView(THIS_
	/* [annotation] */
	__in  ID3D11DepthStencilView *pDepthStencilView,
	/* [annotation] */
	__in  UINT ClearFlags,
	/* [annotation] */
	__in  FLOAT Depth,
	/* [annotation] */
	__in  UINT8 Stencil)
{
	RunViewCommandList(mHackerDevice, this, &G->clear_dsv_command_list, pDepthStencilView, false);
	mOrigContext1->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
	RunViewCommandList(mHackerDevice, this, &G->post_clear_dsv_command_list, pDepthStencilView, true);
}

STDMETHODIMP_(void) HackerContext::GenerateMips(THIS_
	/* [annotation] */
	__in  ID3D11ShaderResourceView *pShaderResourceView)
{
	 mOrigContext1->GenerateMips(pShaderResourceView);
}

STDMETHODIMP_(void) HackerContext::SetResourceMinLOD(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource,
	FLOAT MinLOD)
{
	 mOrigContext1->SetResourceMinLOD(pResource, MinLOD);
}

STDMETHODIMP_(FLOAT) HackerContext::GetResourceMinLOD(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pResource)
{
	FLOAT ret = mOrigContext1->GetResourceMinLOD(pResource);

	return ret;
}

STDMETHODIMP_(void) HackerContext::ResolveSubresource(THIS_
	/* [annotation] */
	__in  ID3D11Resource *pDstResource,
	/* [annotation] */
	__in  UINT DstSubresource,
	/* [annotation] */
	__in  ID3D11Resource *pSrcResource,
	/* [annotation] */
	__in  UINT SrcSubresource,
	/* [annotation] */
	__in  DXGI_FORMAT Format)
{
	 mOrigContext1->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource,
		Format);
}

STDMETHODIMP_(void) HackerContext::ExecuteCommandList(THIS_
	/* [annotation] */
	__in  ID3D11CommandList *pCommandList,
	BOOL RestoreContextState)
{
	if (G->deferred_contexts_enabled)
		mOrigContext1->ExecuteCommandList(pCommandList, RestoreContextState);

	if (!RestoreContextState) {
		// This is equivalent to calling ClearState() afterwards, so we
		// need to rebind the 3DMigoto resources now. See also
		// FinishCommandList's RestoreDeferredContextState:
		Bind3DMigotoResources();
		// TRIPLE: y el estado por defecto tampoco tiene scissor rects.
		mTripleScissorShadowCount = 0;
	}
}

STDMETHODIMP_(void) HackerContext::HSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	SetShaderResources<&ID3D11DeviceContext::HSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::HSSetShader(THIS_
	/* [annotation] */
	__in_opt  ID3D11HullShader *pHullShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11HullShader, &ID3D11DeviceContext::HSSetShader>
		(pHullShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedHullShaders,
		 G->mSelectedHullShader,
		 &mCurrentHullShader,
		 &mCurrentHullShaderHandle);
}

STDMETHODIMP_(void) HackerContext::HSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	 mOrigContext1->HSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::HSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	 mOrigContext1->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::DSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	SetShaderResources<&ID3D11DeviceContext::DSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::DSSetShader(THIS_
	/* [annotation] */
	__in_opt  ID3D11DomainShader *pDomainShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11DomainShader, &ID3D11DeviceContext::DSSetShader>
		(pDomainShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedDomainShaders,
		 G->mSelectedDomainShader,
		 &mCurrentDomainShader,
		 &mCurrentDomainShaderHandle);
}

STDMETHODIMP_(void) HackerContext::DSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	 mOrigContext1->DSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::DSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	 mOrigContext1->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::CSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	// v1.44: recordar que recursos van a los slots t0/t1 del compute. En el
	// Dispatch del shader del pasto esos dos son g_gcMeshNumIndicesBuffer y
	// g_gcMeshCountsBuffer — y el segundo ES el resultado del culling. Se
	// trackea aca en vez de leerlo con CSGetShaderResources porque los Get* no
	// son fiables en contextos diferidos (regla 3 de CLAUDE.md).
	for (UINT i = 0; i < NumViews && i < 2; i++) {
		UINT slot = StartSlot + i;
		if (slot > 1 || !ppShaderResourceViews)
			continue;
		ID3D11Resource *res = NULL;
		if (ppShaderResourceViews[i])
			ppShaderResourceViews[i]->GetResource(&res);
		tls_cs_srv[slot] = res;
		if (res)
			res->Release();	// solo guardamos el puntero como identidad
	}

	SetShaderResources<&ID3D11DeviceContext::CSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::CSSetUnorderedAccessViews(THIS_
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
	/* [annotation] */
	__in_ecount(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
	/* [annotation] */
	__in_ecount(NumUAVs)  const UINT *pUAVInitialCounts)
{
	// v1.45: ¿alguien esta por ESCRIBIR el contador del pasto como UAV? Se
	// marca por hilo y el Dispatch dice que compute shader fue. Es la via que
	// queda: la CPU nunca lo mapea (el breakpoint de la v1.44 nunca disparo),
	// asi que el conteo lo produce la GPU.
	tls_gc_counts_uav_slot = -1;
	if (ppUnorderedAccessViews && (g_gc_counts_res || g_gc_instances_res)) {
		for (UINT i = 0; i < NumUAVs; ++i) {
			if (!ppUnorderedAccessViews[i])
				continue;
			ID3D11Resource *r = NULL;
			ppUnorderedAccessViews[i]->GetResource(&r);
			if (r) {
				if (r == g_gc_counts_res || r == g_gc_instances_res)
					tls_gc_counts_uav_slot = (int)(StartSlot + i);
				r->Release();
			}
		}
	}

	if (ppUnorderedAccessViews) {
		// TODO: Record stats on unordered access view usage
		for (UINT i = 0; i < NumUAVs; ++i) {
			if (!ppUnorderedAccessViews[i])
				continue;
			// TODO: Record stats
		}
	}

	mOrigContext1->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}


// C++ function template of common code shared by all XXSetShader functions:
template <class ID3D11Shader,
	 void (__stdcall ID3D11DeviceContext::*OrigSetShader)(THIS_
			 ID3D11Shader *pShader,
			 ID3D11ClassInstance *const *ppClassInstances,
			 UINT NumClassInstances)
	 >
STDMETHODIMP_(void) HackerContext::SetShader(THIS_
	/* [annotation] */
	__in_opt ID3D11Shader *pShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances,
	std::set<UINT64> *visitedShaders,
	UINT64 selectedShader,
	UINT64 *currentShaderHash,
	ID3D11Shader **currentShaderHandle)
{
	ID3D11Shader *repl_shader = pShader;

	// Always update the current shader handle no matter what so we can
	// reliably check if a shader of a given type is bound and for certain
	// types of old style filtering:
	*currentShaderHandle = pShader;

	if (pShader) {
		// Store as current shader. Need to do this even while
		// not hunting for ShaderOverride section in BeforeDraw
		// We also set the current shader hash, but as an optimization,
		// we skip the lookup if there are no ShaderOverride The
		// lookup/find takes measurable amounts of CPU time.
		//
		// grumble grumble this optimisation caught me out *TWICE* grumble grumble -DSS
		if (!G->mShaderOverrideMap.empty() || !shader_regex_groups.empty() || (G->hunting == HUNTING_MODE_ENABLED)) {
			ShaderMap::iterator i = lookup_shader_hash(pShader);
			if (i != G->mShaders.end()) {
				*currentShaderHash = i->second;
				LogDebug("  shader found: handle = %p, hash = %016I64x\n", *currentShaderHandle, *currentShaderHash);

				if ((G->hunting == HUNTING_MODE_ENABLED) && visitedShaders) {
					EnterCriticalSectionPretty(&G->mCriticalSection);
					visitedShaders->insert(i->second);
					LeaveCriticalSection(&G->mCriticalSection);
				}
			}
			else
				LogDebug("  shader %p not found\n", pShader);
		} else {
			// Not accurate, but if we have a bug where we
			// reference this at least make sure we don't use the
			// *wrong* hash
			*currentShaderHash = 0;
		}

		// If the shader has been live reloaded from ShaderFixes, use the new one
		// No longer conditional on G->hunting now that hunting may be soft enabled via key binding
		ShaderReloadMap::iterator it = lookup_reloaded_shader(pShader);
		if (it != G->mReloadedShaders.end() && it->second.replacement != NULL) {
			LogDebug("  shader replaced by: %p\n", it->second.replacement);

			// It might make sense to Release() the original shader, to recover memory on GPU
			//   -Bo3b
			// No - we're already not incrementing the refcount since we don't bind it, and if we
			// released the original it would mean the game has an invalid pointer and can crash.
			// I wouldn't worry too much about GPU memory usage beyond leaks - the driver has a
			// full virtual memory system and can swap rarely used resources out to system memory.
			// If we did want to do better here we could return a wrapper object when the game
			// creates the original shader, and manage original/replaced/reverted/etc from there.
			//   -DSS
			repl_shader = (ID3D11Shader*)it->second.replacement;
		}

		if (G->hunting == HUNTING_MODE_ENABLED) {
			// Replacement map.
			if (G->marking_mode == MarkingMode::ORIGINAL || !G->fix_enabled) {
				ShaderReplacementMap::iterator j = lookup_original_shader(pShader);
				if ((selectedShader == *currentShaderHash || !G->fix_enabled) && j != G->mOriginalShaders.end()) {
					repl_shader = (ID3D11Shader*)j->second;
				}
			}
		}

	} else {
		*currentShaderHash = 0;
	}

	// Call through to original XXSetShader, but pShader may have been replaced.
	(mOrigContext1->*OrigSetShader)(repl_shader, ppClassInstances, NumClassInstances);
}

STDMETHODIMP_(void) HackerContext::CSSetShader(THIS_
	/* [annotation] */
	__in_opt  ID3D11ComputeShader *pComputeShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11ComputeShader, &ID3D11DeviceContext::CSSetShader>
		(pComputeShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedComputeShaders,
		 G->mSelectedComputeShader,
		 &mCurrentComputeShader,
		 &mCurrentComputeShaderHandle);
}

STDMETHODIMP_(void) HackerContext::CSSetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	 mOrigContext1->CSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::CSSetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	 mOrigContext1->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::VSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::PSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::PSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11PixelShader **ppPixelShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances);
}

STDMETHODIMP_(void) HackerContext::PSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->PSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::VSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11VertexShader **ppVertexShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances);

	// Todo: At GetShader, we need to return the original shader if it's been reloaded.
}

STDMETHODIMP_(void) HackerContext::PSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::IAGetInputLayout(THIS_
	/* [annotation] */
	__out  ID3D11InputLayout **ppInputLayout)
{
	 mOrigContext1->IAGetInputLayout(ppInputLayout);
}

STDMETHODIMP_(void) HackerContext::IAGetVertexBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount_opt(NumBuffers)  ID3D11Buffer **ppVertexBuffers,
	/* [annotation] */
	__out_ecount_opt(NumBuffers)  UINT *pStrides,
	/* [annotation] */
	__out_ecount_opt(NumBuffers)  UINT *pOffsets)
{
	 mOrigContext1->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

STDMETHODIMP_(void) HackerContext::IAGetIndexBuffer(THIS_
	/* [annotation] */
	__out_opt  ID3D11Buffer **pIndexBuffer,
	/* [annotation] */
	__out_opt  DXGI_FORMAT *Format,
	/* [annotation] */
	__out_opt  UINT *Offset)
{
	 mOrigContext1->IAGetIndexBuffer(pIndexBuffer, Format, Offset);
}

STDMETHODIMP_(void) HackerContext::GSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::GSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11GeometryShader **ppGeometryShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances);
}

STDMETHODIMP_(void) HackerContext::IAGetPrimitiveTopology(THIS_
	/* [annotation] */
	__out  D3D11_PRIMITIVE_TOPOLOGY *pTopology)
{
	 mOrigContext1->IAGetPrimitiveTopology(pTopology);
}

STDMETHODIMP_(void) HackerContext::VSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::VSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->VSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::GetPredication(THIS_
	/* [annotation] */
	__out_opt  ID3D11Predicate **ppPredicate,
	/* [annotation] */
	__out_opt  BOOL *pPredicateValue)
{
	 mOrigContext1->GetPredication(ppPredicate, pPredicateValue);
}

STDMETHODIMP_(void) HackerContext::GSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::GSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->GSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::OMGetRenderTargets(THIS_
	/* [annotation] */
	__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
	/* [annotation] */
	__out_ecount_opt(NumViews)  ID3D11RenderTargetView **ppRenderTargetViews,
	/* [annotation] */
	__out_opt  ID3D11DepthStencilView **ppDepthStencilView)
{
	 mOrigContext1->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView);
}

STDMETHODIMP_(void) HackerContext::OMGetRenderTargetsAndUnorderedAccessViews(THIS_
	/* [annotation] */
	__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumRTVs,
	/* [annotation] */
	__out_ecount_opt(NumRTVs)  ID3D11RenderTargetView **ppRenderTargetViews,
	/* [annotation] */
	__out_opt  ID3D11DepthStencilView **ppDepthStencilView,
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - UAVStartSlot)  UINT NumUAVs,
	/* [annotation] */
	__out_ecount_opt(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	 mOrigContext1->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView,
		UAVStartSlot, NumUAVs, ppUnorderedAccessViews);
}

STDMETHODIMP_(void) HackerContext::OMGetBlendState(THIS_
	/* [annotation] */
	__out_opt  ID3D11BlendState **ppBlendState,
	/* [annotation] */
	__out_opt  FLOAT BlendFactor[4],
	/* [annotation] */
	__out_opt  UINT *pSampleMask)
{
	 mOrigContext1->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask);
}

STDMETHODIMP_(void) HackerContext::OMGetDepthStencilState(THIS_
	/* [annotation] */
	__out_opt  ID3D11DepthStencilState **ppDepthStencilState,
	/* [annotation] */
	__out_opt  UINT *pStencilRef)
{
	 mOrigContext1->OMGetDepthStencilState(ppDepthStencilState, pStencilRef);
}

STDMETHODIMP_(void) HackerContext::SOGetTargets(THIS_
	/* [annotation] */
	__in_range(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppSOTargets)
{
	 mOrigContext1->SOGetTargets(NumBuffers, ppSOTargets);
}

STDMETHODIMP_(void) HackerContext::RSGetState(THIS_
	/* [annotation] */
	__out  ID3D11RasterizerState **ppRasterizerState)
{
	 mOrigContext1->RSGetState(ppRasterizerState);
}

STDMETHODIMP_(void) HackerContext::RSGetViewports(THIS_
	/* [annotation] */
	__inout /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumViewports,
	/* [annotation] */
	__out_ecount_opt(*pNumViewports)  D3D11_VIEWPORT *pViewports)
{
	 mOrigContext1->RSGetViewports(pNumViewports, pViewports);
}

STDMETHODIMP_(void) HackerContext::RSGetScissorRects(THIS_
	/* [annotation] */
	__inout /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumRects,
	/* [annotation] */
	__out_ecount_opt(*pNumRects)  D3D11_RECT *pRects)
{
	 mOrigContext1->RSGetScissorRects(pNumRects, pRects);
}

STDMETHODIMP_(void) HackerContext::HSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::HSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11HullShader **ppHullShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances);
}

STDMETHODIMP_(void) HackerContext::HSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->HSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::HSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::DSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::DSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11DomainShader **ppDomainShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances);
}

STDMETHODIMP_(void) HackerContext::DSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->DSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::DSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::CSGetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	 mOrigContext1->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::CSGetUnorderedAccessViews(THIS_
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
	/* [annotation] */
	__out_ecount(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	 mOrigContext1->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews);
}

STDMETHODIMP_(void) HackerContext::CSGetShader(THIS_
	/* [annotation] */
	__out  ID3D11ComputeShader **ppComputeShader,
	/* [annotation] */
	__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
	/* [annotation] */
	__inout_opt  UINT *pNumClassInstances)
{
	 mOrigContext1->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances);
}

STDMETHODIMP_(void) HackerContext::CSGetSamplers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
	/* [annotation] */
	__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	 mOrigContext1->CSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) HackerContext::CSGetConstantBuffers(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	 mOrigContext1->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) HackerContext::ClearState(THIS)
{
	 mOrigContext1->ClearState();

	 // TRIPLE: ClearState desbindea tambien los scissor rects del juego.
	 mTripleScissorShadowCount = 0;

	 // ClearState() will unbind StereoParams and IniParams, so we need to
	 // rebind them now:
	 Bind3DMigotoResources();
}

STDMETHODIMP_(void) HackerContext::Flush(THIS)
{
	 mOrigContext1->Flush();
}

STDMETHODIMP_(D3D11_DEVICE_CONTEXT_TYPE) HackerContext::GetType(THIS)
{
	return mOrigContext1->GetType();
}

STDMETHODIMP_(UINT) HackerContext::GetContextFlags(THIS)
{
	return mOrigContext1->GetContextFlags();
}

STDMETHODIMP HackerContext::FinishCommandList(THIS_
	BOOL RestoreDeferredContextState,
	/* [annotation] */
	__out_opt  ID3D11CommandList **ppCommandList)
{
	BOOL ret = mOrigContext1->FinishCommandList(RestoreDeferredContextState, ppCommandList);

	// TRIPLE: al cerrar la lista, el contexto diferido vuelve al estado por
	// defecto -> el rect del juego ya no esta puesto.
	if (!RestoreDeferredContextState)
		mTripleScissorShadowCount = 0;

	if (!RestoreDeferredContextState) {
		// This is equivalent to calling ClearState() afterwards, so we
		// need to rebind the 3DMigoto resources now. See also
		// ExecuteCommandList's RestoreContextState:
		Bind3DMigotoResources();
	}

	return ret;
}


// -----------------------------------------------------------------------------------------------

template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
		UINT StartSlot,
		UINT NumViews,
		ID3D11ShaderResourceView *const *ppShaderResourceViews)>
void HackerContext::BindStereoResources()
{
	if (!mHackerDevice) {
		LogInfo("  error querying device. Can't set NVidia stereo parameter texture.\n");
		return;
	}

	// Set NVidia stereo texture.
	if (mHackerDevice->mStereoResourceView && G->StereoParamsReg >= 0) {
		LogDebug("  adding NVidia stereo parameter texture to shader resources in slot %i.\n", G->StereoParamsReg);

		(mOrigContext1->*OrigSetShaderResources)(G->StereoParamsReg, 1, &mHackerDevice->mStereoResourceView);
	}

	// Set constants from ini file if they exist
	if (mHackerDevice->mIniResourceView && G->IniParamsReg >= 0) {
		LogDebug("  adding ini constants as texture to shader resources in slot %i.\n", G->IniParamsReg);

		(mOrigContext1->*OrigSetShaderResources)(G->IniParamsReg, 1, &mHackerDevice->mIniResourceView);
	}
}

void HackerContext::Bind3DMigotoResources()
{
	// Third generation of binding the stereo resources. We used to do this
	// in the SetShader calls, but that was problematic in certain games
	// like Akiba's Trip that would unbind all resources between then and
	// the following draw call. We then did this in the draw/dispatch
	// calls, which was very succesful, but cost a few percent CPU time
	// which can add up to a significant drop in framerate in CPU bound
	// games.
	//
	// Our new strategy is to bind them when the context is created, then
	// make sure that they stay bound in the SetShaderResource() calls. We
	// do this after the SetHackerDevice call because we need mHackerDevice
	BindStereoResources<&ID3D11DeviceContext::VSSetShaderResources>();
	BindStereoResources<&ID3D11DeviceContext::HSSetShaderResources>();
	BindStereoResources<&ID3D11DeviceContext::DSSetShaderResources>();
	BindStereoResources<&ID3D11DeviceContext::GSSetShaderResources>();
	BindStereoResources<&ID3D11DeviceContext::PSSetShaderResources>();
	BindStereoResources<&ID3D11DeviceContext::CSSetShaderResources>();
}

void HackerContext::InitIniParams()
{
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	HRESULT hr;

	// Only the immediate context is allowed to perform [Constants]
	// initialisation, as otherwise creating a deferred context could
	// clobber any changes since then. The only exception I can think of is
	// if a deferred context is created before the immediate context, but
	// an immediate context will have to be created before the frame ends
	// to execute that deferred context, so the worst case is that a
	// deferred context may run for one frame without the constants command
	// list being run. This situation is unlikely, and even if it does
	// happen is unlikely to cause any issues in practice, so let's not try
	// to do anything heroic to deal with it.
	if (mOrigContext1->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
		LogInfo("BUG: InitIniParams called on a deferred context\n");
		DoubleBeepExit();
	}

	// The command list only changes ini params that are defined, but for
	// consistency we want all other ini params to be initialised as well:
	memset(G->iniParams.data(), 0, sizeof(DirectX::XMFLOAT4) * G->iniParams.size());

	// Update the IniParams resource on the GPU before executing the
	// [Constants] command list. This ensures that it does get updated,
	// even if the [Constants] command list doesn't initialise any IniParam
	// (to non-zero), and we do this first in case [Constants] runs any
	// custom shaders that may check IniParams. This is a bit wasteful
	// since in most cases we will update the resource twice in a row, and
	// the alternative is setting a flag to force update_params in the
	// [Constants] command list, but this is a cold path so a little extra
	// overhead won't matter and I don't want to forget about this if
	// further command list optimisations cause [Constants] to bail out
	// early and not consider update_params at all.
	if (mHackerDevice->mIniTexture) {
		hr = mOrigContext1->Map(mHackerDevice->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			memcpy(mappedResource.pData, G->iniParams.data(), sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
			mOrigContext1->Unmap(mHackerDevice->mIniTexture, 0);
			Profiling::iniparams_updates++;
		} else {
			LogInfo("InitIniParams: Map failed\n");
		}
	}

	// The command list will take care of initialising any non-zero values:
	RunCommandList(mHackerDevice, this, &G->constants_command_list, NULL, false);
	// We don't consider persistent globals set in the [Constants] pre
	// command list as making the user config file dirty, because this
	// command list includes the user config file's [Constants] itself.
	// We clear only the low bit here, so that this may be overridden if
	// an invalid value is found that is scheduled to be removed:
	G->user_config_dirty &= ~1;
	RunCommandList(mHackerDevice, this, &G->post_constants_command_list, NULL, true);

	// Only want to run [Constants] on initial load and config reload. In
	// some games we see additional DirectX devices & contexts being
	// created (e.g. this happens in DOAXVV when displaying any news) and
	// we don't want to re-run Constants at that time since it may reset
	// variables and resources back to default.
	//
	// This may also happen in some game's init paths if they create some
	// throwaway devices (e.g. to probe the system's capabilities) or
	// potentially due to some overlays or other 3rd party tools. In most
	// of those cases running [Constants] once at launch still should be
	// the preferred option so that variables don't get reset unexpectedly,
	// though it is conceivable that in some cases we might want to reinit
	// custom resources, shaders, discard caches and re-run [Constants] at
	// that time. That could simplify cases where we create a custom
	// resource on one device and later need it on another (which we now do
	// through on-demand inter-device transfers - which has some
	// limitations), but I might wait until we have a proven need for that.
	G->constants_run = true;
}

// This function makes sure that the StereoParams and IniParams resources
// remain pinned whenever the game assigns shader resources:
template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
		UINT StartSlot,
		UINT NumViews,
		ID3D11ShaderResourceView *const *ppShaderResourceViews)>
void HackerContext::SetShaderResources(UINT StartSlot, UINT NumViews,
		ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	ID3D11ShaderResourceView **override_srvs = NULL;

	if (!mHackerDevice)
		return;

	if (mHackerDevice->mStereoResourceView && G->StereoParamsReg >= 0) {
		if (NumViews > G->StereoParamsReg - StartSlot) {
			LogDebug("  Game attempted to unbind StereoParams, pinning in slot %i\n", G->StereoParamsReg);
			override_srvs = new ID3D11ShaderResourceView*[NumViews];
			memcpy(override_srvs, ppShaderResourceViews, sizeof(ID3D11ShaderResourceView*) * NumViews);
			override_srvs[G->StereoParamsReg - StartSlot] = mHackerDevice->mStereoResourceView;
		}
	}

	if (mHackerDevice->mIniResourceView && G->IniParamsReg >= 0) {
		if (NumViews > G->IniParamsReg - StartSlot) {
			LogDebug("  Game attempted to unbind IniParams, pinning in slot %i\n", G->IniParamsReg);
			if (!override_srvs) {
				override_srvs = new ID3D11ShaderResourceView*[NumViews];
				memcpy(override_srvs, ppShaderResourceViews, sizeof(ID3D11ShaderResourceView*) * NumViews);
			}
			override_srvs[G->IniParamsReg - StartSlot] = mHackerDevice->mIniResourceView;
		}
	}

	if (override_srvs) {
		(mOrigContext1->*OrigSetShaderResources)(StartSlot, NumViews, override_srvs);
		delete [] override_srvs;
	} else {
		(mOrigContext1->*OrigSetShaderResources)(StartSlot, NumViews, ppShaderResourceViews);
	}
}

// The rest of these methods are all the primary code for the tool, Direct3D calls that we override
// in order to replace or modify shaders.

STDMETHODIMP_(void) HackerContext::VSSetShader(THIS_
	/* [annotation] */
	__in_opt ID3D11VertexShader *pVertexShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11VertexShader, &ID3D11DeviceContext::VSSetShader>
		(pVertexShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedVertexShaders,
		 G->mSelectedVertexShader,
		 &mCurrentVertexShader,
		 &mCurrentVertexShaderHandle);
}

STDMETHODIMP_(void) HackerContext::PSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	SetShaderResources<&ID3D11DeviceContext::PSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::PSSetShader(THIS_
	/* [annotation] */
	__in_opt ID3D11PixelShader *pPixelShader,
	/* [annotation] */
	__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
	UINT NumClassInstances)
{
	SetShader<ID3D11PixelShader, &ID3D11DeviceContext::PSSetShader>
		(pPixelShader, ppClassInstances, NumClassInstances,
		 &G->mVisitedPixelShaders,
		 G->mSelectedPixelShader,
		 &mCurrentPixelShader,
		 &mCurrentPixelShaderHandle);

	if (pPixelShader) {
		// Set custom depth texture.
		if (mHackerDevice->mZBufferResourceView)
		{
			LogDebug("  adding Z buffer to shader resources in slot 126.\n");

			mOrigContext1->PSSetShaderResources(126, 1, &mHackerDevice->mZBufferResourceView);
		}
	}
}

STDMETHODIMP_(void) HackerContext::DrawIndexed(THIS_
	/* [annotation] */
	__in  UINT IndexCount,
	/* [annotation] */
	__in  UINT StartIndexLocation,
	/* [annotation] */
	__in  INT BaseVertexLocation)
{
	triple_medir_scatter(mCurrentVertexShader);
	triple_medir_sombras(mCurrentPixelShader);
	triple_medir_pasto_scatter(mCurrentVertexShader, IndexCount);
	triple_medir_zonas(mCurrentVertexShader, IndexCount, 1);
	DrawContext c = DrawContext(DrawCall::DrawIndexed, 0, IndexCount, 0, BaseVertexLocation, StartIndexLocation, 0, NULL, 0);
	BeforeDraw(c);

	if (!c.call_info.skip)
		 mOrigContext1->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::Draw(THIS_
	/* [annotation] */
	__in  UINT VertexCount,
	/* [annotation] */
	__in  UINT StartVertexLocation)
{
	triple_medir_scatter(mCurrentVertexShader);
	triple_medir_sombras(mCurrentPixelShader);
	triple_medir_pasto_scatter(mCurrentVertexShader, VertexCount);
	triple_medir_zonas(mCurrentVertexShader, VertexCount, 1);
	DrawContext c = DrawContext(DrawCall::Draw, VertexCount, 0, 0, StartVertexLocation, 0, 0, NULL, 0);
	BeforeDraw(c);

	if (!c.call_info.skip)
		 mOrigContext1->Draw(VertexCount, StartVertexLocation);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::IASetIndexBuffer(THIS_
	/* [annotation] */
	__in_opt ID3D11Buffer *pIndexBuffer,
	/* [annotation] */
	__in DXGI_FORMAT Format,
	/* [annotation] */
	__in  UINT Offset)
{
	mOrigContext1->IASetIndexBuffer(pIndexBuffer, Format, Offset);

	// Sin gate de hunting: se usa para identificar el auto por su malla en juego
	// normal. El hash sale de un mapa poblado al crear el recurso, asi que esto
	// es una busqueda y nada mas.
	mTripleIB = pIndexBuffer ? GetResourceHash(pIndexBuffer) : 0;

	// This is only used for index buffer hunting nowadays since the
	// command list checks the hash on demand only when it is needed
	mCurrentIndexBuffer = 0;
	if (pIndexBuffer && G->hunting == HUNTING_MODE_ENABLED) {
		mCurrentIndexBuffer = GetResourceHash(pIndexBuffer);
		if (mCurrentIndexBuffer) {
			// When hunting, save this as a visited index buffer to cycle through.
			EnterCriticalSectionPretty(&G->mCriticalSection);
			G->mVisitedIndexBuffers.insert(mCurrentIndexBuffer);
			LeaveCriticalSection(&G->mCriticalSection);
		}
	}
}

STDMETHODIMP_(void) HackerContext::DrawIndexedInstanced(THIS_
	/* [annotation] */
	__in  UINT IndexCountPerInstance,
	/* [annotation] */
	__in  UINT InstanceCount,
	/* [annotation] */
	__in  UINT StartIndexLocation,
	/* [annotation] */
	__in  INT BaseVertexLocation,
	/* [annotation] */
	__in  UINT StartInstanceLocation)
{
	triple_medir_scatter(mCurrentVertexShader);
	triple_medir_sombras(mCurrentPixelShader);
	triple_medir_pasto_scatter(mCurrentVertexShader, IndexCountPerInstance);
	triple_medir_zonas(mCurrentVertexShader, IndexCountPerInstance, InstanceCount);
	DrawContext c = DrawContext(DrawCall::DrawIndexedInstanced, 0, IndexCountPerInstance, InstanceCount, BaseVertexLocation, StartIndexLocation, StartInstanceLocation, NULL, 0);
	BeforeDraw(c);

	if (!c.call_info.skip)
		mOrigContext1->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
		BaseVertexLocation, StartInstanceLocation);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::DrawInstanced(THIS_
	/* [annotation] */
	__in  UINT VertexCountPerInstance,
	/* [annotation] */
	__in  UINT InstanceCount,
	/* [annotation] */
	__in  UINT StartVertexLocation,
	/* [annotation] */
	__in  UINT StartInstanceLocation)
{
	triple_medir_scatter(mCurrentVertexShader);
	triple_medir_sombras(mCurrentPixelShader);
	triple_medir_pasto_scatter(mCurrentVertexShader, VertexCountPerInstance);
	triple_medir_zonas(mCurrentVertexShader, VertexCountPerInstance, InstanceCount);
	DrawContext c = DrawContext(DrawCall::DrawInstanced, VertexCountPerInstance, 0, InstanceCount, StartVertexLocation, 0, StartInstanceLocation, NULL, 0);
	BeforeDraw(c);

	if (!c.call_info.skip)
		mOrigContext1->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::VSSetShaderResources(THIS_
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
	/* [annotation] */
	__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	// v1.52: sombra de los primeros slots de SRV del vertex shader. Antes se
	// guardaba solo el slot 1 y no alcanzaba: hay varios buffers con el mismo
	// stride, y adivinar cual es el del pasto por su contenido lleva a
	// confundirlo con los del spline de la pista (que tambien son posiciones).
	// Con la sombra completa, el draw del pasto dice EXACTAMENTE que usa.
	if (ppShaderResourceViews) {
		for (UINT i = 0; i < NumViews; i++) {
			UINT slot = StartSlot + i;
			if (slot >= 32)
				break;
			ID3D11Resource *res = NULL;
			if (ppShaderResourceViews[i])
				ppShaderResourceViews[i]->GetResource(&res);
			tls_vs_srv[slot] = res;
			if (res)
				res->Release();
		}
	}

	SetShaderResources<&ID3D11DeviceContext::VSSetShaderResources>(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) HackerContext::OMSetRenderTargets(THIS_
	/* [annotation] */
	__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
	/* [annotation] */
	__in_ecount_opt(NumViews) ID3D11RenderTargetView *const *ppRenderTargetViews,
	/* [annotation] */
	__in_opt ID3D11DepthStencilView *pDepthStencilView)
{
	Profiling::State profiling_state;

	if (G->hunting == HUNTING_MODE_ENABLED) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
			mCurrentRenderTargets.clear();
			mCurrentDepthTarget = NULL;
			mCurrentPSNumUAVs = 0;
		LeaveCriticalSection(&G->mCriticalSection);

		if (G->DumpUsage) {
			if (Profiling::mode == Profiling::Mode::SUMMARY)
				Profiling::start(&profiling_state);

			if (ppRenderTargetViews) {
				for (UINT i = 0; i < NumViews; ++i) {
					if (!ppRenderTargetViews[i])
						continue;
					RecordRenderTargetInfo(ppRenderTargetViews[i], i);
				}
			}

			RecordDepthStencil(pDepthStencilView);

			if (Profiling::mode == Profiling::Mode::SUMMARY)
				Profiling::end(&profiling_state, &Profiling::stat_overhead);
		}
	}

	mOrigContext1->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

STDMETHODIMP_(void) HackerContext::OMSetRenderTargetsAndUnorderedAccessViews(THIS_
	/* [annotation] */
	__in  UINT NumRTVs,
	/* [annotation] */
	__in_ecount_opt(NumRTVs) ID3D11RenderTargetView *const *ppRenderTargetViews,
	/* [annotation] */
	__in_opt ID3D11DepthStencilView *pDepthStencilView,
	/* [annotation] */
	__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
	/* [annotation] */
	__in  UINT NumUAVs,
	/* [annotation] */
	__in_ecount_opt(NumUAVs) ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
	/* [annotation] */
	__in_ecount_opt(NumUAVs)  const UINT *pUAVInitialCounts)
{
	Profiling::State profiling_state;

	if (G->hunting == HUNTING_MODE_ENABLED) {
		EnterCriticalSectionPretty(&G->mCriticalSection);

		if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
			mCurrentRenderTargets.clear();
			mCurrentDepthTarget = NULL;
			if (G->DumpUsage) {
				if (Profiling::mode == Profiling::Mode::SUMMARY)
					Profiling::start(&profiling_state);

				if (ppRenderTargetViews) {
					for (UINT i = 0; i < NumRTVs; ++i) {
						if (ppRenderTargetViews[i])
							RecordRenderTargetInfo(ppRenderTargetViews[i], i);
					}
				}
				RecordDepthStencil(pDepthStencilView);

				if (Profiling::mode == Profiling::Mode::SUMMARY)
					Profiling::end(&profiling_state, &Profiling::stat_overhead);
			}
		}

		if (NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
			mCurrentPSUAVStartSlot = UAVStartSlot;
			mCurrentPSNumUAVs = NumUAVs;
			// TODO: Record UAV stats
		}

		LeaveCriticalSection(&G->mCriticalSection);
	}

	mOrigContext1->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, pDepthStencilView,
		UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

STDMETHODIMP_(void) HackerContext::DrawAuto(THIS)
{
	DrawContext c = DrawContext(DrawCall::DrawAuto, 0, 0, 0, 0, 0, 0, NULL, 0);
	BeforeDraw(c);

	if (!c.call_info.skip)
		mOrigContext1->DrawAuto();
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::DrawIndexedInstancedIndirect(THIS_
	/* [annotation] */
	__in  ID3D11Buffer *pBufferForArgs,
	/* [annotation] */
	__in  UINT AlignedByteOffsetForArgs)
{
	// v1.42: si el pasto se generara en GPU, se dibujaria por aca (el conteo de
	// instancias lo escribe un compute shader). Censar el VS de estos draws
	// dice si esa via existe en este juego — y si no aparece ninguno, el pasto
	// se siembra en CPU y hay que buscar el filtro alli.


	DrawContext c = DrawContext(DrawCall::DrawIndexedInstancedIndirect, 0, 0, 0, 0, 0, 0, &pBufferForArgs, AlignedByteOffsetForArgs);
	BeforeDraw(c);

	if (!c.call_info.skip)
		mOrigContext1->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::DrawInstancedIndirect(THIS_
	/* [annotation] */
	__in  ID3D11Buffer *pBufferForArgs,
	/* [annotation] */
	__in  UINT AlignedByteOffsetForArgs)
{
	// v1.53: el pasto puede venir por CUALQUIERA de los dos draws indirectos.

	DrawContext c = DrawContext(DrawCall::DrawInstancedIndirect, 0, 0, 0, 0, 0, 0, &pBufferForArgs, AlignedByteOffsetForArgs);
	BeforeDraw(c);

	if (!c.call_info.skip)
		mOrigContext1->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
	AfterDraw(c);
}

STDMETHODIMP_(void) HackerContext::ClearRenderTargetView(THIS_
	/* [annotation] */
	__in  ID3D11RenderTargetView *pRenderTargetView,
	/* [annotation] */
	__in  const FLOAT ColorRGBA[4])
{
	RunViewCommandList(mHackerDevice, this, &G->clear_rtv_command_list, pRenderTargetView, false);
	mOrigContext1->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
	RunViewCommandList(mHackerDevice, this, &G->post_clear_rtv_command_list, pRenderTargetView, true);
}


// -----------------------------------------------------------------------------
// Sort of HackerContext1
//	Requires Win7 Platform Update

// Hierarchy:
//  HackerContext <- ID3D11DeviceContext1 <- ID3D11DeviceContext <- ID3D11DeviceChild <- IUnknown


void STDMETHODCALLTYPE HackerContext::CopySubresourceRegion1(
	/* [annotation] */
	_In_  ID3D11Resource *pDstResource,
	/* [annotation] */
	_In_  UINT DstSubresource,
	/* [annotation] */
	_In_  UINT DstX,
	/* [annotation] */
	_In_  UINT DstY,
	/* [annotation] */
	_In_  UINT DstZ,
	/* [annotation] */
	_In_  ID3D11Resource *pSrcResource,
	/* [annotation] */
	_In_  UINT SrcSubresource,
	/* [annotation] */
	_In_opt_  const D3D11_BOX *pSrcBox,
	/* [annotation] */
	_In_  UINT CopyFlags)
{
	mOrigContext1->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
}

void STDMETHODCALLTYPE HackerContext::UpdateSubresource1(
	/* [annotation] */
	_In_  ID3D11Resource *pDstResource,
	/* [annotation] */
	_In_  UINT DstSubresource,
	/* [annotation] */
	_In_opt_  const D3D11_BOX *pDstBox,
	/* [annotation] */
	_In_  const void *pSrcData,
	/* [annotation] */
	_In_  UINT SrcRowPitch,
	/* [annotation] */
	_In_  UINT SrcDepthPitch,
	/* [annotation] */
	_In_  UINT CopyFlags)
{
	mOrigContext1->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);

	// TODO: Track resource hash updates
}


void STDMETHODCALLTYPE HackerContext::DiscardResource(
	/* [annotation] */
	_In_  ID3D11Resource *pResource)
{
	mOrigContext1->DiscardResource(pResource);
}

void STDMETHODCALLTYPE HackerContext::DiscardView(
	/* [annotation] */
	_In_  ID3D11View *pResourceView)
{
	mOrigContext1->DiscardView(pResourceView);
}


void STDMETHODCALLTYPE HackerContext::VSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::HSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::DSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::GSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::PSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::CSSetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
	/* [annotation] */
	_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	mOrigContext1->CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::VSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::HSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}



void STDMETHODCALLTYPE HackerContext::DSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::GSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::PSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::CSGetConstantBuffers1(
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
	/* [annotation] */
	_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
	/* [annotation] */
	_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext1->CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}


void STDMETHODCALLTYPE HackerContext::SwapDeviceContextState(
	/* [annotation] */
	_In_  ID3DDeviceContextState *pState,
	/* [annotation] */
	_Out_opt_  ID3DDeviceContextState **ppPreviousState)
{
	mOrigContext1->SwapDeviceContextState(pState, ppPreviousState);

	// If a game or overlay creates separate context state objects we won't
	// have had a chance to bind the 3DMigoto resources when it was
	// first created, so do so now:
	Bind3DMigotoResources();
}


void STDMETHODCALLTYPE HackerContext::ClearView(
	/* [annotation] */
	_In_  ID3D11View *pView,
	/* [annotation] */
	_In_  const FLOAT Color[4],
	/* [annotation] */
	_In_reads_opt_(NumRects)  const D3D11_RECT *pRect,
	UINT NumRects)
{
	// TODO: Add a command list here, but we probably actualy want to call
	// the existing RTV / DSV / UAV clear command lists instead for
	// compatibility with engines that might use this if the feature level
	// is high enough, and the others if it is not.
	mOrigContext1->ClearView(pView, Color, pRect, NumRects);
}


void STDMETHODCALLTYPE HackerContext::DiscardView1(
	/* [annotation] */
	_In_  ID3D11View *pResourceView,
	/* [annotation] */
	_In_reads_opt_(NumRects)  const D3D11_RECT *pRects,
	UINT NumRects)
{
	mOrigContext1->DiscardView1(pResourceView, pRects, NumRects);
}
