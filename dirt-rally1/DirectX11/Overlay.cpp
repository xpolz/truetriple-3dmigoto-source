// This Overlay class is to encapsulate all the on-screen drawing code,
// including creating and using the DirectXTK code.

#include "Overlay.h"

#include <stdexcept>
#include <DirectXColors.h>
//#include <StrSafe.h>

#include "SimpleMath.h"
#include "SpriteBatch.h"

#include "log.h"
#include "version.h"
#include "D3D11Wrapper.h"
//#include "nvapi.h"
#include "Globals.h"
#include "profiling.h"
#include "CommandList.h"

#include "HackerDevice.h"
#include "HackerContext.h"

#define MAX_SIMULTANEOUS_NOTICES 10

static bool has_notice = false;
static unsigned notice_cleared_frame = 0;

static class Notices
{
public:
	std::vector<OverlayNotice> notices[NUM_LOG_LEVELS];
	CRITICAL_SECTION lock;

	Notices()
	{
		InitializeCriticalSectionPretty(&lock);
	}

	~Notices()
	{
		DeleteCriticalSection(&lock);
	}
} notices;

struct LogLevelParams {
	DirectX::XMVECTORF32 colour;
	DWORD duration;
	bool hide_in_release;
	std::unique_ptr<DirectX::SpriteFont> Overlay::*font;
};

struct LogLevelParams log_levels[] = {
	{ DirectX::Colors::Red,       20000, false, &Overlay::mFontNotifications }, // DIRE
	{ DirectX::Colors::OrangeRed, 10000, false, &Overlay::mFontNotifications }, // WARNING
	{ DirectX::Colors::OrangeRed, 10000, false, &Overlay::mFontProfiling     }, // WARNING_MONOSPACE
	{ DirectX::Colors::Orange,     5000,  true, &Overlay::mFontNotifications }, // NOTICE
	{ DirectX::Colors::LimeGreen,  2000,  true, &Overlay::mFontNotifications }, // INFO
};

// Side note: Not really stoked with C++ string handling.  There are like 4 or
// 5 different ways to do things, all partly compatible, none a clear winner in
// terms of simplicity and clarity.  Generally speaking we'd want to use C++
// wstring and string, but there are no good output formatters.  Maybe the 
// newer iostream based pieces, but we'd still need to convert.
//
// The philosophy here and in other files, is to use whatever the API that we
// are using wants.  In this case it's a wchar_t* for DrawString, so we'll not
// do a lot of conversions and different formats, we'll just use wchar_t and its
// formatters.
//
// In particular, we also want to avoid 5 different libraries for string handling,
// Microsoft has way too many variants.  We'll use the regular C library from
// the standard c runtime, but use the _s safe versions.

// Max expected on-screen string size, used for buffer safe calls.
const int maxstring = 1024;


Overlay::Overlay(HackerDevice *pDevice, HackerContext *pContext, IDXGISwapChain *pSwapChain)
{
	LogInfo("Overlay::Overlay created for %p\n", pSwapChain);
	LogInfo("  on HackerDevice: %p, HackerContext: %p\n", pDevice, pContext);

	// Drawing environment for this swap chain. This is the game environment.
	// These should specifically avoid Hacker* objects, to avoid object
	// callbacks or other problems. We just want to draw here, nothing tricky.
	// The only exception being that we need the HackerDevice in order to
	// draw the current stereoparams.
	mHackerDevice = pDevice;
	mHackerContext = pContext;
	mOrigSwapChain = pSwapChain;

	// Must use trampoline context to prevent 3DMigoto hunting its own overlay:
	mOrigDevice = mHackerDevice->GetPassThroughOrigDevice1();
	mOrigContext = pContext->GetPassThroughOrigContext1();

	// We are actively using the Device and Context, so we need to make
	// sure they do not get Released without us.  This happened in FFXIV.
	//
	// We do not hold a reference on the swap chain, as that would prevent
	// the swap chain from being released until the overlay is deleted, but
	// the overlay will not be deleted until the swap chain is released,
	// and since the overlay also holds references to the device and
	// context this would prevent everything from being released. So long
	// as the overlay exists we know the swap chain hasn't been released
	// yet, so this is safe.
	//
	// Alternatively, we could forgo holding pointers to these at all,
	// since we can always get access to the device and immediate context
	// via SwapChain->GetParent(ID3D11Device) and GetImmediateContext(),
	// but that would mean extra calls in a fast path.
	//
	// Note that the swap chain itself also holds references to these two
	// now, so this is technically unecessary, but since the overlay code
	// still accesses these it is more safer to leave this in place (more
	// resistant to code changes in the swap chain breaking this).
	mHackerDevice->AddRef();
	mHackerContext->AddRef();

	// The courierbold.spritefont is now included as binary resource data attached
	// to the d3d11.dll.  We can fetch that resource and pass it to new SpriteFont
	// to avoid having to drag around the font file. We get the module
	// handle by address to ensure we don't get some other d3d11.dll, which
	// is of particular importance when we are injected into a Windows
	// Store app and may not even be called that ourselves.
	HMODULE handle = NULL;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
			| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)LogOverlay, &handle);
	HRSRC rc = FindResource(handle, MAKEINTRESOURCE(IDR_COURIERBOLD), MAKEINTRESOURCE(SPRITEFONT));
	HGLOBAL rcData = LoadResource(handle, rc);
	DWORD fontSize = SizeofResource(handle, rc);
	uint8_t const* fontBlob = static_cast<const uint8_t*>(LockResource(rcData));

	// We want to use the original device and original context here, because
	// these will be used by DirectXTK to generate VertexShaders and PixelShaders
	// to draw the text, and we don't want to intercept those.
	mFont.reset(new DirectX::SpriteFont(mOrigDevice, fontBlob, fontSize));
	mFont->SetDefaultCharacter(L'?');

	// Courier is a nice choice for hunting status lines, and showing the
	// shader hashes since it is monospace, but for arbitrary notifications
	// we want something a little smaller, and variable width. Liberation
	// Sans has essentially the same metrics as Arial,
	// but is not encumbered.
	rc = FindResource(handle, MAKEINTRESOURCE(IDR_ARIAL), MAKEINTRESOURCE(SPRITEFONT));
	rcData = LoadResource(handle, rc);
	fontSize = SizeofResource(handle, rc);
	fontBlob = static_cast<const uint8_t*>(LockResource(rcData));
	mFontNotifications.reset(new DirectX::SpriteFont(mOrigDevice, fontBlob, fontSize));
	mFontNotifications->SetDefaultCharacter(L'?');

	// Smaller monospaced font for profiling text
	rc = FindResource(handle, MAKEINTRESOURCE(IDR_COURIERSMALL), MAKEINTRESOURCE(SPRITEFONT));
	rcData = LoadResource(handle, rc);
	fontSize = SizeofResource(handle, rc);
	fontBlob = static_cast<const uint8_t*>(LockResource(rcData));
	mFontProfiling.reset(new DirectX::SpriteFont(mOrigDevice, fontBlob, fontSize));
	mFontProfiling->SetDefaultCharacter(L'?');

	mSpriteBatch.reset(new DirectX::SpriteBatch(mOrigContext));

	// For dark background behind notification text, following
	// https://github.com/Microsoft/DirectXTK/wiki/Simple-rendering
	mStates.reset(new DirectX::CommonStates(mOrigDevice));
	mEffect.reset(new DirectX::BasicEffect(mOrigDevice));

	void const *shaderByteCode;
	size_t byteCodeLength;

	mEffect->SetVertexColorEnabled(true);
	mEffect->GetVertexShaderBytecode(&shaderByteCode, &byteCodeLength);

	HRESULT hr = mOrigDevice->CreateInputLayout(DirectX::VertexPositionColor::InputElements,
			DirectX::VertexPositionColor::InputElementCount,
			shaderByteCode, byteCodeLength,
			mInputLayout.ReleaseAndGetAddressOf());
	if (FAILED(hr))
		throw std::runtime_error("CreateInputLayout failed");

	mPrimitiveBatch.reset(new DirectX::PrimitiveBatch<DirectX::VertexPositionColor>(mOrigContext));
}

Overlay::~Overlay()
{
	LogInfo("Overlay::~Overlay deleted for SwapChain %p\n", mOrigSwapChain);
	// We Release the same interface we called AddRef on, and we use the
	// Hacker interfaces to make sure that our cleanup code is run if this
	// is the last reference.
	mHackerContext->Release();
	mHackerDevice->Release();
}


// -----------------------------------------------------------------------------

using namespace DirectX::SimpleMath;

// Expected to be called at DXGI::Present() to be the last thing drawn.

// Notes:
	//1) Active PS location(probably x / N format)
	//2) Active VS location(x / N format)
	//3) Current convergence and separation. (convergence, a must)
	//4) Error state of reload(syntax errors go red or something)
	//5) Duplicate Mark(maybe yellow text for location, red if Decompile failed)

	//Maybe:
	//5) Other state, like show_original active.
	//6) Active toggle override.


// We need to save off everything that DirectTK will clobber and
// restore it before returning to the application. This is necessary
// to prevent rendering issues in some games like The Long Dark, and
// helps avoid introducing pipeline errors in other games like The
// Witcher 3.
// Only saving and restoring the first RenderTarget, as the only one
// we change for drawing overlay.

void Overlay::SaveState()
{
	memset(&state, 0, sizeof(state));

	save_om_state(mOrigContext, &state.om_state);
	state.RSNumViewPorts = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	mOrigContext->RSGetViewports(&state.RSNumViewPorts, state.pViewPorts);

	mOrigContext->OMGetBlendState(&state.pBlendState, state.BlendFactor, &state.SampleMask);
	mOrigContext->OMGetDepthStencilState(&state.pDepthStencilState, &state.StencilRef);
	mOrigContext->RSGetState(&state.pRasterizerState);
	mOrigContext->PSGetSamplers(0, 1, state.samplers);
	mOrigContext->IAGetPrimitiveTopology(&state.topology);
	mOrigContext->IAGetInputLayout(&state.pInputLayout);
	mOrigContext->VSGetShader(&state.pVertexShader, state.pVSClassInstances, &state.VSNumClassInstances);
	mOrigContext->PSGetShader(&state.pPixelShader, state.pPSClassInstances, &state.PSNumClassInstances);
	mOrigContext->IAGetVertexBuffers(0, 1, state.pVertexBuffers, state.Strides, state.Offsets);
	mOrigContext->IAGetIndexBuffer(&state.IndexBuffer, &state.Format, &state.Offset);
	mOrigContext->VSGetConstantBuffers(0, 1, state.pVSConstantBuffers);
	mOrigContext->PSGetConstantBuffers(0, 1, state.pPSConstantBuffers);
	mOrigContext->PSGetShaderResources(0, 1, state.pShaderResourceViews);
}

void Overlay::RestoreState()
{
	unsigned i;

	restore_om_state(mOrigContext, &state.om_state);

	mOrigContext->RSSetViewports(state.RSNumViewPorts, state.pViewPorts);

	mOrigContext->OMSetBlendState(state.pBlendState, state.BlendFactor, state.SampleMask);
	if (state.pBlendState)
		state.pBlendState->Release();

	mOrigContext->OMSetDepthStencilState(state.pDepthStencilState, state.StencilRef);
	if (state.pDepthStencilState)
		state.pDepthStencilState->Release();

	mOrigContext->RSSetState(state.pRasterizerState);
	if (state.pRasterizerState)
		state.pRasterizerState->Release();

	mOrigContext->PSSetSamplers(0, 1, state.samplers);
	if (state.samplers[0])
		state.samplers[0]->Release();

	mOrigContext->IASetPrimitiveTopology(state.topology);

	mOrigContext->IASetInputLayout(state.pInputLayout);
	if (state.pInputLayout)
		state.pInputLayout->Release();

	mOrigContext->VSSetShader(state.pVertexShader, state.pVSClassInstances, state.VSNumClassInstances);
	if (state.pVertexShader)
		state.pVertexShader->Release();
	for (i = 0; i < state.VSNumClassInstances; i++)
		state.pVSClassInstances[i]->Release();

	mOrigContext->PSSetShader(state.pPixelShader, state.pPSClassInstances, state.PSNumClassInstances);
	if (state.pPixelShader)
		state.pPixelShader->Release();
	for (i = 0; i < state.PSNumClassInstances; i++)
		state.pPSClassInstances[i]->Release();

	mOrigContext->IASetVertexBuffers(0, 1, state.pVertexBuffers, state.Strides, state.Offsets);
	if (state.pVertexBuffers[0])
		state.pVertexBuffers[0]->Release();

	mOrigContext->IASetIndexBuffer(state.IndexBuffer, state.Format, state.Offset);
	if (state.IndexBuffer)
		state.IndexBuffer->Release();

	mOrigContext->VSSetConstantBuffers(0, 1, state.pVSConstantBuffers);
	if (state.pVSConstantBuffers[0])
		state.pVSConstantBuffers[0]->Release();

	mOrigContext->PSSetConstantBuffers(0, 1, state.pPSConstantBuffers);
	if (state.pPSConstantBuffers[0])
		state.pPSConstantBuffers[0]->Release();

	mOrigContext->PSSetShaderResources(0, 1, state.pShaderResourceViews);
	if (state.pShaderResourceViews[0])
		state.pShaderResourceViews[0]->Release();
}

#ifdef NTDDI_WIN10
#include <d3d11on12.h>
#include <dxgi1_4.h>
static ID3D11Texture2D* get_11on12_backbuffer(ID3D11Device *mOrigDevice, IDXGISwapChain *mOrigSwapChain)
{
	ID3D12Resource *d3d12_bb = NULL;
	ID3D11Texture2D *d3d11_bb = NULL;
	ID3D11On12Device *d3d11on12_dev = NULL;
	IDXGISwapChain3 *swap_chain_3 = NULL;
	D3D11_RESOURCE_FLAGS flags = { D3D11_BIND_RENDER_TARGET };
	UINT bb_idx;
	HRESULT hr;

	if (FAILED(mOrigDevice->QueryInterface(IID_ID3D11On12Device, (void**)&d3d11on12_dev)))
		return NULL;
	LogDebug("  ID3D11On12Device: %p\n", d3d11on12_dev);

	// In D3D12 we need to make sure we are writing to the correct back
	// buffer for the current frame, and failing to do this will lead to a
	// DXGI_ERROR_ACCESS_DENIED. This differs from DX11 where index 0
	// always points to the current back buffer. We need the SwapChain3
	// interface to determine which back buffer is currently active:
	if (FAILED(mOrigSwapChain->QueryInterface(IID_IDXGISwapChain3, (void**)&swap_chain_3)))
		goto out;
	bb_idx = swap_chain_3->GetCurrentBackBufferIndex();
	LogDebug("  Current Back Buffer Index: %i\n", bb_idx);

	if (FAILED(mOrigSwapChain->GetBuffer(bb_idx, IID_ID3D12Resource, (void**)&d3d12_bb)))
		goto out;
	LogDebug("  ID3D12Resource: %p\n", d3d12_bb);

	// At the moment I'm creating a wrapped resource every frame, though
	// the 11on12 sample code does this once for every back buffer index
	// and uses AcquireWrappedResources() instead each frame. The sample
	// code also doesn't cope with resizing swap chains on alt+enter, so
	// I'm not at all confident that following it to the letter would work.
	// We know holding a reference to a back buffer will prevent alt+enter
	// from working - maybe Acquire gets around that, but I don't trust it.
	//
	// I'm specifying RT -> PRESENT state transitions here, which follows
	// the 11to12 sample code, but I don't think this is strictly correct -
	// surely the game should have already transitioned the back buffer to
	// the PRESENT state before calling Present(), in which case shouldn't
	// our in state be PRESENT, not RT? However, while both cases seem to
	// work just fine, in Fifa 2018 the debug layer seems to indicate both
	// are wrong - RT->PRESENT says the barrier doesn't match the current
	// resource state of RT, and PRESENT->PRESENT says it doesn't match the
	// previous barrier transitioning it to RT.
	//
	// ... I don't even ...
	//
	// ... What? ...
	//
	// ... That doesn't make sense!!! Maybe they have a game bug and left
	// it as RT by mistake, but that still doesn't make sense!!! I'm either
	// misinterpreting one of the debug messages or I'm missing something.
	//
	// AFAICT there's no way to query the current "state" to answer this
	// correctly, because despite the name there is actually no "state"
	// anywhere outside of the debug layer. D3D11 tracked this stuff and
	// automatically submitted the correct memory barrier type to the GPU,
	// but DX12 saves some CPU cycles and doesn't track anything, leaving
	// it up to the application to know which barrier to use, and the
	// "state transition" is really just a way to assist an inexperienced
	// developer in choosing the right one (that's actually kind of clever
	// - memory barriers are really easy to mess up and not realise) - so,
	// this isn't a state transition at all, it's a memory barrier.
	//
	// Okay, so it's a memory barrier - knowing that, what would be the
	// safest thing to do? Well, if the game already transitioned to
	// PRESENT, then they already did a memory barrier of their own, and so
	// long as we do another after we finish writing to the back buffer it
	// should be fine, and shouldn't matter if we start writing to it now
	// without another barrier to transition it back to RT. If the game
	// didn't transition it to PRESENT, and the last barrier they did left
	// it as RT, then (they have a bug) it won't matter if we start writing
	// to it immediately as is, but we should still do a memory barrier
	// once we are finished. Ok, either way I don't think it actually
	// matters unless they have done something very weird and we need a
	// different barrier altogether, or I haven't considered some subtlty -
	// let's go with RT->PRESENT for now and see how it goes in practice:
	hr = d3d11on12_dev->CreateWrappedResource(d3d12_bb, &flags,
			D3D12_RESOURCE_STATE_RENDER_TARGET, /* in "state" */
			D3D12_RESOURCE_STATE_PRESENT, /* out "state" */
			IID_ID3D11Texture2D, (void**)&d3d11_bb);
	LogDebug("  ID3D11Texture2D: %p, result: 0x%x\n", d3d11_bb, hr);

out:
	if (d3d12_bb)
		d3d12_bb->Release();
	if (swap_chain_3)
		swap_chain_3->Release();
	if (d3d11on12_dev)
		d3d11on12_dev->Release();

	return d3d11_bb;
}

static void flush_d3d11on12(ID3D11Device *mOrigDevice, ID3D11DeviceContext *mOrigContext)
{
	ID3D11On12Device *d3d11on12_dev = NULL;

	if (FAILED(mOrigDevice->QueryInterface(IID_ID3D11On12Device, (void**)&d3d11on12_dev)))
		return;

	// We need to tell 11on12 to release the resources it is wrapping,
	// otherwise it will still hold a reference to the back buffer which
	// will prevent alt+enter from working. This should also transition the
	// D3D12 resource state from RENDER_TARGET to PRESENT. We haven't
	// bothered keeping our own reference to the back buffer, so just have
	// it release all of them - that should be fine unless the game or
	// another overlay is also using 11on12 and for some reason did not
	// release a resource and isn't expecting to have to re-acquire it:
	d3d11on12_dev->ReleaseWrappedResources(NULL, 0);
	d3d11on12_dev->Release();

	// D3D11 Immediate context must be flushed before any further D3D12
	// work can be performed. This should be done as late as possible to
	// ensure no commands are queued up, and in particular needs to be done
	// after we have released any references to the back buffer, including
	// unbinding it from the pipeline, and after ReleaseWrappedResources(),
	// since all the releases can be delayed:
	mOrigContext->Flush();
}

#else

static ID3D11Texture2D* get_11on12_backbuffer(ID3D11Device *mOrigDevice, IDXGISwapChain *mOrigSwapChain)
{
	return NULL;
}

static void flush_d3d11on12(ID3D11Device *mOrigDevice, ID3D11DeviceContext *mOrigContext)
{
}

#endif

// We can't trust the game to have a proper drawing environment for DirectXTK.
//
// For two games we know of (Batman Arkham Knight and Project Cars) we were not
// getting an overlay, because apparently the rendertarget was left in an odd
// state.  This adds an init to be certain that the rendertarget is the backbuffer
// so that the overlay is drawn. 

HRESULT Overlay::InitDrawState()
{
	HRESULT hr;

	ID3D11Texture2D *pBackBuffer;
	hr = mOrigSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
	if (FAILED(hr)) {
		// The back buffer doesn't support the D3D11 Texture2D
		// interface. Maybe this is DX12 - if we have been built with
		// the Win 10 SDK we can handle that via 11On12:
		pBackBuffer = get_11on12_backbuffer(mOrigDevice, mOrigSwapChain);
		if (!pBackBuffer)
			return hr;
	}

	// By doing this every frame, we are always up to date with correct size,
	// and we need the address of the BackBuffer anyway, so this is low cost.
	D3D11_TEXTURE2D_DESC description;
	pBackBuffer->GetDesc(&description);
	mResolution = DirectX::XMUINT2(description.Width, description.Height);

	// use the back buffer address to create the render target
	ID3D11RenderTargetView *backbuffer;
	hr = mOrigDevice->CreateRenderTargetView(pBackBuffer, NULL, &backbuffer);

	pBackBuffer->Release();

	if (FAILED(hr))
		return hr;

	// set the first render target as the back buffer, with no stencil
	mOrigContext->OMSetRenderTargets(1, &backbuffer, NULL);

	// Holding onto a view of the back buffer can cause a crash on
	// ResizeBuffers, so it is very important we release it here - it will
	// still have a reference so long as it is bound to the pipeline -
	// i.e. until RestoreState() unbinds it. Holding onto this view caused
	// a crash in Mass Effect Andromeda when toggling full screen if the
	// hunting overlay had ever been displayed since launch.
	backbuffer->Release();

	// Make sure there is at least one open viewport for DirectXTK to use.
	D3D11_VIEWPORT openView = CD3D11_VIEWPORT(0.0, 0.0, float(mResolution.x), float(mResolution.y));
	mOrigContext->RSSetViewports(1, &openView);

	return S_OK;
}

void Overlay::DrawRectangle(float x, float y, float w, float h, float r, float g, float b, float opacity)
{
	DirectX::XMVECTORF32 colour = {r, g, b, opacity};

	mOrigContext->OMSetBlendState(mStates->AlphaBlend(), nullptr, 0xFFFFFFFF);
	mOrigContext->OMSetDepthStencilState(mStates->DepthNone(), 0);
	mOrigContext->RSSetState(mStates->CullNone());

	// Use pixel coordinates to match SpriteBatch:
	Matrix proj = Matrix::CreateScale(2.0f / mResolution.x, -2.0f / mResolution.y, 1)
		* Matrix::CreateTranslation(-1, 1, 0);
	mEffect->SetProjection(proj);

	// This call will change VS + PS + constant buffer state:
	mEffect->Apply(mOrigContext);

	mOrigContext->IASetInputLayout(mInputLayout.Get());

	mPrimitiveBatch->Begin();

	// DirectXTK is using 0,1,2 0,2,3, so layout the vectors clockwise:
	DirectX::VertexPositionColor v1(Vector3(x  , y  , 0), colour);
	DirectX::VertexPositionColor v2(Vector3(x+w, y  , 0), colour);
	DirectX::VertexPositionColor v3(Vector3(x+w, y+h, 0), colour);
	DirectX::VertexPositionColor v4(Vector3(x  , y+h, 0), colour);

	mPrimitiveBatch->DrawQuad(v1, v2, v3, v4);

	mPrimitiveBatch->End();
}

void Overlay::DrawOutlinedString(DirectX::SpriteFont *font, wchar_t const *text, DirectX::XMFLOAT2 const &position, DirectX::FXMVECTOR color)
{
	font->DrawString(mSpriteBatch.get(), text, position + Vector2(-1, 0), DirectX::Colors::Black);
	font->DrawString(mSpriteBatch.get(), text, position + Vector2( 1, 0), DirectX::Colors::Black);
	font->DrawString(mSpriteBatch.get(), text, position + Vector2( 0,-1), DirectX::Colors::Black);
	font->DrawString(mSpriteBatch.get(), text, position + Vector2( 0, 1), DirectX::Colors::Black);
	font->DrawString(mSpriteBatch.get(), text, position, color);
}

// -----------------------------------------------------------------------------

// The active shader will show where we are in each list. / 0 / 0 will mean that we are not 
// actively searching. 

static void AppendShaderText(wchar_t *fullLine, wchar_t *type, int pos, size_t size)
{
	if (size == 0)
		return;

	// The position is zero based, so we'll make it +1 for the humans.
	if (++pos == 0)
		size = 0;

	// Format: "VS:1/15"
	wchar_t append[maxstring];
	swprintf_s(append, maxstring, L"%ls:%d/%Iu ", type, pos, size);

	wcscat_s(fullLine, maxstring, append);
}


// We also want to show the count of active vertex, pixel, compute, geometry, domain, hull
// shaders, that are active in the frame.  Any that have a zero count will be stripped, to
// keep it from being too busy looking.

static void CreateShaderCountString(wchar_t *counts)
{
	const wchar_t *marking_mode;

	wcscpy_s(counts, maxstring, L"");
	// The order here more or less follows how important these are for
	// shaderhacking. VS and PS are the absolute most important, CS is
	// pretty important, GS and DS show up from time to time and HS is not
	// important at all since we have never needed to fix one.
	AppendShaderText(counts, L"VS", G->mSelectedVertexShaderPos, G->mVisitedVertexShaders.size());
	AppendShaderText(counts, L"PS", G->mSelectedPixelShaderPos, G->mVisitedPixelShaders.size());
	AppendShaderText(counts, L"CS", G->mSelectedComputeShaderPos, G->mVisitedComputeShaders.size());
	AppendShaderText(counts, L"GS", G->mSelectedGeometryShaderPos, G->mVisitedGeometryShaders.size());
	AppendShaderText(counts, L"DS", G->mSelectedDomainShaderPos, G->mVisitedDomainShaders.size());
	AppendShaderText(counts, L"HS", G->mSelectedHullShaderPos, G->mVisitedHullShaders.size());
	if (G->mSelectedVertexBuffer != -1)
		AppendShaderText(counts, L"VB", G->mSelectedVertexBufferPos, G->mVisitedVertexBuffers.size());
	if (G->mSelectedIndexBuffer != -1)
		AppendShaderText(counts, L"IB", G->mSelectedIndexBufferPos, G->mVisitedIndexBuffers.size());
	if (G->mSelectedRenderTarget != (ID3D11Resource *)-1)
		AppendShaderText(counts, L"RT", G->mSelectedRenderTargetPos, G->mVisitedRenderTargets.size());

	marking_mode = lookup_enum_name(MarkingModeNames, G->marking_mode);
	if (marking_mode)
		wcscat_s(counts, maxstring, marking_mode);
}


// Need to convert from the current selection, mSelectedVertexShader as hash, and
// find the OriginalShaderInfo that matches.  This is a linear search instead of a
// hash lookup, because we don't have the ID3D11DeviceChild*.

static bool FindInfoText(wchar_t *info, UINT64 selectedShader)
{
	for each (pair<ID3D11DeviceChild *, OriginalShaderInfo> loaded in G->mReloadedShaders)
	{
		if ((loaded.second.hash == selectedShader) && !loaded.second.infoText.empty())
		{
			// We now use wcsncat_s instead of wcscat_s here,
			// because the later will terminate us if the resulting
			// string would overflow the destination buffer (or
			// fail with EINVAL if we change the parameter
			// validation so it doesn't terminate us). wcsncat_s
			// has a _TRUNCATE option that tells it to fill up as
			// much of the buffer as possible without overflowing
			// and will still NULL terminate the resulting string,
			// which will work fine for this case since that will
			// be more than we can fit on the screen anyway.
			// wcsncat would also work, but its count field is
			// silly (maxstring-strlen(info)-1) and VS complains.
			//
			// Skip past first two characters, which will always be //
			wcsncat_s(info, maxstring, loaded.second.infoText.c_str() + 2, _TRUNCATE);
			return true;
		}
	}
	return false;
}


// This is for a line of text as info about the currently selected shader.
// The line is pulled out of the header of the HLSL text file, and can be
// anything. Since there can be multiple shaders selected, VS and PS and HS for
// example, we'll show one line for each, but only those that are present
// in ShaderFixes and have something other than a blank line at the top.

void Overlay::DrawShaderInfoLine(char *type, UINT64 selectedShader, float *y, bool shader)
{
	wchar_t osdString[maxstring];
	Vector2 strSize;
	Vector2 textPosition;
	float x = 0;

	if (shader) {
		if (selectedShader == -1)
			return;

		if (G->verbose_overlay)
			swprintf_s(osdString, maxstring, L"%S %016llx:", type, selectedShader);
		else
			swprintf_s(osdString, maxstring, L"%S:", type);

		if (!FindInfoText(osdString, selectedShader) && !G->verbose_overlay)
			return;
	} else {
		if (selectedShader == 0xffffffff || !G->verbose_overlay)
			return;

		swprintf_s(osdString, maxstring, L"%S %08llx", type, selectedShader);
	}

	strSize = mFont->MeasureString(osdString);

	if (!G->verbose_overlay)
		x = max(float(mResolution.x - strSize.x) / 2, 0);

	textPosition = Vector2(x, *y);
	*y += strSize.y;
	DrawOutlinedString(mFont.get(), osdString, textPosition, DirectX::Colors::LimeGreen);
}

void Overlay::DrawShaderInfoLines(float *y)
{
	// Order is the same as the pipeline... Not quite the same as the count
	// summary line, which is sorted by "the order in which we added them"
	// (which to be fair, is pretty much their order of importance for our
	// purposes). Since these only show up while hunting, it is better to
	// have them reflect the actual order that they are run in. The summary
	// line can stay in order of importance since it is always shown.
	DrawShaderInfoLine("VB", G->mSelectedVertexBuffer, y, false);
	DrawShaderInfoLine("IB", G->mSelectedIndexBuffer, y, false);
	DrawShaderInfoLine("VS", G->mSelectedVertexShader, y, true);
	DrawShaderInfoLine("HS", G->mSelectedHullShader, y, true);
	DrawShaderInfoLine("DS", G->mSelectedDomainShader, y, true);
	DrawShaderInfoLine("GS", G->mSelectedGeometryShader, y, true);
	DrawShaderInfoLine("PS", G->mSelectedPixelShader, y, true);
	DrawShaderInfoLine("CS", G->mSelectedComputeShader, y, true);
	// FIXME? This one is stored as a handle, not a hash:
	if (G->mSelectedRenderTarget != (ID3D11Resource *)-1)
		DrawShaderInfoLine("RT", GetOrigResourceHash(G->mSelectedRenderTarget), y, false);
}

void Overlay::DrawNotices(float *y)
{
	std::vector<OverlayNotice>::iterator notice;
	DWORD time = GetTickCount();
	Vector2 textPosition;
	Vector2 strSize;
	int level, displayed = 0;

	EnterCriticalSectionPretty(&notices.lock);

	has_notice = false;
	for (level = 0; level < NUM_LOG_LEVELS; level++) {
		if (log_levels[level].hide_in_release && G->hunting == HUNTING_MODE_DISABLED)
			continue;

		for (notice = notices.notices[level].begin(); notice != notices.notices[level].end() && displayed < MAX_SIMULTANEOUS_NOTICES; ) {
			if (!notice->timestamp) {
				// Set the timestamp on the first present call
				// that we display the message after it was
				// issued. Means messages won't be missed if
				// they exceed the maximum we can display
				// simultaneously, and helps combat messages
				// disappearing too quickly if there have been
				// no present calls for a while after they were
				// issued.
				notice->timestamp = time;
			} else if ((time - notice->timestamp) > log_levels[level].duration) {
				notice = notices.notices[level].erase(notice);
				continue;
			}

			strSize = (this->*log_levels[level].font)->MeasureString(notice->message.c_str());

			DrawRectangle(0, *y, strSize.x + 3, strSize.y, 0, 0, 0, 0.75);

			(this->*log_levels[level].font)->DrawString(mSpriteBatch.get(), notice->message.c_str(), Vector2(0, *y), log_levels[level].colour);
			*y += strSize.y + 5;

			has_notice = true;
			notice++;
			displayed++;
		}
	}

	LeaveCriticalSection(&notices.lock);
}

void Overlay::DrawProfiling(float *y)
{
	Vector2 strSize;

	Profiling::update_txt();

	strSize = mFontProfiling->MeasureString(Profiling::text.c_str());
	DrawRectangle(0, *y, strSize.x + 3, strSize.y, 0, 0, 0, 0.75);

	mFontProfiling->DrawString(mSpriteBatch.get(), Profiling::text.c_str(), Vector2(0, *y), DirectX::Colors::Goldenrod);
}

// Create a string for display on the bottom edge of the screen, that contains the current
// stereo info of separation and convergence. 
// Desired format: "Sep:85  Conv:4.5"

static void CreateStereoInfoString(StereoHandle stereoHandle, wchar_t *info)
{
	// Rather than draw graphic bars, this will just be numeric.  Because
	// convergence is essentially an arbitrary number.

	float separation, convergence;
	NvU8 stereo = !!stereoHandle;
	if (stereo)
	{
		NvAPIOverride();
		Profiling::NvAPI_Stereo_IsEnabled(&stereo);
		if (stereo)
		{
			Profiling::NvAPI_Stereo_IsActivated(stereoHandle, &stereo);
			if (stereo)
			{
				Profiling::NvAPI_Stereo_GetSeparation(stereoHandle, &separation);
				Profiling::NvAPI_Stereo_GetConvergence(stereoHandle, &convergence);
			}
		}
	}

	if (stereo)
		swprintf_s(info, maxstring, L"Sep:%.0f  Conv:%.2f", separation, convergence);
	else
		swprintf_s(info, maxstring, L"Stereo disabled");
}

// ===========================================================================
// MENU FLOTANTE DE TrueTriple  (port DR1 del de DiRT Rally 2.0)
//
// Por que aca y no una ventana propia: 3Dmigoto ya trae el motor de overlay
// (SpriteFont + SpriteBatch) y, sobre todo, ya resuelve lo unico verdaderamente
// dificil — SaveState()/RestoreState() alrededor del dibujo. Esa es la regla 1
// del proyecto ("el estado del pipeline es global y sobrevive al draw"), la que
// nos mordio cinco veces. Reusarlo es gratis y ya esta probado en este juego.
//
// De donde sale lo que muestra:
//   - IniParams: x8 = huella del auto, y8 = "mostrar el menu" (lo publica
//     [Present] = triple activo Y menu no oculto).
//   - command_list_globals: las variables del ini por nombre. El mod guarda ahi
//     la postura ($seat_fwd/$seat_up/$seat_right/$seat_pitch) y — cuando exista
//     — el preset activo, asi que el menu no calcula nada por su cuenta:
//     muestra exactamente lo que el sistema esta usando.
//
// ---------------------------------------------------------------------------
// COMO SE DEGRADA CON UN TEMPLATE MAS VIEJO QUE ESTE DLL
//
// (Hasta el 2026-08-21 este bloque decia que en DR1 "todavia no existian" la
// fila 8, los presets ni el reconocimiento de auto. Era de la epoca del
// trasplante y quedo viejo: todo eso existe desde la plantilla v4, y desde la
// v6 ademas hay nombres, vinculo y panel. Lo que sigue vigente es el CRITERIO:
// nada de esto se inventa desde el C++; si el ini no lo publica, no se dibuja.)
//
// 1) Sin la fila 8 de IniParams (y8) el cartel no se dibuja: ni crash, ni
//    cartel fantasma. El flag lo publica [Present] desde $menu_visible.
// 2) Sin los globals de presets ($pre_actual / $pre_car_N / $pre_num_N), el
//    cartel DETECTA que el subsistema no existe (tt_sistema_presets) y:
//      - %PRESET% y %GUARDADAS% se resuelven a "-" (neutro, no vacio);
//      - PRESET / NOPRESET / AUTO / NOAUTO dan false y esas filas desaparecen
//        en vez de afirmar algo falso;
//      - SISPRESETS / NOSISPRESETS (extension DR1 al contrato) dejan que el
//        archivo de textos ponga o saque el bloque entero de teclas.
// 3) Sin $auto_efectivo (v6) no hay panel de asiento; sin $auto_vinculo, las
//    filas AUTO/NOAUTO no se dibujan (ver tt_vinculo: un estado desconocido no
//    se adivina). Sin TrueTriple-DR1.autos.txt el panel dice "CAR 07" a secas.
//
// Desde la v6 el cartel y el panel se dibujan en el monitor IZQUIERDO
// ($ma_fx, default 1/6), apilados: en el del medio esta el camino y esto se
// usa manejando. Decision heredada de DR2; se ajusta desde el ini.
// ===========================================================================
// TIENEN QUE COINCIDIR con el generador de los bloques de presets del ini
// (en DR2, tools/gen_presets_asiento.py: N_SLOTS y N_PRE). Si el generador crece
// y esto no, el menu deja de ver los ultimos slots — y como el resto sigue
// funcionando, el sintoma seria "un preset que guarde no aparece en la lista",
// que es de los que cuesta atribuir.
// TIENE que decir lo MISMO que N_SLOTS/N_PRE de tools/gen_presets_asiento.py
// (lo exige verificar_ini.py). No se sube de un solo lado.
#define TT_SLOTS   300
#define TT_PRESETS 9

static float tt_global(const wchar_t *nombre, float por_defecto)
{
	CommandListVariables::iterator i = command_list_globals.find(nombre);
	if (i == command_list_globals.end())
		return por_defecto;
	return i->second.fval;
}

static bool tt_global_existe(const wchar_t *nombre)
{
	return command_list_globals.find(nombre) != command_list_globals.end();
}

// ¿Esta el subsistema de presets en el ini de esta sesion? Se pregunta por los
// dos nombres porque el generador podria emitir uno sin el otro mientras se
// desarrolla, y basta cualquiera de los dos para que la seccion tenga sentido.
// Se consulta una vez por frame de dibujo, no por fila.
static bool tt_sistema_presets(void)
{
	return tt_global_existe(L"$pre_actual") || tt_global_existe(L"$pre_car_0");
}

// LA IDENTIDAD QUE HAY QUE MOSTRAR ES LA EFECTIVA, NO LA DECLARADA (port de
// DR2 v1.86). $auto_efectivo = la entrada que matcheo en la tabla
// huella<->auto, o $auto_elegido si no hubo match. Es la clave con la que se
// guardan y buscan los presets, asi que es la que dice la pantalla. Todo el
// que pregunte por el auto pasa por aca: si alguien leyera $auto_elegido por
// su cuenta, el panel mostraria la entrada DECLARADA mientras los presets
// serian los de la EFECTIVA — dos numeros para la misma cosa, sin un error a
// la vista.
static float tt_auto_efectivo(float por_defecto)
{
	CommandListVariables::iterator i = command_list_globals.find(L"$auto_efectivo");
	if (i != command_list_globals.end() && i->second.fval > 0.5f)
		return i->second.fval;
	return tt_global(L"$auto_elegido", por_defecto);
}

// ESTADO DE VINCULO DEL HABITACULO (port de DR2 v1.86). Lo publica el
// template: 0 = sin huella estable | 1 = reconocido | 2 = huella estable que
// no esta en la tabla. Devuelve false si la variable no existe (template
// viejo) o trae un valor desconocido: un estado desconocido NO SE ADIVINA.
// En los estados 0 y 2 guardar/borrar por auto esta BLOQUEADO en el ini, asi
// que el panel es lo UNICO que explica por que Ctrl+1 "no hace nada".
#define TT_VINCULO_VAR L"$auto_vinculo"

// Tiene que decir lo MISMO que [KeyAutoVincular] del template --curador. En
// el producto esa seccion no existe, pero el cartel tampoco se ve: sale solo
// en el estado 2, y con la tabla horneada no se llega ahi con un auto del
// plantel.
#define TT_TECLA_VINCULAR L"Ctrl+F2"

static bool tt_vinculo(int *estado)
{
	CommandListVariables::iterator i = command_list_globals.find(TT_VINCULO_VAR);
	if (i == command_list_globals.end())
		return false;
	float v = i->second.fval;
	if (v < -0.5f || v > 2.5f)
		return false;
	*estado = (int)(v + 0.5f);
	return true;
}

// QUE POSTURAS TIENE GUARDADAS ESTE AUTO — CON CACHE (port de DR2 v1.86).
// El barrido pregunta $pre_car_i y $pre_num_i por slot y cada tt_global es
// una busqueda por NOMBRE en un mapa. Con TT_SLOTS = 300 y dos paneles
// abiertos serian ~1200 busquedas por Present (regla dura 2: nada caro por
// frame). La cache se recalcula solo si cambio el auto efectivo, cambio
// $pre_actual, o pasaron TT_CACHE_FRAMES (la RED: guardar sobre el numero ya
// puesto no mueve las otras dos senales).
// ⚠️ Los DOS paneles tienen que pasar exactamente el MISMO par (auto, preset)
// o se invalidan la cache entre si en cada frame.
#define TT_CACHE_FRAMES 30

struct TtGuardadas {
	bool tiene[TT_PRESETS + 1];
	int  cuantos;
};

static TtGuardadas tt_gcache;
static float    tt_gcache_auto = 0.0f;
static float    tt_gcache_pre = 0.0f;
static unsigned tt_gcache_frame = 0;
static bool     tt_gcache_lista = false;

static const TtGuardadas &tt_guardadas_de(float auto_ef, float pre_actual)
{
	if (tt_gcache_lista
			&& auto_ef == tt_gcache_auto
			&& pre_actual == tt_gcache_pre
			&& (unsigned)(G->frame_no - tt_gcache_frame) < TT_CACHE_FRAMES)
		return tt_gcache;

	for (int p = 0; p <= TT_PRESETS; p++)
		tt_gcache.tiene[p] = false;
	tt_gcache.cuantos = 0;

	for (int i = 0; i < TT_SLOTS; i++) {
		wchar_t nombre[32];
		swprintf_s(nombre, L"$pre_car_%d", i);
		// Por DIFERENCIA con ±0.5: con la identidad efectiva (enteros chicos,
		// 1..46) es la igualdad entera robusta. Se conserva el criterio de la
		// huella vieja (>2^23, donde la ventana absoluta colapsaba) para no
		// tener dos.
		float d = tt_global(nombre, 0.0f) - auto_ef;
		if (d < -0.5f || d > 0.5f)
			continue;
		swprintf_s(nombre, L"$pre_num_%d", i);
		int num = (int)tt_global(nombre, 0.0f);
		if (num >= 1 && num <= TT_PRESETS && !tt_gcache.tiene[num]) {
			tt_gcache.tiene[num] = true;
			tt_gcache.cuantos++;
		}
	}

	tt_gcache_auto = auto_ef;
	tt_gcache_pre = pre_actual;
	tt_gcache_frame = G->frame_no;
	tt_gcache_lista = true;
	return tt_gcache;
}

// ---------------------------------------------------------------------------
// LOS TEXTOS DEL CARTEL SALEN DE UN ARCHIVO, NO DEL C++
//
// Contrato completo en docs/CONTRATO-MENU-F3.md (CERRADO; hoy vive en el repo
// de referencia y conviene copiarlo a docs/ de este). Resumen de por que:
// el C++ no puede saber ni el idioma del usuario ni sus binds — el que arma el
// d3dx.ini (la app, o el lanzador PS1) sabe los dos, asi que escribe
// TrueTriple.menu.txt al lado del ini, ya traducido y con los %KEY_*% resueltos.
// Aca solo se parsea y se resuelven los marcadores de ESTADO VIVO (%FWD%, etc).
//
// Formato de cada fila:  tipo|cond|color|centrado|aire|escala|texto
// Lineas vacias y las que empiezan con # se ignoran. UTF-8 sin BOM.
//
// Regla que manda sobre todas: NUNCA abortar el dibujado por un archivo mal
// formado. Fila mala -> se descarta y se sigue. Archivo ausente o ilegible ->
// cartel de respaldo embebido en ingles. Un cartel que no aparece se lee como
// "el mod no anda", que es el peor diagnostico posible.
// ---------------------------------------------------------------------------
#define TT_MAX_FILAS 32
#define TT_MAX_TXT   192
#define TT_MENU_ARCHIVO L"TrueTriple.menu.txt"
#define TT_MAX_BYTES   (256 * 1024)

enum TtCol { TT_ORO, TT_VERDE, TT_NARANJA, TT_BLANCO, TT_CIELO, TT_PLATA, TT_CREMA };
// Las dos ultimas son la extension DR1: no preguntan por el ESTADO del sistema
// de presets sino por su EXISTENCIA. Ver el comentario grande de arriba.
enum TtCond {
	TT_SIEMPRE, TT_CON_PRESET, TT_SIN_PRESET, TT_CON_AUTO, TT_SIN_AUTO,
	TT_HAY_SISTEMA, TT_NO_HAY_SISTEMA
};

struct TtFilaDef {
	std::wstring texto;   // plantilla, con los marcadores todavia sin resolver
	TtCol  color;
	TtCond cond;
	bool   centrado;
	bool   separador;
	float  antes;         // aire por encima, en renglones
	float  escala;        // 1.0 = cuerpo normal de la fuente
};

static std::vector<TtFilaDef> tt_filas;
static bool     tt_filas_listas = false;
static bool     tt_desde_archivo = false;
static FILETIME tt_ft_visto = { 0, 0 };
static ULONGLONG tt_tam_visto = 0;
static DWORD    tt_ultimo_chequeo = 0;

// Parser de float propio a proposito: wcstod depende del locale del CRT y un
// juego puede dejarlo en uno donde el separador decimal es la coma. Un "0.70"
// leido como 0 no rompe nada visible, solo aplasta el aire del cartel, o sea
// que fallaria en silencio. Acepta las dos formas.
static float tt_afloat(const wchar_t *s)
{
	while (*s == L' ' || *s == L'\t')
		s++;
	float signo = 1.0f;
	if (*s == L'-') { signo = -1.0f; s++; }
	else if (*s == L'+') { s++; }
	float ent = 0.0f;
	while (*s >= L'0' && *s <= L'9')
		ent = ent * 10.0f + (float)(*s++ - L'0');
	if (*s == L'.' || *s == L',') {
		s++;
		float div = 1.0f;
		while (*s >= L'0' && *s <= L'9') {
			div *= 10.0f;
			ent += (float)(*s++ - L'0') / div;
		}
	}
	return signo * ent;
}

static void tt_recortar(std::wstring *s)
{
	size_t a = s->find_first_not_of(L" \t\r\n");
	if (a == std::wstring::npos) { s->clear(); return; }
	size_t b = s->find_last_not_of(L" \t\r\n");
	*s = s->substr(a, b - a + 1);
}

static TtCol tt_color_de(const std::wstring &s)
{
	if (!_wcsicmp(s.c_str(), L"ORO"))     return TT_ORO;
	if (!_wcsicmp(s.c_str(), L"VERDE"))   return TT_VERDE;
	if (!_wcsicmp(s.c_str(), L"NARANJA")) return TT_NARANJA;
	if (!_wcsicmp(s.c_str(), L"CIELO"))   return TT_CIELO;
	if (!_wcsicmp(s.c_str(), L"PLATA"))   return TT_PLATA;
	if (!_wcsicmp(s.c_str(), L"CREMA"))   return TT_CREMA;
	return TT_BLANCO;  // desconocido: blanco, no descartar la fila
}

static bool tt_cond_de(const std::wstring &s, TtCond *out)
{
	if (s.empty())                           { *out = TT_SIEMPRE;     return true; }
	if (!_wcsicmp(s.c_str(), L"PRESET"))     { *out = TT_CON_PRESET;  return true; }
	if (!_wcsicmp(s.c_str(), L"NOPRESET"))   { *out = TT_SIN_PRESET;  return true; }
	if (!_wcsicmp(s.c_str(), L"AUTO"))       { *out = TT_CON_AUTO;    return true; }
	if (!_wcsicmp(s.c_str(), L"NOAUTO"))     { *out = TT_SIN_AUTO;    return true; }
	// Extension DR1. Se escriben bien distintas de PRESET/NOPRESET a proposito:
	// "NOPRESETS" (una S de diferencia con NOPRESET) seria imposible de depurar
	// en un archivo generado.
	if (!_wcsicmp(s.c_str(), L"SISPRESETS"))   { *out = TT_HAY_SISTEMA;    return true; }
	if (!_wcsicmp(s.c_str(), L"NOSISPRESETS")) { *out = TT_NO_HAY_SISTEMA; return true; }
	return false;  // condicion desconocida: fila descartada
}

// El mapa TtCol -> color real, compartido por el cartel y el panel de asiento.
static DirectX::XMVECTOR tt_rgb(TtCol c)
{
	switch (c) {
	case TT_ORO:     return DirectX::Colors::Gold;
	case TT_VERDE:   return DirectX::Colors::LimeGreen;
	case TT_NARANJA: return DirectX::Colors::Orange;
	case TT_CIELO:   return DirectX::Colors::LightSkyBlue;
	case TT_PLATA:   return DirectX::Colors::Silver;
	case TT_CREMA:   return DirectX::Colors::Khaki;
	default:         return DirectX::Colors::White;
	}
}

// LOS TEXTOS CON NOMBRE DEL PANEL DE ASIENTO (filas P del menu.txt, port de
// DR2 v1.96). El C++ no sabe el idioma: los rotulos vienen traducidos en el
// archivo y aca solo se buscan por clave, con respaldo EN INGLES (la
// convencion de DR1: el respaldo embebido entero es ingles).
static std::map<std::wstring, std::wstring> tt_textos;

static const wchar_t *tt_txt(const wchar_t *clave, const wchar_t *respaldo)
{
	std::map<std::wstring, std::wstring>::const_iterator i = tt_textos.find(clave);
	return (i != tt_textos.end() && !i->second.empty()) ? i->second.c_str() : respaldo;
}

// Una linea -> una fila. Devuelve false si hay que descartarla (comentario,
// vacia, mal formada). Los primeros seis campos se recortan; el texto NO, que
// sus espacios son la alineacion en columnas del cartel.
static bool tt_parsear_linea(const std::wstring &linea_cruda, TtFilaDef *out)
{
	std::wstring linea = linea_cruda;
	while (!linea.empty() && (linea[linea.size() - 1] == L'\r' || linea[linea.size() - 1] == L'\n'))
		linea.erase(linea.size() - 1);

	std::wstring probe = linea;
	tt_recortar(&probe);
	if (probe.empty() || probe[0] == L'#')
		return false;

	// LAS FILAS P NO SE DIBUJAN: son textos con nombre que usa el panel de
	// asiento. Tienen TRES campos, no siete, asi que se atienden antes del
	// parseo de abajo — que exige seis separadores y las descartaria. El texto
	// se toma CRUDO hasta el fin de linea: puede llevar espacios internos que
	// son parte del diseno, y recortarlo los comeria.
	if (linea.size() > 2 && (linea[0] == L'P' || linea[0] == L'p') && linea[1] == L'|') {
		size_t bar2 = linea.find(L'|', 2);
		if (bar2 != std::wstring::npos) {
			std::wstring clave = linea.substr(2, bar2 - 2);
			tt_recortar(&clave);
			if (!clave.empty())
				tt_textos[clave] = linea.substr(bar2 + 1);
		}
		return false;   // no es una fila dibujable
	}

	std::wstring campo[7];
	int c = 0;
	size_t ini = 0;
	while (c < 6) {
		size_t bar = linea.find(L'|', ini);
		if (bar == std::wstring::npos)
			return false;   // faltan campos
		campo[c++] = linea.substr(ini, bar - ini);
		ini = bar + 1;
	}
	campo[6] = linea.substr(ini);   // el resto entero es el texto

	for (int i = 0; i < 6; i++)
		tt_recortar(&campo[i]);

	if (!_wcsicmp(campo[0].c_str(), L"S"))
		out->separador = true;
	else if (!_wcsicmp(campo[0].c_str(), L"T"))
		out->separador = false;
	else
		return false;

	if (!tt_cond_de(campo[1], &out->cond))
		return false;

	out->color    = tt_color_de(campo[2]);
	out->centrado = (campo[3] == L"1");
	out->antes    = tt_afloat(campo[4].c_str());
	out->escala   = tt_afloat(campo[5].c_str());
	if (out->escala <= 0.01f)
		out->escala = 1.0f;
	out->texto    = out->separador ? std::wstring() : campo[6];

	// El limite de 192 es sobre el texto YA resuelto, pero si la plantilla sola
	// ya lo supera no hay resolucion que la salve.
	if (out->texto.size() >= TT_MAX_TXT)
		return false;

	return true;
}

static void tt_parsear_todo(const wchar_t *contenido, size_t largo)
{
	tt_filas.clear();
	size_t i = 0;
	while (i < largo && tt_filas.size() < TT_MAX_FILAS) {
		size_t fin = i;
		while (fin < largo && contenido[fin] != L'\n')
			fin++;
		TtFilaDef f;
		if (tt_parsear_linea(std::wstring(contenido + i, fin - i), &f))
			tt_filas.push_back(f);
		i = fin + 1;
	}
}

// El cartel de respaldo. Va en el MISMO formato que el archivo y pasa por el
// MISMO parser: asi no hay dos caminos de codigo que puedan divergir, y probar
// el parser prueba tambien el fallback. En ingles y con los binds de fabrica,
// porque si el archivo falta tampoco sabemos que idioma ni que teclas usa.
//
// Los binds son los de FABRICA DE DR1 (los defaults de TrueTripleLauncher.ps1:
// F5, F6, +/-, I/K, T/G, U/O, R/F, V, y J/N/P para el estabilizador), no los de
// DR2, que en DR1 chocan con controles del propio juego. Si el usuario rebindeo
// algo, el archivo generado tiene la verdad; este respaldo aparece justo cuando
// ese archivo no esta.
static const wchar_t *tt_menu_embebido =
	L"T||ORO|1|0.00|1.00|TrueTriple   DiRT Rally\n"
	L"T||BLANCO|1|0.70|0.88|TRIPLE MONITOR   -   SEAT POSITION\n"
	L"S||PLATA|0|0.85|1.00|\n"
	L"T||BLANCO|0|0.60|1.00|Forward   %FWD% m        Height       %UP% m\n"
	L"T||BLANCO|0|0.15|1.00|Lateral   %RIGHT% m        Pitch        %PITCH% degrees\n"
	L"T|PRESET|VERDE|1|0.70|0.80|POSITION %PRESET%          saved:  %GUARDADAS%\n"
	L"T|NOPRESET|CREMA|1|0.70|0.74|NOTHING SAVED YET:  GET COMFORTABLE AND PRESS CTRL + 1\n"
	L"S||PLATA|0|0.85|1.00|\n"
	L"T||CIELO|0|0.60|1.00|I / K               Move closer to or away from the wheel\n"
	L"T||CIELO|0|0.18|1.00|T / G               Raise or lower your head\n"
	L"T||CIELO|0|0.18|1.00|O / U               Move to the right or to the left\n"
	L"T||CIELO|0|0.18|1.00|R / F               Tilt your view up or down\n"
	L"T||CIELO|0|0.18|1.00|V                   Back to zero\n"
	L"T||CIELO|0|0.45|1.00|+ / -               More or less field of view\n"
	L"T||CIELO|0|0.18|1.00|F5                  Triple monitor on / off\n"
	L"T||CIELO|0|0.18|1.00|F6                  Game shadows on / off\n"
	L"T||CIELO|0|0.45|1.00|J / N / P / Y       Look stabiliser (strength / memory / cap / damping):  %SMOOTH%\n"
	L"T|SISPRESETS|ORO|0|0.45|1.00|Ctrl + 1 to 9       Save this position under that number\n"
	L"T|SISPRESETS|ORO|0|0.18|1.00|1 to 9              Go back to a saved position\n"
	L"T|SISPRESETS|ORO|0|0.18|1.00|0                   Jump to the next saved one\n"
	L"T|AUTO|PLATA|1|0.70|0.62|car recognised\n"
	L"T|NOAUTO|NARANJA|1|0.70|0.80|LOOKING FOR THE CAR:  ENTER THE COCKPIT VIEW ONCE\n";

static void tt_cargar_embebido(void)
{
	tt_parsear_todo(tt_menu_embebido, wcslen(tt_menu_embebido));
	tt_desde_archivo = false;
}

// El archivo va al lado del d3dx.ini, y el d3dx.ini lo busca 3Dmigoto en su
// propia carpeta (la del d3d11.dll), no en el cwd — que un juego puede cambiar.
// Se prueba primero ahi y despues junto al exe, que es el mismo lugar salvo en
// instalaciones raras.
static bool tt_ruta_junto_a(HMODULE mod, wchar_t *ruta, const wchar_t *nombre)
{
	if (!GetModuleFileName(mod, ruta, MAX_PATH))
		return false;
	wchar_t *barra = wcsrchr(ruta, L'\\');
	if (!barra)
		return false;
	barra[1] = 0;
	// wcsncat_s con _TRUNCATE y no wcscat_s: el segundo mata el proceso si no
	// entra, y un cartel no vale un crash.
	return wcsncat_s(ruta, MAX_PATH, nombre, _TRUNCATE) == 0;
}

// Generalizado por nombre (v6): lo usan el cartel (TrueTriple.menu.txt) y el
// plantel de autos (TrueTriple-DR1.autos.txt).
static bool tt_ubicar_archivo(const wchar_t *nombre, wchar_t *ruta, WIN32_FILE_ATTRIBUTE_DATA *fad)
{
	if (tt_ruta_junto_a(migoto_handle, ruta, nombre)
			&& GetFileAttributesEx(ruta, GetFileExInfoStandard, fad))
		return true;
	if (tt_ruta_junto_a(NULL, ruta, nombre)
			&& GetFileAttributesEx(ruta, GetFileExInfoStandard, fad))
		return true;
	return false;
}

// Lee un archivo UTF-8 (con o sin BOM) a wchars. Factor comun del cartel y el
// plantel de autos; NUNCA aborta nada: false = no se pudo, el que llama decide
// su degradacion.
static bool tt_leer_utf8(const wchar_t *ruta, std::vector<wchar_t> *wbuf)
{
	HANDLE h = CreateFile(ruta, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	DWORD tam = GetFileSize(h, NULL);
	if (tam == INVALID_FILE_SIZE || tam == 0 || tam > TT_MAX_BYTES) {
		CloseHandle(h);
		return false;
	}

	std::vector<char> buf(tam + 1);
	DWORD leido = 0;
	BOOL ok = ReadFile(h, &buf[0], tam, &leido, NULL);
	CloseHandle(h);
	if (!ok || leido == 0)
		return false;

	const char *inicio = &buf[0];
	int largo_a = (int)leido;
	if (largo_a >= 3 && (unsigned char)inicio[0] == 0xEF
			&& (unsigned char)inicio[1] == 0xBB && (unsigned char)inicio[2] == 0xBF) {
		inicio += 3;
		largo_a -= 3;
	}
	if (largo_a <= 0)
		return false;

	int largo_w = MultiByteToWideChar(CP_UTF8, 0, inicio, largo_a, NULL, 0);
	if (largo_w <= 0)
		return false;
	wbuf->resize(largo_w + 1);
	MultiByteToWideChar(CP_UTF8, 0, inicio, largo_a, &(*wbuf)[0], largo_w);
	(*wbuf)[largo_w] = 0;
	return true;
}

static bool tt_leer_archivo(const wchar_t *ruta)
{
	HANDLE h = CreateFile(ruta, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	DWORD tam = GetFileSize(h, NULL);
	if (tam == INVALID_FILE_SIZE || tam == 0 || tam > TT_MAX_BYTES) {
		CloseHandle(h);
		return false;
	}

	std::vector<char> buf(tam + 1);
	DWORD leido = 0;
	BOOL ok = ReadFile(h, &buf[0], tam, &leido, NULL);
	CloseHandle(h);
	if (!ok || leido == 0)
		return false;
	buf[leido] = 0;

	// El contrato pide UTF-8 SIN BOM, pero si aparece uno se lo saltea: un
	// editor de texto lo agrega solo y el sintoma seria la primera fila
	// desaparecida, que no apunta a la causa.
	const char *inicio = &buf[0];
	int largo_a = (int)leido;
	if (largo_a >= 3 && (unsigned char)inicio[0] == 0xEF
			&& (unsigned char)inicio[1] == 0xBB && (unsigned char)inicio[2] == 0xBF) {
		inicio += 3;
		largo_a -= 3;
	}
	if (largo_a <= 0)
		return false;

	int largo_w = MultiByteToWideChar(CP_UTF8, 0, inicio, largo_a, NULL, 0);
	if (largo_w <= 0)
		return false;
	std::vector<wchar_t> wbuf(largo_w + 1);
	MultiByteToWideChar(CP_UTF8, 0, inicio, largo_a, &wbuf[0], largo_w);
	wbuf[largo_w] = 0;

	tt_parsear_todo(&wbuf[0], (size_t)largo_w);
	if (tt_filas.empty())
		return false;   // archivo ilegible en la practica: mejor el embebido

	tt_desde_archivo = true;
	return true;
}

// Se carga UNA vez, no por frame (esto se dibuja en cada Present con el menu
// abierto). La recarga en caliente se hace mirando fecha y tamano del archivo
// como mucho una vez por segundo: el lanzador y la app reescriben el .menu.txt
// junto con el d3dx.ini, y 3Dmigoto no expone un gancho de "config recargada"
// al que podamos colgarnos desde aca sin tocar IniHandler.cpp.
static void tt_asegurar_menu(void)
{
	DWORD ahora = GetTickCount();
	if (tt_filas_listas && (ahora - tt_ultimo_chequeo) < 1000)
		return;
	tt_ultimo_chequeo = ahora;

	wchar_t ruta[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA fad;

	if (!tt_ubicar_archivo(TT_MENU_ARCHIVO, ruta, &fad)) {
		// Si el archivo desaparecio y antes lo teniamos, hay que volver al
		// embebido; si ya estabamos en el embebido no se rehace nada.
		if (!tt_filas_listas || tt_desde_archivo) {
			tt_cargar_embebido();
			tt_ft_visto.dwLowDateTime = tt_ft_visto.dwHighDateTime = 0;
			tt_tam_visto = 0;
			tt_filas_listas = true;
			LogInfo("TrueTriple DR1 [cartel F3]: %S no encontrado, uso el respaldo embebido en ingles\n", TT_MENU_ARCHIVO);
		}
		return;
	}

	ULONGLONG tam = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
	if (tt_filas_listas && tt_desde_archivo
			&& tam == tt_tam_visto
			&& fad.ftLastWriteTime.dwLowDateTime == tt_ft_visto.dwLowDateTime
			&& fad.ftLastWriteTime.dwHighDateTime == tt_ft_visto.dwHighDateTime)
		return;

	if (tt_leer_archivo(ruta)) {
		tt_ft_visto = fad.ftLastWriteTime;
		tt_tam_visto = tam;
		LogInfo("TrueTriple DR1 [cartel F3]: leido de %S (%Iu filas)\n", ruta, tt_filas.size());
	} else {
		tt_cargar_embebido();
		tt_ft_visto = fad.ftLastWriteTime;
		tt_tam_visto = tam;
		LogInfo("TrueTriple DR1 [cartel F3]: %S ilegible o vacio, uso el respaldo embebido\n", ruta);
	}
	tt_filas_listas = true;
}

// ---------------------------------------------------------------------------
// LOS NOMBRES DE LOS AUTOS TAMBIEN SALEN DE UN ARCHIVO (port de DR2 v1.86)
//
// Mismo criterio que el cartel: el C++ no sabe que auto es el 7 para este
// usuario. La app (o el lanzador) conoce la lista y la escribe al lado del
// d3dx.ini.
//   Archivo:  TrueTriple-DR1.autos.txt   (junto al d3d11.dll, o junto al exe)
//   Formato:  una linea por auto, "<numero>|<nombre>"; UTF-8; vacias y # se
//   ignoran. Linea mal formada -> se descarta y se sigue. Archivo ausente ->
//   el panel muestra "CAR 07" a secas: el numero es el dato con autoridad.
// ---------------------------------------------------------------------------
#define TT_AUTOS_ARCHIVO   L"TrueTriple-DR1.autos.txt"
// TECHO, no espejo de N_AUTOS: alcanza con que le sobre (DiRT Rally tiene 46 y
// la lista esta cerrada). Quedarse corto seria una truncacion silenciosa; si
// pasa, el LogInfo de abajo lo dice.
#define TT_AUTOS_MAX       128
#define TT_AUTO_NOMBRE_MAX 48    // mas largo ensancharia el panel sin fin

static std::wstring tt_autos[TT_AUTOS_MAX + 1];   // indexado por numero, 0 sin usar
// Tercera columna del .txt (2026-09-05): el id de modelo del motor (las 3 letras
// de cars\models\<id>), en minusculas. Es lo que el detector de auto de
// CommandList.cpp cruza contra lo que lee del neRacing::VehicleSystem.
static std::string  tt_autos_id[TT_AUTOS_MAX + 1];
static bool      tt_autos_listos = false;
static bool      tt_autos_desde_archivo = false;
static FILETIME  tt_autos_ft = { 0, 0 };
static ULONGLONG tt_autos_tam = 0;
static DWORD     tt_autos_chequeo = 0;
static int       tt_autos_fuera = 0;

static void tt_limpiar_autos(void)
{
	for (int i = 0; i <= TT_AUTOS_MAX; i++)
		tt_autos[i].clear();
	for (int i = 0; i <= TT_AUTOS_MAX; i++)
		tt_autos_id[i].clear();
}

// Devuelve cuantas lineas validas entraron (para el log: "0" y "no existe"
// son dos problemas distintos y en pantalla se ven igual).
static int tt_parsear_autos(const wchar_t *contenido, size_t largo)
{
	tt_limpiar_autos();
	tt_autos_fuera = 0;
	int cargados = 0;
	size_t i = 0;
	while (i < largo) {
		size_t fin = i;
		while (fin < largo && contenido[fin] != L'\n')
			fin++;
		std::wstring linea(contenido + i, fin - i);
		i = fin + 1;

		tt_recortar(&linea);
		if (linea.empty() || linea[0] == L'#')
			continue;

		size_t bar = linea.find(L'|');
		if (bar == std::wstring::npos)
			continue;
		std::wstring snum = linea.substr(0, bar);
		std::wstring nom  = linea.substr(bar + 1);
		// "N|Nombre|id": el id es opcional (un .txt viejo no lo trae) y va en
		// minusculas para que la busqueda no dependa de como se escribio.
		std::wstring wid;
		size_t bar2 = nom.find(L'|');
		if (bar2 != std::wstring::npos) {
			wid = nom.substr(bar2 + 1);
			nom = nom.substr(0, bar2);
		}
		tt_recortar(&snum);
		tt_recortar(&nom);
		tt_recortar(&wid);
		if (snum.empty() || nom.empty())
			continue;

		// Parseo a mano y no con wcstol: el CRT depende del locale y aca
		// queremos que "07" sea 7 en cualquier PC.
		int num = 0;
		bool digitos = true;
		for (size_t k = 0; k < snum.size(); k++) {
			if (snum[k] < L'0' || snum[k] > L'9') { digitos = false; break; }
			num = num * 10 + (int)(snum[k] - L'0');
			if (num > 999999) { digitos = false; break; }
		}
		if (!digitos || num < 1)
			continue;
		if (num > TT_AUTOS_MAX) {
			tt_autos_fuera++;   // no se pierde en silencio: sale en el log
			continue;
		}

		if (nom.size() > TT_AUTO_NOMBRE_MAX)
			nom = nom.substr(0, TT_AUTO_NOMBRE_MAX);
		if (tt_autos[num].empty())
			cargados++;
		tt_autos[num] = nom;   // repetido: gana el ultimo
		std::string id;
		for (size_t k = 0; k < wid.size() && k < 8; k++) {
			wchar_t c = wid[k];
			if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
			if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_')
				id.push_back((char)c);
		}
		tt_autos_id[num] = id;
	}
	return cargados;
}

// Mismo ritmo de recarga en caliente que el cartel: una mirada por segundo a
// fecha y tamano, como mucho.
static void tt_asegurar_autos(void)
{
	DWORD ahora = GetTickCount();
	if (tt_autos_listos && (ahora - tt_autos_chequeo) < 1000)
		return;
	tt_autos_chequeo = ahora;

	wchar_t ruta[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA fad;

	if (!tt_ubicar_archivo(TT_AUTOS_ARCHIVO, ruta, &fad)) {
		if (!tt_autos_listos || tt_autos_desde_archivo) {
			tt_limpiar_autos();
			tt_autos_desde_archivo = false;
			tt_autos_ft.dwLowDateTime = tt_autos_ft.dwHighDateTime = 0;
			tt_autos_tam = 0;
			tt_autos_listos = true;
			LogInfo("TrueTriple DR1: %S no encontrado, el panel muestra solo el numero de auto\n",
				TT_AUTOS_ARCHIVO);
		}
		return;
	}

	ULONGLONG tam = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
	if (tt_autos_listos && tt_autos_desde_archivo
			&& tam == tt_autos_tam
			&& fad.ftLastWriteTime.dwLowDateTime == tt_autos_ft.dwLowDateTime
			&& fad.ftLastWriteTime.dwHighDateTime == tt_autos_ft.dwHighDateTime)
		return;

	std::vector<wchar_t> wbuf;
	if (tt_leer_utf8(ruta, &wbuf)) {
		int cargados = tt_parsear_autos(&wbuf[0], wbuf.size() - 1);
		LogInfo("TrueTriple DR1: nombres de auto leidos de %S (%d)\n", ruta, cargados);
		if (tt_autos_fuera)
			LogInfo("TrueTriple DR1: %d nombres IGNORADOS por numero > %d "
				"(subir TT_AUTOS_MAX en Overlay.cpp)\n",
				tt_autos_fuera, TT_AUTOS_MAX);
	} else {
		tt_limpiar_autos();
		LogInfo("TrueTriple DR1: %S ilegible o vacio, el panel muestra solo el numero\n", ruta);
	}
	tt_autos_desde_archivo = true;
	tt_autos_ft = fad.ftLastWriteTime;
	tt_autos_tam = tam;
	tt_autos_listos = true;
}

// Numero de plantel (1..N) del auto cuyo id de modelo es `id`, o 0 si no esta
// en el .txt. La usa el detector de auto de CommandList.cpp: el motor sabe el
// id (cars\models\<id>) y el plantel es el que numera los presets guardados.
int tt_autos_por_id(const char *id)
{
	if (!id || !*id)
		return 0;
	tt_asegurar_autos();
	for (int i = 1; i <= TT_AUTOS_MAX; i++)
		if (!tt_autos_id[i].empty() && tt_autos_id[i] == id)
			return i;
	return 0;
}

// Una fila del panel de asiento. Se arma con tt_fila_panel porque hay filas
// CONDICIONALES (las del estado de vinculo) y el maximo no se ve leyendo una
// sola rama: sin el tope, una fila de mas pisaba la pila en silencio.
struct TtFilaPanel {
	const wchar_t *txt;
	float esc;     // sobre el cuerpo base del cartel
	float antes;   // aire por encima, en renglones base
	TtCol color;
};

static void tt_fila_panel(TtFilaPanel *f, int *n, int cap, const wchar_t *txt,
		float esc, float antes, TtCol color)
{
	if (*n >= cap) {
		static bool dicho = false;
		if (!dicho) {
			dicho = true;
			LogInfo("TrueTriple DR1: el panel de asientos no entra en %d filas "
				"(subir el arreglo en Overlay.cpp)\n", cap);
		}
		return;
	}
	f[*n].txt   = txt;
	f[*n].esc   = esc;
	f[*n].antes = antes;
	f[*n].color = color;
	(*n)++;
}

// Estado vivo que el C++ (y solo el C++) puede resolver. Los %KEY_*% ya vienen
// sustituidos por quien genero el archivo.
struct TtEstado {
	float fwd, up, right, pitch;
	int   activo;
	const wchar_t *guardadas;
	bool  hay_auto;
	bool  hay_preset;
	// DR1: ¿existen siquiera los presets / el reconocimiento de auto en el ini
	// de esta sesion? Mientras no existan, todo lo que dependa de ellos se
	// oculta en vez de mostrar un cero que se leeria como dato real.
	bool  sis_presets;
	bool  sis_auto;
};

// Lo que se muestra cuando el subsistema NO existe. Un guion, no una cadena
// vacia: vacio deja "saved:" colgando sin nada al lado y eso se lee como bug.
#define TT_NEUTRO L"-"

// Devuelve false si el resultado no entra en TT_MAX_TXT (fila descartada). Un
// %...% que no conocemos se copia tal cual, para que un archivo mas nuevo que
// el dll se vea feo pero no rompa.
static bool tt_resolver(const std::wstring &plantilla, const TtEstado &st, wchar_t *salida)
{
	std::wstring out;
	wchar_t num[64];

	for (size_t i = 0; i < plantilla.size(); ) {
		if (plantilla[i] != L'%') {
			out += plantilla[i++];
			continue;
		}
		size_t cierre = plantilla.find(L'%', i + 1);
		if (cierre == std::wstring::npos) {
			out += plantilla[i++];
			continue;
		}
		std::wstring nombre = plantilla.substr(i + 1, cierre - i - 1);
		const wchar_t *n = nombre.c_str();

		if (!_wcsicmp(n, L"SMOOTH"))
			// Las tres perillas del estabilizador de la mirada tal como estan
			// EN ESE MOMENTO, mas el angulo que esta corrigiendo. Se muestran
			// porque el punto se busca MANEJANDO —es la unica forma honesta de
			// encontrarlo— y sin verlas no hay manera de saber en que paso del
			// ciclo quedo cada tecla. El ultimo numero es el que dice si la
			// correccion esta actuando, y cuanto.
			//
			// SIN PALABRAS, a diferencia del DR2 de referencia, que emite
			// "memoria N tope N gr" desde el C++. Todo el punto del contrato
			// del cartel (docs/CONTRATO-MENU-F3.md) es que el C++ NO tenga
			// texto: no sabe el idioma del usuario. En DR2 eso deja el respaldo
			// embebido —que es en ingles— diciendo media frase en espanol. Aca
			// los rotulos van en el archivo, que si esta traducido, y esto
			// emite solo numeros: fuerza %, memoria en segundos, tope en
			// grados, la amortiguacion vertical en % (v6), y despues de la
			// flecha la correccion aplicada, tambien en grados. El signo va
			// explicito porque una correccion es hacia arriba o hacia abajo y
			// sin el no se distingue.
			//
			// El ORDEN de los numeros lo dice el texto de la fila (menu.smooth
			// en la web y la fila del PS1), no esto: si se agrega uno aca hay
			// que tocar los tres textos a la vez.
			swprintf_s(num, L"%d%%   %d s   %.1f   %d%%   ->  %+.2f",
				(int)tt_global(L"$ph_int", 0.0f),
				(int)tt_global(L"$ph_mem", 0.0f),
				tt_global(L"$ph_tope", 0.0f),
				(int)tt_global(L"$cu_int", 0.0f),
				tt_global(L"$ph_d", 0.0f));
		else if (!_wcsicmp(n, L"FWD"))
			swprintf_s(num, L"%+.2f", st.fwd);
		else if (!_wcsicmp(n, L"UP"))
			swprintf_s(num, L"%+.2f", st.up);
		else if (!_wcsicmp(n, L"RIGHT"))
			swprintf_s(num, L"%+.2f", st.right);
		else if (!_wcsicmp(n, L"PITCH"))
			swprintf_s(num, L"%+.1f", st.pitch);
		else if (!_wcsicmp(n, L"PRESET")) {
			if (st.sis_presets && st.activo >= 1 && st.activo <= TT_PRESETS)
				swprintf_s(num, L"%d", st.activo);
			else
				wcscpy_s(num, TT_NEUTRO);   // sin sistema, o hay guardadas pero ninguna puesta
		} else if (!_wcsicmp(n, L"GUARDADAS")) {
			out += (st.sis_presets && st.guardadas[0]) ? st.guardadas : TT_NEUTRO;
			i = cierre + 1;
			continue;
		} else {
			// desconocido: se deja el marcador entero, incluidos los %
			out += plantilla.substr(i, cierre - i + 1);
			i = cierre + 1;
			continue;
		}

		out += num;
		i = cierre + 1;
	}

	if (out.size() >= TT_MAX_TXT)
		return false;
	wcscpy_s(salida, TT_MAX_TXT, out.c_str());
	return true;
}

static bool tt_cond_se_cumple(TtCond cond, const TtEstado &st)
{
	// Las cuatro condiciones de estado exigen ANTES que el subsistema exista: si
	// no existe, ni "hay preset" ni "no hay preset" son afirmaciones honestas, y
	// la fila se saca. Esa es toda la diferencia con el DR2 de referencia.
	switch (cond) {
	case TT_CON_PRESET:     return st.sis_presets && st.hay_preset;
	case TT_SIN_PRESET:     return st.sis_presets && !st.hay_preset;
	case TT_CON_AUTO:       return st.sis_auto && st.hay_auto;
	case TT_SIN_AUTO:       return st.sis_auto && !st.hay_auto;
	case TT_HAY_SISTEMA:    return st.sis_presets;
	case TT_NO_HAY_SISTEMA: return !st.sis_presets;
	default:                return true;
	}
}

float Overlay::DrawTripleMenu(void)
{
	// Se arma TODO el contenido primero y recien despues se dibuja: el fondo
	// tiene que medir lo que mide el texto, y eso no se sabe hasta haberlo
	// compuesto. La primera version fijaba un ancho a ojo y el texto se salia
	// de la caja — MeasureString existe justamente para no adivinar.
	//
	// QUE SE MUESTRA Y QUE NO: la huella del auto y el numero de perfil son
	// identificadores INTERNOS (un hash y un indice que se asigna solo). Al
	// usuario no le dicen nada y no puede crearlos ni nombrarlos, asi que no van
	// en pantalla. Pero tampoco se ocultan del todo: si el reconocimiento falla,
	// sin ninguna senal el usuario no tendria como enterarse. Queda un renglon
	// chico al pie que en el caso normal casi no se ve, y que se pone naranja y
	// explicito solo cuando algo NO se reconocio.
	//
	// La ESCALA por fila es lo que permite jerarquia tipografica: la fuente del
	// overlay viene de un recurso incrustado a un tamano fijo, asi que sin
	// escalar todo tendria el mismo cuerpo.
	struct Fila {
		wchar_t txt[TT_MAX_TXT];
		TtCol color;
		bool titulo;      // centrado
		bool separador;   // linea horizontal en vez de texto
		float antes;      // aire por encima, en renglones
		float escala;     // 1.0 = tamano normal de la fuente
	};
	Fila fila[TT_MAX_FILAS];
	int n = 0;

	// DR1: los presets y el reconocimiento de auto todavia no existen. Se detecta
	// una vez por dibujo y se propaga; sin esto el barrido de slots de abajo
	// compararia 0 contra 0 y "encontraria" 40 presets guardados del auto 0.
	bool sis_presets = tt_sistema_presets();
	// v6: la identidad es la EFECTIVA ($auto_efectivo, entrada 1..N del
	// selector), no la huella cruda de x8. Y "hay auto" pasa a ser el estado
	// de vinculo: 1 = la huella estable del habitaculo matcheo la tabla. Con
	// template viejo (sin $auto_vinculo) las filas AUTO/NOAUTO no se dibujan.
	bool sis_auto = sis_presets && tt_global_existe(TT_VINCULO_VAR);

	int vinculo = 0;
	bool hay_auto = sis_auto && tt_vinculo(&vinculo) && vinculo == 1;
	float auto_ef = sis_presets ? tt_auto_efectivo(0.0f) : 0.0f;
	float pre_crudo = sis_presets ? tt_global(L"$pre_actual", 0.0f) : 0.0f;
	int activo = (int)pre_crudo;

	// Que presets tiene guardados ESTE auto: el barrido cacheado (v6). El
	// panel de asiento pasa exactamente el MISMO par (auto, preset) — si
	// difirieran, cada panel invalidaria la cache del otro en cada frame.
	const TtGuardadas &gd = tt_guardadas_de(sis_presets ? auto_ef : 0.0f, pre_crudo);
	int cuantos = sis_presets ? gd.cuantos : 0;

	// La lista de numeros guardados se arma una sola vez y se le pasa al
	// resolvedor como %GUARDADAS%.
	wchar_t usados[64];
	usados[0] = 0;
	{
		int k = 0;
		for (int p = 1; sis_presets && p <= TT_PRESETS; p++) {
			if (!gd.tiene[p])
				continue;
			k += swprintf_s(usados + k, 64 - k, (k ? L" %d" : L"%d"), p);
		}
	}

	TtEstado st;
	st.fwd        = tt_global(L"$seat_fwd", 0.0f);
	st.up         = tt_global(L"$seat_up", 0.0f);
	st.right      = tt_global(L"$seat_right", 0.0f);
	st.pitch      = tt_global(L"$seat_pitch", 0.0f);
	st.activo     = activo;
	st.guardadas  = usados;
	st.hay_auto   = hay_auto;
	// PRESET/NOPRESET conservan la semantica que ya tenia el cartel horneado: la
	// pregunta que le importa al usuario es "¿tengo algo guardado para este
	// auto?", que es la que decide entre el renglon de estado y el de "todavia no
	// guardaste ninguna". Cual esta PUESTA lo dice %PRESET%, que vale "-" si hay
	// guardadas pero ninguna cargada.
	st.hay_preset = (cuantos > 0);
	st.sis_presets = sis_presets;
	st.sis_auto    = sis_auto;

	tt_asegurar_menu();

	for (size_t i = 0; i < tt_filas.size() && n < TT_MAX_FILAS; i++) {
		const TtFilaDef &d = tt_filas[i];
		if (!tt_cond_se_cumple(d.cond, st))
			continue;
		if (d.separador)
			fila[n].txt[0] = 0;
		else if (!tt_resolver(d.texto, st, fila[n].txt))
			continue;   // no entra en 192: se descarta la fila y se sigue
		fila[n].color     = d.color;
		fila[n].titulo    = d.centrado;
		fila[n].separador = d.separador;
		fila[n].antes     = d.antes;
		fila[n].escala    = d.escala;
		n++;
	}

	// Sin ninguna fila no hay nada que medir ni que encuadrar (dibujar la caja
	// vacia seria un rectangulo negro sin explicacion).
	if (n == 0)
		return -1.0f;

	// --- medir ---
	const float RENGLON = mFont->GetLineSpacing();
	const float MARGEN = RENGLON * 1.0f;
	float ancho = 0.0f, alto = 0.0f;
	for (int i = 0; i < n; i++) {
		alto += RENGLON * fila[i].antes;
		if (fila[i].separador) {
			alto += RENGLON * 0.25f;
			continue;
		}
		Vector2 m = mFont->MeasureString(fila[i].txt);
		if (m.x * fila[i].escala > ancho)
			ancho = m.x * fila[i].escala;
		alto += RENGLON * fila[i].escala;
	}

	// --- ubicar: por FRACCIONES del render target (v6, port de DR2) ---
	// El centro sale de $ma_fx si el ini lo trae, con default 1/6: el monitor
	// IZQUIERDO. En el medio esta el camino, y este cartel se usa MANEJANDO
	// (el estabilizador se calibra en pista); DR2 lo movio por eso y aca se
	// hereda la decision. Se ajusta desde el ini sin recompilar.
	float cx = (float)mResolution.x * tt_global(L"$ma_fx", 1.0f / 6.0f);
	float x0 = cx - ancho / 2.0f;
	float y0 = RENGLON * 2.0f + MARGEN;

	DrawRectangle(x0 - MARGEN, y0 - MARGEN,
		ancho + MARGEN * 2.0f, alto + MARGEN * 2.0f, 0.0f, 0.0f, 0.0f, 0.80f);

	// --- dibujar ---
	float y = y0;
	for (int i = 0; i < n; i++) {
		y += RENGLON * fila[i].antes;
		if (fila[i].separador) {
			DrawRectangle(x0, y + RENGLON * 0.10f, ancho, 1.0f,
				1.0f, 1.0f, 1.0f, 0.22f);
			y += RENGLON * 0.25f;
			continue;
		}
		DirectX::XMVECTOR c = tt_rgb(fila[i].color);
		float esc = fila[i].escala;
		Vector2 m = mFont->MeasureString(fila[i].txt);
		float x = fila[i].titulo ? x0 + (ancho - m.x * esc) / 2.0f : x0;

		// Mismo contorno negro que DrawOutlinedString, pero con escala. El
		// contorno se deja en 1 px a cualquier escala: es lo que hace legible
		// el texto sobre cualquier imagen del juego.
		static const Vector2 borde[4] = {
			Vector2(-1, 0), Vector2(1, 0), Vector2(0, -1), Vector2(0, 1)
		};
		for (int k = 0; k < 4; k++) {
			mFont->DrawString(mSpriteBatch.get(), fila[i].txt,
				Vector2(x, y) + borde[k], DirectX::Colors::Black,
				0.0f, Vector2(0, 0), esc);
		}
		mFont->DrawString(mSpriteBatch.get(), fila[i].txt, Vector2(x, y), c,
			0.0f, Vector2(0, 0), esc);

		y += RENGLON * esc;
	}

	// El borde INFERIOR de la caja, para que el panel de asiento se apile
	// justo debajo. MEDIDO, no calculado aparte: el alto depende de las filas
	// que realmente se dibujaron.
	return y0 + alto + MARGEN;
}

// ===========================================================================
// EL PANEL DE ASIENTO (port de DR2 v1.86..v1.96)
//
// Las filas, de arriba a abajo (las del medio dependen del estado de vinculo,
// asi que va de 3 a 6 renglones):
//   Lancia Delta S4                   el nombre sale de TrueTriple-DR1.autos.txt
//   NO RECONOCEMOS EL AUTO / AUTO SIN VINCULAR   (el estado 1, sano, no dibuja)
//   Pasa por la vista de cockpit      solo en los estados que BLOQUEAN
//   Los slots estan bloqueados...     idem
//   SLOTS GUARDADOS: 1 - 2 - 3        o "ninguno"
//   ADELANTE 0.300  ARRIBA -0.040 ... la postura viva
//   Cargar 1-9  Guardar Ctrl+1-9 ...  teclas fijas
//
// Se abre JUNTO con el cartel (misma tecla): no tiene interruptor propio, y el
// gate de edicion del ini ($menu_visible) es la misma variable de la que sale
// y8 — si ves el panel, las teclas andan, por construccion.
// ===========================================================================
float Overlay::DrawMenuAutos(float y_arranque)
{
	float sel = tt_auto_efectivo(-1.0f);
	if (sel < -0.5f)
		return -1.0f;   // template viejo sin la variable: sin panel, sin adivinar
	// 0 = AUTO NO RECONOCIDO (2026-09-05): el panel se dibuja igual, con ese
	// texto en el lugar del nombre. Pedido del autor: al abrir el juego no
	// tiene que decir el auto anterior ni uno cualquiera.
	int elegido = (int)(sel + 0.5f);

	// El MISMO barrido cacheado que DrawTripleMenu, con el MISMO par.
	const TtGuardadas &g = tt_guardadas_de(sel, tt_global(L"$pre_actual", 0.0f));
	bool alguna = (g.cuantos > 0);

	// El nombre del auto es OPCIONAL y sale de TrueTriple-DR1.autos.txt. Sin
	// archivo, o sin linea para este numero, se dibuja "CAR 07" a secas: el
	// numero es el dato con autoridad y esta siempre.
	tt_asegurar_autos();
	wchar_t linea_auto[16 + TT_AUTO_NOMBRE_MAX];
	const wchar_t *nombre = (elegido >= 1 && elegido <= TT_AUTOS_MAX)
			? tt_autos[elegido].c_str() : L"";
	if (elegido < 1)
		swprintf_s(linea_auto, L"%s", tt_txt(L"no_reconocido", L"CAR NOT RECOGNISED"));
	else if (nombre[0])
		swprintf_s(linea_auto, L"%s", nombre);
	else
		swprintf_s(linea_auto, L"%s %02d", tt_txt(L"auto", L"CAR"), elegido);

	wchar_t lista[96];
	{
		int k = swprintf_s(lista, L"%s", tt_txt(L"slots", L"SAVED SLOTS:"));
		bool primero = true;
		for (int p = 1; p <= TT_PRESETS; p++) {
			if (!g.tiene[p])
				continue;
			k += swprintf_s(lista + k, _countof(lista) - k,
				primero ? L" %d" : L" - %d", p);
			primero = false;
		}
		// "ninguno" y no un guion: un simbolo suelto en el renglon mas mirado
		// del panel se lee como que algo se rompio. ⚠️ LAS LLAVES NO SON DE
		// ESTILO: sin ellas, en DR2 el segundo wcscat_s quedaba fuera del if y
		// "ninguno" se pegaba siempre ("1 - 2none").
		if (!alguna) {
			wcscat_s(lista, L" ");
			wcscat_s(lista, tt_txt(L"ninguno", L"none"));
		}
	}

	// Estado de vinculo. El estado BUENO (1) no dibuja nada: un cartel verde
	// permanente entrena a no mirar el renglon. Los estados 0 y 2 explican por
	// que las teclas de slot no responden — sin esto vuelve el bug de "aprieto
	// y no pasa nada".
	int vinculo = 0;
	bool hay_vinculo = tt_vinculo(&vinculo);
	const wchar_t *txt_estado = NULL;
	const wchar_t *txt_como = NULL;
	TtCol col_estado = TT_NARANJA;
	switch (vinculo) {
	case 1:
		txt_estado = NULL;
		break;
	default:
		// Estado 0. (El 2, "huella sin vincular", se fue con la tabla de pares.)
		txt_estado = tt_txt(L"sin_auto", L"CAR NOT RECOGNISED");
		txt_como   = tt_txt(L"pasa_cockpit", L"It is recognised as soon as the stage loads");
		break;
	}

	TtFilaPanel f[12];
	int nf = 0;
	// LAS ESCALAS SALEN DE PIXELES REALES: la fuente del overlay tiene 19 px
	// de mayuscula a escala 1.0 (~26 px de cuerpo) -> escala = px/26, todo
	// multiplicado por base = alto/1440. SIN TITULO: el cartel ya dice
	// TrueTriple arriba; lo que identifica al panel es el nombre del auto.
	tt_fila_panel(f, &nf, _countof(f), linea_auto,       1.15f, 0.30f,
	              elegido < 1 ? TT_NARANJA : TT_ORO);
	if (hay_vinculo && txt_estado) {
		tt_fila_panel(f, &nf, _countof(f), txt_estado,   0.69f, 0.45f, col_estado);
		if (txt_como) {
			tt_fila_panel(f, &nf, _countof(f), txt_como, 0.55f, 0.12f, TT_NARANJA);
			tt_fila_panel(f, &nf, _countof(f),
				tt_txt(L"bloqueados", L"Slots stay locked until the car is recognised"),
				                    0.55f, 0.12f, TT_NARANJA);
		}
	}
	tt_fila_panel(f, &nf, _countof(f), lista,            0.69f, 0.45f, TT_VERDE);

	// LA POSTURA VIVA: el menu es ademas el modo de edicion (con el cerrado
	// las teclas de asiento no hacen nada), asi que esta es la pantalla que el
	// usuario tiene delante mientras ajusta. Sale de los MISMOS globals que
	// mueven las teclas; si la plantilla no los declara, la fila no se dibuja.
	wchar_t postura[96];
	if (tt_global_existe(L"$seat_fwd") && tt_global_existe(L"$seat_up")
			&& tt_global_existe(L"$seat_right") && tt_global_existe(L"$seat_pitch")) {
		// EL FORMATO LO PONE EL C++, LAS ETIQUETAS VIENEN TRADUCIDAS: si el
		// texto traducido llevara los %.3f adentro, un especificador de mas
		// seria basura de memoria en pantalla o un cierre del juego.
		swprintf_s(postura, L"%s %.3f   %s %.3f   %s %.3f   %s %.1f",
			tt_txt(L"adelante", L"FWD"),      tt_global(L"$seat_fwd", 0.0f),
			tt_txt(L"arriba", L"UP"),         tt_global(L"$seat_up", 0.0f),
			tt_txt(L"lateral", L"SIDE"),      tt_global(L"$seat_right", 0.0f),
			tt_txt(L"inclinacion", L"TILT"),  tt_global(L"$seat_pitch", 0.0f));
		tt_fila_panel(f, &nf, _countof(f), postura, 0.62f, 0.20f, TT_CIELO);
	}

	tt_fila_panel(f, &nf, _countof(f),
		tt_txt(L"teclas", L"Load 1-9   Save Ctrl+1-9   Delete Ctrl+Shift+1-9   Cycle 0"),
		                                    0.50f, 1.00f, TT_PLATA);

	// --- medir ---
	const float RENGLON = mFont->GetLineSpacing();
	float base = (float)mResolution.y / 1440.0f;
	float ancho = 0.0f, alto = 0.0f;
	for (int i = 0; i < nf; i++) {
		Vector2 m = mFont->MeasureString(f[i].txt);
		float e = f[i].esc * base;
		if (m.x * e > ancho)
			ancho = m.x * e;
		alto += RENGLON * (f[i].antes * base + e);
	}

	// --- ubicar: centro por FRACCIONES del render target ---
	float cx = (float)mResolution.x * tt_global(L"$ma_fx", 1.0f / 6.0f);
	float cy = (float)mResolution.y * tt_global(L"$ma_fy", 0.5f);
	float x0 = cx - ancho / 2.0f;
	float MARGEN = RENGLON * base;
	// Apilado debajo del cartel si se dibujo: el +MARGEN saca el borde de la
	// caja de arriba y el +RENGLON es el renglon de aire que las separa (dos
	// cajas, no una partida al medio).
	float y0 = (y_arranque >= 0.0f) ? (y_arranque + MARGEN + RENGLON * base)
						: (cy - alto / 2.0f);

	DrawRectangle(x0 - MARGEN, y0 - MARGEN,
		ancho + MARGEN * 2.0f, alto + MARGEN * 2.0f, 0.0f, 0.0f, 0.0f, 0.80f);

	// --- dibujar: todo centrado, mismo contorno de 1 px que el cartel ---
	float y = y0;
	for (int i = 0; i < nf; i++) {
		float e = f[i].esc * base;
		y += RENGLON * f[i].antes * base;
		DirectX::XMVECTOR c = tt_rgb(f[i].color);
		Vector2 m = mFont->MeasureString(f[i].txt);
		float x = x0 + (ancho - m.x * e) / 2.0f;
		static const Vector2 borde[4] = {
			Vector2(-1, 0), Vector2(1, 0), Vector2(0, -1), Vector2(0, 1)
		};
		for (int k = 0; k < 4; k++) {
			mFont->DrawString(mSpriteBatch.get(), f[i].txt,
				Vector2(x, y) + borde[k], DirectX::Colors::Black,
				0.0f, Vector2(0, 0), e);
		}
		mFont->DrawString(mSpriteBatch.get(), f[i].txt, Vector2(x, y), c,
			0.0f, Vector2(0, 0), e);
		y += RENGLON * e;
	}

	return y0 + alto + MARGEN;
}

void Overlay::DrawOverlay(void)
{
	Profiling::State profiling_state;
	HRESULT hr;

	// TrueTriple: el cartel tambien abre el overlay. y8 lo tiene que publicar
	// [Present] del template (= triple activo Y cartel no oculto). Sin esto el
	// overlay solo se dibuja cazando, con un aviso pendiente o midiendo.
	//
	// !! DR1, HOY: el template llega solo hasta la fila 7, asi que
	// G->iniParams.size() vale 8, `triple_menu` es SIEMPRE false y el cartel no
	// se dibuja nunca. Es deliberado — el flag es del template y aca no se
	// inventa uno. Se enciende solo el dia que [Present] publique y8. El log lo
	// dice una vez por sesion para que no haya que adivinar por que no aparece.
	bool hay_fila8 = (G->iniParams.size() >= 9);
	bool triple_menu = (hay_fila8 && G->iniParams[8].y > 0.5f);

	// v6: LOS DOS MENUES SE ABREN JUNTOS (misma tecla). El panel de asiento se
	// dibuja cuando se dibuja el cartel, apilado debajo. Sin identidad de auto
	// (plantilla vieja) queda apagado: el panel entero habla de un auto.
	bool menu_autos = triple_menu && (tt_auto_efectivo(-1.0f) >= 0.5f);

	static bool tt_aviso_fila8 = false;
	if (!hay_fila8 && !tt_aviso_fila8) {
		tt_aviso_fila8 = true;
		LogInfo("TrueTriple DR1 [cartel F3]: el ini no publica la fila 8 de IniParams"
			" (size=%Iu); el cartel queda apagado. Hace falta y8 en [Present].\n",
			G->iniParams.size());
	}

	if (G->hunting != HUNTING_MODE_ENABLED && !has_notice && !triple_menu
			&& !menu_autos && Profiling::mode == Profiling::Mode::NONE)
		return;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	// Since some games did not like having us change their drawing state from
	// SpriteBatch, we now save and restore all state information for the GPU
	// around our drawing.  
	SaveState();
	{
		hr = InitDrawState();
		if (FAILED(hr))
			goto fail_restore;

		mSpriteBatch->Begin();
		{
			wchar_t osdString[maxstring];
			Vector2 strSize;
			Vector2 textPosition;
			float y = 10.0f;

			if (G->hunting == HUNTING_MODE_ENABLED) {
				// Top of screen
				CreateShaderCountString(osdString);
				strSize = mFont->MeasureString(osdString);
				textPosition = Vector2(float(mResolution.x - strSize.x) / 2, y);
				DrawOutlinedString(mFont.get(), osdString, textPosition, DirectX::Colors::LimeGreen);
				y += strSize.y;

				DrawShaderInfoLines(&y);

				// Bottom of screen
				CreateStereoInfoString(mHackerDevice->mStereoHandle, osdString);
				strSize = mFont->MeasureString(osdString);
				textPosition = Vector2(float(mResolution.x - strSize.x) / 2, float(mResolution.y - strSize.y - 10));
				DrawOutlinedString(mFont.get(), osdString, textPosition, DirectX::Colors::LimeGreen);
			}

			if (has_notice)
				DrawNotices(&y);

			if (Profiling::mode != Profiling::Mode::NONE)
				DrawProfiling(&y);

			// El cartel devuelve donde termina su caja y el panel arranca
			// ahi: apilados de verdad, no con dos posiciones que se parecen.
			float abajo_del_cartel = -1.0f;
			if (triple_menu)
				abajo_del_cartel = DrawTripleMenu();

			if (menu_autos)
				DrawMenuAutos(abajo_del_cartel);
		}
		mSpriteBatch->End();
	}
fail_restore:
	RestoreState();

	flush_d3d11on12(mOrigDevice, mOrigContext);

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::overlay_overhead);
}

OverlayNotice::OverlayNotice(std::wstring message) :
	message(message),
	timestamp(0)
{
}

void ClearNotices()
{
	int level;

	if (notice_cleared_frame == G->frame_no)
		return;

	EnterCriticalSectionPretty(&notices.lock);

	for (level = 0; level < NUM_LOG_LEVELS; level++)
		notices.notices[level].clear();

	notice_cleared_frame = G->frame_no;
	has_notice = false;

	LeaveCriticalSection(&notices.lock);
}

void LogOverlayW(LogLevel level, wchar_t *fmt, ...)
{
	wchar_t msg[maxstring];
	va_list ap;

	va_start(ap, fmt);
	vLogInfoW(fmt, ap);

	// Using _vsnwprintf_s so we don't crash if the message is too long for
	// the buffer, and truncate it instead - unless we can automatically
	// wrap the message, which DirectXTK doesn't appear to support, who
	// cares if it gets cut off somewhere off screen anyway?
	_vsnwprintf_s(msg, maxstring, _TRUNCATE, fmt, ap);

	EnterCriticalSectionPretty(&notices.lock);

	notices.notices[level].emplace_back(msg);
	has_notice = true;

	LeaveCriticalSection(&notices.lock);

	va_end(ap);
}

// ASCII version of the above. DirectXTK only understands wide strings, so we
// need to convert it to that, but we can't just convert the format and hand it
// to LogOverlayW, because that would reverse the meaning of %s and %S in the
// format string. Instead we do our own vLogInfo and _vsnprintf_s to handle the
// format string correctly and convert the result to a wide string.
void LogOverlay(LogLevel level, char *fmt, ...)
{
	char amsg[maxstring];
	wchar_t wmsg[maxstring];
	va_list ap;

	va_start(ap, fmt);
	vLogInfo(fmt, ap);

	if (!log_levels[level].hide_in_release || G->hunting) {
		// Using _vsnprintf_s so we don't crash if the message is too long for
		// the buffer, and truncate it instead - unless we can automatically
		// wrap the message, which DirectXTK doesn't appear to support, who
		// cares if it gets cut off somewhere off screen anyway?
		_vsnprintf_s(amsg, maxstring, _TRUNCATE, fmt, ap);
		mbstowcs(wmsg, amsg, maxstring);

		EnterCriticalSectionPretty(&notices.lock);

		notices.notices[level].emplace_back(wmsg);
		has_notice = true;

		LeaveCriticalSection(&notices.lock);
	}

	va_end(ap);
}
