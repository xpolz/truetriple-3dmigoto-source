// This Overlay class is to encapsulate all the on-screen drawing code,
// including creating and using the DirectXTK code.

#include "Overlay.h"

#include <stdexcept>
#include <map>
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

// CommandList.cpp: linea permanente del ensayo de culling (2026-09-05). Vacia si no aplica.
void TripleCullingOSD(wchar_t *buf, size_t n);
void TripleAutoCapturaOSD(wchar_t *buf, size_t n);

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
// MENU FLOTANTE DE TrueTriple
//
// Por que aca y no una ventana propia: 3Dmigoto ya trae el motor de overlay
// (SpriteFont + SpriteBatch) y, sobre todo, ya resuelve lo unico verdaderamente
// dificil — SaveState()/RestoreState() alrededor del dibujo. Esa es la regla 1
// del proyecto ("el estado del pipeline es global y sobrevive al draw"), la que
// nos mordio cinco veces. Reusarlo es gratis y ya esta probado en este juego.
//
// De donde sale lo que muestra:
//   - IniParams: w3 = m11 de la camara, w7 = su near, y8 = "mostrar el menu"
//     (lo publica [Present]). x8 es la huella de la malla del habitaculo: se
//     retiro en v1.85 y VOLVIO en v1.86, pero como LECTURA DE INSTRUMENTO —
//     la identidad sigue siendo MANUAL ($auto_elegido) y el ini la usa solo
//     para auto-seleccionar un auto que el usuario ya asocio a esa huella.
//     Este cartel no la lee: muestra el numero elegido, no el medido.
//   - command_list_globals: las variables del ini por nombre. El mod ya guarda
//     ahi el slot activo y la postura, asi que el menu no calcula nada por su
//     cuenta: muestra exactamente lo que el sistema esta usando.
//
// El menu NO tiene entrada propia todavia: las teclas siguen siendo las de
// siempre y el panel las lista. Se dibuja centrado en el monitor del medio,
// porque en un triple el centro de la pantalla completa es justo donde mira el
// usuario.
// ===========================================================================
// TIENEN QUE COINCIDIR con tools/gen_presets_asiento.py (N_SLOTS y N_PRE). Si el
// generador crece y esto no, el menu deja de ver los ultimos slots — y como el
// resto sigue funcionando, el sintoma seria "un preset que guarde no aparece en
// la lista", que es de los que cuesta atribuir. verificar_ini.py lo comprueba.
//
// v1.86: 40 -> 540 (60 entradas de auto x 9 posturas, decision del autor). Con
// ese salto el barrido de slots dejo de ser gratis y paso a tener cache: ver
// tt_guardadas_de mas abajo, que es donde esta el razonamiento.
#define TT_SLOTS   540
#define TT_PRESETS 9

// Si el global EXISTE. `tt_global` devuelve el default cuando no esta, y eso no
// alcanza para decidir si dibujar un dato: un 0.0 de verdad y un 0.0 por
// ausencia se ven identicos en pantalla.
static bool tt_hay_global(const wchar_t *nombre)
{
	return command_list_globals.find(nombre) != command_list_globals.end();
}

static float tt_global(const wchar_t *nombre, float por_defecto)
{
	CommandListVariables::iterator i = command_list_globals.find(nombre);
	if (i == command_list_globals.end())
		return por_defecto;
	return i->second.fval;
}

// LA IDENTIDAD QUE HAY QUE MOSTRAR ES LA EFECTIVA, NO LA DECLARADA (v1.86)
//
// Plantilla v13: $auto_efectivo = la entrada que matcheo en la tabla
// huella<->auto que el usuario enseño, o $auto_elegido si no hubo match. Es la
// clave con la que se guardan y se buscan los presets, asi que es la que tiene
// que decir la pantalla.
//
// LOS CUATRO LUGARES que preguntan por el auto pasan por aca a proposito. Si
// alguno leyera $auto_elegido por su cuenta, con la auto-seleccion activa el
// panel mostraria la entrada DECLARADA mientras los presets serian los de la
// EFECTIVA — dos numeros distintos para la misma cosa, sin un solo error a la
// vista. CLAUDE.md lo dejo anotado como contrato pendiente del lado del C++.
//
// Los dos respaldos son a proposito y no son lo mismo:
//   - la variable no existe (template v12 o anterior) -> vale $auto_elegido,
//     que es la identidad de esa plantilla; el panel no se apaga.
//   - existe pero todavia vale 0 (el primer frame, antes de que corra el
//     [Present] que la deriva) -> tambien $auto_elegido, para no parpadear.
static float tt_auto_efectivo(float por_defecto)
{
	CommandListVariables::iterator i = command_list_globals.find(L"$auto_efectivo");
	if (i != command_list_globals.end() && i->second.fval > 0.5f)
		return i->second.fval;
	return tt_global(L"$auto_elegido", por_defecto);
}

// ---------------------------------------------------------------------------
// ESTADO DE VINCULO DEL HABITACULO (v1.86)
//
// Lo publica la plantilla (v14):
//   0 = la entrada seleccionada no tiene habitaculo vinculado
//   1 = vinculada, y coincide con el habitaculo que se esta viendo
//   2 = el habitaculo que se ve esta vinculado a OTRA entrada
//
// El NOMBRE del global vive en UNA sola constante a proposito: lo esta cerrando
// el agente de la plantilla y cambiarlo no puede obligar a buscar por todo el
// archivo.
//
// Devuelve false si la variable NO EXISTE (dll nuevo + plantilla vieja) o si
// trae un valor que este dll no conoce. En los dos casos el panel no dibuja la
// linea: **un estado desconocido no se adivina**, porque el renglon existe
// justamente para que el autor sepa por que autos ya paso en la recorrida de
// vinculacion, y uno que mienta es peor que uno que falte.
//
// POR QUE ESTE CARTEL ES LA FUNCION ENTERA, Y NO UN ADORNO. Con la plantilla
// v14, en los estados 0 y 2 **guardar y cargar slots esta BLOQUEADO** (para que
// nadie guarde una postura bajo el auto equivocado). O sea que el usuario
// aprieta Ctrl+1 y no pasa nada, y este panel es lo UNICO que se lo explica.
// Sin el, o con un texto generico, vuelve el bug de "aprieto y no pasa nada"
// que ya costo una noche entera de diagnostico. Por eso los estados 0 y 2 no
// dicen solo que fallo: dicen QUE HACER, y despues que los slots estan
// bloqueados hasta arreglarlo. El estado 1, en cambio, dice una sola palabra:
// un diagnostico largo con todo sano se lee como un error (leccion del repo).
//
// ⚠️ SIN ACENTOS, y no es un descuido. La fuente del overlay es un recurso
// incrustado con 95 glifos (ASCII 0x20..0x7E) y SetDefaultCharacter(L'?'), asi
// que "Pasa" con tilde se dibujaria "Pas?". El cartel de F3 que genera el
// lanzador ya escribe asi ("Inclinacion", "posicion", "menu"): es la convencion
// del proyecto, no una licencia de este panel.
#define TT_VINCULO_VAR L"$auto_vinculo"

// La tecla que vincula el habitaculo con la entrada elegida. Tiene que decir lo
// MISMO que [KeyAutoVincular] del template (hoy 'ctrl no_shift VK_F2'): un
// cartel que nombra una tecla equivocada es peor que uno que no la nombre.
//
// OJO: esta seccion SOLO existe en el template de --curador. En el producto la
// tecla no esta, pero el cartel tampoco se ve: sale unicamente en el estado 2
// ($auto_vinculo == 2, "hay huella y no esta en la tabla"), y con la tabla
// horneada el usuario no llega a ese estado con un auto del plantel.
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

// ---------------------------------------------------------------------------
// QUE POSTURAS TIENE GUARDADAS ESTE AUTO — CON CACHE (v1.86)
//
// El barrido pregunta $pre_car_i y $pre_num_i por cada slot, y cada tt_global
// es una busqueda por NOMBRE en un mapa (arma un wstring y compara texto). Con
// TT_SLOTS = 40 eran 80 busquedas por panel y por frame; con 540 son 1080, y
// los dos paneles pueden estar abiertos a la vez -> ~2160 por frame, en cada
// Present. Es la regla dura 2 del proyecto: nada caro por frame.
//
// La cache se recalcula solo cuando algo que la afecta pudo cambiar:
//   - cambio el auto EFECTIVO (otro auto, otros presets);
//   - cambio $pre_actual (cargar o guardar una postura mueve ese numero);
//   - o pasaron TT_CACHE_FRAMES. Esta es la RED, y hace falta: guardar una
//     postura sobre el numero que YA estaba puesto no mueve ninguna de las dos
//     senales de arriba, y sin tope la lista se quedaria vieja hasta el
//     proximo cambio de auto.
// Quedan 3 busquedas por frame en vez de ~2160, y el peor retraso posible en un
// renglon informativo es medio segundo.
//
// ⚠️ Los DOS paneles tienen que pasar exactamente el MISMO par (auto, preset) o
// se invalidan la cache entre si en cada frame y no cachea nada. Por eso los
// dos leen $pre_actual crudo y le pasan el float, no el int truncado.
// ---------------------------------------------------------------------------
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
		// La comparacion es por DIFERENCIA con ±0.5. Con la huella de malla
		// (> 2^23) era obligatorio —float32 ahi no tiene medios y (huella -
		// 0.5f) se redondeaba al propio valor—; con la identidad efectiva
		// (enteros chicos: el selector va 1..N, hoy 82) es la igualdad entera
		// robusta, y se conserva el
		// criterio para no tener dos.
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
// Contrato completo en docs/CONTRATO-MENU-F3.md (CERRADO). Resumen de por que:
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
// El color de una fila. Estaba escrito como un `switch` COPIADO en cada lugar
// que dibuja (cartel, panel de asiento y, al sumar la caja del tipo Z, iba por
// la tercera). Tres copias del mismo mapeo se desincronizan al primer color
// nuevo, y el sintoma seria una fila de otro color en un solo recuadro.
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

enum TtCond { TT_SIEMPRE, TT_CON_PRESET, TT_SIN_PRESET, TT_CON_AUTO, TT_SIN_AUTO };

struct TtFilaDef {
	std::wstring texto;   // plantilla, con los marcadores todavia sin resolver
	TtCol  color;
	TtCond cond;
	bool   centrado;
	bool   separador;
	// Tipo Z (v1.95): NO va adentro del cartel. El dll la saca y la dibuja en
	// una caja propia, debajo del panel de asiento. Existe porque el texto y
	// el bind tienen que seguir saliendo del archivo —traducidos y con el
	// rebind del usuario resuelto—: escribirla en el C++ la dejaria en
	// espanol fijo y con F3 horneada.
	bool   aparte;
	float  antes;         // aire por encima, en renglones
	float  escala;        // 1.0 = cuerpo normal de la fuente
};

// Las filas de tipo Z, YA RESUELTAS, tal como las dejo la ultima pasada de
// DrawTripleMenu. Se llenan ahi y no en DrawFilaAparte para no armar el
// TtEstado dos veces: ese estado sale de media docena de globals y de la
// cache de slots, y dos copias de esa logica se desincronizan solas. Como
// las Z solo se dibujan cuando el cartel se dibujo, una pasada alcanza.
struct TtFilaZ { wchar_t txt[TT_MAX_TXT]; TtCol color; bool centrado; float escala; };
static TtFilaZ tt_z[4];
static int     tt_z_n = 0;

// LOS TEXTOS DEL PANEL DE ASIENTO, POR CLAVE (v1.96).
//
// Hasta ahora estaban escritos a mano en espanol ACA ADENTRO, asi que el panel
// salia en espanol aunque la app estuviera en ingles. El cartel de F3 no tiene
// ese problema porque su texto lo escribe la app en TrueTriple.menu.txt, ya
// traducido; el panel reusa ese mismo archivo con filas `P|clave|texto`.
//
// SIEMPRE CON RESPALDO: tt_txt() devuelve el literal espanol si la clave no
// esta. Un archivo viejo, o uno al que le falte una linea, deja el panel como
// estaba — nunca en blanco. Un panel con un renglon vacio no se lee como
// "falta una traduccion", se lee como que el mod se rompio.
//
// NINGUN texto de estos puede traer un especificador de formato: el dll compone
// los numeros por su cuenta y solo concatena estas etiquetas. Un %.3f mal
// escrito en una traduccion no seria un texto feo, seria basura de memoria en
// pantalla. Lo ancla un check del generador, no la buena memoria de nadie.
static std::map<std::wstring, std::wstring> tt_textos;

static const wchar_t *tt_txt(const wchar_t *clave, const wchar_t *respaldo)
{
	std::map<std::wstring, std::wstring>::const_iterator i = tt_textos.find(clave);
	return (i != tt_textos.end() && !i->second.empty()) ? i->second.c_str() : respaldo;
}

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
	return false;  // condicion desconocida: fila descartada
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
	// asiento (v1.96). Tienen TRES campos, no siete, asi que se atienden antes
	// del parseo de abajo — que exige seis separadores y las descartaria.
	//
	// El texto se toma CRUDO hasta el fin de linea: puede llevar espacios
	// internos que son parte del diseno (la linea de teclas separa columnas con
	// espacios), y recortarlo los comeria.
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

	out->aparte = !_wcsicmp(campo[0].c_str(), L"Z");
	if (!_wcsicmp(campo[0].c_str(), L"S"))
		out->separador = true;
	else if (!_wcsicmp(campo[0].c_str(), L"T"))
		out->separador = false;
	else if (!_wcsicmp(campo[0].c_str(), L"Z"))
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
	// Y los textos con nombre, por el mismo motivo: el archivo se relee en
	// caliente cuando cambia, y una clave que desaparecio del archivo nuevo
	// seguiria viva con el valor viejo — o sea el idioma anterior mezclado.
	tt_textos.clear();
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
static const wchar_t *tt_menu_embebido =
	L"T||ORO|1|0.00|1.00|TrueTriple   DiRT Rally 2\n"
	L"T||BLANCO|1|0.70|0.88|SAVE UP TO NINE CAMERA POSITIONS\n"
	L"T||BLANCO|1|0.02|0.88|FOR EACH CAR\n"
	L"S||PLATA|0|0.85|1.00|\n"
	L"T||BLANCO|0|0.60|1.00|Forward   %FWD% m        Height       %UP% m\n"
	L"T||BLANCO|0|0.15|1.00|Lateral   %RIGHT% m        Pitch        %PITCH% degrees\n"
	L"T|PRESET|VERDE|1|0.70|0.80|POSITION %PRESET%          saved:  %GUARDADAS%\n"
	L"T|NOPRESET|CREMA|1|0.70|0.74|NOTHING SAVED YET:  GET COMFORTABLE AND PRESS CTRL + 1\n"
	L"S||PLATA|0|0.85|1.00|\n"
	L"T||CIELO|0|0.60|1.00|Home / End          Move closer to or away from the wheel\n"
	L"T||CIELO|0|0.18|1.00|PgUp / PgDn         Raise or lower your head\n"
	L"T||CIELO|0|0.18|1.00|Insert / Delete     Move to the right or to the left\n"
	L"T||CIELO|0|0.18|1.00|Numpad 8 / 2        Tilt your view up or down\n"
	L"T||CIELO|0|0.18|1.00|Ctrl + Home         Back to zero\n"
	L"T||ORO|0|0.45|1.00|Ctrl + 1 to 9       Save this position under that number\n"
	L"T||ORO|0|0.18|1.00|1 to 9              Go back to a saved position\n"
	L"T||ORO|0|0.18|1.00|0                   Jump to the next saved one\n"
	L"T||ORO|0|0.18|1.00|Ctrl + Shift + 1 to 9 Delete that saved position\n"
	L"T||CIELO|0|0.45|1.00|F7 / F3             Show or hide this menu\n"
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
//
// Toma el NOMBRE del archivo (v1.86) porque ahora son dos: el cartel F3
// (TrueTriple.menu.txt) y los nombres de auto (TrueTriple.autos.txt). Un
// segundo par de funciones copiado seria un segundo lugar donde arreglar el
// dia que la busqueda cambie.
static bool tt_ruta_junto_a(HMODULE mod, const wchar_t *archivo, wchar_t *ruta)
{
	if (!GetModuleFileName(mod, ruta, MAX_PATH))
		return false;
	wchar_t *barra = wcsrchr(ruta, L'\\');
	if (!barra)
		return false;
	barra[1] = 0;
	// wcsncat_s con _TRUNCATE y no wcscat_s: el segundo mata el proceso si no
	// entra, y un cartel no vale un crash.
	return wcsncat_s(ruta, MAX_PATH, archivo, _TRUNCATE) == 0;
}

static bool tt_ubicar_archivo(const wchar_t *archivo, wchar_t *ruta, WIN32_FILE_ATTRIBUTE_DATA *fad)
{
	if (tt_ruta_junto_a(migoto_handle, archivo, ruta)
			&& GetFileAttributesEx(ruta, GetFileExInfoStandard, fad))
		return true;
	if (tt_ruta_junto_a(NULL, archivo, ruta)
			&& GetFileAttributesEx(ruta, GetFileExInfoStandard, fad))
		return true;
	return false;
}

// Lee un archivo de texto UTF-8 y lo deja en `out` como wide, terminado en NUL
// (o sea out->size() == largo + 1). Devuelve false ante cualquier problema: no
// existe, esta vacio, es enorme, o no es UTF-8 valido. Lo comparten el cartel y
// los nombres de auto — es el unico lugar del proyecto que sabe de BOMs.
static bool tt_leer_utf8(const wchar_t *ruta, std::vector<wchar_t> *out)
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
	out->assign((size_t)largo_w + 1, 0);
	MultiByteToWideChar(CP_UTF8, 0, inicio, largo_a, &(*out)[0], largo_w);
	(*out)[largo_w] = 0;
	return true;
}

static bool tt_leer_archivo(const wchar_t *ruta)
{
	std::vector<wchar_t> wbuf;
	if (!tt_leer_utf8(ruta, &wbuf))
		return false;

	tt_parsear_todo(&wbuf[0], wbuf.size() - 1);
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
			LogInfo("TrueTriple: %S no encontrado, cartel de respaldo embebido\n", TT_MENU_ARCHIVO);
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
		LogInfo("TrueTriple: cartel leido de %S (%Iu filas)\n", ruta, tt_filas.size());
	} else {
		tt_cargar_embebido();
		tt_ft_visto = fad.ftLastWriteTime;
		tt_tam_visto = tam;
		LogInfo("TrueTriple: %S ilegible o vacio, cartel de respaldo embebido\n", ruta);
	}
	tt_filas_listas = true;
}

// ---------------------------------------------------------------------------
// LOS NOMBRES DE LOS AUTOS TAMBIEN SALEN DE UN ARCHIVO (v1.86)
//
// Mismo criterio que el cartel F3, y por el mismo motivo: el C++ no sabe —ni
// puede saber— que auto es el 7 para este usuario. La app (o el lanzador) es
// quien conoce la lista y la escribe al lado del d3dx.ini.
//
//   Archivo:  TrueTriple.autos.txt   (junto al d3d11.dll, o junto al exe)
//   Formato:  una linea por auto, "<numero>|<nombre>"
//   Ejemplo:  7|Lancia Delta HF Integrale
//   UTF-8 (con BOM o sin el), lineas vacias y las que empiezan con # ignoradas.
//
// Numero fuera de 1..TT_AUTOS_MAX, sin barra, o nombre vacio -> se descarta esa
// linea y se sigue, igual que el cartel: un archivo mal formado NUNCA puede
// dejar el panel sin dibujar.
//
// Y el archivo entero es OPCIONAL. Si no existe —hoy no lo genera nadie— el
// panel muestra "AUTO 01" a secas, sin el guion ni el nombre. Esa es la
// degradacion buena: el numero es el dato con autoridad y esta siempre.
// ---------------------------------------------------------------------------
#define TT_AUTOS_ARCHIVO   L"TrueTriple.autos.txt"
// TECHO, no espejo de N_AUTOS. No tiene que coincidir con el generador: alcanza
// con que le sobre, porque lo unico que hace es acotar el array y rechazar
// numeros basura. Pero quedarse CORTO es una truncacion silenciosa —el auto 65
// existiria en el ini y el panel lo mostraria sin nombre—, y por eso va con
// mucho aire: el selector iba en 60 con la plantilla v13 y en la v14 ya son 82.
// Si algun dia lo pasa, el LogInfo de abajo lo dice en vez de callarse.
#define TT_AUTOS_MAX       128
#define TT_AUTO_NOMBRE_MAX 48    // mas largo que esto ensancharia el panel sin fin

static std::wstring tt_autos[TT_AUTOS_MAX + 1];   // indexado por numero, 0 sin usar
static bool      tt_autos_listos = false;
static bool      tt_autos_desde_archivo = false;
static FILETIME  tt_autos_ft = { 0, 0 };
static ULONGLONG tt_autos_tam = 0;
static DWORD     tt_autos_chequeo = 0;

static void tt_limpiar_autos(void)
{
	for (int i = 0; i <= TT_AUTOS_MAX; i++)
		tt_autos[i].clear();
}

// Cuantas lineas se descartaron por numero FUERA DE RANGO en la ultima lectura.
// Se lleva aparte porque es el unico descarte que no es culpa del archivo sino
// del techo de este dll, y es el que hay que poder ver en el log: si el ini
// crece de autos y el dll no, el sintoma serian los ultimos autos sin nombre,
// que es indistinguible de "todavia no les puse nombre".
static int tt_autos_fuera = 0;

// Devuelve cuantas lineas validas entraron (para el log: "0" y "no existe" son
// dos problemas distintos y en pantalla se ven igual).
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
		tt_recortar(&snum);
		tt_recortar(&nom);
		if (snum.empty() || nom.empty())
			continue;

		// Parseo a mano y no con wcstol por lo mismo que tt_afloat: el CRT
		// depende del locale y aca queremos que "07" sea 7 en cualquier PC.
		int num = 0;
		bool digitos = true;
		for (size_t k = 0; k < snum.size(); k++) {
			if (snum[k] < L'0' || snum[k] > L'9') { digitos = false; break; }
			num = num * 10 + (int)(snum[k] - L'0');
			if (num > 999999) { digitos = false; break; }   // corta un numero absurdo
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
		tt_autos[num] = nom;   // repetido: gana el ultimo, sin descartar nada
	}
	return cargados;
}

// Mismo ritmo de recarga en caliente que el cartel: como mucho una mirada por
// segundo a la fecha y el tamano. La app reescribe este archivo junto con el
// d3dx.ini, asi que cambiar la lista no obliga a salir del juego.
static void tt_asegurar_autos(void)
{
	DWORD ahora = GetTickCount();
	if (tt_autos_listos && (ahora - tt_autos_chequeo) < 1000)
		return;
	tt_autos_chequeo = ahora;

	wchar_t ruta[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA fad;

	if (!tt_ubicar_archivo(TT_AUTOS_ARCHIVO, ruta, &fad)) {
		// Si el archivo desaparecio y antes lo teniamos, hay que volver a los
		// numeros pelados; si ya estabamos asi no se rehace nada (ni se
		// re-loguea: el caso normal de hoy es que el archivo no exista).
		if (!tt_autos_listos || tt_autos_desde_archivo) {
			tt_limpiar_autos();
			tt_autos_desde_archivo = false;
			tt_autos_ft.dwLowDateTime = tt_autos_ft.dwHighDateTime = 0;
			tt_autos_tam = 0;
			tt_autos_listos = true;
			LogInfo("TrueTriple: %S no encontrado, el panel muestra solo el numero de auto\n",
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
		LogInfo("TrueTriple: nombres de auto leidos de %S (%d)\n", ruta, cargados);
		// Truncacion VISIBLE. Si esto aparece, el ini tiene mas autos que los
		// que este dll puede nombrar y hay que subir TT_AUTOS_MAX: en pantalla
		// se veria igual que "a esos autos todavia no les puse nombre".
		if (tt_autos_fuera)
			LogInfo("TrueTriple: %d nombres IGNORADOS por numero > %d "
				"(subir TT_AUTOS_MAX en Overlay.cpp)\n",
				tt_autos_fuera, TT_AUTOS_MAX);
	} else {
		tt_limpiar_autos();
		LogInfo("TrueTriple: %S ilegible o vacio, el panel muestra solo el numero\n", ruta);
	}
	// desde_archivo queda en true tambien cuando fallo la lectura: quiere decir
	// "el archivo existe y ya lo miramos", que es lo que la fecha y el tamano de
	// abajo van a usar para no releerlo —ni re-loguearlo— cada segundo.
	tt_autos_desde_archivo = true;
	tt_autos_ft = fad.ftLastWriteTime;
	tt_autos_tam = tam;
	tt_autos_listos = true;
}

// Estado vivo que el C++ (y solo el C++) puede resolver. Los %KEY_*% ya vienen
// sustituidos por quien genero el archivo.
struct TtEstado {
	float fwd, up, right, pitch;
	int   activo;
	const wchar_t *guardadas;
	bool  hay_auto;
	bool  hay_preset;
};

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
			// Las tres perillas que se ajustan en vivo, mas el angulo que el
			// estabilizador esta corrigiendo en ese momento. Se muestran porque
			// el punto se busca MANEJANDO —es la unica forma honesta de
			// encontrarlo— y sin verlas no hay manera de saber en que paso del
			// ciclo quedo cada tecla. El ultimo numero es el que dice si la
			// correccion esta actuando, y cuanto.
			swprintf_s(num, L"%d%%   memoria %d   tope %.1f gr   -> %.2f gr",
				(int)tt_global(L"$ph_int", 0.0f),
				(int)tt_global(L"$ph_mem", 0.0f),
				tt_global(L"$ph_tope", 0.0f),
				tt_global(L"$ph_d", 0.0f));
		// v1.92: las tres perillas SUELTAS, y el angulo corregido aparte. El
		// %SMOOTH% de arriba las junta en un renglon de estado; el cartel las
		// pide una por una para poder poner cada valor AL LADO de la tecla que
		// lo mueve, que es lo que pidio el autor: asi se ve que cambia que cosa
		// sin tener que cruzar dos renglones distintos.
		//
		// %SMOOTH% se queda: un cartel viejo (de una app anterior) lo sigue
		// usando, y un marcador que desaparece se dibuja LITERAL en pantalla.
		// EL RELLENO A 8 LO HACE EL C++, no el generador del cartel (v1.94).
		// Cuando el autor pidio alinear estas tres filas, la respuesta habia sido
		// "no se puede": el valor se resuelve por frame, asi que su largo no se
		// conoce al generar el archivo. Era una conclusion mal sacada de una
		// premisa correcta — el ancho no tiene por que salir del texto, se puede
		// FIJAR. Y el unico que puede rellenar es el que conoce el largo real.
		//
		// El 8 esta medido sobre los rangos reales de las tres listas de ciclo de
		// la plantilla ($ph_int 0..100, $ph_mem 2..12, $ph_tope 2..9): el mas
		// largo es "10.0 gr" = 7, mas 1 de aire. A la IZQUIERDA para que el borde
		// del panel quede a plomo con la tabla de asiento.
		else if (!_wcsicmp(n, L"PH_INT")) {
			wchar_t v[16];
			swprintf_s(v, L"%d%%", (int)tt_global(L"$ph_int", 0.0f));
			swprintf_s(num, L"%-8s", v);
		} else if (!_wcsicmp(n, L"PH_MEM")) {
			wchar_t v[16];
			swprintf_s(v, L"%d", (int)tt_global(L"$ph_mem", 0.0f));
			swprintf_s(num, L"%-8s", v);
		} else if (!_wcsicmp(n, L"PH_TOPE")) {
			wchar_t v[16];
			swprintf_s(v, L"%.1f gr", tt_global(L"$ph_tope", 0.0f));
			swprintf_s(num, L"%-8s", v);
		}
		// La compensacion vertical, en cm. CON SIGNO: el signo es la mitad del
		// ajuste —una direccion aquieta el tablero y la otra lo agita— asi que
		// mostrar el valor absoluto le sacaria al cartel justo el dato que el
		// usuario esta mirando mientras mueve la perilla.
		//
		// Mismo relleno a 8 que las otras tres, para que las cuatro filas del
		// estabilizador alineen. El mas largo es "-60 cm" (6), o sea 2 de aire.
		else if (!_wcsicmp(n, L"PH_DIST")) {
			wchar_t v[16];
			swprintf_s(v, L"%d cm", (int)tt_global(L"$ph_dist", 0.0f));
			swprintf_s(num, L"%-8s", v);
		}
		// La intensidad del cuello (v18), mismo formato que la del estabilizador.
		else if (!_wcsicmp(n, L"PH_CUELLO")) {
			wchar_t v[16];
			swprintf_s(v, L"%d%%", (int)tt_global(L"$cu_int", 0.0f));
			swprintf_s(num, L"%-8s", v);
		}
		// %PH_D% NO se rellena: vive en la fila del TITULO, que es centrada, y el
		// C++ mide el texto ya resuelto — unos espacios de mas le correrian el
		// centrado a la derecha.
		else if (!_wcsicmp(n, L"PH_D"))
			swprintf_s(num, L"%.2f gr", tt_global(L"$ph_d", 0.0f));
		else if (!_wcsicmp(n, L"FWD"))
			swprintf_s(num, L"%+.2f", st.fwd);
		else if (!_wcsicmp(n, L"UP"))
			swprintf_s(num, L"%+.2f", st.up);
		else if (!_wcsicmp(n, L"RIGHT"))
			swprintf_s(num, L"%+.2f", st.right);
		else if (!_wcsicmp(n, L"PITCH"))
			swprintf_s(num, L"%+.1f", st.pitch);
		else if (!_wcsicmp(n, L"AUTO_SEL")) {
			// v1.86: el auto EFECTIVO — el que eligio el usuario a mano, o el
			// que la tabla enseñada auto-selecciono (ver tt_auto_efectivo).
			// Si el template es viejo y no hay ninguna de las dos variables,
			// "?" — que se vea que falta, no un 0 que parece un auto valido.
			// Es la misma degradacion explicita con la que el panel lateral no
			// se dibuja.
			// v1.89: el NOMBRE, no el numero. Antes decia "AUTO ACTIVO: 37", y
			// desde que el panel dejo de mostrar el numero ese 37 no se podia
			// cruzar con nada: era un dato que el usuario no ve en ningun otro
			// lado. El numero queda de repuesto por si falta el .txt de nombres.
			float sel = tt_auto_efectivo(-1.0f);
			if (sel < 0.5f) {
				wcscpy_s(num, L"?");
			} else {
				int k = (int)(sel + 0.5f);
				tt_asegurar_autos();
				const wchar_t *nom = (k >= 1 && k <= TT_AUTOS_MAX)
						? tt_autos[k].c_str() : L"";
				// num[64] y TT_AUTO_NOMBRE_MAX es 48: entra con margen.
				if (nom[0])
					wcscpy_s(num, nom);
				else
					swprintf_s(num, L"%d", k);
			}
		} else if (!_wcsicmp(n, L"PRESET")) {
			if (st.activo >= 1 && st.activo <= TT_PRESETS)
				swprintf_s(num, L"%d", st.activo);
			else
				wcscpy_s(num, L"-");   // hay guardadas pero ninguna puesta
		} else if (!_wcsicmp(n, L"GUARDADAS")) {
			out += st.guardadas;
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
	switch (cond) {
	case TT_CON_PRESET: return st.hay_preset;
	case TT_SIN_PRESET: return !st.hay_preset;
	case TT_CON_AUTO:   return st.hay_auto;
	case TT_SIN_AUTO:   return !st.hay_auto;
	default:            return true;
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

	// v1.85: la identidad del auto la declara el usuario ($auto_elegido, que
	// cicla con teclas del ini). v1.86: lo que se muestra y con lo que se
	// buscan los presets es el EFECTIVO — igual al declarado salvo que la
	// tabla enseñada por el propio usuario haya matcheado la huella del
	// habitaculo (iniParams[8].x, un instrumento; la decision es del ini). Con
	// un template viejo (sin ninguna de las dos variables) no hay auto, y las
	// filas AUTO/NOAUTO del cartel lo dicen en pantalla.
	float auto_sel = tt_auto_efectivo(0.0f);
	bool hay_auto = (auto_sel > 0.5f);
	float pre_actual = tt_global(L"$pre_actual", 0.0f);
	int activo = (int)pre_actual;

	// Que presets tiene guardados ESTE auto. El barrido de los 540 slots esta
	// cacheado (ver tt_guardadas_de); aca se le pasa $pre_actual CRUDO, igual
	// que el panel lateral, para que los dos compartan la misma entrada.
	const TtGuardadas &g = tt_guardadas_de(auto_sel, pre_actual);
	int cuantos = g.cuantos;

	// La lista de numeros guardados se arma una sola vez y se le pasa al
	// resolvedor como %GUARDADAS%.
	wchar_t usados[64];
	usados[0] = 0;
	{
		int k = 0;
		for (int p = 1; p <= TT_PRESETS; p++) {
			if (!g.tiene[p])
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

	tt_asegurar_menu();
	tt_z_n = 0;   // lo rellena el bucle de abajo, una vez por frame

	for (size_t i = 0; i < tt_filas.size() && n < TT_MAX_FILAS; i++) {
		const TtFilaDef &d = tt_filas[i];
		if (!tt_cond_se_cumple(d.cond, st))
			continue;
		// Las filas Z no van adentro del cartel: se resuelven aca —con ESTE
		// estado, el unico que hay— y las dibuja DrawFilaAparte en su propia caja.
		if (d.aparte) {
			if (tt_z_n < (int)_countof(tt_z)
					&& tt_resolver(d.texto, st, tt_z[tt_z_n].txt)) {
				tt_z[tt_z_n].color    = d.color;
				tt_z[tt_z_n].centrado = d.centrado;
				tt_z[tt_z_n].escala   = (d.escala > 0.01f) ? d.escala : 1.0f;
				tt_z_n++;
			}
			continue;
		}
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
		return -1.0f;   // -1 = no se dibujo: el de asiento se ubica solo

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

	// --- ubicar: en el monitor IZQUIERDO, como el de asiento (v1.93) ---
	//
	// Antes iba centrado en el monitor del medio, o sea justo donde esta la
	// pista. Los dos menues se abren juntos con F3 y se apilan aca; comparten
	// la MISMA fraccion ($ma_fx) para que queden alineados por construccion y
	// no por dos numeros parecidos que alguien despues mueve de a uno.
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

	// El borde INFERIOR de la caja, para que el menu de asiento se apile justo
	// debajo. Se devuelve MEDIDO y no calculado aparte: el alto del cartel
	// depende de los binds que tenga puestos el usuario (un `Ctrl + Inicio`
	// puede partir una fila), asi que cualquier constante se desalinea sola.
	return y0 + alto + MARGEN;
}

// Una fila del panel lateral. Se arma con tt_fila_panel en vez de un array
// inicializado de golpe porque desde v1.86 hay filas CONDICIONALES (la del
// estado de vinculo, que no existe con una plantilla vieja): con el array fijo
// habria que dejar un hueco y acordarse de saltearlo al medir y al dibujar, que
// es la clase de detalle que se olvida en el segundo de los dos bucles.
struct TtFilaPanel {
	const wchar_t *txt;
	float esc;     // sobre el cuerpo base del F3
	float antes;   // aire por encima, en renglones base
	TtCol color;
};

// `cap` no es ceremonia: el arreglo del panel se dimensiona a mano y las filas
// que se dibujan dependen del ESTADO, asi que el maximo no se ve leyendo una
// sola rama. Sin este tope, agregar una fila mas de las que entran pisaba la
// pila en silencio. Si alguna vez sobra se descarta la de mas y el log lo dice
// UNA vez: perder una fila callado se ve igual que no haberla escrito nunca.
static void tt_fila_panel(TtFilaPanel *f, int *n, int cap, const wchar_t *txt,
		float esc, float antes, TtCol color)
{
	if (*n >= cap) {
		static bool dicho = false;
		if (!dicho) {
			dicho = true;
			LogInfo("TrueTriple: el panel de asientos no entra en %d filas "
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

// ===========================================================================
// MENU ASIENTOS — panel lateral (v1.85, redisenado en v1.86)
//
// La identidad del auto la declara el usuario: cicla $auto_elegido (1..82 en
// la plantilla v14; el C++ no depende de ese tope) con
// teclas del ini. Este panel es la confirmacion visual de ese numero — vive en
// el monitor IZQUIERDO de un triple (centro en x = ancho/6), lejos del cartel
// F3 del monitor central, para que se puedan mirar los dos a la vez.
//
// LA JERARQUIA ES DELIBERADA (pedido del autor, v1.86): titulo > slots >
// auto/teclas. Hasta v1.85 lo mas grande era el NUMERO de auto, y quedaba un
// panel que gritaba un dato que el usuario acababa de elegir el; lo que se
// mira de reojo mientras se maneja es que slots hay guardados. El numero pasa
// a dos digitos (AUTO 07, no AUTO 7) para que la linea no cambie de ancho al
// cruzar el 9 — con el texto centrado, eso se ve como un temblor.
//
// Las filas, de arriba a abajo (las del medio dependen del estado de vinculo,
// asi que el panel va de 5 a 7 renglones y de dos anchos muy distintos):
//   MENU ASIENTOS                     titulo
//   AUTO 07 - Lancia Delta            el nombre sale de TrueTriple.autos.txt
//   VINCULADO / NO RECONOCEMOS EL AUTO / AUTO SIN VINCULAR
//   Pasa por la vista de cockpit      solo en los estados que BLOQUEAN
//   Los slots estan bloqueados...     idem
//   SLOTS GUARDADOS: 1 - 2 - 3        o "ninguno"
//   Cargar Slot: ...                  teclas fijas
//
// Visible solo con $menu_autos_on == 1 (lo prende y apaga el ini). Si el
// template es viejo y $auto_elegido no existe, NO se dibuja — degradacion
// limpia y a proposito, la misma con la que %AUTO_SEL% resuelve a "?".
//
// POSICION Y ESCALA POR FRACCIONES del render target, nunca pixeles. El centro
// sale de $ma_fx/$ma_fy si el ini los trae (asi la app puede afinar por perfil
// en una fase 2 sin recompilar), con default 1/6 y 0.5. La fuente del overlay
// es un recurso incrustado a tamano fijo y el F3 se dibuja con escala 1.0 en
// el rig de referencia de 1440p: aca todo se multiplica por alto/1440 para que
// el panel ocupe la misma fraccion de pantalla en cualquier resolucion.
//
// El fondo se MIDE con MeasureString antes de dibujarse, como el F3 — fijar
// el ancho a ojo fue el primer bug de aquel panel.
// ===========================================================================
// ===========================================================================
// LA TERCERA CAJA: las filas de tipo Z (v1.95)
//
// Pedido del autor: sacar el renglon de "ocultar este cartel" de adentro del
// cartel y darle un recuadro propio, debajo del panel de asiento.
//
// POR QUE EL TEXTO SIGUE SALIENDO DEL ARCHIVO y no esta escrito aca: el C++ no
// conoce el nombre del bind —los %KEY_*% los resuelve el generador, que es
// quien sabe que tecla puso el usuario— ni el idioma elegido. Escribir esta
// caja en el C++ la dejaria en espanol fijo y con F3 horneada, o sea perdiendo
// las dos cosas que veniamos cuidando. Por eso el archivo gana un TIPO nuevo en
// vez de que el dll gane un texto.
//
// Devuelve el borde inferior, igual que las otras dos, por si algun dia se
// apila algo debajo. -1 = no habia ninguna fila Z (el archivo puede ser de una
// version anterior: eso NO es un error, simplemente no se dibuja la caja).
float Overlay::DrawFilaAparte(float y_arranque)
{
	// No se rearma nada: DrawTripleMenu ya resolvio estas filas con el unico
	// TtEstado que hay. Si el cartel no se dibujo, tt_z_n quedo en 0 y aca no
	// hay nada que hacer — que es lo correcto: los recuadros se abren y se
	// cierran juntos con F3.
	const TtFilaZ *f = tt_z;
	const int n = tt_z_n;
	if (n == 0)
		return -1.0f;

	const float RENGLON = mFont->GetLineSpacing();
	float base = (float)mResolution.y / 1440.0f;
	float MARGEN = RENGLON * base;

	float ancho = 0.0f, alto = 0.0f;
	for (int i = 0; i < n; i++) {
		Vector2 m = mFont->MeasureString(f[i].txt);
		float e = f[i].escala * base;
		if (m.x * e > ancho)
			ancho = m.x * e;
		alto += RENGLON * e;
	}

	// Mismo centro horizontal que las otras dos cajas: las tres cuelgan de
	// $ma_fx, asi que quedan alineadas por construccion.
	float cx = (float)mResolution.x * tt_global(L"$ma_fx", 1.0f / 6.0f);
	float x0 = cx - ancho / 2.0f;
	float y0 = (y_arranque >= 0.0f) ? (y_arranque + MARGEN + RENGLON * base)
						: (RENGLON * 2.0f + MARGEN);

	DrawRectangle(x0 - MARGEN, y0 - MARGEN,
		ancho + MARGEN * 2.0f, alto + MARGEN * 2.0f, 0.0f, 0.0f, 0.0f, 0.80f);

	float y = y0;
	for (int i = 0; i < n; i++) {
		float e = f[i].escala * base;
		Vector2 m = mFont->MeasureString(f[i].txt);
		float x = f[i].centrado ? (cx - m.x * e / 2.0f) : x0;
		DirectX::XMVECTOR c = tt_rgb(f[i].color);
		static const Vector2 borde[4] = {
			Vector2(-1, 0), Vector2(1, 0), Vector2(0, -1), Vector2(0, 1)
		};
		for (int k = 0; k < 4; k++)
			mFont->DrawString(mSpriteBatch.get(), f[i].txt, Vector2(x, y) + borde[k],
				DirectX::Colors::Black, 0.0f, Vector2(0, 0), e);
		mFont->DrawString(mSpriteBatch.get(), f[i].txt, Vector2(x, y), c,
			0.0f, Vector2(0, 0), e);
		y += RENGLON * e;
	}
	return y0 + alto + MARGEN;
}

float Overlay::DrawMenuAutos(float y_arranque)
{
	float sel = tt_auto_efectivo(-1.0f);
	if (sel < 0.5f)
		return -1.0f;   // template viejo sin la variable: sin panel, sin adivinar
	int elegido = (int)(sel + 0.5f);

	// Slots guardados de ESTE auto: el MISMO barrido cacheado que DrawTripleMenu,
	// con el MISMO par (auto, $pre_actual crudo) — si difirieran, cada panel
	// invalidaria la cache del otro en cada frame y no cachearia nada.
	const TtGuardadas &g = tt_guardadas_de(sel, tt_global(L"$pre_actual", 0.0f));
	bool alguna = (g.cuantos > 0);

	// El nombre del auto es OPCIONAL y sale de TrueTriple.autos.txt (ver arriba).
	// Sin archivo, o sin linea para este numero, se dibuja "AUTO 07" a secas:
	// ni el guion suelto ni un "(sin nombre)", que serian ruido para el caso
	// que hoy es el normal.
	tt_asegurar_autos();
	wchar_t linea_auto[16 + TT_AUTO_NOMBRE_MAX];
	const wchar_t *nombre = (elegido >= 1 && elegido <= TT_AUTOS_MAX)
			? tt_autos[elegido].c_str() : L"";
	// SOLO EL NOMBRE (v1.89, pedido del autor). El numero de entrada es una
	// clave INTERNA —de hecho ya se corrio dos veces— y al usuario no le dice
	// nada que el nombre no diga mejor. Se conserva unicamente como REPUESTO:
	// sin el .txt de nombres, "AUTO 07" es feo pero identifica; una fila vacia
	// pareceria el panel roto.
	if (nombre[0])
		swprintf_s(linea_auto, L"%s", nombre);
	else
		swprintf_s(linea_auto, L"%s %02d", tt_txt(L"auto", L"AUTO"), elegido);

	wchar_t lista[96];
	{
		int k = swprintf_s(lista, L"%s", tt_txt(L"slots", L"SLOTS GUARDADOS:"));
		bool primero = true;
		for (int p = 1; p <= TT_PRESETS; p++) {
			if (!g.tiene[p])
				continue;
			k += swprintf_s(lista + k, _countof(lista) - k,
				primero ? L" %d" : L" - %d", p);
			primero = false;
		}
		// "ninguno" y no un guion: un simbolo suelto en el renglon mas mirado
		// del panel se lee como que algo se rompio.
		// ⚠️ LAS LLAVES NO SON DE ESTILO. Sin ellas el segundo `wcscat_s`
		// quedaba FUERA del `if` y "ninguno" se pegaba SIEMPRE, incluso con
		// slots guardados: el autor lo vio en pantalla como "1 - 2none".
		// El espacio si estaba adentro, por eso salia pegado al numero.
		if (!alguna) {
			wcscat_s(lista, L" ");
			wcscat_s(lista, tt_txt(L"ninguno", L"ninguno"));
		}
	}

	// Estado de vinculo: el renglon de la RECORRIDA de vinculacion (el autor va
	// auto por auto), y en los estados 0 y 2 tambien la explicacion de por que
	// las teclas de slot no responden. Sin la variable no se dibuja nada de
	// esto: ver tt_vinculo, que es donde esta el razonamiento entero.
	int vinculo = 0;
	bool hay_vinculo = tt_vinculo(&vinculo);
	const wchar_t *txt_estado = NULL;   // NULL = estado sano, no se dibuja
	const wchar_t *txt_como = NULL;   // NULL = el estado no necesita instruccion
	TtCol col_estado = TT_NARANJA;
	switch (vinculo) {
	case 1:
		// v1.91: el estado BUENO no dibuja nada. "VINCULADO" era del taller de
		// curaduria —le decia al autor si esa entrada ya tenia habitaculo— y al
		// usuario final no le dice nada: con la tabla horneada, TODOS sus autos
		// estan vinculados. Un cartel que siempre dice lo mismo se vuelve ruido, y
		// peor, uno verde permanente entrena a no mirar el renglon cuando algun
		// dia diga otra cosa. Los estados 0 y 2 SI siguen: esos explican por que
		// las teclas no responden.
		txt_estado = NULL;
		break;
	case 2:
		txt_estado = tt_txt(L"sin_vincular", L"AUTO SIN VINCULAR");
		txt_como   = L"Elegi el auto en la lista y apreta " TT_TECLA_VINCULAR L" para vincular";
		break;
	default:
		txt_estado = tt_txt(L"sin_auto", L"NO RECONOCEMOS EL AUTO");
		txt_como   = tt_txt(L"pasa_cockpit", L"Pasa por la vista de cockpit");
		break;
	}

	// Las teclas de los presets van HORNEADAS porque no son rebindeables
	// (ctrl/no_ctrl 0..9, fijas en la plantilla desde v1.40). Las que ciclan el
	// selector de auto son binds del template y el C++ no puede conocerlas:
	// esas las lista el cartel F3 (TrueTriple.menu.txt), no este panel.
	//
	// El numero de filas VARIA con el estado (de 5 a 7), asi que ni el alto ni
	// el ancho pueden estar horneados: los dos salen del bucle de medicion de
	// mas abajo, sobre las filas que realmente se van a dibujar. Con los
	// mensajes de los estados 0 y 2 el panel es bastante mas ancho que con
	// "VINCULADO", y el que manda el ancho ya no es el titulo.
	TtFilaPanel f[12];
	int nf = 0;
	// LAS ESCALAS SALEN DE PIXELES REALES, NO A OJO. La fuente del overlay
	// (courierbold.spritefont) tiene 19 px de alto de MAYUSCULA a escala 1.0,
	// que en tamanos de fuente al uso equivale a ~26 px. De ahi: escala = px/26.
	// Todo esto se multiplica despues por `base` (= alto/1440), asi que las
	// medidas valen igual en 1080, 1440 o ultrawide.
	// v1.94: SIN TITULO. Desde la v1.93 este panel se abre pegado abajo del
	// cartel, que ya dice TrueTriple arriba de todo: repetir un titulo a dos
	// renglones de otro no ubica a nadie, solo gasta alto en el unico monitor
	// donde entran los dos. Lo que identifica al panel es el nombre del auto,
	// que es la fila siguiente y ahora queda primera.
	// Sin reconocimiento, la seleccion persistida puede ser el auto anterior.
	// No presentarla como titulo del actual; el aviso queda como primera fila.
	if (!hay_vinculo || vinculo != 0)
		tt_fila_panel(f, &nf, _countof(f), linea_auto,   1.15f, 0.30f, TT_ORO);
	if (hay_vinculo && txt_estado) {
		tt_fila_panel(f, &nf, _countof(f), txt_estado,   0.69f, 0.45f, col_estado);
		if (txt_como) {
			tt_fila_panel(f, &nf, _countof(f), txt_como, 0.55f, 0.12f, TT_NARANJA);
			// La que conecta el cartel con la tecla que no responde. Va SOLO en
			// los estados que bloquean; con el auto reconocido seria el aviso de
			// un problema que no existe.
			tt_fila_panel(f, &nf, _countof(f),
				tt_txt(L"bloqueados", L"Los slots estan bloqueados hasta reconocer el auto"),
				                    0.55f, 0.12f, TT_NARANJA);
		}
	}
	tt_fila_panel(f, &nf, _countof(f), lista,            0.69f, 0.45f, TT_VERDE);

	// LA POSTURA VIVA (v1.88). Hasta ahora el asiento se acomodaba A CIEGAS: se
	// apretaba la tecla y se juzgaba por lo que se veia, sin saber en que valor
	// se estaba parado ni como volver a uno de antes. Desde la v15 el menu es
	// ademas el modo de edicion —con el cerrado las teclas de asiento no hacen
	// nada—, asi que esta es la pantalla que el usuario TIENE delante mientras
	// ajusta, y el unico lugar donde el numero le sirve.
	//
	// Sale de los MISMOS globals que mueven las teclas, no de una copia: si
	// alguna vez no coincidieran, el numero mentiria justo cuando se lo usa para
	// decidir. Y si la plantilla es vieja y no los declara, la fila no se dibuja:
	// un cero inventado se leeria como una postura de verdad.
	wchar_t postura[96];
	if (tt_hay_global(L"$seat_fwd") && tt_hay_global(L"$seat_up")
			&& tt_hay_global(L"$seat_right") && tt_hay_global(L"$seat_pitch")) {
		// Sin acentos ni simbolo de grados: la fuente del overlay son 95 glifos
		// ASCII (0x20-0x7E), la misma razon por la que el titulo dice ASIENTOS.
		// EL FORMATO LO PONE EL C++, LAS ETIQUETAS VIENEN TRADUCIDAS. Se compone
		// etiqueta + numero por separado a proposito: si el texto traducido
		// llevara los %.3f adentro, un error de traduccion —un %s donde va un
		// float, un especificador de mas— no daria un texto feo sino basura de
		// memoria en pantalla o un cierre del juego. Asi, lo peor que puede pasar
		// con una traduccion mala es que se lea raro.
		swprintf_s(postura, L"%s %.3f   %s %.3f   %s %.3f   %s %.1f",
			tt_txt(L"adelante", L"ADELANTE"),    tt_global(L"$seat_fwd", 0.0f),
			tt_txt(L"arriba", L"ARRIBA"),        tt_global(L"$seat_up", 0.0f),
			tt_txt(L"lateral", L"LATERAL"),      tt_global(L"$seat_right", 0.0f),
			tt_txt(L"inclinacion", L"INCLINACION"), tt_global(L"$seat_pitch", 0.0f));
		tt_fila_panel(f, &nf, _countof(f), postura, 0.62f, 0.20f, TT_CIELO);
	}

	tt_fila_panel(f, &nf, _countof(f),
		tt_txt(L"teclas", L"Cargar 1-9   Guardar Ctrl+1-9   Borrar Ctrl+Shift+1-9   Ciclar 0"),
		                                    0.50f, 1.00f, TT_PLATA);

	// --- medir ---
	const float RENGLON = mFont->GetLineSpacing();
	float base = (float)mResolution.y / 1440.0f;   // ver el comentario grande
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
	// v1.93: si el cartel se dibujo, este va APILADO debajo suyo; si no, cae
	// en su posicion de siempre. El separador es un renglon entero para que
	// se lean como dos cajas y no como una partida al medio, que es lo que
	// pidio el autor ("dos contenedores diferentes").
	float cy = (float)mResolution.y * tt_global(L"$ma_fy", 0.5f);
	float x0 = cx - ancho / 2.0f;
	float MARGEN = RENGLON * base;
	// El +MARGEN saca el borde de la caja de arriba; el +RENGLON es el renglon
	// de aire que las separa. Sin el, los dos rectangulos negros se tocan y se
	// leen como una sola caja partida al medio.
	float y0 = (y_arranque >= 0.0f) ? (y_arranque + MARGEN + RENGLON * base)
						: (cy - alto / 2.0f);

	DrawRectangle(x0 - MARGEN, y0 - MARGEN,
		ancho + MARGEN * 2.0f, alto + MARGEN * 2.0f, 0.0f, 0.0f, 0.0f, 0.80f);

	// --- dibujar: todo centrado, mismo contorno de 1 px que el F3 ---
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

	// El borde inferior, para que la caja del tipo Z se apile debajo. Mismo
	// criterio que el cartel: medido, no calculado aparte.
	return y0 + alto + MARGEN;
}

void Overlay::DrawOverlay(void)
{
	Profiling::State profiling_state;
	HRESULT hr;

	// TrueTriple: el menu tambien abre el overlay. y8 lo publica [Present]
	// (triple activo Y menu no oculto). Sin esto el overlay solo se dibuja
	// cazando, con un aviso pendiente o midiendo rendimiento.
	bool triple_menu = (G->iniParams.size() >= 9 && G->iniParams[8].y > 0.5f);

	// v1.93: LOS DOS MENUES SE ABREN JUNTOS CON F3. El panel de asiento dejo de
	// tener interruptor propio (se fue [KeyMenuAutos] y su bind de la app): se
	// dibuja cuando se dibuja el cartel, apilado debajo.
	//
	// El gate de edicion del ini sigue siendo `$menu_autos_on`, y por eso NO se
	// lee aca: ahora lo prende y apaga la misma tecla que el cartel, asi que
	// preguntar por el seria preguntar dos veces lo mismo y abrir la puerta a
	// que un dia digan cosas distintas — panel visible con las teclas muertas,
	// que es justo el desajuste que este panel existe para no tener.
	//
	// Sin identidad de auto (plantilla vieja) queda apagado: el panel entero
	// habla de un auto, y sin auto no tiene nada que decir.
	bool menu_autos = triple_menu && (tt_auto_efectivo(-1.0f) >= 0.5f);

	// Ensayo de culling: si hay gancho instalado, la linea se dibuja SIEMPRE, con o sin
	// menu, para que cualquier captura diga en que modo se tomo.
	wchar_t cull_osd[256];
	TripleCullingOSD(cull_osd, _countof(cull_osd));
	bool cull_line = cull_osd[0] != 0;
	wchar_t auto_osd[256];
	TripleAutoCapturaOSD(auto_osd, _countof(auto_osd));
	bool auto_line = auto_osd[0] != 0;

	if (G->hunting != HUNTING_MODE_ENABLED && !has_notice && !triple_menu
			&& !menu_autos && !cull_line && !auto_line && Profiling::mode == Profiling::Mode::NONE)
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

			if (cull_line) {
				DrawOutlinedString(mFontNotifications.get(), cull_osd, Vector2(10.0f, y), DirectX::Colors::Yellow);
				Vector2 cull_size = mFontNotifications->MeasureString(cull_osd);
				y += cull_size.y;
			}
			if (auto_line) {
				DrawOutlinedString(mFont.get(), auto_osd, Vector2(10.0f, y), DirectX::Colors::LimeGreen);
				Vector2 auto_size = mFont->MeasureString(auto_osd);
				y += auto_size.y;
			}

			// El cartel devuelve donde termina su caja y el de asiento arranca
			// ahi: apilados de verdad, no con dos posiciones que se parecen.
			float abajo_del_cartel = -1.0f;
			if (triple_menu)
				abajo_del_cartel = DrawTripleMenu();

			float abajo_del_panel = -1.0f;
			if (menu_autos)
				abajo_del_panel = DrawMenuAutos(abajo_del_cartel);

			// La caja del tipo Z va ultima, debajo del panel. Si el panel no se
			// dibujo (auto sin identificar) cae debajo del cartel, y si tampoco
			// hubo cartel no hay filas Z resueltas y no se dibuja nada.
			DrawFilaAparte(abajo_del_panel >= 0.0f ? abajo_del_panel
								: abajo_del_cartel);
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
